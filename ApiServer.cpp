#include "framework.h"
#include "ApiServer.h"
#include "ActivityStore.h"
#include "EntityStore.h"
#include "EntityKey.h"
#include "HistoryEngine.h"
#include "ContextInference.h"
#include "Settings.h"
#include "Logger.h"

#include <sddl.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <set>

#pragma comment(lib, "advapi32.lib")

using WarpJson::Value;
using WarpJson::Writer;

namespace
{
    const int    kWorkerCount     = 8;
    const size_t kMaxQueued       = 64;
    const int    kMaxLongPolls    = 4;
    const DWORD  kFirstReadMs     = 5000;
    const DWORD  kIdleReadMs      = 10000;    // keep-alive: also lets slow clients finish reading
    const DWORD  kWriteMs         = 30000;
    const int    kMaxRequestsPerConnection = 256;

    int64_t NowMs()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
    }
    int64_t NowUtc() { return NowMs() / 1000; }

    std::wstring LowerW(std::wstring s)
    {
        for (wchar_t& c : s) c = (wchar_t)std::towlower(c);
        return s;
    }

    std::wstring FileNameOf(const std::wstring& path)
    {
        size_t p = path.find_last_of(L"\\/");
        return p == std::wstring::npos ? path : path.substr(p + 1);
    }

    bool ListContains(const std::vector<std::wstring>& list, const std::wstring& lowerName)
    {
        for (const std::wstring& s : list)
            if (LowerW(s) == lowerName) return true;
        return false;
    }

    std::wstring CurrentUserSid(bool& elevated)
    {
        elevated = false;
        std::wstring out;
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return out;
        DWORD len = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
        std::vector<BYTE> buf(len ? len : 1);
        if (len && GetTokenInformation(tok, TokenUser, buf.data(), len, &len))
        {
            LPWSTR s = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s))
            {
                out = s;
                LocalFree(s);
            }
        }
        TOKEN_ELEVATION te = {};
        DWORD rl = 0;
        if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &rl)) elevated = te.TokenIsElevated != 0;
        CloseHandle(tok);
        return out;
    }

    std::wstring SelfExePath()
    {
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
        return std::wstring(buf, n);
    }
}

ApiClient ApiClient::InProcess()
{
    ApiClient c;
    c.inProcess = true;
    c.pid = GetCurrentProcessId();
    ProcessIdToSessionId(c.pid, &c.sessionId);
    c.activityScope = true;
    c.controlScope = true;
    return c;
}

ApiServer::ApiServer()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_legacy.evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_session.evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

ApiServer::~ApiServer()
{
    Stop();
    if (m_legacy.evt) CloseHandle(m_legacy.evt);
    if (m_session.evt) CloseHandle(m_session.evt);
    if (m_stopEvent) CloseHandle(m_stopEvent);
    if (m_sd) LocalFree(m_sd);
}

std::wstring ApiServer::SessionPipeName(DWORD sessionId)
{
    return std::wstring(LegacyPipeName()) + L"-" + std::to_wstring(sessionId);
}

void ApiServer::Configure(const Deps& deps)
{
    m_deps = deps;
    m_startedUtc = NowUtc();
    ProcessIdToSessionId(GetCurrentProcessId(), &m_sessionId);
    m_selfExe = SelfExePath();
    m_legacy.name = LegacyPipeName();
    m_legacy.legacy = true;
    m_session.name = SessionPipeName(m_sessionId);
    m_session.legacy = false;
}

bool ApiServer::Start(const Deps& deps)
{
    if (m_running.load()) return true;
    Configure(deps);

    // Explicit DACL (API-1): network denied first; SYSTEM/Administrators full; the owning user (and,
    // by policy, AppContainer clients) get FILE_GENERIC_READ | FILE_GENERIC_WRITE (0x12019F) - what
    // every standard client requests (GENERIC_READ|GENERIC_WRITE, .NET NamedPipeClientStream InOut).
    // That includes FILE_CREATE_PIPE_INSTANCE, so name squatting is handled by
    // FILE_FLAG_FIRST_PIPE_INSTANCE at startup, remote rejection, caller identification and
    // clients checking GetNamedPipeServerProcessId. Medium label (low when packaged AppContainer
    // clients are allowed by policy).
    bool elevated = false;
    std::wstring sid = CurrentUserSid(elevated);
    (void)elevated;
    WarpSettings s = Settings::Instance().Get();
    std::wstring sddl = L"D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;BA)";
    if (!sid.empty())
        sddl += L"(A;;0x12019f;;;" + sid + L")";
    if (s.apiAllowAppContainerClients) sddl += L"(A;;0x12019f;;;AC)";
    sddl += s.apiAllowAppContainerClients ? L"S:(ML;;NW;;;LW)" : L"S:(ML;;NW;;;ME)";
    if (m_sd) { LocalFree(m_sd); m_sd = nullptr; }
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &m_sd, nullptr))
    {
        WARP_LOG_WARN("api", "explicit pipe security descriptor rejected (%lu); retrying without label", GetLastError());
        std::wstring noLabel = sddl.substr(0, sddl.find(L"S:"));
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(noLabel.c_str(), SDDL_REVISION_1, &m_sd, nullptr))
        {
            WARP_LOG_ERROR("api", "cannot build pipe security descriptor (%lu); API disabled", GetLastError());
            return false;
        }
    }

    ResetEvent(m_stopEvent);
    m_running = true;
    for (int i = 0; i < kWorkerCount; ++i)
        m_workers.emplace_back(&ApiServer::WorkerLoop, this);
    m_acceptThread = std::thread(&ApiServer::AcceptLoop, this);
    WARP_LOG_INFO("api", "API v%d listening on %S and %S", kApiVersion, m_session.name.c_str(), m_legacy.name.c_str());
    return true;
}

void ApiServer::Stop()
{
    if (!m_running.exchange(false)) return;
    SetEvent(m_stopEvent);
    if (m_acceptThread.joinable()) m_acceptThread.join();
    {
        std::lock_guard<std::mutex> lk(m_qMtx);   // no worker may sit between its predicate check and wait()
    }
    m_qCv.notify_all();
    for (std::thread& t : m_workers)
        if (t.joinable()) t.join();
    m_workers.clear();
    std::lock_guard<std::mutex> lk(m_qMtx);
    for (Conn& c : m_queue)
    {
        DisconnectNamedPipe(c.pipe);
        CloseHandle(c.pipe);
        if (c.listener) --c.listener->live;
    }
    m_queue.clear();
}

ApiServer::Stats ApiServer::GetStats() const
{
    std::lock_guard<std::mutex> lk(m_statsMtx);
    return m_stats;
}

