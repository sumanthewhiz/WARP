#pragma once
// ActivityStore — persistence layer for WARP.
//
// Design (replaces the old ActivityDatabase, see gap-analysis BUG-1/DATA-5/DATA-6/PRIV-4/RANK-4):
//   * ONE writer thread owns the writer connection. Producers never touch SQLite: they
//     enqueue typed records (bounded queue, never blocks the caller). The writer batches
//     jobs into BEGIN IMMEDIATE ... COMMIT transactions with cached prepared statements and
//     invokes registered IWriterSink objects (EntityStore, HistoryEngine) inside the same
//     transaction, so raw rows and derived state commit atomically and in order.
//   * Reads use a small pool of separate connections (WAL => readers never block the writer).
//   * Schema is versioned (PRAGMA user_version) with ordered, transactional migrations;
//     PRAGMA quick_check runs at open and a corrupt file is quarantined and recreated.
//   * A global, monotonic change sequence (change_seq) drives the consumer change feed.
//   * Retention, scoped purge and secure clear (secure_delete + WAL truncate + VACUUM) run
//     on the writer thread, so they can never race with in-flight inserts.

#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <memory>
#include <future>
#include <cstdint>

#include "EventContext.h"

struct sqlite3;
struct sqlite3_stmt;

static const uint32_t EVENT_TYPE_FILE       = 0x01;
static const uint32_t EVENT_TYPE_APP_LAUNCH = 0x02;
static const uint32_t EVENT_TYPE_BROWSING   = 0x04;
static const uint32_t EVENT_TYPE_APP_FOCUS  = 0x08;
static const uint32_t EVENT_TYPE_ALL        = 0x0F;

// ---------------- Read-side rows ----------------
struct FileActivity
{
    int64_t      id = 0;
    int64_t      timestampUtc = 0;
    int64_t      eventMs = 0;
    std::wstring action;            // OPEN, CREATE, MODIFY, DELETE, RENAME, DOWNLOAD
    std::wstring path;
    std::wstring oldPath;
    std::wstring source;            // etw, rdcw, shell, usn, recent, mru
    std::wstring sourceExe;
    std::string  entityKey;
    double       confidence = 1.0;
    bool         isDir = false;
};

struct AppLaunchActivity
{
    int64_t      id = 0;
    int64_t      timestampUtc = 0;
    std::wstring exeName;
    std::wstring exePath;
    DWORD        pid = 0;
    std::wstring creatorExe;
    std::wstring aumid;
    double       confidence = 1.0;
};

struct BrowsingActivity
{
    int64_t      id = 0;
    int64_t      timestampUtc = 0;
    std::wstring browser;
    std::wstring title;
    std::wstring url;               // canonical, privacy-filtered
    std::wstring host;
    std::wstring docId;
    std::string  entityKey;
    int          dwellSecs = 0;     // dwell of the PREVIOUS page shown in the same window (windowId)
    int64_t      windowId = 0;      // browser top-level window (0 = unknown / legacy row)
    double       confidence = 1.0;
};

struct AppFocusActivity
{
    int64_t      id = 0;
    int64_t      timestampUtc = 0;  // legacy: session END (seconds)
    int64_t      startTs = 0;       // session start (seconds)
    int64_t      endTs = 0;         // session end (seconds)
    std::wstring exeName;
    std::wstring exePath;
    std::wstring windowTitle;
    int          durationSecs = 0;  // wall-clock length (legacy field)
    int          activeSecs = 0;    // excludes idle/away time
    std::string  docKey;            // resolved document/URL entity key ("" if unresolved)
    std::wstring docDisplay;
    std::string  docType;           // file | url | webdoc | ""
    double       confidence = 1.0;
};

enum class TimeWindow { Minutes15, Minutes30, Hours1, Hours2, Hours6, Hours24, Days7, Days15, Days30 };

