// Typewriter: write into today's daily note on a Bluetooth keyboard.
//
// The note lives in flash at /daily/YYYY-MM-DD.md (the Obsidian daily note name)
// and autosaves a couple of seconds after you stop typing. Over Wi-Fi it syncs
// with /OS/Daily Notes/YYYY-MM-DD.md in Dropbox: when the note opens, after a
// minute without typing, and on Cmd+S. The screen shows only your text; a footer
// line appears when something needs attention, or on Cmd+I.
//
// After 10 minutes without typing, or a press of the WAKE button, it saves,
// syncs and deep-sleeps showing the list, so your writing isn't left on screen.
// Waking and switching on both start at the list.
//
// Wi-Fi and Dropbox credentials are in secrets.h (see secrets.example.h).
//
// Cmd+O lists today's note and your drafts (essays in /Site/blog without
// isPublished: true), all kept on the Inkplate; Cmd+D returns to today's note.
// Cmd+N on the list starts a new draft, untitled.mdx (then untitled-1.mdx, ...);
// Cmd+S there syncs everything waiting.
// Drafts sync the same way. Blank daily notes and blank new drafts never go to
// Dropbox, and a new draft left blank is removed.
//
// ↑/↓ read back through earlier pages (full refresh each); typing returns to the
// end. Keys: Backspace (Option+Backspace deletes a word), Return, Cmd+Z undo and
// Cmd+Shift+Z redo (typing or deleting), Cmd+S save and sync,
// Cmd+I show status (file, words, saved state, power), Cmd+L full refresh, Cmd+Shift+K forget the keyboard.
// Ctrl works in place of Cmd.
//
// Over USB (tools/settime.py, tools/type.py): Ctrl+T sets the clock, Ctrl+K
// forgets the keyboard, Ctrl+O/D/N/S/Z act as Cmd, other bytes type text. Serial
// logs events and timings.

#include "secrets.h"

#include <Clock.h>
#include <Dropbox.h>
#include <FastInkplate.h>
#include <History.h>
#include <Keyboard.h>
#include <Page.h>
#include <Storage.h>
#include <Sync.h>
#include <Text.h>
#include <Preferences.h>
#include <WiFi.h>
#include <algorithm>
#include <esp_sntp.h>
#include <fonts/Duo24.h>
#include <fonts/Duo28.h>
#include <fonts/Duo40.h>

tw::FastInkplate display(INKPLATE_1BIT);
tw::Page page(display, Duo40, Duo40Ascent, Duo24, Duo24Ascent);
tw::Keyboard keyboard;
tw::History history; // Cmd+Z / Cmd+Shift+Z for the open document

// Reading back with ↑/↓: a page at a time, each a full refresh. Typing anything
// returns to the end and types there, so it stays a typewriter.
bool reading = false;
tw::Page::Reading readingPages;
size_t readingPage = 0;
bool readingDirty = false;
tw::Dropbox dropbox(DROPBOX_APP_KEY, DROPBOX_REFRESH_TOKEN);

const uint32_t PASS_US = 20000;               // drive pass length chosen in Step 1
const uint32_t FOOTER_PASS_US = 55000;        // library-length passes: erases notices cleanly
const uint32_t POWER_OFF_AFTER_MS = 1500;     // keep the panel powered between keystrokes
const uint32_t CLEAN_AFTER_IDLE_MS = 60000;   // full refresh after this long without typing
const uint32_t SAVE_AFTER_IDLE_MS = 2000;     // autosave once typing pauses this long...
const uint32_t SAVE_AT_LEAST_EVERY_MS = 30000; // ...or at least this often while typing
const uint32_t SYNC_AFTER_IDLE_MS = 60000;    // sync once typing pauses this long
const uint32_t SYNC_RETRY_MS = 120000;        // wait this long after a failed sync
const int SYNC_FAILURES_BEFORE_NOTICE = 3;
const char *DAILY_NOTES = "/OS/Daily Notes/"; // in Dropbox
const char *ESSAYS = "/Site/blog";            // in Dropbox
const uint32_t SLEEP_AFTER_IDLE_MS = 10 * 60 * 1000; // deep sleep after this long without typing
const gpio_num_t WAKE_BUTTON = GPIO_NUM_36;           // the board's WAKE button
const char *ASLEEP = "In sleep";
const uint32_t BATTERY_CHECK_EVERY_MS = 60000;
const int LOW_BATTERY_PERCENT = 15;

struct Document {
    std::string path;       // in flash, e.g. /daily/2026-09-29.md
    std::string remotePath; // in Dropbox, e.g. /OS/Daily Notes/2026-09-29.md ("" = don't sync)
    std::string name;       // shown to you, e.g. 2026-09-29.md
    std::string text;
    bool unsaved = false;
    bool saveFailed = false;
    uint32_t firstUnsavedAt = 0;
    tw::SyncState sync;
    bool checkedRemote = false; // synced at least once since opening
    int syncFailures = 0;       // in a row
    uint32_t lastSyncAttemptAt = 0;
    std::string syncError;
};

// Drafts: essays in /Site/blog with no front matter, or isPublished: false.
// Every draft is kept on the Inkplate; published essays aren't.
struct DraftInfo {
    std::string name; // e.g. design-engineering-revisited.mdx
    std::string rev;
    uint32_t modified = 0;
    bool draft = false;
};

enum class JobKind { File, Drafts };
struct SyncJob {
    JobKind kind;
    std::string localPath, remotePath;
    std::string text;    // snapshot, replaced by the shared version if it changed
    size_t snapshotSize; // bytes in the snapshot
    tw::SyncState state;
    bool blankMeansAbsent; // daily notes: never upload a blank note
    bool newDraft;         // an essay started here, never synced: its name may be taken by now
    std::string renamedTo; // the free name a new draft was uploaded under instead
    tw::SyncResult result;
    // Drafts job
    std::string openPath; // the note on screen: left to its own sync
    std::vector<DraftInfo> all;
    bool listed;
    int downloaded, removed;
    const char *why;
    uint32_t startedAt;
};

// Kept in RTC memory through deep sleep, to tell waking from switching on.
RTC_DATA_ATTR bool resumeAfterSleep = false;
void goToSleep(uint32_t wakeAfterSeconds = 0);
void checkBattery(uint32_t now, bool force = false);

Document doc;
uint32_t lastKeyAt = 0;
bool dirty = false;
bool forceFull = true;
bool showStatus = false; // Cmd+I: status line until the next key
bool panelOn = false;
bool partialsSinceClean = false;
bool showingPairing = false;
uint32_t lastUpdateAt = 0;
uint32_t firstUnshownKeyAt = 0;
uint32_t seenKeyboardRevision = UINT32_MAX;
int batteryPercent = -1; // -1: no battery reading
uint32_t lastBatteryCheckAt = 0;
std::string notice;      // one-off message, cleared by the next key
bool clockFromNetwork = false;

// ---- Document ----

bool blankNewDraftOpen();
void removeLocalCopy(const std::string &localPath);

// Takes copies: callers may pass the current document's own fields.
void openDocument(std::string path, std::string remotePath, std::string name) {
    if (path != doc.path && blankNewDraftOpen()) {
        Serial.printf("open: removed %s, left blank\n", doc.path.c_str());
        removeLocalCopy(doc.path);
    }
    doc = Document();
    history.clear();
    reading = false;
    doc.path = path;
    doc.remotePath = remotePath;
    doc.name = name;
    if (tw::storage::exists(path.c_str()) && !tw::storage::read(path.c_str(), doc.text))
        Serial.printf("open: couldn't read %s\n", path.c_str());
    if (!tw::loadSyncState(path, doc.sync)) {
        // Never synced: everything here is new to Dropbox.
        doc.sync = tw::SyncState();
    }
    page.showEnd(doc.text);
    forceFull = true;
    Serial.printf("open: %s (%u bytes, synced rev %s)\n", path.c_str(), (unsigned)doc.text.size(),
                  doc.sync.rev.empty() ? "none" : doc.sync.rev.c_str());
}

std::string todayName() {
    char date[11];
    if (tw::wallclock::isSet())
        tw::wallclock::today(date);
    else
        strcpy(date, "undated");
    return std::string(date) + ".md";
}

void openToday() {
    std::string name = todayName();
    // An undated note stays on the Inkplate: it would collide across days.
    std::string remote = tw::wallclock::isSet() ? DAILY_NOTES + name : "";
    openDocument("/daily/" + name, remote, name);
}

