#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

#include "ModernBertTokenizer.h"

// Forward-declare the ONNX Runtime types so callers don't need to pull the
// header into every translation unit.  All access lives behind opaque pointers.
namespace Ort {
    struct Env;
    struct Session;
    struct SessionOptions;
    struct MemoryInfo;
}

class ActivityStore;
class EntityStore;
class HistoryEngine;
class FocusTracker;

// One snapshot in the rolling history of "what is the user doing right now?".
// Each snapshot is composed from the configured lookback window of foreground /
// browsing / app-launch / file activity (default 15 min, user-selectable from
// 5 min to 30 days).  The literal **summary** text is built from the actual
// document, tab, and application titles captured in the window, as a vector
// of 1-3 short phrase lines (one per cluster of activity).
//
// When the optional sentence-encoder embedding model is available the
// composer additionally runs **dynamic semantic clustering** on the per-app
// descriptions: apps whose embeddings are close in the 384-dim sentence space
// are merged into one "thread of work" so the summary can surface that the
// user has, e.g., the auth source file open in VS Code AND the Auth PR open in
// Edge as a single thread instead of two unrelated lines.  The clusters are
// formed *dynamically from the observed titles* -- there is no fixed bucket
// list or pre-defined topic taxonomy.  The supported model is
// `ibm-granite/granite-embedding-small-english-r2` under `<models>\granite\`.
//
// When the model files are absent the composer falls back to a deterministic
// per-app composition (every app is its own cluster).
struct ContextSnapshot
{
    int64_t       timestamp      = 0;   // When this snapshot was generated (epoch s).
    int64_t       windowStartTs  = 0;   // Inclusive start of the lookback window (epoch s).
    int64_t       windowEndTs    = 0;   // Exclusive end of the evidence window (epoch s).
    int64_t       windowSeconds  = 0;   // Width of the lookback window (= windowEndTs - windowStartTs).
    int64_t       generatedAt = 0, asOfMs = 0, inputWatermark = 0, availableStartMs = 0;
    bool          partial = true, truncated = false, historical = false, liveOverlay = false;
    std::string   evidenceJson;
    std::string   modelRevision, tokenizerRevision;
    // Multi-line "context summary": 1-3 short lines (one per cluster
    // of activity) that semantically describe what the user is doing.
    // Each entry is a separate phrase; consumers render them as
    // separate lines or join with newlines.  Empty vector means no
    // usable activity in the window.
    //
    // Source of truth (v5.17): the canonical summary is a RICH
    // DETERMINISTIC LISTING of the top cleaned title phrases
    // ("User is working on auth middleware, login handler and session
    // store in Visual Studio Code"), produced by ComposeRichListing.
    // The cluster->theme token output is retained internally as the
    // LLM topic hint.  When the optional Qwen3-0.6B "brain" model is
    // loaded it may REFINE this listing into nicer prose, but its
    // output only replaces the listing when it is at least as grounded
    // in the actual window titles (granite cosine) -- so it can only
    // help, never regress below the listing.  An A/B harness showed
    // the listing matches the LLM on semantic fidelity while being
    // more grounded, closer to a human reference, and ~2x more
    // specific, at zero inference cost and full determinism.  There
    // is no separate "polished" field; the canonical summary lives
    // here.
    std::vector<std::string> summary;
    // Category-specific summaries.  Each is composed independently from a
    // *projection* of the same activity window so a consumer can ask
    // "what files has the user been in?", "what websites?", or "what
    // apps?" without having to mentally separate them out of the
    // combined summary.  All three use the same dynamic-clustering +
    // theme-distillation pipeline -- only the input bag of titles differs.
    //   * Files     -- ANY app where the user is engaged with a file: not
    //                  just whitelisted document-editor apps but ALSO any
    //                  app whose window title contains a recognized file
    //                  extension (doc/docx in Word, xls/xlsx in Excel,
    //                  ppt/pptx in PowerPoint, jpg/png/heic in Photos /
    //                  IrfanView / Paint, cpp/h/json/txt in VS Code /
    //                  Notepad++ / Sublime, pdf in any reader, etc.) plus
    //                  recently-opened file basenames from FileCapture.
    //   * Websites  -- per-tab aggregation of WebCapture records.
    //   * Apps      -- non-file, non-browser foreground apps: communications
    //                  (Outlook / Teams / Slack / Discord / WhatsApp / Zoom),
    //                  media players (Spotify / VLC), terminals, remote
    //                  desktop, version-control UIs, and anything else that
    //                  isn't engaged with a specific file.  Strictly
    //                  disjoint from Files and Websites.
    // An empty vector here means the category had no usable activity in
    // the window.
    std::vector<std::string> summaryFiles;
    std::vector<std::string> summaryWebsites;
    std::vector<std::string> summaryApps;
    // Granite-embedded vector of the combined `summary`, ready for
    // similarity search / clustering across snapshots.  384-dim,
    // L2-normalized, produced by the same
    // `ibm-granite/granite-embedding-small-english-r2` ModernBERT
    // encoder used internally for per-app clustering.  Empty when
    // the embedding model is not loaded or the summary is empty.
    std::vector<float> summaryEmbedding;
    int           activityCount  = 0;   // Number of raw signals considered.
    int           focusSeconds   = 0;   // Interaction-active foreground time, never an observation proxy.
    int           exposureSeconds = 0;
    int           passiveSeconds = 0;
    int           idleSeconds = 0;
    int           unknownSeconds = 0;
    double        confidence     = 0.0; // [0,1] -- signal-quality, NOT ML confidence.
    int           dominantPct    = 0;   // % of focus seconds spent in the dominant app.
    std::vector<std::string> signalTypes; // {"app_focus","browsing","app_launch","file"}
    std::string   model;                // "granite-embedding-small-english-r2" or "deterministic"
    std::string   modelPolish;          // "qwen3-0.6b" or "(not loaded)"
    int           threadCount    = 0;   // Number of distinct semantic clusters.