// ---------------- Write-side records ----------------
struct FileEventRecord
{
    int64_t      eventMs = 0;       // when it happened (UTC ms); 0 => now
    std::wstring action;
    std::wstring path;
    std::wstring oldPath;
    std::wstring source = L"etw";
    bool         isDir = false;
    std::wstring originUrl;         // download origin (Zone.Identifier), canonical
    std::string  presence;
    EventContext ctx;
};

struct LaunchEventRecord
{
    int64_t      eventMs = 0;
    std::wstring exeName;
    std::wstring exePath;
    DWORD        pid = 0;
    DWORD        creatorPid = 0;
    std::wstring creatorExe;
    DWORD        sessionId = 0;
    std::wstring aumid;
    EventContext ctx;
};

struct ProcessExitRecord
{
    int64_t      eventMs = 0;
    DWORD        pid = 0;
    int64_t      startMs = 0;
};

struct BrowseEventRecord
{
    int64_t      eventMs = 0;
    std::wstring browser;
    std::wstring title;
    std::wstring url;               // canonical URL (already privacy filtered) or empty
    std::wstring host;
    std::wstring docId;
    std::wstring docKind;
    int          dwellSecs = 0;     // dwell of the PREVIOUS page in this tab/window, if known
    int64_t      windowId = 0;      // browser top-level window the page was shown in (0 = unknown)
    std::string  presence;
    EventContext ctx;
};

struct FocusEventRecord
{
    int64_t      startMs = 0;
    int64_t      endMs = 0;
    std::wstring exeName;
    std::wstring exePath;
    std::wstring windowTitle;
    std::wstring windowClass;
    std::wstring desktopId;
    std::wstring aumid;
    int          activeSecs = 0;
    int          idleSecs = 0;
    std::string  docKey;            // entity key of the document/URL this session was about
    std::wstring docDisplay;
    std::string  docType;           // file | url | webdoc
    std::string  presence;
    EventContext ctx;               // captured at session START
};

struct PresenceRecord { int64_t startMs = 0; int64_t endMs = 0; std::string state; };
struct ClipboardRecord { int64_t eventMs = 0; std::wstring ownerExe; std::wstring formats; };
struct PrintRecord { int64_t eventMs = 0; std::wstring document; std::wstring printer; DWORD pages = 0; };

// ---------------- Writer plumbing ----------------
class WriterContext
{
public:
    sqlite3* db = nullptr;
    int64_t  nowMs = 0;                       // batch timestamp
    bool     ftsAvailable = false;            // activity_fts exists (SQLite built with FTS5)
    sqlite3_stmt* Stmt(const char* sql);      // cached per SQL text (pointer identity)
    int64_t NextChangeSeq();
    int64_t CurrentChangeSeq() const;
private:
    friend class ActivityStore;
    std::unordered_map<const char*, sqlite3_stmt*> m_cache;
    std::atomic<int64_t>* m_seq = nullptr;
};

class IWriterSink
{
public:
    virtual ~IWriterSink() = default;
    virtual void OnFileEvent(WriterContext&, const FileEventRecord&) {}
    virtual void OnLaunchEvent(WriterContext&, const LaunchEventRecord&) {}
    virtual void OnBrowseEvent(WriterContext&, const BrowseEventRecord&) {}
    virtual void OnFocusEvent(WriterContext&, const FocusEventRecord&) {}
    virtual void OnPresence(WriterContext&, const PresenceRecord&) {}
    virtual void OnMaintenance(WriterContext&, int64_t /*nowUtc*/) {}
    virtual void OnClearAll(WriterContext&) {}
    virtual void OnRollback(WriterContext&) {}   // a batch or a failed job was rolled back: drop write-through caches
    virtual void OnEntityRemoved(WriterContext&, const std::string& /*entityKey*/) {}   // purge / retention eviction
};

class ActivityStore
{
public:
    ActivityStore();
    ~ActivityStore();

    // Opens (creating if needed), integrity-checks, migrates, opens the read pool and starts the writer.
    bool Open(const std::wstring& dbPath);
    void Close();                              // drains (bounded), stops writer, closes connections
    bool IsOpen() const { return m_open.load(); }
    std::wstring Path() const { return m_path; }

