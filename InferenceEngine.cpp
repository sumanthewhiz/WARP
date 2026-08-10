#include "framework.h"
#include "InferenceEngine.h"
#include "sqlite3.h"
#include <ctime>
#include <cmath>
#include <algorithm>
#include <sstream>

static const double TAU_SECONDS = 172800.0;
static const double MAX_SCORE   = 200.0;
static const double SCORE_CAP   = 255.0;

InferenceEngine::InferenceEngine() {}
InferenceEngine::~InferenceEngine() {}

void InferenceEngine::Init(struct sqlite3** dbHandle, std::mutex* dbMutex)
{
    m_dbHandle = dbHandle;
    m_dbMutex  = dbMutex;
}

// ===================== Helpers =====================

std::string InferenceEngine::WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

std::string InferenceEngine::EscapeJson(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}

std::string InferenceEngine::NormalizeKey(const std::wstring& widePath)
{
    std::wstring lower = widePath;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    return WideToUtf8(lower);
}

double InferenceEngine::ComputeRecencyScore(int64_t now, int64_t lastOpenTs, double openCount7d)
{
    if (lastOpenTs <= 0) return 0.0;

    double decay = exp(-(double)(now - lastOpenTs) / TAU_SECONDS);
    double score = MAX_SCORE * decay;
    score += log(1.0 + openCount7d) * 5.0;
    if (score > SCORE_CAP) score = SCORE_CAP;
    if (score < 0.0)       score = 0.0;
    return score;
}

// ===================== DB I/O =====================

bool InferenceEngine::LoadFromDb(const std::string& key, InferenceRecord& out)
{
    // Caller must hold m_dbMutex
    if (!m_dbHandle || !*m_dbHandle) return false;

    const char* sql =
        "SELECT entity_key, entity_type, last_event_ts, last_open_ts, last_edit_ts,"
        "       open_count_7d, open_count_30d, open_count_total,"
        "       recency_score, version, updated_at"
        " FROM inference WHERE entity_key = ?;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(*m_dbHandle, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char* ek = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* et = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        out.entityKey      = ek ? ek : "";
        out.entityType     = et ? et : "";
        out.lastEventTs    = sqlite3_column_int64(stmt, 2);
        out.lastOpenTs     = sqlite3_column_int64(stmt, 3);
        out.lastEditTs     = sqlite3_column_int64(stmt, 4);
        out.openCount7d    = sqlite3_column_double(stmt, 5);
        out.openCount30d   = sqlite3_column_double(stmt, 6);
        out.openCountTotal = sqlite3_column_double(stmt, 7);
        out.recencyScore   = sqlite3_column_double(stmt, 8);
        out.version        = static_cast<uint32_t>(sqlite3_column_int(stmt, 9));
        out.updatedAt      = sqlite3_column_int64(stmt, 10);
        found = true;
    }

    sqlite3_finalize(stmt);
    return found;
}

