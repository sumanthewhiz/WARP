#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstdint>

struct sqlite3;

struct InferenceRecord
{
    std::string entityKey;
    std::string entityType;
    int64_t     lastEventTs    = 0;
    int64_t     lastOpenTs     = 0;
    int64_t     lastEditTs     = 0;
    // Confidence-weighted rolling counts. The schema column is INTEGER
    // affinity but SQLite stores REAL values without lossy conversion;
    // we read back via sqlite3_column_double. Storing as REAL lets a
    // sequence of low-confidence events (e.g. 10 events at conf 0.1)
    // accumulate to the same effective weight as one full-confidence
    // event, instead of being dropped wholesale by an arbitrary
    // 0.5 threshold.
    double      openCount7d    = 0.0;
    double      openCount30d   = 0.0;
    double      openCountTotal = 0.0;
    double      recencyScore   = 0.0;
    uint32_t    version        = 0;
    int64_t     updatedAt      = 0;
};

class InferenceEngine
{
public:
    InferenceEngine();
    ~InferenceEngine();

    // Must be called after the DB is opened. Stores the db pointer for persistence.
    void Init(struct sqlite3** dbHandle, std::mutex* dbMutex);

    // Called after every raw file-activity event is written.
    // action: CREATE, OPEN, MODIFY, DELETE, RENAME
    // path: full file path (will be lowercased as entity_key)
    // confidence: producer's [0,1] estimate that this event is user-initiated.
    //             Counters are incremented by `confidence` (REAL) so noisy
    //             system-attributed events contribute proportionally less to
    //             the rolling counts than user-foreground events.
    void OnFileEvent(const std::wstring& action,
                     const std::wstring& path,
                     int64_t             eventTs,
                     double              confidence = 1.0);

    // Called after every raw app-launch event is written.
    void OnAppLaunchEvent(const std::wstring& exePath,
                          int64_t             eventTs,
                          double              confidence = 1.0);

    // Called after every raw browsing event is written.
    void OnBrowsingEvent(const std::wstring& url,
                         int64_t             eventTs,
                         double              confidence = 1.0);

    // Called after every foreground-app focus session ends.
    void OnAppFocusEvent(const std::wstring& exePath,
                         int64_t             eventTs,
                         double              confidence = 1.0);

    // Bulk typed lookup for in-process consumers (e.g. ContextInference)
    // that need to read per-entity records without going through the
    // JSON wire format used by HandleQueryInferences.  Returns one
    // entry per input key in the same order; entries that don't
    // exist in the inference store come back default-constructed
    // (entityKey/entityType empty, all numeric fields zero).
    //
    // Reads cache first; missing entries fall through to a single DB
    // round-trip per key.  Typical snapshot size (<50 keys) completes
    // in microseconds once the cache is warm.
    std::vector<InferenceRecord> Lookup(const std::vector<std::string>& keys);

    // Public alias of the internal NormalizeKey() used by all
    // OnXxxEvent() methods, exposed so callers can normalize their
    // wide-path / URL keys to the same lowercased-UTF-8 form the
    // per-entity records are stored under.  Without this, every
    // caller would have to replicate the lowercase + UTF-8 logic
    // and silently drift if the storage scheme ever changes.
    static std::string NormalizeEntityKey(const std::wstring& widePath);

    // Batch lookup for QueryInferences op.
    // paths: list of entity keys to look up (already UTF-8 lowercase).
    // fields: which fields to include in the response (empty = all).
    std::string HandleQueryInferences(const std::vector<std::string>& paths,
                                      const std::vector<std::string>& fields);

    // Delta query for GetInferenceDeltas op.
    std::string HandleGetInferenceDeltas(uint32_t sinceVersion);

    // Clear the in-memory cache (called when user clears history).
    void ClearCache();

    // Recompute open_count_7d / open_count_30d from raw event tables
    // and recalculate recency_score. Call on startup and periodically.
    void RefreshRollingCounts();

private:
    struct sqlite3** m_dbHandle = nullptr;
    std::mutex*      m_dbMutex  = nullptr;

    std::unordered_map<std::string, InferenceRecord> m_cache;
    std::mutex m_cacheMutex;

    static double ComputeRecencyScore(int64_t now, int64_t lastOpenTs, double openCount7d);
    static std::string NormalizeKey(const std::wstring& widePath);
    static std::string WideToUtf8(const std::wstring& w);
    static std::string EscapeJson(const std::string& s);

    // Load a single record from DB into cache if not already present.
    // Must be called with m_cacheMutex held.
    InferenceRecord& LoadOrCreate(const std::string& key, const std::string& entityType);

    // Persist a record to the inference table via INSERT OR REPLACE.
    // Must be called with m_dbMutex held.
    void PersistRecord(const InferenceRecord& rec);

    // Load a single record from DB. Returns false if not found.
    bool LoadFromDb(const std::string& key, InferenceRecord& out);
};
