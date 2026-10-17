// Keeps a local file and its Dropbox copy together without losing edits on either
// side. The typewriter only adds text at the end (Backspace aside), so when both
// sides changed, the text typed on the Inkplate since the last sync is added to
// the end of the laptop's version. Only when Backspace removed already-synced
// text and the laptop also edited the file is the Inkplate's version saved as a
// conflicted copy instead.
#pragma once

#include "Dropbox.h"

#include <string>

namespace tw {

// What the last successful sync left behind, saved next to the local file.
struct SyncState {
    std::string rev;          // Dropbox revision both sides last agreed on ("" = never synced)
    size_t syncedLength = 0;  // bytes of text at that point
    size_t keptLength = 0;    // fewest bytes since then: Backspace below syncedLength means a deletion
    std::string id;           // Dropbox file ID ("" until learned), to follow renames
};

bool loadSyncState(const std::string &localPath, SyncState &state);
bool saveSyncState(const std::string &localPath, const SyncState &state);

// True if the text changed since the last sync.
bool hasLocalChanges(const std::string &text, const SyncState &state);

// True if the file at localPath has text Dropbox doesn't have yet: changed since
// it last synced, or never synced (written offline) and not blank. A file with
// no sync state (written before sync existed) never counts.
bool waitingToSync(const std::string &localPath);

// True if the file at localPath can be removed from flash without losing
// anything: it synced and Dropbox has all of it, or it's blank and never synced.
// A file with no sync state (written before sync existed) is always kept. The
// caller keeps today's note and the open one.
bool safelyInDropbox(const std::string &localPath);

struct SyncResult {
    enum Outcome {
        UpToDate,   // nothing to do
        Uploaded,   // Dropbox now has the Inkplate's text
        Downloaded, // the Inkplate now has Dropbox's text (no local changes)
        Merged,     // both changed; the Inkplate's new text was added to the end
        Conflict,   // both changed; the Inkplate's version was saved as a copy
        Deleted,    // deleted in Dropbox and unchanged here: text cleared too
        Moved,      // renamed in Dropbox within the same folder: message is the new path; nothing synced yet
        Failed,
    } outcome;
    std::string message; // for Failed, Conflict and Moved
    bool textChanged = false;
};

// True if text holds nothing but spaces, tabs and line breaks.
bool isBlank(const std::string &text);

// Syncs text with remotePath and updates state. On Downloaded, Merged and
// Conflict, text is replaced with the new shared version; on Deleted it's emptied.
// With blankMeansAbsent (daily notes), a blank note is never uploaded, and
// clearing a synced one deletes it from Dropbox. A file missing from remotePath
// is looked up by its ID: renamed within the same folder, it's Moved; anywhere
// else, or gone, it's treated as deleted.
SyncResult syncFile(Dropbox &dropbox, const std::string &remotePath, std::string &text, SyncState &state,
                    bool blankMeansAbsent = false);

} // namespace tw
