#include "framework.h"
#include "AppLaunchMonitor.h"
#include "EventContext.h"
#include <tlhelp32.h>
#include <algorithm>
#include <unordered_set>
#include <mutex>
#include <psapi.h>
#include <evntcons.h>
#include <vector>
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

// ---------------------------------------------------------------------------
// Static state for the ETW callback (must be accessible from a C-style
// ETW callback that has no `this` pointer).
// ---------------------------------------------------------------------------
static AppLaunchMonitor* g_pAppLaunchMonitor = nullptr;

// Microsoft-Windows-Kernel-Process provider GUID
//   {22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}
static const GUID KernelProcessProviderGuid =
    { 0x22FB2CD6, 0x0E7B, 0x422B, { 0xA0, 0xC7, 0x2F, 0xAD, 0x1F, 0xD0, 0xE7, 0x16 } };

// Keyword bits from the Microsoft-Windows-Kernel-Process manifest
static const ULONGLONG KERNEL_PROC_KEYWORD_PROCESS = 0x10;

// Per-WARP-instance private session GUID (random GUID, never shipped)
static const GUID WarpProcSessionGuid =
    { 0xb8e94a17, 0x4d12, 0x4b88, { 0x83, 0xc2, 0x1c, 0x32, 0x9d, 0x44, 0x88, 0x09 } };

static const wchar_t* const kAppLaunchSessionName = L"WARP-ProcessTrace";

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
    g_pAppLaunchMonitor = this;
    // EtwLoop blocks in ProcessTrace, so we own the thread for its lifetime.
    m_thread = std::thread(&AppLaunchMonitor::EtwLoop, this);
}

void AppLaunchMonitor::Stop()
{
    if (!m_running) return;
    m_running = false;
    SetEvent(m_stopEvent);

    // Tear down the trace so ProcessTrace unblocks
    StopProcessEtwTrace();

    if (m_thread.joinable()) m_thread.join();
    g_pAppLaunchMonitor = nullptr;
}

void AppLaunchMonitor::Pause()
{
    m_paused = true;
}

void AppLaunchMonitor::Resume()
{
    m_paused = false;
}

// ---------------------------------------------------------------------------
// EtwLoop
//
// Replaces the previous 2-second Toolhelp32 polling loop with a real-time
// ETW subscription on Microsoft-Windows-Kernel-Process. The polling design
// was unreliable for two reasons:
//   1. Short-lived processes (PowerShell one-liners, build tools, certutil
//      verifications, defender scan helpers) often complete in <2s and were
//      never observed at all.
//   2. PID recycling between polls would cause us to miss the new process
//      AND conflate it with whatever recycled PID was now running.
//
// ETW gives us exact creation events with the new PID, parent PID, and the
// command line, so we get correct attribution for every process start.
// ---------------------------------------------------------------------------
void AppLaunchMonitor::EtwLoop()
{
    StartProcessEtwTrace();
}