void save(const char *why) {
    if (!doc.unsaved)
        return;
    uint32_t t0 = millis();
    bool ok = tw::storage::write(doc.path.c_str(), doc.text) && tw::saveSyncState(doc.path, doc.sync);
    doc.saveFailed = !ok;
    doc.unsaved = !ok;
    if (ok)
        doc.firstUnsavedAt = 0;
    Serial.printf("save (%s): %s, %u bytes, %lu ms\n", why, ok ? "ok" : "FAILED", (unsigned)doc.text.size(),
                  (unsigned long)(millis() - t0));
    if (!ok)
        dirty = true; // show the notice
}

extern size_t shortestSinceSnapshot;

void edited(uint32_t now) {
    if (!doc.unsaved)
        doc.firstUnsavedAt = now;
    doc.unsaved = true;
    // Remember how far Backspace reached into text Dropbox already has.
    if (doc.text.size() < doc.sync.keptLength)
        doc.sync.keptLength = doc.text.size();
    if (doc.text.size() < shortestSinceSnapshot)
        shortestSinceSnapshot = doc.text.size();
}

bool online() {
    return WiFi.status() == WL_CONNECTED;
}

const char *DRAFTS_CACHE = "/blog/.drafts"; // name, rev, draft (0/1), modified per line

std::vector<DraftInfo> loadDraftsCache() {
    std::vector<DraftInfo> all;
    std::string data;
    if (!tw::storage::read(DRAFTS_CACHE, data))
        return all;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos)
            break;
        std::string line = data.substr(pos, nl - pos);
        pos = nl + 1;
        size_t a = line.find('\t'), b = line.find('\t', a + 1), c = line.find('\t', b + 1);
        if (c == std::string::npos)
            continue;
        DraftInfo d;
        d.name = line.substr(0, a);
        d.rev = line.substr(a + 1, b - a - 1);
        d.draft = line[b + 1] == '1';
        d.modified = strtoul(line.c_str() + c + 1, nullptr, 10);
        all.push_back(d);
    }
    return all;
}

void saveDraftsCache(const std::vector<DraftInfo> &all) {
    std::string data;
    for (const DraftInfo &d : all)
        data += d.name + "\t" + d.rev + "\t" + (d.draft ? "1" : "0") + "\t" + std::to_string(d.modified) + "\n";
    tw::storage::write(DRAFTS_CACHE, data);
}

// Drafts only, newest first.
std::vector<DraftInfo> draftsFrom(const std::vector<DraftInfo> &all) {
    std::vector<DraftInfo> drafts;
    for (const DraftInfo &d : all) {
        if (d.draft)
            drafts.push_back(d);
    }
    std::sort(drafts.begin(), drafts.end(), [](const DraftInfo &a, const DraftInfo &b) { return a.modified > b.modified; });
    return drafts;
}

// Published essays start with front matter ("---" lines) that sets
// isPublished: true. No front matter, or anything else, means a draft.
bool isDraftText(const std::string &start) {
    if (start.rfind("---", 0) != 0)
        return true;
    size_t end = start.find("\n---", 3);
    std::string front = start.substr(0, end == std::string::npos ? start.size() : end);
    size_t key = front.find("isPublished:");
    if (key == std::string::npos)
        return true;
    size_t value = front.find_first_not_of(" \t'\"", key + strlen("isPublished:"));
    return value == std::string::npos || front.compare(value, 4, "true") != 0;
}

bool endsWith(const std::string &s, const char *suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool isMarkdown(const std::string &name) {
    return endsWith(name, ".md") || endsWith(name, ".mdx");
}

bool isEssay(const std::string &localPath) {
    return localPath.rfind("/blog/", 0) == 0;
}

// Dropbox ignores case in names, so comparisons do too.
std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// New drafts: untitled.mdx, untitled-1.mdx, untitled-2.mdx, ...
std::string untitledName(int n) {
    return n == 0 ? "untitled.mdx" : "untitled-" + std::to_string(n) + ".mdx";
}

void removeLocalCopy(const std::string &localPath) {
    tw::storage::remove(localPath.c_str());
    tw::storage::remove((localPath + ".sync").c_str());
}

// An essay started on the Inkplate (never synced) with nothing in it yet.
bool isBlankNewDraft(const std::string &localPath) {
    std::string text;
    tw::SyncState state;
    tw::loadSyncState(localPath, state);
    return isEssay(localPath) && state.rev.empty() && tw::storage::read(localPath.c_str(), text) && tw::isBlank(text);
}

// Background work runs in its own task, so typing never waits on the network.
// A file sync works on a snapshot of the note; text typed meanwhile stays local
// and goes up next time.
SyncJob syncJob;
volatile bool syncRunning = false;
volatile bool syncDone = false;
size_t shortestSinceSnapshot = 0; // Backspace below the snapshot while syncing
TaskHandle_t syncTask;

bool blankNewDraftOpen() {
    return isEssay(doc.path) && doc.sync.rev.empty() && tw::isBlank(doc.text) &&
           !(syncRunning && syncJob.localPath == doc.path);
}

// What the list shows: drafts started here and not in Dropbox yet first (dated
// by their last save), then Dropbox's drafts, newest first. New drafts left
// blank are removed on the way.
std::vector<DraftInfo> listDrafts(const std::vector<DraftInfo> &all) {
    std::vector<DraftInfo> list;
    for (const std::string &name : tw::storage::list("/blog")) {
        if (!isMarkdown(name))
            continue;
        bool inDropbox = std::any_of(all.begin(), all.end(), [&](const DraftInfo &d) { return lowercase(d.name) == lowercase(name); });
        if (inDropbox)
            continue;
        std::string path = "/blog/" + name;
        if (path != doc.path && isBlankNewDraft(path)) {
            removeLocalCopy(path);
            continue;
        }
        DraftInfo d;
        d.name = name;
        d.draft = true;
        d.modified = tw::storage::modified(path.c_str());
        if (d.modified < 1700000000) // written before the clock was set
            d.modified = 0;
        list.push_back(d);
    }
    // Dropbox's drafts that have a copy here (one that couldn't be downloaded,
    // or was just deleted, would open as an empty page).
    for (const DraftInfo &d : draftsFrom(all)) {
        std::string path = "/blog/" + d.name;
        if (path == doc.path || tw::storage::exists(path.c_str()))
            list.push_back(d);
    }
    return list;
}

// Brings one draft's copy on the Inkplate up to date with Dropbox's, unless it
// has changes of its own (the regular sync merges those).
bool fetchDraft(const DraftInfo &d) {
    std::string local = "/blog/" + d.name;
    tw::SyncState state;
    std::string text;
    bool exists = tw::storage::read(local.c_str(), text);
    tw::loadSyncState(local, state);
    if (exists && (state.rev == d.rev || tw::hasLocalChanges(text, state)))
        return false;
    std::string rev;
    if (dropbox.download(std::string(ESSAYS) + "/" + d.name, text, rev) != tw::Dropbox::Result::Ok)
        return false;
    state.rev = rev;
    state.syncedLength = state.keptLength = text.size();
    return tw::storage::write(local.c_str(), text) && tw::saveSyncState(local, state);
}

// Lists /Site/blog, sorts drafts from published essays (reading only the start
// of files that changed since last time), keeps every draft on the Inkplate,
// and removes copies of essays that are now published or gone.
void refreshDrafts(SyncJob &j) {
    j.downloaded = j.removed = 0;
    std::vector<tw::Dropbox::Entry> entries;
    j.listed = dropbox.list(ESSAYS, entries) == tw::Dropbox::Result::Ok;
    if (!j.listed)
        return;
    std::vector<DraftInfo> cached = loadDraftsCache();
    j.all.clear();
    for (const tw::Dropbox::Entry &e : entries) {
        if (e.folder || !isMarkdown(e.name))
            continue;
        DraftInfo d;
        d.name = e.name;
        d.rev = e.rev;
        d.modified = e.modified;
        auto known = std::find_if(cached.begin(), cached.end(), [&](const DraftInfo &c) { return c.name == e.name; });
        if (known != cached.end() && known->rev == e.rev) {
            d.draft = known->draft;
        } else {
            std::string start;
            if (dropbox.downloadStart(std::string(ESSAYS) + "/" + e.name, 1024, start) != tw::Dropbox::Result::Ok)
                continue; // try again next refresh
            d.draft = isDraftText(start);
        }
        j.all.push_back(d);
        if (d.draft && "/blog/" + d.name != j.openPath && fetchDraft(d))
            j.downloaded++;
    }
    saveDraftsCache(j.all);

    // Copies of essays that are no longer drafts, and have nothing unsynced.
    for (const std::string &name : tw::storage::list("/blog")) {
        if (!isMarkdown(name) || "/blog/" + name == j.openPath)
            continue;
        bool isDraft = std::any_of(j.all.begin(), j.all.end(), [&](const DraftInfo &d) { return d.draft && d.name == name; });
        if (isDraft)
            continue;
        std::string local = "/blog/" + name, text;
        tw::SyncState state;
        tw::storage::read(local.c_str(), text);
        tw::loadSyncState(local, state);
        if (tw::hasLocalChanges(text, state))
            continue;
        tw::storage::remove(local.c_str());
        tw::storage::remove((local + ".sync").c_str());
        j.removed++;
    }
}

// A new draft's first upload: if its name was taken in Dropbox meanwhile (the
// laptop started an untitled.mdx too, say), take the next free untitled name
// rather than merge into that file.
bool pickFreeName(SyncJob &j) {
    std::string name = j.remotePath.substr(strlen(ESSAYS) + 1);
    for (int n = -1; n < 1000; n++) {
        std::string candidate = n < 0 ? name : untitledName(n);
        if (n >= 0 && (candidate == lowercase(name) || tw::storage::exists(("/blog/" + candidate).c_str())))
            continue;
        tw::Dropbox::Entry entry;
        tw::Dropbox::Result r = dropbox.metadata(std::string(ESSAYS) + "/" + candidate, entry);
        if (r == tw::Dropbox::Result::Error)
            return false;
        if (r == tw::Dropbox::Result::NotFound) {
            if (n >= 0) {
                j.renamedTo = candidate;
                j.remotePath = std::string(ESSAYS) + "/" + candidate;
            }
            return true;
        }
    }
    return false;
}

void syncTaskLoop(void *) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (syncJob.kind == JobKind::Drafts)
            refreshDrafts(syncJob);
        else if (syncJob.newDraft && !tw::isBlank(syncJob.text) && !pickFreeName(syncJob))
            syncJob.result = {tw::SyncResult::Failed, "checking the name: " + dropbox.lastError(), false};
        else
            syncJob.result = tw::syncFile(dropbox, syncJob.remotePath, syncJob.text, syncJob.state,
                                          syncJob.blankMeansAbsent);
        syncDone = true;
    }
}

