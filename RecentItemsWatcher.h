#pragma once

// RecentItemsWatcher
//
// Watches the shell "Recent Items" folder (FOLDERID_Recent). Explorer and most apps add or
// refresh a .lnk there whenever the user explicitly opens a document or folder, so each
// change is a strong "the user opened X" signal.
//
//   * Start() first replays every .lnk written after backfillSinceUtcMs, oldest first
//     (backfill = true), then reports live changes debounced 500 ms per shortcut.
//   * utcMs is the shortcut's last-write time (when the open was recorded).
//   * Runs one STA worker thread; the callback is invoked on it. Stop() may be called from
//     the callback; destroying the watcher from the callback is not supported.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct RecentItemEvent
{
    std::wstring target;        // file/folder path, shell parsing name, or URL
    std::wstring lnkPath;       // the .lnk in the Recent folder
    int64_t      utcMs = 0;     // .lnk last-write time, UTC ms since 1970-01-01
    bool         isUrl = false;
    bool         isFolder = false;
    bool         backfill = false;
};

class RecentItemsWatcher
{
public:
    using Callback = std::function<void(const RecentItemEvent&)>;

    RecentItemsWatcher();
    ~RecentItemsWatcher();

    RecentItemsWatcher(const RecentItemsWatcher&) = delete;
    RecentItemsWatcher& operator=(const RecentItemsWatcher&) = delete;

    // Returns false if already running, cb is empty, or the Recent folder cannot be watched.
    bool Start(Callback cb, int64_t backfillSinceUtcMs);
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
