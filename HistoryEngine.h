#pragma once
// HistoryEngine — the historical layer (gap-analysis HIST-1..4, INTEL-6 persistence).
//
//   * Work sessions : contiguous presence (active/passive) or focus activity, split by
//                     away / lock / sleep gaps >= kSessionGapSec.
//   * Work threads  : every context snapshot carries its entity set (keys + active seconds);
//                     consecutive snapshots join the thread with the highest weighted-Jaccard
//                     overlap (with hysteresis for the current thread), otherwise a new thread
//                     starts. Threads persist label, members (decayed weights), project and
//                     time spans, and are resumed across days when a later snapshot matches.
//   * Projects      : stable anchors - git / solution roots, OneDrive / SharePoint library
//                     folders, repository URLs - voted by member weight.
//   * Edges         : decayed co-occurrence between entities seen in the same snapshot,
//                     used for "related items" and contextual boosting.
//
// Writes happen on the ActivityStore writer thread (IWriterSink / Enqueue/RunSync jobs).
// Reads use the store's read pool and may run on any thread.

#include "ActivityStore.h"

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct SnapshotEntity
{
    std::string  entityKey;
    std::string  entityType;     // file | app | url
    std::wstring display;
    double       activeSecs = 0; // clipped active seconds inside the snapshot window
};

struct SnapshotInput
{
    int64_t ts = 0;                        // UTC seconds
    int64_t windowSecs = 0;
    std::vector<SnapshotEntity> entities;  // app entities excluded by the caller when a document is known
    std::string label;                     // human-readable line (rich listing)
    std::string json;                      // full snapshot JSON (persisted when `persist`)
    std::vector<float> embedding;          // optional label embedding (L2-normalized)
    std::string model;
    bool persist = false;                  // store in context_snapshot (material change)
    // HistoryEngine::Generation() read before the snapshot's inputs were queried. A clear-all or
    // purge in between makes the snapshot stale and it is dropped. UINT64_MAX = no check.
    uint64_t generation = UINT64_MAX;
};

struct WorkThreadMember
{
    std::string  entityKey;
    std::string  entityType;
    std::wstring display;
    std::wstring title;
    double       weight = 0;
};

struct WorkThreadInfo
{
    int64_t      id = 0;
    std::string  label;
    int64_t      firstTs = 0;
    int64_t      lastTs = 0;
    double       activeSecs = 0;           // lifetime
    double       activeSecsInRange = 0;    // inside the queried range (from spans)
    std::string  projectKey;
    int64_t      changeSeq = 0;
    bool         current = false;
    std::vector<std::pair<int64_t, int64_t>> spans;   // [start, end] inside the queried range
    std::vector<WorkThreadMember> members;
};

struct WorkSessionInfo
{
    int64_t id = 0;
    int64_t startTs = 0;
    int64_t endTs = 0;
    int     activeSecs = 0;
    int     passiveSecs = 0;
};

struct RelatedEntity
{
    std::string  entityKey;
    std::string  entityType;
    std::wstring display;
    double       weight = 0;               // decayed at query time
    int64_t      lastTs = 0;
};

class HistoryEngine : public IWriterSink
{
public:
    static constexpr int64_t kSessionGapSec = 15 * 60;

    explicit HistoryEngine(ActivityStore* store);

    // Called by ContextInference after each compose (blocks briefly; runs on the writer thread).
    // Returns the thread id the snapshot was assigned to (0 if it carried no entities).
    int64_t RecordSnapshot(const SnapshotInput& in, DWORD timeoutMs = 5000);

    // ---- reads (any thread) ----
    std::vector<WorkThreadInfo>  GetThreads(int64_t startUtc, int64_t endUtc, int limit, bool includeMembers = true);
    std::vector<WorkSessionInfo> GetSessions(int64_t startUtc, int64_t endUtc, int limit);
    std::vector<RelatedEntity>   Related(const std::string& entityKey, int limit, int64_t nowUtc = 0);
    // Contextual boost in [0,1] for each key relative to the current thread (membership + edges).
    std::vector<double>          ContextBoost(const std::vector<std::string>& keys, int64_t nowUtc = 0);
    int64_t     CurrentThreadId() const { return m_currentThread.load(); }
    std::string CurrentThreadLabel() const;
    // Bumped (writer thread) by clear-all, purge and rolled-back writes; see SnapshotInput::generation.
    uint64_t    Generation() const { return m_generation.load(); }
    std::vector<std::string> ContextSnapshotsJson(int count, int64_t sinceUtc);   // newest first

    // Project anchor for a file path or URL ("" if none) - exposed for tests and the context layer.
    static std::string ProjectKeyFor(const std::wstring& pathOrUrl, const std::string& entityType);

    // ---- IWriterSink (writer thread only) ----
    void OnFocusEvent(WriterContext& w, const FocusEventRecord& r) override;
    void OnPresence(WriterContext& w, const PresenceRecord& r) override;
    void OnMaintenance(WriterContext& w, int64_t nowUtc) override;
    void OnClearAll(WriterContext& w) override;
    void OnRollback(WriterContext& w) override;
    void OnEntityRemoved(WriterContext& w, const std::string& entityKey) override;

private:
    struct ThreadState
    {
        int64_t     id = 0;
        std::string label;
        int64_t     firstTs = 0;
        int64_t     lastTs = 0;
        double      activeSecs = 0;
        std::string projectKey;
        std::vector<float> centroid;
        std::unordered_map<std::string, double> members;   // entity key -> decayed weight
        std::unordered_map<std::string, std::string> memberTypes;
        bool        dirtyMembers = false;
    };
    struct SessionState
    {
        int64_t id = 0;
        int64_t startTs = 0;
        int64_t endTs = 0;
        double  activeSecs = 0;
        double  passiveSecs = 0;
    };

    int64_t AssignSnapshot(WriterContext& w, const SnapshotInput& in);
    void    EnsureThreadsLoaded(WriterContext& w);
    void    PersistThread(WriterContext& w, ThreadState& t);
    void    ExtendSpan(WriterContext& w, int64_t threadId, int64_t ts, double activeSecs);
    void    UpdateEdges(WriterContext& w, const SnapshotInput& in, double scale);
    void    ExtendSession(WriterContext& w, int64_t startTs, int64_t endTs, double activeSecs, double passiveSecs);
    void    CloseSession(WriterContext& w);
    void    ResetCaches();

    ActivityStore* m_store = nullptr;

    // writer-thread only
    bool    m_threadsLoaded = false;
    std::map<int64_t, ThreadState> m_threads;          // recently active threads
    SessionState m_session;
    bool    m_haveSession = false;
    int64_t m_spanThread = 0;                          // thread of the currently open span
    int64_t m_spanStart = 0;
    int64_t m_spanEnd = 0;
    int64_t m_lastSnapshotTs = 0;

    // shared
    std::atomic<int64_t> m_currentThread{ 0 };
    std::atomic<uint64_t> m_generation{ 0 };
    mutable std::mutex   m_labelMtx;
    std::string          m_currentLabel;
};
