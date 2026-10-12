#include "framework.h"
#include "EventContext.h"

#include <psapi.h>
#include <unordered_map>
#include <list>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <iterator>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace
{
    // ---- Bounded LRU PID -> exe-path cache ----
    constexpr size_t    kPidCacheCapacity = 1024;
    constexpr ULONGLONG kNegativeTtlMs    = 5000;    // failed lookups are retried soon (FILE-9)

    struct PidCacheEntry
    {
        DWORD        pid = 0;
        std::wstring exe;
        ULONGLONG    atTick = 0;
    };

    std::mutex                                                          g_pidCacheMutex;
    std::list<PidCacheEntry>                                            g_pidCacheLru;
    std::unordered_map<DWORD, std::list<PidCacheEntry>::iterator>       g_pidCacheIndex;

    // Process table fed by Kernel-Process start/stop (FILE-9, SIG-6).
    constexpr size_t kProcTableCapacity = 16384;
    std::mutex g_procMtx;
    std::unordered_map<DWORD, EventContextUtil::ProcessInfo> g_procs;

    // Cached foreground state (updated by ForegroundChangeBroker).
    std::atomic<HWND>  g_lastForegroundHwnd{ nullptr };
    std::atomic<DWORD> g_lastForegroundPid{ 0 };

    std::wstring ResolveExeViaPsApi(DWORD pid)
    {
        if (pid == 0 || pid == 4) return L"";    // 0 = idle, 4 = System

        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return L"";

        std::vector<wchar_t> buf(MAX_PATH * 2);
        std::wstring out;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            DWORD sz = static_cast<DWORD>(buf.size());
            if (QueryFullProcessImageNameW(h, 0, buf.data(), &sz))
            {
                out.assign(buf.data(), sz);
                break;
            }
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
            buf.resize(buf.size() * 2);
        }
        CloseHandle(h);
        return out;
    }

    // Layout of PROCESS_BASIC_INFORMATION (documented fields named explicitly).
    struct BasicProcessInfo
    {
        LONG      ExitStatus;
        PVOID     PebBaseAddress;
        ULONG_PTR AffinityMask;
        LONG      BasePriority;
        ULONG_PTR UniqueProcessId;
        ULONG_PTR InheritedFromUniqueProcessId;
    };
    typedef LONG (NTAPI* PFN_NtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);

    DWORD ParentViaNtQuery(DWORD pid)
    {
        static PFN_NtQueryInformationProcess fn = []() -> PFN_NtQueryInformationProcess {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            return ntdll ? reinterpret_cast<PFN_NtQueryInformationProcess>(GetProcAddress(ntdll, "NtQueryInformationProcess")) : nullptr;
        }();
        if (!fn) return 0;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return 0;
        BasicProcessInfo pbi = {};
        ULONG len = 0;
        DWORD parent = 0;
        if (fn(h, 0 /* ProcessBasicInformation */, &pbi, sizeof(pbi), &len) >= 0)
            parent = static_cast<DWORD>(pbi.InheritedFromUniqueProcessId);
        CloseHandle(h);
        return parent;
    }
}

namespace EventContextUtil
{
    std::wstring GetExePathByPid(DWORD pid)
    {
        if (pid == 0) return L"";

        {
            // Process table first: survives process exit (late ETW events) and PID reuse.
            std::lock_guard<std::mutex> lk(g_procMtx);
            auto it = g_procs.find(pid);
            if (it != g_procs.end() && !it->second.imagePath.empty() && it->second.exitMs == 0)
                return it->second.imagePath;
        }
        {
            std::lock_guard<std::mutex> lk(g_pidCacheMutex);
            auto it = g_pidCacheIndex.find(pid);
            if (it != g_pidCacheIndex.end())
            {
                bool expiredNegative = it->second->exe.empty() && GetTickCount64() - it->second->atTick > kNegativeTtlMs;
                if (!expiredNegative)
                {
                    g_pidCacheLru.splice(g_pidCacheLru.begin(), g_pidCacheLru, it->second);
                    return it->second->exe;
                }
                g_pidCacheLru.erase(it->second);
                g_pidCacheIndex.erase(it);
            }
        }

        std::wstring exe = ResolveExeViaPsApi(pid);
        if (exe.empty())
        {
            // An exited process: fall back to what the process table remembers.
            std::lock_guard<std::mutex> lk(g_procMtx);
            auto it = g_procs.find(pid);
            if (it != g_procs.end()) exe = it->second.imagePath;
        }

        {
            std::lock_guard<std::mutex> lk(g_pidCacheMutex);
            auto it = g_pidCacheIndex.find(pid);
            if (it != g_pidCacheIndex.end())
            {
                g_pidCacheLru.splice(g_pidCacheLru.begin(), g_pidCacheLru, it->second);
                return it->second->exe;
            }

            PidCacheEntry e{ pid, exe, GetTickCount64() };
            g_pidCacheLru.push_front(e);
            g_pidCacheIndex[pid] = g_pidCacheLru.begin();

            while (g_pidCacheLru.size() > kPidCacheCapacity)
            {
                auto& tail = g_pidCacheLru.back();
                g_pidCacheIndex.erase(tail.pid);
                g_pidCacheLru.pop_back();
            }
        }

        return exe;
    }

