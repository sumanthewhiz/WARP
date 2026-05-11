#include "framework.h"
#include "ForegroundMonitor.h"
#include "ForegroundChangeBroker.h"
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
}

ForegroundMonitor::~ForegroundMonitor()
{
    Stop();
}

void ForegroundMonitor::SetCallback(ForegroundCallback cb)
{
    m_callback = std::move(cb);
}

void ForegroundMonitor::Start()
{
    if (m_running) return;
    m_running = true;
    // Subscribe to the broker. The broker installs the WinEventHook lazily;
    // we just need to be told when the foreground changes.
    m_brokerToken = ForegroundChangeBroker::Instance().Subscribe(
        [this](HWND hwnd, DWORD pid) { OnForegroundChanged(hwnd, pid); });
}

void ForegroundMonitor::Stop()
{
    if (!m_running) return;
    m_running = false;
    if (m_brokerToken)
    {
        ForegroundChangeBroker::Instance().Unsubscribe(m_brokerToken);
        m_brokerToken = 0;
    }

    // Emit the final session on shutdown so we don't lose the dwell time
    // for the last-foreground app.
    std::lock_guard<std::mutex> lk(m_stateMtx);
    EmitPreviousLocked(GetTickCount64());
    m_prevPid = 0;
    m_prevExeName.clear();
    m_prevExePath.clear();
    m_prevTitle.clear();
    m_prevStartTick = 0;
}

void ForegroundMonitor::Pause()
{
    m_paused = true;
}

void ForegroundMonitor::Resume()
{
    m_paused = false;
}

// Invoked by ForegroundChangeBroker on the main UI thread. Must do minimal
// blocking work; everything here is constant-time apart from the user's
// callback (which is responsible for its own performance).
void ForegroundMonitor::OnForegroundChanged(HWND hwnd, DWORD pid)
{
    if (!m_running || m_paused) return;
    if (!hwnd || pid == 0)      return;

    wchar_t titleBuf[1024] = {};
    int titleLen = GetWindowTextW(hwnd, titleBuf, 1024);
    std::wstring title(titleBuf, titleLen > 0 ? titleLen : 0);

    std::wstring exeName, exePath;
    if (!GetProcessInfo(pid, exeName, exePath))
    {
        // Can't resolve PID -- still emit the prior session so we don't
        // accidentally extend its duration past the actual focus loss.
        std::lock_guard<std::mutex> lk(m_stateMtx);
        EmitPreviousLocked(GetTickCount64());
        m_prevPid = 0;
        m_prevExeName.clear();
        m_prevExePath.clear();
        m_prevTitle.clear();
        m_prevStartTick = 0;
        return;
    }

    std::wstring exeLower = exeName;
    std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(), ::towlower);

    ULONGLONG nowTick = GetTickCount64();
    std::lock_guard<std::mutex> lk(m_stateMtx);

    // Don't suppress the emission of the PREVIOUS session just because the
    // NEW foreground is a system shell -- the user really did spend N
    // seconds in the prior app. Always flush first.
    EmitPreviousLocked(nowTick);

    if (IsSystemForeground(exeLower))
    {
        // Don't track system / shell windows as foreground sessions.
        m_prevPid = 0;
        m_prevExeName.clear();
        m_prevExePath.clear();
        m_prevTitle.clear();
        m_prevStartTick = 0;
        return;
    }

    m_prevPid       = pid;
    m_prevExeName   = exeName;
    m_prevExePath   = exePath;
    m_prevTitle     = title;
    m_prevStartTick = nowTick;
}

void ForegroundMonitor::EmitPreviousLocked(ULONGLONG nowTick)
{
    if (m_prevPid == 0 || m_prevExeName.empty() || m_prevStartTick == 0)
        return;
    int durationSecs = static_cast<int>((nowTick - m_prevStartTick) / 1000);
    if (durationSecs < 1 || !m_callback) return;
    EventContext ctx = EventContextUtil::CaptureContext(m_prevPid);
    m_callback(m_prevExeName, m_prevExePath, m_prevTitle, durationSecs, ctx);
}