    // Bounded app/document observations (active focus first, capped at 5).
    struct AppItem
    {
        std::string app;          // Friendly app name e.g. "Visual Studio".
        std::string exe;          // exe basename e.g. "devenv.exe".
        std::string title;        // Cleaned document/tab title.
        std::string rawTitle;     // Original window title before cleaning.
        int         focusSeconds = 0;
        int         pct          = 0; // % of total focus seconds.
        int         threadId     = 0; // Cluster ID this item belongs to (1-based).
        std::string entityKey;    // Joinable entity key (document/URL when resolved, else app).
        std::string action;
        std::vector<std::string> evidenceRefs;
    };
    std::vector<AppItem> items;

    // Structured "current task" (INTEL-2): the entities behind this snapshot with keys that
    // join against ScoreEntities / GetChanges / QueryInferences.
    struct EntityItem
    {
        std::string entityKey;
        std::string entityType;   // file | url | app
        std::string display;      // path / URL / app name
        std::string title;        // cleaned title
        std::string app;          // friendly app the entity was used in
        std::string role;         // focus | reference | communication | observed | passive | attention_unknown
        int         activeSecs   = 0;   // clipped active seconds inside the window
        int         exposureSecs = 0;
        int         passiveSecs = 0;
        int         idleSecs = 0;
        int         unknownSecs = 0;
        std::string action;             // foreground | open | modify | create | download | page_observed | launch
        std::string resolution;
        int64_t     lastActiveTs = 0;
        int64_t     lastObservedTs = 0;
        double      score        = 0.0; // long-horizon engagement percentile [0,1]
        std::vector<std::string> evidenceRefs, actions;
    };
    std::vector<EntityItem> entities;
    int64_t      threadId = 0;          // persistent work-thread id (HistoryEngine), 0 if none
    std::string  presence;              // active | passive | away | locked | asleep | ""
};

