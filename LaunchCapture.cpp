#include "framework.h"
#include "LaunchCapture.h"
#include "EventContext.h"
#include "SystemProcessClassifier.h"
#include "DesktopInfo.h"
#include "Settings.h"
#include "Logger.h"

#include <tdh.h>
#include <appmodel.h>
#include <algorithm>
#include <cwctype>
#include <iterator>
#include <vector>

#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")

namespace
{
    // Microsoft-Windows-Kernel-Process {22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}
    const GUID kKernelProcess = { 0x22FB2CD6, 0x0E7B, 0x422B, { 0xA0, 0xC7, 0x2F, 0xAD, 0x1F, 0xD0, 0xE7, 0x16 } };
    const ULONGLONG kKeywordProcess = 0x10;
    const DWORD kWindowWaitMs = 8000;           // a launch may take a while to paint
    const ULONGLONG kRecentWindowMs = 10000;    // window shown up to 10 s before the (lagging) ETW start
    const size_t kMaxPending = 2048;

    LaunchCapture* g_self = nullptr;

    int64_t FileTimeToUnixMs(ULONGLONG ft)
    {
        if (ft < 116444736000000000ULL) return 0;
        return (int64_t)((ft - 116444736000000000ULL) / 10000ULL);
    }

    int64_t NowMs()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        return FileTimeToUnixMs(((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    }

    std::wstring Lower(std::wstring s)
    {
        for (wchar_t& c : s) c = (wchar_t)std::towlower(c);
        return s;
    }

    std::wstring BaseName(const std::wstring& p)
    {
        size_t s = p.find_last_of(L"\\/");
        return s == std::wstring::npos ? p : p.substr(s + 1);
    }

    // Reads payload properties by NAME (robust across manifest versions, APP-1).
    bool TdhProperty(PEVENT_RECORD ev, const wchar_t* name, std::vector<BYTE>& out)
    {
        PROPERTY_DATA_DESCRIPTOR d = {};
        d.PropertyName = reinterpret_cast<ULONGLONG>(name);
        d.ArrayIndex = ULONG_MAX;
        ULONG size = 0;
        if (TdhGetPropertySize(ev, 0, nullptr, 1, &d, &size) != ERROR_SUCCESS || size == 0) return false;
        out.assign(size, 0);
        return TdhGetProperty(ev, 0, nullptr, 1, &d, size, out.data()) == ERROR_SUCCESS;
    }

    bool TdhUInt32(PEVENT_RECORD ev, const wchar_t* name, DWORD& v)
    {
        std::vector<BYTE> b;
        if (!TdhProperty(ev, name, b)) return false;
        if (b.size() >= 4) { memcpy(&v, b.data(), 4); return true; }
        return false;
    }

    bool TdhUInt64(PEVENT_RECORD ev, const wchar_t* name, ULONGLONG& v)
    {
        std::vector<BYTE> b;
        if (!TdhProperty(ev, name, b)) return false;
        if (b.size() >= 8) { memcpy(&v, b.data(), 8); return true; }
        if (b.size() >= 4) { DWORD d = 0; memcpy(&d, b.data(), 4); v = d; return true; }
        return false;
    }

    bool TdhString(PEVENT_RECORD ev, const wchar_t* name, std::wstring& s)
    {
        std::vector<BYTE> b;
        if (!TdhProperty(ev, name, b) || b.size() < sizeof(wchar_t)) return false;
        const wchar_t* w = reinterpret_cast<const wchar_t*>(b.data());
        size_t n = b.size() / sizeof(wchar_t);
        size_t len = 0;
        while (len < n && w[len] != L'\0') ++len;
        s.assign(w, len);
        return true;
    }

    // Device path ("\Device\HarddiskVolume3\...") -> DOS path ("C:\...").
    std::mutex g_devMtx;
    std::vector<std::pair<std::wstring, std::wstring>> g_devMap;   // device prefix -> drive
    ULONGLONG g_devMapTick = 0;

    void RefreshDeviceMap()
    {
        std::vector<std::pair<std::wstring, std::wstring>> m;
        wchar_t target[1024];
        for (wchar_t d = L'A'; d <= L'Z'; ++d)
        {
            wchar_t drive[3] = { d, L':', 0 };
            if (QueryDosDeviceW(drive, target, (DWORD)(sizeof(target) / sizeof(target[0]))))
                m.emplace_back(target, drive);
        }
        g_devMap.swap(m);
        g_devMapTick = GetTickCount64();
    }

    // Executables that are infrastructure helpers, never "an app the user launched".
    bool IsHelperImage(const std::wstring& nameLower)
    {
        static const wchar_t* const kHelpers[] = {
            L"conhost.exe", L"dllhost.exe", L"runtimebroker.exe", L"backgroundtaskhost.exe", L"taskhostw.exe",
            L"smartscreen.exe", L"werfault.exe", L"wermgr.exe", L"crashpad_handler.exe", L"msedgewebview2.exe",
            L"searchprotocolhost.exe", L"searchfilterhost.exe", L"audiodg.exe", L"fontdrvhost.exe", L"ctfmon.exe",
            L"sihost.exe", L"textinputhost.exe", L"dashost.exe", L"consent.exe", L"elevation_service.exe",
            L"identity_helper.exe", L"notification_helper.exe", L"wmiprvse.exe", L"warp!.exe",
        };
        for (const wchar_t* h : kHelpers) if (nameLower == h) return true;
        return false;
    }
}

std::wstring LaunchCapture::DevicePathToDos(const std::wstring& devicePath)
{
    if (devicePath.empty()) return devicePath;
    if (devicePath.size() > 2 && devicePath[1] == L':') return devicePath;                 // already DOS
    static const std::wstring kMup = L"\\Device\\Mup\\";
    if (devicePath.size() > kMup.size() && _wcsnicmp(devicePath.c_str(), kMup.c_str(), kMup.size()) == 0)
        return L"\\\\" + devicePath.substr(kMup.size());
    std::lock_guard<std::mutex> lk(g_devMtx);
    for (int pass = 0; pass < 2; ++pass)
    {
        if (g_devMap.empty() || (pass == 1 && GetTickCount64() - g_devMapTick > 30000)) RefreshDeviceMap();
        for (const auto& kv : g_devMap)
        {
            const std::wstring& dev = kv.first;
            if (devicePath.size() >= dev.size() && _wcsnicmp(devicePath.c_str(), dev.c_str(), dev.size()) == 0 &&
                (devicePath.size() == dev.size() || devicePath[dev.size()] == L'\\'))
                return kv.second + devicePath.substr(dev.size());
        }
    }
    return devicePath;
}

bool LaunchCapture::IsUserFacingWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return false;
    if (DesktopInfo::IsWindowCloaked(hwnd)) return false;
    RECT rc = {};
    if (!GetWindowRect(hwnd, &rc) || (rc.right - rc.left) < 80 || (rc.bottom - rc.top) < 40) return false;
    return true;
}

LaunchCapture::LaunchCapture()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

LaunchCapture::~LaunchCapture()
{
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

void LaunchCapture::SetCallbacks(LaunchCallback onLaunch, ExitCallback onExit)
{
    m_onLaunch = std::move(onLaunch);
    m_onExit = std::move(onExit);
}

void LaunchCapture::SetPaused(bool paused) { m_paused = paused; }

LaunchCapture::Stats LaunchCapture::GetStats() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    Stats s = m_stats;
    s.etwRunning = m_etwAlive.load();
    s.sessionName = m_sessionName;
    return s;
}

bool LaunchCapture::Start()
{
    if (m_running.exchange(true)) return true;
    ResetEvent(m_stopEvent);
    g_self = this;
    m_sessionName = L"WARP-Process-" + std::to_wstring(EventContextUtil::OwnSessionId());
    m_sweeper = std::thread(&LaunchCapture::SweeperThread, this);
    m_etwExited = false;
    m_etwThread = std::thread(&LaunchCapture::EtwThread, this);
    return true;
}

void LaunchCapture::Stop()
{
    if (!m_running.exchange(false)) return;
    SetEvent(m_stopEvent);
    StopSession();
    if (m_etwThread.joinable()) m_etwThread.join();
    if (m_sweeper.joinable()) m_sweeper.join();

    // Parked launches still land (without a window) so nothing is silently lost on shutdown.
    std::vector<Pending> drained;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto& kv : m_pending) drained.push_back(std::move(kv.second));
        m_pending.clear();
    }
    for (Pending& p : drained) Finalize(std::move(p), false, 0);
    g_self = nullptr;
}

