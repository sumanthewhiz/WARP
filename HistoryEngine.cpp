#include "framework.h"
#include "HistoryEngine.h"
#include "Attention.h"
#include <stdexcept>
#include "sqlite3.h"
#include "EntityKey.h"
#include "Json.h"
#include "Logger.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <set>

using namespace StoreUtil;

namespace
{
    const int64_t kThreadLookbackSec = 14LL * 86400;   // threads that can still be resumed
    const double  kMemberTauSec      = 2.0 * 86400.0;  // member weights fade over days
    const double  kEdgeTauSec        = 14.0 * 86400.0; // co-occurrence fades over weeks
    const double  kJoinThreshold     = 0.25;           // weighted Jaccard to join / resume a thread
    const double  kStayThreshold     = 0.12;           // hysteresis: keep the current thread
    const size_t  kMaxMembers        = 40;
    const size_t  kMaxCachedThreads  = 300;
    const size_t  kEdgeTopEntities   = 8;

    std::string ColU(sqlite3_stmt* st, int i)
    {
        const unsigned char* t = sqlite3_column_text(st, i);
        return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    }

    std::wstring ColW(sqlite3_stmt* st, int i)
    {
        return U2W(reinterpret_cast<const char*>(sqlite3_column_text(st, i)));
    }

    void BindU(sqlite3_stmt* st, int i, const std::string& s)
    {
        if (s.empty()) sqlite3_bind_null(st, i);
        else sqlite3_bind_text(st, i, s.c_str(), (int)s.size(), SQLITE_TRANSIENT);
    }

    std::vector<float> BlobToVec(sqlite3_stmt* st, int i)
    {
        std::vector<float> v;
        const void* p = sqlite3_column_blob(st, i);
        int n = sqlite3_column_bytes(st, i);
        if (p && n > 0 && n % (int)sizeof(float) == 0)
        {
            v.resize((size_t)n / sizeof(float));
            memcpy(v.data(), p, (size_t)n);
        }
        return v;
    }

