#include "framework.h"
#include "ClipboardMonitor.h"
#include "EventContext.h"

#include <system_error>
#include <utility>

#pragma comment(lib, "user32.lib")

namespace
{
    const wchar_t   kWindowClass[] = L"WarpClipboardMonitorWnd";
    const ULONGLONG kDebounceMs    = 250;
    const DWORD     kStartupWaitMs = 5000;

    enum FormatKind : unsigned
    {
        KindText  = 1u << 0,
        KindImage = 1u << 1,
        KindFiles = 1u << 2,
        KindHtml  = 1u << 3,
        KindRtf   = 1u << 4,
        KindOther = 1u << 5,
    };

    int64_t NowUnixMs()
    {
        FILETIME ft = {};
        GetSystemTimeAsFileTime(&ft);
        const ULONGLONG ticks = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        return static_cast<int64_t>((ticks - 116444736000000000ULL) / 10000ULL);
    }
}

ClipboardMonitor::~ClipboardMonitor()
{
    Stop();
    if (m_readyEvent)
        CloseHandle(m_readyEvent);
    if (m_stopEvent)
        CloseHandle(m_stopEvent);
}

bool ClipboardMonitor::IsMonitorThread() const
{
    const DWORD id = m_threadId.load();
    return id != 0 && id == GetCurrentThreadId();
}

bool ClipboardMonitor::Start(Callback cb)
{
    if (!cb)
        return false;
    if (IsMonitorThread())
        return WaitForSingleObject(m_stopEvent, 0) != WAIT_OBJECT_0;   // called from our own callback

    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    if (m_thread.joinable())
    {
        if (WaitForSingleObject(m_stopEvent, 0) != WAIT_OBJECT_0)
            return true;                                // already running
        m_thread.join();                                // reap a thread stopped from its callback
    }
    if (!m_stopEvent)
        m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_readyEvent)
        m_readyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_stopEvent || !m_readyEvent)
        return false;

    ResetEvent(m_stopEvent);
    ResetEvent(m_readyEvent);
    m_startResult.store(0);
    m_callback = std::move(cb);
    try
    {
        m_thread = std::thread(&ClipboardMonitor::ThreadMain, this);
    }
    catch (const std::system_error&)
    {
        m_callback = nullptr;
        return false;
    }

    // Window creation and listener registration are local calls; the timeout is only a safety net.
    if (WaitForSingleObject(m_readyEvent, kStartupWaitMs) == WAIT_OBJECT_0 && m_startResult.load() < 0)
    {
        m_thread.join();
        m_callback = nullptr;
        return false;
    }
    return true;
}

void ClipboardMonitor::Stop()
{
    if (IsMonitorThread())
    {
        // Called from the callback: the loop exits once it returns; the next Start()/Stop()
        // on another thread (or the destructor) joins the thread.
        SetEvent(m_stopEvent);
        return;
    }
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    if (!m_thread.joinable())
        return;
    SetEvent(m_stopEvent);
    m_thread.join();
    m_callback = nullptr;
}

void ClipboardMonitor::ThreadMain()
{
    m_threadId.store(GetCurrentThreadId());
    m_pending = false;
    m_pendingSinceTick = 0;
    m_cfHtml = RegisterClipboardFormatW(L"HTML Format");
    m_cfRtf  = RegisterClipboardFormatW(L"Rich Text Format");

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize        = static_cast<UINT>(sizeof(wc));
    wc.lpfnWndProc   = &ClipboardMonitor::WndProc;
    wc.hInstance     = instance;
    wc.lpszClassName = kWindowClass;
    // The class stays registered for the process lifetime: other instances may be using it.
    if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
        m_hwnd = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);

    const bool listening = m_hwnd != nullptr && AddClipboardFormatListener(m_hwnd) != FALSE;
    m_lastSequence = GetClipboardSequenceNumber();      // report changes, not what was there before Start()
    m_startResult.store(listening ? 1 : -1);
    SetEvent(m_readyEvent);

    if (listening)
    {
        try
        {
            RunLoop();
        }
        catch (...)
        {
            // Out of memory: fall through to orderly teardown.
        }
        RemoveClipboardFormatListener(m_hwnd);
    }
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    SetEvent(m_stopEvent);      // an unexpected exit looks like a stop, so Start() can restart cleanly
    m_threadId.store(0);
}

