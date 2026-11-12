#include "WarpTests.h"
#include "ActivityStore.h"
#include "ApiServer.h"
#include "ContextInference.h"
#include "EntityKey.h"
#include "EntityStore.h"
#include "Evidence.h"
#include "FocusTracker.h"
#include "HistoryEngine.h"
#include "Json.h"
#include "WebCapture.h"
#include "sqlite3.h"

#include <algorithm>
#include <set>
#include <thread>
#include <stdexcept>
#include <fstream>
#include <filesystem>

namespace
{
    struct EvidenceFixture
    {
        wt::TempDir dir;
        ActivityStore store;
        EntityStore entities{&store};
        HistoryEngine history{&store};
        int64_t now = wt::NowUtc() - 2;
        EvidenceFixture()
        {
            store.AddSink(&entities);
            store.AddSink(&history);
            if (!store.Open(dir.File(L"evidence.db")))
                throw std::runtime_error("fixture open failed");
        }
        ~EvidenceFixture()
        {
            store.Close();
        }
        void Flush()
        {
            REQUIRE(store.RunSync([](WriterContext&) {}));
        }
        FileEventRecord File(const wchar_t* name = L"Plan.docx", int offset = -60)
        {
            FileEventRecord r;
            r.path = L"C:\\WarpEvidence\\" + std::wstring(name);
            r.action = L"OPEN";
            r.source = L"recent";
            r.eventMs = (now + offset) * 1000;
            r.ctx.evidence.observedMs = r.eventMs + 200;
            r.ctx.evidence.receivedMs = r.eventMs + 400;
            r.ctx.evidence.timestampKind = "recent_item_time";
            r.ctx.evidence.actorMethod = "recency_observation";
            return r;
        }
        FocusEventRecord Focus(int64_t start, int64_t end, const std::string& id, int revision = 1)
        {
            FocusEventRecord r;
            r.startMs = start;
            r.endMs = end;
            r.activeSecs = (int)((end - start) / 1000);
            r.exeName = L"WINWORD.EXE";
            r.exePath = L"C:\\WarpEvidence\\WINWORD.EXE";
            r.windowTitle = L"Plan.docx";
            r.docDisplay = L"C:\\WarpEvidence\\Plan.docx";
            r.docKey = EntityKey::FileKey(r.docDisplay);
            r.docType = "file";
            r.docResolution = "filename_candidate";
            r.ctx.evidence.episodeId = id;
            r.ctx.evidence.revision = revision;
            r.ctx.evidence.open = true;
            r.ctx.evidence.receivedMs = end + 10;
            r.ctx.evidence.observedMs = end;
            return r;
        }
        ActivityStore::EvidenceSet Read(int seconds = 1800)
        {
            Flush();
            return store.ReadEvidence((now - seconds) * 1000, now * 1000);
        }
    };

