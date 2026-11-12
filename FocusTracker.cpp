#include "framework.h"
#include "FocusTracker.h"
#include "ForegroundChangeBroker.h"
#include "EventContext.h"
#include "DocResolver.h"
#include "DesktopInfo.h"
#include "Logger.h"

#include <objbase.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace
{
    const ULONGLONG kTitleDebounceMs = 2000;    // a title must be stable this long before the session splits
    const DWORD     kIdleThresholdMs = 120000;  // inside a session, no input for > 2 min counts as idle (unless passive presence)
    const int64_t   kMinSegmentMs    = 1000;
    const ULONGLONG kDocRetryMs      = 3000;

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

    std::wstring WindowText(HWND h)
    {
        int n = GetWindowTextLengthW(h);
        if (n <= 0) return std::wstring();
        std::wstring s((size_t)n + 1, L'\0');
        int got = GetWindowTextW(h, &s[0], n + 1);
        s.resize(got > 0 ? (size_t)got : 0);
        return s;
    }

    std::wstring ClassOf(HWND h)
    {
        wchar_t buf[256] = {};
        int n = GetClassNameW(h, buf, 256);
        return std::wstring(buf, n > 0 ? (size_t)n : 0);
    }
}

FocusTracker::FocusTracker()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

FocusTracker::~FocusTracker()
{
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
    if (m_wakeEvent) CloseHandle(m_wakeEvent);
}

int64_t FocusTracker::NowMs()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)((t - 116444736000000000ULL) / 10000ULL);
}

void FocusTracker::SetCallback(Callback cb) { m_callback = std::move(cb); }
void FocusTracker::SetDocResolver(DocResolverFn fn) { std::lock_guard<std::mutex> lk(m_mtx); m_docResolver = std::move(fn); }
void FocusTracker::SetRedaction(RedactFn fn) { std::lock_guard<std::mutex> lk(m_mtx); m_redact = std::move(fn); }

bool FocusTracker::IsShellChrome(const std::wstring& exeLower, const std::wstring& windowClass)
{
    static const wchar_t* const kChrome[] = {
        L"shellexperiencehost.exe", L"startmenuexperiencehost.exe", L"textinputhost.exe", L"lockapp.exe",
        L"logonui.exe", L"dwm.exe", L"csrss.exe", L"taskhostw.exe", L"runtimebroker.exe", L"sihost.exe",
        L"ctfmon.exe", L"widgets.exe", L"widgetservice.exe", L"consent.exe", L"smartscreen.exe",
        L"securityhealthsystray.exe", L"werfault.exe", L"wermgr.exe", L"applicationframehost.exe", L"warp!.exe",
    };
    for (const wchar_t* c : kChrome) if (exeLower == c) return true;
    if (exeLower == L"explorer.exe")
        return windowClass != L"CabinetWClass";     // folder windows are real work (APP-8); taskbar/desktop are not
    return false;
}

void FocusTracker::Start()
{
    if (m_running.exchange(true)) return;
    ResetEvent(m_stopEvent);
    m_brokerToken = ForegroundChangeBroker::Instance().Subscribe([this](HWND h, DWORD p) { OnForegroundChanged(h, p); });
    std::vector<FocusEventRecord> out;
    {
        // Seed with the window that is already in front (no foreground event will arrive for it).
        HWND fg = GetForegroundWindow();
        HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
        if (root && !IsWindowVisible(root)) root = GetAncestor(fg, GA_ROOT);
        DWORD pid = 0;
        if (root) GetWindowThreadProcessId(root, &pid);
        std::lock_guard<std::mutex> lk(m_mtx);
        if (root && pid) BeginLocked(root, pid, NowMs(), out);
    }
    Emit(out);
    m_sampler = std::thread(&FocusTracker::SamplerThread, this);
}

void FocusTracker::Stop()
{
    if (!m_running.exchange(false)) return;
    if (m_brokerToken)
    {
        ForegroundChangeBroker::Instance().Unsubscribe(m_brokerToken);
        m_brokerToken = 0;
    }
    SetEvent(m_stopEvent);
    if (m_sampler.joinable()) m_sampler.join();
    std::vector<FocusEventRecord> out;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        AccountLocked(GetTickCount64());
        EndLocked(NowMs(), out, "shutdown");
    }
    Emit(out);
}