void InferenceEngine::PersistRecord(const InferenceRecord& rec)
{
    // Caller must hold m_dbMutex
    if (!m_dbHandle || !*m_dbHandle) return;

    const char* sql =
        "INSERT OR REPLACE INTO inference"
        " (entity_key, entity_type, last_event_ts, last_open_ts, last_edit_ts,"
        "  open_count_7d, open_count_30d, open_count_total,"
        "  recency_score, version, updated_at)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?);";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(*m_dbHandle, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;

    sqlite3_bind_text(stmt, 1, rec.entityKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, rec.entityType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, rec.lastEventTs);
    sqlite3_bind_int64(stmt, 4, rec.lastOpenTs);
    sqlite3_bind_int64(stmt, 5, rec.lastEditTs);
    sqlite3_bind_double(stmt, 6, rec.openCount7d);
    sqlite3_bind_double(stmt, 7, rec.openCount30d);
    sqlite3_bind_double(stmt, 8, rec.openCountTotal);
    sqlite3_bind_double(stmt, 9, rec.recencyScore);
    sqlite3_bind_int(stmt, 10, static_cast<int>(rec.version));
    sqlite3_bind_int64(stmt, 11, rec.updatedAt);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

InferenceRecord& InferenceEngine::LoadOrCreate(const std::string& key,
                                                const std::string& entityType)
{
    // Caller must hold m_cacheMutex
    auto it = m_cache.find(key);
    if (it != m_cache.end())
        return it->second;

    // Try loading from DB (need DB lock)
    InferenceRecord rec;
    {
        std::lock_guard<std::mutex> dbLock(*m_dbMutex);
        if (LoadFromDb(key, rec))
        {
            m_cache[key] = rec;
            return m_cache[key];
        }
    }

    // Brand new record
    rec.entityKey  = key;
    rec.entityType = entityType;
    m_cache[key]   = rec;
    return m_cache[key];
}

// ===================== Event Ingestion =====================

void InferenceEngine::OnFileEvent(const std::wstring& action,
                                   const std::wstring& path,
                                   int64_t             eventTs,
                                   double              confidence)
{
    if (path.empty()) return;

    std::string key = NormalizeKey(path);
    std::string actionUtf8 = WideToUtf8(action);

    std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
    InferenceRecord& rec = LoadOrCreate(key, "file");

    rec.lastEventTs = eventTs;

    // Confidence-weighted accumulation: a stream of (n) events with
    // average confidence c contributes (n * c) to the rolling counts.
    // The previous threshold-based design (delta = (conf >= 0.5)? 1:0)
    // dropped low-confidence events entirely; the new design lets
    // them contribute proportionally so a steady trickle of dim
    // signal still surfaces in popularity ranking, just at a
    // commensurate weight.
    double countDelta = (confidence > 0.0) ? confidence : 0.0;

    if (actionUtf8 == "OPEN" && countDelta > 0.0)
    {
        rec.lastOpenTs = eventTs;
        rec.openCount7d    += countDelta;
        rec.openCount30d   += countDelta;
        rec.openCountTotal += countDelta;
    }
    else if (actionUtf8 == "MODIFY" || actionUtf8 == "CREATE")
    {
        rec.lastEditTs = eventTs;
    }

    rec.recencyScore = ComputeRecencyScore(eventTs, rec.lastOpenTs, rec.openCount7d);
    rec.version++;
    rec.updatedAt = eventTs;

    // Persist under DB lock
    {
        std::lock_guard<std::mutex> dbLock(*m_dbMutex);
        PersistRecord(rec);
    }
}

void InferenceEngine::OnAppLaunchEvent(const std::wstring& exePath,
                                        int64_t             eventTs,
                                        double              confidence)
{
    if (exePath.empty()) return;

    std::string key = NormalizeKey(exePath);

    std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
    InferenceRecord& rec = LoadOrCreate(key, "app");

    double countDelta = (confidence > 0.0) ? confidence : 0.0;

    rec.lastEventTs    = eventTs;
    if (countDelta > 0.0)
    {
        rec.lastOpenTs     = eventTs;
        rec.openCount7d    += countDelta;
        rec.openCount30d   += countDelta;
        rec.openCountTotal += countDelta;
    }
    rec.recencyScore   = ComputeRecencyScore(eventTs, rec.lastOpenTs, rec.openCount7d);
    rec.version++;
    rec.updatedAt      = eventTs;

    {
        std::lock_guard<std::mutex> dbLock(*m_dbMutex);
        PersistRecord(rec);
    }
}

void InferenceEngine::OnBrowsingEvent(const std::wstring& url,
                                       int64_t             eventTs,
                                       double              confidence)
{
    if (url.empty()) return;

    std::string key = NormalizeKey(url);

    std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
    InferenceRecord& rec = LoadOrCreate(key, "url");

    double countDelta = (confidence > 0.0) ? confidence : 0.0;

    rec.lastEventTs    = eventTs;
    if (countDelta > 0.0)
    {
        rec.lastOpenTs     = eventTs;
        rec.openCount7d    += countDelta;
        rec.openCount30d   += countDelta;
        rec.openCountTotal += countDelta;
    }
    rec.recencyScore   = ComputeRecencyScore(eventTs, rec.lastOpenTs, rec.openCount7d);
    rec.version++;
    rec.updatedAt      = eventTs;

    {
        std::lock_guard<std::mutex> dbLock(*m_dbMutex);
        PersistRecord(rec);
    }
}

void InferenceEngine::OnAppFocusEvent(const std::wstring& exePath,
                                       int64_t             eventTs,
                                       double              confidence)
{
    if (exePath.empty()) return;

    std::string key = NormalizeKey(exePath);

    std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
    InferenceRecord& rec = LoadOrCreate(key, "app");

    double countDelta = (confidence > 0.0) ? confidence : 0.0;

    rec.lastEventTs    = eventTs;
    if (countDelta > 0.0)
    {
        rec.lastOpenTs     = eventTs;
        rec.openCount7d    += countDelta;
        rec.openCount30d   += countDelta;
        rec.openCountTotal += countDelta;
    }
    rec.recencyScore   = ComputeRecencyScore(eventTs, rec.lastOpenTs, rec.openCount7d);
    rec.version++;
    rec.updatedAt      = eventTs;

    {
        std::lock_guard<std::mutex> dbLock(*m_dbMutex);
        PersistRecord(rec);
    }
}

// ===================== Query Handlers =====================

std::vector<InferenceRecord> InferenceEngine::Lookup(
    const std::vector<std::string>& keys)
{
    std::vector<InferenceRecord> out;
    out.reserve(keys.size());

    for (const auto& key : keys)
    {
        InferenceRecord rec;
        bool found = false;

        // Cache first.
        {
            std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
            auto it = m_cache.find(key);
            if (it != m_cache.end())
            {
                rec = it->second;
                found = true;
            }
        }

        // Fall back to DB and warm the cache on hit.
        if (!found)
        {
            std::lock_guard<std::mutex> dbLock(*m_dbMutex);
            if (LoadFromDb(key, rec))
            {
                found = true;
                std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
                m_cache[key] = rec;
            }
        }

        // Default-constructed record (all zeros, empty strings) when
        // not found -- callers can detect this via entityKey.empty().
        out.push_back(found ? rec : InferenceRecord{});
    }

    return out;
}

std::string InferenceEngine::NormalizeEntityKey(const std::wstring& widePath)
{
    return NormalizeKey(widePath);
}

std::string InferenceEngine::HandleQueryInferences(
    const std::vector<std::string>& paths,
    const std::vector<std::string>& fields)
{
    int64_t now = static_cast<int64_t>(std::time(nullptr));

    // Determine which fields to emit
    bool allFields = fields.empty();
    auto hasField = [&](const char* f) -> bool {
        if (allFields) return true;
        for (const auto& s : fields)
            if (s == f) return true;
        return false;
    };

    std::ostringstream oss;
    oss << "{\"now\":" << now << ",\"results\":{";

    bool first = true;
    for (const auto& rawPath : paths)
    {
        // Normalize to lowercase for lookup
        std::string key = rawPath;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });

        InferenceRecord rec;
        bool found = false;

        {
            std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
            auto it = m_cache.find(key);
            if (it != m_cache.end())
            {
                rec = it->second;
                found = true;
            }
        }

        if (!found)
        {
            std::lock_guard<std::mutex> dbLock(*m_dbMutex);
            if (LoadFromDb(key, rec))
            {
                found = true;
                std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
                m_cache[key] = rec;
            }
        }

        if (!first) oss << ",";
        first = false;

        oss << "\"" << EscapeJson(rawPath) << "\":{";

        if (found)
        {
            bool ff = true;
            auto comma = [&]() { if (!ff) oss << ","; ff = false; };

            if (hasField("entity_type"))     { comma(); oss << "\"entity_type\":\"" << EscapeJson(rec.entityType) << "\""; }
            if (hasField("last_event_ts"))   { comma(); oss << "\"last_event_ts\":" << rec.lastEventTs; }
            if (hasField("last_open_ts"))    { comma(); oss << "\"last_open_ts\":" << rec.lastOpenTs; }
            if (hasField("last_edit_ts"))    { comma(); oss << "\"last_edit_ts\":" << rec.lastEditTs; }
            if (hasField("open_count_7d"))   { comma(); oss << "\"open_count_7d\":"   << static_cast<int64_t>(llround(rec.openCount7d)); }
            if (hasField("open_count_30d"))  { comma(); oss << "\"open_count_30d\":"  << static_cast<int64_t>(llround(rec.openCount30d)); }
            if (hasField("open_count_total")){ comma(); oss << "\"open_count_total\":"<< static_cast<int64_t>(llround(rec.openCountTotal)); }
            if (hasField("recency_score"))   { comma(); oss << "\"recency_score\":" << rec.recencyScore; }
            if (hasField("version"))         { comma(); oss << "\"version\":" << rec.version; }
            if (hasField("updated_at"))      { comma(); oss << "\"updated_at\":" << rec.updatedAt; }
        }

        oss << "}";
    }

    oss << "}}";
    return oss.str();
}

