#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

#include "BertTokenizer.h"

// Forward-declare the ONNX Runtime types so callers don't need to pull the
// header into every translation unit.  All access lives behind opaque pointers.
namespace Ort {
    struct Env;
    struct Session;
    struct SessionOptions;
    struct MemoryInfo;
}

class ActivityDatabase;
class ForegroundMonitor;

// One snapshot in the rolling history of "what is the user doing right now?".
// Each snapshot is composed from the last 15 minutes of foreground / browsing /
// app-launch / file activity.  The literal one-liner text is built from the
// actual document, tab, and application titles captured in the window.
//
// When the optional sentence-encoder embedding model is available the
// composer additionally runs **dynamic semantic clustering** on the per-app
// descriptions: apps whose embeddings are close in the 384-dim sentence space
// are merged into one "thread of work" so the one-liner can surface that the
// user has, e.g., the auth source file open in VS Code AND the Auth PR open in
// Edge as a single thread instead of two unrelated lines.  The clusters are
// formed *dynamically from the observed titles* -- there is no fixed bucket
// list or pre-defined topic taxonomy.  Default model is
// `BAAI/bge-small-en-v1.5` (`bge-small.onnx`); legacy installations with
// `minilm.onnx` (all-MiniLM-L6-v2) continue to work transparently.
//
// When the model files are absent the composer falls back to a deterministic
// per-app composition (every app is its own cluster).
struct ContextSnapshot
{
    int64_t       timestamp      = 0;   // When this snapshot was generated (epoch s).
    int64_t       windowStartTs  = 0;   // Inclusive start of the lookback window (epoch s).
    int64_t       windowEndTs    = 0;   // Inclusive end of the lookback window (epoch s).
    int64_t       windowSeconds  = 0;   // Width of the lookback window (= windowEndTs - windowStartTs).
    std::string   oneLiner;             // Combined "All" one-liner summary.
    // Category-specific one-liners.  Each is composed independently from a
    // *projection* of the same activity window so a consumer can ask
    // "what files has the user been in?", "what websites?", or "what
    // apps?" without having to mentally separate them out of the combined
    // line.  All three use the same dynamic-clustering + theme-distillation
    // pipeline as the combined one -- only the input bag of titles differs.
    //   * Files     -- ANY app where the user is engaged with a file: not
    //                  just whitelisted document-editor apps but ALSO any
    //                  app whose window title contains a recognized file
    //                  extension (doc/docx in Word, xls/xlsx in Excel,
    //                  ppt/pptx in PowerPoint, jpg/png/heic in Photos /
    //                  IrfanView / Paint, cpp/h/json/txt in VS Code /
    //                  Notepad++ / Sublime, pdf in any reader, etc.) plus
    //                  recently-opened file basenames from FileMonitor.
    //   * Websites  -- per-tab aggregation of BrowsingMonitor records.
    //   * Apps      -- non-file, non-browser foreground apps: communications
    //                  (Outlook / Teams / Slack / Discord / WhatsApp / Zoom),
    //                  media players (Spotify / VLC), terminals, remote
    //                  desktop, version-control UIs, and anything else that
    //                  isn't engaged with a specific file.  Strictly
    //                  disjoint from Files and Websites.
    // An empty string here means the category had no usable activity in the
    // window.
    std::string   oneLinerFiles;
    std::string   oneLinerWebsites;
    std::string   oneLinerApps;
    int           activityCount  = 0;   // Number of raw signals considered.
    int           focusSeconds   = 0;   // Total foreground time in the window.
    double        confidence     = 0.0; // [0,1] -- signal-quality, NOT ML confidence.
    int           dominantPct    = 0;   // % of focus seconds spent in the dominant app.
    std::vector<std::string> signalTypes; // {"app_focus","browsing","app_launch","file"}
    std::string   model;                // "bge-small-en-v1.5", "all-MiniLM-L6-v2", or "deterministic"
    int           threadCount    = 0;   // Number of distinct semantic clusters.

    // Bounded structured breakdown (top apps by focus seconds, capped at 5).
    struct AppItem
    {
        std::string app;          // Friendly app name e.g. "Visual Studio".
        std::string exe;          // exe basename e.g. "devenv.exe".
        std::string title;        // Cleaned document/tab title.
        std::string rawTitle;     // Original window title before cleaning.
        int         focusSeconds = 0;
        int         pct          = 0; // % of total focus seconds.
        int         threadId     = 0; // Cluster ID this item belongs to (1-based).
    };
    std::vector<AppItem> items;
};