// =====================================================================
// Listening (API-2)
// =====================================================================
bool ApiServer::CreateInstance(Listener& l)
{
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const bool first = l.live.load() == 0;
        DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
        SECURITY_ATTRIBUTES sa = { sizeof(sa), m_sd, FALSE };
        HANDLE h = CreateNamedPipeW(l.name.c_str(), openMode,
                                    PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &sa);
        if (h == INVALID_HANDLE_VALUE)
        {
            DWORD e = GetLastError();
            if (first && (e == ERROR_ACCESS_DENIED || e == ERROR_PIPE_BUSY))
            {
                if (l.owned || l.retryAtMs == 0)
                    WARP_LOG_WARN("api", "pipe %S is owned by another process (another WARP instance or a squatter)", l.name.c_str());
                l.owned = false;
            }
            else
            {
                WARP_LOG_WARN("api", "CreateNamedPipe(%S) failed: %lu", l.name.c_str(), e);
            }
            l.retryAtMs = NowMs() + (first ? 10000 : 1000);
            std::lock_guard<std::mutex> lk(m_statsMtx);
            (l.legacy ? m_stats.legacyPipeOwned : m_stats.sessionPipeOwned) = l.owned;
            return false;
        }
        if (first && !l.owned) WARP_LOG_INFO("api", "serving %S", l.name.c_str());
        l.owned = true;
        ++l.live;
        {
            std::lock_guard<std::mutex> lk(m_statsMtx);
            (l.legacy ? m_stats.legacyPipeOwned : m_stats.sessionPipeOwned) = true;
        }
        l.pipe = h;
        l.preConnected = false;
        ZeroMemory(&l.ov, sizeof(l.ov));
        l.ov.hEvent = l.evt;
        ResetEvent(l.evt);
        if (ConnectNamedPipe(h, &l.ov))
        {
            l.preConnected = true;
            SetEvent(l.evt);
            return true;
        }
        DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) return true;
        if (e == ERROR_PIPE_CONNECTED)
        {
            l.preConnected = true;          // client connected between create and connect
            SetEvent(l.evt);
            return true;
        }
        // ERROR_NO_DATA: a client connected and already left (e.g. an availability probe). Re-arm.
        DisconnectNamedPipe(h);
        CloseHandle(h);
        --l.live;
        l.pipe = INVALID_HANDLE_VALUE;
        if (e != ERROR_NO_DATA)
        {
            WARP_LOG_WARN("api", "ConnectNamedPipe(%S) failed: %lu", l.name.c_str(), e);
            l.retryAtMs = NowMs() + 1000;
            return false;
        }
    }
    l.retryAtMs = NowMs() + 100;
    return false;
}

void ApiServer::AcceptLoop()
{
    SetThreadDescription(GetCurrentThread(), L"WARP api accept");
    Listener* ls[2] = { &m_session, &m_legacy };
    for (Listener* l : ls) CreateInstance(*l);

    while (m_running.load())
    {
        HANDLE hs[3];
        Listener* who[3];
        DWORD n = 0;
        hs[n] = m_stopEvent; who[n++] = nullptr;
        for (Listener* l : ls)
            if (l->pipe != INVALID_HANDLE_VALUE) { hs[n] = l->evt; who[n++] = l; }
        DWORD wr = WaitForMultipleObjects(n, hs, FALSE, 1000);
        if (!m_running.load() || wr == WAIT_OBJECT_0) break;
        if (wr > WAIT_OBJECT_0 && wr < WAIT_OBJECT_0 + n)
        {
            Listener* l = who[wr - WAIT_OBJECT_0];
            DWORD dummy = 0;
            BOOL ok = l->preConnected ? TRUE : GetOverlappedResult(l->pipe, &l->ov, &dummy, FALSE);
            HANDLE connected = l->pipe;
            l->pipe = INVALID_HANDLE_VALUE;
            CreateInstance(*l);                     // re-arm BEFORE serving (API-2)
            if (!ok)
            {
                DisconnectNamedPipe(connected);
                CloseHandle(connected);
                --l->live;
            }
            else
            {
                bool queued = false;
                {
                    std::lock_guard<std::mutex> lk(m_qMtx);
                    if (m_queue.size() < kMaxQueued)
                    {
                        m_queue.push_back(Conn{ connected, l });
                        queued = true;
                    }
                }
                if (queued) m_qCv.notify_one();
                else
                {
                    // Tell the client why instead of a silent disconnect; closing (not disconnecting)
                    // keeps the reply readable after this end is gone.
                    HANDLE evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    if (evt)
                    {
                        WriteMessage(connected, evt, Error("busy", "the server is busy; retry shortly"), 1000);
                        CloseHandle(evt);
                    }
                    CloseHandle(connected);
                    --l->live;
                    std::lock_guard<std::mutex> lk(m_statsMtx);
                    ++m_stats.busy;
                }
            }
        }
        int64_t now = NowMs();
        for (Listener* l : ls)
            if (l->pipe == INVALID_HANDLE_VALUE && now >= l->retryAtMs) CreateInstance(*l);
    }

    for (Listener* l : ls)
    {
        if (l->pipe == INVALID_HANDLE_VALUE) continue;
        CancelIoEx(l->pipe, &l->ov);
        DWORD dummy = 0;
        GetOverlappedResult(l->pipe, &l->ov, &dummy, TRUE);
        CloseHandle(l->pipe);
        l->pipe = INVALID_HANDLE_VALUE;
        --l->live;
    }
}

void ApiServer::WorkerLoop()
{
    SetThreadDescription(GetCurrentThread(), L"WARP api worker");
    for (;;)
    {
        Conn c{};
        {
            std::unique_lock<std::mutex> lk(m_qMtx);
            m_qCv.wait(lk, [this] { return !m_queue.empty() || !m_running.load(); });
            if (m_queue.empty()) return;
            c = m_queue.front();
            m_queue.pop_front();
        }
        {
            std::lock_guard<std::mutex> lk(m_statsMtx);
            ++m_stats.activeConnections;
        }
        Serve(c.pipe, c.listener);
        std::lock_guard<std::mutex> lk(m_statsMtx);
        --m_stats.activeConnections;
    }
}

// =====================================================================
// Per-connection I/O (API-3, API-5)
// =====================================================================
bool ApiServer::ReadMessage(HANDLE pipe, HANDLE evt, std::string& out, DWORD timeoutMs, bool& tooLarge)
{
    out.clear();
    tooLarge = false;
    std::vector<char> buf(65536);
    for (;;)
    {
        OVERLAPPED ov = {};
        ov.hEvent = evt;
        ResetEvent(evt);
        DWORD n = 0;
        BOOL ok = ReadFile(pipe, buf.data(), (DWORD)buf.size(), &n, &ov);
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && err == ERROR_IO_PENDING)
        {
            HANDLE hs[2] = { evt, m_stopEvent };
            DWORD wr = WaitForMultipleObjects(2, hs, FALSE, timeoutMs);
            if (wr != WAIT_OBJECT_0)
            {
                CancelIoEx(pipe, &ov);
                GetOverlappedResult(pipe, &ov, &n, TRUE);
                return false;
            }
            ok = GetOverlappedResult(pipe, &ov, &n, FALSE);
            err = ok ? ERROR_SUCCESS : GetLastError();
        }
        if (n > 0)
        {
            if (out.size() + n > kMaxRequestBytes)
            {
                tooLarge = true;
                return false;
            }
            out.append(buf.data(), n);
        }
        if (ok) return !out.empty();
        if (err == ERROR_MORE_DATA) continue;      // rest of the same message
        return false;                              // ERROR_BROKEN_PIPE: client closed
    }
}