    void ForgetPid(DWORD pid)
    {
        if (pid == 0) return;
        std::lock_guard<std::mutex> lk(g_pidCacheMutex);
        auto it = g_pidCacheIndex.find(pid);
        if (it != g_pidCacheIndex.end())
        {
            g_pidCacheLru.erase(it->second);
            g_pidCacheIndex.erase(it);
        }
    }

    DWORD GetParentPid(DWORD pid)
    {
        if (pid == 0) return 0;
        {
            std::lock_guard<std::mutex> lk(g_procMtx);
            auto it = g_procs.find(pid);
            if (it != g_procs.end() && it->second.parentPid != 0) return it->second.parentPid;
        }
        return ParentViaNtQuery(pid);   // O(1); no process snapshot (SIG-6)
    }

    void RegisterProcess(const ProcessInfo& info)
    {
        if (info.pid == 0) return;
        {
            std::lock_guard<std::mutex> lk(g_procMtx);
            if (g_procs.size() >= kProcTableCapacity)
            {
                // Evict exited entries first, then the oldest starts.
                for (auto it = g_procs.begin(); it != g_procs.end();)
                    it = it->second.exitMs != 0 ? g_procs.erase(it) : std::next(it);
                while (g_procs.size() >= kProcTableCapacity)
                {
                    auto oldest = std::min_element(g_procs.begin(), g_procs.end(),
                        [](const std::pair<const DWORD, ProcessInfo>& a, const std::pair<const DWORD, ProcessInfo>& b) { return a.second.startMs < b.second.startMs; });
                    g_procs.erase(oldest);
                }
            }
            g_procs[info.pid] = info;
        }
        ForgetPid(info.pid);            // a recycled PID must not inherit the old image
    }

    void MarkProcessExited(DWORD pid, int64_t exitMs)
    {
        std::lock_guard<std::mutex> lk(g_procMtx);
        auto it = g_procs.find(pid);
        if (it != g_procs.end()) it->second.exitMs = exitMs > 0 ? exitMs : 1;
    }

    bool LookupProcess(DWORD pid, ProcessInfo& out)
    {
        std::lock_guard<std::mutex> lk(g_procMtx);
        auto it = g_procs.find(pid);
        if (it == g_procs.end()) return false;
        out = it->second;
        return true;
    }

    DWORD OwnSessionId()
    {
        static const DWORD s = []() {
            DWORD id = 0;
            ProcessIdToSessionId(GetCurrentProcessId(), &id);
            return id;
        }();
        return s;
    }

    bool IsOwnSession(DWORD sessionId)
    {
        return sessionId == OwnSessionId();
    }

    bool IsOwnSessionPid(DWORD pid)
    {
        if (pid == 0 || pid == 4) return false;
        {
            std::lock_guard<std::mutex> lk(g_procMtx);
            auto it = g_procs.find(pid);
            if (it != g_procs.end() && it->second.sessionId != 0xFFFFFFFFu) return it->second.sessionId == OwnSessionId();
        }
        DWORD s = 0;
        if (!ProcessIdToSessionId(pid, &s)) return false;     // gone and unknown: cannot attribute to this user
        return s == OwnSessionId();
    }

    DWORD MsSinceInput()
    {
        LASTINPUTINFO lii{};
        lii.cbSize = sizeof(lii);
        if (!GetLastInputInfo(&lii)) return 0xFFFFFFFFu;
        DWORD now = GetTickCount();
        return (now >= lii.dwTime) ? (now - lii.dwTime) : 0;
    }

    void NotifyForegroundChanged(HWND hwnd, DWORD pid)
    {
        g_lastForegroundHwnd.store(hwnd, std::memory_order_relaxed);
        g_lastForegroundPid.store(pid,  std::memory_order_relaxed);
    }

    // Tick (in GetTickCount64() units) before which all events are
    // considered low-confidence (system housekeeping after wake).
    // 0 means "no active wake boundary".
    static std::atomic<ULONGLONG> g_wakeBoundaryUntilTick{ 0 };

