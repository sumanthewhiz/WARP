// ---------------------------------------------------------------------------
// UrlExtractor
//
// The previous URL-detection logic only emitted a URL when the browser's
// window title literally began with "http://" or "https://" -- a code path
// that essentially never fires in modern browsers (which always show a
// page title, not the URL, in the title bar). The result was that
// ~99% of browsing events had a NULL url field downstream.
//
// This module uses the Windows UI Automation API to query the browser's
// address bar directly:
//
//   * Chromium-family (Chrome, Edge, Brave, Opera, Vivaldi):
//     The omnibox surfaces an Edit control whose AutomationId is "url" or
//     whose ControlType is Edit and whose Name contains "Address". We
//     read its ValuePattern.CurrentValue.
//
//   * Firefox:
//     The "Search or enter address" edit control is a top-level Edit
//     descendant of the main window, AutomationId "urlbar-input".
//
// Important constraints:
//
//   * UI Automation calls cross process boundaries to the browser. A hung
//     browser can hang the caller indefinitely. We therefore run UIA on
//     a dedicated worker thread, and the work loop itself uses
//     Co(Un)Initialize per thread. The BrowsingMonitor's hook thread
//     submits requests to a bounded queue and never blocks waiting.
//
//   * Requests carry the full information needed to emit the final
//     event (browser, title, ctx). The worker fills in the URL and
//     invokes the user callback.
//
//   * The queue is bounded; if the worker falls behind (browser is
//     unresponsive), new requests are dropped rather than queued
//     unboundedly. This means we miss URLs but never stall.
// ---------------------------------------------------------------------------

#pragma once

#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "EventContext.h"

class UrlExtractor
{
public:
    using EmitCallback = std::function<void(const std::wstring& browser,
                                            const std::wstring& title,
                                            const std::wstring& url,
                                            const EventContext& ctx)>;

    static UrlExtractor& Instance();

    void Start();
    void Stop();

    // Submit a request. `hwnd` is the browser top-level window. The worker
    // will compute the URL via UIA and invoke the previously-registered
    // emit callback. Drops the request if the queue is full.
    void Submit(HWND hwnd,
                const std::wstring& browser,
                const std::wstring& title,
                const EventContext& ctx);

    void SetEmitCallback(EmitCallback cb) { m_emitCb = std::move(cb); }

private:
    UrlExtractor() = default;
    UrlExtractor(const UrlExtractor&) = delete;

    struct Request
    {
        HWND          hwnd;
        std::wstring  browser;
        std::wstring  title;
        EventContext  ctx;
    };

    void WorkerLoop();
    std::wstring ExtractUrl(HWND hwnd, const std::wstring& browser);

    static constexpr size_t kMaxQueue = 32;

    EmitCallback              m_emitCb;
    std::thread               m_worker;
    std::atomic<bool>         m_running{ false };
    std::mutex                m_mtx;
    std::condition_variable   m_cv;
    std::deque<Request>       m_queue;
};