    void AddSink(IWriterSink* sink);           // call before Open
    void SetRetention(int rawDays, int entityDays);

    // ---- async writes: never block; return false if dropped (queue full / closed) ----
    bool RecordFile(FileEventRecord r);
    bool RecordLaunch(LaunchEventRecord r);
    bool RecordProcessExit(ProcessExitRecord r);
    bool RecordBrowse(BrowseEventRecord r);
    bool RecordFocus(FocusEventRecord r);
    bool RecordPresence(PresenceRecord r);
    bool RecordClipboard(ClipboardRecord r);
    bool RecordPrint(PrintRecord r);

    using WriteJob = std::function<void(WriterContext&)>;
    bool Enqueue(WriteJob job, bool critical = false);
    bool RunSync(WriteJob job, DWORD timeoutMs = 30000);   // waits until committed

    // ---- reads (pooled read-only connections) ----
    class ReadLease
    {
    public:
        ReadLease() = default;
        ReadLease(ActivityStore* s, sqlite3* db) : m_store(s), m_db(db) {}
        ReadLease(ReadLease&& o) noexcept : m_store(o.m_store), m_db(o.m_db) { o.m_store = nullptr; o.m_db = nullptr; }
        ReadLease& operator=(ReadLease&& o) noexcept;
        ReadLease(const ReadLease&) = delete;
        ReadLease& operator=(const ReadLease&) = delete;
        ~ReadLease();
        sqlite3* get() const { return m_db; }
        explicit operator bool() const { return m_db != nullptr; }
    private:
        ActivityStore* m_store = nullptr;
        sqlite3*       m_db = nullptr;
    };
    ReadLease AcquireReader(DWORD timeoutMs = 10000);

    // limit <= 0 => unlimited (in-process callers); beforeId > 0 => rows with id < beforeId (paging)
    std::vector<FileActivity>      QueryFiles(int64_t startUtc, int64_t endUtc, int limit = 0, int64_t beforeId = 0, double minConfidence = 0.0);
    std::vector<AppLaunchActivity> QueryLaunches(int64_t startUtc, int64_t endUtc, int limit = 0, int64_t beforeId = 0, double minConfidence = 0.0);
    std::vector<BrowsingActivity>  QueryBrowsing(int64_t startUtc, int64_t endUtc, int limit = 0, int64_t beforeId = 0, double minConfidence = 0.0);
    // Focus sessions OVERLAPPING [startUtc, endUtc] (by start/end), newest first.
    std::vector<AppFocusActivity>  QueryFocus(int64_t startUtc, int64_t endUtc, int limit = 0, int64_t beforeId = 0, double minConfidence = 0.0);

    // Legacy convenience (now = end of window).
    std::vector<FileActivity>      QueryFilesCustomSeconds(int64_t seconds);
    std::vector<AppLaunchActivity> QueryAppLaunchesCustomSeconds(int64_t seconds);
    std::vector<BrowsingActivity>  QueryBrowsingCustomSeconds(int64_t seconds);
    std::vector<AppFocusActivity>  QueryAppFocusCustomSeconds(int64_t seconds);
    static int64_t WindowToSeconds(TimeWindow w);

    // ---- maintenance & privacy ----
    bool ScheduleMaintenance();                       // retention + checkpoint + incremental vacuum (async)
    bool ClearAll(DWORD timeoutMs = 120000);          // secure wipe of everything (sync)
    struct PurgeSpec
    {
        int64_t      startUtc = 0;                    // 0 = unbounded
        int64_t      endUtc   = 0;                    // 0 = unbounded
        std::string  entityKey;                       // purge one entity everywhere
        std::wstring domain;                          // purge a web host (and subdomains)
        std::wstring pathPrefix;                      // purge files under a folder
    };
    bool Purge(const PurgeSpec& spec, int64_t& rowsDeleted, DWORD timeoutMs = 120000);