void ClipboardMonitor::RunLoop()
{
    for (;;)
    {
        DWORD timeout = INFINITE;
        if (m_pending)
        {
            const ULONGLONG elapsed = GetTickCount64() - m_pendingSinceTick;
            timeout = (elapsed >= kDebounceMs) ? 0 : static_cast<DWORD>(kDebounceMs - elapsed);
        }

        const DWORD rc = MsgWaitForMultipleObjectsEx(1, &m_stopEvent, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (rc == WAIT_OBJECT_0)
            return;                                     // Stop()
        if (rc == WAIT_OBJECT_0 + 1)
        {
            MSG msg = {};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                    return;
                DispatchMessageW(&msg);
            }
        }
        else if (rc == WAIT_FAILED)
        {
            // Not expected; back off instead of spinning while still honouring Stop().
            if (WaitForSingleObject(m_stopEvent, static_cast<DWORD>(kDebounceMs)) == WAIT_OBJECT_0)
                return;
        }

        if (m_pending && GetTickCount64() - m_pendingSinceTick >= kDebounceMs)
        {
            m_pending = false;
            if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0)
                return;
            try
            {
                EmitIfChanged();
            }
            catch (...)
            {
                // Out of memory building this event: drop it and keep monitoring.
            }
        }
    }
}

LRESULT CALLBACK ClipboardMonitor::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    else if (msg == WM_NCDESTROY)
    {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    }
    else if (msg == WM_CLIPBOARDUPDATE)
    {
        // Only open the debounce window here; everything else happens when it closes.
        ClipboardMonitor* self = reinterpret_cast<ClipboardMonitor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && !self->m_pending)
        {
            self->m_pending = true;
            self->m_pendingSinceTick = GetTickCount64();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ClipboardMonitor::EmitIfChanged()
{
    const DWORD sequence = GetClipboardSequenceNumber();
    if (sequence == 0 || sequence == m_lastSequence)
        return;     // no clipboard access, or nothing new since the last event
    m_lastSequence = sequence;

    ClipboardEvent ev;
    ev.utcMs = NowUnixMs();
    ev.sequence = sequence;
    const HWND owner = GetClipboardOwner();
    if (owner)
        GetWindowThreadProcessId(owner, &ev.ownerPid);
    if (ev.ownerPid != 0)
        ev.ownerExePath = EventContextUtil::GetExePathByPid(ev.ownerPid);
    ev.formats = ReadFormatKinds();

    try
    {
        m_callback(ev);
    }
    catch (...)
    {
        // The callback must not take down the monitor thread.
    }
}

// Maps the available format IDs to coarse kinds. GetUpdatedClipboardFormats needs no
// OpenClipboard and never triggers delayed rendering, so no data is touched.
std::vector<std::wstring> ClipboardMonitor::ReadFormatKinds() const
{
    std::vector<UINT> ids(32);
    UINT count = 0;
    bool ok = false;
    for (int attempt = 0; attempt < 4 && !ok; ++attempt)
    {
        count = 0;
        if (GetUpdatedClipboardFormats(ids.data(), static_cast<UINT>(ids.size()), &count))
            ok = true;
        else if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
            ids.resize(count > ids.size() ? count : ids.size() * 2);
        else
            break;
    }

    unsigned kinds = 0;
    if (ok)
    {
        const UINT n = (count < ids.size()) ? count : static_cast<UINT>(ids.size());
        for (UINT i = 0; i < n; ++i)
        {
            const UINT cf = ids[i];
            switch (cf)
            {
            case CF_UNICODETEXT:
            case CF_TEXT:
            case CF_OEMTEXT:
                kinds |= KindText;
                break;
            case CF_BITMAP:
            case CF_DIB:
            case CF_DIBV5:
                kinds |= KindImage;
                break;
            case CF_HDROP:
                kinds |= KindFiles;
                break;
            case CF_LOCALE:
                break;      // synthesized by the system alongside text; not content of its own
            default:
                if (m_cfHtml != 0 && cf == m_cfHtml)
                    kinds |= KindHtml;
                else if (m_cfRtf != 0 && cf == m_cfRtf)
                    kinds |= KindRtf;
                else
                    kinds |= KindOther;
                break;
            }
        }
    }

    std::vector<std::wstring> result;
    if (kinds & KindText)
        result.emplace_back(L"text");
    if (kinds & KindImage)
        result.emplace_back(L"image");
    if (kinds & KindFiles)
        result.emplace_back(L"files");
    if (kinds & KindHtml)
        result.emplace_back(L"html");
    if (kinds & KindRtf)
        result.emplace_back(L"rtf");
    if (kinds & KindOther)
        result.emplace_back(L"other");
    return result;
}
