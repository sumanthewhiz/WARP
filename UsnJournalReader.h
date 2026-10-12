#pragma once

// UsnJournalReader
//
// Tails the NTFS/ReFS update sequence number (USN) change journal of every fixed local
// volume and reports file creates, content modifications, deletes and renames. Journals
// are only read, never created or modified. Requires an elevated process.
//
//   * One worker thread polls all volumes every 2 s (FSCTL_READ_USN_JOURNAL).
//   * Created / Modified / Deleted are reported once per handle close (USN_REASON_CLOSE);
//     Renamed is reported when a RENAME_NEW_NAME record follows its RENAME_OLD_NAME.
//   * A per-volume cursor (journal id + next USN) is persisted through CursorLoad /
//     CursorSave (every 30 s and on Stop) so changes made while WARP was not running are
//     replayed with backfill = true, limited to the last maxBackfillHours (<= 0 disables).
//   * All callbacks (events, load, save) run on the worker thread. Stop() may be called
//     from a callback; destroying the reader from a callback is not supported.
//   * Consumers should drop events for WARP's own database files to avoid feedback loops.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct UsnFileEvent
{
    enum class Kind { Created, Modified, Deleted, Renamed };

    Kind         kind = Kind::Modified;
    std::wstring path;              // full DOS path, e.g. C:\Users\me\file.txt
    std::wstring oldPath;           // Renamed only
    int64_t      utcMs = 0;         // journal record time, UTC ms since 1970-01-01
    uint64_t     fileRef = 0;       // NTFS file reference (128-bit ReFS ids folded to 64 bits)
    std::wstring volume;            // volume GUID path (\\?\Volume{...}\), same as the cursor key
    bool         isDirectory = false;
    bool         backfill = false;  // happened before Start(), replayed from the saved cursor
};

class UsnJournalReader
{
public:
    using Callback = std::function<void(const UsnFileEvent&)>;
    // load(volumeKey, journalId, nextUsn) returns false if none; save(volumeKey, journalId, nextUsn).
    using CursorLoad = std::function<bool(const std::wstring&, uint64_t&, int64_t&)>;
    using CursorSave = std::function<void(const std::wstring&, uint64_t, int64_t)>;

    UsnJournalReader();
    ~UsnJournalReader();

    UsnJournalReader(const UsnJournalReader&) = delete;
    UsnJournalReader& operator=(const UsnJournalReader&) = delete;

    // Returns false if already running, cb is empty, or no fixed NTFS/ReFS volume has an
    // active change journal. load/save may be empty (no persistence, no backfill).
    bool Start(Callback cb, CursorLoad load, CursorSave save, int maxBackfillHours = 72);
    void Stop();
    uint64_t RecordsProcessed() const;
    std::wstring Status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