bool isDailyNote(const std::string &remotePath) {
    return remotePath.rfind(DAILY_NOTES, 0) == 0;
}

// Blank daily notes and blank new drafts stay out of Dropbox. Clearing a synced
// daily note deletes it from Dropbox; clearing a synced essay doesn't.
void setFileJob(SyncJob &j) {
    j.kind = JobKind::File;
    j.newDraft = isEssay(j.localPath) && j.state.rev.empty();
    j.blankMeansAbsent = isDailyNote(j.remotePath) || j.newDraft;
    j.renamedTo.clear();
}

void startSync(const char *why) {
    if (syncRunning || doc.remotePath.empty() || !online())
        return;
    if (isEssay(doc.path) && doc.sync.rev.empty() && tw::isBlank(doc.text)) {
        doc.checkedRemote = true; // a blank new draft: nothing to upload yet
        return;
    }
    save(why);
    syncJob.localPath = doc.path;
    syncJob.remotePath = doc.remotePath;
    syncJob.text = doc.text;
    syncJob.snapshotSize = doc.text.size();
    syncJob.state = doc.sync;
    setFileJob(syncJob);
    syncJob.why = why;
    syncJob.startedAt = millis();
    shortestSinceSnapshot = doc.text.size();
    doc.lastSyncAttemptAt = syncJob.startedAt;
    syncDone = false;
    syncRunning = true;
    xTaskNotifyGive(syncTask);
}

// Starts a background sync for a note other than the open one (e.g. yesterday's,
// left unsynced while offline). Its text is read from flash.
void startBacklogSync(const std::string &localPath, const std::string &remotePath) {
    if (syncRunning || !online())
        return;
    syncJob.localPath = localPath;
    syncJob.remotePath = remotePath;
    if (!tw::storage::read(localPath.c_str(), syncJob.text))
        return;
    syncJob.snapshotSize = syncJob.text.size();
    tw::loadSyncState(localPath, syncJob.state);
    setFileJob(syncJob);
    syncJob.why = "backlog";
    syncJob.startedAt = millis();
    syncDone = false;
    syncRunning = true;
    xTaskNotifyGive(syncTask);
}

// Looks for another note or essay in flash with changes Dropbox doesn't have yet
// (other than those in skip).
bool findBacklog(std::string &localPath, std::string &remotePath, const std::vector<std::string> &skip = {}) {
    struct Folder {
        const char *local;
        std::string remote;
    };
    const Folder folders[] = {{"/daily/", DAILY_NOTES}, {"/blog/", std::string(ESSAYS) + "/"}};
    for (const Folder &folder : folders) {
    std::string dir(folder.local);
    dir.pop_back();
    for (const std::string &name : tw::storage::list(dir.c_str())) {
        if (!isMarkdown(name) || name == "undated.md")
            continue;
        std::string path = folder.local + name;
        if (path == doc.path || std::find(skip.begin(), skip.end(), path) != skip.end())
            continue;
        // Only notes that have synced before: older notes written before sync
        // existed are left for you to decide about. New drafts started here
        // are the exception, once they have something in them.
        tw::SyncState state;
        if (!tw::loadSyncState(path, state))
            continue;
        if (state.rev.empty() && (!isEssay(path) || isBlankNewDraft(path)))
            continue;
        long size = tw::storage::size(path.c_str());
        if ((size >= 0 && (size_t)size != state.syncedLength) || state.keptLength < state.syncedLength) {
            localPath = path;
            remotePath = folder.remote + name;
            return true;
        }
    }
    }
    return false;
}

void finishDraftsRefresh();
void moveNewDraft(SyncJob &j);
void showOpenScreen();
void relistDrafts();
std::string essayTitle(const std::string &name);
void rememberOpenEssay(const std::string &name);

// Cmd+S on the list: syncs the open note, then every other file waiting, then
// refreshes the list, one after another.
bool syncAllActive = false;
bool syncAllListed = false;
bool syncAllFailed = false;
std::vector<std::string> syncAllTried; // local paths synced this round (a failure isn't retried)
std::string openNotice;                // the list's footer until the next key

// Applies a finished sync to the note (or to the file it was for, if you've
// moved on to another note since).
void finishSync() {
    if (!syncDone)
        return;
    syncDone = false;
    syncRunning = false;
    SyncJob &j = syncJob;
    if (j.kind == JobKind::Drafts) {
        finishDraftsRefresh();
        return;
    }
    const tw::SyncResult &r = j.result;
    if (syncAllActive && r.outcome == tw::SyncResult::Failed)
        syncAllFailed = true;
    if (!j.renamedTo.empty() && r.outcome != tw::SyncResult::Failed)
        moveNewDraft(j);
    static const char *OUTCOMES[] = {"up to date", "uploaded", "downloaded", "merged", "conflict", "deleted",
                                     "FAILED"};
    Serial.printf("sync (%s): %s in %lu ms, rev %s %s\n", j.why, OUTCOMES[r.outcome],
                  (unsigned long)(millis() - j.startedAt), j.state.rev.c_str(), r.message.c_str());

    if (r.outcome == tw::SyncResult::Deleted) {
        bool open = j.localPath == doc.path;
        if (open && (shortestSinceSnapshot < j.snapshotSize || doc.text.size() != j.snapshotSize)) {
            // You typed into it while the sync ran: keep all of it. The next sync
            // puts it back in Dropbox, as with edits made offline.
            Serial.printf("sync: %s was deleted in Dropbox, but you typed since: keeping it\n", j.localPath.c_str());
            doc.unsaved = true;
            return;
        }
        // Deleted in Dropbox: remove the Inkplate's copy too.
        removeLocalCopy(j.localPath);
        if (open && isEssay(doc.path)) {
            // Back to the list, rather than a blank page that typing would
            // bring back as a new file.
            std::string title = essayTitle(doc.name);
            doc.unsaved = false;
            rememberOpenEssay("");
            openToday();
            showOpenScreen();
            openNotice = "“" + title + "” was deleted in Dropbox.";
        } else if (open) {
            doc.text.clear();
            doc.sync = tw::SyncState();
            doc.unsaved = false;
            doc.checkedRemote = true;
            page.reset();
            forceFull = true;
        } else if (isEssay(j.localPath)) {
            relistDrafts();
        }
        return;
    }
    if (j.localPath != doc.path) {
        if (r.outcome != tw::SyncResult::Failed) {
            if (r.textChanged)
                tw::storage::write(j.localPath.c_str(), j.text);
            tw::saveSyncState(j.localPath, j.state);
        }
        return;
    }
    if (r.outcome == tw::SyncResult::Failed) {
        doc.syncFailures++;
        doc.syncError = r.message;
        dirty = true;
        return;
    }
    doc.checkedRemote = true;
    doc.syncFailures = 0;
    doc.syncError.clear();

    // What you typed while the sync ran: only additions, or did Backspace reach
    // into the snapshot?
    bool onlyAdded = shortestSinceSnapshot >= j.snapshotSize;
    std::string typedSince = onlyAdded ? doc.text.substr(j.snapshotSize) : "";
    if (r.textChanged) {
        if (onlyAdded) {
            history.clear(); // the text changed underneath: old steps no longer apply
            reading = false;
            doc.text = j.text + typedSince;
            doc.sync = j.state;
            doc.unsaved = true;
            save("after sync");
            page.showEnd(doc.text);
            forceFull = true;
        }
        // Otherwise keep your text and the old sync state: the next sync sees
        // Dropbox's new version and merges (or saves a conflicted copy).
    } else {
        doc.sync = j.state; // agreed on the snapshot
        if (!onlyAdded)
            doc.sync.keptLength = shortestSinceSnapshot;
        tw::saveSyncState(doc.path, doc.sync);
    }
    if (r.outcome == tw::SyncResult::Conflict)
        notice = "Sync conflict. Saved in Dropbox as " +
                 r.message.substr(r.message.rfind('/') + 1);
    dirty = true;
}