// =====================================================================
// Dynamic context-inference engine.  Uses the granite sentence-encoder
// (ModernBERT, 384-dim, ibm-granite/granite-embedding-small-english-r2)
// to **dynamically cluster** the observed window/tab/app titles -- the
// summary is composed from the literal documents, tabs, and apps the
// user is engaged with, with semantically related items merged into
// "threads of work".  No fixed taxonomy / pre-defined topic buckets.
//
// Lifecycle
//   * Init(modelsDir) -- always returns true.  If the granite files
//     under `modelsDir\granite\` (`model_quantized.onnx` plus the
//     `vocab.txt` / `merges.txt` / `special_tokens.txt` extracted at
//     build time from the upstream `tokenizer.json`) are present the
//     embedding pipeline initializes; otherwise the engine still runs
//     in a deterministic-only fallback mode.  Pass an empty wstring
//     to skip the model load entirely.
//   * Start(store, fg) -- spawns a background (EcoQoS) thread that wakes every
//     ContextCadenceSeconds (default 60 s), reads the clipped activity intervals of the
//     last 15 minutes from the ActivityStore plus the live foreground session (with its
//     resolved document) from `fg`, composes a summary + current_entities[], records the
//     observation in the HistoryEngine (work threads) and appends it to history when it
//     materially changes (or every 5 min as a heartbeat).  Material changes are also
//     persisted so history survives restarts.  The "latest" cache is always refreshed.
//   * Stop()     -- joins the thread.
//
// Query API
//   * GetRecentContext(category, windowSecs, includeVector)
//         JSON: {"recent_context": <snapshot>}  (single snapshot)
//   * GetRecentContexts(n, category, windowSecs, includeVector)
//         JSON: {"recent_contexts":[...]} newest first (memory, then persisted history).
//   * GetCurrentContextJson(includeVector) -- presence, foreground, latest snapshot.
//   * ClearHistory() -- empties history (called on user "Clear"); epoch-safe.
//
// `category` is one of "all" (default), "files", "websites", "apps".
// When supplied (and not "all") the response carries ONLY that
// category's summary (as `summary`); the other categories are
// omitted from the response.  Backward-compat: the legacy value
// "documents" is accepted and treated as "files".
//
// `windowSecs` is the activity lookback window in seconds (default
// 15 * 60 = 900).  When the requested window equals the default and a
// fresh cached snapshot is available, that cache is returned; any
// other window is composed on demand WITHOUT the LLM, cached with a TTL
// (60 s up to 2 h, 5 min up to 24 h, 15 min beyond) and serialized so
// identical concurrent requests coalesce.  For GetRecentContexts the
// history is filtered by timestamp to the last `windowSecs`.
//
// In-memory history cap is 1440 (24 h at one snapshot per minute, with material-
// change dedup the actual count is usually much smaller).
// =====================================================================
class LlmSummarizer;   // optional polishing layer, see LlmSummarizer.h

class ContextInference
{
public:
    ContextInference();
    ~ContextInference();

    bool Init(const std::wstring& modelsDir = L"");
    bool IsModelLoaded() const { return m_modelReady; }
    void Start(ActivityStore* store, FocusTracker* fg);
    void Stop();

    // Wire in (or detach with nullptr) the optional LLM refiner.  Caller retains ownership.
    // The LLM only runs when enabled in Settings (off by default), on AC power unless
    // LlmAllowedOnBattery, for the combined "all" summary, and only when its inputs changed.
    void SetLlmSummarizer(LlmSummarizer* llm);

    // Per-entity engagement store (EntityStore).  When connected, in-window ranking gets a
    // BOUNDED historical tie-breaker (<= 1.5x, INTEL-4) from each entity's engagement percentile,
    // and current_entities[] carry their long-horizon score.
    void SetEntityStore(EntityStore* es);

    // Historical layer: each compose is recorded as a work-thread observation and material
    // changes are persisted so history survives restarts (HIST-1, INTEL-6).
    void SetHistoryEngine(HistoryEngine* h);

    // Returns the current presence state ("active", "passive", "away", "locked", "asleep").
    void SetPresenceProvider(std::function<std::string()> provider);

    void RunOnce();
    std::string GetRecentContext (const std::string& category    = "all",
                                  int64_t            windowSecs  = 0,
                                  bool               includeVector = true);
    std::string GetRecentContexts(int                count,
                                  const std::string& category    = "all",
                                  int64_t            windowSecs  = 0,
                                  bool               includeVector = true);
    // Structured current context (API v2 GetCurrentContext): presence, foreground, current
    // entities with keys/roles/active seconds, combined summary.  JSON object.
    std::string GetCurrentContextJson(bool includeVector = false);
    std::string GetIntervalContext(int64_t startUtc, int64_t endUtc, int64_t asOfUtc,
                                   const std::string& category = "all", bool includeVector = false);
    void ClearHistory();