std::string InferenceEngine::HandleGetInferenceDeltas(uint32_t sinceVersion)
{
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    std::vector<InferenceRecord> results;

    {
        std::lock_guard<std::mutex> dbLock(*m_dbMutex);
        if (!m_dbHandle || !*m_dbHandle)
            return "{\"now\":0,\"deltas\":[]}";

        const char* sql =
            "SELECT entity_key, entity_type, last_event_ts, last_open_ts, last_edit_ts,"
            "       open_count_7d, open_count_30d, open_count_total,"
            "       recency_score, version, updated_at"
            " FROM inference WHERE version > ? ORDER BY version ASC LIMIT 5000;";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(*m_dbHandle, sql, -1, &stmt, nullptr) != SQLITE_OK)
            return "{\"now\":0,\"deltas\":[]}";

        sqlite3_bind_int(stmt, 1, static_cast<int>(sinceVersion));

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            InferenceRecord r;
            const char* ek = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* et = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            r.entityKey      = ek ? ek : "";
            r.entityType     = et ? et : "";
            r.lastEventTs    = sqlite3_column_int64(stmt, 2);
            r.lastOpenTs     = sqlite3_column_int64(stmt, 3);
            r.lastEditTs     = sqlite3_column_int64(stmt, 4);
            r.openCount7d    = sqlite3_column_double(stmt, 5);
            r.openCount30d   = sqlite3_column_double(stmt, 6);
            r.openCountTotal = sqlite3_column_double(stmt, 7);
            r.recencyScore   = sqlite3_column_double(stmt, 8);
            r.version        = static_cast<uint32_t>(sqlite3_column_int(stmt, 9));
            r.updatedAt      = sqlite3_column_int64(stmt, 10);
            results.push_back(r);
        }
        sqlite3_finalize(stmt);
    }

    std::ostringstream oss;
    oss << "{\"now\":" << now << ",\"deltas\":[";

    for (size_t i = 0; i < results.size(); ++i)
    {
        const auto& r = results[i];
        if (i > 0) oss << ",";
        oss << "{\"entity_key\":\"" << EscapeJson(r.entityKey) << "\""
            << ",\"entity_type\":\"" << EscapeJson(r.entityType) << "\""
            << ",\"last_event_ts\":" << r.lastEventTs
            << ",\"last_open_ts\":" << r.lastOpenTs
            << ",\"last_edit_ts\":" << r.lastEditTs
            << ",\"open_count_7d\":"    << static_cast<int64_t>(llround(r.openCount7d))
            << ",\"open_count_30d\":"   << static_cast<int64_t>(llround(r.openCount30d))
            << ",\"open_count_total\":" << static_cast<int64_t>(llround(r.openCountTotal))
            << ",\"recency_score\":" << r.recencyScore
            << ",\"version\":" << r.version
            << ",\"updated_at\":" << r.updatedAt
            << "}";
    }

    oss << "]}";
    return oss.str();
}