    // ---- change feed ----
    int64_t CurrentChangeSeq() const { return m_committedSeq.load(); }
    int64_t ResetSeq() const { return m_resetSeq.load(); }
    bool    WaitForChange(int64_t afterSeq, DWORD timeoutMs);   // true if CurrentChangeSeq() > afterSeq

    // Writer-thread only: delete an inference entity, emit a tombstone into the change feed and
    // evict it from every sink cache. Used by sinks for deletes/renames.
    void Tombstone(WriterContext& w, const std::string& entityKey, const std::string& entityType, const char* reason);

    // Writer-thread only (sinks): true when a purged or cleared time range lies between t0 and t1 (UTC seconds: the
    // end of earlier activity and the start of later activity), so activity on both sides of it must not be joined
    // into one work session.
    bool PurgedBetween(int64_t t0, int64_t t1) const;

    // ---- meta key/value ----
    bool GetMeta(const std::string& key, std::string& value);
    void SetMeta(const std::string& key, const std::string& value);   // async

    struct Stats
    {
        uint64_t enqueued = 0, dropped = 0, written = 0, batches = 0, writeErrors = 0;
        size_t   queueDepth = 0;
        int64_t  lastCommitUtc = 0;
        int64_t  dbBytes = 0, walBytes = 0;
        int      schemaVersion = 0;
        std::string integrity;      // "ok", "recovered", or error text
        int64_t  changeSeq = 0, resetSeq = 0;
        int      rawRetentionDays = 30, entityRetentionDays = 395;
    };
    Stats GetStats() const;

private:
    struct Job
    {
        WriteJob fn;
        bool     critical = false;                       // bypasses queue capacity
        bool     noTxn = false;                          // must run outside a transaction (VACUUM, checkpoint)
        std::shared_ptr<std::promise<bool>> done;        // signalled after commit (RunSync)
    };

    bool Submit(Job job, DWORD timeoutMs);               // enqueue + wait for completion

    bool OpenConnection(sqlite3** db, bool writer);
    bool CheckIntegrityOrQuarantine();
    bool Migrate();
    void WriterLoop();
    void RunBatch(std::vector<Job>& batch);
    void ReleaseReader(sqlite3* db);
    void FinalizeWriterStatements();
    bool EnqueueRecord(WriteJob job);
    void DoMaintenance(WriterContext& w);
    void DoClearAll(WriterContext& w);
    int64_t DoPurge(WriterContext& w, const PurgeSpec& spec);
    // Daily-rollup totals of one entity, with engagement re-derived from them (days < 0: unreadable).
    struct RollupSummary { double opens = 0, edits = 0, active = 0, s = 0, l = 0; int days = 0; int64_t lastDay = 0; };
    RollupSummary SummarizeRollups(WriterContext& w, const std::string& key, const std::string& type);
    // Time-range purge: scales an entity's totals and engagement down to the evidence that remains
    // (`before` = its rollups before the purge); false => no evidence remains.
    bool RecomputeEntityAfterPurge(WriterContext& w, const std::string& key, const std::string& type, int64_t a, int64_t b,
                                   const RollupSummary& before);
    // Re-derives work threads that held purged items or purged time; returns the number deleted.
    int64_t ScrubThreadsAfterPurge(WriterContext& w, std::vector<int64_t> threadIds);