bool ApiServer::WriteMessage(HANDLE pipe, HANDLE evt, const std::string& data, DWORD timeoutMs)
{
    OVERLAPPED ov = {};
    ov.hEvent = evt;
    ResetEvent(evt);
    DWORD n = 0;
    BOOL ok = WriteFile(pipe, data.data(), (DWORD)data.size(), &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
    {
        HANDLE hs[2] = { evt, m_stopEvent };
        DWORD wr = WaitForMultipleObjects(2, hs, FALSE, timeoutMs);
        if (wr != WAIT_OBJECT_0)
        {
            CancelIoEx(pipe, &ov);
            GetOverlappedResult(pipe, &ov, &n, TRUE);
            return false;
        }
        ok = GetOverlappedResult(pipe, &ov, &n, FALSE);
    }
    return ok && n == data.size();
}

ApiClient ApiServer::IdentifyClient(HANDLE pipe, bool legacy, std::string& rejectCode)
{
    ApiClient c;
    rejectCode.clear();
    GetNamedPipeClientProcessId(pipe, &c.pid);
    if (!GetNamedPipeClientSessionId(pipe, &c.sessionId)) c.sessionId = (DWORD)-1;
    if (c.pid)
    {
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, c.pid);
        if (hp)
        {
            wchar_t buf[MAX_PATH * 2] = {};
            DWORD len = (DWORD)(sizeof(buf) / sizeof(buf[0]));
            if (QueryFullProcessImageNameW(hp, 0, buf, &len)) c.exePath.assign(buf, len);
            CloseHandle(hp);
        }
    }
    WarpSettings s = Settings::Instance().Get();
    std::wstring name = LowerW(FileNameOf(c.exePath));
    if (legacy && c.sessionId != m_sessionId)
        rejectCode = "wrong_session";
    else if (!s.apiAllowedClients.empty() && (name.empty() || !ListContains(s.apiAllowedClients, name)))
        rejectCode = "unauthorized";
    c.activityScope = !s.apiRestrictActivityScope || (!name.empty() && ListContains(s.apiActivityClients, name));
    c.controlScope = !c.exePath.empty() && LowerW(c.exePath) == LowerW(m_selfExe);
    return c;
}

void ApiServer::Serve(HANDLE pipe, Listener* l)
{
    HANDLE evt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::string reject;
    ApiClient c = IdentifyClient(pipe, l && l->legacy, reject);
    if (!reject.empty())
    {
        std::lock_guard<std::mutex> lk(m_statsMtx);
        ++m_stats.rejected;
    }
    for (int i = 0; i < kMaxRequestsPerConnection && m_running.load(); ++i)
    {
        std::string req;
        bool tooLarge = false;
        if (!ReadMessage(pipe, evt, req, i == 0 ? kFirstReadMs : kIdleReadMs, tooLarge))
        {
            if (tooLarge)
                WriteMessage(pipe, evt, Error("request_too_large", "requests are limited to 1 MiB"), 5000);
            break;
        }
        std::string resp;
        if (reject == "wrong_session")
            resp = Error("wrong_session", c.sessionId == (DWORD)-1
                ? std::string("the client's Windows session could not be determined; connect to the per-session pipe "
                              "\\\\.\\pipe\\WarpFileActivityAPI-<SessionId>")
                : "this client runs in another Windows session; connect to " + WarpJson::WideToUtf8(SessionPipeName(c.sessionId)));
        else if (!reject.empty())
            resp = Error("unauthorized", "this client is not in the API allow-list (policy ApiAllowedClients)");
        else
            resp = Handle(req, c);
        if (!WriteMessage(pipe, evt, resp, kWriteMs)) break;
        if (!reject.empty()) break;
        // Keep-alive must not starve queued clients: hand the worker over (the client reconnects).
        bool othersWaiting = false;
        {
            std::lock_guard<std::mutex> lk(m_qMtx);
            othersWaiting = !m_queue.empty();
        }
        if (othersWaiting) break;
    }
    // CloseHandle only: DisconnectNamedPipe would discard a reply the client has not read yet (every
    // connection gets a fresh instance, so there is nothing to reuse).
    CloseHandle(pipe);
    if (evt) CloseHandle(evt);
    if (l) --l->live;
}

// =====================================================================
// Request handling
// =====================================================================
namespace
{
    const char* const kActivityOps[] = {
        "GetInferenceDeltas", "GetRecentContext", "GetRecentContexts", "TopEntities", "SearchActivity", "GetChanges",
        "QueryTimeline", "GetWorkThreads", "GetRelated", "GetCurrentContext",
    };
    const char* const kControlOps[] = { "Purge", "Pause", "Resume" };

    bool In(const char* const* list, size_t n, const std::string& s)
    {
        for (size_t i = 0; i < n; ++i) if (s == list[i]) return true;
        return false;
    }

    int64_t ClampI(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // Core entity fields shared by several ops.
    void WriteEntity(Writer& w, const EntityRecord& r, bool withDisplay)
    {
        w.KV("entity_key", r.entityKey);
        w.KV("entity_type", r.entityType);
        if (withDisplay)
        {
            w.KV("display_name", r.displayName);
            if (!r.title.empty()) w.KV("title", r.title);
        }
        w.KV("last_event_ts", r.lastEventTs);
        w.KV("last_open_ts", r.lastOpenTs);
        w.KV("last_edit_ts", r.lastEditTs);
        w.KV("last_active_ts", r.lastActiveTs);
        w.KV("open_count_7d", (int64_t)std::llround(r.openCount7d));
        w.KV("open_count_30d", (int64_t)std::llround(r.openCount30d));
        w.KV("open_count_total", (int64_t)std::llround(r.openCountTotal));
        w.KV("edit_count_total", (int64_t)std::llround(r.editCountTotal));
        w.KV("active_secs_7d", (int64_t)std::llround(r.activeSecs7d));
        w.KV("active_secs_total", (int64_t)std::llround(r.activeSecsTotal));
        w.KV("distinct_days", (int64_t)r.distinctDays);
        w.KV("recency_score", r.recencyScore);
        w.KV("change_seq", r.changeSeq);
    }

    void WriteScore(Writer& w, const EntityScore& s)
    {
        w.KV("score", s.score);
        w.KV("prior", s.prior);
        w.KV("short_term", s.shortTerm);
        w.KV("long_term", s.longTerm);
        w.Key("reasons").BeginArray();
        for (const std::string& r : s.reasons) w.String(r);
        w.EndArray();
    }

    uint32_t ParseTypes(const Value& v, bool& sessions, bool& threads)
    {
        uint32_t t = 0;
        sessions = threads = false;
        for (const std::string& s : v.asStringArray())
        {
            if (s == "file") t |= EVENT_TYPE_FILE;
            else if (s == "app_launch") t |= EVENT_TYPE_APP_LAUNCH;
            else if (s == "app_focus") t |= EVENT_TYPE_APP_FOCUS;
            else if (s == "browsing") t |= EVENT_TYPE_BROWSING;
            else if (s == "sessions") sessions = true;
            else if (s == "threads") threads = true;
        }
        return t;
    }

    std::string NormalizeRaw(const std::string& utf8)
    {
        return EntityStore::NormalizeKey(WarpJson::Utf8ToWide(utf8));
    }
}

std::string ApiServer::Error(const char* code, const std::string& message)
{
    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("ok", false);
    w.Key("error").BeginObject().KV("code", code).KV("message", message).EndObject();
    w.EndObject();
    return out;
}

std::string ApiServer::WithApiVersion(const std::string& json)
{
    if (json.size() < 2 || json[0] != '{') return json;
    std::string head = "{\"api_version\":" + std::to_string(kApiVersion);
    size_t i = 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) ++i;
    if (i < json.size() && json[i] == '}') return head + "}";
    return head + "," + json.substr(1);
}