void FocusTracker::Flush()
{
    std::vector<FocusEventRecord> out;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_haveCur) return;
        AccountLocked(GetTickCount64());
        HWND h = m_cur.hwnd;
        DWORD p = m_cur.pid;
        int64_t now = NowMs();
        EndLocked(now, out, "pause");
        if (m_running.load() && !m_paused.load() && !m_away && h && IsWindow(h)) BeginLocked(h, p, now, out);
    }
    Emit(out);
}

void FocusTracker::SetPaused(bool paused)
{
    std::vector<FocusEventRecord> out;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_paused.load() == paused) return;
        m_paused = paused;
        int64_t now = NowMs();
        if (paused)
        {
            AccountLocked(GetTickCount64());
            EndLocked(now, out);
        }
        else if (!m_away && m_running.load())
        {
            HWND fg = GetForegroundWindow();
            HWND root = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
            DWORD pid = 0;
            if (root) GetWindowThreadProcessId(root, &pid);
            if (root && pid) BeginLocked(root, pid, now, out);
        }
    }
    Emit(out);
}

void FocusTracker::OnPresenceChanged(const std::string& state, int64_t lastInputUtcMs)
{
    std::vector<FocusEventRecord> out;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        AccountLocked(GetTickCount64()); // account the elapsed sample using the previous state
        const bool away = state == "away" || state == "locked" || state == "asleep";
        const int64_t now = NowMs();
        if (away && !m_away)
        {
            // Checkpointed foreground exposure is immutable. The detected boundary is
            // not retroactively moved over already-measured recent-input/idle slices.
            EndLocked(now, out, state.c_str());
            m_away = true;
        }
        else if (!away && m_away)
        {
            m_away = false;
            if (!m_paused.load() && m_running.load())
            {
                HWND fg = GetForegroundWindow();
                HWND root = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
                DWORD pid = 0;
                if (root) GetWindowThreadProcessId(root, &pid);
                if (root && pid) BeginLocked(root, pid, now, out);
            }
        }
        m_presence = state;
    }
    Emit(out);
}

void FocusTracker::OnForegroundChanged(HWND hwnd, DWORD pid)
{
    if (!m_running.load() || !hwnd || pid == 0) return;
    std::vector<FocusEventRecord> out;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_paused.load() || m_away) return;
        if (m_haveCur && m_cur.hwnd == hwnd && m_cur.pid == pid && !m_away) return;
        const int64_t now = NowMs();
        AccountLocked(GetTickCount64());
        EndLocked(now, out);
        BeginLocked(hwnd, pid, now, out);
    }
    Emit(out);
}

