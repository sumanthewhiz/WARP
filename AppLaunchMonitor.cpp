#include "framework.h"
#include "AppLaunchMonitor.h"
#include <tlhelp32.h>
#include <algorithm>
#include <unordered_set>
#include <mutex>
#include <psapi.h>

// Known system/noise processes to exclude from app launch events
static bool IsSystemProcess(const std::wstring& exeLower)
{
    static const wchar_t* const excluded[] = {
        L"svchost.exe", L"csrss.exe", L"smss.exe", L"lsass.exe",
        L"services.exe", L"wininit.exe", L"spoolsv.exe", L"wmiprvse.exe",
        L"taskhostw.exe", L"runtimebroker.exe", L"backgroundtaskhost.exe",
        L"audiodg.exe", L"fontdrvhost.exe", L"dwm.exe", L"conhost.exe",
        L"dllhost.exe", L"sihost.exe", L"ctfmon.exe", L"settingsynchost.exe",
        L"searchprotocolhost.exe", L"searchindexer.exe", L"searchfilterhost.exe",
        L"msmpeng.exe", L"mpcmdrun.exe", L"nissrv.exe",
        L"securityhealthservice.exe", L"securityhealthsystray.exe",
        L"trustedinstaller.exe", L"tiworker.exe",
        L"compattelrunner.exe", L"devicecensus.exe",
        L"crashpad_handler.exe", L"systemsettings.exe",
        L"phoneexperiencehost.exe", L"widgetservice.exe",
        L"gamebarpresencewriter.exe", L"system",
        L"registry", L"idle",
        L"warp!.exe",
    };

    for (const auto* s : excluded)
    {
        if (exeLower == s) return true;
    }
    return false;
}

AppLaunchMonitor::AppLaunchMonitor()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

AppLaunchMonitor::~AppLaunchMonitor()
{
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

void AppLaunchMonitor::SetCallback(AppLaunchCallback cb)
{
    m_callback = std::move(cb);
}

void AppLaunchMonitor::Start()
{
    if (m_running) return;
    m_running = true;
    ResetEvent(m_stopEvent);
    m_thread = std::thread(&AppLaunchMonitor::MonitorLoop, this);
}

void AppLaunchMonitor::Stop()
{
    if (!m_running) return;
    m_running = false;
    SetEvent(m_stopEvent);
    if (m_thread.joinable()) m_thread.join();
}

void AppLaunchMonitor::Pause()
{
    m_paused = true;
}

void AppLaunchMonitor::Resume()
{
    m_paused = false;
}

void AppLaunchMonitor::MonitorLoop()
{
    // Track known PIDs; report new ones appearing between polls
    std::unordered_set<DWORD> knownPids;

    // Seed with current processes (don't report existing ones)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            PROCESSENTRY32W pe = {};
            pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe))
            {
                do {
                    knownPids.insert(pe.th32ProcessID);
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
    }

    while (m_running)
    {
        // Poll every 2 seconds
        DWORD waitResult = WaitForSingleObject(m_stopEvent, 2000);
        if (waitResult == WAIT_OBJECT_0 || !m_running)
            break;

        if (m_paused)
            continue;

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            continue;

        std::unordered_set<DWORD> currentPids;
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);

        if (Process32FirstW(snap, &pe))
        {
            do {
                currentPids.insert(pe.th32ProcessID);

                // New process?
                if (knownPids.find(pe.th32ProcessID) == knownPids.end())
                {
                    DWORD pid = pe.th32ProcessID;
                    if (pid == 0 || pid == 4)
                        continue;

                    // Check session -- only interactive sessions
                    DWORD sessionId = 0;
                    if (!ProcessIdToSessionId(pid, &sessionId) || sessionId == 0)
                        continue;

                    std::wstring exeName(pe.szExeFile);
                    std::wstring exeLower = exeName;
                    std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(), ::towlower);

                    if (IsSystemProcess(exeLower))
                        continue;

                    // Get full path
                    std::wstring exePath;
                    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                    if (hProc)
                    {
                        wchar_t path[MAX_PATH] = {};
                        DWORD pathLen = MAX_PATH;
                        if (QueryFullProcessImageNameW(hProc, 0, path, &pathLen) && pathLen > 0)
                            exePath.assign(path, pathLen);
                        CloseHandle(hProc);
                    }

                    if (exePath.empty())
                        exePath = exeName;

                    if (m_callback)
                        m_callback(exeName, exePath, pid);
                }
            } while (Process32NextW(snap, &pe));
        }

        CloseHandle(snap);
        knownPids = std::move(currentPids);
    }
}
