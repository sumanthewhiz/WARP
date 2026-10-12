// Consumer API tests (gap analysis API-3/4/6/7, BUG-7, PRIV-2): ApiServer::Handle in process,
// against a real store - the same code path the pipe workers use, minus the transport.
#include "WarpTests.h"

#include "ActivityStore.h"
#include "ApiServer.h"
#include "EntityKey.h"
#include "EntityStore.h"
#include "HistoryEngine.h"
#include "Json.h"

#include <thread>

namespace
{
    struct ApiFixture
    {
        wt::TempDir   dir;
        ActivityStore store;
        EntityStore   ents{ &store };
        HistoryEngine hist{ &store };
        ApiServer     api;
        bool          open = false;
        int64_t       pausedUntil = -1;

        ApiFixture()
        {
            store.AddSink(&ents);
            store.AddSink(&hist);
            open = store.Open(dir.File(L"api.db"));
            ApiServer::Deps d;
            d.store = &store;
            d.entities = &ents;
            d.history = &hist;
            d.context = nullptr;                 // context ops answer "unavailable"
            d.productVersion = "test";
            d.setPaused = [this](int64_t until) { pausedUntil = until; return true; };
            api.Configure(d);
        }
        ~ApiFixture() { store.Close(); }

        bool Barrier() { return store.RunSync([](WriterContext&) {}, 30000); }

        void File(const wchar_t* action, const std::wstring& path, double confidence = 1.0)
        {
            FileEventRecord r;
            r.action = action;
            r.path = path;
            r.ctx.confidence = confidence;
            store.RecordFile(std::move(r));
        }

        WarpJson::Value Call(const std::string& request, const ApiClient& client = ApiClient::InProcess())
        {
            const std::string resp = api.Handle(request, client);
            WarpJson::ParseResult pr = WarpJson::Parse(resp);
            if (!pr.ok)
            {
                wt::Fail(__FILE__, __LINE__, "response is not valid JSON: " + resp);
                return WarpJson::Value();
            }
            if (pr.value["api_version"].asInt64() != ApiServer::kApiVersion)
                wt::Fail(__FILE__, __LINE__, "response without api_version: " + resp);
            return pr.value;
        }
    };

    std::string ErrorCode(const WarpJson::Value& v) { return v["error"]["code"].asString(std::string()); }
}

TEST(Api, RejectsMalformedRequestsWithStructuredErrors)
{
    ApiFixture f;
    REQUIRE(f.open);
    WarpJson::Value bad = f.Call("not json");
    CHECK(!bad["ok"].asBool(true));
    CHECK_EQ(ErrorCode(bad), std::string("invalid_json"));
    CHECK(!bad["error"]["message"].asString().empty());
    CHECK_EQ(ErrorCode(f.Call("[1,2]")), std::string("invalid_request"));
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"NoSuchOp\"}")), std::string("unknown_op"));
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"QueryInferences\",\"paths\":\"C:\\\\x\"}")), std::string("invalid_request"));
    CHECK_EQ(ErrorCode(f.Call("{\"window\":\"5y\"}")), std::string("invalid_request"));
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"GetCurrentContext\"}")), std::string("unavailable"));
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"SearchActivity\"}")), std::string("invalid_request"));
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"GetRelated\"}")), std::string("invalid_request"));
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"TopEntities\",\"type\":\"folder\"}")), std::string("invalid_request"));
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"Purge\"}")), std::string("invalid_request"));
    WarpJson::Value ping = f.Call("{\"op\":\"Ping\"}");
    CHECK(ping["ok"].asBool());
    CHECK(ping["pong"].asBool());
    CHECK_EQ(ping["version"].asString(), std::string("test"));
}

