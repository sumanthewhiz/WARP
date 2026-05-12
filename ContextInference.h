#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

class ActivityDatabase;
class ForegroundMonitor;

// One snapshot in the rolling history of "what is the user doing right now?".
// Each snapshot is a *deterministically composed* one-line summary derived
// from the last 15 minutes of foreground / browsing / app-launch / file
// activity.  No machine-learning model, no fixed bucket list -- the prose
// is generated from the actual document, tab, and application titles
// captured in that window.
struct ContextSnapshot
{
    int64_t       timestamp      = 0;   // When this snapshot was generated (epoch s).
    int64_t       windowStartTs  = 0;   // Inclusive start of the 15-min window (epoch s).
    int64_t       windowEndTs    = 0;   // Inclusive end of the 15-min window (epoch s).
    std::string   oneLiner;             // Human-readable 1-line summary.
    int           activityCount  = 0;   // Number of raw signals considered.
    int           focusSeconds   = 0;   // Total foreground time in the window.
    double        confidence     = 0.0; // [0,1] -- signal-quality, NOT ML confidence.
    int           dominantPct    = 0;   // % of focus seconds spent in the dominant app.
    std::vector<std::string> signalTypes; // {"app_focus","browsing","app_launch","file"}

    // Bounded structured breakdown (top apps by focus seconds, capped at 5).
    struct AppItem
    {
        std::string app;          // Friendly app name e.g. "Visual Studio".
        std::string exe;          // exe basename e.g. "devenv.exe".
        std::string title;        // Cleaned document/tab title.
        int         focusSeconds = 0;
        int         pct          = 0; // % of total focus seconds.
    };
    std::vector<AppItem> items;
};

// =====================================================================
// Dynamic context-inference engine (replaces TopicInference).
//
// Lifecycle
//   * Init(...)  -- always succeeds; no model files required.
//   * Start(db, fg) -- spawns a background thread that wakes every 60 s,
//     reads the last 15 minutes of activity from `db` plus the live
//     foreground session from `fg`, composes a one-liner, and appends
//     it to history when it materially changes (or every 5 min as a
//     heartbeat -- whichever comes first).  The "latest" cache is
//     always refreshed.
//   * Stop()     -- joins the thread.
//
// Query API
//   * GetRecentContext()        -- JSON: {"recent_context": <latest snapshot>}
//   * GetRecentContexts(int n)  -- JSON: {"recent_contexts":[...]} newest first.
//   * ClearHistory()            -- empties history (called on user "Clear").
//
// History cap is 1440 (24 h at one snapshot per minute, with material-
// change dedup the actual count is usually much smaller).
// =====================================================================
class ContextInference
{
public:
    ContextInference();
    ~ContextInference();

    bool Init();
    void Start(ActivityDatabase* db, ForegroundMonitor* fg);
    void Stop();

    void RunOnce();
    std::string GetRecentContext();
    std::string GetRecentContexts(int count);
    void ClearHistory();

private:
    ActivityDatabase*  m_db = nullptr;
    ForegroundMonitor* m_fg = nullptr;
    std::thread        m_thread;
    std::atomic<bool>  m_running{ false };
    HANDLE             m_stopEvent = nullptr;

    std::vector<ContextSnapshot> m_history;
    ContextSnapshot              m_latest;       // Always the most recent compute.
    bool                         m_haveLatest = false;
    std::mutex                   m_mutex;

    void TimerLoop();
    ContextSnapshot ComposeSnapshot();

    // Decide whether `snap` is materially different from the last-appended
    // history entry.  Used to keep `GetRecentContexts` from returning rows
    // of duplicates for a user who is doing one thing for an hour.
    bool ShouldAppendToHistory(const ContextSnapshot& snap) const;

    static std::string SnapshotToJsonObject(const ContextSnapshot& s);
    static std::string WideToUtf8(const std::wstring& w);
    static std::string EscapeJson(const std::string& s);
};
