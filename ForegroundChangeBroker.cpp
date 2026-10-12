#include "framework.h"
#include "ForegroundChangeBroker.h"
#include "EventContext.h"

#include <string>

ForegroundChangeBroker& ForegroundChangeBroker::Instance()
{
    static ForegroundChangeBroker inst;
    return inst;
}

void ForegroundChangeBroker::Start()
{
    if (m_hook) return;
    m_hook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr,
        &ForegroundChangeBroker::WinEventProc,
        0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void ForegroundChangeBroker::Stop()
{
    if (m_hook)
    {
        UnhookWinEvent(m_hook);
        m_hook = nullptr;
    }
    std::lock_guard<std::mutex> lk(m_mtx);
    m_listeners.clear();
}

size_t ForegroundChangeBroker::Subscribe(Listener cb)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    size_t tok = m_nextToken++;
    m_listeners.emplace_back(tok, std::move(cb));
    return tok;
}

void ForegroundChangeBroker::Unsubscribe(size_t token)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    for (auto it = m_listeners.begin(); it != m_listeners.end(); ++it)
    {
        if (it->first == token) { m_listeners.erase(it); return; }
    }
}

void CALLBACK ForegroundChangeBroker::WinEventProc(
    HWINEVENTHOOK, DWORD event, HWND hwnd,
    LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    if (idObject != OBJID_WINDOW)         return;
    if (idChild != CHILDID_SELF)          return;
    if (!hwnd)                            return;
    Instance().Dispatch(hwnd);
}

namespace
{
    struct AfhDrillState
    {
        DWORD hostPid  = 0;
        DWORD childPid = 0;
    };

    BOOL CALLBACK AfhDrillEnumProc(HWND hwnd, LPARAM lParam)
    {
        auto* state = reinterpret_cast<AfhDrillState*>(lParam);
        wchar_t cls[256] = {};
        int n = GetClassNameW(hwnd, cls, 256);
        if (n <= 0) return TRUE;
        std::wstring cn(cls, (size_t)n);
        if (cn != L"Windows.UI.Core.CoreWindow" && cn != L"ApplicationFrameInputSinkWindow") return TRUE;
        DWORD childPid = 0;
        GetWindowThreadProcessId(hwnd, &childPid);
        if (childPid != 0 && childPid != state->hostPid)
        {
            state->childPid = childPid;
            return FALSE;
        }
        return TRUE;
    }

    bool IsAppFrameHost(DWORD pid)
    {
        std::wstring exe = EventContextUtil::GetExePathByPid(pid);
        size_t s = exe.find_last_of(L"\\/");
        std::wstring name = s == std::wstring::npos ? exe : exe.substr(s + 1);
        return _wcsicmp(name.c_str(), L"applicationframehost.exe") == 0;
    }
}

void ForegroundChangeBroker::Dispatch(HWND hwnd)
{
    // Owned dialogs ("Save As", "Find") belong to their app's main window (APP-5).
    HWND root = GetAncestor(hwnd, GA_ROOTOWNER);
    if (root && root != hwnd && IsWindowVisible(root)) hwnd = root;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return;

    // UWP / packaged apps are hosted by ApplicationFrameHost: publish the real app process so
    // no subscriber mistakes Settings, Photos or Calculator for a browser or for shell chrome
    // (WEB-3). The frame window keeps the title, so the HWND stays the frame.
    if (IsAppFrameHost(pid))
    {
        AfhDrillState st{};
        st.hostPid = pid;
        EnumChildWindows(hwnd, AfhDrillEnumProc, reinterpret_cast<LPARAM>(&st));
        if (st.childPid != 0) pid = st.childPid;
    }

    // Update the global "current foreground" so EventContext capture from
    // worker threads (ETW callbacks, etc.) doesn't have to call
    // GetForegroundWindow() and risk racing with focus changes.
    EventContextUtil::NotifyForegroundChanged(hwnd, pid);

    std::vector<std::pair<size_t, Listener>> snapshot;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        snapshot = m_listeners;   // copy under lock so dispatch is unlocked
    }
    for (auto& l : snapshot) l.second(hwnd, pid);
}