TEST(Api, QueryInferencesEchoesCallerKeysVerbatim)
{
    ApiFixture f;
    REQUIRE(f.open);
    f.File(L"MODIFY", L"C:\\api\\Na\x00EFve Plan (v2).docx");
    REQUIRE(f.Barrier());
    // JSON escapes, Unicode and a different spelling of the same path (API-4: the old hand-rolled
    // parser broke on escaped backslashes and \u sequences).
    WarpJson::Value v = f.Call(R"({"op":"QueryInferences","paths":["C:\\api\\Na\u00efve Plan (v2).docx","c:\\API\\NA\u00cfVE PLAN (V2).DOCX","C:\\api\\missing.txt"]})");
    const WarpJson::Value& res = v["results"];
    REQUIRE(res.isObject());
    CHECK_EQ(res["C:\\api\\Na\xC3\xAFve Plan (v2).docx"]["entity_type"].asString(), std::string("file"));
    CHECK_EQ(res["c:\\API\\NA\xC3\x8FVE PLAN (V2).DOCX"]["entity_type"].asString(), std::string("file"));
    CHECK(res.has("C:\\api\\missing.txt"));
    CHECK_EQ(res["C:\\api\\missing.txt"].size(), (size_t)0);
}

TEST(Api, EnforcesClientScopes)
{
    ApiFixture f;
    REQUIRE(f.open);
    ApiClient external;
    external.pid = 4321;
    external.activityScope = false;
    external.controlScope = false;
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"TopEntities\"}", external)), std::string("forbidden"));
    CHECK_EQ(ErrorCode(f.Call("{\"window\":\"1h\"}", external)), std::string("forbidden"));
    CHECK(f.Call("{\"op\":\"ScoreEntities\",\"keys\":[]}", external)["ok"].asBool());

    ApiClient reader;
    reader.pid = 4321;
    reader.activityScope = true;
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"Pause\",\"minutes\":5}", reader)), std::string("forbidden"));
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"Purge\",\"start\":1}", reader)), std::string("forbidden"));
    CHECK_EQ(f.pausedUntil, (int64_t)-1);

    WarpJson::Value p = f.Call("{\"op\":\"Pause\",\"minutes\":5}");     // WARP's own tray / CLI
    CHECK(p["ok"].asBool());
    CHECK(f.pausedUntil > wt::NowUtc());
    CHECK(f.Call("{\"op\":\"Resume\"}")["ok"].asBool());
    CHECK_EQ(f.pausedUntil, (int64_t)0);
}

