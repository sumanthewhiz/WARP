#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>

// Callback: action (CREATE/MODIFY/DELETE/RENAME), path, old_path (for rename)
using FileActivityCallback = std::function<void(const std::wstring& action,
                                                const std::wstring& path,
                                                const std::wstring& oldPath)>;

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

    void MonitorDrive(const std::wstring& root);
    std::vector<std::wstring> GetDriveRoots();
    void MonitorShellNotifications();
};
