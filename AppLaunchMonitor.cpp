#include "framework.h"
#include "AppLaunchMonitor.h"
#include "EventContext.h"
#include <tlhelp32.h>
#include <algorithm>
#include <unordered_set>
#include <mutex>
#include <psapi.h>

// Known system/noise processes to exclude from app launch events
static bool IsSystemProcess(const std::wstring& exeLower)
{
    static const wchar_t* const excluded[] = {
        // Core Windows system processes
        L"svchost.exe", L"csrss.exe", L"smss.exe", L"lsass.exe",
        L"services.exe", L"wininit.exe", L"winlogon.exe", L"spoolsv.exe",
        L"wmiprvse.exe", L"wmiapsrv.exe",
        // Shell infrastructure
        L"taskhostw.exe", L"runtimebroker.exe", L"backgroundtaskhost.exe",
        L"audiodg.exe", L"fontdrvhost.exe", L"dwm.exe", L"conhost.exe",
        L"dllhost.exe", L"sihost.exe", L"ctfmon.exe", L"settingsynchost.exe",
        L"explorer.exe",
        L"shellexperiencehost.exe", L"startmenuexperiencehost.exe",
        L"applicationframehost.exe", L"textinputhost.exe",
        L"lockapp.exe", L"logonui.exe",
        // Search / Indexer
        L"searchprotocolhost.exe", L"searchindexer.exe", L"searchfilterhost.exe",
        L"searchhost.exe", L"searchapp.exe",
        // Windows Defender / Security
        L"msmpeng.exe", L"mpcmdrun.exe", L"nissrv.exe",
        L"securityhealthservice.exe", L"securityhealthsystray.exe",
        L"smartscreen.exe", L"sgrmbroker.exe",
        // Windows Update / Servicing
        L"trustedinstaller.exe", L"tiworker.exe", L"musnotifybroker.exe",
        L"usoclient.exe", L"usocoreworker.exe", L"wuauclt.exe",
        L"msiexec.exe", L"msiexec64.exe",
        // Telemetry / Compat
        L"compattelrunner.exe", L"devicecensus.exe",
        L"diagtrack.exe", L"disksnapshot.exe",
        L"musnotification.exe", L"windowsupdatebox.exe",
        // Browser helpers / crash handlers
        L"crashpad_handler.exe",
        L"msedgewebview2.exe",
        L"elevation_service.exe",
        // Widgets / Phone / Xbox
        L"phoneexperiencehost.exe", L"widgetservice.exe", L"widgets.exe",
        L"gamebarpresencewriter.exe", L"gamebarftserver.exe",
        L"gamebar.exe", L"gamedvr.exe",
        // System settings / UWP infrastructure
        L"systemsettings.exe", L"systemsettingsbroker.exe",
        L"systemsettingsadminflows.exe",
        L"useroobebroker.exe", L"oobe.exe",
        // .NET / runtime hosts
        L"dashost.exe", L"wsappx.exe",
        L"comppkgsrv.exe", L"pkgmgr.exe",
        L"ngen.exe", L"ngentask.exe", L"mscorsvw.exe",
        // Misc system utilities
        L"consent.exe",
        L"taskmgr.exe",
        L"cmd.exe", L"powershell.exe", L"pwsh.exe",
        L"werfault.exe", L"wermgr.exe",
        L"mobsync.exe", L"prevhost.exe",
        // Pseudo-processes
        L"system", L"registry", L"idle",
        // Self
        L"warp!.exe",
    };

    for (const auto* s : excluded)
    {
        if (exeLower == s) return true;
    }
    return false;
}

// Returns true if the exe path is inside a system directory, suggesting the
// process is OS-managed rather than user-initiated.
static bool IsSystemExePath(const std::wstring& exePath)
{
    if (exePath.empty())
        return false;

    std::wstring lp = exePath;
    std::transform(lp.begin(), lp.end(), lp.begin(), ::towlower);

    // Processes running from Windows, WinSxS, or SystemApps are system-initiated
    static const wchar_t* const systemDirs[] = {
        L"\\windows\\system32\\",
        L"\\windows\\syswow64\\",
        L"\\windows\\systemapps\\",
        L"\\windows\\immersivecontrolpanel\\",
        L"\\windows\\winsxs\\",
        L"\\windows\\servicing\\",
        L"\\windows\\security\\",
        L"\\windows\\temp\\",
    };

    for (const auto* dir : systemDirs)
    {
        if (lp.find(dir) != std::wstring::npos)
            return true;
    }
    return false;
}

// Returns true if the process was spawned by a service host (services.exe or
// svchost.exe), which indicates it is system-managed, not user-initiated.
static bool IsSpawnedByServiceHost(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    DWORD parentPid = 0;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do {
            if (pe.th32ProcessID == pid)
            {
                parentPid = pe.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }

    if (parentPid == 0 || parentPid == 4)
    {
        CloseHandle(snap);
        return true; // parented by System
    }

    // Look up the parent's exe name
    bool isServiceHost = false;
    pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do {
            if (pe.th32ProcessID == parentPid)
            {
                std::wstring parentExe(pe.szExeFile);
                std::transform(parentExe.begin(), parentExe.end(),
                               parentExe.begin(), ::towlower);
                if (parentExe == L"services.exe" ||
                    parentExe == L"svchost.exe")
                {
                    isServiceHost = true;
                }
                break;
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return isServiceHost;
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

                    // Filter out processes running from system directories
                    if (IsSystemExePath(exePath))
                        continue;

                    // Filter out processes spawned by services.exe / svchost.exe
                    if (IsSpawnedByServiceHost(pid))
                        continue;

                    if (m_callback)
                    {
                        // For a freshly-launched process the source IS the
                        // new PID. parentPid+parentExe are stamped here so
                        // downstream consumers can identify svchost-spawned
                        // workers without re-walking the process tree.
                        EventContext ctx = EventContextUtil::CaptureContext(pid);
                        ctx.parentPid = EventContextUtil::GetParentPid(pid);
                        if (ctx.parentPid != 0)
                            ctx.parentExe = EventContextUtil::GetExePathByPid(ctx.parentPid);
                        m_callback(exeName, exePath, pid, ctx);
                    }
                }
            } while (Process32NextW(snap, &pe));
        }

        CloseHandle(snap);
        knownPids = std::move(currentPids);
    }
}