bool LaunchCapture::StartSession()
{
    const size_t nameBytes = (m_sessionName.size() + 1) * sizeof(wchar_t);
    const size_t bufSize = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes + 64;
    std::vector<BYTE> buf(bufSize, 0);
    auto init = [&]() -> EVENT_TRACE_PROPERTIES*
    {
        std::fill(buf.begin(), buf.end(), (BYTE)0);
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
        p->Wnode.BufferSize = (ULONG)bufSize;
        p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        p->Wnode.ClientContext = 1;                       // QPC; ProcessTrace converts to FILETIME
        p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        p->BufferSize = 64;
        p->MinimumBuffers = 4;
        p->MaximumBuffers = 16;
        p->FlushTimer = 1;
        return p;
    };
    EVENT_TRACE_PROPERTIES* props = init();
    TRACEHANDLE session = 0;
    ULONG st = StartTraceW(&session, m_sessionName.c_str(), props);
    if (st == ERROR_ALREADY_EXISTS)
    {
        // Only our own (per-session) name can collide: a previous run of WARP in THIS session.
        props = init();
        ControlTraceW(0, m_sessionName.c_str(), props, EVENT_TRACE_CONTROL_STOP);
        props = init();
        st = StartTraceW(&session, m_sessionName.c_str(), props);
    }
    if (st != ERROR_SUCCESS)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stats.lastError = L"StartTrace failed: " + std::to_wstring(st);
        WARP_LOG_ERROR("launch", "StartTrace(%S) failed: %lu", m_sessionName.c_str(), st);
        return false;
    }
    m_session = session;
    st = EnableTraceEx2(session, &kKernelProcess, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION,
                        kKeywordProcess, 0, 0, nullptr);
    if (st != ERROR_SUCCESS)
    {
        WARP_LOG_ERROR("launch", "EnableTraceEx2(Kernel-Process) failed: %lu", st);
        StopSession();
        return false;
    }
    return true;
}