void InferenceEngine::ClearCache()
{
    std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
    m_cache.clear();
}

void InferenceEngine::RefreshRollingCounts()
{
    if (!m_dbHandle || !*m_dbHandle || !m_dbMutex)
        return;

    int64_t now = static_cast<int64_t>(std::time(nullptr));
    int64_t cutoff7d  = now - 7LL * 24 * 3600;
    int64_t cutoff30d = now - 30LL * 24 * 3600;

    // Collect fresh 7d/30d OPEN counts from the three raw-event tables.
    // file_activity: count OPEN actions per path (lowercased)
    // app_launch_activity: every row is an "open" per exe_path (lowercased)
    // browsing_activity: every row is a "visit" per url or title (lowercased)
    struct Counts { double c7d = 0.0; double c30d = 0.0; };
    std::unordered_map<std::string, Counts> freshCounts;

    std::lock_guard<std::mutex> dbLock(*m_dbMutex);

    // --- File OPEN counts (sum of per-event confidence) ---
    {
        const char* sql =
            "SELECT LOWER(path), "
            "  SUM(CASE WHEN timestamp >= ? THEN COALESCE(confidence, 1.0) ELSE 0 END), "
            "  SUM(CASE WHEN timestamp >= ? THEN COALESCE(confidence, 1.0) ELSE 0 END) "
            "FROM file_activity WHERE action = 'OPEN' AND timestamp >= ? "
            "GROUP BY LOWER(path);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(*m_dbHandle, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(stmt, 1, cutoff7d);
            sqlite3_bind_int64(stmt, 2, cutoff30d);
            sqlite3_bind_int64(stmt, 3, cutoff30d);
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                const char* k = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (!k) continue;
                auto& c = freshCounts[k];
                c.c7d  = sqlite3_column_double(stmt, 1);
                c.c30d = sqlite3_column_double(stmt, 2);
            }
            sqlite3_finalize(stmt);
        }
    }

    // --- App launch counts (sum of per-event confidence) ---
    {
        const char* sql =
            "SELECT LOWER(exe_path), "
            "  SUM(CASE WHEN timestamp >= ? THEN COALESCE(confidence, 1.0) ELSE 0 END), "
            "  SUM(CASE WHEN timestamp >= ? THEN COALESCE(confidence, 1.0) ELSE 0 END) "
            "FROM app_launch_activity WHERE timestamp >= ? "
            "GROUP BY LOWER(exe_path);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(*m_dbHandle, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(stmt, 1, cutoff7d);
            sqlite3_bind_int64(stmt, 2, cutoff30d);
            sqlite3_bind_int64(stmt, 3, cutoff30d);
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                const char* k = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (!k) continue;
                auto& c = freshCounts[k];
                c.c7d  = sqlite3_column_double(stmt, 1);
                c.c30d = sqlite3_column_double(stmt, 2);
            }
            sqlite3_finalize(stmt);
        }
    }

    // --- App focus counts (sum of per-event confidence; aggregated with launch) ---
    {
        const char* sql =
            "SELECT LOWER(exe_path), "
            "  SUM(CASE WHEN timestamp >= ? THEN COALESCE(confidence, 1.0) ELSE 0 END), "
            "  SUM(CASE WHEN timestamp >= ? THEN COALESCE(confidence, 1.0) ELSE 0 END) "
            "FROM app_focus_activity WHERE timestamp >= ? "
            "GROUP BY LOWER(exe_path);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(*m_dbHandle, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(stmt, 1, cutoff7d);
            sqlite3_bind_int64(stmt, 2, cutoff30d);
            sqlite3_bind_int64(stmt, 3, cutoff30d);
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                const char* k = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (!k) continue;
                auto& c = freshCounts[k];
                c.c7d  += sqlite3_column_double(stmt, 1);
                c.c30d += sqlite3_column_double(stmt, 2);
            }
            sqlite3_finalize(stmt);
        }
    }

    // --- Browsing counts (sum of per-event confidence; key = url else title) ---
    {
        const char* sql =
            "SELECT LOWER(CASE WHEN url IS NOT NULL AND url != '' THEN url ELSE title END), "
            "  SUM(CASE WHEN timestamp >= ? THEN COALESCE(confidence, 1.0) ELSE 0 END), "
            "  SUM(CASE WHEN timestamp >= ? THEN COALESCE(confidence, 1.0) ELSE 0 END) "
            "FROM browsing_activity WHERE timestamp >= ? "
            "GROUP BY LOWER(CASE WHEN url IS NOT NULL AND url != '' THEN url ELSE title END);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(*m_dbHandle, sql, -1, &stmt, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(stmt, 1, cutoff7d);
            sqlite3_bind_int64(stmt, 2, cutoff30d);
            sqlite3_bind_int64(stmt, 3, cutoff30d);
            while (sqlite3_step(stmt) == SQLITE_ROW)
            {
                const char* k = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (!k) continue;
                auto& c = freshCounts[k];
                c.c7d  = sqlite3_column_double(stmt, 1);
                c.c30d = sqlite3_column_double(stmt, 2);
            }
            sqlite3_finalize(stmt);
        }
    }

    // --- Walk all inference records and patch counts + recency_score ---
    {
        const char* selSql =
            "SELECT entity_key, entity_type, last_event_ts, last_open_ts, last_edit_ts,"
            "       open_count_7d, open_count_30d, open_count_total,"
            "       recency_score, version, updated_at"
            " FROM inference;";
        sqlite3_stmt* selStmt = nullptr;
        if (sqlite3_prepare_v2(*m_dbHandle, selSql, -1, &selStmt, nullptr) != SQLITE_OK)
            return;

        const char* updSql =
            "UPDATE inference SET open_count_7d=?, open_count_30d=?, "
            "recency_score=?, version=version+1, updated_at=? "
            "WHERE entity_key=?;";
        sqlite3_stmt* updStmt = nullptr;
        if (sqlite3_prepare_v2(*m_dbHandle, updSql, -1, &updStmt, nullptr) != SQLITE_OK)
        {
            sqlite3_finalize(selStmt);
            return;
        }

        while (sqlite3_step(selStmt) == SQLITE_ROW)
        {
            const char* ek = reinterpret_cast<const char*>(sqlite3_column_text(selStmt, 0));
            if (!ek) continue;
            std::string key(ek);

            double  old7d  = sqlite3_column_double(selStmt, 5);
            double  old30d = sqlite3_column_double(selStmt, 6);
            int64_t lastOpenTs = sqlite3_column_int64(selStmt, 3);

            auto it = freshCounts.find(key);
            double new7d  = it != freshCounts.end() ? it->second.c7d  : 0.0;
            double new30d = it != freshCounts.end() ? it->second.c30d : 0.0;

            // Tolerate small drift (rounding noise) before re-writing.
            const double kEpsilon = 1e-6;
            if (fabs(new7d - old7d) > kEpsilon || fabs(new30d - old30d) > kEpsilon)
            {
                double score = ComputeRecencyScore(now, lastOpenTs, new7d);
                sqlite3_reset(updStmt);
                sqlite3_bind_double(updStmt, 1, new7d);
                sqlite3_bind_double(updStmt, 2, new30d);
                sqlite3_bind_double(updStmt, 3, score);
                sqlite3_bind_int64(updStmt, 4, now);
                sqlite3_bind_text(updStmt, 5, key.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(updStmt);
            }
        }

        sqlite3_finalize(selStmt);
        sqlite3_finalize(updStmt);
    }

    // Invalidate in-memory cache so subsequent queries pick up fresh data
    {
        std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
        m_cache.clear();
    }
}