std::string ApiServer::Handle(const std::string& request, const ApiClient& client)
{
    {
        std::lock_guard<std::mutex> lk(m_statsMtx);
        ++m_stats.requests;
    }
    std::string resp;
    try
    {
        WarpJson::ParseResult pr = WarpJson::Parse(request, 32);
        if (!pr.ok)
            resp = Error("invalid_json", pr.error + " (offset " + std::to_string(pr.errorOffset) + ")");
        else if (!pr.value.isObject())
            resp = Error("invalid_request", "the request must be a JSON object");
        else
            resp = Dispatch(pr.value, client);
    }
    catch (const std::bad_alloc&) { resp = Error("internal_error", "out of memory"); }
    catch (const std::exception& e) { resp = Error("internal_error", e.what()); }
    catch (...) { resp = Error("internal_error", "unexpected failure"); }

    static const std::string kErrPrefix = "{\"api_version\":" + std::to_string(kApiVersion) + ",\"ok\":false";
    if (resp.compare(0, kErrPrefix.size(), kErrPrefix) == 0)
    {
        std::lock_guard<std::mutex> lk(m_statsMtx);
        ++m_stats.errors;
    }
    return resp;
}

std::string ApiServer::Dispatch(const Value& req, const ApiClient& c)
{
    const std::string op = req.get("op").asString(std::string());

    // Health ops answer even when the store is down - that is exactly when they are needed.
    if (op == "Ping")
    {
        std::string out;
        Writer w(out);
        w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("ok", true).KV("pong", true)
         .KV("now", NowUtc()).KV("version", m_deps.productVersion)
         .KV("store_open", m_deps.store && m_deps.store->IsOpen()).EndObject();
        return out;
    }
    if (op == "GetStatus") return GetStatus(c);

    if (!m_deps.store || !m_deps.store->IsOpen())
        return Error("unavailable", "the activity store is not open");

    if (op.empty())
    {
        // v1 raw-event query: {"window": "15m"} / {"seconds": N} / {"types": [...]}
        if (!c.activityScope) return Error("forbidden", "the activity scope is not granted to this client");
        return RawEvents(req, c);
    }
    if (In(kControlOps, sizeof(kControlOps) / sizeof(kControlOps[0]), op) && !c.controlScope && !c.inProcess)
        return Error("forbidden", "control operations are only available to WARP itself");
    if (In(kActivityOps, sizeof(kActivityOps) / sizeof(kActivityOps[0]), op) && !c.activityScope)
        return Error("forbidden", "the activity scope is not granted to this client (policy ApiActivityClients)");

    if (op == "QueryInferences")    return QueryInferences(req);
    if (op == "GetInferenceDeltas") return GetInferenceDeltas(req);
    if (op == "ScoreEntities")      return ScoreEntities(req, c);
    if (op == "TopEntities")        return TopEntities(req, c);
    if (op == "SearchActivity")     return SearchActivity(req);
    if (op == "GetChanges")         return GetChanges(req, c);
    if (op == "WaitForChanges")     return WaitForChanges(req);
    if (op == "QueryTimeline")      return QueryTimeline(req);
    if (op == "GetWorkThreads")     return GetWorkThreads(req);
    if (op == "GetRelated")         return GetRelated(req);
    if (op == "GetCurrentContext")  return GetCurrentContext(req);
    if (op == "Purge")              return Purge(req);
    if (op == "Pause")              return Pause(req, false);
    if (op == "Resume")             return Pause(req, true);
    if (op == "GetRecentContext" || op == "GetRecentContexts")
    {
        if (!m_deps.context) return Error("unavailable", "context inference is not running");
        std::string category = req.get("category").asString(std::string("all"));
        const Value& wv = req.has("window_seconds") ? req.get("window_seconds") : req.get("window_secs");
        int64_t winSecs = ClampI(wv.asInt64(0), 0, 30LL * 86400);
        bool includeVector = req.get("include_vector").asBool(true);
        if (op == "GetRecentContext")
            return WithApiVersion(m_deps.context->GetRecentContext(category, winSecs, includeVector));
        int count = (int)req.get("count").asInt64(10);
        if (count <= 0) count = 10;
        if (count > 200) count = 200;
        return WithApiVersion(m_deps.context->GetRecentContexts(count, category, winSecs, includeVector));
    }
    return Error("unknown_op", "unknown op '" + op + "'");
}

std::string ApiServer::RawEvents(const Value& req, const ApiClient&)
{
    bool sessions = false, threads = false;
    uint32_t types = ParseTypes(req.get("types"), sessions, threads);
    if (types == 0) types = EVENT_TYPE_ALL;

    int64_t secs = 3600;
    std::string win = req.get("window").asString(std::string());
    if (!win.empty())
    {
        static const std::pair<const char*, int64_t> kWins[] = {
            { "15m", 900 }, { "30m", 1800 }, { "1h", 3600 }, { "2h", 7200 }, { "6h", 21600 },
            { "24h", 86400 }, { "7d", 604800 }, { "15d", 1296000 }, { "30d", 2592000 },
        };
        bool known = false;
        for (const auto& kv : kWins) if (win == kv.first) { secs = kv.second; known = true; }
        if (!known) return Error("invalid_request", "unknown window '" + win + "' (use 15m,30m,1h,2h,6h,24h,7d,15d,30d)");
    }
    else if (req.has("seconds"))
    {
        int64_t v = req.get("seconds").asInt64(0);
        if (v <= 0) return Error("invalid_request", "'seconds' must be a positive integer");
        secs = (std::min)(v, (int64_t)400 * 86400);
    }
    const int limit = (int)ClampI(req.get("limit").asInt64(5000), 1, 50000);
    const double minConf = req.get("min_confidence").asDouble(0.0);
    // Paging: "before_id" echoes "next_before_id" - one id (applies to every requested type) or an
    // object keyed by type, e.g. {"file": 123, "browsing": 456}.
    const Value& bid = req.get("before_id");
    auto beforeFor = [&bid](const char* type) -> int64_t { return bid.isObject() ? bid.get(type).asInt64(0) : bid.asInt64(0); };
    const int64_t now = NowUtc();
    const int64_t start = now - secs, end = now + 60;

    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("window_start", start).KV("window_end", now);
    ActivityStore* st = m_deps.store;
    if (types & EVENT_TYPE_FILE)
    {
        auto rows = st->QueryFiles(start, end, limit + 1, beforeFor("file"), minConf);
        bool trunc = (int)rows.size() > limit;
        if (trunc) rows.resize((size_t)limit);
        w.Key("file_activities").BeginObject().KV("count", (int64_t)rows.size()).KV("truncated", trunc).Key("events").BeginArray();
        for (const FileActivity& a : rows)
        {
            w.BeginObject().KV("id", a.id).KV("timestamp", a.timestampUtc).KV("action", a.action).KV("path", a.path);
            if (!a.oldPath.empty()) w.KV("old_path", a.oldPath);
            w.KV("source", a.source).KV("confidence", a.confidence).KV("entity_key", a.entityKey).EndObject();
        }
        w.EndArray();
        if (trunc) w.KV("next_before_id", rows.back().id);
        w.EndObject();
    }
    if (types & EVENT_TYPE_APP_LAUNCH)
    {
        auto rows = st->QueryLaunches(start, end, limit + 1, beforeFor("app_launch"), minConf);
        bool trunc = (int)rows.size() > limit;
        if (trunc) rows.resize((size_t)limit);
        w.Key("app_launch_activities").BeginObject().KV("count", (int64_t)rows.size()).KV("truncated", trunc).Key("events").BeginArray();
        for (const AppLaunchActivity& a : rows)
        {
            w.BeginObject().KV("id", a.id).KV("timestamp", a.timestampUtc).KV("exe_name", a.exeName).KV("exe_path", a.exePath)
             .KV("pid", (int64_t)a.pid).KV("confidence", a.confidence);
            if (!a.creatorExe.empty()) w.KV("creator_exe", a.creatorExe);
            if (!a.aumid.empty()) w.KV("aumid", a.aumid);
            w.EndObject();
        }
        w.EndArray();
        if (trunc) w.KV("next_before_id", rows.back().id);
        w.EndObject();
    }
    if (types & EVENT_TYPE_BROWSING)
    {
        auto rows = st->QueryBrowsing(start, end, limit + 1, beforeFor("browsing"), minConf);
        bool trunc = (int)rows.size() > limit;
        if (trunc) rows.resize((size_t)limit);
        w.Key("browsing_activities").BeginObject().KV("count", (int64_t)rows.size()).KV("truncated", trunc).Key("events").BeginArray();
        for (const BrowsingActivity& b : rows)
        {
            w.BeginObject().KV("id", b.id).KV("timestamp", b.timestampUtc).KV("browser", b.browser).KV("title", b.title);
            if (!b.url.empty()) w.KV("url", b.url);
            if (!b.host.empty()) w.KV("host", b.host);
            w.KV("confidence", b.confidence).KV("entity_key", b.entityKey).EndObject();
        }
        w.EndArray();
        if (trunc) w.KV("next_before_id", rows.back().id);
        w.EndObject();
    }
    if (types & EVENT_TYPE_APP_FOCUS)
    {
        auto rows = st->QueryFocus(start, end, limit + 1, beforeFor("app_focus"), minConf);
        bool trunc = (int)rows.size() > limit;
        if (trunc) rows.resize((size_t)limit);
        w.Key("app_focus_activities").BeginObject().KV("count", (int64_t)rows.size()).KV("truncated", trunc).Key("events").BeginArray();
        for (const AppFocusActivity& f : rows)
        {
            w.BeginObject().KV("id", f.id).KV("timestamp", f.timestampUtc).KV("exe_name", f.exeName).KV("exe_path", f.exePath)
             .KV("window_title", f.windowTitle).KV("duration_secs", (int64_t)f.durationSecs)
             .KV("start_ts", f.startTs).KV("end_ts", f.endTs).KV("aumid", f.aumid)
             .KV("attention_status", f.attentionStatus).KV("unknown_attention_secs", f.unknownSecs).KV("doc_resolution", f.docResolution);
            w.Key("active_secs"); if (f.activeSecs < 0) w.Null(); else w.Int(f.activeSecs);
            w.Key("passive_secs"); if (f.passiveSecs < 0) w.Null(); else w.Int(f.passiveSecs);
            w.Key("idle_secs"); if (f.idleSecs < 0) w.Null(); else w.Int(f.idleSecs);
            if (!f.docKey.empty()) w.KV("doc_key", f.docKey).KV("doc_display", f.docDisplay).KV("doc_type", f.docType);
            w.KV("confidence", f.confidence).EndObject();
        }
        w.EndArray();
        if (trunc) w.KV("next_before_id", rows.back().id);
        w.EndObject();
    }
    w.EndObject();
    return out;
}