// Removes the last character (all its UTF-8 bytes); returns what was removed.
std::string deleteLastChar() {
    size_t cut = doc.text.size();
    while (cut > 0) {
        uint8_t byte = doc.text[--cut];
        if ((byte & 0xC0) != 0x80)
            break;
    }
    std::string removed = doc.text.substr(cut);
    doc.text.resize(cut);
    return removed;
}

// Option+Backspace: removes the previous word and the spaces after it.
std::string deleteLastWord() {
    std::string &t = doc.text;
    size_t cut = t.size();
    while (cut > 0 && (t[cut - 1] == ' ' || t[cut - 1] == '\n'))
        cut--;
    while (cut > 0 && t[cut - 1] != ' ' && t[cut - 1] != '\n')
        cut--;
    std::string removed = t.substr(cut);
    t.resize(cut);
    return removed;
}

// ---- Input ----

void markChanged(uint32_t now) {
    if (!dirty)
        firstUnshownKeyAt = now;
    dirty = true;
}

// ---- Open: today's note and drafts ----
//
// Cmd+O shows today's note and the drafts in /Site/blog, newest first, right
// away from what's on the Inkplate, and checks Dropbox in the background.
// Return opens one at its end to keep writing.

enum class Screen { Writing, Open };
Screen screen = Screen::Writing;
std::vector<DraftInfo> drafts; // what the Open screen lists after today's note
size_t selected = 0;           // 0: today's note, then drafts
size_t firstVisible = 0;       // first draft row on screen
bool openDirty = false;
bool draftsWanted = true;  // refresh drafts from Dropbox when free
bool draftsChecking = false;
uint32_t lastDraftsRefreshAt = 0;
const uint32_t DRAFTS_REFRESH_EVERY_MS = 10 * 60 * 1000;

// "design-engineering-revisited.mdx" -> "Design engineering revisited"
std::string essayTitle(const std::string &name) {
    std::string t = name.substr(0, name.rfind('.'));
    std::replace(t.begin(), t.end(), '-', ' ');
    if (!t.empty() && t[0] >= 'a' && t[0] <= 'z')
        t[0] -= 32;
    return t;
}

void startDraftsRefresh() {
    if (syncRunning || !online())
        return;
    syncJob.kind = JobKind::Drafts;
    syncJob.openPath = doc.path;
    syncJob.why = "drafts";
    syncJob.startedAt = millis();
    draftsWanted = false;
    draftsChecking = true;
    lastDraftsRefreshAt = millis();
    syncDone = false;
    syncRunning = true;
    xTaskNotifyGive(syncTask);
}

void finishDraftsRefresh() {
    SyncJob &j = syncJob;
    draftsChecking = false;
    if (syncAllActive && !j.listed)
        syncAllFailed = true;
    if (!j.listed) {
        Serial.printf("drafts: couldn't list %s: %s\n", ESSAYS, dropbox.lastError().c_str());
        return;
    }
    std::vector<DraftInfo> fresh = listDrafts(j.all);
    Serial.printf("drafts: %u of %u essays, %d downloaded, %d removed, %lu ms\n", (unsigned)fresh.size(),
                  (unsigned)j.all.size(), j.downloaded, j.removed, (unsigned long)(millis() - j.startedAt));
    bool changed = fresh.size() != drafts.size();
    for (size_t i = 0; !changed && i < fresh.size(); i++)
        changed = fresh[i].name != drafts[i].name || fresh[i].modified != drafts[i].modified;
    std::string selectedName = selected > 0 && selected <= drafts.size() ? drafts[selected - 1].name : "";
    drafts = fresh;
    if (selected > 0) {
        selected = 0;
        for (size_t i = 0; i < drafts.size(); i++) {
            if (drafts[i].name == selectedName)
                selected = i + 1;
        }
    }
    if (screen == Screen::Open)
        openDirty = openDirty || changed;
}

void rememberOpenEssay(const std::string &name) {
    Preferences prefs;
    prefs.begin("typewriter", false);
    prefs.putString("essay", name.c_str());
    prefs.end();
}

void openEssay(const std::string &name) {
    save("switching");
    openDocument("/blog/" + name, std::string(ESSAYS) + "/" + name, name);
    rememberOpenEssay(name);
}

// After a new draft went up under another name (see pickFreeName), its copy
// here follows.
void moveNewDraft(SyncJob &j) {
    std::string to = "/blog/" + j.renamedTo;
    Serial.printf("sync: %s was taken in Dropbox, now %s\n", j.localPath.c_str(), to.c_str());
    if (j.localPath == doc.path) {
        doc.path = to;
        doc.remotePath = j.remotePath;
        doc.name = j.renamedTo;
        doc.unsaved = true;
        save("renamed");
        rememberOpenEssay(doc.name);
    } else {
        std::string text;
        if (tw::storage::read(j.localPath.c_str(), text))
            tw::storage::write(to.c_str(), text);
    }
    removeLocalCopy(j.localPath);
    j.localPath = to;
    relistDrafts();
}

// Rebuilds the list after a draft here was added, renamed or removed.
void relistDrafts() {
    drafts = listDrafts(loadDraftsCache());
    if (selected > drafts.size())
        selected = drafts.size();
    if (screen == Screen::Open)
        openDirty = true;
}

// Cmd+N on the list: a new, empty essay named untitled.mdx, or the next free
// untitled-N.mdx among the essays in Dropbox and on the Inkplate.
void startNewDraft() {
    std::vector<DraftInfo> all = loadDraftsCache();
    std::vector<std::string> local = tw::storage::list("/blog");
    std::string name;
    for (int n = 0; name.empty(); n++) {
        std::string candidate = untitledName(n);
        bool taken = std::any_of(all.begin(), all.end(), [&](const DraftInfo &d) { return lowercase(d.name) == candidate; }) ||
                     std::any_of(local.begin(), local.end(), [&](const std::string &f) { return lowercase(f) == candidate; });
        if (!taken)
            name = candidate;
    }
    save("switching");
    std::string path = "/blog/" + name;
    tw::storage::write(path.c_str(), "");
    tw::saveSyncState(path, tw::SyncState());
    openDocument(path, std::string(ESSAYS) + "/" + name, name);
    doc.checkedRemote = true; // nothing in Dropbox to pick up
    rememberOpenEssay(name);
}

void openTodayFromList() {
    rememberOpenEssay("");
    if (doc.path.rfind("/daily/", 0) != 0) {
        save("switching");
        openToday();
    }
}

void showOpenScreen() {
    // Leaving a new draft with nothing in it: openDocument removes it.
    if (blankNewDraftOpen()) {
        rememberOpenEssay("");
        openToday();
    }
    save("opening the list");
    screen = Screen::Open;
    openNotice.clear();
    drafts = listDrafts(loadDraftsCache());
    selected = 0;
    for (size_t i = 0; i < drafts.size(); i++) {
        if ("/blog/" + drafts[i].name == doc.path)
            selected = i + 1;
    }
    firstVisible = 0;
    draftsWanted = true; // check Dropbox for new drafts in the background
    forceFull = true;
}

void closeOpenScreen() {
    screen = Screen::Writing;
    forceFull = true;
}

