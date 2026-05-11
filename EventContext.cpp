#include "framework.h"
#include "EventContext.h"

#include <psapi.h>
#include <tlhelp32.h>
#include <unordered_map>
#include <list>
#include <mutex>
#include <atomic>

#pragma comment(lib, "psapi.lib")

namespace
{
    // ---- Bounded LRU PID -> exe-path cache ----
    constexpr size_t kPidCacheCapacity = 1024;

    struct PidCacheEntry
    {
        DWORD        pid = 0;
        std::wstring exe;
    };

    std::mutex                                                          g_pidCacheMutex;
    std::list<PidCacheEntry>                                            g_pidCacheLru;
    std::unordered_map<DWORD, std::list<PidCacheEntry>::iterator>       g_pidCacheIndex;

    // Cached foreground state (updated by ForegroundChangeBroker).
    std::atomic<HWND>  g_lastForegroundHwnd{ nullptr };
    std::atomic<DWORD> g_lastForegroundPid{ 0 };

    std::wstring ResolveExeViaPsApi(DWORD pid)
    {
        if (pid == 0 || pid == 4) return L"";    // 0 = idle, 4 = System

        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) return L"";

        wchar_t buf[MAX_PATH * 2] = {};
        DWORD   sz = static_cast<DWORD>(_countof(buf));
        std::wstring out;
        if (QueryFullProcessImageNameW(h, 0, buf, &sz))
        {
            out.assign(buf, sz);
        }
        CloseHandle(h);
        return out;
    }
}

namespace EventContextUtil
{
    std::wstring GetExePathByPid(DWORD pid)
    {
        if (pid == 0) return L"";

        {
            std::lock_guard<std::mutex> lk(g_pidCacheMutex);
            auto it = g_pidCacheIndex.find(pid);
            if (it != g_pidCacheIndex.end())
            {
                // Promote to MRU
                g_pidCacheLru.splice(g_pidCacheLru.begin(), g_pidCacheLru, it->second);
                return it->second->exe;
            }
        }

        std::wstring exe = ResolveExeViaPsApi(pid);

        {
            std::lock_guard<std::mutex> lk(g_pidCacheMutex);
            // Re-check after acquiring the lock
            auto it = g_pidCacheIndex.find(pid);
            if (it != g_pidCacheIndex.end())
            {
                g_pidCacheLru.splice(g_pidCacheLru.begin(), g_pidCacheLru, it->second);
                return it->second->exe;
            }

            PidCacheEntry e{ pid, exe };
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

    // GetParentPid: walks Toolhelp32 snapshot once. It would be faster to use
    // NtQueryInformationProcess(ProcessBasicInformation) but that requires
    // pulling in winternl.h and an undocumented signature; for now the
    // Toolhelp path is acceptable because callers typically request parent
    // PID once per process-start event (not per file event).
    DWORD GetParentPid(DWORD pid)
    {
        if (pid == 0) return 0;

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        DWORD parent = 0;

        if (Process32FirstW(snap, &pe))
        {
            do
            {
                if (pe.th32ProcessID == pid)
                {
                    parent = pe.th32ParentProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return parent;
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

        // ms-since-last-input. LASTINPUTINFO uses GetTickCount, so
        // we measure dwTime against GetTickCount() on the current thread.
        LASTINPUTINFO lii{};
        lii.cbSize = sizeof(lii);
        if (GetLastInputInfo(&lii))
        {
            DWORD now = GetTickCount();
            ctx.msSinceInput = (now >= lii.dwTime) ? (now - lii.dwTime) : 0;
        }
        else
        {
            ctx.msSinceInput = 0xFFFFFFFFu;
        }

        // Wake-boundary attenuation: in the seconds after a resume from
        // sleep / long-idle, the OS unleashes a burst of housekeeping
        // activity (SuperFetch, Defender scan, indexer catch-up, sync
        // clients reconciling). Multiply confidence by 0.2 so these
        // events land in the DB but don't pollute popularity counts.
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
}