std::string ApiServer::QueryInferences(const Value& req)
{
    const auto& identityVersion = req.get("identity_version");
    if (!identityVersion.isNull() && (!identityVersion.isInteger() ||
        (identityVersion.asInt64() != 1 && identityVersion.asInt64() != 2)))
        return Error("invalid_request", "'identity_version' must be 1 (legacy keys) or 2 (case-safe keys)");
    if (!m_deps.entities) return Error("unavailable", "entity store is not running");
    const Value& paths = req.has("paths") ? req.get("paths") : req.get("keys");
    if (!paths.isArray()) return Error("invalid_request", "'paths' must be an array of strings");
    if (paths.size() > 5000) return Error("too_many_keys", "at most 5000 keys per request");
    std::vector<std::string> fields = req.get("fields").asStringArray();
    auto has = [&](const char* f) { return fields.empty() || std::find(fields.begin(), fields.end(), f) != fields.end(); };

    std::vector<std::string> raw, keys;
    for (const Value& v : paths.items())
    {
        if (!v.isString()) continue;
        raw.push_back(v.asString());
        keys.push_back(identityVersion.asInt64(2) == 1
            ? EntityKey::Key(WarpJson::Utf8ToWide(v.asString())) : NormalizeRaw(v.asString()));
    }
    const int64_t now = NowUtc();
    std::vector<EntityScore> scores = m_deps.entities->Score(keys, "blend", now, false, identityVersion.asInt64(2) == 1);

    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("now", now).Key("results").BeginObject();
    for (size_t i = 0; i < raw.size(); ++i)
    {
        w.Key(raw[i]).BeginObject();
        const EntityScore& s = scores[i];
        if (s.found)
        {
            const EntityRecord& r = s.rec;
            if (has("entity_type"))      w.KV("entity_type", r.entityType);
            if (has("last_event_ts"))    w.KV("last_event_ts", r.lastEventTs);
            if (has("last_open_ts"))     w.KV("last_open_ts", r.lastOpenTs);
            if (has("last_edit_ts"))     w.KV("last_edit_ts", r.lastEditTs);
            if (has("open_count_7d"))    w.KV("open_count_7d", (int64_t)std::llround(r.openCount7d));
            if (has("open_count_30d"))   w.KV("open_count_30d", (int64_t)std::llround(r.openCount30d));
            if (has("open_count_total")) w.KV("open_count_total", (int64_t)std::llround(r.openCountTotal));
            if (has("recency_score"))    w.KV("recency_score", r.recencyScore);
            if (has("version"))          w.KV("version", r.changeSeq);
            if (has("updated_at"))       w.KV("updated_at", r.updatedAt);
            if (has("entity_key"))       w.KV("entity_key", r.entityKey);
            if (has("score"))            w.KV("score", s.score);
            if (has("last_active_ts"))   w.KV("last_active_ts", r.lastActiveTs);
            if (has("active_secs_7d"))   w.KV("active_secs_7d", (int64_t)std::llround(r.activeSecs7d));
        }
        w.EndObject();
    }
    w.EndObject().EndObject();
    return out;
}

std::string ApiServer::GetInferenceDeltas(const Value& req)
{
    if (!m_deps.entities) return Error("unavailable", "entity store is not running");
    int64_t since = (std::max)((int64_t)0, req.get("since_version").asInt64(0));
    int limit = (int)ClampI(req.get("limit").asInt64(5000), 1, 5000);
    EntityChanges ch = m_deps.entities->GetChanges(since, limit, true);

    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("now", NowUtc()).Key("deltas").BeginArray();
    for (const EntityRecord& r : ch.upserts)
    {
        w.BeginObject().KV("entity_key", r.entityKey).KV("entity_type", r.entityType)
         .KV("last_event_ts", r.lastEventTs).KV("last_open_ts", r.lastOpenTs).KV("last_edit_ts", r.lastEditTs)
         .KV("open_count_7d", (int64_t)std::llround(r.openCount7d)).KV("open_count_30d", (int64_t)std::llround(r.openCount30d))
         .KV("open_count_total", (int64_t)std::llround(r.openCountTotal)).KV("recency_score", r.recencyScore)
         .KV("version", r.changeSeq).KV("updated_at", r.updatedAt).EndObject();
    }
    w.EndArray().Key("deleted").BeginArray();
    for (const EntityTombstone& t : ch.deletes)
        w.BeginObject().KV("entity_key", t.entityKey).KV("entity_type", t.entityType).KV("version", t.changeSeq).KV("reason", t.reason).EndObject();
    w.EndArray().KV("next_version", ch.nextSeq).KV("current_version", ch.currentSeq).KV("more", ch.more).KV("reset", ch.reset).EndObject();
    return out;
}