void startSyncAll() {
    save("Cmd+S");
    if (!online()) {
        openNotice = "Saved locally. Connect to Wi-Fi to sync.";
        return;
    }
    syncAllActive = true;
    syncAllListed = syncAllFailed = false;
    syncAllTried.clear();
    openNotice = "Syncing…";
}

// Starts the next step of a Cmd+S sync when the last one is done.
void continueSyncAll() {
    if (!syncAllActive || syncRunning)
        return;
    std::string local, remote;
    if (!online()) {
        syncAllActive = false;
        openNotice = "Saved locally. Connect to Wi-Fi to sync.";
    } else if (!doc.remotePath.empty() &&
               std::find(syncAllTried.begin(), syncAllTried.end(), doc.path) == syncAllTried.end()) {
        syncAllTried.push_back(doc.path);
        startSync("Cmd+S"); // a blank new draft starts nothing
        return;
    } else if (findBacklog(local, remote, syncAllTried)) {
        syncAllTried.push_back(local);
        startBacklogSync(local, remote);
        return;
    } else if (!syncAllListed) {
        syncAllListed = true;
        startDraftsRefresh();
        return;
    } else {
        syncAllActive = false;
        openNotice = syncAllFailed ? "Partially synced. Saved locally." : "Synced";
    }
    if (screen == Screen::Open)
        openDirty = true;
}

void handleOpenKey(const tw::KeyEvent &e) {
    if (!openNotice.empty() && !syncAllActive) {
        openNotice.clear();
        openDirty = true;
    }
    switch (e.type) {
    case tw::KeyEvent::Up:
        if (selected > 0)
            selected--;
        break;
    case tw::KeyEvent::Down:
        if (selected < drafts.size())
            selected++;
        break;
    case tw::KeyEvent::Enter:
        if (selected == 0)
            openTodayFromList();
        else
            openEssay(drafts[selected - 1].name);
        closeOpenScreen();
        return;
    case tw::KeyEvent::Escape:
        closeOpenScreen();
        return;
    case tw::KeyEvent::Shortcut:
        if (e.key == 'd')
            openTodayFromList();
        if (e.key == 'n')
            startNewDraft();
        if (e.key == 'o' || e.key == 'd' || e.key == 'n')
            closeOpenScreen();
        if (e.key == 's') {
            startSyncAll();
            openDirty = true;
        }
        return;
    default:
        return;
    }
    openDirty = true;
}

void stopReading() {
    if (!reading)
        return;
    reading = false;
    forceFull = true; // back to the writing page, cleanly
}

// ↑/↓ page through what you've written; Esc or Backspace return to the end;
// typing returns to the end and types there. Returns true if the key was used.
bool handleReadingKey(const tw::KeyEvent &e) {
    if (!reading) {
        if (e.type != tw::KeyEvent::Up)
            return false;
        readingPages = page.readingPages(doc.text);
        if (readingPages.starts.empty()) {
            notice = "This is the first page.";
            dirty = true;
            return true;
        }
        reading = true;
        readingPage = readingPages.starts.size() - 1;
        readingDirty = true;
        return true;
    }
    switch (e.type) {
    case tw::KeyEvent::Up:
        if (readingPage > 0) {
            readingPage--;
            readingDirty = true;
        }
        return true;
    case tw::KeyEvent::Down:
        if (readingPage + 1 < readingPages.starts.size()) {
            readingPage++;
            readingDirty = true;
        } else {
            stopReading();
        }
        return true;
    case tw::KeyEvent::Left:
    case tw::KeyEvent::Right:
        return true;
    case tw::KeyEvent::Escape:
    case tw::KeyEvent::Backspace:
    case tw::KeyEvent::BackspaceWord:
        stopReading();
        return true;
    default:
        stopReading(); // typing and shortcuts carry on at the end
        return false;
    }
}

void handleKey(const tw::KeyEvent &e, uint32_t now) {
    lastKeyAt = now;
    if (screen == Screen::Open) {
        handleOpenKey(e);
        return;
    }
    if (showStatus || !notice.empty()) {
        showStatus = false;
        notice.clear();
        markChanged(now);
    }
    if (handleReadingKey(e))
        return;
    switch (e.type) {
    case tw::KeyEvent::Text:
        doc.text += e.text;
        history.typed(e.text, now);
        break;
    case tw::KeyEvent::Backspace:
        history.deleted(deleteLastChar(), now);
        break;
    case tw::KeyEvent::BackspaceWord:
        history.deleted(deleteLastWord(), now);
        break;
    case tw::KeyEvent::Enter:
        doc.text += '\n';
        history.typed("\n", now);
        break;
    case tw::KeyEvent::Tab:
        doc.text += "    ";
        history.typed("    ", now);
        break;
    case tw::KeyEvent::Escape:
        return;
    case tw::KeyEvent::Shortcut:
        if (e.key == 'z') {
            if (e.shift ? history.redo(doc.text) : history.undo(doc.text)) {
                edited(now);
                markChanged(now);
            }
        } else if (e.key == 's') {
            save("Cmd+S");
            if (blankNewDraftOpen())
                notice = "Nothing to save yet.";
            else if (doc.remotePath.empty())
                notice = "Unable to sync undated note. Saved locally.";
            else if (!online())
                notice = "Saved locally. Connect to Wi-Fi to sync.";
            else
                startSync("Cmd+S");
            markChanged(now);
        } else if (e.key == 'i') {
            showStatus = true;
            checkBattery(now, true); // a fresh reading, not one from up to a minute ago
            markChanged(now);
        } else if (e.key == 'l') {
            forceFull = true;
        } else if (e.key == 'k' && e.shift) {
            keyboard.forget();
        } else if (e.key == 'o') {
            showOpenScreen();
        } else if (e.key == 'd') {
            openTodayFromList();
        }
        return;
    case tw::KeyEvent::Up:
    case tw::KeyEvent::Down:
    case tw::KeyEvent::Left:
    case tw::KeyEvent::Right:
        return; // typewriter: the cursor stays at the end
    }
    edited(now);
    markChanged(now);
}

// Ctrl+T over USB: "<utc epoch> <posix tz>\n" sets the clock (tools/settime.py).
// Any other byte types text, like tools/type.py.
void handleSerial(uint32_t now) {
    static std::string pendingUtf8;
    while (Serial.available()) {
        int b = Serial.read();
        if (b == 0x14) {
            String line = Serial.readStringUntil('\n');
            int space = line.indexOf(' ');
            uint32_t epoch = line.substring(0, space).toInt();
            String tz = space > 0 ? line.substring(space + 1) : "UTC0";
            tz.trim();
            if (epoch > 1700000000) {
                save("clock change");
                tw::wallclock::set(display, epoch, tz.c_str());
                Serial.printf("clock set: %lu %s\n", (unsigned long)epoch, tz.c_str());
                openToday();
            }
            continue;
        }
        tw::KeyEvent nav = {};
        if (b == 0x0F || b == 0x04 || b == 0x0E || b == 0x13 || b == 0x1A) { // Ctrl+O/D/N/S/Z: as with Cmd
            nav.type = tw::KeyEvent::Shortcut;
            nav.key = b == 0x0F ? 'o' : b == 0x04 ? 'd' : b == 0x0E ? 'n' : b == 0x13 ? 's' : 'z';
            nav.ctrl = true;
            handleKey(nav, now);
            continue;
        }
        if (b == 0x1B) { // Esc, or a terminal arrow key: ESC [ A/B/C/D
            delay(5);
            if (Serial.peek() == '[') {
                Serial.read();
                int code = Serial.read();
                nav.type = code == 'A' ? tw::KeyEvent::Up
                         : code == 'B' ? tw::KeyEvent::Down
                         : code == 'C' ? tw::KeyEvent::Right
                                       : tw::KeyEvent::Left;
            } else {
                nav.type = tw::KeyEvent::Escape;
            }
            handleKey(nav, now);
            continue;
        }
        if (b == 0x12) { // Ctrl+R over USB, then "status", "ls", "rm <path>" or "sleep [seconds]"
            String line = Serial.readStringUntil('\n');
            line.trim();
            if (line == "status") {
                Serial.printf("status: screen %s, document %s (%u bytes)\n", screen == Screen::Open ? "list" : "writing",
                              doc.path.c_str(), (unsigned)doc.text.size());
            } else if (line == "ls") {
                for (const char *dir : {"/daily", "/blog"}) {
                    for (const std::string &name : tw::storage::list(dir)) {
                        std::string path = std::string(dir) + "/" + name;
                        Serial.printf("file: %s (%ld bytes, modified %lu)\n", path.c_str(), tw::storage::size(path.c_str()),
                                      (unsigned long)tw::storage::modified(path.c_str()));
                    }
                }
            } else if (line.startsWith("sleep")) { // "sleep <seconds>": sleep now, wake on a timer
                goToSleep(line.length() > 6 ? line.substring(6).toInt() : 0);
            } else if (line.startsWith("rm ")) {
                std::string path = line.substring(3).c_str();
                bool ok = tw::storage::remove(path.c_str());
                tw::storage::remove((path + ".sync").c_str());
                Serial.printf("rm %s: %s\n", path.c_str(), ok ? "ok" : "not found");
                if (path == doc.path)
                    openDocument(doc.path, doc.remotePath, doc.name); // reopen, now empty
            }
            continue;
        }
        if (b == 0x0B) { // Ctrl+K over USB: forget the keyboard
            Serial.println("kbd: forgetting paired keyboards");
            keyboard.forget();
            continue;
        }
        tw::KeyEvent e = {};
        if (b == 0x7F || b == 0x08) {
            e.type = tw::KeyEvent::Backspace;
        } else if (b == '\r' || b == '\n') {
            e.type = tw::KeyEvent::Enter;
        } else if (b >= 0x20) {
            // Collect whole UTF-8 characters before typing them.
            pendingUtf8 += (char)b;
            uint8_t lead = pendingUtf8[0];
            size_t need = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
            if (pendingUtf8.size() < need)
                continue;
            e.type = tw::KeyEvent::Text;
            strlcpy(e.text, pendingUtf8.c_str(), sizeof(e.text));
            pendingUtf8.clear();
        } else {
            continue;
        }
        handleKey(e, now);
    }
}