    int64_t Count(ActivityStore& store, const char* table)
    {
        auto lease = store.AcquireReader();
        REQUIRE(lease);
        sqlite3_stmt* st = nullptr;
        const auto sql = std::string("SELECT COUNT(*) FROM ") + table;
        REQUIRE(sqlite3_prepare_v2(lease.get(), sql.c_str(), -1, &st, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(st) == SQLITE_ROW);
        const auto count = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        return count;
    }
} // namespace

struct FocusTrackerTestAccess
{
    static void RunSampler(FocusTracker& tracker, HWND window)
    {
        tracker.m_cur.hwnd = window;
        tracker.m_cur.pid = GetCurrentProcessId();
        tracker.m_cur.exeName = L"notepad.exe";
        tracker.m_cur.exePath = L"C:\\WarpEvidence\\notepad.exe";
        tracker.m_cur.title = L"Synthetic checkpoint";
        tracker.m_cur.episodeId = "focus:real-sampler";
        tracker.m_cur.startMs = StoreUtil::NowMs() - 31000;
        tracker.m_cur.lastSampleTick = GetTickCount64();
        tracker.m_cur.ctx.evidence.processInstance = EventContextUtil::ProcessInstanceId(GetCurrentProcessId());
        tracker.m_haveCur = true;
        tracker.m_running = true;
        tracker.m_sampler = std::thread(&FocusTracker::SamplerThread, &tracker);
    }
    static std::vector<FocusEventRecord> Slices(int64_t start)
    {
        FocusTracker tracker;
        tracker.m_haveCur = true;
        tracker.m_cur.startMs = start;
        tracker.m_cur.episodeId = "focus:fixture";
        tracker.m_cur.exeName = L"notepad.exe";
        tracker.m_cur.exePath = L"C:\\WarpEvidence\\notepad.exe";
        tracker.m_cur.title = L"fixture.txt";
        tracker.m_cur.activeSecs = 30;
        std::vector<FocusEventRecord> out;
        tracker.CheckpointLocked(start + 30000, true, "checkpoint", out);
        tracker.m_cur.activeSecs = 10;
        tracker.EndLocked(start + 40000, out, "window_closed");
        return out;
    }
};

struct EvidenceReadTestAccess
{
    static void BetweenTypes(ActivityStore& store, std::function<void()> callback)
    {
        store.m_evidenceReadBarrier = std::move(callback);
    }
};

TEST(Evidence, ProvenanceSurvivesPersistenceAndIdempotentReplay)
{
    EvidenceFixture f;
    auto file = f.File();
    file.ctx.evidence.id = "file:journal:42";
    file.ctx.evidence.fileId = "fedcba98765432100123456789abcdef";
    file.ctx.evidence.volumeId = "volume:one";
    file.ctx.evidence.journalId = "18446744073709551615";
    file.ctx.evidence.replayed = true;
    file.ctx.evidence.actorMethod = "opener_association";
    file.originUrl = L"https://example.org/PlanA";
    REQUIRE(f.store.RecordFile(file));
    REQUIRE(f.store.RecordFile(file));
    f.Flush();
    const auto evidence = f.Read();
    REQUIRE(evidence.files.size() == 1);
    const auto& row = evidence.files[0];
    CHECK_EQ(row.evidence.id, file.ctx.evidence.id);
    CHECK_EQ(row.evidence.fileId, file.ctx.evidence.fileId);
    CHECK_EQ(row.evidence.journalId, file.ctx.evidence.journalId);
    CHECK_EQ(row.evidence.actorMethod, std::string("unknown"));
    CHECK(row.evidence.replayed);
    CHECK_EQ(row.originUrl, file.originUrl);
    CHECK_EQ(Count(f.store, "observation"), 1);
    CHECK_EQ(f.entities.Lookup({row.entityKey})[0].openCountTotal, 1.0);
}

TEST(Evidence, HalfOpenIntervalsAndAsOfExcludeLateArrivals)
{
    EvidenceFixture f;
    auto a = f.File(L"AtStart.pdf", -100), b = f.File(L"AtEnd.pdf", 0), late = f.File(L"Late.pdf", -50);
    a.ctx.evidence.receivedMs = (f.now - 90) * 1000;
    late.ctx.evidence.receivedMs = (f.now + 1) * 1000;
    REQUIRE(f.store.RecordFile(a));
    REQUIRE(f.store.RecordFile(b));
    REQUIRE(f.store.RecordFile(late));
    f.Flush();
    const auto before = f.store.ReadEvidence((f.now - 100) * 1000, f.now * 1000, f.now * 1000);
    REQUIRE(before.files.size() == 1);
    CHECK_EQ(before.files[0].path, a.path);
    const auto after = f.store.ReadEvidence((f.now - 100) * 1000, f.now * 1000, (f.now + 1) * 1000);
    CHECK_EQ(after.files.size(), size_t(2));
    CHECK_EQ(before.watermark, after.watermark);
}

TEST(Evidence, CheckpointsAreIncrementalAndSurviveRestartWithoutDoubleCredit)
{
    EvidenceFixture f;
    const int64_t start = (f.now - 90) * 1000;
    const auto slices = FocusTrackerTestAccess::Slices(start);
    REQUIRE(slices.size() == 2);
    CHECK_EQ(slices[0].endMs, slices[1].startMs);
    CHECK_EQ(slices[0].ctx.evidence.episodeId, slices[1].ctx.evidence.episodeId);
    CHECK(slices[0].ctx.evidence.open);
    CHECK(!slices[1].ctx.evidence.open);
    for (const auto& slice : slices)
        REQUIRE(f.store.RecordFocus(slice));
    f.Flush();
    f.store.Close();
    REQUIRE(f.store.Open(f.dir.File(L"evidence.db")));
    for (const auto& slice : slices)
        REQUIRE(f.store.RecordFocus(slice));
    const auto evidence = f.Read();
    REQUIRE(evidence.episodes.size() == 2);
    CHECK_EQ(evidence.focus.size(), size_t(2));
    int64_t active = 0;
    for (const auto& e : evidence.episodes)
        active += e.activeMs;
    CHECK_EQ(active, 40000);
    const auto sessions = f.history.GetSessions(f.now - 100, f.now, 10);
    REQUIRE(sessions.size() == 1);
    CHECK_EQ(sessions[0].activeSecs, 40);
}

TEST(Evidence, CrashDoesNotExtrapolateTheUncheckpointedTail)
{
    EvidenceFixture f;
    auto r = f.Focus((f.now - 70) * 1000, (f.now - 40) * 1000, "open:checkpoint");
    REQUIRE(f.store.RecordFocus(r));
    f.Flush();
    f.store.Close();
    REQUIRE(f.store.Open(f.dir.File(L"evidence.db")));
    const auto evidence = f.Read();
    REQUIRE(evidence.episodes.size() == 1);
    CHECK_EQ(evidence.episodes[0].endMs, r.endMs);
    CHECK_EQ(evidence.episodes[0].activeMs, 30000);
    CHECK(evidence.episodes[0].open);
    CHECK(evidence.partial);
}

TEST(Evidence, FinalPageExposureDoesNotRequireASuccessorOrInventActiveTime)
{
    EvidenceFixture f;
    EpisodeRecord page;
    page.id = "page:final";
    page.revision = 1;
    page.windowInstance = "window:one";
    page.startMs = (f.now - 45) * 1000;
    page.endMs = (f.now - 15) * 1000;
    page.exposureMs = 30000;
    page.unknownMs = 30000;
    page.entityKey = "https://example.org/PlanA";
    page.display = L"https://example.org/PlanA";
    page.title = L"Final page";
    page.resolution = "resolved";
    page.boundaryReason = "window_closed";
    REQUIRE(f.store.RecordPageEpisode(page));
    REQUIRE(f.store.RecordPageEpisode(page));
    const auto e = f.Read();
    REQUIRE(e.episodes.size() == 1);
    CHECK_EQ(e.episodes[0].activeMs, -1);
    CHECK_EQ(e.episodes[0].exposureMs, 30000);
    CHECK_EQ(e.episodes[0].boundaryReason, std::string("window_closed"));
    const auto half = f.store.ReadEvidence(page.startMs + 15000, page.endMs);
    REQUIRE(half.episodes.size() == 1);
    CHECK_EQ(half.episodes[0].exposureMs, 15000);
}

TEST(Evidence, PageAndWindowReuseDoNotJoinDistinctEpisodes)
{
    EvidenceFixture f;
    for (int i = 0; i < 2; ++i)
    {
        EpisodeRecord page;
        page.id = "page:" + std::to_string(i);
        page.windowInstance = "window:" + std::to_string(i);
        page.revision = 1;
        page.startMs = (f.now - 60 + i * 20) * 1000;
        page.endMs = page.startMs + 10000;
        page.exposureMs = 10000;
        page.unknownMs = 10000;
        page.title = L"Same title";
        page.entityKey = i ? "https://example.org/B" : "https://example.org/A";
        page.display = EntityKey::FromUtf8(page.entityKey);
        REQUIRE(f.store.RecordPageEpisode(page));
    }
    const auto e = f.Read();
    REQUIRE(e.episodes.size() == 2);
    CHECK(e.episodes[0].id != e.episodes[1].id);
    CHECK(e.episodes[0].windowInstance != e.episodes[1].windowInstance);
}

TEST(Evidence, RenamesAndSafeSaveKeepLogicalIdentityButCopiesDoNot)
{
    EvidenceFixture f;
    auto first = f.File(L"Before.docx", -100);
    first.ctx.evidence.volumeId = "vol-a";
    first.ctx.evidence.fileId = "00000000000000000000000000000001";
    auto rename = first;
    rename.oldPath = first.path;
    rename.path = L"C:\\WarpEvidence\\After.docx";
    rename.action = L"RENAME";
    rename.eventMs += 1000;
    auto replace = rename;
    replace.oldPath.clear();
    replace.action = L"MODIFY";
    replace.eventMs += 1000;
    replace.ctx.evidence.fileId = "00000000000000000000000000000002";
    auto copy = replace;
    copy.path = L"D:\\WarpEvidence\\Copy.docx";
    copy.action = L"CREATE";
    copy.eventMs += 1000;
    copy.ctx.evidence.volumeId = "vol-b";
    REQUIRE(f.store.RecordFile(first));
    REQUIRE(f.store.RecordFile(rename));
    REQUIRE(f.store.RecordFile(replace));
    REQUIRE(f.store.RecordFile(copy));
    f.Flush();
    const auto moved = f.store.ResolveFileIdentity(EntityKey::FileKey(rename.path), replace.eventMs + 1);
    REQUIRE(moved.size() == 3);
    std::set<std::string> logical, physical;
    for (const auto& a : moved)
    {
        logical.insert(a.artifactId);
        physical.insert(a.physicalId);
    }
    CHECK_EQ(logical.size(), size_t(1));
    CHECK_EQ(physical.size(), size_t(2));
    const auto copied = f.store.ResolveFileIdentity(EntityKey::FileKey(copy.path), copy.eventMs + 1);
    REQUIRE(copied.size() == 1);
    CHECK(copied[0].artifactId != moved[0].artifactId);
    CHECK(f.store.ResolveFileIdentity(EntityKey::FileKey(first.path), rename.eventMs + 1).empty());
    CHECK(!f.store.ResolveFileIdentity(EntityKey::FileKey(first.path), first.eventMs).empty());
}

TEST(Evidence, FullWidthFileIdsAndPathFallbackAreNotConflated)
{
    EvidenceFixture f;
    auto a = f.File(L"A.pdf", -80), b = f.File(L"B.pdf", -70), fallback = f.File(L"Fallback.pdf", -60);
    a.ctx.evidence.volumeId = b.ctx.evidence.volumeId = "vol";
    a.ctx.evidence.fileId = "00000000000000010000000000000001";
    b.ctx.evidence.fileId = "00000000000000020000000000000001";
    REQUIRE(f.store.RecordFile(a));
    REQUIRE(f.store.RecordFile(b));
    REQUIRE(f.store.RecordFile(fallback));
    auto e = f.Read();
    REQUIRE(e.files.size() == 3);
    CHECK_EQ(e.files[0].evidence.identityBasis, std::string("path_fallback"));
    CHECK(e.files[1].evidence.artifactId != e.files[2].evidence.artifactId);
    CHECK(e.files[1].evidence.fileId != e.files[2].evidence.fileId);
}

TEST(Evidence, MissingDisabledAndDroppedCoverageAreNotInactivity)
{
    EvidenceFixture f;
    SourceCoverage c;
    c.source = "files";
    c.startMs = (f.now - 100) * 1000;
    c.endMs = f.now * 1000;
    c.state = "disabled";
    c.reason = "user_setting";
    REQUIRE(f.store.RecordCoverage(c));
    c.source = "storage";
    c.state = "degraded";
    c.dropped = 7;
    c.reason = "queue_overflow";
    REQUIRE(f.store.RecordCoverage(c));
    const auto e = f.Read(100);
    CHECK(e.partial);
    REQUIRE(e.coverage.size() == 2);
    CHECK_EQ(e.coverage[0].state, std::string("disabled"));
    CHECK_EQ(e.coverage[1].dropped, uint64_t(7));
    auto json = WarpJson::Parse(ActivityStore::EvidenceJson(e));
    REQUIRE(json.ok);
    CHECK(json.value["partial"].asBool());
    CHECK(json.value["observations"].items().empty());
}

TEST(Evidence, BoundedQueriesExplicitlyReportTruncation)
{
    EvidenceFixture f;
    for (int i = 0; i < 5; ++i)
        REQUIRE(f.store.RecordFile(f.File((L"File" + std::to_wstring(i) + L".pdf").c_str(), -10 - i)));
    f.Flush();
    const auto e = f.store.ReadEvidence((f.now - 100) * 1000, f.now * 1000, 0, 2);
    CHECK(e.truncated);
    CHECK(e.partial);
    CHECK_EQ(e.files.size(), size_t(2));
}

TEST(Evidence, ScopedDeletionRemovesAliasesEpisodesAndProvenance)
{
    EvidenceFixture f;
    auto a = f.File(L"Before.docx", -100), b = a;
    b.oldPath = a.path;
    b.path = L"C:\\WarpEvidence\\After.docx";
    b.action = L"RENAME";
    b.eventMs += 1000;
    REQUIRE(f.store.RecordFile(a));
    REQUIRE(f.store.RecordFile(b));
    auto focus = f.Focus((f.now - 80) * 1000, (f.now - 50) * 1000, "focus:deleted");
    focus.docKey = EntityKey::FileKey(b.path);
    focus.docDisplay = b.path;
    REQUIRE(f.store.RecordFocus(focus));
    f.Flush();
    ActivityStore::PurgeSpec spec;
    spec.entityKey = EntityKey::FileKey(b.path);
    int64_t removed = 0;
    REQUIRE(f.store.Purge(spec, removed));
    CHECK_EQ(Count(f.store, "artifact_alias"), 0);
    CHECK_EQ(Count(f.store, "observation"), 0);
    CHECK_EQ(Count(f.store, "activity_episode"), 0);
    CHECK(f.Read().files.empty());
    REQUIRE(f.store.RecordFile(a));
    CHECK(f.Read().files.empty());
}

TEST(Evidence, TimePurgeAndClearRemoveDurableCheckpoints)
{
    EvidenceFixture f;
    auto r = f.Focus((f.now - 100) * 1000, (f.now - 70) * 1000, "focus:time");
    REQUIRE(f.store.RecordFocus(r));
    SourceCoverage c;
    c.source = "focus";
    c.startMs = r.startMs;
    c.endMs = r.endMs;
    c.state = "healthy";
    REQUIRE(f.store.RecordCoverage(c));
    f.Flush();
    ActivityStore::PurgeSpec spec;
    spec.startUtc = f.now - 100;
    spec.endUtc = f.now - 70;
    int64_t removed = 0;
    REQUIRE(f.store.Purge(spec, removed));
    CHECK_EQ(Count(f.store, "activity_episode"), 0);
    CHECK_EQ(Count(f.store, "source_coverage"), 0);
    REQUIRE(f.store.ClearAll());
    CHECK_EQ(Count(f.store, "observation"), 0);
    CHECK_EQ(Count(f.store, "artifact_alias"), 0);
}

TEST(Evidence, UnknownHistoricForegroundIsNotTheCurrentForeground)
{
    EventContextUtil::ProcessInfo before;
    before.pid = 987654;
    before.sessionId = EventContextUtil::OwnSessionId();
    before.startMs = 100000;
    before.startKey = 123;
    before.imagePath = L"C:\\Fixture\\Old.exe";
    EventContextUtil::RegisterProcess(before);
    auto captured = EventContextUtil::CaptureContextAt(before.pid, 110000, true);
    CHECK_EQ(captured.sourceExe, before.imagePath);
    CHECK_EQ(captured.foregroundPid, DWORD(0));
    CHECK_EQ(captured.msSinceInput, DWORD(0xFFFFFFFF));
    auto after = before;
    after.startMs = 200000;
    after.startKey = 456;
    after.imagePath = L"C:\\Fixture\\New.exe";
    EventContextUtil::RegisterProcess(after);
    auto late = EventContextUtil::CaptureContextAt(before.pid, 110000);
    CHECK(late.sourceExe.empty());
    CHECK(late.evidence.processInstance.empty());
}

TEST(Evidence, IntervalApiRequiresActivityScopeAndStrictBounds)
{
    EvidenceFixture f;
    ApiServer api;
    ApiServer::Deps deps;
    deps.store = &f.store;
    api.Configure(deps);
    ApiClient client;
    client.activityScope = false;
    auto denied = WarpJson::Parse(api.Handle("{\"op\":\"GetContextEvidence\",\"start\":1,\"end\":2}", client));
    CHECK_EQ(denied.value["error"]["code"].asString(), std::string("forbidden"));
    auto invalid =
        WarpJson::Parse(api.Handle("{\"op\":\"GetContextEvidence\",\"start\":2,\"end\":1}", ApiClient::InProcess()));
    CHECK_EQ(invalid.value["error"]["code"].asString(), std::string("invalid_request"));
    auto sparse =
        WarpJson::Parse(api.Handle("{\"op\":\"GetContextEvidence\",\"start\":1,\"end\":2}", ApiClient::InProcess()));
    REQUIRE(sparse.ok);
    CHECK(sparse.value["partial"].asBool());
    CHECK(sparse.value["available"].asBool());
}

TEST(Evidence, BrowserCheckpointClosesFinalPageExactlyOnce)
{
    const auto slices = WebCapture::TestPageCheckpoints();
    REQUIRE(slices.size() == 2);
    CHECK_EQ(slices[0].exposureMs, 30000);
    CHECK_EQ(slices[1].exposureMs, 10000);
    CHECK(slices[0].open);
    CHECK(!slices[1].open);
    CHECK_EQ(slices[0].endMs, slices[1].startMs);
    CHECK_EQ(slices[0].id, slices[1].id);
    CHECK(!WebCapture::SamePageIdentity(L"https://example.org/A", L"https://example.org/B", L"same", L"same", false));
    CHECK(!WebCapture::SamePageIdentity(L"https://example.org/A", L"", L"same", L"same", false));
    CHECK(WebCapture::SamePageIdentity(L"https://example.org/A", L"https://example.org/A", L"old", L"new", false));
}

TEST(Evidence, BrowserRejectsReadsFromPreviousForegroundOrWindowLifetime)
{
    CHECK(WebCapture::TestStaleRequestGuards());
}

TEST(Evidence, MissingProvenanceIsPartialEvenWithHealthyCoverage)
{
    EvidenceFixture f;
    REQUIRE(f.store.RecordFile(f.File()));
    const int64_t start = (f.now - 100) * 1000, end = f.now * 1000;
    for (const char* source : {"files", "launch", "browser", "focus", "presence", "storage"})
    {
        SourceCoverage coverage;
        coverage.source = source;
        coverage.instance = "fixture";
        coverage.startMs = start;
        coverage.endMs = end;
        coverage.state = "healthy";
        coverage.receivedMs = end;
        REQUIRE(f.store.RecordCoverage(coverage));
    }
    REQUIRE(f.store.RunSync([&](WriterContext& w) {
        const std::string sql =
            "UPDATE meta SET value='" + std::to_string(start) + "' WHERE key='evidence_available_from'";
        if (sqlite3_exec(w.db, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(w.db));
    }));
    CHECK(!f.store.ReadEvidence(start, end).partial);
    REQUIRE(f.store.RunSync([](WriterContext& w) {
        if (sqlite3_exec(w.db, "DELETE FROM observation", nullptr, nullptr, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(w.db));
    }));
    const auto evidence = f.store.ReadEvidence(start, end);
    REQUIRE(evidence.files.size() == 1);
    CHECK(evidence.files[0].evidence.id.empty());
    CHECK(evidence.partial);
}

TEST(Evidence, ReadSnapshotCannotMixDifferentWriterTransactions)
{
    EvidenceFixture f;
    bool wrote = false;
    EvidenceReadTestAccess::BetweenTypes(f.store, [&] {
        if (!wrote)
        {
            REQUIRE(f.store.RunSync([&](WriterContext& w) {
                const std::string ts = std::to_string(f.now - 30);
                const auto sql = "INSERT INTO file_activity(timestamp,action,path) VALUES(" + ts +
                                 ",'OPEN','C:\\Fixture\\Pair.txt');"
                                 "INSERT INTO browsing_activity(timestamp,browser,title,url) VALUES(" +
                                 ts + ",'fixture','pair','https://example.org/Pair');";
                if (sqlite3_exec(w.db, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK)
                    throw std::runtime_error(sqlite3_errmsg(w.db));
            }));
            wrote = true;
        }
    });
    const auto evidence = f.store.ReadEvidence((f.now - 60) * 1000, f.now * 1000);
    CHECK(wrote);
    CHECK(evidence.files.empty());
    CHECK(evidence.browsing.empty());
    CHECK_EQ(Count(f.store, "file_activity"), 1);
    CHECK_EQ(Count(f.store, "browsing_activity"), 1);
    EvidenceReadTestAccess::BetweenTypes(f.store, {});
}

TEST(Evidence, LegacyV3UpgradeKeepsUnknownProvenanceRatherThanFabricatingIt)
{
    EvidenceFixture f;
    auto file = f.File();
    REQUIRE(f.store.RecordFile(file));
    f.Flush();
    f.store.Close();
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open16(f.dir.File(L"evidence.db").c_str(), &db) == SQLITE_OK);
    const char* downgrade =
        "DROP TRIGGER evidence_delete_file; DROP TRIGGER evidence_delete_launch; DROP TRIGGER evidence_delete_browse;"
        "DROP TRIGGER evidence_delete_focus; DROP TRIGGER evidence_delete_presence;"
        "DROP TABLE observation; DROP TABLE activity_episode; DROP TABLE source_coverage; DROP TABLE artifact_alias;"
        "DELETE FROM meta WHERE key='evidence_available_from'; PRAGMA user_version=3;";
    REQUIRE(sqlite3_exec(db, downgrade, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
    ActivityStore restored;
    REQUIRE(restored.Open(f.dir.File(L"evidence.db")));
    const auto evidence = restored.ReadEvidence((f.now - 100) * 1000, f.now * 1000);
    REQUIRE(evidence.files.size() == 1);
    CHECK_EQ(restored.GetStats().schemaVersion, 4);
    CHECK(evidence.files[0].evidence.id.empty());
    CHECK(evidence.partial);
    CHECK(evidence.episodes.empty());
    restored.Close();
}

TEST(Evidence, DomainPurgeScrubsCloudAndDownloadProvenanceWithoutDeletingLocalFile)
{
    EvidenceFixture f;
    auto file = f.File();
    file.originUrl = L"https://example.org:8443/Download";
    file.ctx.evidence.cloudUrl = "https://docs.example.org/Plan";
    REQUIRE(f.store.RecordFile(file));
    f.Flush();
    ActivityStore::PurgeSpec spec;
    spec.domain = L"example.org";
    int64_t removed = 0;
    REQUIRE(f.store.Purge(spec, removed));
    const auto evidence = f.Read();
    REQUIRE(evidence.files.size() == 1);
    CHECK(evidence.files[0].originUrl.empty());
    CHECK(evidence.files[0].evidence.cloudUrl.empty());
}

TEST(Evidence, RedactedEpisodeCannotRetainContent)
{
    EvidenceFixture f;
    auto focus = f.Focus((f.now - 100) * 1000, (f.now - 70) * 1000, "focus:redacted");
    focus.ctx.evidence.redaction = "content_withheld";
    REQUIRE(f.store.RecordFocus(focus));
    EpisodeRecord page;
    page.id = "page:redacted";
    page.revision = 1;
    page.startMs = (f.now - 60) * 1000;
    page.endMs = (f.now - 30) * 1000;
    page.entityKey = "https://example.org/Private";
    page.display = L"https://example.org/Private";
    page.title = L"Private";
    page.exposureMs = 30000;
    page.evidence.redaction = "content_withheld";
    REQUIRE(f.store.RecordPageEpisode(page));
    const auto e = f.Read();
    REQUIRE(e.focus.size() == 1);
    CHECK(e.focus[0].windowTitle.empty());
    CHECK(e.focus[0].docKey.empty());
    for (const auto& episode : e.episodes)
    {
        CHECK(episode.title.empty());
        CHECK(episode.entityKey.empty());
    }
    CHECK(e.partial);
}

TEST(Evidence, StableObservationFingerprintAndModelFileHashesAreSha256)
{
    const std::string expected = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    CHECK_EQ(Evidence::Fingerprint("abc"), expected);
    CHECK(Evidence::Fingerprint("abc") != Evidence::Fingerprint("Abc"));
    wt::TempDir dir;
    const auto path = dir.File(L"model.bin");
    {
        std::ofstream out(std::filesystem::path(path), std::ios::binary);
        out << "abc";
    }
    CHECK_EQ(Evidence::FileSha256(path), expected);
    CHECK(Evidence::FileSha256(dir.File(L"missing.bin")).empty());
}

TEST(Evidence, ScopedPrivacyFloorsSurviveRestartWithoutKeepingPlaintextScopes)
{
    EvidenceFixture f;
    auto old = f.File();
    REQUIRE(f.store.RecordFile(old));
    f.Flush();
    ActivityStore::PurgeSpec spec;
    spec.pathPrefix = L"C:\\WarpEvidence";
    int64_t removed = 0;
    REQUIRE(f.store.Purge(spec, removed));
    f.store.Close();
    ActivityStore restored;
    REQUIRE(restored.Open(f.dir.File(L"evidence.db")));
    REQUIRE(restored.RecordFile(old));
    REQUIRE(restored.RunSync([](WriterContext&) {}));
    CHECK_EQ(Count(restored, "file_activity"), 0);
    auto fresh = old;
    fresh.eventMs = StoreUtil::NowMs();
    fresh.ctx = EventContext{};
    REQUIRE(restored.RecordFile(fresh));
    REQUIRE(restored.RunSync([](WriterContext&) {}));
    CHECK_EQ(Count(restored, "file_activity"), 1);
    auto lease = restored.AcquireReader();
    REQUIRE(lease);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(lease.get(), "SELECT scope_hash FROM privacy_scope", -1, &st, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    const std::string hash = reinterpret_cast<const char*>(sqlite3_column_text(st, 0));
    CHECK_EQ(hash.size(), size_t(64));
    CHECK(hash.find("warpevidence") == std::string::npos);
    sqlite3_finalize(st);
    lease = ActivityStore::ReadLease{};
    restored.Close();
}

TEST(Evidence, HistoricalAttributionPreservesWakeAttenuationWithoutBorrowingCurrentInput)
{
    EventContextUtil::SetWakeBoundary(GetTickCount64() + 10000);
    auto recent = EventContextUtil::CaptureContextAt(0, StoreUtil::NowMs());
    CHECK_NEAR(recent.confidence, 0.2, 1e-9);
    auto replay = EventContextUtil::CaptureContextAt(0, StoreUtil::NowMs() - 600000, true);
    CHECK_EQ(replay.foregroundPid, DWORD(0));
    CHECK_EQ(replay.msSinceInput, DWORD(0xFFFFFFFFu));
    EventContextUtil::SetWakeBoundary(0);
}

TEST(Evidence, UntimedRecencyObservationDoesNotCreateFreshEngagement)
{
    EvidenceFixture f;
    auto file = f.File();
    file.ctx.evidence.timestampKind = "recency_observation";
    REQUIRE(f.store.RecordFile(file));
    f.Flush();
    CHECK_EQ(Count(f.store, "file_activity"), 1);
    const auto entities = f.entities.Lookup({EntityKey::FileKey(file.path)});
    REQUIRE(entities.size() == 1);
    CHECK(entities[0].entityKey.empty());
}

TEST(Evidence, RealSamplerCheckpointsAnUnchangedDocument)
{
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE captured = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(ready && captured);
    std::atomic<HWND> window{nullptr};
    std::thread owner([&] {
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"WarpCheckpointFixture";
        cls.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
            if (message == WM_DESTROY)
            {
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(hwnd, message, wp, lp);
        };
        RegisterClassW(&cls);
        window = CreateWindowExW(0, cls.lpszClassName, L"Synthetic checkpoint", 0, 0, 0, 200, 100, nullptr, nullptr,
                                 cls.hInstance, nullptr);
        SetEvent(ready);
        if (window.load())
        {
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0)
                DispatchMessageW(&message);
        }
        UnregisterClassW(cls.lpszClassName, cls.hInstance);
    });
    const bool created = WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0 && window.load();
    bool checkpoint = false;
    if (created)
    {
        FocusTracker tracker;
        tracker.SetCallback([&](FocusEventRecord&& row) {
            if (row.ctx.evidence.open && row.ctx.evidence.boundaryReason == "checkpoint" &&
                row.endMs - row.startMs >= 30000)
                SetEvent(captured);
        });
        FocusTrackerTestAccess::RunSampler(tracker, window.load());
        checkpoint = WaitForSingleObject(captured, 8000) == WAIT_OBJECT_0;
        tracker.Stop();
        PostMessageW(window.load(), WM_CLOSE, 0, 0);
    }
    else
        PostThreadMessageW(GetThreadId(owner.native_handle()), WM_QUIT, 0, 0);
    owner.join();
    CloseHandle(ready);
    CloseHandle(captured);
    CHECK(created);
    CHECK(checkpoint);
}