std::string ApiServer::ScoreEntities(const Value& req, const ApiClient& c)
{
    const auto& identityVersion = req.get("identity_version");
    if (!identityVersion.isNull() && (!identityVersion.isInteger() ||
        (identityVersion.asInt64() != 1 && identityVersion.asInt64() != 2)))
        return Error("invalid_request", "'identity_version' must be 1 (legacy keys) or 2 (case-safe keys)");
    if (!m_deps.entities) return Error("unavailable", "entity store is not running");
    const Value& keysV = req.has("keys") ? req.get("keys") : req.get("paths");
    if (!keysV.isArray()) return Error("invalid_request", "'keys' must be an array of strings");
    if (keysV.size() > 5000) return Error("too_many_keys", "at most 5000 keys per request");
    std::string horizon = req.get("horizon").asString(std::string("blend"));
    if (horizon != "short" && horizon != "long" && horizon != "blend")
        return Error("invalid_request", "'horizon' must be short, long or blend");
    bool withReasons = req.get("reasons").asBool(true);
    bool withContext = req.get("context").asString(std::string()) == "current" && m_deps.history;

    std::vector<std::string> raw, keys;
    for (const Value& v : keysV.items())
    {
        raw.push_back(v.isString() ? v.asString() : std::string());
        keys.push_back(v.isString() ? (identityVersion.asInt64(2) == 1
            ? EntityKey::Key(WarpJson::Utf8ToWide(v.asString())) : NormalizeRaw(v.asString())) : std::string());
    }
    const int64_t now = NowUtc();
    std::vector<EntityScore> scores = m_deps.entities->Score(keys, horizon, now, withReasons, identityVersion.asInt64(2) == 1);
    for (size_t i = 0; i < scores.size(); ++i) if (scores[i].found) keys[i] = scores[i].rec.entityKey;
    std::vector<double> boost;
    if (withContext) boost = m_deps.history->ContextBoost(keys, now);

    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("ok", true).KV("now", now).KV("horizon", horizon);
    if (withContext) w.KV("current_thread", m_deps.history->CurrentThreadId());
    w.Key("results").BeginArray();
    for (size_t i = 0; i < raw.size(); ++i)
    {
        const EntityScore& s = scores[i];
        w.BeginObject().KV("key", raw[i]).KV("entity_key", keys[i]).KV("found", s.found);
        if (s.found)
        {
            WriteEntity(w, s.rec, c.activityScope);
            WriteScore(w, s);
        }
        if (withContext) w.KV("context_boost", boost[i]);
        w.EndObject();
    }
    w.EndArray().EndObject();
    return out;
}

std::string ApiServer::TopEntities(const Value& req, const ApiClient&)
{
    if (!m_deps.entities) return Error("unavailable", "entity store is not running");
    std::string type = req.get("type").asString(std::string());
    if (!type.empty() && type != "file" && type != "app" && type != "url")
        return Error("invalid_request", "'type' must be file, app, url or omitted");
    int limit = (int)ClampI(req.get("limit").asInt64(50), 1, 1000);
    std::string horizon = req.get("horizon").asString(std::string("blend"));
    if (horizon != "short" && horizon != "long" && horizon != "blend")
        return Error("invalid_request", "'horizon' must be short, long or blend");
    const int64_t now = NowUtc();
    std::vector<EntityScore> top = m_deps.entities->Top(type, limit, horizon, now, true);

    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("ok", true).KV("now", now).KV("horizon", horizon).Key("results").BeginArray();
    for (const EntityScore& s : top)
    {
        w.BeginObject();
        WriteEntity(w, s.rec, true);
        WriteScore(w, s);
        w.EndObject();
    }
    w.EndArray().EndObject();
    return out;
}

std::string ApiServer::SearchActivity(const Value& req)
{
    if (!m_deps.entities) return Error("unavailable", "entity store is not running");
    std::string q = req.get("query").asString(std::string());
    if (q.empty()) return Error("invalid_request", "'query' is required");
    if (q.size() > 512) return Error("invalid_request", "'query' is limited to 512 bytes");
    std::string type = req.get("type").asString(std::string());
    if (!type.empty() && type != "file" && type != "app" && type != "url")
        return Error("invalid_request", "'type' must be file, app, url or omitted");
    int limit = (int)ClampI(req.get("limit").asInt64(25), 1, 200);
    const int64_t now = NowUtc();
    std::vector<EntityScore> hits = m_deps.entities->Search(WarpJson::Utf8ToWide(q), type, limit, now);

    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("ok", true).KV("now", now).Key("results").BeginArray();
    for (const EntityScore& s : hits)
    {
        w.BeginObject();
        WriteEntity(w, s.rec, true);
        w.KV("relevance", s.relevance);
        WriteScore(w, s);
        w.EndObject();
    }
    w.EndArray().EndObject();
    return out;
}

std::string ApiServer::GetChanges(const Value& req, const ApiClient& c)
{
    if (!m_deps.entities) return Error("unavailable", "entity store is not running");
    const Value& sv = req.has("since") ? req.get("since") : req.get("since_seq");
    int64_t since = (std::max)((int64_t)0, sv.asInt64(0));
    int limit = (int)ClampI(req.get("limit").asInt64(1000), 1, 5000);
    bool includeDeletes = req.get("include_deletes").asBool(true);
    EntityChanges ch = m_deps.entities->GetChanges(since, limit, includeDeletes);

    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("ok", true).KV("now", NowUtc())
     .KV("reset", ch.reset).KV("next", ch.nextSeq).KV("current", ch.currentSeq).KV("more", ch.more).Key("upserts").BeginArray();
    for (const EntityRecord& r : ch.upserts)
    {
        w.BeginObject();
        WriteEntity(w, r, c.activityScope);
        w.EndObject();
    }
    w.EndArray().Key("deletes").BeginArray();
    for (const EntityTombstone& t : ch.deletes)
        w.BeginObject().KV("entity_key", t.entityKey).KV("entity_type", t.entityType).KV("change_seq", t.changeSeq)
         .KV("reason", t.reason).KV("ts", t.ts).EndObject();
    w.EndArray().EndObject();
    return out;
}

std::string ApiServer::WaitForChanges(const Value& req)
{
    const Value& sv = req.has("since") ? req.get("since") : req.get("since_seq");
    int64_t since = (std::max)((int64_t)0, sv.asInt64(0));
    DWORD timeout = (DWORD)ClampI(req.get("timeout_ms").asInt64(20000), 0, 25000);
    bool changed = false;
    bool throttled = false;
    if (++m_longPolls > kMaxLongPolls) throttled = true;
    else
    {
        // Wait in slices so Stop() never has to wait for a parked long-poll.
        const ULONGLONG deadline = GetTickCount64() + timeout;
        for (;;)
        {
            const ULONGLONG tick = GetTickCount64();
            const DWORD slice = tick >= deadline ? 0 : (DWORD)(std::min<ULONGLONG>)(deadline - tick, 250);
            changed = m_deps.store->WaitForChange(since, slice);
            if (changed || slice == 0 || !m_deps.store->IsOpen() || WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0)
                break;
        }
    }
    --m_longPolls;

    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("ok", true).KV("changed", changed || m_deps.store->CurrentChangeSeq() > since)
     .KV("current", m_deps.store->CurrentChangeSeq()).KV("reset_seq", m_deps.store->ResetSeq());
    if (throttled) w.KV("retry_after_ms", (int64_t)1000);
    w.EndObject();
    return out;
}

