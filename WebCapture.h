// ---------------------------------------------------------------------------
// WebCapture -- browser page capture. Replaces BrowsingMonitor + UrlExtractor
// (gap-analysis WEB-1..WEB-8).
//
// Threading:
//   * UI thread (the thread that calls Start(); it must pump messages): receives
//     ForegroundChangeBroker callbacks, owns the per-browser-pid
//     EVENT_OBJECT_NAMECHANGE hook and a message-only window. Only hook
//     management, cheap window queries and enqueueing happen there.
//   * One worker thread (COINIT_MULTITHREADED) owns every UI Automation object
//     (AddressBarReader), debounces per window (latest request wins), reads the
//     address bar, applies privacy rules and invokes the callback.
//
// Privacy: private windows (InPrivate / Incognito / Private Browsing / Tor) and
// excluded apps, titles and domains are never emitted. While paused (SetPaused or
// Settings::IsPaused) nothing is recorded. URLs are canonicalized (UrlCanon).
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "ActivityStore.h"

class WebCapture
{
public:
    using Callback = std::function<void(BrowseEventRecord&& rec)>;

    struct PageInfo
    {
        std::wstring browser, title, url, host, docId, docKind;
        bool         isPrivate = false, excluded = false;
        int64_t      sinceMs = 0;          // Unix ms when this page was first seen in the window
        DWORD        pid = 0;
    };

    struct Stats
    {
        uint64_t pages = 0, privateDropped = 0, excludedDropped = 0, uiaRequests = 0, uiaTimeouts = 0,
                 uiaFailures = 0, queueDropped = 0;
        int64_t  lastEventMs = 0;
    };

    WebCapture();
    ~WebCapture();
    WebCapture(const WebCapture&) = delete;
    WebCapture& operator=(const WebCapture&) = delete;

    void SetCallback(Callback cb);   // callback runs on the worker thread; do not call Stop() from it
    void Start();                    // call on the UI thread (it has a message pump)
    void Stop();                     // joins the worker; hooks are removed on (or posted to) the UI thread
    void SetPaused(bool paused);     // privacy pause: nothing recorded
    void Heartbeat();                // optional, any thread: re-checks Settings pause state promptly

    // Latest page seen in this top-level window (thread-safe). Used by the focus tracker to attach the
    // URL entity to focus sessions and to redact private / excluded windows.
    bool LookupPage(HWND root, PageInfo& out) const;
    bool IsPrivateWindow(HWND root) const;    // cached; quick heuristics synchronously, UIA result when available
    // Known browser executables plus Chromium/Gecko browsers learned by the runtime probe.
    static bool IsBrowserProcess(const std::wstring& exePath, std::wstring* browserName = nullptr);
    Stats GetStats() const;

    // ---- pure helpers (public for unit tests) ----
    // Display title: invisible chars removed, Unicode spaces collapsed, browser suffix / private
    // markers / Edge "and N more pages" / leading unread counters stripped.
    static std::wstring NormalizeTitle(const std::wstring& rawTitle, const std::wstring& browserName = std::wstring());
    // De-dup key of a normalized title: lowercase, digits and emptied brackets removed.
    static std::wstring TitleDedupKey(const std::wstring& normalizedTitle);
    static bool TitleMarksPrivate(const std::wstring& rawTitle);
    static bool CommandLineMarksPrivate(const std::wstring& commandLine);
    static bool IsPlaceholderTitle(const std::wstring& normalizedTitle);   // "New Tab", "Untitled", ...

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
