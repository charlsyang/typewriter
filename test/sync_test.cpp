// Host tests for lib/Typewriter/src/Sync.cpp. Run: test/run.sh
#include "Sync.h"

#include <cstdio>
#include <cstdlib>

using namespace tw;

static int failures = 0;
#define EXPECT(cond)                                                                                                   \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("  FAIL line %d: %s\n", __LINE__, #cond);                                                          \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static const std::string PATH = "/OS/Daily Notes/2026-10-01.md";

// Typing on the Inkplate: append, keeping keptLength as the firmware does.
static void type(std::string &text, SyncState &s, const std::string &more) {
    text += more;
}
static void backspace(std::string &text, SyncState &s, size_t n) {
    text.resize(text.size() - n);
    if (text.size() < s.keptLength)
        s.keptLength = text.size();
}

int main() {
    {
        puts("first sync uploads a new file");
        Dropbox db;
        SyncState s;
        std::string text = "Morning pages.";
        SyncResult r = syncFile(db, PATH, text, s);
        EXPECT(r.outcome == SyncResult::Uploaded);
        EXPECT(db.files[PATH] == "Morning pages.");
        EXPECT(s.rev == db.revs[PATH] && s.syncedLength == text.size());
        EXPECT(syncFile(db, PATH, text, s).outcome == SyncResult::UpToDate);
    }
    {
        puts("local typing uploads over the same revision");
        Dropbox db;
        SyncState s;
        std::string text = "One.";
        syncFile(db, PATH, text, s);
        type(text, s, " Two.");
        SyncResult r = syncFile(db, PATH, text, s);
        EXPECT(r.outcome == SyncResult::Uploaded && db.files[PATH] == "One. Two.");
    }
    {
        puts("laptop edit with no local changes downloads");
        Dropbox db;
        SyncState s;
        std::string text = "Draft.";
        syncFile(db, PATH, text, s);
        db.put(PATH, "Draft, edited on the laptop.");
        SyncResult r = syncFile(db, PATH, text, s);
        EXPECT(r.outcome == SyncResult::Downloaded && r.textChanged);
        EXPECT(text == "Draft, edited on the laptop.");
    }
    {
        puts("laptop edited earlier text, Inkplate kept typing: continue in place");
        Dropbox db;
        SyncState s;
        std::string text = "Firts paragraph.\n\nSecond paragraph, still going";
        syncFile(db, PATH, text, s);
        db.put(PATH, "First paragraph.\n\nSecond paragraph, still going");
        type(text, s, " and going.");
        SyncResult r = syncFile(db, PATH, text, s);
        EXPECT(r.outcome == SyncResult::Merged);
        EXPECT(text == "First paragraph.\n\nSecond paragraph, still going and going.");
        EXPECT(db.files[PATH] == text);
        EXPECT(syncFile(db, PATH, text, s).outcome == SyncResult::UpToDate);
    }
    {
        puts("both added to the end: Inkplate text becomes a new paragraph");
        Dropbox db;
        SyncState s;
        std::string text = "Shared start.";
        syncFile(db, PATH, text, s);
        db.put(PATH, "Shared start. Laptop sentence.\n");
        type(text, s, " Inkplate sentence.");
        SyncResult r = syncFile(db, PATH, text, s);
        EXPECT(r.outcome == SyncResult::Merged);
        EXPECT(text == "Shared start. Laptop sentence.\n\nInkplate sentence.");
    }
    {
        puts("never synced on the Inkplate but the laptop made today's note");
        Dropbox db;
        SyncState s;
        db.put(PATH, "Written on the laptop.");
        std::string text = "Written on the Inkplate.";
        SyncResult r = syncFile(db, PATH, text, s);
        EXPECT(r.outcome == SyncResult::Merged);
        EXPECT(text == "Written on the laptop.\n\nWritten on the Inkplate.");
    }
    {
        puts("Backspace into synced text plus a laptop edit: conflicted copy");
        Dropbox db;
        SyncState s;
        std::string text = "Keep this. Delete this.";
        syncFile(db, PATH, text, s);
        backspace(text, s, 12);
        type(text, s, "Replacement.");
        db.put(PATH, "Keep this. Delete this. Laptop addition.");
        SyncResult r = syncFile(db, PATH, text, s);
        EXPECT(r.outcome == SyncResult::Conflict);
        std::string copy = "/OS/Daily Notes/2026-10-01 (Inkplate conflicted copy).md";
        EXPECT(db.files[copy] == "Keep this. Replacement.");
        EXPECT(text == "Keep this. Delete this. Laptop addition.");
        EXPECT(db.files[PATH] == text);
    }
    {
        puts("Backspace into synced text, laptop untouched: plain upload");
        Dropbox db;
        SyncState s;
        std::string text = "Typo here";
        syncFile(db, PATH, text, s);
        backspace(text, s, 4);
        type(text, s, "there");
        SyncResult r = syncFile(db, PATH, text, s);
        EXPECT(r.outcome == SyncResult::Uploaded && db.files[PATH] == "Typo there");
    }
    {
        puts("laptop edit lands between the check and the upload: merges");
        Dropbox db;
        SyncState s;
        std::string text = "Start";
        syncFile(db, PATH, text, s);
        type(text, s, " and more.");
        db.editBeforeNextUpload = "Start, edited";
        SyncResult r = syncFile(db, PATH, text, s);
        EXPECT(r.outcome == SyncResult::Merged);
        EXPECT(text == "Start, edited\n\nand more.");
    }
    {
        puts("sync state survives a restart");
        Dropbox db;
        SyncState s;
        std::string text = "Saved.";
        syncFile(db, PATH, text, s);
        saveSyncState("/daily/2026-10-01.md", s);
        SyncState loaded;
        EXPECT(loadSyncState("/daily/2026-10-01.md", loaded));
        EXPECT(loaded.rev == s.rev && loaded.syncedLength == 6 && loaded.keptLength == 6);
    }
    {
        puts("deleted in Dropbox with nothing new typed: deleted here too");
        Dropbox db;
        SyncState s;
        std::string text = "Old note.";
        syncFile(db, PATH, text, s);
        db.files.erase(PATH);
        SyncResult r = syncFile(db, PATH, text, s);
        EXPECT(r.outcome == SyncResult::Deleted && text.empty() && s.rev.empty());
        EXPECT(!db.files.count(PATH));
    }
    {
        puts("deleted in Dropbox but typed since: uploaded again");
        Dropbox db;
        SyncState s;
        std::string text = "Old note.";
        syncFile(db, PATH, text, s);
        db.files.erase(PATH);
        type(text, s, " New words.");
        SyncResult r = syncFile(db, PATH, text, s);
        EXPECT(r.outcome == SyncResult::Uploaded && db.files[PATH] == "Old note. New words.");
    }
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