void FocusTracker::BeginLocked(HWND hwnd, DWORD pid, int64_t startMs, std::vector<FocusEventRecord>&)
{
    m_haveCur = false;
    if (pid == GetCurrentProcessId()) return;   // own windows: never tracked (WM_GETTEXT to our UI thread under m_mtx could deadlock)
    std::wstring exePath = EventContextUtil::GetExePathByPid(pid);
    if (exePath.empty()) return;
    Segment s;
    s.hwnd = hwnd;
    s.pid = pid;
    s.exePath = exePath;
    s.exeName = BaseName(exePath);
    s.windowClass = ClassOf(hwnd);
    if (IsShellChrome(Lower(s.exeName), s.windowClass)) return;
    s.title = DocResolver::NormalizeTitle(WindowText(hwnd));
    s.redaction = m_redact ? m_redact(hwnd, pid, exePath, s.title) : Redaction::None;
    if (s.redaction == Redaction::DropAll) return;
    // AUMID / virtual desktop need COM calls into Explorer. An STA (the UI thread) pumps messages
    // during such a call, so a WinEvent could re-enter OnForegroundChanged and lock m_mtx again on
    // the same thread: the sampler thread fetches them outside the lock. A new segment of the same
    // window (title split, Flush, back from away) keeps what is already known.
    const bool sameWindow = m_cur.hwnd == hwnd && m_cur.pid == pid && !m_cur.shellInfoPending;
    if (sameWindow)
    {
        s.aumid = m_cur.aumid;
        s.desktopId = m_cur.desktopId;
    }
    s.shellInfoPending = !sameWindow;
    if (s.shellInfoPending && m_wakeEvent) SetEvent(m_wakeEvent);   // fetched within ms, not at the next 1 s tick
    s.startMs = startMs;
    s.episodeId = Evidence::NewId("focus");
    s.lastSampleTick = GetTickCount64();
    s.ctx = EventContextUtil::CaptureContext(pid);                    // context at session START (APP-6)
    s.ctx.evidence.timestampKind = "foreground_sample";
    s.ctx.evidence.actorMethod = "foreground_owner";
    s.ctx.evidence.confirmation = "foreground_observed";
    s.ctx.evidence.sourceQuality = 1;
    s.ctx.evidence.actorQuality = 0.9;
    s.ctx.confidence = EventContextUtil::ScoreEvent(s.ctx, EventContextUtil::EventKind::Focus,
                                                    EventContextUtil::SourceClass::Focus);
    m_cur = std::move(s);
    m_haveCur = true;
}

std::string FocusTracker::AttentionState(const std::string& presence, DWORD msSinceInput)
{
    if (presence == "away" || presence == "locked" || presence == "asleep") return "idle";
    if (msSinceInput == 0xFFFFFFFFu) return presence == "passive" ? "passive" : "unknown";
    if (msSinceInput <= kIdleThresholdMs) return "active";
    return presence == "passive" ? "passive" : "idle";
}

void FocusTracker::AccountLocked(ULONGLONG nowTick)
{
    if (!m_haveCur) return;
    if (nowTick <= m_cur.lastSampleTick) return;
    double dt = (double)(nowTick - m_cur.lastSampleTick) / 1000.0;
    m_cur.lastSampleTick = nowTick;
    const std::string state = AttentionState(m_presence, EventContextUtil::MsSinceInput());
    if (state == "active") m_cur.activeSecs += dt;
    else if (state == "passive") m_cur.passiveSecs += dt;
    else if (state == "idle") m_cur.idleSecs += dt;
    else m_cur.unknownSecs += dt;
}

void FocusTracker::ResolveDocLocked(Segment& s)
{
    if (s.redaction != Redaction::None || !m_docResolver) return;
    DocInfo d;
    s.docResolved = m_docResolver(s.hwnd, s.pid, s.exePath, s.title, d) && !d.key.empty();
    if (!s.docResolved) { d.key.clear(); d.display.clear(); d.type.clear(); }
    s.doc = std::move(d);
}

void FocusTracker::EndLocked(int64_t endMs, std::vector<FocusEventRecord>& out, const char* reason)
{
    if (!m_haveCur) return;
    CheckpointLocked(endMs, false, reason, out);
    m_haveCur = false;
}