void LaunchCapture::StopSession()
{
    TRACEHANDLE t = m_trace.exchange(INVALID_PROCESSTRACE_HANDLE);
    if (t != INVALID_PROCESSTRACE_HANDLE) CloseTrace(t);
    const size_t bufSize = sizeof(EVENT_TRACE_PROPERTIES) + (m_sessionName.size() + 1) * sizeof(wchar_t) + 64;
    std::vector<BYTE> buf(bufSize, 0);
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    p->Wnode.BufferSize = (ULONG)bufSize;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    if (!m_sessionName.empty()) ControlTraceW(0, m_sessionName.c_str(), p, EVENT_TRACE_CONTROL_STOP);
    m_session = 0;
}

void LaunchCapture::EtwThread()
{
    struct ExitFlag { std::atomic<bool>& f; ~ExitFlag() { f = true; } } exitFlag{ m_etwExited };
    SetThreadDescription(GetCurrentThread(), L"WARP launch ETW");
    if (!StartSession()) return;
    EVENT_TRACE_LOGFILEW lf = {};
    lf.LoggerName = const_cast<LPWSTR>(m_sessionName.c_str());
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = &LaunchCapture::EventCallback;
    TRACEHANDLE t = OpenTraceW(&lf);
    if (t == INVALID_PROCESSTRACE_HANDLE)
    {
        WARP_LOG_ERROR("launch", "OpenTrace failed: %lu", GetLastError());
        StopSession();
        return;
    }
    m_trace = t;
    if (!m_running.load())
    {
        StopSession();          // Stop() ran before the handle was published: ProcessTrace would never return
        return;
    }
    m_etwAlive = true;
    WARP_LOG_INFO("launch", "process capture running (%S)", m_sessionName.c_str());
    ULONG st = ProcessTrace(&t, 1, nullptr, nullptr);
    m_etwAlive = false;
    if (m_running.load())
        WARP_LOG_WARN("launch", "ProcessTrace returned unexpectedly (%lu); the supervisor will restart it", st);
    TRACEHANDLE cur = m_trace.exchange(INVALID_PROCESSTRACE_HANDLE);
    if (cur != INVALID_PROCESSTRACE_HANDLE) CloseTrace(cur);
}