    double Dot(const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.empty() || a.size() != b.size()) return 0.0;
        double s = 0.0;
        for (size_t i = 0; i < a.size(); ++i) s += (double)a[i] * (double)b[i];
        return s;
    }

    void Normalize(std::vector<float>& v)
    {
        double n = 0.0;
        for (float f : v) n += (double)f * f;
        n = std::sqrt(n);
        if (n > 1e-9) for (float& f : v) f = (float)(f / n);
    }

    std::wstring Lower(const std::wstring& s)
    {
        std::wstring o(s);
        for (wchar_t& c : o) c = (wchar_t)towlower(c);
        return o;
    }

    std::vector<std::wstring> SplitPath(const std::wstring& p, wchar_t sep)
    {
        std::vector<std::wstring> parts;
        std::wstring cur;
        for (wchar_t c : p)
        {
            if (c == sep) { if (!cur.empty()) parts.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) parts.push_back(cur);
        return parts;
    }

    // Folder -> project anchor cache shared by writer + readers.
    std::mutex g_projMtx;
    std::unordered_map<std::wstring, std::string> g_projCache;

    std::wstring EnvPath(const wchar_t* name)
    {
        wchar_t buf[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return std::wstring();
        std::wstring s = Lower(buf);
        while (!s.empty() && s.back() == L'\\') s.pop_back();
        return s;
    }

    std::string FolderAnchor(const std::wstring& dirLower)
    {
        {
            std::lock_guard<std::mutex> lk(g_projMtx);
            auto it = g_projCache.find(dirLower);
            if (it != g_projCache.end()) return it->second;
        }
        std::string res;
        DWORD a = GetFileAttributesW((dirLower + L"\\.git").c_str());
        if (a != INVALID_FILE_ATTRIBUTES)
            res = "git:" + EntityKey::Key(dirLower);
        else
        {
            static const wchar_t* const kMarkers[] = { L"\\*.sln", L"\\*.slnx", L"\\*.code-workspace" };
            for (const wchar_t* m : kMarkers)
            {
                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileExW((dirLower + m).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
                if (h != INVALID_HANDLE_VALUE)
                {
                    FindClose(h);
                    res = "ws:" + EntityKey::Key(dirLower);
                    break;
                }
            }
        }
        std::lock_guard<std::mutex> lk(g_projMtx);
        if (g_projCache.size() > 5000) g_projCache.clear();
        g_projCache[dirLower] = res;
        return res;
    }

    // Weighted Jaccard between a normalized snapshot vector and a thread's members.
    double WeightedJaccard(const std::unordered_map<std::string, double>& snap,
                           const std::unordered_map<std::string, double>& members)
    {
        double mTotal = 0.0;
        for (const auto& kv : members) mTotal += kv.second;
        if (mTotal <= 0.0 || snap.empty()) return 0.0;
        double num = 0.0, den = 0.0;
        for (const auto& kv : snap)
        {
            auto it = members.find(kv.first);
            double b = it == members.end() ? 0.0 : it->second / mTotal;
            num += (std::min)(kv.second, b);
            den += (std::max)(kv.second, b);
        }
        for (const auto& kv : members)
            if (snap.find(kv.first) == snap.end()) den += kv.second / mTotal;
        return den > 0.0 ? num / den : 0.0;
    }
}

HistoryEngine::HistoryEngine(ActivityStore* store) : m_store(store) {}

std::string HistoryEngine::CurrentThreadLabel() const
{
    std::lock_guard<std::mutex> lk(m_labelMtx);
    return m_currentLabel;
}

std::string HistoryEngine::ProjectKeyFor(const std::wstring& pathOrUrl, const std::string& entityType)
{
    if (pathOrUrl.empty()) return std::string();
    std::wstring s = Lower(pathOrUrl);

    if (entityType == "url" || s.compare(0, 4, L"http") == 0)
    {
        size_t schemeEnd = s.find(L"://");
        if (schemeEnd == std::wstring::npos) return std::string();
        std::wstring rest = s.substr(schemeEnd + 3);
        size_t q = rest.find_first_of(L"?#");
        if (q != std::wstring::npos) rest = rest.substr(0, q);
        std::vector<std::wstring> seg = SplitPath(rest, L'/');
        if (seg.empty()) return std::string();
        const std::wstring& host = seg[0];
        auto join = [&](size_t n) -> std::string
        {
            std::wstring k;
            for (size_t i = 0; i < n && i < seg.size(); ++i) { if (i) k += L'/'; k += seg[i]; }
            return EntityKey::Key(k);
        };
        if ((host == L"github.com" || host == L"gitlab.com" || host == L"bitbucket.org") && seg.size() >= 3)
            return "repo:" + join(3);
        if (host == L"dev.azure.com" && seg.size() >= 5 && seg[3] == L"_git")
            return "repo:" + EntityKey::Key(host + L"/" + seg[1] + L"/" + seg[2] + L"/" + seg[4]);
        if (host.size() > 17 && host.compare(host.size() - 17, 17, L".visualstudio.com") == 0 && seg.size() >= 4 && seg[2] == L"_git")
            return "repo:" + EntityKey::Key(host + L"/" + seg[1] + L"/" + seg[3]);
        if (host.find(L".sharepoint.com") != std::wstring::npos && seg.size() >= 3 && (seg[1] == L"sites" || seg[1] == L"teams"))
            return "sp:" + join(3);
        return std::string();
    }
    if (entityType != "file") return std::string();

    // OneDrive / SharePoint synced libraries: first folder under the sync root.
    static const wchar_t* const kRoots[] = { L"OneDriveCommercial", L"OneDriveConsumer", L"OneDrive" };
    for (const wchar_t* r : kRoots)
    {
        std::wstring root = EnvPath(r);
        if (root.empty() || s.size() <= root.size() + 1 || s.compare(0, root.size(), root) != 0 || s[root.size()] != L'\\')
            continue;
        std::wstring rel = s.substr(root.size() + 1);
        size_t slash = rel.find(L'\\');
        if (slash == std::wstring::npos) return std::string();            // file directly in the root
        return "od:" + EntityKey::Key(root + L"\\" + rel.substr(0, slash));
    }

    // Walk up looking for a repository / workspace marker (cached per folder). Never on network
    // paths: this runs on the store's writer thread inside the open transaction, and an offline
    // share can block a file-system probe for tens of seconds.
    std::wstring profile = EnvPath(L"USERPROFILE");
    std::wstring dir = s;
    size_t cut = dir.find_last_of(L'\\');
    if (cut == std::wstring::npos) return std::string();
    dir = dir.substr(0, cut);
    std::wstring firstParent = dir;
    bool remote = s.compare(0, 2, L"\\\\") == 0;
    if (!remote && s.size() >= 3 && s[1] == L':' && s[2] == L'\\')
    {
        const wchar_t root[4] = { s[0], L':', L'\\', L'\0' };
        remote = GetDriveTypeW(root) == DRIVE_REMOTE;
    }
    for (int depth = 0; !remote && depth < 10 && dir.size() > 3; ++depth)
    {
        if (!profile.empty() && dir == profile) break;
        std::string a = FolderAnchor(dir);
        if (!a.empty()) return a;
        size_t p = dir.find_last_of(L'\\');
        if (p == std::wstring::npos || p < 2) break;
        dir = dir.substr(0, p);
    }

    // Fallback: the containing folder when it is a specific place (not a profile/known root).
    size_t depthBelow = 0;
    if (!profile.empty() && firstParent.compare(0, profile.size(), profile) == 0)
        depthBelow = SplitPath(firstParent.substr(profile.size()), L'\\').size();
    else
        depthBelow = SplitPath(firstParent, L'\\').size() - 1;            // minus drive
    if (depthBelow >= 2) return "dir:" + EntityKey::Key(firstParent);
    return std::string();
}

// =====================================================================
// Writer side
// =====================================================================
void HistoryEngine::ResetCaches()
{
    m_threads.clear();
    m_threadsLoaded = false;
    m_spanThread = m_spanStart = m_spanEnd = 0;
}

void HistoryEngine::EnsureThreadsLoaded(WriterContext& w)
{
    if (m_threadsLoaded) return;
    m_threadsLoaded = true;
    int64_t cutoff = w.nowMs / 1000 - kThreadLookbackSec;
    sqlite3_stmt* st = w.Stmt("SELECT id, label, first_ts, last_ts, active_secs, project_key, centroid FROM work_thread "
                              "WHERE last_ts >= ?1 ORDER BY last_ts DESC LIMIT 300;");
    if (!st) return;
    sqlite3_bind_int64(st, 1, cutoff);
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        ThreadState t;
        t.id = sqlite3_column_int64(st, 0);
        t.label = ColU(st, 1);
        t.firstTs = sqlite3_column_int64(st, 2);
        t.lastTs = sqlite3_column_int64(st, 3);
        t.activeSecs = sqlite3_column_double(st, 4);
        t.projectKey = ColU(st, 5);
        t.centroid = BlobToVec(st, 6);
        m_threads[t.id] = std::move(t);
    }
    sqlite3_stmt* mem = w.Stmt("SELECT m.entity_key, m.weight, COALESCE(i.entity_type, '') FROM thread_member m "
                               "LEFT JOIN inference i ON i.entity_key = m.entity_key WHERE m.thread_id = ?1;");
    if (!mem) return;
    for (auto& kv : m_threads)
    {
        sqlite3_reset(mem);
        sqlite3_bind_int64(mem, 1, kv.first);
        while (sqlite3_step(mem) == SQLITE_ROW)
        {
            std::string k = ColU(mem, 0);
            kv.second.members[k] = sqlite3_column_double(mem, 1);
            kv.second.memberTypes[k] = ColU(mem, 2);
        }
    }
}

void HistoryEngine::PersistThread(WriterContext& w, ThreadState& t)
{
    int64_t seq = w.NextChangeSeq();
    const void* blob = t.centroid.empty() ? nullptr : t.centroid.data();
    int blobLen = (int)(t.centroid.size() * sizeof(float));
    if (t.id == 0)
    {
        sqlite3_stmt* st = w.Stmt("INSERT INTO work_thread (label, first_ts, last_ts, active_secs, project_key, centroid, change_seq) "
                                  "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);");
        if (!st) return;
        BindU(st, 1, t.label);
        sqlite3_bind_int64(st, 2, t.firstTs);
        sqlite3_bind_int64(st, 3, t.lastTs);
        sqlite3_bind_double(st, 4, t.activeSecs);
        BindU(st, 5, t.projectKey);
        if (blob) sqlite3_bind_blob(st, 6, blob, blobLen, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 6);
        sqlite3_bind_int64(st, 7, seq);
        if (sqlite3_step(st) != SQLITE_DONE) return;
        t.id = sqlite3_last_insert_rowid(w.db);
    }
    else
    {
        sqlite3_stmt* st = w.Stmt("UPDATE work_thread SET label = ?2, first_ts = ?3, last_ts = ?4, active_secs = ?5, project_key = ?6, "
                                  "centroid = ?7, change_seq = ?8 WHERE id = ?1;");
        if (!st) return;
        sqlite3_bind_int64(st, 1, t.id);
        BindU(st, 2, t.label);
        sqlite3_bind_int64(st, 3, t.firstTs);
        sqlite3_bind_int64(st, 4, t.lastTs);
        sqlite3_bind_double(st, 5, t.activeSecs);
        BindU(st, 6, t.projectKey);
        if (blob) sqlite3_bind_blob(st, 7, blob, blobLen, SQLITE_TRANSIENT); else sqlite3_bind_null(st, 7);
        sqlite3_bind_int64(st, 8, seq);
        sqlite3_step(st);
    }
    if (t.dirtyMembers)
    {
        sqlite3_stmt* del = w.Stmt("DELETE FROM thread_member WHERE thread_id = ?1;");
        if (del) { sqlite3_bind_int64(del, 1, t.id); sqlite3_step(del); }
        sqlite3_stmt* ins = w.Stmt("INSERT OR REPLACE INTO thread_member (thread_id, entity_key, weight, last_ts) VALUES (?1, ?2, ?3, ?4);");
        if (ins)
        {
            for (const auto& kv : t.members)
            {
                sqlite3_reset(ins);
                sqlite3_bind_int64(ins, 1, t.id);
                BindU(ins, 2, kv.first);
                sqlite3_bind_double(ins, 3, kv.second);
                sqlite3_bind_int64(ins, 4, t.lastTs);
                sqlite3_step(ins);
            }
        }
        t.dirtyMembers = false;
    }
}

void HistoryEngine::ExtendSpan(WriterContext& w, int64_t threadId, int64_t ts, double activeSecs)
{
    if (threadId == 0) return;
    bool newSpan = m_spanThread != threadId || m_spanStart == 0 || ts - m_spanEnd > kSessionGapSec;
    if (newSpan)
    {
        m_spanThread = threadId;
        m_spanStart = ts;
        m_spanEnd = ts;
        sqlite3_stmt* st = w.Stmt("INSERT OR IGNORE INTO thread_span (thread_id, start_ts, end_ts, active_secs) VALUES (?1, ?2, ?2, ?3);");
        if (!st) return;
        sqlite3_bind_int64(st, 1, threadId);
        sqlite3_bind_int64(st, 2, ts);
        sqlite3_bind_double(st, 3, activeSecs);
        sqlite3_step(st);
        return;
    }
    if (ts > m_spanEnd) m_spanEnd = ts;
    sqlite3_stmt* st = w.Stmt("UPDATE thread_span SET end_ts = MAX(end_ts, ?3), active_secs = active_secs + ?4 WHERE thread_id = ?1 AND start_ts = ?2;");
    if (!st) return;
    sqlite3_bind_int64(st, 1, threadId);
    sqlite3_bind_int64(st, 2, m_spanStart);
    sqlite3_bind_int64(st, 3, m_spanEnd);
    sqlite3_bind_double(st, 4, activeSecs);
    sqlite3_step(st);
}

void HistoryEngine::UpdateEdges(WriterContext& w, const SnapshotInput& in, double scale)
{
    if (scale <= 0.0) return;
    std::vector<const SnapshotEntity*> top;
    for (const SnapshotEntity& e : in.entities)
        if (!e.entityKey.empty() && e.activeSecs > 0) top.push_back(&e);
    std::sort(top.begin(), top.end(), [](const SnapshotEntity* a, const SnapshotEntity* b) { return a->activeSecs > b->activeSecs; });
    if (top.size() > kEdgeTopEntities) top.resize(kEdgeTopEntities);
    if (top.size() < 2) return;
    double maxAct = top.front()->activeSecs;
    int64_t ts = in.ts;
    sqlite3_stmt* sel = w.Stmt("SELECT weight, last_ts FROM edge WHERE a = ?1 AND b = ?2;");
    sqlite3_stmt* up = w.Stmt("INSERT INTO edge (a, b, weight, last_ts) VALUES (?1, ?2, ?3, ?4) "
                              "ON CONFLICT(a, b) DO UPDATE SET weight = excluded.weight, last_ts = excluded.last_ts;");
    if (!sel || !up) return;
    for (size_t i = 0; i < top.size(); ++i)
        for (size_t j = i + 1; j < top.size(); ++j)
        {
            const std::string& k1 = top[i]->entityKey;
            const std::string& k2 = top[j]->entityKey;
            if (k1 == k2) continue;
            const std::string& a = k1 < k2 ? k1 : k2;
            const std::string& b = k1 < k2 ? k2 : k1;
            double inc = scale * (std::min)(top[i]->activeSecs, top[j]->activeSecs) / maxAct;
            double weight = inc;
            sqlite3_reset(sel);
            BindU(sel, 1, a);
            BindU(sel, 2, b);
            if (sqlite3_step(sel) == SQLITE_ROW)
            {
                double old = sqlite3_column_double(sel, 0);
                int64_t oldTs = sqlite3_column_int64(sel, 1);
                weight += old * std::exp(-(double)(std::max<int64_t>)(0, ts - oldTs) / kEdgeTauSec);
            }
            sqlite3_reset(up);
            BindU(up, 1, a);
            BindU(up, 2, b);
            sqlite3_bind_double(up, 3, weight);
            sqlite3_bind_int64(up, 4, ts);
            sqlite3_step(up);
        }
}

int64_t HistoryEngine::AssignSnapshot(WriterContext& w, const SnapshotInput& in)
{
    // Composed before a clear-all / purge: its entities may be the ones just deleted.
    if (in.generation != UINT64_MAX && in.generation != m_generation.load())
        return m_currentThread.load();
    EnsureThreadsLoaded(w);
    const int64_t ts = in.ts > 0 ? in.ts : w.nowMs / 1000;

    std::unordered_map<std::string, double> snap;
    std::unordered_map<std::string, std::string> snapTypes;
    double total = 0.0;
    for (const SnapshotEntity& e : in.entities)
    {
        if (e.entityKey.empty()) continue;
        double v = (std::max)(1.0, e.activeSecs);
        snap[e.entityKey] += v;
        snapTypes[e.entityKey] = e.entityType;
        total += v;
    }

    int64_t chosen = m_currentThread.load();
    if (!snap.empty())
    {
        for (auto& kv : snap) kv.second /= total;

        int64_t cur = m_currentThread.load();
        int64_t bestId = 0;
        double bestJ = 0.0, bestScore = -1.0, curJ = 0.0;
        for (auto& kv : m_threads)
        {
            ThreadState& t = kv.second;
            if (ts - t.lastTs > kThreadLookbackSec) continue;
            double j = WeightedJaccard(snap, t.members);
            // Label-embedding similarity only breaks ties (granite's cosine floor is high: never an absolute test).
            double score = j + (in.embedding.empty() ? 0.0 : 0.05 * Dot(in.embedding, t.centroid));
            if (t.id == cur) curJ = j;
            if (score > bestScore) { bestScore = score; bestJ = j; bestId = t.id; }
        }

        ThreadState* target = nullptr;
        bool isNew = false;
        if (bestId != 0 && bestJ >= (bestId == cur ? kStayThreshold : kJoinThreshold))
            target = &m_threads[bestId];
        else if (cur != 0 && curJ >= kStayThreshold && m_threads.count(cur))
            target = &m_threads[cur];
        else
        {
            ThreadState fresh;
            fresh.firstTs = ts;
            fresh.lastTs = ts;
            PersistThread(w, fresh);                       // allocates the id
            if (fresh.id == 0) return cur;
            int64_t id = fresh.id;
            m_threads[id] = std::move(fresh);
            target = &m_threads[id];
            isNew = true;
        }

        ThreadState& t = *target;
        double j = isNew ? 1.0 : WeightedJaccard(snap, t.members);
        double decay = std::exp(-(double)(std::max<int64_t>)(0, ts - t.lastTs) / kMemberTauSec);
        for (auto& m : t.members) m.second *= decay;
        for (const SnapshotEntity& e : in.entities)
        {
            if (e.entityKey.empty()) continue;
            t.members[e.entityKey] += (std::max)(1.0, e.activeSecs) / 60.0;
            t.memberTypes[e.entityKey] = e.entityType;
        }
        // Keep the strongest members only.
        if (t.members.size() > kMaxMembers || !t.members.empty())
        {
            std::vector<std::pair<std::string, double>> v(t.members.begin(), t.members.end());
            std::sort(v.begin(), v.end(), [](const std::pair<std::string, double>& a, const std::pair<std::string, double>& b) { return a.second > b.second; });
            double maxW = v.front().second;
            std::unordered_map<std::string, double> kept;
            for (size_t i = 0; i < v.size() && kept.size() < kMaxMembers; ++i)
                if (v[i].second >= 0.02 * maxW) kept.insert(v[i]);
            t.members.swap(kept);
            for (auto it = t.memberTypes.begin(); it != t.memberTypes.end();)
                it = t.members.count(it->first) ? std::next(it) : t.memberTypes.erase(it);
        }
        t.dirtyMembers = true;
        if (ts > t.lastTs) t.lastTs = ts;
        if (t.firstTs == 0 || ts < t.firstTs) t.firstTs = ts;
        if (!in.label.empty() && (isNew || t.label.empty() || j >= 0.35)) t.label = in.label;
        if (!in.embedding.empty())
        {
            if (t.centroid.size() != in.embedding.size()) t.centroid = in.embedding;
            else
            {
                for (size_t i = 0; i < t.centroid.size(); ++i) t.centroid[i] = 0.8f * t.centroid[i] + 0.2f * in.embedding[i];
                Normalize(t.centroid);
            }
        }
        // Project: member-weighted vote over stable anchors.
        {
            std::unordered_map<std::string, double> votes;
            double all = 0.0;
            for (const auto& m : t.members)
            {
                auto ty = t.memberTypes.find(m.first);
                std::string type = ty == t.memberTypes.end() ? std::string() : ty->second;
                if (type != "file" && type != "url") continue;
                all += m.second;
                std::string pk = ProjectKeyFor(U2W(m.first.c_str()), type);
                if (!pk.empty()) votes[pk] += m.second;
            }
            std::string best;
            double bestV = 0.0;
            for (const auto& v : votes) if (v.second > bestV) { bestV = v.second; best = v.first; }
            if (!best.empty() && all > 0.0 && bestV / all >= 0.3) t.projectKey = best;
        }
        PersistThread(w, t);
        chosen = t.id;
        ExtendSpan(w, t.id, ts, 0.0);
        m_currentThread = t.id;
        {
            std::lock_guard<std::mutex> lk(m_labelMtx);
            m_currentLabel = t.label;
        }

        double scale = m_lastSnapshotTs ? (std::min)(5.0, (std::max)(0.0, (double)(ts - m_lastSnapshotTs) / 60.0)) : 1.0;
        UpdateEdges(w, in, scale);
        m_lastSnapshotTs = ts;

        if (m_threads.size() > kMaxCachedThreads)
        {
            int64_t oldestId = 0, oldestTs = INT64_MAX;
            for (const auto& kv : m_threads)
                if (kv.first != chosen && kv.second.lastTs < oldestTs) { oldestTs = kv.second.lastTs; oldestId = kv.first; }
            if (oldestId) m_threads.erase(oldestId);
        }
    }

    if (in.persist && !in.json.empty())
    {
        sqlite3_stmt* st = w.Stmt("INSERT INTO context_snapshot (ts, window_secs, json, thread_id, embedding, model) VALUES (?1, ?2, ?3, ?4, ?5, ?6);");
        if (st)
        {
            sqlite3_bind_int64(st, 1, ts);
            sqlite3_bind_int64(st, 2, in.windowSecs);
            sqlite3_bind_text(st, 3, in.json.c_str(), (int)in.json.size(), SQLITE_TRANSIENT);
            if (chosen) sqlite3_bind_int64(st, 4, chosen); else sqlite3_bind_null(st, 4);
            if (!in.embedding.empty())
                sqlite3_bind_blob(st, 5, in.embedding.data(), (int)(in.embedding.size() * sizeof(float)), SQLITE_TRANSIENT);
            else
                sqlite3_bind_null(st, 5);
            BindU(st, 6, in.model);
            sqlite3_step(st);
        }
    }
    return chosen;
}

int64_t HistoryEngine::RecordSnapshot(const SnapshotInput& in, DWORD timeoutMs)
{
    auto result = std::make_shared<int64_t>(0);
    SnapshotInput copy = in;
    bool ok = m_store->RunSync([this, copy, result](WriterContext& w) { *result = AssignSnapshot(w, copy); }, timeoutMs);
    return ok ? *result : m_currentThread.load();
}

void HistoryEngine::ExtendSession(WriterContext& w, int64_t startTs, int64_t endTs, double activeSecs, double passiveSecs)
{
    if (endTs <= startTs || (activeSecs <= 0 && passiveSecs <= 0)) return;
    auto done = [&](sqlite3_stmt* st) {
        if (!st || sqlite3_step(st) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(w.db));
        sqlite3_reset(st);
    };
    const double wall = static_cast<double>(endTs - startTs);
    std::vector<Attention::Span> spans{ { startTs, endTs, activeSecs / wall, passiveSecs / wall } };
    sqlite3_stmt* st = w.Stmt("SELECT start_ts, end_ts, active_rate, passive_rate FROM session_attention WHERE end_ts >= ?1 AND start_ts <= ?2;");
    if (!st) throw std::runtime_error(sqlite3_errmsg(w.db));
    sqlite3_bind_int64(st, 1, startTs);
    sqlite3_bind_int64(st, 2, endTs);
    while (sqlite3_step(st) == SQLITE_ROW)
        spans.push_back({ sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1),
                          sqlite3_column_double(st, 2), sqlite3_column_double(st, 3) });
    sqlite3_reset(st);
    st = w.Stmt("DELETE FROM session_attention WHERE end_ts >= ?1 AND start_ts <= ?2;");
    sqlite3_bind_int64(st, 1, startTs);
    sqlite3_bind_int64(st, 2, endTs);
    done(st);
    for (const auto& span : Attention::Union(spans))
    {
        st = w.Stmt("INSERT INTO session_attention (start_ts, end_ts, active_rate, passive_rate) VALUES (?1, ?2, ?3, ?4);");
        sqlite3_bind_int64(st, 1, span.start);
        sqlite3_bind_int64(st, 2, span.end);
        sqlite3_bind_double(st, 3, span.active);
        sqlite3_bind_double(st, 4, span.passive);
        done(st);
    }

    // Merge every neighboring session, including a late interval bridging two sessions.
    // The durable union makes replay, arrival order and agent restarts idempotent.
    std::map<int64_t, int> joined;
    int version = 1;
    bool expanded;
    do
    {
        expanded = false;
        st = w.Stmt("SELECT id, start_ts, end_ts, accounting_version FROM work_session WHERE end_ts > ?1 AND start_ts < ?2;");
        sqlite3_bind_int64(st, 1, startTs - kSessionGapSec);
        sqlite3_bind_int64(st, 2, endTs + kSessionGapSec);
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            const int64_t id = sqlite3_column_int64(st, 0);
            if (joined.count(id)) continue;
            const int64_t s = sqlite3_column_int64(st, 1), e = sqlite3_column_int64(st, 2);
            if ((startTs > e && m_store->PurgedBetween(e, startTs)) ||
                (s > endTs && m_store->PurgedBetween(endTs, s))) continue;
            const int v = sqlite3_column_int(st, 3);
            joined[id] = v;
            version = (std::min)(version, v);
            startTs = (std::min)(startTs, s);
            endTs = (std::max)(endTs, e);
            expanded = true;
        }
        sqlite3_reset(st);
    } while (expanded);

    st = w.Stmt("SELECT COALESCE(SUM((MIN(end_ts, ?2) - MAX(start_ts, ?1)) * active_rate), 0), "
                "COALESCE(SUM((MIN(end_ts, ?2) - MAX(start_ts, ?1)) * passive_rate), 0) "
                "FROM session_attention WHERE end_ts > ?1 AND start_ts < ?2;");
    sqlite3_bind_int64(st, 1, startTs);
    sqlite3_bind_int64(st, 2, endTs);
    if (sqlite3_step(st) != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(w.db));
    activeSecs = sqlite3_column_double(st, 0);
    passiveSecs = sqlite3_column_double(st, 1);
    sqlite3_reset(st);
    const int64_t id = joined.empty() ? 0 : joined.begin()->first;
    st = id ? w.Stmt("UPDATE work_session SET start_ts=?2, end_ts=?3, active_secs=?4, passive_secs=?5, accounting_version=?6 WHERE id=?1;")
            : w.Stmt("INSERT INTO work_session (id, start_ts, end_ts, active_secs, passive_secs, accounting_version) VALUES (NULL, ?2, ?3, ?4, ?5, ?6);");
    if (id) sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_int64(st, 2, startTs);
    sqlite3_bind_int64(st, 3, endTs);
    sqlite3_bind_double(st, 4, activeSecs);
    sqlite3_bind_double(st, 5, passiveSecs);
    sqlite3_bind_int(st, 6, version);
    done(st);
    for (const auto& other : joined) if (other.first != id)
    {
        st = w.Stmt("DELETE FROM work_session WHERE id=?1;");
        sqlite3_bind_int64(st, 1, other.first);
        done(st);
    }
}