void FocusTracker::CheckpointLocked(int64_t endMs, bool open, const char* reason, std::vector<FocusEventRecord>& out)
{
    if (!m_haveCur) return;
    Segment& s = m_cur;
    const int64_t dur = endMs - s.startMs;
    if (dur < 0 || (open && dur < kMinSegmentMs)) return;

    FocusEventRecord r;
    r.startMs = s.startMs;
    r.endMs = endMs;
    r.exeName = s.exeName;
    r.exePath = s.exePath;
    r.windowClass = s.windowClass;
    r.desktopId = s.desktopId;
    r.aumid = s.aumid;
    const double wall = (double)dur / 1000.0;
    const int whole = static_cast<int>(wall);
    r.activeSecs = (std::min)(whole, (int)std::lround(s.activeSecs));
    r.passiveSecs = (std::min)(whole - r.activeSecs, (int)std::lround(s.passiveSecs));
    r.idleSecs = (std::min)(whole - r.activeSecs - r.passiveSecs, (int)std::lround(s.idleSecs));
    r.unknownSecs = whole - r.activeSecs - r.passiveSecs - r.idleSecs;
    r.docResolution = s.redaction == Redaction::None ? s.doc.resolution : "withheld";
    if (s.redaction == Redaction::None)
    {
        r.windowTitle = s.title;
        if (!s.doc.key.empty())
        {
            r.docKey = s.doc.key;
            r.docDisplay = s.doc.display;
            r.docType = s.doc.type;
        }
    }
    r.presence = r.passiveSecs > r.activeSecs ? "passive" : "active";
    r.ctx = s.ctx;
    r.ctx.evidence.id = s.episodeId + ":" + std::to_string(++s.revision);
    r.ctx.evidence.episodeId = s.episodeId;
    r.ctx.evidence.revision = s.revision;
    r.ctx.evidence.open = open;
    r.ctx.evidence.boundaryReason = reason;
    r.ctx.evidence.observedMs = NowMs();
    r.ctx.evidence.resolution = r.docResolution;
    r.ctx.evidence.identityQuality = s.doc.confidence;
    r.ctx.evidence.resolutionObservedMs = s.doc.observedMs;
    r.ctx.evidence.resolutionValidUntilMs = s.doc.validUntilMs;
    r.ctx.evidence.windowInstance = s.doc.instance;
    r.ctx.evidence.redaction = s.redaction == Redaction::None ? "none" : "content_withheld";
    out.push_back(std::move(r));
    s.startMs = endMs;
    s.activeSecs = s.passiveSecs = s.idleSecs = s.unknownSecs = 0;
}

void FocusTracker::Emit(std::vector<FocusEventRecord>& out)
{
    if (!m_callback) return;
    for (FocusEventRecord& r : out) m_callback(std::move(r));
    out.clear();
}

