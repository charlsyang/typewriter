#include "Sync.h"

#include "Storage.h"

namespace tw {

namespace {

std::string statePath(const std::string &localPath) {
    return localPath + ".sync";
}

// "<folder>/<name> (Copy).<ext>"
std::string conflictPath(const std::string &path) {
    size_t slash = path.rfind('/');
    size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        dot = path.size();
    return path.substr(0, dot) + " (Copy)" + path.substr(dot);
}

// "/Site/blog/a.mdx" and "/site/Blog/b.mdx" are in the same folder (Dropbox
// ignores case).
bool sameFolder(const std::string &a, const std::string &b) {
    std::string fa = a.substr(0, a.rfind('/') + 1), fb = b.substr(0, b.rfind('/') + 1);
    if (fa.size() != fb.size())
        return false;
    for (size_t i = 0; i < fa.size(); i++)
        if (tolower((unsigned char)fa[i]) != tolower((unsigned char)fb[i]))
            return false;
    return true;
}

bool endsWith(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Adds the text typed on the Inkplate since the last sync to the laptop's
// version. If the laptop's version still ends the way the synced text ended
// (it edited earlier parts), the new text continues straight on, mid-sentence
// if need be. Otherwise both sides added to the end, so it starts a new paragraph.
std::string merge(const std::string &remote, const std::string &local, size_t syncedLength) {
    std::string added = local.substr(syncedLength);
    std::string syncedTail = local.substr(syncedLength > 40 ? syncedLength - 40 : 0, syncedLength > 40 ? 40 : syncedLength);
    if (remote.empty() || (syncedLength > 0 && endsWith(remote, syncedTail)))
        return remote + added;
    std::string merged = remote;
    while (!merged.empty() && merged.back() == '\n')
        merged.pop_back();
    size_t start = 0;
    while (start < added.size() && (added[start] == '\n' || added[start] == ' '))
        start++;
    return merged + "\n\n" + added.substr(start);
}

} // namespace

bool loadSyncState(const std::string &localPath, SyncState &state) {
    state = SyncState();
    std::string data;
    if (!storage::read(statePath(localPath).c_str(), data))
        return false;
    // Lines: rev, syncedLength, keptLength, then the file ID (missing in older files).
    size_t a = data.find('\n');
    size_t b = a == std::string::npos ? a : data.find('\n', a + 1);
    if (b == std::string::npos)
        return false;
    state.rev = data.substr(0, a);
    state.syncedLength = strtoul(data.c_str() + a + 1, nullptr, 10);
    state.keptLength = strtoul(data.c_str() + b + 1, nullptr, 10);
    size_t c = data.find('\n', b + 1);
    if (c != std::string::npos) {
        size_t d = data.find('\n', c + 1);
        state.id = data.substr(c + 1, d == std::string::npos ? std::string::npos : d - c - 1);
    }
    return true;
}

bool saveSyncState(const std::string &localPath, const SyncState &state) {
    std::string data = state.rev + "\n" + std::to_string(state.syncedLength) + "\n" +
                       std::to_string(state.keptLength) + "\n" + state.id + "\n";
    return storage::write(statePath(localPath).c_str(), data);
}

bool isBlank(const std::string &text) {
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

bool hasLocalChanges(const std::string &text, const SyncState &state) {
    return text.size() != state.syncedLength || state.keptLength < state.syncedLength;
}

bool waitingToSync(const std::string &localPath) {
    SyncState state;
    if (!loadSyncState(localPath, state))
        return false;
    if (state.rev.empty()) {
        std::string text;
        return storage::read(localPath.c_str(), text) && !isBlank(text);
    }
    // The size is enough here, without reading the whole file.
    long size = storage::size(localPath.c_str());
    return (size >= 0 && (size_t)size != state.syncedLength) || state.keptLength < state.syncedLength;
}

bool safelyInDropbox(const std::string &localPath) {
    SyncState state;
    // Never synced, waitingToSync is false only for a blank note: nothing to lose.
    return loadSyncState(localPath, state) && !waitingToSync(localPath);
}

SyncResult syncFile(Dropbox &dropbox, const std::string &remotePath, std::string &text, SyncState &state,
                    bool blankMeansAbsent) {
    SyncResult result = {SyncResult::UpToDate, "", false};
    auto fail = [&](const char *what) {
        result.outcome = SyncResult::Failed;
        result.message = std::string(what) + ": " + dropbox.lastError();
        return result;
    };
    auto agreed = [&](const std::string &rev, const std::string &id) {
        state.rev = rev;
        if (!id.empty())
            state.id = id;
        state.syncedLength = state.keptLength = text.size();
    };

    bool localChanges = hasLocalChanges(text, state);

    if (blankMeansAbsent && isBlank(text)) {
        if (state.rev.empty()) {
            // Nothing written here yet: never upload a blank note, but pick up
            // one the laptop may have started.
            localChanges = false;
        } else if (localChanges) {
            // You cleared a synced note: it shouldn't exist, so delete it from
            // Dropbox (only if the laptop hasn't changed it since).
            Dropbox::Result d = dropbox.remove(remotePath, state.rev);
            if (d == Dropbox::Result::Ok || d == Dropbox::Result::NotFound) {
                text.clear();
                state = SyncState();
                result.outcome = SyncResult::Deleted;
                result.textChanged = true;
                return result;
            }
            if (d == Dropbox::Result::Error)
                return fail("deleting the empty note");
            // The laptop changed it: keep the laptop's version (below).
            localChanges = false;
        }
    }

    // The common case: you typed, the laptop didn't touch the file. One request:
    // Dropbox only accepts the upload if the file is still at our revision.
    if (localChanges && !state.rev.empty()) {
        std::string rev, id;
        Dropbox::Result u = dropbox.upload(remotePath, text, state.rev, rev, &id);
        if (u == Dropbox::Result::Ok && !state.id.empty() && !id.empty() && id != state.id) {
            // The file was renamed away and Dropbox made a new one at the old
            // path: take that back out, and look for the renamed file below.
            if (dropbox.remove(remotePath, rev) == Dropbox::Result::Error)
                return fail("undoing an upload to a renamed file's old name");
        } else if (u == Dropbox::Result::Ok) {
            agreed(rev, id);
            result.outcome = SyncResult::Uploaded;
            return result;
        } else if (u == Dropbox::Result::Error) {
            return fail("uploading");
        }
        // Conflict or gone: look at what Dropbox has and merge below.
    }

    Dropbox::Entry remote;
    Dropbox::Result r = dropbox.metadata(remotePath, remote);
    if (r == Dropbox::Result::Error)
        return fail("checking Dropbox");

    if (r == Dropbox::Result::NotFound) {
        if (text.empty() || (blankMeansAbsent && isBlank(text)))
            return result;
        // Renamed? The file ID survives renames and moves. Followed within the
        // same folder; moved anywhere else, it's as good as deleted here.
        if (!state.id.empty()) {
            Dropbox::Entry moved;
            Dropbox::Result m = dropbox.metadata(state.id, moved);
            if (m == Dropbox::Result::Error)
                return fail("looking for a renamed file");
            if (m == Dropbox::Result::Ok && !moved.folder && sameFolder(moved.path, remotePath)) {
                result.outcome = SyncResult::Moved;
                result.message = moved.path;
                return result;
            }
        }
        // It was synced and you haven't typed since: it was deleted elsewhere
        // (e.g. on the laptop), so don't bring it back.
        if (!state.rev.empty() && !localChanges) {
            text.clear();
            state = SyncState();
            result.outcome = SyncResult::Deleted;
            result.textChanged = true;
            return result;
        }
        std::string rev, id;
        if (dropbox.upload(remotePath, text, "", rev, &id) != Dropbox::Result::Ok)
            return fail("uploading");
        agreed(rev, id);
        result.outcome = SyncResult::Uploaded;
        return result;
    }

    if (!remote.id.empty())
        state.id = remote.id; // files synced before IDs were kept learn theirs here
    if (remote.rev == state.rev) {
        if (!localChanges)
            return result;
        std::string rev, id;
        Dropbox::Result u = dropbox.upload(remotePath, text, state.rev, rev, &id);
        if (u == Dropbox::Result::Ok) {
            agreed(rev, id);
            result.outcome = SyncResult::Uploaded;
            return result;
        }
        if (u != Dropbox::Result::Conflict)
            return fail("uploading");
        // Changed on Dropbox in the last moment: fall through and merge.
    }

    std::string remoteText, remoteRev, remoteId;
    if (dropbox.download(remotePath, remoteText, remoteRev, &remoteId) != Dropbox::Result::Ok)
        return fail("downloading");

    if (!localChanges) {
        text = remoteText;
        agreed(remoteRev, remoteId);
        result.outcome = SyncResult::Downloaded;
        result.textChanged = true;
        return result;
    }

    if (state.keptLength >= state.syncedLength) {
        std::string merged = merge(remoteText, text, state.syncedLength);
        std::string rev, id;
        if (dropbox.upload(remotePath, merged, remoteRev, rev, &id) != Dropbox::Result::Ok)
            return fail("uploading the merge");
        text = merged;
        agreed(rev, id);
        result.outcome = SyncResult::Merged;
        result.textChanged = true;
        return result;
    }

    // Backspace removed synced text and the laptop edited too: keep both.
    std::string savedAs;
    if (dropbox.uploadCopy(conflictPath(remotePath), text, savedAs) != Dropbox::Result::Ok)
        return fail("saving a conflicted copy");
    text = remoteText;
    agreed(remoteRev, remoteId);
    result.outcome = SyncResult::Conflict;
    result.message = savedAs;
    result.textChanged = true;
    return result;
}

} // namespace tw