// =====================================================================
// Dynamic context-inference engine (replaces the old TopicInference, which
// used a MiniLM model to map activities to ~50 *pre-defined* topic
// buckets).  This engine instead uses a sentence-encoder (BGE-small by
// default, MiniLM as a backward-compatibility fallback) to **dynamically
// cluster** the
// observed titles -- the one-liner is composed from the literal documents,
// tabs, and apps the user is engaged with, with semantically related items
// merged into "threads of work".
//
// Lifecycle
//   * Init(modelsDir) -- always returns true.  If the model files
//     (`vocab.txt` + `bge-small.onnx`, or the legacy `minilm.onnx`) are
//     present in `modelsDir` the embedding
//     pipeline initializes; otherwise the engine still runs in a
//     deterministic-only fallback mode.  Pass an empty wstring to skip the
//     model load entirely.
//   * Start(db, fg) -- spawns a background thread that wakes every 60 s,
//     reads the last 15 minutes of activity from `db` plus the live
//     foreground session from `fg`, composes a one-liner, and appends it
//     to history when it materially changes (or every 5 min as a heartbeat
//     -- whichever comes first).  The "latest" cache is always refreshed.
//   * Stop()     -- joins the thread.
//
// Query API
//   * GetRecentContext(category, windowSecs)
//         JSON: {"recent_context": <snapshot>}  (single snapshot)
//   * GetRecentContexts(n, category, windowSecs)
//         JSON: {"recent_contexts":[...]} newest first.
//   * ClearHistory() -- empties history (called on user "Clear").
//
// `category` is one of "all" (default), "files", "websites", "apps".
// When supplied (and not "all") the response carries ONLY that
// category's one-liner (as `one_liner`); the other categories are
// omitted from the response.  Backward-compat: the legacy value
// "documents" is accepted and treated as "files".
//
// `windowSecs` is the activity lookback window in seconds (default
// 15 * 60 = 900).  When the requested window equals the default and a
// fresh cached snapshot is available, that cache is returned; for any
// other window the snapshot is composed on demand against the
// requested span.  For GetRecentContexts the rolling history is also
// filtered by timestamp to only include entries whose snapshot
// timestamp falls within the last `windowSecs`.
//
// History cap is 1440 (24 h at one snapshot per minute, with material-
// change dedup the actual count is usually much smaller).
// =====================================================================
class ContextInference
{
public:
    ContextInference();
    ~ContextInference();

    bool Init(const std::wstring& modelsDir = L"");
    bool IsModelLoaded() const { return m_modelReady; }
    void Start(ActivityDatabase* db, ForegroundMonitor* fg);
    void Stop();

    void RunOnce();
    std::string GetRecentContext (const std::string& category    = "all",
                                  int64_t            windowSecs  = 0);
    std::string GetRecentContexts(int                count,
                                  const std::string& category    = "all",
                                  int64_t            windowSecs  = 0);
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

    // ---- Sentence-encoder embedding pipeline (optional) ----------------
    // Default model: BAAI/bge-small-en-v1.5 (384-dim, BERT WordPiece
    // tokenizer, ~33 M params).  Compatible with the previous
    // all-MiniLM-L6-v2 (same dim, same tokenizer); if `bge-small.onnx`
    // isn't present, Init() falls back to `minilm.onnx` so legacy
    // installations keep working without forcing a redownload.
    BertTokenizer        m_tokenizer;
    Ort::Env*            m_ortEnv     = nullptr;
    Ort::SessionOptions* m_ortOpts    = nullptr;
    Ort::Session*        m_ortSession = nullptr;
    Ort::MemoryInfo*     m_ortMemInfo = nullptr;
    bool                 m_modelReady = false;
    std::string          m_modelName;        // e.g. "bge-small-en-v1.5"

    // Compute a 384-dim L2-normalized sentence embedding for `text`.
    // Returns an empty vector if the model is not loaded or inference fails.
    std::vector<float> Embed(const std::string& text);

    // Cosine similarity for two L2-normalized vectors (== dot product).
    static float CosineSim(const std::vector<float>& a,
                           const std::vector<float>& b);

    void TimerLoop();
    // Compose a fresh snapshot covering the last `windowSecs` seconds of
    // activity.  Pass 0 to use the default 15-minute window.
    ContextSnapshot ComposeSnapshot(int64_t windowSecs = 0);

    // Decide whether `snap` is materially different from the last-appended
    // history entry.  Used to keep `GetRecentContexts` from returning rows
    // of duplicates for a user who is doing one thing for an hour.
    bool ShouldAppendToHistory(const ContextSnapshot& snap) const;

    static std::string SnapshotToJsonObject(const ContextSnapshot& s,
                                            const std::string& category = "all");
    static std::string WideToUtf8(const std::wstring& w);
    static std::string EscapeJson(const std::string& s);
};
