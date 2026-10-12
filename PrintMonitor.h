#pragma once

// PrintMonitor
// -----------------------------------------------------------------------------
// Reports printed documents from the Microsoft-Windows-PrintService/Operational
// event log channel (EventID 307, "document printed") through a push
// subscription (EvtSubscribe, future events only). The callback runs on a
// system thread-pool thread and must not call Start()/Stop() expecting them to
// block (Stop() from the callback only stops delivery; the subscription handle
// is closed by the next Start()/Stop() on another thread or the destructor).
// The channel is disabled by default; Start() can enable it (administrator
// rights required -- WARP runs elevated). The channel is left enabled on Stop().
// -----------------------------------------------------------------------------

#include <windows.h>
#include <functional>
#include <string>
#include <mutex>
#include <atomic>
#include <cstdint>

struct PrintEvent
{
    int64_t utcMs = 0;
    DWORD   jobId = 0;
    std::wstring documentName;
    std::wstring owner;
    std::wstring clientMachine;
    std::wstring printer;
    DWORD   pages = 0;
};

class PrintMonitor
{
public:
    using Callback = std::function<void(const PrintEvent&)>;

    PrintMonitor() = default;
    ~PrintMonitor();
    PrintMonitor(const PrintMonitor&) = delete;
    PrintMonitor& operator=(const PrintMonitor&) = delete;

    // Subscribes to Microsoft-Windows-PrintService/Operational EventID 307 (document printed) via EvtSubscribe (future events).
    // That channel is disabled by default; if enableChannelIfDisabled, enable it via EvtOpenChannelConfig /
    // EvtSetChannelConfigProperty(EvtChannelConfigEnabled) / EvtSaveChannelConfig (requires admin; WARP is elevated).
    // Returns false (and Status() explains) if unavailable; never throws.
    bool Start(Callback cb, bool enableChannelIfDisabled);
    void Stop();                 // idempotent; waits for an in-flight callback to finish
    bool IsActive() const;       // false after a subscription error: Stop() + Start() to resubscribe
    std::wstring Status() const;

private:
    friend struct PrintMonitorThunk;     // EvtSubscribe callback bridge, defined in PrintMonitor.cpp

    void OnEvent(HANDLE eventHandle);
    void OnSubscriptionError(DWORD error);
    void SetStatus(const std::wstring& status);
    void CloseSubscriptionLocked();      // requires m_lifecycleMutex

    mutable std::mutex m_mutex;                    // guards m_status and m_callback
    std::mutex         m_lifecycleMutex;           // serializes Start/Stop
    std::wstring       m_status = L"not started";
    Callback           m_callback;
    HANDLE             m_subscription = nullptr;   // EVT_HANDLE
    std::atomic<bool>  m_accepting{ false };       // deliver events to the callback
    std::atomic<bool>  m_healthy{ false };         // subscribed and no error reported since
};