// ---- Screen ----

// Typical resting voltage of a Li-ion cell at each charge level: the curve is
// flat in the middle and steep near the ends, so a straight line misleads.
const float CELL_CURVE[][2] = {{3.40, 0},  {3.61, 5},  {3.69, 10}, {3.73, 20}, {3.77, 30}, {3.79, 40},
                               {3.82, 50}, {3.86, 60}, {3.91, 70}, {3.97, 80}, {4.05, 90}, {4.15, 100}};

int percentFromVoltage(float v) {
    const int n = sizeof(CELL_CURVE) / sizeof(CELL_CURVE[0]);
    if (v <= CELL_CURVE[0][0])
        return 0;
    for (int i = 1; i < n; i++) {
        if (v <= CELL_CURVE[i][0]) {
            float t = (v - CELL_CURVE[i - 1][0]) / (CELL_CURVE[i][0] - CELL_CURVE[i - 1][0]);
            return CELL_CURVE[i - 1][1] + t * (CELL_CURVE[i][1] - CELL_CURVE[i - 1][1]);
        }
    }
    return 100;
}

// Measures the battery: the average of several readings, smoothed over time
// (radio bursts and the charger's surface charge make single readings jumpy),
// shown in steps of 5%. force: measure now (Cmd+I) rather than once a minute.
void checkBattery(uint32_t now, bool force) {
    static float smoothed = 0;
    if (!force && lastBatteryCheckAt && now - lastBatteryCheckAt < BATTERY_CHECK_EVERY_MS)
        return;
    lastBatteryCheckAt = now;
    float v = 0;
    for (int i = 0; i < 8; i++)
        v += display.readBattery();
    v /= 8;
    int previous = batteryPercent;
    // On USB with no battery, or a full one, the charger's 4.2 V shows through.
    if (v < 3.0 || v >= 4.18) {
        batteryPercent = -1;
        smoothed = 0;
    } else {
        smoothed = smoothed == 0 || previous < 0 ? v : smoothed * 0.7f + v * 0.3f;
        int percent = (percentFromVoltage(smoothed) + 2) / 5 * 5;
        // On battery the charge only goes down; ignore small upward wobbles.
        if (previous >= 0 && percent > previous && percent - previous < 10)
            percent = previous;
        batteryPercent = percent;
    }
    Serial.printf("battery: %.3f V, smoothed %.3f V (%d%%)\n", v, smoothed, batteryPercent);
    bool wasLow = previous >= 0 && previous <= LOW_BATTERY_PERCENT;
    bool isLow = batteryPercent >= 0 && batteryPercent <= LOW_BATTERY_PERCENT;
    if (wasLow != isLow || (showStatus && previous != batteryPercent))
        markChanged(now);
}

int wordCount() {
    int words = 0;
    bool inWord = false;
    for (char c : doc.text) {
        bool space = c == ' ' || c == '\n';
        if (!space && !inWord)
            words++;
        inWord = !space;
    }
    return words;
}

// The footer: problems first, then the Cmd+I status (file and words on the
// left, saved state and power on the right). Empty when all is well.
void footer(char *out, size_t size, char *right, size_t rightSize) {
    out[0] = 0;
    right[0] = 0;
    tw::Keyboard::State kb = keyboard.state();
    if (doc.saveFailed) {
        snprintf(out, size, "Failed to save. Cmd+S to retry.");
    } else if (!notice.empty()) {
        snprintf(out, size, "%s", notice.c_str());
    } else if (!doc.checkedRemote && doc.text.empty() && online() && !doc.remotePath.empty()) {
        snprintf(out, size, "Downloading %s…", doc.name.c_str());
    } else if (doc.syncFailures >= SYNC_FAILURES_BEFORE_NOTICE) {
        snprintf(out, size, "Failed to sync. Saved locally."); // the reason is in the serial log
    } else if (kb == tw::Keyboard::State::Searching || kb == tw::Keyboard::State::Connecting) {
        snprintf(out, size, "Keyboard not connected. Press a key on it to wake it.");
    } else if (!tw::wallclock::isSet()) {
        snprintf(out, size, "Clock unset. Connect to USB or Wi-Fi.");
    } else if (batteryPercent >= 0 && batteryPercent <= LOW_BATTERY_PERCENT) {
        snprintf(out, size, "Low battery: %d%%", batteryPercent);
    } else if (showStatus) {
        char battery[24] = "USB power";
        if (batteryPercent >= 0)
            snprintf(battery, sizeof(battery), "Battery %d%%", batteryPercent);
        int words = wordCount();
        snprintf(out, size, "%s · %d %s", doc.name.c_str(), words, words == 1 ? "word" : "words");
        const char *state = doc.unsaved                              ? "Not saved yet"
                            : doc.remotePath.empty()                 ? "Saved locally, note is undated"
                            : syncRunning                            ? "Syncing…"
                            : !tw::hasLocalChanges(doc.text, doc.sync) && doc.checkedRemote ? "Synced"
                            : online()                               ? "Saved, not synced yet"
                                                                     : "Saved locally";
        snprintf(right, rightSize, "%s · %s", state, battery);
    }
}

std::string shownFooter;
bool footerChanged = false; // since the last render

// Renders the writing page; returns true if the page turned.
bool drawPage() {
    char left[128], right[64];
    footer(left, sizeof(left), right, sizeof(right));
    std::string both = std::string(left) + "|" + right;
    footerChanged = shownFooter != both;
    shownFooter = both;
    return page.render(doc.text, left, right);
}

void drawCentered(int32_t y, const char *s) {
    size_t len = strlen(s);
    int32_t w = tw::measure(Duo40, s, 0, len);
    tw::drawText(display, Duo40, (display.width() - w) / 2, y + Duo40Ascent, s, 0, len);
}

// Full-screen pairing instructions with the passkey.
void drawPairing() {
    char digits[8], spaced[16] = {0};
    snprintf(digits, sizeof(digits), "%06lu", (unsigned long)keyboard.passkey());
    for (int i = 0, j = 0; i < 6; i++) {
        spaced[j++] = digits[i];
        if (i < 5)
            spaced[j++] = ' ';
    }
    page.invalidate();
    display.clearDisplay();
    drawCentered(180, "Type this code on the keyboard,");
    drawCentered(240, "then press Return:");
    display.setFont(&Duo40);
    display.setTextSize(3);
    int32_t w = tw::measure(Duo40, spaced, 0, strlen(spaced)) * 3;
    display.setCursor((display.width() - w) / 2, 330 + Duo40Ascent * 3);
    display.print(spaced);
    display.setTextSize(1);
}

// MM-DD-YYYY, e.g. 09-26-2026.
std::string numericDate(uint32_t epoch) {
    if (!epoch)
        return "";
    time_t t = epoch;
    tm local;
    localtime_r(&t, &local);
    char out[16];
    strftime(out, sizeof(out), "%m-%d-%Y", &local);
    return out;
}

// Height of a font's capital letters above the baseline, in pixels.
int16_t capHeight(const GFXfont &font) {
    return -font.glyph['H' - font.first].yOffset;
}