void HistoryEngine::OnFocusEvent(WriterContext& w, const FocusEventRecord& r)
{
    if (r.activeSecs <= 0 && r.passiveSecs <= 0) return;
    int64_t endTs = r.endMs / 1000;
    int64_t startTs = r.startMs > 0 ? r.startMs / 1000 : endTs - r.activeSecs;
    ExtendSession(w, startTs, endTs, (double)(std::max)(0, r.activeSecs), (double)(std::max)(0, r.passiveSecs));
    if (r.activeSecs <= 0) return;

    int64_t cur = m_currentThread.load();
    if (cur == 0) return;
    auto it = m_threads.find(cur);
    if (it == m_threads.end()) return;
    it->second.activeSecs += r.activeSecs;
    sqlite3_stmt* st = w.Stmt("UPDATE work_thread SET active_secs = active_secs + ?2 WHERE id = ?1;");
    if (st)
    {
        sqlite3_bind_int64(st, 1, cur);
        sqlite3_bind_double(st, 2, (double)r.activeSecs);
        sqlite3_step(st);
    }
    if (m_spanThread == cur) ExtendSpan(w, cur, (std::max)(endTs, m_spanEnd), (double)r.activeSecs);
}

void HistoryEngine::OnPresence(WriterContext& w, const PresenceRecord& r)
{
    int64_t s = r.startMs / 1000;
    int64_t e = (r.endMs > 0 ? r.endMs : w.nowMs) / 1000;
    if (r.state == "active")
        ExtendSession(w, s, e, (double)(e - s), 0.0);
    else if (r.state == "passive")
        ExtendSession(w, s, e, 0.0, (double)(e - s));
}

