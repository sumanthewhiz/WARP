#include "framework.h"
#include "ForegroundMonitor.h"
#include "EventContext.h"
#include <algorithm>
#include <psapi.h>

// Returns true if the process is a system/shell process whose foreground
// presence is not meaningful user activity.
static bool IsSystemForeground(const std::wstring& exeLower)
{
    static const wchar_t* const excluded[] = {
        // Shell / desktop
        L"explorer.exe",
        L"shellexperiencehost.exe",
        L"startmenuexperiencehost.exe",
        L"applicationframehost.exe",
        L"textinputhost.exe",
        L"lockapp.exe",
        L"logonui.exe",
        // System UI
        L"dwm.exe",
        L"csrss.exe",
        L"taskhostw.exe",
        L"runtimebroker.exe",
        L"sihost.exe",
        L"ctfmon.exe",
        // Widgets / Cortana
        L"widgets.exe",
        L"widgetservice.exe",
        L"searchhost.exe",
        L"searchapp.exe",
        L"searchui.exe",
        // Security popups
        L"consent.exe",
        L"smartscreen.exe",
        L"securityhealthsystray.exe",
        // Error reporting
        L"werfault.exe",
        L"wermgr.exe",
        // Self
        L"warp!.exe",
    };

    for (const auto* s : excluded)
    {
        if (exeLower == s) return true;
    }
    return false;
}

// Get process exe name and full path for a PID.
// Returns false if the process cannot be queried.
static bool GetProcessInfo(DWORD pid, std::wstring& outExeName, std::wstring& outExePath)
{
    outExeName.clear();
    outExePath.clear();

    if (pid == 0 || pid == 4)
        return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc)
        return false;

    wchar_t exePath[MAX_PATH] = {};
    DWORD exeLen = MAX_PATH;
    if (!QueryFullProcessImageNameW(hProc, 0, exePath, &exeLen) || exeLen == 0)
    {
        CloseHandle(hProc);
        return false;
    }
    CloseHandle(hProc);

    outExePath.assign(exePath, exeLen);

    // Extract filename
    size_t slash = outExePath.rfind(L'\\');
    outExeName = (slash != std::wstring::npos)
                     ? outExePath.substr(slash + 1)
                     : outExePath;

    return true;
}

ForegroundMonitor::ForegroundMonitor()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

ForegroundMonitor::~ForegroundMonitor()
{
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

void ForegroundMonitor::SetCallback(ForegroundCallback cb)
{
    m_callback = std::move(cb);
}

void ForegroundMonitor::Start()
{
    if (m_running) return;
    m_running = true;
    ResetEvent(m_stopEvent);
    m_thread = std::thread(&ForegroundMonitor::MonitorLoop, this);
}

void ForegroundMonitor::Stop()
{
    if (!m_running) return;
    m_running = false;
    SetEvent(m_stopEvent);
    if (m_thread.joinable()) m_thread.join();
}

void ForegroundMonitor::Pause()
{
    m_paused = true;
}

void ForegroundMonitor::Resume()
{
    m_paused = false;
}

void ForegroundMonitor::MonitorLoop()
{
    // State for the currently-tracked foreground session
    DWORD        prevPid = 0;
    std::wstring prevExeName;
    std::wstring prevExePath;
    std::wstring prevTitle;
    ULONGLONG    prevStartTick = 0;

    while (m_running)
    {
        // Poll every 3 seconds
        DWORD waitResult = WaitForSingleObject(m_stopEvent, 3000);
        if (waitResult == WAIT_OBJECT_0 || !m_running)
            break;

        if (m_paused)
            continue;

        HWND hFg = GetForegroundWindow();
        if (!hFg)
            continue;

        DWORD fgPid = 0;
        GetWindowThreadProcessId(hFg, &fgPid);
        if (fgPid == 0)
            continue;

        // Get window title
        wchar_t titleBuf[1024] = {};
        int titleLen = GetWindowTextW(hFg, titleBuf, 1024);
        std::wstring title(titleBuf, titleLen > 0 ? titleLen : 0);

        // Detect foreground change: different PID or different window title
        bool changed = (fgPid != prevPid) || (title != prevTitle);

        if (changed)
        {
            ULONGLONG nowTick = GetTickCount64();

            // Emit the PREVIOUS session if it was valid and had meaningful duration
            if (prevPid != 0 && !prevExeName.empty() && prevStartTick != 0)
            {
                int durationSecs = static_cast<int>((nowTick - prevStartTick) / 1000);
                if (durationSecs >= 3 && m_callback)  // at least one poll interval
                {
                    EventContext ctx = EventContextUtil::CaptureContext(prevPid);
                    m_callback(prevExeName, prevExePath, prevTitle, durationSecs, ctx);
                }
            }

            // Start tracking the new foreground
            std::wstring exeName, exePath;
            if (GetProcessInfo(fgPid, exeName, exePath))
            {
                std::wstring exeLower = exeName;
                std::transform(exeLower.begin(), exeLower.end(),
                               exeLower.begin(), ::towlower);

                if (IsSystemForeground(exeLower))
                {
                    // Don't track system windows; clear state
                    prevPid = 0;
                    prevExeName.clear();
                    prevExePath.clear();
                    prevTitle.clear();
                    prevStartTick = 0;
                }
                else
                {
                    prevPid       = fgPid;
                    prevExeName   = exeName;
                    prevExePath   = exePath;
                    prevTitle     = title;
                    prevStartTick = nowTick;
                }
            }
            else
            {
                prevPid = 0;
                prevExeName.clear();
                prevExePath.clear();
                prevTitle.clear();
                prevStartTick = 0;
            }
        }
    }

    // Emit the final session on shutdown
    if (prevPid != 0 && !prevExeName.empty() && prevStartTick != 0)
    {
        ULONGLONG nowTick = GetTickCount64();
        int durationSecs = static_cast<int>((nowTick - prevStartTick) / 1000);
        if (durationSecs >= 3 && m_callback)
        {
            EventContext ctx = EventContextUtil::CaptureContext(prevPid);
            m_callback(prevExeName, prevExePath, prevTitle, durationSecs, ctx);
        }
    }
}