std::string ApiServer::QueryTimeline(const Value& req)
{
    const int64_t now = NowUtc();
    int64_t end = req.has("end") ? req.get("end").asInt64(now) : now;
    int64_t start = req.has("start") ? req.get("start").asInt64(0) : end - 3600;
    if (start <= 0 || end <= 0 || end < start) return Error("invalid_request", "'start'/'end' must be Unix seconds with start <= end");
    bool sessions = false, threads = false;
    uint32_t types = ParseTypes(req.get("types"), sessions, threads);
    if (!req.has("types") || (types == 0 && !sessions && !threads)) { types = EVENT_TYPE_ALL; sessions = threads = true; }
    const int limit = (int)ClampI(req.get("limit").asInt64(1000), 1, 20000);
    const double minConf = req.get("min_confidence").asDouble(0.0);
    const Value& cursor = req.get("cursor");
    ActivityStore* st = m_deps.store;

    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("ok", true).KV("start", start).KV("end", end);
    std::vector<std::pair<std::string, int64_t>> next;
    if (types & EVENT_TYPE_FILE)
    {
        auto rows = st->QueryFiles(start, end, limit + 1, cursor.get("file").asInt64(0), minConf);
        bool more = (int)rows.size() > limit;
        if (more) { rows.resize((size_t)limit); next.emplace_back("file", rows.back().id); }
        w.Key("files").BeginArray();
        for (const FileActivity& a : rows)
        {
            w.BeginObject().KV("id", a.id).KV("timestamp", a.timestampUtc).KV("action", a.action).KV("path", a.path);
            if (!a.oldPath.empty()) w.KV("old_path", a.oldPath);
            w.KV("entity_key", a.entityKey).KV("source", a.source).KV("confidence", a.confidence).EndObject();
        }
        w.EndArray();
    }
    if (types & EVENT_TYPE_APP_LAUNCH)
    {
        auto rows = st->QueryLaunches(start, end, limit + 1, cursor.get("app_launch").asInt64(0), minConf);
        bool more = (int)rows.size() > limit;
        if (more) { rows.resize((size_t)limit); next.emplace_back("app_launch", rows.back().id); }
        w.Key("app_launches").BeginArray();
        for (const AppLaunchActivity& a : rows)
            w.BeginObject().KV("id", a.id).KV("timestamp", a.timestampUtc).KV("exe_name", a.exeName).KV("exe_path", a.exePath)
             .KV("aumid", a.aumid).KV("confidence", a.confidence).EndObject();
        w.EndArray();
    }
    if (types & EVENT_TYPE_APP_FOCUS)
    {
        auto rows = st->QueryFocus(start, end, limit + 1, cursor.get("app_focus").asInt64(0), minConf);
        bool more = (int)rows.size() > limit;
        if (more) { rows.resize((size_t)limit); next.emplace_back("app_focus", rows.back().id); }
        w.Key("focus").BeginArray();
        for (const AppFocusActivity& f : rows)
        {
            w.BeginObject().KV("id", f.id).KV("start_ts", f.startTs).KV("end_ts", f.endTs)
             .KV("exe_name", f.exeName).KV("exe_path", f.exePath).KV("aumid", f.aumid).KV("window_title", f.windowTitle)
             .KV("attention_status", f.attentionStatus).KV("unknown_attention_secs", f.unknownSecs).KV("doc_resolution", f.docResolution);
            w.Key("active_secs"); if (f.activeSecs < 0) w.Null(); else w.Int(f.activeSecs);
            w.Key("passive_secs"); if (f.passiveSecs < 0) w.Null(); else w.Int(f.passiveSecs);
            w.Key("idle_secs"); if (f.idleSecs < 0) w.Null(); else w.Int(f.idleSecs);
            if (!f.docKey.empty()) w.KV("doc_key", f.docKey).KV("doc_display", f.docDisplay).KV("doc_type", f.docType);
            w.EndObject();
        }
        w.EndArray();
    }
    if (types & EVENT_TYPE_BROWSING)
    {
        auto rows = st->QueryBrowsing(start, end, limit + 1, cursor.get("browsing").asInt64(0), minConf);
        bool more = (int)rows.size() > limit;
        if (more) { rows.resize((size_t)limit); next.emplace_back("browsing", rows.back().id); }
        w.Key("browsing").BeginArray();
        for (const BrowsingActivity& b : rows)
            w.BeginObject().KV("id", b.id).KV("timestamp", b.timestampUtc).KV("browser", b.browser).KV("title", b.title)
             .KV("url", b.url).KV("entity_key", b.entityKey).EndObject();
        w.EndArray();
    }
    if (sessions && m_deps.history)
    {
        w.Key("sessions").BeginArray();
        for (const WorkSessionInfo& s : m_deps.history->GetSessions(start, end, 500))
            w.BeginObject().KV("id", s.id).KV("start_ts", s.startTs).KV("end_ts", s.endTs)
             .KV("active_secs", (int64_t)s.activeSecs).KV("passive_secs", (int64_t)s.passiveSecs)
             .KV("accounting_version", s.accountingVersion).EndObject();
        w.EndArray();
    }
    if (threads && m_deps.history)
    {
        w.Key("threads").BeginArray();
        for (const WorkThreadInfo& t : m_deps.history->GetThreads(start, end, 50, false))
        {
            w.BeginObject().KV("id", t.id).KV("label", t.label).KV("active_secs_in_range", t.activeSecsInRange).Key("spans").BeginArray();
            for (const auto& sp : t.spans) w.BeginArray().Int(sp.first).Int(sp.second).EndArray();
            w.EndArray().EndObject();
        }
        w.EndArray();
    }
    w.Key("next_cursor");
    if (next.empty()) w.Null();
    else
    {
        w.BeginObject();
        for (const auto& kv : next) w.KV(kv.first, kv.second);
        w.EndObject();
    }
    w.EndObject();
    return out;
}

std::string ApiServer::GetWorkThreads(const Value& req)
{
    if (!m_deps.history) return Error("unavailable", "history engine is not running");
    const int64_t now = NowUtc();
    int64_t end = req.has("end") ? req.get("end").asInt64(now) : now;
    int64_t start = req.has("start") ? req.get("start").asInt64(0) : end - 7LL * 86400;
    if (end < start) return Error("invalid_request", "'start' must be <= 'end'");
    int limit = (int)ClampI(req.get("limit").asInt64(20), 1, 200);
    bool members = req.get("members").asBool(true);
    std::vector<WorkThreadInfo> threads = m_deps.history->GetThreads(start, end, limit, members);

    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("ok", true).KV("start", start).KV("end", end)
     .KV("current_thread", m_deps.history->CurrentThreadId()).Key("threads").BeginArray();
    for (const WorkThreadInfo& t : threads)
    {
        w.BeginObject().KV("id", t.id).KV("label", t.label).KV("first_ts", t.firstTs).KV("last_ts", t.lastTs)
         .KV("active_secs", (int64_t)std::llround(t.activeSecs)).KV("active_secs_in_range", (int64_t)std::llround(t.activeSecsInRange))
         .KV("project", t.projectKey).KV("current", t.current).KV("change_seq", t.changeSeq).Key("spans").BeginArray();
        for (const auto& sp : t.spans) w.BeginArray().Int(sp.first).Int(sp.second).EndArray();
        w.EndArray();
        if (members)
        {
            w.Key("members").BeginArray();
            for (const WorkThreadMember& m : t.members)
            {
                w.BeginObject().KV("entity_key", m.entityKey).KV("entity_type", m.entityType).KV("display_name", m.display);
                if (!m.title.empty()) w.KV("title", m.title);
                w.KV("weight", m.weight).EndObject();
            }
            w.EndArray();
        }
        w.EndObject();
    }
    w.EndArray().EndObject();
    return out;
}