void HistoryEngine::OnMaintenance(WriterContext&, int64_t nowUtc)
{
    for (auto it = m_threads.begin(); it != m_threads.end();)
    {
        if (nowUtc - it->second.lastTs > kThreadLookbackSec && it->first != m_currentThread.load())
            it = m_threads.erase(it);
        else
            ++it;
    }
}

void HistoryEngine::OnClearAll(WriterContext&)
{
    ++m_generation;
    ResetCaches();
    m_currentThread = 0;
    m_lastSnapshotTs = 0;
    std::lock_guard<std::mutex> lk(m_labelMtx);
    m_currentLabel.clear();
}

void HistoryEngine::OnRollback(WriterContext& w)
{
    ++m_generation;
    ResetCaches();
    // A purge may have deleted the current thread (or its creation was rolled back).
    const int64_t cur = m_currentThread.load();
    if (cur != 0)
    {
        bool exists = false;
        if (sqlite3_stmt* st = w.Stmt("SELECT 1 FROM work_thread WHERE id = ?1;"))
        {
            sqlite3_bind_int64(st, 1, cur);
            exists = sqlite3_step(st) == SQLITE_ROW;
            sqlite3_reset(st);                       // no read transaction left open on the writer connection
        }
        if (!exists) m_currentThread = 0;
    }
    // A purge may have rewritten the current thread's label (or deleted the thread); the next snapshot
    // publishes the label again.
    std::lock_guard<std::mutex> lk(m_labelMtx);
    m_currentLabel.clear();
}

