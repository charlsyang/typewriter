// Typewriter: write into today's daily note on a Bluetooth keyboard.
//
// The note lives in flash at /daily/YYYY-MM-DD.md (the Obsidian daily note name)
// and autosaves a couple of seconds after you stop typing. Over Wi-Fi it syncs
// with /OS/Daily Notes/YYYY-MM-DD.md in Dropbox: when the note opens, after a
// minute without typing, and on Cmd+S. The screen shows only your text; a footer
// line appears when something needs attention, or on Cmd+I.
//
// Wi-Fi and Dropbox credentials are in secrets.h (see secrets.example.h).
//
// Cmd+O lists today's note and your drafts (essays in /Site/blog without
// isPublished: true), all kept on the Inkplate; Cmd+D returns to today's note.
// Drafts sync the same way. Daily notes that are blank never go to Dropbox.
//
// Keys: Backspace (Option+Backspace deletes a word), Return, Cmd+S save and sync,
// Cmd+I show status (file, words, saved state, power), Cmd+L full refresh, Cmd+Shift+K forget the keyboard.
// Ctrl works in place of Cmd.
//
// Over USB (tools/settime.py, tools/type.py): Ctrl+T sets the clock, Ctrl+K
// forgets the keyboard, other bytes type text. Serial logs events and timings.

#include "secrets.h"

#include <Clock.h>
#include <Dropbox.h>
#include <FastInkplate.h>
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
#include <fonts/Duo40.h>

tw::FastInkplate display(INKPLATE_1BIT);
tw::Page page(display, Duo40, Duo40Ascent, Duo24, Duo24Ascent);
tw::Keyboard keyboard;
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
    tw::SyncResult result;
    // Drafts job
    std::string openPath; // the note on screen: left to its own sync
    std::vector<DraftInfo> all;
    bool listed;
    int downloaded, removed;
    const char *why;
    uint32_t startedAt;
};

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

// Takes copies: callers may pass the current document's own fields.
void openDocument(std::string path, std::string remotePath, std::string name) {
    doc = Document();
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

// Background work runs in its own task, so typing never waits on the network.
// A file sync works on a snapshot of the note; text typed meanwhile stays local
// and goes up next time.
SyncJob syncJob;
volatile bool syncRunning = false;
volatile bool syncDone = false;
size_t shortestSinceSnapshot = 0; // Backspace below the snapshot while syncing
TaskHandle_t syncTask;

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

void syncTaskLoop(void *) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (syncJob.kind == JobKind::Drafts)
            refreshDrafts(syncJob);
        else
            syncJob.result = tw::syncFile(dropbox, syncJob.remotePath, syncJob.text, syncJob.state,
                                          syncJob.blankMeansAbsent);
        syncDone = true;
    }
}

bool isDailyNote(const std::string &remotePath) {
    return remotePath.rfind(DAILY_NOTES, 0) == 0;
}

void startSync(const char *why) {
    if (syncRunning || doc.remotePath.empty() || !online())
        return;
    save(why);
    syncJob.kind = JobKind::File;
    syncJob.blankMeansAbsent = isDailyNote(doc.remotePath);
    syncJob.localPath = doc.path;
    syncJob.remotePath = doc.remotePath;
    syncJob.text = doc.text;
    syncJob.snapshotSize = doc.text.size();
    syncJob.state = doc.sync;
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
    syncJob.kind = JobKind::File;
    syncJob.blankMeansAbsent = isDailyNote(remotePath);
    syncJob.localPath = localPath;
    syncJob.remotePath = remotePath;
    if (!tw::storage::read(localPath.c_str(), syncJob.text))
        return;
    syncJob.snapshotSize = syncJob.text.size();
    tw::loadSyncState(localPath, syncJob.state);
    syncJob.why = "backlog";
    syncJob.startedAt = millis();
    syncDone = false;
    syncRunning = true;
    xTaskNotifyGive(syncTask);
}