    void SetWakeBoundary(ULONGLONG untilTickMs)
    {
        g_wakeBoundaryUntilTick.store(untilTickMs, std::memory_order_relaxed);
    }

    EventContext CaptureContext(DWORD sourcePid)
    {
        EventContext ctx;
        ctx.sourcePid = sourcePid;
        if (sourcePid != 0)
            ctx.sourceExe = GetExePathByPid(sourcePid);

        // Foreground: prefer the broker's cached value (works from any thread,
        // including ETW callbacks). Fall back to GetForegroundWindow() when
        // the broker hasn't been initialized yet.
        DWORD fgPid = g_lastForegroundPid.load(std::memory_order_relaxed);
        if (fgPid == 0)
        {
            HWND fg = GetForegroundWindow();
            if (fg)
            {
                GetWindowThreadProcessId(fg, &fgPid);
            }
        }
        ctx.foregroundPid = fgPid;
        if (fgPid != 0)
            ctx.foregroundExe = GetExePathByPid(fgPid);

        ctx.msSinceInput = MsSinceInput();

        // Wake-boundary attenuation: in the seconds after a resume from
        // sleep, the OS unleashes a burst of housekeeping activity
        // (SuperFetch, Defender scan, indexer catch-up, sync clients
        // reconciling). Multiply confidence by 0.2 so these events land in
        // the DB but don't pollute engagement.
        ULONGLONG until = g_wakeBoundaryUntilTick.load(std::memory_order_relaxed);
        if (until != 0)
        {
            ULONGLONG now64 = GetTickCount64();
            if (now64 < until)
                ctx.confidence *= 0.2;
            else
                g_wakeBoundaryUntilTick.store(0, std::memory_order_relaxed);
        }

        return ctx;
    }

    double ScoreEvent(const EventContext& ctx, EventKind kind, SourceClass source, const ScoreHints& hints)
    {
        // 1. Prior by source class: explicit user surfaces > kernel / journal observation.
        double s = 0.7;
        switch (source)
        {
        case SourceClass::Focus:  s = 1.0;  break;
        case SourceClass::Shell:
        case SourceClass::Recent:
        case SourceClass::Mru:    s = 0.95; break;
        case SourceClass::Uia:    s = 0.9;  break;
        case SourceClass::Etw:    s = 0.75; break;
        case SourceClass::Usn:    s = 0.7;  break;
        case SourceClass::Rdcw:   s = 0.6;  break;
        }
        const bool observed = source == SourceClass::Etw || source == SourceClass::Usn ||
                              source == SourceClass::Rdcw || source == SourceClass::Uia;

        // 2. Foreground correlation: the app the user is looking at caused it.
        const bool attributed = ctx.sourcePid != 0;
        const bool fgMatch = attributed && ctx.foregroundPid == ctx.sourcePid;
        const bool fgSameApp = !fgMatch && attributed && !ctx.sourceExe.empty() &&
                               _wcsicmp(ctx.sourceExe.c_str(), ctx.foregroundExe.c_str()) == 0;
        if (fgMatch) s += 0.25;
        else if (fgSameApp) s += 0.2;

        // 3. Input recency (observed sources only; explicit surfaces are user actions by definition).
        if (observed)
        {
            DWORD ms = ctx.msSinceInput;
            double r = ms == 0xFFFFFFFFu ? 0.8 : ms <= 10000 ? 1.0 : ms <= 60000 ? 0.85 : ms <= 300000 ? 0.7 : 0.5;
            s *= r;
        }

        // 4. Attribution and process role.
        if (!attributed && (source == SourceClass::Etw || source == SourceClass::Usn || source == SourceClass::Rdcw))
            s = (std::min)(s, 0.5);
        if (hints.sourceIsSystem)     s *= 0.2;
        if (hints.sourceIsShellInfra) s *= 0.35;
        if (hints.sourceIsSyncClient) s = (std::min)(s, 0.3);   // "external change": keep, but weakly

        // 5. Event kind.
        switch (kind)
        {
        case EventKind::FileOpen:   if (!fgMatch && observed) s *= 0.8; break;
        case EventKind::FileDelete:
        case EventKind::FileRename: s *= 0.9; break;
        case EventKind::Download:   if (!hints.sourceIsSystem) s = (std::max)(s, 0.6); break;
        case EventKind::AppLaunch:  s = hints.userVisibleWindow ? (std::max)(s, 0.85) : s * 0.4; break;
        case EventKind::Focus:      s = 1.0; break;
        default: break;
        }

        s = (std::max)(0.0, (std::min)(1.0, s)) * ctx.confidence;   // multiply, never assign (SIG-4)
        return (std::max)(0.02, (std::min)(1.0, s));
    }
}