    // Embedding helper shared with the history layer (thread labels). Empty if no model.
    std::vector<float> EmbedText(const std::string& text) { return Embed(text); }

private:
#ifdef WARP_UNIT_TESTS
    friend struct ContextInferenceTestAccess;
#endif
    ActivityStore*     m_store = nullptr;
    FocusTracker*      m_fg = nullptr;
    std::thread        m_thread;
    std::atomic<bool>  m_running{ false };
    HANDLE             m_stopEvent = nullptr;

    std::vector<ContextSnapshot> m_history;
    ContextSnapshot              m_latest;       // Always the most recent compute.
    bool                         m_haveLatest = false;
    std::mutex                   m_mutex;
    std::atomic<uint64_t>        m_epoch{ 0 };   // bumped by ClearHistory: in-flight composes are discarded

    // On-demand (non-default window) snapshots, cached with a TTL and composed one at a
    // time so identical concurrent requests coalesce (INTEL-7).
    std::mutex                                      m_composeMtx;
    std::unordered_map<int64_t, ContextSnapshot>    m_windowCache;

    // Embedding LRU cache keyed by text (INTEL-6).
    std::mutex                                                  m_embedMtx;
    std::unordered_map<std::string, std::vector<float>>        m_embedCache;
    std::deque<std::string>                                     m_embedOrder;

    // ---- Sentence-encoder embedding pipeline (optional) ----------------
    // Single supported model: ibm-granite/granite-embedding-small-english-r2
    // (ModernBERT-based, 47 M params, byte-level BPE tokenizer, 384-dim,
    // Apache 2.0, August 2025).  Granite is explicitly trained on code
    // retrieval (COIR benchmark) so it handles code-y window titles
    // (`auth.cpp`, `useEffect`, `node_modules`) robustly.
    //
    // Older BGE-small / MiniLM fallbacks were removed in v5.16 -- the
    // production stack is granite + qwen exclusively, so the fallback
    // surface added test/maintenance burden without contributing to a
    // shipped configuration.  When the granite files are missing the
    // engine still runs in deterministic-only mode (m_modelReady=false),
    // exactly as before.
    ModernBertTokenizer  m_mbTokenizer;
    Ort::Env*            m_ortEnv     = nullptr;
    Ort::SessionOptions* m_ortOpts    = nullptr;
    Ort::Session*        m_ortSession = nullptr;
    Ort::MemoryInfo*     m_ortMemInfo = nullptr;
    bool                 m_modelReady = false;
    std::string          m_modelName;        // "granite-embedding-small-english-r2" or empty
    std::string          m_modelRevision, m_tokenizerRevision;

    // Optional polishing layer.  Borrowed; not owned.
    LlmSummarizer*       m_llm = nullptr;

    // Borrowed collaborators (nullptr in unit tests / tooling).
    EntityStore*         m_entities = nullptr;
    HistoryEngine*       m_historyEngine = nullptr;
    std::function<std::string()> m_presence;

    // Compute a 384-dim L2-normalized sentence embedding for `text` (LRU-cached).
    // Returns an empty vector if the model is not loaded or inference fails.
    std::vector<float> Embed(const std::string& text);
    std::vector<float> EmbedUncached(const std::string& text);

    // Cosine similarity for two L2-normalized vectors (== dot product).
    static float CosineSim(const std::vector<float>& a,
                           const std::vector<float>& b);

    void TimerLoop();
    // Compose a fresh snapshot covering the last `windowSecs` seconds of
    // activity.  Pass 0 to use the default 15-minute window.  `allowLlm` is false for
    // on-demand windows (the LLM never runs on a client's request path).
    ContextSnapshot ComposeSnapshot(int64_t windowSecs = 0, bool allowLlm = true, int64_t asOf = 0,
                                     int64_t knowledgeAsOfMs = 0, bool historical = false);

    // Decide whether `snap` is materially different from the last-appended
    // history entry.  Used to keep `GetRecentContexts` from returning rows
    // of duplicates for a user who is doing one thing for an hour.
    bool ShouldAppendToHistory(const ContextSnapshot& snap) const;

    static std::string SnapshotToJsonObject(const ContextSnapshot& s,
                                            const std::string& category = "all",
                                            bool includeVector = true);
    static std::string WideToUtf8(const std::wstring& w);
};
