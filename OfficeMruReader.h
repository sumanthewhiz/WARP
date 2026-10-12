#pragma once

// OfficeMruReader
//
// Reads the Office "recent files" lists from the registry:
//   HKCU\Software\Microsoft\Office\{16.0,15.0}\<App>\File MRU
//   HKCU\Software\Microsoft\Office\{16.0,15.0}\<App>\User MRU\<identity>\File MRU
// for Word, Excel, PowerPoint, Visio, Access and Publisher. Each "Item N" value looks like
//   [F00000000][T01D9C2A3B4C5D6E7][O00000000]*C:\path\file.docx
// where T is the last-open FILETIME (hex) and the text after '*' is a path or URL.
//
// Start() polls every pollMs (minimum 1 s) on a worker thread and reports an item when its
// time is newer than backfillSinceUtcMs and newer than the last time reported for the same
// (app, path). Items found by the first poll are flagged backfill = true. The callback runs
// on the worker thread; Stop() may be called from it.

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct OfficeMruItem
{
    std::wstring app;           // "Word", "Excel", "PowerPoint", "Visio", "Access", "Publisher"
    std::wstring pathOrUrl;
    int64_t      utcMs = 0;     // last open, UTC ms since 1970-01-01
    bool         isUrl = false;
    bool         backfill = false;
};

class OfficeMruReader
{
public:
    using Callback = std::function<void(const OfficeMruItem&)>;

    OfficeMruReader();
    ~OfficeMruReader();

    OfficeMruReader(const OfficeMruReader&) = delete;
    OfficeMruReader& operator=(const OfficeMruReader&) = delete;

    // Returns false if already running or cb is empty.
    bool Start(Callback cb, int64_t backfillSinceUtcMs, DWORD pollMs = 120000);
    void Stop();

    // One snapshot of all MRU lists, de-duplicated per (app, path) keeping the newest time,
    // sorted oldest first. backfill is always false. Safe to call from any thread.
    static std::vector<OfficeMruItem> ReadAll();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