void LaunchCapture::Heartbeat()
{
    if (!m_running.load()) return;
    // Only a thread that has returned is joined: one still starting (not yet alive) would block
    // the UI thread until shutdown.
    if (!m_etwAlive.load() && m_etwExited.load() && GetTickCount64() >= m_nextRestartTick)
    {
        if (m_etwThread.joinable()) m_etwThread.join();
        m_nextRestartTick = GetTickCount64() + (ULONGLONG)m_restartBackoffSec * 1000;
        m_restartBackoffSec = (std::min)(m_restartBackoffSec * 2, 300);
        WARP_LOG_INFO("launch", "restarting process capture");
        m_etwExited = false;
        m_etwThread = std::thread(&LaunchCapture::EtwThread, this);
        return;
    }
    if (m_etwAlive.load())
    {
        m_restartBackoffSec = 5;
        const size_t bufSize = sizeof(EVENT_TRACE_PROPERTIES) + (m_sessionName.size() + 1) * sizeof(wchar_t) + 64;
        std::vector<BYTE> buf(bufSize, 0);
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
        p->Wnode.BufferSize = (ULONG)bufSize;
        p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        if (ControlTraceW(0, m_sessionName.c_str(), p, EVENT_TRACE_CONTROL_QUERY) == ERROR_SUCCESS)
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stats.eventsLost = p->EventsLost + p->RealTimeBuffersLost;
        }
    }
}

void WINAPI LaunchCapture::EventCallback(PEVENT_RECORD ev)
{
    LaunchCapture* self = g_self;
    if (!self || !ev || !self->m_running.load()) return;
    if (!IsEqualGUID(ev->EventHeader.ProviderId, kKernelProcess)) return;
    switch (ev->EventHeader.EventDescriptor.Id)
    {
    case 1: self->OnProcessStart(ev); break;
    case 2: self->OnProcessStop(ev); break;
    default: break;
    }
}