void HistoryEngine::OnEntityRemoved(WriterContext&, const std::string& entityKey)
{
    for (auto& kv : m_threads)
    {
        kv.second.members.erase(entityKey);
        kv.second.memberTypes.erase(entityKey);
    }
}

// =====================================================================
// Read side
// =====================================================================
std::vector<WorkThreadInfo> HistoryEngine::GetThreads(int64_t startUtc, int64_t endUtc, int limit, bool includeMembers)
{
    std::vector<WorkThreadInfo> out;
    limit = (std::max)(1, (std::min)(limit, 500));
    if (endUtc <= 0) endUtc = INT64_MAX;
    ActivityStore::ReadLease rl = m_store->AcquireReader();
    if (!rl) return out;
    sqlite3* db = rl.get();

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, label, first_ts, last_ts, active_secs, project_key, change_seq FROM work_thread "
                               "WHERE last_ts >= ?1 AND first_ts <= ?2 ORDER BY last_ts DESC LIMIT ?3;", -1, &st, nullptr) != SQLITE_OK)
        return out;
    sqlite3_stmt* sp = nullptr;
    sqlite3_prepare_v2(db, "SELECT start_ts, end_ts, active_secs FROM thread_span WHERE thread_id = ?1 AND end_ts >= ?2 AND start_ts <= ?3 "
                           "ORDER BY start_ts LIMIT 200;", -1, &sp, nullptr);
    sqlite3_stmt* mem = nullptr;
    if (includeMembers)
        sqlite3_prepare_v2(db, "SELECT m.entity_key, m.weight, i.entity_type, i.display_name, i.title FROM thread_member m "
                               "LEFT JOIN inference i ON i.entity_key = m.entity_key AND i.deleted = 0 "
                               "WHERE m.thread_id = ?1 ORDER BY m.weight DESC LIMIT 15;", -1, &mem, nullptr);

    sqlite3_bind_int64(st, 1, startUtc);
    sqlite3_bind_int64(st, 2, endUtc);
    sqlite3_bind_int(st, 3, limit * 2);
    int64_t cur = m_currentThread.load();
    while (sqlite3_step(st) == SQLITE_ROW && (int)out.size() < limit)
    {
        WorkThreadInfo t;
        t.id = sqlite3_column_int64(st, 0);
        t.label = ColU(st, 1);
        t.firstTs = sqlite3_column_int64(st, 2);
        t.lastTs = sqlite3_column_int64(st, 3);
        t.activeSecs = sqlite3_column_double(st, 4);
        t.projectKey = ColU(st, 5);
        t.changeSeq = sqlite3_column_int64(st, 6);
        t.current = t.id == cur;
        if (sp)
        {
            sqlite3_reset(sp);
            sqlite3_bind_int64(sp, 1, t.id);
            sqlite3_bind_int64(sp, 2, startUtc);
            sqlite3_bind_int64(sp, 3, endUtc);
            while (sqlite3_step(sp) == SQLITE_ROW)
            {
                t.spans.emplace_back(sqlite3_column_int64(sp, 0), sqlite3_column_int64(sp, 1));
                t.activeSecsInRange += sqlite3_column_double(sp, 2);
            }
            if (startUtc > 0 && t.spans.empty()) continue;      // touched the range only between spans
        }
        if (mem)
        {
            sqlite3_reset(mem);
            sqlite3_bind_int64(mem, 1, t.id);
            while (sqlite3_step(mem) == SQLITE_ROW)
            {
                WorkThreadMember m;
                m.entityKey = ColU(mem, 0);
                m.weight = sqlite3_column_double(mem, 1);
                m.entityType = ColU(mem, 2);
                m.display = ColW(mem, 3);
                m.title = ColW(mem, 4);
                if (m.entityType.empty()) continue;                // entity since deleted
                t.members.push_back(std::move(m));
            }
        }
        out.push_back(std::move(t));
    }
    sqlite3_finalize(st);
    if (sp) sqlite3_finalize(sp);
    if (mem) sqlite3_finalize(mem);
    return out;
}

