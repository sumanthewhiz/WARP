#pragma once
// EntityStore — per-entity engagement model (replaces InferenceEngine).
//
// Gap-analysis fixes: BUG-1 (single writer), RANK-1 (scores computed at READ time, never
// frozen at write time), RANK-2/3 (decayed short/long-term engagement with per-type
// percentile normalization and human-readable reasons), RANK-4 / DATA-2/3 (shared entity
// keys, renames migrate history, deletes tombstone into the change feed), API-6 (scored
// lookups, top-N, search, cursor-based change feed with resets).
//
// Threading: the IWriterSink callbacks run ONLY on the ActivityStore writer thread, inside
// the batch transaction (write-through cache, no locks). Read methods may be called from any
// thread and use the store's pooled read-only connections.

#include "ActivityStore.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct EntityRecord
{
    std::string  entityKey;
    std::string  entityType;          // file | app | url
    std::wstring displayName;         // full path, exe path / AUMID, canonical URL
    std::wstring title;               // best human title (page title, app name)
    int64_t      lastEventTs = 0;
    int64_t      lastOpenTs = 0;
    int64_t      lastEditTs = 0;
    int64_t      lastActiveTs = 0;
    double       openCountTotal = 0;  // confidence-weighted
    double       editCountTotal = 0;
    double       activeSecsTotal = 0;
    int          distinctDays = 0;
    int64_t      lastDay = 0;         // local day index
    double       engS = 0;            // short-horizon accumulator value at engSTs
    int64_t      engSTs = 0;
    double       engL = 0;            // long-horizon accumulator value at engLTs
    int64_t      engLTs = 0;
    int64_t      changeSeq = 0;       // global change sequence (exposed as legacy "version")
    int64_t      updatedAt = 0;
    bool         deleted = false;

    // Read-time derived (from entity_day rollups at query time)
    double       openCount7d = 0;
    double       openCount30d = 0;
    double       edits7d = 0;
    double       activeSecs7d = 0;
    int          activeDays7 = 0;
    double       recencyScore = 0;    // legacy 0-255 score, evaluated at the query's `now`
};

struct EntityScore
{
    EntityRecord rec;
    bool         found = false;
    double       shortTerm = 0;       // decayed engagement at `now` (days horizon)
    double       longTerm = 0;        // decayed engagement at `now` (weeks horizon)
    double       prior = 0;           // horizon-specific raw prior
    double       score = 0;           // percentile of `prior` among entities of the same type, [0,1]
    double       relevance = 0;       // Search only: text relevance in [0,1]
    std::vector<std::string> reasons; // e.g. "edited today", "42 min active in the last 7 days"
};

struct EntityTombstone
{
    int64_t     changeSeq = 0;
    std::string entityKey;
    std::string entityType;
    std::string reason;               // deleted | renamed | purge | retention
    int64_t     ts = 0;
};

struct EntityChanges
{
    std::vector<EntityRecord>    upserts;
    std::vector<EntityTombstone> deletes;
    int64_t nextSeq = 0;              // pass back as `since` on the next call
    int64_t currentSeq = 0;
    bool    reset = false;            // cursor predates a clear-all / pruned tombstones: drop local state and resync from 0
    bool    more = false;             // more changes are immediately available
};

class EntityStore : public IWriterSink
{
public:
    explicit EntityStore(ActivityStore* store);

    // Normalizes a raw identifier exactly as producers do: URL -> canonical URL / document key,
    // AUMID -> "aumid:..." key, anything else -> canonical file key. Empty if excluded/invalid.
    static std::string NormalizeKey(const std::wstring& raw);
    // Editor temp / lock / partial-download names that must never become ranked entities.
    static bool IsTransientFileName(const std::wstring& path);
    static double LegacyRecencyScore(int64_t nowUtc, int64_t lastOpenTs, double openCount7d);

