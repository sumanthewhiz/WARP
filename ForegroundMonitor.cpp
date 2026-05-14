#include "framework.h"
#include "ForegroundMonitor.h"
#include "ForegroundChangeBroker.h"
#include "EventContext.h"
#include <algorithm>
#include <ctime>
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

// EnumChildWindows callback: walks the immediate children of an
// ApplicationFrameHost HWND looking for the UWP/packaged-app's
// CoreWindow.  The first one whose PID is *different* from the
// outer host PID is the actual UWP app process (Photos, Camera,
// Calculator, Microsoft Store, etc.).
struct AfhDrillState
{
    DWORD hostPid    = 0;
    DWORD childPid   = 0;
    HWND  childHwnd  = nullptr;
};

static BOOL CALLBACK AfhDrillEnumProc(HWND hwnd, LPARAM lParam)
{
    auto* state = reinterpret_cast<AfhDrillState*>(lParam);

    wchar_t cls[256] = {};
    int n = GetClassNameW(hwnd, cls, 256);
    if (n <= 0) return TRUE;

    // The UWP host window class.  Matches both legacy
    // `Windows.UI.Core.CoreWindow` and the newer
    // `ApplicationFrameInputSinkWindow` sometimes seen on Win11.
    std::wstring cn(cls, n);
    if (cn != L"Windows.UI.Core.CoreWindow" &&
        cn != L"ApplicationFrameInputSinkWindow")
        return TRUE;

    DWORD childPid = 0;
    GetWindowThreadProcessId(hwnd, &childPid);
    if (childPid != 0 && childPid != state->hostPid)
    {
        state->childPid  = childPid;
        state->childHwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

// For ApplicationFrameHost.exe foreground windows, drill into the
// children and find the actual UWP app PID + its CoreWindow HWND.
// Returns true if a real child was found.  Photos, Camera,
// Calculator, the legacy Microsoft Store, etc., all rely on this --
// without it they're indistinguishable from generic system shell
// chrome and would be filtered.
static bool ResolveAppFrameHostChild(HWND hostHwnd,
                                     DWORD hostPid,
                                     HWND& outChildHwnd,
                                     DWORD& outChildPid)
{
    if (!hostHwnd || hostPid == 0) return false;
    AfhDrillState state{};
    state.hostPid = hostPid;
    EnumChildWindows(hostHwnd, AfhDrillEnumProc, reinterpret_cast<LPARAM>(&state));
    if (state.childPid == 0) return false;
    outChildHwnd = state.childHwnd ? state.childHwnd : hostHwnd;
    outChildPid  = state.childPid;
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
    m_prevStartTick    = 0;
    m_prevStartUtcSecs = 0;
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

    // Drill through ApplicationFrameHost to find the real UWP app
    // process (Photos, Camera, Calculator, etc.) -- without this we'd
    // filter them out as system shell chrome and lose all file/image
    // context for the user's Photos / Paint / image-viewer sessions.
    {
        std::wstring hostExe, hostPath;
        if (GetProcessInfo(pid, hostExe, hostPath))
        {
            std::wstring hostExeLower = hostExe;
            std::transform(hostExeLower.begin(), hostExeLower.end(),
                           hostExeLower.begin(), ::towlower);
            if (hostExeLower == L"applicationframehost.exe")
            {
                HWND  childHwnd = nullptr;
                DWORD childPid  = 0;
                if (ResolveAppFrameHostChild(hwnd, pid, childHwnd, childPid))
                {
                    hwnd = childHwnd;
                    pid  = childPid;
                }
            }
        }
    }

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
        m_prevHwnd = nullptr;
        m_prevPid = 0;
        m_prevExeName.clear();
        m_prevExePath.clear();
        m_prevTitle.clear();
        m_prevStartTick    = 0;
        m_prevStartUtcSecs = 0;
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
        m_prevHwnd = nullptr;
        m_prevPid = 0;
        m_prevExeName.clear();
        m_prevExePath.clear();
        m_prevTitle.clear();
        m_prevStartTick    = 0;
        m_prevStartUtcSecs = 0;
        return;
    }

    m_prevHwnd         = hwnd;
    m_prevPid          = pid;
    m_prevExeName      = exeName;
    m_prevExePath      = exePath;
    m_prevTitle        = title;
    m_prevStartTick    = nowTick;
    m_prevStartUtcSecs = static_cast<int64_t>(std::time(nullptr));
}

void ForegroundMonitor::EmitPreviousLocked(ULONGLONG nowTick)
{
    if (m_prevPid == 0 || m_prevExeName.empty() || m_prevStartTick == 0)
        return;
    int durationSecs = static_cast<int>((nowTick - m_prevStartTick) / 1000);
    if (durationSecs < 1 || !m_callback) return;

    // Refresh the title from the live HWND if we still hold a valid
    // handle.  This catches the common case where the user opened a
    // file (Notepad: Ctrl+O), switched tabs (VS Code / Notepad++ /
    // Sublime / Word), or navigated within the same app *without*
    // changing focus -- OnForegroundChanged never fired for those, so
    // m_prevTitle is stale.  Falls back to the captured-at-focus
    // title if the HWND is gone (window closed before focus loss).
    std::wstring titleToEmit = m_prevTitle;
    if (m_prevHwnd && IsWindow(m_prevHwnd))
    {
        wchar_t buf[1024] = {};
        int n = GetWindowTextW(m_prevHwnd, buf, 1024);
        if (n > 0) titleToEmit = std::wstring(buf, n);
    }

    EventContext ctx = EventContextUtil::CaptureContext(m_prevPid);
    m_callback(m_prevExeName, m_prevExePath, titleToEmit, durationSecs, ctx);
}

bool ForegroundMonitor::GetCurrentSession(ActiveSession& out) const
{
    std::lock_guard<std::mutex> lk(m_stateMtx);
    if (m_prevPid == 0 || m_prevExeName.empty() || m_prevStartTick == 0)
        return false;

    out.exeName          = m_prevExeName;
    out.exePath          = m_prevExePath;
    out.windowTitle      = m_prevTitle;

    // Re-read the live window title.  Critical for apps like Notepad,
    // VS Code, Word, Excel, Acrobat, etc., where the user can open a
    // file (or switch tabs) *without* changing focus -- in which case
    // OnForegroundChanged never fires and `m_prevTitle` would be stale.
    // We capture HWND at the last focus change and ask the window for
    // its current title here.  IsWindow guards against stale handles
    // (e.g. the original window was closed and recreated).
    if (m_prevHwnd && IsWindow(m_prevHwnd))
    {
        wchar_t buf[1024] = {};
        int n = GetWindowTextW(m_prevHwnd, buf, 1024);
        if (n > 0)
            out.windowTitle = std::wstring(buf, n);
    }

    out.startedAtUtcSecs = m_prevStartUtcSecs;
    out.durationSoFarSecs =
        static_cast<int>((GetTickCount64() - m_prevStartTick) / 1000);
    return true;
}