// Where draft titles start: after the two-character selection marker.
int32_t openTitleX() {
    return 64 + tw::measure(Duo40, "  ", 0, 2);
}

void drawOpenRow(int32_t y, bool isSelected, const std::string &label, const std::string &date) {
    const int16_t marginX = 64;
    std::string row = (isSelected ? "› " : "  ") + label;
    int32_t baseline = y + Duo40Ascent;
    tw::drawText(display, Duo40, marginX, baseline, row.c_str(), 0, row.size());
    // Center the date's digits on the title's capitals.
    int32_t dateBaseline = baseline - capHeight(Duo40) / 2 + capHeight(Duo28) / 2;
    int32_t w = tw::measure(Duo28, date.c_str(), 0, date.size());
    tw::drawText(display, Duo28, display.width() - marginX - w, dateBaseline, date.c_str(), 0, date.size());
}

// Where the Open screen's rows go.
const int16_t OPEN_TOP = 64;                              // the daily note row
const int16_t OPEN_LABEL_Y = OPEN_TOP + Duo40.yAdvance + 48; // "Drafts", after a gap
const int16_t OPEN_LIST_TOP = OPEN_LABEL_Y + 60;           // first draft row
const size_t OPEN_ROWS = (720 - 72 - OPEN_LIST_TOP) / Duo40.yAdvance;

// Scrolls the drafts so the selected one is on screen.
void scrollOpenList() {
    if (selected == 0)
        return;
    size_t i = selected - 1;
    if (i < firstVisible)
        firstVisible = i;
    if (i >= firstVisible + OPEN_ROWS)
        firstVisible = i - OPEN_ROWS + 1;
}

// Top of a row on screen (0: the daily note), or -1 if it's scrolled off.
int32_t openRowTop(size_t row) {
    if (row == 0)
        return OPEN_TOP;
    if (row - 1 < firstVisible || row - 1 >= firstVisible + OPEN_ROWS)
        return -1;
    return OPEN_LIST_TOP + (row - 1 - firstVisible) * Duo40.yAdvance;
}

// Everything the Open screen shows apart from the selection, to tell when an
// arrow key only needs the marker moved.
std::string openContents(const char *status) {
    std::string c = todayName() + "|" + (status ? status : "") + "|" + std::to_string(firstVisible);
    if (drafts.empty())
        c += draftsChecking ? "|checking" : online() ? "|online" : "|offline";
    for (const DraftInfo &d : drafts)
        c += "|" + d.name + " " + std::to_string(d.modified);
    return c;
}

// Moves the "›" marker from one row to the selected one, in the frame buffer.
void moveOpenMarker(size_t from) {
    const int16_t marginX = 64, rowH = Duo40.yAdvance;
    int32_t y = openRowTop(from);
    if (y >= 0)
        display.fillRect(marginX, y, openTitleX() - marginX, rowH, WHITE);
    y = openRowTop(selected);
    if (y >= 0)
        tw::drawText(display, Duo40, marginX, y + Duo40Ascent, "›", 0, strlen("›"));
}

// The daily note, then a "Drafts" section. Drafts scroll; the daily note stays put.
// The footer has the key hint on the left and status (syncing) on the right; the
// hint gives way when both don't fit. Asleep, the status alone is centered.
void drawOpenScreen(const char *status = nullptr, bool asleep = false) {
    const int16_t marginX = 64, top = OPEN_TOP, rowH = Duo40.yAdvance;
    const int16_t labelY = OPEN_LABEL_Y, listTop = OPEN_LIST_TOP;
    const size_t rows = OPEN_ROWS;
    scrollOpenList();

    page.invalidate(); // the writing page has to be redrawn in full next time
    display.clearDisplay();
    std::string today = todayName().substr(0, 10); // YYYY-MM-DD
    std::string todayDate = today.size() == 10 ? today.substr(5, 2) + "-" + today.substr(8, 2) + "-" + today.substr(0, 4) : "";
    drawOpenRow(top, selected == 0, "Daily note", todayDate);

    const char *label = "Drafts";
    tw::drawText(display, Duo28, openTitleX(), labelY + Duo28Ascent, label, 0, strlen(label));
    if (drafts.empty()) {
        const char *none = draftsChecking ? "Checking Dropbox…" : online() ? "No drafts found" : "No drafts yet. Connect to Wi-Fi to check.";
        tw::drawText(display, Duo28, openTitleX(), listTop + Duo28Ascent, none, 0, strlen(none));
    }
    for (size_t i = firstVisible; i < drafts.size() && i < firstVisible + rows; i++)
        drawOpenRow(listTop + (i - firstVisible) * rowH, selected == i + 1, essayTitle(drafts[i].name),
                    numericDate(drafts[i].modified));

    const int32_t baseline = 720 - 44 + Duo24Ascent;
    const char *hint = "↑↓ choose · Return open · Esc back";
    int32_t hintW = tw::measure(Duo24, hint, 0, strlen(hint));
    int32_t statusW = status ? tw::measure(Duo24, status, 0, strlen(status)) : 0;
    const int32_t gap = 48;
    if (!asleep && (!status || hintW + gap + statusW <= display.width() - 2 * marginX))
        tw::drawText(display, Duo24, marginX, baseline, hint, 0, strlen(hint));
    int32_t statusX = asleep ? (display.width() - statusW) / 2 : display.width() - marginX - statusW;
    if (status)
        tw::drawText(display, Duo24, statusX, baseline, status, 0, strlen(status));
}

void fullRefresh(const char *why) {
    uint32_t t0 = millis();
    partialsSinceClean = false;
    drawPage();
    display.display();
    panelOn = false;
    lastUpdateAt = millis();
    Serial.printf("full refresh (%s): %lu ms\n", why, (unsigned long)(lastUpdateAt - t0));
}

// ---- Sleep ----

// Waits (up to a limit) for the background task to go idle.
void waitForBackground(uint32_t limitMs) {
    uint32_t start = millis();
    while (syncRunning && !syncDone && millis() - start < limitMs)
        delay(50);
    finishSync();
}

// Saves, syncs what it can, shows the list with a small note (so your writing
// isn't left on screen), and sleeps until the WAKE button. Waking restarts the
// program at the list.
void goToSleep(uint32_t wakeAfterSeconds) {
    Serial.println("sleep: going to sleep");
    save("sleep");
    waitForBackground(30000);
    if (online() && !doc.remotePath.empty() && tw::hasLocalChanges(doc.text, doc.sync)) {
        startSync("sleep");
        waitForBackground(30000);
    }
    resumeAfterSleep = true;
    reading = false;
    if (screen != Screen::Open)
        showOpenScreen();
    // A full refresh: the resting image should be clean, with no ghosting.
    drawOpenScreen(ASLEEP, true);
    display.display();
    display.einkOff();
    WiFi.disconnect(true);
    esp_sleep_enable_ext0_wakeup(WAKE_BUTTON, 0);
    if (wakeAfterSeconds) // testing over USB
        esp_sleep_enable_timer_wakeup((uint64_t)wakeAfterSeconds * 1000000ULL);
    esp_deep_sleep_start(); // does not return
}

// The WAKE button also puts it to sleep. It must be let go once first (it is
// usually still held from waking), and again before sleeping, or the press
// would wake it straight back up.
void checkWakeButton(uint32_t now) {
    static bool releasedSinceStart = false;
    static uint32_t pressedAt = 0;
    bool pressed = digitalRead(WAKE_BUTTON) == LOW;
    if (!pressed) {
        releasedSinceStart = true;
        pressedAt = 0;
        return;
    }
    if (!releasedSinceStart)
        return;
    if (!pressedAt)
        pressedAt = now;
    if (now - pressedAt < 50) // debounce
        return;
    Serial.println("sleep: WAKE button pressed");
    uint32_t waitStart = millis();
    while (digitalRead(WAKE_BUTTON) == LOW && millis() - waitStart < 5000)
        delay(10);
    goToSleep();
}

// ---- Main ----