void AppLaunchMonitor::StartProcessEtwTrace()
{
    StopProcessEtwTrace();          // tear down any stale prior session

    const size_t sessionNameBytes = (wcslen(kAppLaunchSessionName) + 1) * sizeof(wchar_t);
    const size_t bufSize = sizeof(EVENT_TRACE_PROPERTIES) + sessionNameBytes + 1024;
    std::vector<BYTE> propsBuf(bufSize, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuf.data());

    props->Wnode.BufferSize    = static_cast<ULONG>(bufSize);
    props->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1;
    props->Wnode.Guid          = WarpProcSessionGuid;
    props->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
    props->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);
    props->BufferSize          = 32;        // KB; process events are tiny
    props->MinimumBuffers      = 2;
    props->MaximumBuffers      = 8;
    props->FlushTimer          = 1;

    ULONG status = StartTraceW(&m_etwSessionHandle, kAppLaunchSessionName, props);
    if (status == ERROR_ALREADY_EXISTS)
    {
        ControlTraceW(0, kAppLaunchSessionName, props, EVENT_TRACE_CONTROL_STOP);
        memset(propsBuf.data(), 0, bufSize);
        props->Wnode.BufferSize    = static_cast<ULONG>(bufSize);
        props->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
        props->Wnode.ClientContext = 1;
        props->Wnode.Guid          = WarpProcSessionGuid;
        props->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
        props->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);
        props->BufferSize          = 32;
        props->MinimumBuffers      = 2;
        props->MaximumBuffers      = 8;
        props->FlushTimer          = 1;
        status = StartTraceW(&m_etwSessionHandle, kAppLaunchSessionName, props);
    }
    if (status != ERROR_SUCCESS)
    {
        m_etwSessionHandle = 0;
        return;
    }

    status = EnableTraceEx2(
        m_etwSessionHandle,
        &KernelProcessProviderGuid,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_INFORMATION,
        KERNEL_PROC_KEYWORD_PROCESS,
        0, 0, nullptr);
    if (status != ERROR_SUCCESS)
    {
        StopProcessEtwTrace();
        return;
    }

    EVENT_TRACE_LOGFILEW logFile = {};
    logFile.LoggerName          = const_cast<LPWSTR>(kAppLaunchSessionName);
    logFile.ProcessTraceMode    = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = &AppLaunchMonitor::EtwEventCallback;

    m_etwTraceHandle = OpenTraceW(&logFile);
    if (m_etwTraceHandle == INVALID_PROCESSTRACE_HANDLE)
    {
        StopProcessEtwTrace();
        return;
    }

    // Blocks until Stop() tears down the session
    ProcessTrace(&m_etwTraceHandle, 1, nullptr, nullptr);

    if (m_etwTraceHandle != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(m_etwTraceHandle);
        m_etwTraceHandle = INVALID_PROCESSTRACE_HANDLE;
    }
}

void AppLaunchMonitor::StopProcessEtwTrace()
{
    if (m_etwTraceHandle != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(m_etwTraceHandle);
        m_etwTraceHandle = INVALID_PROCESSTRACE_HANDLE;
    }
    const size_t sessionNameBytes = (wcslen(kAppLaunchSessionName) + 1) * sizeof(wchar_t);
    const size_t bufSize = sizeof(EVENT_TRACE_PROPERTIES) + sessionNameBytes + 1024;
    std::vector<BYTE> propsBuf(bufSize, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuf.data());
    props->Wnode.BufferSize = static_cast<ULONG>(bufSize);
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    ControlTraceW(0, kAppLaunchSessionName, props, EVENT_TRACE_CONTROL_STOP);
    m_etwSessionHandle = 0;
}

void WINAPI AppLaunchMonitor::EtwEventCallback(PEVENT_RECORD pEvent)
{
    if (!pEvent) return;
    AppLaunchMonitor* self = g_pAppLaunchMonitor;
    if (!self || !self->m_running) return;
    if (!IsEqualGUID(pEvent->EventHeader.ProviderId, KernelProcessProviderGuid))
        return;

    const USHORT id = pEvent->EventHeader.EventDescriptor.Id;
    if (id == 1)
    {
        // ProcessStart
        if (!self->m_callback) return;
        if (self->m_paused) return;

        DWORD pid = pEvent->EventHeader.ProcessId;
        if (pid == 0 || pid == 4) return;
        if (pid == GetCurrentProcessId()) return;

        std::wstring exePath = EventContextUtil::GetExePathByPid(pid);
        if (exePath.empty()) return;

        std::wstring exeName;
        size_t bs = exePath.find_last_of(L"\\/");
        exeName = (bs == std::wstring::npos) ? exePath : exePath.substr(bs + 1);
        std::wstring exeLower = exeName;
        std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(), ::towlower);

        if (IsSystemProcess(exeLower))   return;
        if (IsSystemExePath(exePath))    return;
        if (IsSpawnedByServiceHost(pid)) return;

        DWORD sessionId = 0;
        if (!ProcessIdToSessionId(pid, &sessionId) || sessionId == 0) return;

        EventContext ctx = EventContextUtil::CaptureContext(pid);
        ctx.parentPid = EventContextUtil::GetParentPid(pid);
        if (ctx.parentPid != 0)
            ctx.parentExe = EventContextUtil::GetExePathByPid(ctx.parentPid);
        self->m_callback(exeName, exePath, pid, ctx);
    }
    else if (id == 2)
    {
        // ProcessStop -- forget cached image path so a recycled PID doesn't
        // resolve to the dead image later.
        EventContextUtil::ForgetPid(pEvent->EventHeader.ProcessId);
    }
}