void LaunchCapture::OnProcessStart(PEVENT_RECORD ev)
{
    DWORD pid = 0, parent = 0, session = 0xFFFFFFFFu;
    ULONGLONG createTime = 0;
    std::wstring image, packageFullName, relativeAppId;
    if (!TdhUInt32(ev, L"ProcessID", pid) || pid == 0) return;
    TdhUInt32(ev, L"ParentProcessID", parent);
    TdhUInt32(ev, L"SessionID", session);
    TdhUInt64(ev, L"CreateTime", createTime);
    TdhString(ev, L"ImageName", image);
    TdhString(ev, L"PackageFullName", packageFullName);
    TdhString(ev, L"PackageRelativeAppId", relativeAppId);

    const int64_t eventMs = FileTimeToUnixMs((ULONGLONG)ev->EventHeader.TimeStamp.QuadPart);
    std::wstring exePath = DevicePathToDos(image);
    if (exePath.empty()) exePath = EventContextUtil::GetExePathByPid(pid);

    EventContextUtil::ProcessInfo pi;
    pi.pid = pid;
    pi.parentPid = parent;
    pi.sessionId = session;
    pi.startKey = createTime;
    pi.startMs = createTime ? FileTimeToUnixMs(createTime) : eventMs;
    pi.imagePath = exePath;
    EventContextUtil::RegisterProcess(pi);           // always: file attribution needs it

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        ++m_stats.starts;
        m_stats.lastEventMs = eventMs;
    }
    if (pid == GetCurrentProcessId() || exePath.empty()) return;
    if (session == 0xFFFFFFFFu ? !EventContextUtil::IsOwnSessionPid(pid) : !EventContextUtil::IsOwnSession(session))
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        ++m_stats.otherSession;
        return;
    }
    if (m_paused.load() || Settings::Instance().IsPaused(eventMs / 1000) ||
        !Settings::Instance().Get().captureAppLaunch) return;

    const std::wstring exeName = BaseName(exePath);
    const std::wstring nameLower = Lower(exeName);
    std::wstring parentPath = EventContextUtil::GetExePathByPid(parent);
    if (IsHelperImage(nameLower) || (!parentPath.empty() && _wcsicmp(parentPath.c_str(), exePath.c_str()) == 0))
    {
        // Chromium/Electron renderers, VS Code helpers, COM surrogates...: part of an app already running.
        std::lock_guard<std::mutex> lk(m_mtx);
        ++m_stats.helpers;
        return;
    }
    if (WARP::SystemProcessClassifier::IsHardBlocked(nameLower) ||
        WARP::SystemProcessClassifier::Instance().Classify(exePath, pid).isSystem ||
        Settings::Instance().IsExcludedApp(exePath))
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        ++m_stats.system;
        return;
    }

    Pending p;
    p.rec.eventMs = eventMs;
    p.rec.exeName = exeName;
    p.rec.exePath = exePath;
    p.rec.pid = pid;
    p.rec.creatorPid = ev->EventHeader.ProcessId;             // the HEADER pid is the creator (APP-1)
    p.rec.creatorExe = EventContextUtil::GetExePathByPid(p.rec.creatorPid);
    p.rec.sessionId = session == 0xFFFFFFFFu ? EventContextUtil::OwnSessionId() : session;
    if (!packageFullName.empty() && !relativeAppId.empty())
    {
        wchar_t family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1] = {};
        UINT32 len = (UINT32)(sizeof(family) / sizeof(family[0]));
        if (PackageFamilyNameFromFullName(packageFullName.c_str(), &len, family) == ERROR_SUCCESS)
            p.rec.aumid = std::wstring(family) + L"!" + relativeAppId;
    }
    p.rec.ctx = EventContextUtil::CaptureContextAt(pid, eventMs);
    p.rec.ctx.evidence.timestampKind = "process_creation";
    p.rec.ctx.evidence.actorMethod = "process_creation_not_gesture";
    p.rec.ctx.evidence.confirmation = "process_created";
    p.rec.ctx.evidence.resolution = "process";
    p.rec.ctx.evidence.sourceQuality = 1;
    p.rec.ctx.evidence.actorQuality = 0.3;
    p.rec.ctx.evidence.identityQuality = 1;
    p.rec.ctx.parentPid = parent;
    p.rec.ctx.parentExe = parentPath;
    p.parkedTick = GetTickCount64();
    p.startTick = p.parkedTick;

    // APP-2: the window may already exist (fast apps paint before ETW delivers the start).
    bool shown = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_recentWindows.find(pid);
        shown = it != m_recentWindows.end() && GetTickCount64() - it->second <= kRecentWindowMs;
    }
    if (shown || HasUserFacingWindowNow(pid))
    {
        p.rec.ctx.evidence.confirmation = "window_visible";
        Finalize(std::move(p), true, 0);
        return;
    }
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_pending.size() >= kMaxPending)
    {
        auto oldest = std::min_element(m_pending.begin(), m_pending.end(),
            [](const std::pair<const DWORD, Pending>& a, const std::pair<const DWORD, Pending>& b) { return a.second.parkedTick < b.second.parkedTick; });
        m_pending.erase(oldest);
    }
    m_pending[pid] = std::move(p);
}