// Looks for another note or essay in flash with changes Dropbox doesn't have yet.
bool findBacklog(std::string &localPath, std::string &remotePath) {
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
        if (path == doc.path)
            continue;
        // Only notes that have synced before: older notes written before sync
        // existed are left for you to decide about.
        tw::SyncState state;
        if (!tw::loadSyncState(path, state) || state.rev.empty())
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
    static const char *OUTCOMES[] = {"up to date", "uploaded", "downloaded", "merged", "conflict", "deleted",
                                     "FAILED"};
    Serial.printf("sync (%s): %s in %lu ms, rev %s %s\n", j.why, OUTCOMES[r.outcome],
                  (unsigned long)(millis() - j.startedAt), j.state.rev.c_str(), r.message.c_str());

    if (r.outcome == tw::SyncResult::Deleted) {
        // Deleted in Dropbox: remove the Inkplate's copy too.
        tw::storage::remove(j.localPath.c_str());
        tw::storage::remove((j.localPath + ".sync").c_str());
        if (j.localPath == doc.path && shortestSinceSnapshot >= j.snapshotSize && doc.text.size() == j.snapshotSize) {
            doc.text.clear();
            doc.sync = tw::SyncState();
            doc.unsaved = false;
            doc.checkedRemote = true;
            page.reset();
            forceFull = true;
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
        notice = "Changed on both sides. Your Inkplate version is in Dropbox as " +
                 r.message.substr(r.message.rfind('/') + 1);
    dirty = true;
}

void deleteLastChar() {
    while (!doc.text.empty()) {
        uint8_t last = doc.text.back();
        doc.text.pop_back();
        if ((last & 0xC0) != 0x80)
            break;
    }
}

void deleteLastWord() {
    std::string &t = doc.text;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n'))
        t.pop_back();
    while (!t.empty() && t.back() != ' ' && t.back() != '\n')
        deleteLastChar();
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
    if (!j.listed) {
        Serial.printf("drafts: couldn't list %s: %s\n", ESSAYS, dropbox.lastError().c_str());
        return;
    }
    std::vector<DraftInfo> fresh = draftsFrom(j.all);
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

void openTodayFromList() {
    rememberOpenEssay("");
    if (doc.path.rfind("/daily/", 0) != 0) {
        save("switching");
        openToday();
    }
}

void showOpenScreen() {
    save("opening the list");
    screen = Screen::Open;
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

void handleOpenKey(const tw::KeyEvent &e) {
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
        if (e.key == 'o')
            closeOpenScreen();
        return;
    default:
        return;
    }
    openDirty = true;
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
    switch (e.type) {
    case tw::KeyEvent::Text:
        doc.text += e.text;
        break;
    case tw::KeyEvent::Backspace:
        deleteLastChar();
        break;
    case tw::KeyEvent::BackspaceWord:
        deleteLastWord();
        break;
    case tw::KeyEvent::Enter:
        doc.text += '\n';
        break;
    case tw::KeyEvent::Tab:
        doc.text += "    ";
        break;
    case tw::KeyEvent::Escape:
        return;
    case tw::KeyEvent::Shortcut:
        if (e.key == 's') {
            save("Cmd+S");
            if (doc.remotePath.empty())
                notice = "Saved on the Inkplate. Undated notes don't sync.";
            else if (!online())
                notice = "Saved on the Inkplate. It will sync when Wi-Fi is back.";
            else
                startSync("Cmd+S");
            markChanged(now);
        } else if (e.key == 'i') {
            showStatus = true;
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
        if (b == 0x0F || b == 0x04) { // Ctrl+O, Ctrl+D: same as Cmd+O, Cmd+D
            nav.type = tw::KeyEvent::Shortcut;
            nav.key = b == 0x0F ? 'o' : 'd';
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
        if (b == 0x12) { // Ctrl+R over USB, then "ls" or "rm <path>": manage files in flash
            String line = Serial.readStringUntil('\n');
            line.trim();
            if (line == "ls") {
                for (const char *dir : {"/daily", "/blog"}) {
                    for (const std::string &name : tw::storage::list(dir)) {
                        std::string path = std::string(dir) + "/" + name;
                        Serial.printf("file: %s (%ld bytes)\n", path.c_str(), tw::storage::size(path.c_str()));
                    }
                }
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

void checkBattery(uint32_t now) {
    if (lastBatteryCheckAt && now - lastBatteryCheckAt < BATTERY_CHECK_EVERY_MS)
        return;
    lastBatteryCheckAt = now;
    double v = display.readBattery();
    // A Li-ion cell reads about 3.4 V empty to 4.15 V full. On USB with no
    // battery the charger's own 4.2 V shows through, so treat that as no battery;
    // a real cell only reaches it while fully charged on USB.
    int previous = batteryPercent;
    if (v < 3.0 || v >= 4.18)
        batteryPercent = -1;
    else
        batteryPercent = constrain((int)((v - 3.4) / (4.15 - 3.4) * 100), 0, 100);
    Serial.printf("battery: %.2f V (%d%%)\n", v, batteryPercent);
    bool wasLow = previous >= 0 && previous <= LOW_BATTERY_PERCENT;
    bool isLow = batteryPercent >= 0 && batteryPercent <= LOW_BATTERY_PERCENT;
    if (wasLow != isLow)
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
        snprintf(out, size, "Not saved: couldn’t write to flash. Cmd+S to retry.");
    } else if (!notice.empty()) {
        snprintf(out, size, "%s", notice.c_str());
    } else if (!doc.checkedRemote && doc.text.empty() && online() && !doc.remotePath.empty()) {
        snprintf(out, size, "Downloading %s…", doc.name.c_str());
    } else if (doc.syncFailures >= SYNC_FAILURES_BEFORE_NOTICE) {
        snprintf(out, size, "Can’t sync with Dropbox (%s). Saved on the Inkplate.", doc.syncError.c_str());
    } else if (kb == tw::Keyboard::State::Searching || kb == tw::Keyboard::State::Connecting) {
        snprintf(out, size, "Keyboard not connected. Press a key on it to wake it.");
    } else if (!tw::wallclock::isSet()) {
        snprintf(out, size, "Clock not set, so this is undated.md. Run tools/settime.py over USB.");
    } else if (batteryPercent >= 0 && batteryPercent <= LOW_BATTERY_PERCENT) {
        snprintf(out, size, "Battery low: %d%%", batteryPercent);
    } else if (showStatus) {
        char battery[24] = "USB power";
        if (batteryPercent >= 0)
            snprintf(battery, sizeof(battery), "Battery %d%%", batteryPercent);
        int words = wordCount();
        snprintf(out, size, "%s · %d %s", doc.name.c_str(), words, words == 1 ? "word" : "words");
        const char *state = doc.unsaved                              ? "Not saved yet"
                            : doc.remotePath.empty()                 ? "Saved"
                            : syncRunning                            ? "Syncing…"
                            : !tw::hasLocalChanges(doc.text, doc.sync) && doc.checkedRemote ? "Synced"
                            : online()                               ? "Saved, not synced yet"
                                                                     : "Saved, offline";
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

// MM/DD/YYYY, e.g. 09/26/2026.
std::string numericDate(uint32_t epoch) {
    if (!epoch)
        return "";
    time_t t = epoch;
    tm local;
    localtime_r(&t, &local);
    char out[16];
    strftime(out, sizeof(out), "%m/%d/%Y", &local);
    return out;
}

void drawOpenRow(int32_t y, bool isSelected, const std::string &label, const std::string &date) {
    const int16_t marginX = 64;
    std::string row = (isSelected ? "› " : "  ") + label;
    tw::drawText(display, Duo40, marginX, y + Duo40Ascent, row.c_str(), 0, row.size());
    int32_t w = tw::measure(Duo24, date.c_str(), 0, date.size());
    // Dates sit on the same baseline as the row's text.
    tw::drawText(display, Duo24, display.width() - marginX - w, y + Duo40Ascent, date.c_str(), 0, date.size());
}

// Today's note, then a "Drafts" section. Drafts scroll; today's note stays put.
void drawOpenScreen() {
    const int16_t marginX = 64, top = 64, rowH = Duo40.yAdvance;
    const int16_t labelY = top + rowH + 24;         // section label, after a gap
    const int16_t listTop = labelY + 40;             // first draft row
    const int16_t bottom = 720 - 72;
    const size_t rows = (bottom - listTop) / rowH;
    if (selected > 0) {
        size_t i = selected - 1;
        if (i < firstVisible)
            firstVisible = i;
        if (i >= firstVisible + rows)
            firstVisible = i - rows + 1;
    }

    display.clearDisplay();
    std::string today = todayName().substr(0, 10); // YYYY-MM-DD
    std::string todayDate = today.size() == 10 ? today.substr(5, 2) + "/" + today.substr(8, 2) + "/" + today.substr(0, 4) : "";
    drawOpenRow(top, selected == 0, "Today’s note", todayDate);

    const char *label = "Drafts";
    tw::drawText(display, Duo24, marginX, labelY + Duo24Ascent, label, 0, strlen(label));
    if (drafts.empty()) {
        const char *none = draftsChecking ? "Checking Dropbox…" : online() ? "No drafts in /Site/blog." : "No drafts yet. Connect to Wi-Fi to check.";
        tw::drawText(display, Duo24, marginX, listTop + Duo24Ascent, none, 0, strlen(none));
    }
    for (size_t i = firstVisible; i < drafts.size() && i < firstVisible + rows; i++)
        drawOpenRow(listTop + (i - firstVisible) * rowH, selected == i + 1, essayTitle(drafts[i].name),
                    numericDate(drafts[i].modified));

    const char *hint = "↑↓ choose · Return open · Esc back";
    tw::drawText(display, Duo24, marginX, 720 - 44 + Duo24Ascent, hint, 0, strlen(hint));
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

// ---- Main ----

void setup() {
    Serial.begin(115200);
    display.begin();
    display.setFullUpdateThreshold(0); // we decide when to do full refreshes
    if (!tw::storage::begin())
        Serial.println("storage: mount FAILED");
    Serial.printf("storage: %u of %u KB used\n", (unsigned)(tw::storage::usedBytes() / 1024),
                  (unsigned)(tw::storage::totalBytes() / 1024));
    tw::wallclock::begin(display);
    tw::storage::remove("/blog/.list"); // replaced by DRAFTS_CACHE
    drafts = draftsFrom(loadDraftsCache());
    // Reopen the essay you were writing, if any; otherwise today's note.
    Preferences prefs;
    prefs.begin("typewriter", true);
    String essay = prefs.getString("essay", "");
    prefs.end();
    if (essay.length() && tw::storage::exists(("/blog/" + std::string(essay.c_str())).c_str()))
        openEssay(essay.c_str());
    else
        openToday();
    keyboard.begin("Typewriter");
    xTaskCreatePinnedToCore(syncTaskLoop, "sync", 12288, nullptr, 1, &syncTask, 0);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    configTzTime(tw::wallclock::zone().c_str(), "pool.ntp.org", "time.apple.com");
}

// Once Wi-Fi has the time, keep the RTC right (and leave undated.md behind).
void checkNetworkClock() {
    if (clockFromNetwork || sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED)
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

    bool idle = now - lastKeyAt > SYNC_AFTER_IDLE_MS;
    // A new day: once you pause, move on to the new daily note.
    if (idle && !syncRunning && tw::wallclock::isSet() && doc.remotePath.rfind(DAILY_NOTES, 0) == 0 &&
        todayName() != doc.name) {
        // The sync finishes in the background against yesterday's file.
        startSync("day ended");
        save("day ended");
        openToday();
    }
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
        if (forceFull || openDirty) {
            bool full = forceFull;
            drawOpenScreen();
            if (full)
                display.display();
            else
                display.quickPartialUpdate(true, FOOTER_PASS_US);
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
}