std::vector<WorkSessionInfo> HistoryEngine::GetSessions(int64_t startUtc, int64_t endUtc, int limit)
{
    std::vector<WorkSessionInfo> out;
    limit = (std::max)(1, (std::min)(limit, 1000));
    if (endUtc <= 0) endUtc = INT64_MAX;
    ActivityStore::ReadLease rl = m_store->AcquireReader();
    if (!rl) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(rl.get(), "SELECT id, start_ts, end_ts, active_secs, passive_secs, accounting_version FROM work_session "
                                     "WHERE end_ts >= ?1 AND start_ts <= ?2 ORDER BY start_ts DESC LIMIT ?3;", -1, &st, nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_int64(st, 1, startUtc);
    sqlite3_bind_int64(st, 2, endUtc);
    sqlite3_bind_int(st, 3, limit);
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        WorkSessionInfo s;
        s.id = sqlite3_column_int64(st, 0);
        s.startTs = sqlite3_column_int64(st, 1);
        s.endTs = sqlite3_column_int64(st, 2);
        s.activeSecs = sqlite3_column_int(st, 3);
        s.passiveSecs = sqlite3_column_int(st, 4);
        s.accountingVersion = sqlite3_column_int(st, 5);
        out.push_back(s);
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<RelatedEntity> HistoryEngine::Related(const std::string& entityKey, int limit, int64_t nowUtc)
{
    std::vector<RelatedEntity> out;
    if (entityKey.empty()) return out;
    if (nowUtc == 0) nowUtc = NowUtc();
    limit = (std::max)(1, (std::min)(limit, 100));
    ActivityStore::ReadLease rl = m_store->AcquireReader();
    if (!rl) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(rl.get(),
        "SELECT CASE WHEN e.a = ?1 THEN e.b ELSE e.a END AS other, e.weight, e.last_ts, i.entity_type, i.display_name "
        "FROM edge e JOIN inference i ON i.entity_key = (CASE WHEN e.a = ?1 THEN e.b ELSE e.a END) AND i.deleted = 0 "
        "WHERE e.a = ?1 OR e.b = ?1;", -1, &st, nullptr) != SQLITE_OK)
        return out;
    BindU(st, 1, entityKey);
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        RelatedEntity r;
        r.entityKey = ColU(st, 0);
        r.lastTs = sqlite3_column_int64(st, 2);
        r.weight = sqlite3_column_double(st, 1) * std::exp(-(double)(std::max<int64_t>)(0, nowUtc - r.lastTs) / kEdgeTauSec);
        r.entityType = ColU(st, 3);
        r.display = ColW(st, 4);
        out.push_back(std::move(r));
    }
    sqlite3_finalize(st);
    std::sort(out.begin(), out.end(), [](const RelatedEntity& a, const RelatedEntity& b) { return a.weight > b.weight; });
    if ((int)out.size() > limit) out.resize((size_t)limit);
    return out;
}