void FocusTracker::SamplerThread()
{
    SetThreadDescription(GetCurrentThread(), L"WARP focus sampler");
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);     // MTA: outgoing COM calls never pump messages
    ULONGLONG lastDocAttempt = 0;
    const HANDLE waits[2] = { m_stopEvent, m_wakeEvent };
    for (;;)
    {
        const DWORD w = WaitForMultipleObjects(m_wakeEvent ? 2 : 1, waits, FALSE, 1000);
        if (w != WAIT_TIMEOUT && w != WAIT_OBJECT_0 + 1) break;          // stop requested (or wait failed)
        EventContextUtil::SampleContext();

        // Shell info of the current segment (see BeginLocked): fetched with m_mtx RELEASED and
        // stored only if the same segment is still current.
        HWND shellHwnd = nullptr;
        int64_t shellStartMs = 0;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_haveCur && m_cur.shellInfoPending)
            {
                shellHwnd = m_cur.hwnd;
                shellStartMs = m_cur.startMs;
            }
        }
        if (shellHwnd)
        {
            std::wstring aumid = DesktopInfo::GetAumidForWindow(shellHwnd);
            std::wstring desktopId = DesktopInfo::GetWindowDesktopId(shellHwnd);
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_haveCur && m_cur.shellInfoPending && m_cur.hwnd == shellHwnd && m_cur.startMs == shellStartMs)
            {
                m_cur.aumid = std::move(aumid);
                m_cur.desktopId = std::move(desktopId);
                m_cur.shellInfoPending = false;
            }
        }

        std::vector<FocusEventRecord> out;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_haveCur && !m_paused.load() && !m_away)
            {
                const ULONGLONG now = GetTickCount64();
                AccountLocked(now);
                if (!IsWindow(m_cur.hwnd))
                {
                    EndLocked(NowMs(), out, "window_closed");
                }
                else
                {
                    // APP-5: a stable title change inside the same window starts a new document session.
                    std::wstring t = DocResolver::NormalizeTitle(WindowText(m_cur.hwnd));
                    if (t != m_cur.title)
                    {
                        if (t != m_cur.pendingTitle)
                        {
                            m_cur.pendingTitle = t;
                            m_cur.pendingSinceTick = now;
                        }
                        else if (now - m_cur.pendingSinceTick >= kTitleDebounceMs)
                        {
                            const int64_t splitMs = NowMs() - (int64_t)(now - m_cur.pendingSinceTick);
                            HWND h = m_cur.hwnd;
                            DWORD p = m_cur.pid;
                            EndLocked(splitMs, out, "title_changed");
                            BeginLocked(h, p, splitMs, out);
                        }
                    }
                    else
                    {
                        m_cur.pendingTitle.clear();
                    }
                    // Resolve the document early so the live overlay can carry it.
                    if (m_haveCur && now - lastDocAttempt >= kDocRetryMs)
                    {
                        lastDocAttempt = now;
                        const Redaction redaction = m_redact
                            ? m_redact(m_cur.hwnd, m_cur.pid, m_cur.exePath, t) : Redaction::None;
                        Segment candidate = m_cur;
                        candidate.redaction = redaction;
                        if (redaction == Redaction::None) ResolveDocLocked(candidate);
                        else { candidate.doc = DocInfo{}; candidate.docResolved = false; }
                        if (redaction != m_cur.redaction || candidate.doc.key != m_cur.doc.key ||
                            candidate.doc.resolution != m_cur.doc.resolution || candidate.doc.instance != m_cur.doc.instance ||
                            m_cur.ctx.evidence.processInstance != EventContextUtil::ProcessInstanceId(m_cur.pid))
                        {
                            HWND h = m_cur.hwnd;
                            DWORD p = m_cur.pid;
                            const int64_t split = NowMs();
                            EndLocked(split, out, "identity_or_policy_changed");
                            BeginLocked(h, p, split, out);
                            if (m_haveCur && m_cur.redaction == Redaction::None)
                            {
                                m_cur.doc = std::move(candidate.doc);
                                m_cur.docResolved = candidate.docResolved;
                            }
                        }
                        else
                        {
                            m_cur.doc.observedMs = candidate.doc.observedMs;
                            m_cur.doc.validUntilMs = candidate.doc.validUntilMs;
                            m_cur.doc.confidence = candidate.doc.confidence;
                        }
                    }
                    if (m_haveCur && NowMs() - m_cur.startMs >= Evidence::CheckpointSeconds * 1000)
                        CheckpointLocked(NowMs(), true, "checkpoint", out);
                }
            }
        }
        Emit(out);
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

bool FocusTracker::GetCurrentSession(ActiveSession& out) const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!m_haveCur || m_away || m_paused.load()) return false;      // SIG-2: no fictional dwell while away
    out = ActiveSession{};
    const int64_t now = NowMs();
    out.exeName = m_cur.exeName;
    out.exePath = m_cur.exePath;
    out.windowTitle = m_cur.redaction == Redaction::None ? m_cur.title : std::wstring();
    out.startedAtUtcSecs = m_cur.startMs / 1000;
    out.durationSoFarSecs = (int)((now - m_cur.startMs) / 1000);
    out.activeSecsSoFar = (int)std::lround((std::min)(m_cur.activeSecs, (double)out.durationSoFarSecs));
    out.passiveSecsSoFar = (std::min)(out.durationSoFarSecs - out.activeSecsSoFar, (int)std::lround(m_cur.passiveSecs));
    out.idleSecsSoFar = (std::min)(out.durationSoFarSecs - out.activeSecsSoFar - out.passiveSecsSoFar, (int)std::lround(m_cur.idleSecs));
    out.unknownSecsSoFar = out.durationSoFarSecs - out.activeSecsSoFar - out.passiveSecsSoFar - out.idleSecsSoFar;
    out.aumid = m_cur.aumid;
    out.docResolution = m_cur.redaction == Redaction::None ? m_cur.doc.resolution : "withheld";
    out.episodeId = m_cur.episodeId;
    out.confidence = m_cur.ctx.confidence;
    if (m_cur.redaction == Redaction::None && !m_cur.doc.key.empty())
    {
        out.docKey = m_cur.doc.key;
        out.docDisplay = m_cur.doc.display;
        out.docType = m_cur.doc.type;
    }
    return true;
}