void setup() {
    Serial.begin(115200);
    display.begin();
    display.setFullUpdateThreshold(0); // we decide when to do full refreshes
    if (!tw::storage::begin())
        Serial.println("storage: mount FAILED");
    Serial.printf("storage: %u of %u KB used\n", (unsigned)(tw::storage::usedBytes() / 1024),
                  (unsigned)(tw::storage::totalBytes() / 1024));
    pinMode(WAKE_BUTTON, INPUT); // GPIO 36 is input-only; the board pulls it up
    tw::wallclock::begin(display);
    tw::storage::remove("/blog/.list"); // replaced by DRAFTS_CACHE
    // Reopen the essay you were writing, if any; otherwise today's note.
    Preferences prefs;
    prefs.begin("typewriter", true);
    String essay = prefs.getString("essay", "");
    prefs.end();
    if (essay.length() && tw::storage::exists(("/blog/" + std::string(essay.c_str())).c_str()))
        openEssay(essay.c_str());
    else
        openToday();
    // Waking from sleep and switching on both start at the list, with that note
    // selected.
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool woke = (cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_TIMER) && resumeAfterSleep;
    Serial.printf("start: %s\n", woke ? "woke" : "power on");
    showOpenScreen();
    resumeAfterSleep = false;
    lastKeyAt = millis(); // the sleep timer starts now
    keyboard.begin("Typewriter");
    xTaskCreatePinnedToCore(syncTaskLoop, "sync", 12288, nullptr, 1, &syncTask, 0);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// Once Wi-Fi has the time, keep the RTC right (and leave undated.md behind).
void checkNetworkClock() {
    static bool started = false;
    if (!started && online()) {
        // Started only once Wi-Fi is up, with servers given by IP address (Google
        // Public NTP): if a name lookup fails, lwIP's SNTP retry asserts and
        // restarts the board.
        started = true;
        configTzTime(tw::wallclock::zone().c_str(), "216.239.35.0", "216.239.35.4");
    }
    if (clockFromNetwork || !started || sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED)
        return;
    clockFromNetwork = true;
    bool wasSet = tw::wallclock::isSet();
    tw::wallclock::set(display, time(nullptr), tw::wallclock::zone().c_str());
    Serial.printf("clock: set from the network\n");
    if (!wasSet) {
        save("clock set");
        openToday();
    }
}

void loop() {
    uint32_t now = millis();
    handleSerial(now);
    tw::KeyEvent e;
    while (keyboard.poll(e))
        handleKey(e, now);

    // Autosave: after a pause, or regularly during long stretches of typing.
    if (doc.unsaved && !doc.saveFailed &&
        (now - lastKeyAt > SAVE_AFTER_IDLE_MS || now - doc.firstUnsavedAt > SAVE_AT_LEAST_EVERY_MS))
        save("auto");

    checkBattery(now);
    checkNetworkClock();
    finishSync();

    if (now - lastKeyAt > SLEEP_AFTER_IDLE_MS && !showingPairing)
        goToSleep();
    checkWakeButton(now);

    bool idle = now - lastKeyAt > SYNC_AFTER_IDLE_MS;
    // A new day: once you pause, move on to the new daily note.
    if (idle && !syncRunning && tw::wallclock::isSet() && doc.remotePath.rfind(DAILY_NOTES, 0) == 0 &&
        todayName() != doc.name) {
        // The sync finishes in the background against yesterday's file.
        startSync("day ended");
        save("day ended");
        openToday();
    }
    continueSyncAll(); // Cmd+S on the list goes first
    // Sync: when the note opens (to pick up laptop edits), then after pauses.
    bool retryWait = doc.syncFailures && now - doc.lastSyncAttemptAt < SYNC_RETRY_MS;
    if (online() && !retryWait && !doc.remotePath.empty()) {
        if (!doc.checkedRemote || (idle && tw::hasLocalChanges(doc.text, doc.sync)))
            startSync(doc.checkedRemote ? "idle" : "open");
    }
    // Drafts: at startup, when the Open screen shows, and every 10 minutes.
    if (now - lastDraftsRefreshAt > DRAFTS_REFRESH_EVERY_MS && lastDraftsRefreshAt)
        draftsWanted = true;
    if (draftsWanted && online() && !syncRunning && (doc.checkedRemote || doc.remotePath.empty()))
        startDraftsRefresh();
    // Then catch up on other notes left unsynced, one at a time, while you pause.
    static uint32_t lastBacklogCheckAt = 0;
    if (idle && online() && !syncRunning && doc.checkedRemote && now - lastBacklogCheckAt > SYNC_RETRY_MS) {
        lastBacklogCheckAt = now;
        std::string local, remote;
        if (findBacklog(local, remote))
            startBacklogSync(local, remote);
    }

    // Keyboard state changes: the pairing screen, or the footer notice.
    uint32_t rev = keyboard.revision();
    if (rev != seenKeyboardRevision) {
        seenKeyboardRevision = rev;
        if (keyboard.state() == tw::Keyboard::State::Pairing) {
            drawPairing();
            display.display();
            panelOn = false;
            showingPairing = true;
            return;
        }
        if (showingPairing) {
            showingPairing = false;
            forceFull = true;
        } else {
            markChanged(now);
        }
    }
    if (showingPairing)
        return;

    if (screen == Screen::Open) {
        // Moving the selection uses typing's short passes; a changed footer
        // gets long ones so the old message erases cleanly. The faint trace
        // short passes leave goes at a full refresh after a minute idle.
        static std::string footerShown;
        static bool listPartialsSinceClean = false;
        if (listPartialsSinceClean && now - lastKeyAt > CLEAN_AFTER_IDLE_MS && now - lastUpdateAt > CLEAN_AFTER_IDLE_MS)
            forceFull = true;
        static std::string contentsShown;
        static size_t selectedShown = 0;
        if (forceFull || openDirty) {
            bool full = forceFull;
            bool footerChanged = openNotice != footerShown;
            const char *status = openNotice.empty() ? nullptr : openNotice.c_str();
            uint32_t t0 = millis();
            scrollOpenList();
            std::string contents = openContents(status);
            bool markerOnly = !full && contents == contentsShown;
            if (markerOnly)
                moveOpenMarker(selectedShown); // an arrow key: just the marker
            else
                drawOpenScreen(status);
            contentsShown = contents;
            selectedShown = selected;
            uint32_t t1 = millis();
            if (full)
                display.display();
            else
                display.quickPartialUpdate(true, footerChanged ? FOOTER_PASS_US : PASS_US);
            Serial.printf("list: %s, draw %lu ms, panel %lu ms, key waited %lu ms\n",
                          full ? "full refresh" : markerOnly ? "marker" : "partial",
                          (unsigned long)(t1 - t0), (unsigned long)(millis() - t1), (unsigned long)(millis() - lastKeyAt));
            footerShown = openNotice;
            listPartialsSinceClean = !full;
            forceFull = openDirty = false;
            panelOn = !full;
            lastUpdateAt = millis();
        }
        if (panelOn && now - lastUpdateAt > POWER_OFF_AFTER_MS) {
            display.einkOff();
            panelOn = false;
        }
        return;
    }

    if (reading) {
        if (readingDirty) {
            readingDirty = false;
            char left[64];
            snprintf(left, sizeof(left), "Page %u of %u · Esc to return", (unsigned)readingPage + 1,
                     (unsigned)readingPages.starts.size() + 1);
            uint32_t t0 = millis();
            page.renderReading(doc.text, readingPages, readingPage, left, nullptr);
            display.display();
            Serial.printf("reading: %s, line %u, %lu ms\n", left, (unsigned)readingPages.starts[readingPage],
                          (unsigned long)(millis() - t0));
            panelOn = false;
            lastUpdateAt = millis();
        }
        return;
    }

    if (forceFull) {
        forceFull = false;
        dirty = false;
        fullRefresh("requested");
        return;
    }

    if (dirty) {
        dirty = false;
        uint32_t t0 = millis();
        if (drawPage()) {
            display.display();
            partialsSinceClean = false;
            panelOn = false;
            Serial.printf("page turn: %lu ms\n", (unsigned long)(millis() - t0));
        } else {
            uint32_t t1 = millis();
            // Quick passes leave a faint trace when erasing, which is fine for a
            // typo but not for a notice that should be gone.
            display.quickPartialUpdate(true, footerChanged ? FOOTER_PASS_US : PASS_US);
            panelOn = true;
            partialsSinceClean = true;
            Serial.printf("partial: draw %lu ms, panel %lu ms, key waited %lu ms\n", (unsigned long)(t1 - t0),
                          (unsigned long)(millis() - t1), (unsigned long)(millis() - firstUnshownKeyAt));
        }
        lastUpdateAt = millis();
        return;
    }

    if (panelOn && now - lastUpdateAt > POWER_OFF_AFTER_MS) {
        display.einkOff();
        panelOn = false;
    }

    if (partialsSinceClean && now - lastKeyAt > CLEAN_AFTER_IDLE_MS && now - lastUpdateAt > CLEAN_AFTER_IDLE_MS)
        fullRefresh("idle");

    // Nothing to do this time round: rest a moment instead of spinning the CPU
    // flat out (it adds at most 5 ms before a key shows).
    delay(5);
}
