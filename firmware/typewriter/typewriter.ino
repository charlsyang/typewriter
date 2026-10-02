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
// Cmd+O lists the essays in /Site/blog to continue one; Cmd+D returns to today's
// note. Essays sync the same way.
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

// Sync runs in its own task, so typing never waits on the network. It works on
// a snapshot of the note; text typed meanwhile stays local and goes up next time.
struct SyncJob {
    std::string localPath, remotePath;
    std::string text;    // snapshot, replaced by the shared version if it changed
    size_t snapshotSize; // bytes in the snapshot
    tw::SyncState state;
    tw::SyncResult result;
    const char *why;
    uint32_t startedAt;
};
SyncJob syncJob;
volatile bool syncRunning = false;
volatile bool syncDone = false;
size_t shortestSinceSnapshot = 0; // Backspace below the snapshot while syncing
TaskHandle_t syncTask;

void syncTaskLoop(void *) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        syncJob.result = tw::syncFile(dropbox, syncJob.remotePath, syncJob.text, syncJob.state);
        syncDone = true;
    }
}

void startSync(const char *why) {
    if (syncRunning || doc.remotePath.empty() || !online())
        return;
    save(why);
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

bool endsWith(const std::string &s, const char *suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
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
        if (!(endsWith(name, ".md") || endsWith(name, ".mdx")) || name == "undated.md")
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

// Applies a finished sync to the note (or to the file it was for, if you've
// moved on to another note since).
void finishSync() {
    if (!syncDone)
        return;
    syncDone = false;
    syncRunning = false;
    SyncJob &j = syncJob;
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

// ---- Essays ----
//
// Cmd+O lists the essays in /Site/blog, newest first; Return opens one at its
// end to keep writing. The list is cached in flash for when Wi-Fi is off.

enum class Screen { Writing, Essays };
Screen screen = Screen::Writing;
std::vector<tw::Dropbox::Entry> essays; // [0] is "Today's note"
size_t essaySelected = 0;
size_t essayTop = 0; // first row on screen
std::string essaysProblem;
bool essaysDirty = false;
const char *ESSAY_LIST_CACHE = "/blog/.list";

// "design-engineering-revisited.mdx" -> "Design engineering revisited"
std::string essayTitle(const std::string &name) {
    std::string t = name.substr(0, name.rfind('.'));
    std::replace(t.begin(), t.end(), '-', ' ');
    if (!t.empty() && t[0] >= 'a' && t[0] <= 'z')
        t[0] -= 32;
    return t;
}

void saveEssayCache() {
    std::string data;
    for (size_t i = 1; i < essays.size(); i++)
        data += essays[i].name + "\t" + std::to_string(essays[i].modified) + "\n";
    tw::storage::write(ESSAY_LIST_CACHE, data);
}

void loadEssayCache() {
    std::string data;
    if (!tw::storage::read(ESSAY_LIST_CACHE, data))
        return;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t tab = data.find('\t', pos), nl = data.find('\n', pos);
        if (tab == std::string::npos || nl == std::string::npos || tab > nl)
            break;
        tw::Dropbox::Entry e;
        e.name = data.substr(pos, tab - pos);
        e.modified = strtoul(data.c_str() + tab + 1, nullptr, 10);
        essays.push_back(e);
        pos = nl + 1;
    }
}

// The sync task shares the Dropbox client, so let it finish first.
void waitForSync() {
    uint32_t start = millis();
    while (syncRunning && !syncDone && millis() - start < 30000)
        delay(20);
    finishSync();
}

void drawEssays();

void openEssayList() {
    save("opening essays");
    screen = Screen::Essays;
    essays.clear();
    tw::Dropbox::Entry today;
    today.name = "";
    essays.push_back(today);
    essaysProblem.clear();
    essaySelected = essayTop = 0;

    std::vector<tw::Dropbox::Entry> found;
    bool listed = false;
    if (online()) {
        essaysProblem = "Loading essays…";
        drawEssays();
        display.display();
        waitForSync();
        listed = dropbox.list(ESSAYS, found) == tw::Dropbox::Result::Ok;
        if (!listed)
            Serial.printf("essays: list failed: %s\n", dropbox.lastError().c_str());
    }
    if (listed) {
        for (const tw::Dropbox::Entry &e : found) {
            if (!e.folder && (endsWith(e.name, ".md") || endsWith(e.name, ".mdx")))
                essays.push_back(e);
        }
        std::sort(essays.begin() + 1, essays.end(),
                  [](const tw::Dropbox::Entry &a, const tw::Dropbox::Entry &b) { return a.modified > b.modified; });
        saveEssayCache();
        essaysProblem.clear();
    } else {
        loadEssayCache();
        essaysProblem = essays.size() > 1 ? "Offline: showing the list from last time." : "Connect to Wi-Fi to load your essays.";
    }
    Serial.printf("essays: %u (%s)\n", (unsigned)essays.size() - 1, listed ? "from Dropbox" : "cached");
    // Start on the essay you have open, if any.
    for (size_t i = 1; i < essays.size(); i++) {
        if ("/blog/" + essays[i].name == doc.path)
            essaySelected = i;
    }
    essaysDirty = true;
    forceFull = true;
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

void closeEssayList() {
    screen = Screen::Writing;
    forceFull = true;
}

void handleEssayKey(const tw::KeyEvent &e) {
    size_t count = essays.size();
    switch (e.type) {
    case tw::KeyEvent::Up:
        if (essaySelected > 0)
            essaySelected--;
        break;
    case tw::KeyEvent::Down:
        if (essaySelected + 1 < count)
            essaySelected++;
        break;
    case tw::KeyEvent::Enter:
        if (essaySelected == 0) {
            rememberOpenEssay("");
            if (doc.path.rfind("/daily/", 0) != 0) {
                save("switching");
                openToday();
            }
        } else {
            openEssay(essays[essaySelected].name);
        }
        closeEssayList();
        return;
    case tw::KeyEvent::Escape:
        closeEssayList();
        return;
    case tw::KeyEvent::Shortcut:
        if (e.key == 'o')
            closeEssayList();
        return;
    default:
        return;
    }
    essaysDirty = true;
}

void handleKey(const tw::KeyEvent &e, uint32_t now) {
    lastKeyAt = now;
    if (screen == Screen::Essays) {
        handleEssayKey(e);
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
            openEssayList();
        } else if (e.key == 'd') {
            rememberOpenEssay("");
            if (doc.path.rfind("/daily/", 0) != 0) {
                save("switching");
                openToday();
            }
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

std::string monthDay(uint32_t epoch) {
    if (!epoch)
        return "";
    time_t t = epoch;
    tm local;
    localtime_r(&t, &local);
    char out[16];
    strftime(out, sizeof(out), "%b %e", &local);
    return out;
}

void drawEssays() {
    const int16_t marginX = 64, top = 64, rowH = Duo40.yAdvance, listTop = top + rowH;
    const int16_t bottom = 720 - 72;
    const size_t rows = (bottom - listTop) / rowH;
    if (essaySelected < essayTop)
        essayTop = essaySelected;
    if (essaySelected >= essayTop + rows)
        essayTop = essaySelected - rows + 1;

    display.clearDisplay();
    const char *heading = "Open";
    tw::drawText(display, Duo40, marginX, top + Duo40Ascent, heading, 0, strlen(heading));
    for (size_t i = essayTop; i < essays.size() && i < essayTop + rows; i++) {
        int32_t y = listTop + (i - essayTop) * rowH + Duo40Ascent;
        std::string label = i == 0 ? "Today’s note" : essayTitle(essays[i].name);
        std::string row = (i == essaySelected ? "› " : "  ") + label;
        tw::drawText(display, Duo40, marginX, y, row.c_str(), 0, row.size());
        std::string date = i == 0 ? todayName().substr(0, 10) : monthDay(essays[i].modified);
        int32_t w = tw::measure(Duo24, date.c_str(), 0, date.size());
        tw::drawText(display, Duo24, display.width() - marginX - w, y, date.c_str(), 0, date.size());
    }
    std::string footer = essaysProblem.empty() ? "↑↓ choose · Return open · Esc back" : essaysProblem;
    tw::drawText(display, Duo24, marginX, 720 - 44 + Duo24Ascent, footer.c_str(), 0, footer.size());
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

    if (screen == Screen::Essays) {
        if (forceFull || essaysDirty) {
            bool full = forceFull;
            drawEssays();
            if (full)
                display.display();
            else
                display.quickPartialUpdate(true, FOOTER_PASS_US);
            forceFull = essaysDirty = false;
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
