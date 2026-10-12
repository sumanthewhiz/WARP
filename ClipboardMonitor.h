#pragma once

// ClipboardMonitor
// -----------------------------------------------------------------------------
// Reports clipboard METADATA only: sequence number, owner process and the
// coarse kinds of data on offer. It never opens the clipboard and never reads
// clipboard data.
//
// A dedicated thread owns a message-only window registered with
// AddClipboardFormatListener. Updates are coalesced: the first
// WM_CLIPBOARDUPDATE of a burst opens a 250 ms window and one event describing
// the latest clipboard state is emitted when it closes (skipped if the
// sequence number did not change). The callback runs on that thread.
// -----------------------------------------------------------------------------

#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdint>

struct ClipboardEvent
{
    int64_t utcMs = 0;
    DWORD   sequence = 0;
    DWORD   ownerPid = 0;
    std::wstring ownerExePath;
    std::vector<std::wstring> formats;   // subset of: L"text", L"image", L"files", L"html", L"rtf", L"other"
};

class ClipboardMonitor
{
public:
    using Callback = std::function<void(const ClipboardEvent&)>;

    ClipboardMonitor() = default;
    ~ClipboardMonitor();
    ClipboardMonitor(const ClipboardMonitor&) = delete;
    ClipboardMonitor& operator=(const ClipboardMonitor&) = delete;

    // Own thread + message-only window + AddClipboardFormatListener. Returns true if already
    // running (the new callback is then ignored); false if cb is empty or setup failed.
    bool Start(Callback cb);
    void Stop();   // idempotent; joins the thread (only signals it when called from the callback)

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void ThreadMain();
    void RunLoop();
    void EmitIfChanged();
    std::vector<std::wstring> ReadFormatKinds() const;
    bool IsMonitorThread() const;

    Callback           m_callback;
    std::mutex         m_lifecycleMutex;     // serializes Start/Stop
    std::thread        m_thread;
    std::atomic<DWORD> m_threadId{ 0 };
    std::atomic<int>   m_startResult{ 0 };   // 0 pending, 1 ok, -1 failed
    HANDLE             m_stopEvent  = nullptr;
    HANDLE             m_readyEvent = nullptr;

    // Monitor-thread state.
    HWND      m_hwnd = nullptr;
    bool      m_pending = false;
    ULONGLONG m_pendingSinceTick = 0;
    DWORD     m_lastSequence = 0;
    UINT      m_cfHtml = 0;
    UINT      m_cfRtf = 0;
};
