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

double InferenceEngine::ComputeRecencyScore(int64_t now, int64_t lastOpenTs, int openCount7d)
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
        out.openCount7d    = sqlite3_column_int(stmt, 5);
        out.openCount30d   = sqlite3_column_int(stmt, 6);
        out.openCountTotal = sqlite3_column_int(stmt, 7);
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
    sqlite3_bind_int(stmt, 6, rec.openCount7d);
    sqlite3_bind_int(stmt, 7, rec.openCount30d);
    sqlite3_bind_int(stmt, 8, rec.openCountTotal);
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
                                   int64_t eventTs)
{
    if (path.empty()) return;

    std::string key = NormalizeKey(path);
    std::string actionUtf8 = WideToUtf8(action);

    std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
    InferenceRecord& rec = LoadOrCreate(key, "file");

    rec.lastEventTs = eventTs;

    if (actionUtf8 == "OPEN")
    {
        rec.lastOpenTs = eventTs;
        rec.openCount7d++;
        rec.openCount30d++;
        rec.openCountTotal++;
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

void InferenceEngine::OnAppLaunchEvent(const std::wstring& exePath, int64_t eventTs)
{
    if (exePath.empty()) return;

    std::string key = NormalizeKey(exePath);

    std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
    InferenceRecord& rec = LoadOrCreate(key, "app");

    rec.lastEventTs    = eventTs;
    rec.lastOpenTs     = eventTs;
    rec.openCount7d++;
    rec.openCount30d++;
    rec.openCountTotal++;
    rec.recencyScore   = ComputeRecencyScore(eventTs, rec.lastOpenTs, rec.openCount7d);
    rec.version++;
    rec.updatedAt      = eventTs;

    {
        std::lock_guard<std::mutex> dbLock(*m_dbMutex);
        PersistRecord(rec);
    }
}

void InferenceEngine::OnBrowsingEvent(const std::wstring& url, int64_t eventTs)
{
    if (url.empty()) return;

    std::string key = NormalizeKey(url);

    std::lock_guard<std::mutex> cacheLock(m_cacheMutex);
    InferenceRecord& rec = LoadOrCreate(key, "url");

    rec.lastEventTs    = eventTs;
    rec.lastOpenTs     = eventTs;
    rec.openCount7d++;
    rec.openCount30d++;
    rec.openCountTotal++;
    rec.recencyScore   = ComputeRecencyScore(eventTs, rec.lastOpenTs, rec.openCount7d);
    rec.version++;
    rec.updatedAt      = eventTs;

    {
        std::lock_guard<std::mutex> dbLock(*m_dbMutex);
        PersistRecord(rec);
    }
}

// ===================== Query Handlers =====================

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
            if (hasField("open_count_7d"))   { comma(); oss << "\"open_count_7d\":" << rec.openCount7d; }
            if (hasField("open_count_30d"))  { comma(); oss << "\"open_count_30d\":" << rec.openCount30d; }
            if (hasField("open_count_total")){ comma(); oss << "\"open_count_total\":" << rec.openCountTotal; }
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
            r.openCount7d    = sqlite3_column_int(stmt, 5);
            r.openCount30d   = sqlite3_column_int(stmt, 6);
            r.openCountTotal = sqlite3_column_int(stmt, 7);
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
            << ",\"open_count_7d\":" << r.openCount7d
            << ",\"open_count_30d\":" << r.openCount30d
            << ",\"open_count_total\":" << r.openCountTotal
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