std::vector<double> HistoryEngine::ContextBoost(const std::vector<std::string>& keys, int64_t nowUtc)
{
    std::vector<double> out(keys.size(), 0.0);
    int64_t cur = m_currentThread.load();
    if (cur == 0 || keys.empty()) return out;
    if (nowUtc == 0) nowUtc = NowUtc();
    ActivityStore::ReadLease rl = m_store->AcquireReader();
    if (!rl) return out;
    sqlite3* db = rl.get();

    std::unordered_map<std::string, double> members;
    double maxW = 0.0;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT entity_key, weight FROM thread_member WHERE thread_id = ?1 ORDER BY weight DESC LIMIT 20;", -1, &st, nullptr) != SQLITE_OK)
            return out;
        sqlite3_bind_int64(st, 1, cur);
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            double wgt = sqlite3_column_double(st, 1);
            members[ColU(st, 0)] = wgt;
            maxW = (std::max)(maxW, wgt);
        }
        sqlite3_finalize(st);
    }
    if (members.empty() || maxW <= 0.0) return out;

    sqlite3_stmt* ed = nullptr;
    sqlite3_prepare_v2(db, "SELECT CASE WHEN a = ?1 THEN b ELSE a END, weight, last_ts FROM edge WHERE a = ?1 OR b = ?1;", -1, &ed, nullptr);
    for (size_t i = 0; i < keys.size(); ++i)
    {
        const std::string& k = keys[i];
        if (k.empty()) continue;
        auto m = members.find(k);
        if (m != members.end())
        {
            out[i] = 0.5 + 0.5 * (m->second / maxW);
            continue;
        }
        if (!ed) continue;
        double s = 0.0;
        sqlite3_reset(ed);
        BindU(ed, 1, k);
        while (sqlite3_step(ed) == SQLITE_ROW)
        {
            auto mm = members.find(ColU(ed, 0));
            if (mm == members.end()) continue;
            double wgt = sqlite3_column_double(ed, 1) *
                         std::exp(-(double)(std::max<int64_t>)(0, nowUtc - sqlite3_column_int64(ed, 2)) / kEdgeTauSec);
            s += wgt * (mm->second / maxW);
        }
        out[i] = 0.5 * (1.0 - std::exp(-s));
    }
    if (ed) sqlite3_finalize(ed);
    return out;
}