    // What purges and clears deleted, so records reported LATER about that time or those items are not stored
    // again: Office MRU / Recent-items polls, held safe-saves, a focus session or presence interval that was
    // still open. kind 0: time range [fromMs, toMs] (persisted in meta 'purged_ranges'); 1 entity key; 2 web
    // host (and subdomains); 3 path prefix (file key) - scoped floors cover activity up to the purge time and
    // stay in memory only (their value names what was purged). Writer thread only.
    struct PurgeFloor { int kind = 0; std::string value; int64_t fromMs = 0; int64_t toMs = 0; };
    // A record's identities (empty = not applicable): entity keys, lowercase web host, file keys.
    struct RecordIds { std::string key, key2, host, fileKey, fileKey2; };
    std::vector<PurgeFloor>  m_floors;
    std::vector<PurgeFloor>  m_floorsCommitted;   // m_floors as of the last commit (restored when a commit fails)
    bool                     m_floorsDirty = false;
    void AddFloor(WriterContext& w, PurgeFloor f);
    void LoadFloors();
    bool Suppressed(int64_t ms, const RecordIds& ids) const;
    // The parts of [startMs, endMs] outside every floor that covers the record.
    std::vector<std::pair<int64_t, int64_t>> UnpurgedParts(int64_t startMs, int64_t endMs, const RecordIds& ids) const;

    std::wstring             m_path;
    sqlite3*                 m_wdb = nullptr;
    std::vector<sqlite3*>    m_readers;          // pool
    std::vector<sqlite3*>    m_freeReaders;
    std::mutex               m_readMtx;
    std::condition_variable  m_readCv;

    std::vector<IWriterSink*> m_sinks;
    std::thread              m_writer;
    mutable std::mutex       m_qMtx;
    std::condition_variable  m_qCv;
    std::deque<Job>          m_queue;
    std::atomic<bool>        m_open{ false };
    std::atomic<bool>        m_stopping{ false };
    std::atomic<int64_t>     m_stopDeadlineMs{ 0 };   // after this, Close() drops non-critical backlog
    WriterContext            m_ctx;

    std::atomic<int64_t>     m_seq{ 0 };
    std::atomic<int64_t>     m_committedSeq{ 0 };
    std::atomic<int64_t>     m_resetSeq{ 0 };
    std::mutex               m_seqMtx;
    std::condition_variable  m_seqCv;

    std::atomic<int>         m_rawRetentionDays{ 30 };
    std::atomic<int>         m_entityRetentionDays{ 395 };

    mutable std::mutex       m_statsMtx;
    Stats                    m_stats;

    static const size_t      kQueueCapacity = 20000;
    static const size_t      kBatchMax = 512;
    static const int         kReaderCount = 3;
};

// Helpers shared by store users.
namespace StoreUtil
{
    // Engagement decay time constants (seconds): short-term (~3 days) and long-term (~30 days).
    constexpr double kTauShortSec = 3.0 * 86400.0;
    constexpr double kTauLongSec  = 30.0 * 86400.0;

    // Engagement weights (shared by the v2 migration and EntityStore so seeded and live
    // values are on the same scale).
    constexpr double kWOpenPassive   = 0.3;   // kernel-observed open, x confidence
    constexpr double kWOpenExplicit  = 1.5;   // shell/Recent/Office-MRU open, user-visible launch
    constexpr double kWEdit          = 3.0;   // save / modify / create, x confidence
    constexpr double kWActivePerMin  = 1.0;   // active focus minute on the entity
    constexpr double kWVisit         = 0.5;   // web page visit, x confidence
    constexpr double kWNewDay        = 2.0;   // first engagement on a new local day
    constexpr double kMaxActiveMinutesPerSession = 60.0;

    std::string  W2U(const std::wstring& w);
    std::wstring U2W(const char* s);
    int64_t      NowMs();
    int64_t      NowUtc();
    int64_t      LocalDayIndex(int64_t utcSeconds);  // days since 1970-01-01 in local time
    // Time-independent ranking key for an exponentially-decayed accumulator (S at time t0):
    // S(now) = S*e^{-(now-t0)/tau}  =>  ordering by ln(S) + t0/tau is identical for every `now`.
    double       DecayKey(double s, int64_t t0, double tauSec);

    // Entity keys shared by every producer/consumer (app: AUMID when known so packaged-app
    // updates don't split history; web: document identity > canonical URL > title).
    std::string  AppEntityKey(const std::wstring& exePath, const std::wstring& aumid);
    std::string  WebEntityKey(const std::wstring& canonicalUrl, const std::wstring& docId, const std::wstring& title);
}