void LaunchCapture::OnProcessStop(PEVENT_RECORD ev)
{
    DWORD pid = 0;
    if (!TdhUInt32(ev, L"ProcessID", pid) || pid == 0) return;
    const int64_t eventMs = FileTimeToUnixMs((ULONGLONG)ev->EventHeader.TimeStamp.QuadPart);
    EventContextUtil::MarkProcessExited(pid, eventMs);
    EventContextUtil::ForgetPid(pid);

    int64_t startMs = 0;
    bool recorded = false;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_recorded.find(pid);
        if (it != m_recorded.end())
        {
            startMs = it->second;
            recorded = true;
            m_recorded.erase(it);
            ++m_stats.exits;
        }
        m_recentWindows.erase(pid);
    }
    if (recorded && m_onExit)
    {
        ProcessExitRecord r;
        r.eventMs = eventMs;
        r.pid = pid;
        r.startMs = startMs;
        m_onExit(std::move(r));
    }
}

bool LaunchCapture::HasUserFacingWindowNow(DWORD pid) const
{
    struct Ctx { DWORD pid; bool found; } c = { pid, false };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL
    {
        Ctx* x = reinterpret_cast<Ctx*>(lp);
        DWORD wp = 0;
        GetWindowThreadProcessId(h, &wp);
        if (wp == x->pid && LaunchCapture::IsUserFacingWindow(h)) { x->found = true; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&c));
    return c.found;
}

void LaunchCapture::OnWindowShown(HWND hwnd)
{
    if (!m_running.load() || !IsUserFacingWindow(hwnd)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return;
    Pending p;
    bool found = false;
    ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_recentWindows[pid] = now;
        if (m_recentWindows.size() > 4096)
            for (auto it = m_recentWindows.begin(); it != m_recentWindows.end();)
                it = now - it->second > kRecentWindowMs ? m_recentWindows.erase(it) : std::next(it);
        auto it = m_pending.find(pid);
        if (it != m_pending.end())
        {
            p = std::move(it->second);
            m_pending.erase(it);
            found = true;
        }
    }
    if (found)
    {
        p.rec.ctx.evidence.confirmation = "window_visible";
        Finalize(std::move(p), true, (DWORD)(now - p.startTick));
    }
}

void LaunchCapture::SweeperThread()
{
    SetThreadDescription(GetCurrentThread(), L"WARP launch correlator");
    while (WaitForSingleObject(m_stopEvent, 500) == WAIT_TIMEOUT)
    {
        std::vector<Pending> expired;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            ULONGLONG now = GetTickCount64();
            for (auto it = m_pending.begin(); it != m_pending.end();)
            {
                if (now - it->second.parkedTick >= kWindowWaitMs)
                {
                    expired.push_back(std::move(it->second));
                    it = m_pending.erase(it);
                }
                else ++it;
            }
        }
        for (Pending& p : expired)
        {
            // Last chance: a window that was already visible but never re-shown.
            bool win = HasUserFacingWindowNow(p.rec.pid);
            Finalize(std::move(p), win, win ? kWindowWaitMs : 0);
        }
    }
}

void LaunchCapture::Finalize(Pending&& p, bool sawWindow, DWORD windowDelayMs)
{
    if (m_paused.load() || !Settings::Instance().Get().captureAppLaunch || Settings::Instance().IsPaused(StoreUtil::NowUtc())) return;
    LaunchEventRecord rec = std::move(p.rec);
    if (rec.aumid.empty()) rec.aumid = DesktopInfo::GetAumidForProcess(rec.pid);
    rec.ctx.createdWindowMs = sawWindow ? (std::max<DWORD>)(1, windowDelayMs) : 0;
    EventContextUtil::ScoreHints hints;
    hints.userVisibleWindow = sawWindow;
    rec.ctx.confidence = EventContextUtil::ScoreEvent(rec.ctx, EventContextUtil::EventKind::AppLaunch,
                                                      EventContextUtil::SourceClass::Etw, hints);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        ++m_stats.recorded;
        sawWindow ? ++m_stats.withWindow : ++m_stats.noWindow;
        m_recorded[rec.pid] = rec.eventMs;
        if (m_recorded.size() > 8192) m_recorded.clear();
    }
    if (m_onLaunch) m_onLaunch(std::move(rec));
}