TEST(Api, ScoresRanksAndSearchesEntities)
{
    ApiFixture f;
    REQUIRE(f.open);
    f.File(L"MODIFY", L"C:\\api\\Budget Review.docx");
    f.File(L"OPEN", L"C:\\api\\b.docx", 0.5);
    REQUIRE(f.Barrier());

    WarpJson::Value top = f.Call("{\"op\":\"TopEntities\",\"type\":\"file\",\"limit\":5}");
    REQUIRE(top["ok"].asBool());
    REQUIRE(top["results"].size() == 2);
    CHECK_EQ(top["results"][0]["entity_key"].asString(), EntityKey::FileKey(L"C:\\api\\Budget Review.docx"));
    CHECK(top["results"][0]["reasons"].isArray());
    CHECK(top["results"][0]["score"].isNumber());

    WarpJson::Value sc = f.Call(R"({"op":"ScoreEntities","keys":["C:\\api\\Budget Review.docx","https://example.com/never"],"horizon":"short"})");
    REQUIRE(sc["results"].size() == 2);
    CHECK(sc["results"][0]["found"].asBool());
    CHECK_EQ(sc["results"][0]["key"].asString(), std::string("C:\\api\\Budget Review.docx"));
    CHECK(!sc["results"][1]["found"].asBool(true));

    WarpJson::Value sr = f.Call("{\"op\":\"SearchActivity\",\"query\":\"budget\"}");
    REQUIRE(sr["results"].size() >= 1);
    CHECK(sr["results"][0]["relevance"].isNumber());
}

TEST(Api, ChangeFeedAndPurgeRoundTrip)
{
    ApiFixture f;
    REQUIRE(f.open);
    f.File(L"MODIFY", L"C:\\api\\feed\\x.txt");
    REQUIRE(f.Barrier());
    WarpJson::Value ch = f.Call("{\"op\":\"GetChanges\",\"since\":0}");
    REQUIRE(ch["ok"].asBool());
    CHECK_EQ(ch["upserts"].size(), (size_t)1);
    const int64_t next = ch["next"].asInt64();
    CHECK(next > 0);

    WarpJson::Value purge = f.Call(R"({"op":"Purge","path_prefix":"C:\\api\\feed"})");
    CHECK(purge["ok"].asBool());
    CHECK(purge["rows_deleted"].asInt64() > 0);
    WarpJson::Value ch2 = f.Call("{\"op\":\"GetChanges\",\"since\":" + std::to_string(next) + "}");
    REQUIRE(ch2["deletes"].size() == 1);
    CHECK_EQ(ch2["deletes"][0]["reason"].asString(), std::string("purge"));

    // The v1 delta feed keeps working (version == change_seq).
    WarpJson::Value d = f.Call("{\"op\":\"GetInferenceDeltas\",\"since_version\":0}");
    CHECK(d["deltas"].isArray());
    CHECK(d["deleted"].size() >= 1);
    CHECK(d["next_version"].asInt64() >= next);
}

TEST(Api, TimelinePagesWithCursors)
{
    ApiFixture f;
    REQUIRE(f.open);
    for (int i = 0; i < 3; ++i) f.File(L"MODIFY", L"C:\\api\\tl\\f" + std::to_wstring(i) + L".txt");
    BrowseEventRecord b;
    b.browser = L"msedge.exe";
    b.title = L"Docs";
    b.url = L"https://learn.example/docs";
    b.host = L"learn.example";
    f.store.RecordBrowse(std::move(b));
    REQUIRE(f.Barrier());

    WarpJson::Value raw = f.Call("{\"window\":\"15m\",\"types\":[\"file\",\"browsing\"]}");
    CHECK_EQ(raw["file_activities"]["count"].asInt64(), (int64_t)3);
    CHECK_EQ(raw["browsing_activities"]["count"].asInt64(), (int64_t)1);
    CHECK(!raw.has("app_launch_activities"));

    const int64_t now = wt::NowUtc();
    const std::string range = "\"start\":" + std::to_string(now - 600) + ",\"end\":" + std::to_string(now + 60);
    WarpJson::Value p1 = f.Call("{\"op\":\"QueryTimeline\"," + range + ",\"types\":[\"file\"],\"limit\":2}");
    REQUIRE(p1["ok"].asBool());
    CHECK_EQ(p1["files"].size(), (size_t)2);
    REQUIRE(p1["next_cursor"].isObject());
    const int64_t cursor = p1["next_cursor"]["file"].asInt64();
    WarpJson::Value p2 = f.Call("{\"op\":\"QueryTimeline\"," + range + ",\"types\":[\"file\"],\"limit\":2,\"cursor\":{\"file\":" +
                                std::to_string(cursor) + "}}");
    CHECK_EQ(p2["files"].size(), (size_t)1);
    CHECK(p2["next_cursor"].isNull());
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"QueryTimeline\",\"start\":10,\"end\":5}")), std::string("invalid_request"));
}