std::vector<std::string> HistoryEngine::ContextSnapshotsJson(int count, int64_t sinceUtc)
{
    std::vector<std::string> out;
    count = (std::max)(1, (std::min)(count, 1440));
    ActivityStore::ReadLease rl = m_store->AcquireReader();
    if (!rl) return out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(rl.get(), "SELECT json, thread_id FROM context_snapshot WHERE ts >= ?1 ORDER BY ts DESC, id DESC LIMIT ?2;", -1, &st, nullptr) != SQLITE_OK)
        return out;
    sqlite3_bind_int64(st, 1, sinceUtc);
    sqlite3_bind_int(st, 2, count);
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        std::string json = ColU(st, 0);
        // The JSON is serialized before the snapshot is assigned to a thread: patch in the real id
        // (top level and every current_entities[] item).
        if (sqlite3_column_type(st, 1) != SQLITE_NULL)
        {
            WarpJson::ParseResult pr = WarpJson::Parse(json);
            if (pr.ok && pr.value.isObject())
            {
                const int64_t tid = sqlite3_column_int64(st, 1);
                pr.value.set("thread_id", WarpJson::Value::Int(tid));
                const WarpJson::Value& ents = pr.value.get("current_entities");
                if (ents.isArray())
                {
                    WarpJson::Value patched = WarpJson::Value::Array();
                    for (const WarpJson::Value& item : ents.items())
                    {
                        WarpJson::Value copy = item;
                        if (copy.isObject()) copy.set("thread_id", WarpJson::Value::Int(tid));
                        patched.push(std::move(copy));
                    }
                    pr.value.set("current_entities", std::move(patched));
                }
                json = WarpJson::Serialize(pr.value);
            }
        }
        out.push_back(std::move(json));
    }
    sqlite3_finalize(st);
    return out;
}
