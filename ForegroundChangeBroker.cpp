#include "framework.h"
#include "ForegroundChangeBroker.h"
#include "EventContext.h"

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

void ForegroundChangeBroker::Dispatch(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return;

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
