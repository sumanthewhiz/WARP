#include "framework.h"
#include "LaunchCorrelator.h"

#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>

namespace
{
    constexpr DWORD kWindowWaitMs       = 5000;     // allow up to 5s for a window
    constexpr double kConfidenceWithWnd = 1.0;
    constexpr double kConfidenceNoWnd   = 0.3;
    constexpr size_t kMaxPending        = 1024;     // safety cap
}

struct LaunchCorrelator::Pending
{
    std::wstring  exeName;
    std::wstring  exePath;
    DWORD         pid          = 0;
    EventContext  ctx;
    FireCallback  cb;
    ULONGLONG     parkedTick   = 0;
};

struct LaunchCorrelator::Impl
{
    std::mutex                              mtx;
    std::unordered_map<DWORD, Pending>      pending;
    std::thread                             sweeper;
    std::atomic<bool>                       running { false };
    HANDLE                                  stopEvent = nullptr;
};

LaunchCorrelator& LaunchCorrelator::Instance()
{
    static LaunchCorrelator s_instance;
    return s_instance;
}

LaunchCorrelator::LaunchCorrelator()
    : m_impl(new Impl)
{
    m_impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

LaunchCorrelator::~LaunchCorrelator()
{
    Stop();
    if (m_impl)
    {
        if (m_impl->stopEvent) CloseHandle(m_impl->stopEvent);
        delete m_impl;
        m_impl = nullptr;
    }
}

void LaunchCorrelator::Start()
{
    if (m_impl->running.exchange(true)) return;
    ResetEvent(m_impl->stopEvent);
    m_impl->sweeper = std::thread([this]() {
        for (;;)
        {
            DWORD wait = WaitForSingleObject(m_impl->stopEvent, 1000);
            if (wait == WAIT_OBJECT_0) return;

            std::vector<Pending> expired;
            {
                std::lock_guard<std::mutex> lk(m_impl->mtx);
                ULONGLONG now = GetTickCount64();
                for (auto it = m_impl->pending.begin(); it != m_impl->pending.end(); )
                {
                    if (now - it->second.parkedTick >= kWindowWaitMs)
                    {
                        expired.emplace_back(std::move(it->second));
                        it = m_impl->pending.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }

            // Fire timeouts OUTSIDE the lock so consumer callbacks can take
            // their own locks (e.g., DB mutex) without ordering risk.
            for (auto& p : expired)
            {
                EventContext ctx = p.ctx;
                ctx.confidence = kConfidenceNoWnd;
                ctx.createdWindowMs = 0;        // explicit: no window observed
                if (p.cb) p.cb(p.exeName, p.exePath, p.pid, ctx);
            }
        }
    });
}

void LaunchCorrelator::Stop()
{
    if (!m_impl->running.exchange(false)) return;
    SetEvent(m_impl->stopEvent);
    if (m_impl->sweeper.joinable())
        m_impl->sweeper.join();

    // Drain anything still pending. Fire as no-window so the rows still
    // land in the DB for forensic purposes.
    std::vector<Pending> drained;
    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        drained.reserve(m_impl->pending.size());
        for (auto& kv : m_impl->pending) drained.emplace_back(std::move(kv.second));
        m_impl->pending.clear();
    }
    for (auto& p : drained)
    {
        EventContext ctx = p.ctx;
        ctx.confidence = kConfidenceNoWnd;
        ctx.createdWindowMs = 0;
        if (p.cb) p.cb(p.exeName, p.exePath, p.pid, ctx);
    }
}

void LaunchCorrelator::RecordPending(const std::wstring& exeName,
                                     const std::wstring& exePath,
                                     DWORD                pid,
                                     const EventContext&  ctx,
                                     FireCallback         cb)
{
    if (!m_impl->running.load() || pid == 0)
    {
        // Correlator not running yet -- pass through immediately at full
        // confidence so we never silently drop launches during startup.
        if (cb) cb(exeName, exePath, pid, ctx);
        return;
    }

    Pending p;
    p.exeName    = exeName;
    p.exePath    = exePath;
    p.pid        = pid;
    p.ctx        = ctx;
    p.cb         = std::move(cb);
    p.parkedTick = GetTickCount64();

    std::lock_guard<std::mutex> lk(m_impl->mtx);

    // Bound memory: if we've blown past the cap (rare; would indicate a
    // burst of process starts and no window observer running), drop the
    // oldest entry to make room.
    if (m_impl->pending.size() >= kMaxPending)
    {
        auto oldest = m_impl->pending.begin();
        for (auto it = m_impl->pending.begin(); it != m_impl->pending.end(); ++it)
            if (it->second.parkedTick < oldest->second.parkedTick) oldest = it;
        m_impl->pending.erase(oldest);
    }

    // Clobber any previous pending entry for the same PID; the only way
    // to legitimately hit this is rapid PID recycle inside our 5s window,
    // and the latest start should win.
    m_impl->pending[pid] = std::move(p);
}

void LaunchCorrelator::OnWindowCreated(HWND hwnd)
{
    if (!m_impl->running.load() || !hwnd) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return;

    // Only count top-level windows (no parent and not a tool/owner-only
    // child). MDI children, tooltips, IME candidates etc. don't count as
    // "the user can see this".
    if (GetParent(hwnd) != nullptr) return;

    Pending p;
    bool found = false;
    ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lk(m_impl->mtx);
        auto it = m_impl->pending.find(pid);
        if (it != m_impl->pending.end())
        {
            p = std::move(it->second);
            m_impl->pending.erase(it);
            found = true;
        }
    }
    if (!found) return;

    EventContext ctx = p.ctx;
    ctx.confidence      = kConfidenceWithWnd;
    ctx.createdWindowMs = static_cast<DWORD>(now - p.parkedTick);
    if (p.cb) p.cb(p.exeName, p.exePath, p.pid, ctx);
}