    // ---- reads (any thread) ----
    std::vector<EntityRecord> Lookup(const std::vector<std::string>& keys, int64_t nowUtc = 0);
    // horizon: "short" (days), "long" (weeks), anything else = "blend" (0.7 short + 0.3 long)
    std::vector<EntityScore>  Score(const std::vector<std::string>& keys, const std::string& horizon = "blend",
                                    int64_t nowUtc = 0, bool withReasons = true);
    // entityType: file | app | url | "" (all types merged by percentile)
    std::vector<EntityScore>  Top(const std::string& entityType, int limit, const std::string& horizon = "blend",
                                  int64_t nowUtc = 0, bool withReasons = true);
    std::vector<EntityScore>  Search(const std::wstring& query, const std::string& entityType, int limit,
                                     int64_t nowUtc = 0);
    EntityChanges             GetChanges(int64_t sinceSeq, int limit, bool includeDeletes = true);

    // Tests only: shorten the safe-save grace window (default 15 s) after which a delete becomes a tombstone.
    void SetDeleteGraceForTesting(int64_t ms) { m_deleteGraceMs = ms; }

    // ---- IWriterSink (writer thread only) ----
    void OnFileEvent(WriterContext& w, const FileEventRecord& r) override;
    void OnLaunchEvent(WriterContext& w, const LaunchEventRecord& r) override;
    void OnBrowseEvent(WriterContext& w, const BrowseEventRecord& r) override;
    void OnFocusEvent(WriterContext& w, const FocusEventRecord& r) override;
    void OnMaintenance(WriterContext& w, int64_t nowUtc) override;
    void OnClearAll(WriterContext& w) override;
    void OnRollback(WriterContext& w) override;
    void OnEntityRemoved(WriterContext& w, const std::string& entityKey) override;

private:
    struct CacheEntry
    {
        EntityRecord rec;
        bool         exists = false;       // row present in the inference table
        std::wstring ftsText;              // last text written to the search index
        int64_t      lastOpenCreditMs = 0; // de-dup repeated opens / visits / launches
        int64_t      lastEditCreditMs = 0; // de-dup autosave bursts
    };

    void        BeginEvent(WriterContext& w);
    CacheEntry* Get(WriterContext& w, const std::string& key);
    CacheEntry& GetOrCreate(WriterContext& w, const std::string& key, const char* type);
    void        SetDisplay(CacheEntry& e, const std::wstring& display, const std::wstring& title);
    void        Credit(WriterContext& w, CacheEntry& e, double weight, int64_t ts,
                       double opens, double edits, double activeSecs, int sessions);
    void        Persist(WriterContext& w, CacheEntry& e);
    void        MarkPendingDelete(WriterContext& w, const std::string& key);
    void        FinalizePendingDeletes(WriterContext& w, bool force);
    void        RenameEntity(WriterContext& w, const std::string& oldKey, const std::string& newKey,
                             const std::wstring& newPath, int64_t ts);
    std::vector<std::string> KeysUnderFolder(WriterContext& w, const std::wstring& folder, int cap);

    void   EnsureDistribution(int64_t nowUtc);
    double Percentile(const std::string& type, const std::string& horizon, double prior);
    bool   FtsAvailable();
    static void FillReasons(EntityScore& s, int64_t nowUtc);
    void   Finish(EntityScore& s, const std::string& horizon, int64_t nowUtc, bool withReasons);

    ActivityStore* m_store = nullptr;

    // writer-thread only
    std::unordered_map<std::string, CacheEntry> m_cache;
    std::unordered_map<std::string, int64_t>    m_pendingDeletes;   // key -> writer time (ms) of delete
    int64_t m_lastPendingSweepMs = 0;

    // read side
    std::mutex m_distMtx;
    std::unordered_map<std::string, std::vector<double>> m_dist;    // "type|horizon" -> sorted priors
    int64_t m_distBuiltUtc = 0;
    std::atomic<int> m_fts{ -1 };                                   // -1 unknown, 0 no, 1 yes
    std::atomic<int64_t> m_deleteGraceMs{ 15000 };
};
