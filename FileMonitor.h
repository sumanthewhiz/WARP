#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <evntrace.h>

#include "EventContext.h"

// Callback: action (CREATE/OPEN/MODIFY/DELETE/RENAME), path, old_path (for
// rename), and the EventContext describing source/foreground/intent state.
using FileActivityCallback = std::function<void(const std::wstring& action,
                                                const std::wstring& path,
                                                const std::wstring& oldPath,
                                                const EventContext&  ctx)>;

class FileMonitor
{
public:
    FileMonitor();
    ~FileMonitor();

    void SetCallback(FileActivityCallback cb);
    void Start();
    void Stop();
    void Pause();
    void Resume();

private:
    FileActivityCallback m_callback;
    std::vector<std::thread> m_threads;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_paused{ false };
    HANDLE m_stopEvent = nullptr;

    // ETW session for file-open tracking
    TRACEHANDLE m_etwSessionHandle = 0;
    TRACEHANDLE m_etwTraceHandle = INVALID_PROCESSTRACE_HANDLE;

    void MonitorDrive(const std::wstring& root);
    std::vector<std::wstring> GetDriveRoots();
    void MonitorShellNotifications();
    void StartEtwTrace();
    void StopEtwTrace();
    static void WINAPI EtwEventCallback(PEVENT_RECORD pEvent);
};