std::string ApiServer::GetRelated(const Value& req)
{
    if (!m_deps.history) return Error("unavailable", "history engine is not running");
    std::string key = req.get("entity_key").asString(std::string());
    if (key.empty()) key = NormalizeRaw(req.get("key").asString(std::string()));
    if (key.empty()) return Error("invalid_request", "'key' or 'entity_key' is required");
    int limit = (int)ClampI(req.get("limit").asInt64(20), 1, 100);
    std::vector<RelatedEntity> rel = m_deps.history->Related(key, limit);

    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("ok", true).KV("entity_key", key).Key("results").BeginArray();
    for (const RelatedEntity& r : rel)
        w.BeginObject().KV("entity_key", r.entityKey).KV("entity_type", r.entityType).KV("display_name", r.display)
         .KV("weight", r.weight).KV("last_ts", r.lastTs).EndObject();
    w.EndArray().EndObject();
    return out;
}

std::string ApiServer::GetCurrentContext(const Value& req)
{
    if (!m_deps.context) return Error("unavailable", "context inference is not running");
    bool includeVector = req.get("include_vector").asBool(false);
    std::string ctx = m_deps.context->GetCurrentContextJson(includeVector);
    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("ok", true).Key("context").Raw(ctx);
    if (m_deps.history)
        w.Key("thread").BeginObject().KV("id", m_deps.history->CurrentThreadId()).KV("label", m_deps.history->CurrentThreadLabel()).EndObject();
    w.EndObject();
    return out;
}

std::string ApiServer::GetStatus(const ApiClient& c)
{
    const int64_t now = NowUtc();
    WarpSettings s = Settings::Instance().Get();
    // Settings (exclusion lists name sensitive sites/folders) and log lines (may contain paths) are
    // only for WARP's own CLI / tray; other clients get the health summary.
    const bool full = c.inProcess || c.controlScope;
    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("ok", true).KV("version", m_deps.productVersion)
     .KV("pid", (int64_t)GetCurrentProcessId()).KV("session_id", (int64_t)m_sessionId)
     .KV("started_utc", m_startedUtc).KV("uptime_secs", now - m_startedUtc)
     .KV("paused", Settings::Instance().IsPaused(now)).KV("paused_until", s.pausedUntilUtc)
     .KV("consent_acknowledged", s.consentAcknowledged);

    if (m_deps.store)
    {
        ActivityStore::Stats st = m_deps.store->GetStats();
        w.Key("store").BeginObject().KV("open", m_deps.store->IsOpen());
        if (full) w.KV("path", m_deps.store->Path());
        w.KV("schema_version", (int64_t)st.schemaVersion).KV("integrity", st.integrity)
         .KV("db_bytes", st.dbBytes).KV("wal_bytes", st.walBytes).KV("queue_depth", (int64_t)st.queueDepth)
         .KV("enqueued", (uint64_t)st.enqueued).KV("dropped", (uint64_t)st.dropped).KV("written", (uint64_t)st.written)
         .KV("batches", (uint64_t)st.batches).KV("write_errors", (uint64_t)st.writeErrors).KV("last_commit_utc", st.lastCommitUtc)
         .KV("change_seq", st.changeSeq).KV("reset_seq", st.resetSeq)
         .KV("raw_retention_days", (int64_t)st.rawRetentionDays).KV("entity_retention_days", (int64_t)st.entityRetentionDays).EndObject();
    }

    Stats a = GetStats();
    w.Key("api").BeginObject().KV("requests", (uint64_t)a.requests).KV("errors", (uint64_t)a.errors)
     .KV("rejected", (uint64_t)a.rejected).KV("busy", (uint64_t)a.busy).KV("active_connections", (int64_t)a.activeConnections)
     .KV("session_pipe", m_session.name).KV("session_pipe_owned", a.sessionPipeOwned)
     .KV("legacy_pipe", m_legacy.name).KV("legacy_pipe_owned", a.legacyPipeOwned).EndObject();

    if (m_deps.statusProvider) m_deps.statusProvider(w, full);

    w.Key("log").BeginObject()
     .KV("warnings", (uint64_t)Log::CountAtLevel(Log::Level::Warn)).KV("errors", (uint64_t)Log::CountAtLevel(Log::Level::Error));
    if (full)
    {
        w.KV("path", Log::LogFilePath()).Key("recent_problems").BeginArray();
        for (const std::string& p : Log::RecentProblems(10)) w.String(p);
        w.EndArray();
    }
    w.EndObject();

    if (full)
    {
        w.Key("settings").BeginObject();
        for (const auto& kv : Settings::Instance().Describe()) w.KV(WarpJson::WideToUtf8(kv.first), kv.second);
        w.EndObject();
    }
    w.EndObject();
    return out;
}

std::string ApiServer::Purge(const Value& req)
{
    ActivityStore::PurgeSpec spec;
    spec.startUtc = req.get("start").asInt64(0);
    spec.endUtc = req.get("end").asInt64(0);
    spec.entityKey = req.get("entity_key").asString(std::string());
    if (spec.entityKey.empty() && req.has("key")) spec.entityKey = NormalizeRaw(req.get("key").asString(std::string()));
    spec.domain = WarpJson::Utf8ToWide(req.get("domain").asString(std::string()));
    spec.pathPrefix = WarpJson::Utf8ToWide(req.get("path_prefix").asString(std::string()));
    const int criteria = ((spec.startUtc > 0 || spec.endUtc > 0) ? 1 : 0) + (spec.entityKey.empty() ? 0 : 1) +
                         (spec.domain.empty() ? 0 : 1) + (spec.pathPrefix.empty() ? 0 : 1);
    if (criteria == 0)
        return Error("invalid_request", "specify start/end, key, entity_key, domain or path_prefix");
    // Criteria are not intersected: one kind per request avoids deleting more than was asked for.
    if (criteria > 1)
        return Error("invalid_request", "specify exactly one of: start/end, key/entity_key, domain, path_prefix");
    if (spec.endUtc > 0 && spec.startUtc > spec.endUtc)
        return Error("invalid_request", "'start' must be <= 'end'");
    int64_t rows = 0;
    bool ok = m_deps.store->Purge(spec, rows);
    if (ok && m_deps.context) m_deps.context->ClearHistory();     // in-memory snapshots may mention purged items
    WARP_LOG_INFO("api", "purge requested: ok=%d rows=%lld", ok ? 1 : 0, (long long)rows);
    if (!ok) return Error("internal_error", "the purge did not complete (store busy or closed); nothing was reported as deleted");
    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("ok", true).KV("rows_deleted", rows).EndObject();
    return out;
}

std::string ApiServer::Pause(const Value& req, bool resume)
{
    if (!m_deps.setPaused) return Error("unavailable", "pause is not supported");
    int64_t until = 0;
    if (!resume)
    {
        if (req.get("indefinite").asBool(false)) until = INT64_MAX;
        else if (req.has("until")) until = req.get("until").asInt64(0);
        else until = NowUtc() + ClampI(req.get("minutes").asInt64(60), 1, 7LL * 24 * 60) * 60;
        if (until <= NowUtc()) return Error("invalid_request", "'until' must be in the future");
    }
    bool ok = m_deps.setPaused(until);
    std::string out;
    Writer w(out);
    w.BeginObject().KV("api_version", (int64_t)kApiVersion).KV("ok", ok).KV("paused", !resume && ok).KV("paused_until", ok ? until : (int64_t)0);
    if (!ok) w.Key("error").BeginObject().KV("code", "locked").KV("message", "pause state is controlled by policy").EndObject();
    w.EndObject();
    return out;
}