TEST(Api, StatusThreadsAndLongPoll)
{
    ApiFixture f;
    REQUIRE(f.open);
    WarpJson::Value s = f.Call("{\"op\":\"GetStatus\"}");
    CHECK(s["ok"].asBool());
    CHECK(s["store"]["open"].asBool());
    CHECK_EQ(s["store"]["schema_version"].asInt64(), (int64_t)2);
    CHECK(s["settings"].isObject());
    CHECK(f.Call("{\"op\":\"GetWorkThreads\"}")["threads"].isArray());

    const int64_t seq = f.store.CurrentChangeSeq();
    std::thread writer([&f]
    {
        Sleep(200);
        f.File(L"MODIFY", L"C:\\api\\poll.txt");
    });
    const ULONGLONG t0 = GetTickCount64();
    WarpJson::Value w = f.Call("{\"op\":\"WaitForChanges\",\"since\":" + std::to_string(seq) + ",\"timeout_ms\":10000}");
    writer.join();
    CHECK(w["changed"].asBool());
    CHECK(GetTickCount64() - t0 < 9000);              // woke up on the commit, not on the timeout
}

TEST(Api, HealthOpsAnswerWithTheStoreClosedAndRedactForThirdParties)
{
    ApiFixture f;
    REQUIRE(f.open);
    ApiClient other;
    other.pid = 4321;
    other.activityScope = true;
    WarpJson::Value s = f.Call("{\"op\":\"GetStatus\"}", other);
    CHECK(s["ok"].asBool());
    CHECK(!s.has("settings"));                        // exclusion lists name sensitive sites / folders
    CHECK(!s["store"].has("path"));
    CHECK(!s["log"].has("recent_problems"));
    CHECK(f.Call("{\"op\":\"GetStatus\"}")["settings"].isObject());   // WARP itself sees everything

    f.store.Close();
    WarpJson::Value ping = f.Call("{\"op\":\"Ping\"}", other);
    CHECK(ping["ok"].asBool());
    CHECK(!ping["store_open"].asBool(true));
    WarpJson::Value st = f.Call("{\"op\":\"GetStatus\"}", other);
    CHECK(st["ok"].asBool());
    CHECK(!st["store"]["open"].asBool(true));
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"TopEntities\"}")), std::string("unavailable"));
}

TEST(Api, ValidatesParametersAndPurgeCriteria)
{
    ApiFixture f;
    REQUIRE(f.open);
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"TopEntities\",\"horizon\":\"forever\"}")), std::string("invalid_request"));
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"ScoreEntities\",\"keys\":[],\"horizon\":\"x\"}")), std::string("invalid_request"));
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"SearchActivity\",\"query\":\"a\",\"type\":\"folder\"}")), std::string("invalid_request"));
    // One purge criterion per request: a time range AND a domain is refused, not unioned.
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"Purge\",\"start\":1,\"domain\":\"example.com\"}")), std::string("invalid_request"));
    CHECK_EQ(ErrorCode(f.Call("{\"op\":\"Purge\",\"start\":20,\"end\":10}")), std::string("invalid_request"));
    CHECK(f.Call("{\"op\":\"Purge\",\"domain\":\"example.com\"}")["ok"].asBool());
}

TEST(Api, RawQueryPagesWithBeforeId)
{
    ApiFixture f;
    REQUIRE(f.open);
    for (int i = 0; i < 5; ++i) f.File(L"MODIFY", L"C:\\api\\raw\\f" + std::to_wstring(i) + L".txt");
    REQUIRE(f.Barrier());
    WarpJson::Value p1 = f.Call("{\"window\":\"15m\",\"types\":[\"file\"],\"limit\":3}");
    REQUIRE(p1["file_activities"]["truncated"].asBool());
    CHECK_EQ(p1["file_activities"]["count"].asInt64(), (int64_t)3);
    const int64_t next = p1["file_activities"]["next_before_id"].asInt64();
    REQUIRE(next > 0);
    WarpJson::Value p2 = f.Call("{\"window\":\"15m\",\"types\":[\"file\"],\"limit\":3,\"before_id\":{\"file\":" + std::to_string(next) + "}}");
    CHECK_EQ(p2["file_activities"]["count"].asInt64(), (int64_t)2);
    CHECK(!p2["file_activities"]["truncated"].asBool(true));
    for (const WarpJson::Value& e : p2["file_activities"]["events"].items()) CHECK(e["id"].asInt64() < next);
}
