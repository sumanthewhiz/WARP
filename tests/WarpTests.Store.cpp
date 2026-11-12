// Storage core tests (gap analysis BUG-1/3, RANK-1..4, DATA-2..7, PRIV-2/4, HIST-1..4, API-6 feed):
// single-writer store + EntityStore + HistoryEngine against a real SQLite file in %TEMP%.
#include "WarpTests.h"

#include "ActivityStore.h"
#include "EntityKey.h"
#include "EntityStore.h"
#include "HistoryEngine.h"
#include "sqlite3.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <set>
#include <thread>

namespace
{
    struct Fixture
    {
        wt::TempDir   dir;
        ActivityStore store;
        EntityStore   ents{ &store };
        HistoryEngine hist{ &store };
        bool          open = false;

        Fixture()
        {
            store.AddSink(&ents);
            store.AddSink(&hist);
            open = store.Open(dir.File(L"activity.db"));
        }
        ~Fixture() { store.Close(); }     // before the sinks are destroyed

        // Every job enqueued before this one has committed when it returns true.
        bool Barrier() { return store.RunSync([](WriterContext&) {}, 30000); }
    };

    FileEventRecord FileEv(const wchar_t* action, const std::wstring& path, const std::wstring& oldPath = std::wstring(),
                           const wchar_t* source = L"etw", double confidence = 1.0, int64_t eventMs = 0)
    {
        FileEventRecord r;
        r.action = action;
        r.path = path;
        r.oldPath = oldPath;
        r.source = source;
        r.ctx.confidence = confidence;
        r.eventMs = eventMs;
        return r;
    }

    BrowseEventRecord WebEv(const std::wstring& url, const std::wstring& host, const std::wstring& title,
                            int64_t windowId = 0, int dwellSecs = 0)
    {
        BrowseEventRecord r;
        r.browser = L"msedge.exe";
        r.url = url;
        r.host = host;
        r.title = title;
        r.windowId = windowId;
        r.dwellSecs = dwellSecs;
        r.ctx.confidence = 1.0;
        return r;
    }

    EntityRecord One(EntityStore& es, const std::string& key)
    {
        std::vector<EntityRecord> v = es.Lookup({ key });
        return v.empty() ? EntityRecord() : v[0];
    }

    bool HasTombstone(const EntityChanges& ch, const std::string& key, const char* reason)
    {
        for (const EntityTombstone& t : ch.deletes)
            if (t.entityKey == key && t.reason == reason) return true;
        return false;
    }
}

TEST(Store, OpensFreshDatabaseAtSchemaV4)
{
    Fixture f;
    REQUIRE(f.open);
    ActivityStore::Stats s = f.store.GetStats();
    CHECK_EQ(s.schemaVersion, 4);
    std::string fts;
    CHECK(f.store.GetMeta("fts_available", fts));
    CHECK_EQ(fts, std::string("1"));                  // the test build compiles SQLite with FTS5
    f.store.SetMeta("test-key", "v1");
    REQUIRE(f.Barrier());
    std::string v;
    CHECK(f.store.GetMeta("test-key", v));
    CHECK_EQ(v, std::string("v1"));
}

TEST(Store, RecordsFileEditsAsEntities)
{
    Fixture f;
    REQUIRE(f.open);
    const std::wstring p = L"C:\\WarpTest\\Plans\\Q3 Plan.docx";
    CHECK(f.store.RecordFile(FileEv(L"MODIFY", p, L"", L"etw", 0.8)));
    REQUIRE(f.Barrier());
    EntityRecord r = One(f.ents, EntityKey::FileKey(p));
    REQUIRE(!r.entityKey.empty());
    CHECK_EQ(r.entityType, std::string("file"));
    CHECK_EQ(r.displayName, p);
    CHECK_NEAR(r.editCountTotal, 0.8, 1e-9);
    CHECK(r.lastEditTs > 0);
    CHECK_EQ(r.distinctDays, 1);
    std::vector<FileActivity> rows = f.store.QueryFiles(0, wt::NowUtc() + 60);
    REQUIRE(rows.size() == 1);
    CHECK_EQ(rows[0].action, std::wstring(L"MODIFY"));
    CHECK_EQ(rows[0].entityKey, EntityKey::FileKey(p));
    CHECK_NEAR(rows[0].confidence, 0.8, 1e-9);
}

TEST(Store, AutosaveBurstsAreCreditedOnce)
{
    Fixture f;
    REQUIRE(f.open);
    const std::wstring p = L"C:\\WarpTest\\notes.md";
    const int64_t t0 = wt::NowMs() - 5 * 60 * 1000;
    for (int i = 0; i < 10; ++i) f.store.RecordFile(FileEv(L"MODIFY", p, L"", L"etw", 1.0, t0 + i * 2000));
    REQUIRE(f.Barrier());
    CHECK_NEAR(One(f.ents, EntityKey::FileKey(p)).editCountTotal, 1.0, 1e-9);
}

TEST(Store, SafeSaveKeepsTheDocumentAndItsHistory)
{
    Fixture f;
    REQUIRE(f.open);
    const std::wstring dir = L"C:\\WarpTest\\Docs\\";
    const std::wstring doc = dir + L"Report.docx";
    const std::string key = EntityKey::FileKey(doc);
    const int64_t t0 = wt::NowMs() - 10 * 60 * 1000;
    const int64_t t1 = t0 + 5 * 60 * 1000;
    f.store.RecordFile(FileEv(L"MODIFY", doc, L"", L"etw", 1.0, t0));
    // Word's safe-save: original -> temp name, new temp -> original name, old temp deleted.
    f.store.RecordFile(FileEv(L"RENAME", dir + L"~WRL0001.tmp", doc, L"etw", 1.0, t1));
    f.store.RecordFile(FileEv(L"RENAME", doc, dir + L"~WRL0002.tmp", L"etw", 1.0, t1 + 50));
    f.store.RecordFile(FileEv(L"DELETE", dir + L"~WRL0001.tmp", L"", L"etw", 1.0, t1 + 80));
    REQUIRE(f.Barrier());
    REQUIRE(f.store.ScheduleMaintenance());
    REQUIRE(f.Barrier());
    EntityRecord r = One(f.ents, key);
    REQUIRE(!r.entityKey.empty());
    CHECK_NEAR(r.editCountTotal, 2.0, 1e-9);           // first edit + the atomic save five minutes later
    EntityChanges ch = f.ents.GetChanges(0, 1000);
    for (const EntityTombstone& t : ch.deletes) CHECK(t.entityKey != key);
}

TEST(Store, RenameMovesEngagementToTheNewKey)
{
    Fixture f;
    REQUIRE(f.open);
    const std::wstring a = L"C:\\WarpTest\\Notes\\draft.txt";
    const std::wstring b = L"C:\\WarpTest\\Notes\\final.txt";
    f.store.RecordFile(FileEv(L"OPEN", a, L"", L"recent"));
    f.store.RecordFile(FileEv(L"MODIFY", a));
    REQUIRE(f.Barrier());
    const int64_t cursor = f.ents.GetChanges(0, 1000).nextSeq;
    f.store.RecordFile(FileEv(L"RENAME", b, a));
    REQUIRE(f.Barrier());
    CHECK(One(f.ents, EntityKey::FileKey(a)).entityKey.empty());
    EntityRecord r = One(f.ents, EntityKey::FileKey(b));
    REQUIRE(!r.entityKey.empty());
    CHECK_EQ(r.displayName, b);
    CHECK_NEAR(r.openCountTotal, 1.0, 1e-9);
    CHECK_NEAR(r.editCountTotal, 1.0, 1e-9);
    CHECK(r.engS > 0.0);
    CHECK(r.openCount7d > 0.0);                          // daily rollups moved with the entity
    EntityChanges ch = f.ents.GetChanges(cursor, 1000);
    CHECK(!ch.reset);
    CHECK(HasTombstone(ch, EntityKey::FileKey(a), "renamed"));
    bool upsert = false;
    for (const EntityRecord& u : ch.upserts) if (u.entityKey == EntityKey::FileKey(b)) upsert = true;
    CHECK(upsert);
}

TEST(Store, FolderRenameRekeysContainedFiles)
{
    Fixture f;
    REQUIRE(f.open);
    f.store.RecordFile(FileEv(L"MODIFY", L"C:\\WarpTest\\Proj\\src\\main.cpp"));
    f.store.RecordFile(FileEv(L"MODIFY", L"C:\\WarpTest\\Proj\\README.md"));
    f.store.RecordFile(FileEv(L"MODIFY", L"C:\\WarpTest\\Project\\other.md"));
    REQUIRE(f.Barrier());
    FileEventRecord mv = FileEv(L"RENAME", L"C:\\WarpTest\\Proj2", L"C:\\WarpTest\\Proj");
    mv.isDir = true;
    f.store.RecordFile(std::move(mv));
    REQUIRE(f.Barrier());
    CHECK(!One(f.ents, EntityKey::FileKey(L"C:\\WarpTest\\Proj2\\src\\main.cpp")).entityKey.empty());
    CHECK(!One(f.ents, EntityKey::FileKey(L"C:\\WarpTest\\Proj2\\README.md")).entityKey.empty());
    CHECK(One(f.ents, EntityKey::FileKey(L"C:\\WarpTest\\Proj\\README.md")).entityKey.empty());
    CHECK(!One(f.ents, EntityKey::FileKey(L"C:\\WarpTest\\Project\\other.md")).entityKey.empty());   // prefix sibling untouched
}

TEST(Store, DeleteBecomesATombstoneAfterTheGraceWindow)
{
    Fixture f;
    REQUIRE(f.open);
    f.ents.SetDeleteGraceForTesting(0);
    const std::wstring p = L"C:\\WarpTest\\old.csv";
    const std::string key = EntityKey::FileKey(p);
    f.store.RecordFile(FileEv(L"MODIFY", p));
    f.store.RecordFile(FileEv(L"DELETE", p));
    REQUIRE(f.Barrier());
    CHECK(One(f.ents, key).entityKey.empty());          // hidden immediately
    REQUIRE(f.store.ScheduleMaintenance());
    REQUIRE(f.Barrier());
    EntityChanges ch = f.ents.GetChanges(0, 1000);
    CHECK(HasTombstone(ch, key, "deleted"));
    for (const EntityRecord& u : ch.upserts) CHECK(u.entityKey != key);
}

TEST(Store, ChangeFeedSignalsResetAfterClearAll)
{
    Fixture f;
    REQUIRE(f.open);
    f.store.RecordFile(FileEv(L"MODIFY", L"C:\\WarpTest\\a.txt"));
    REQUIRE(f.Barrier());
    EntityChanges first = f.ents.GetChanges(0, 1000);
    REQUIRE(first.nextSeq > 0);
    CHECK_EQ(first.upserts.size(), (size_t)1);
    REQUIRE(f.store.ClearAll());
    CHECK(f.ents.GetChanges(first.nextSeq, 1000).reset);
    CHECK(f.ents.Top("file", 10).empty());
    CHECK(f.store.QueryFiles(0, wt::NowUtc() + 60).empty());
    f.store.RecordFile(FileEv(L"MODIFY", L"C:\\WarpTest\\b.txt"));
    REQUIRE(f.Barrier());
    EntityChanges resync = f.ents.GetChanges(0, 1000);
    CHECK(!resync.reset);
    CHECK_EQ(resync.upserts.size(), (size_t)1);
}

TEST(Store, ChangeFeedPagesWithoutGaps)
{
    Fixture f;
    REQUIRE(f.open);
    for (int i = 0; i < 25; ++i)
        f.store.RecordFile(FileEv(L"MODIFY", L"C:\\WarpTest\\page\\f" + std::to_wstring(i) + L".txt"));
    REQUIRE(f.Barrier());
    std::set<std::string> seen;
    int64_t cursor = 0;
    for (int guard = 0; guard < 20; ++guard)
    {
        EntityChanges ch = f.ents.GetChanges(cursor, 7);
        CHECK(ch.upserts.size() <= 7);
        for (const EntityRecord& u : ch.upserts) seen.insert(u.entityKey);
        cursor = ch.nextSeq;
        if (!ch.more) break;
    }
    CHECK_EQ(seen.size(), (size_t)25);
    CHECK(f.ents.GetChanges(cursor, 7).upserts.empty());
}

TEST(Store, ScopedPurgeRemovesOnlyTheTarget)
{
    Fixture f;
    REQUIRE(f.open);
    f.store.RecordFile(FileEv(L"MODIFY", L"C:\\WarpTest\\Private\\diary.txt"));
    f.store.RecordFile(FileEv(L"MODIFY", L"C:\\WarpTest\\Private\\sub\\notes.txt"));
    f.store.RecordFile(FileEv(L"MODIFY", L"C:\\WarpTest\\PrivateNot\\keep.txt"));
    f.store.RecordBrowse(WebEv(L"https://www.bank.example/acct", L"www.bank.example", L"My account"));
    f.store.RecordBrowse(WebEv(L"https://news.example/story", L"news.example", L"Story"));
    REQUIRE(f.Barrier());

    ActivityStore::PurgeSpec byPath;
    byPath.pathPrefix = L"C:\\WarpTest\\Private";
    int64_t rows = 0;
    REQUIRE(f.store.Purge(byPath, rows));
    CHECK(rows > 0);
    CHECK(One(f.ents, EntityKey::FileKey(L"C:\\WarpTest\\Private\\diary.txt")).entityKey.empty());
    CHECK(One(f.ents, EntityKey::FileKey(L"C:\\WarpTest\\Private\\sub\\notes.txt")).entityKey.empty());
    CHECK(!One(f.ents, EntityKey::FileKey(L"C:\\WarpTest\\PrivateNot\\keep.txt")).entityKey.empty());
    for (const FileActivity& a : f.store.QueryFiles(0, wt::NowUtc() + 60))
        CHECK(a.path.find(L"\\Private\\") == std::wstring::npos);

    ActivityStore::PurgeSpec byDomain;
    byDomain.domain = L"bank.example";
    REQUIRE(f.store.Purge(byDomain, rows));
    std::vector<BrowsingActivity> web = f.store.QueryBrowsing(0, wt::NowUtc() + 60);
    REQUIRE(web.size() == 1);
    CHECK_EQ(web[0].host, std::wstring(L"news.example"));
    CHECK(f.ents.Search(L"account", "url", 10).empty());
    CHECK(HasTombstone(f.ents.GetChanges(0, 1000), EntityKey::UrlKey(L"https://www.bank.example/acct"), "purge"));
}

TEST(Store, PurgeReachesDocumentPagesAndRowsOfDeletedFiles)
{
    Fixture f;
    REQUIRE(f.open);
    f.ents.SetDeleteGraceForTesting(0);
    // A page keyed by its document id: only its display URL names the site.
    BrowseEventRecord page = WebEv(L"https://docs.example.com/document/d/abc123/edit", L"docs.example.com", L"Plan");
    page.docId = L"gdoc:abc123";
    f.store.RecordBrowse(std::move(page));
    // A file worked on and then deleted (tombstoned) before the purge: its focus row is still stored.
    const std::wstring gone = L"C:\\WarpTest\\Secret\\gone.txt";
    const std::string goneKey = EntityKey::FileKey(gone);
    FocusEventRecord fr;
    fr.startMs = wt::NowMs() - 120000;
    fr.endMs = wt::NowMs() - 60000;
    fr.exeName = L"notepad.exe";
    fr.exePath = L"C:\\Windows\\notepad.exe";
    fr.windowTitle = L"gone.txt - Notepad";
    fr.activeSecs = 60;
    fr.docKey = goneKey;
    fr.docDisplay = gone;
    fr.docType = "file";
    f.store.RecordFocus(std::move(fr));
    f.store.RecordFile(FileEv(L"DELETE", gone));
    REQUIRE(f.Barrier());
    REQUIRE(f.store.ScheduleMaintenance());
    REQUIRE(f.Barrier());
    auto focusRows = [&f, &goneKey]()
    {
        size_t n = 0;
        for (const AppFocusActivity& a : f.store.QueryFocus(0, wt::NowUtc() + 60))
            if (a.docKey == goneKey) ++n;
        return n;
    };
    const std::string docKey = EntityKey::DocKey(L"gdoc:abc123");
    REQUIRE(!One(f.ents, docKey).entityKey.empty());
    REQUIRE(One(f.ents, goneKey).entityKey.empty());
    REQUIRE(focusRows() == 1);

    int64_t rows = 0;
    ActivityStore::PurgeSpec byDomain;
    byDomain.domain = L"example.com";
    REQUIRE(f.store.Purge(byDomain, rows));
    CHECK(One(f.ents, docKey).entityKey.empty());
    CHECK(HasTombstone(f.ents.GetChanges(0, 1000), docKey, "purge"));

    ActivityStore::PurgeSpec byPath;
    byPath.pathPrefix = L"C:\\WarpTest\\Secret";
    REQUIRE(f.store.Purge(byPath, rows));
    CHECK_EQ(focusRows(), (size_t)0);
}

TEST(Store, TimeRangePurgeKeepsOtherRows)
{
    Fixture f;
    REQUIRE(f.open);
    const int64_t now = wt::NowMs();
    const std::wstring oldPath = L"C:\\WarpTest\\t\\old.txt", newPath = L"C:\\WarpTest\\t\\new.txt";
    const std::wstring mixedPath = L"C:\\WarpTest\\t\\mixed.txt";
    f.store.RecordFile(FileEv(L"MODIFY", oldPath, L"", L"etw", 1.0, now - 3 * 3600 * 1000LL));
    f.store.RecordFile(FileEv(L"MODIFY", mixedPath, L"", L"etw", 1.0, now - 2 * 3600 * 1000LL));
    f.store.RecordFile(FileEv(L"MODIFY", mixedPath, L"", L"etw", 1.0, now - 2 * 60 * 1000));
    f.store.RecordFile(FileEv(L"MODIFY", newPath, L"", L"etw", 1.0, now - 60 * 1000));
    REQUIRE(f.Barrier());
    const int64_t cursor = f.ents.GetChanges(0, 1000).nextSeq;
    ActivityStore::PurgeSpec last15;
    last15.startUtc = now / 1000 - 15 * 60;
    int64_t rows = 0;
    REQUIRE(f.store.Purge(last15, rows));
    std::vector<FileActivity> left = f.store.QueryFiles(0, wt::NowUtc() + 60);
    REQUIRE(left.size() == 2);
    for (const FileActivity& a : left) CHECK(a.timestampUtc < last15.startUtc);

    // PRIV-2: a document first touched inside the purged range is gone everywhere, including the
    // ranked entities and the change feed; one with earlier history survives without the purged part.
    CHECK(One(f.ents, EntityKey::FileKey(newPath)).entityKey.empty());
    EntityChanges ch = f.ents.GetChanges(cursor, 1000);
    CHECK(HasTombstone(ch, EntityKey::FileKey(newPath), "purge"));
    EntityRecord mixed = One(f.ents, EntityKey::FileKey(mixedPath));
    REQUIRE(!mixed.entityKey.empty());
    CHECK(mixed.lastEditTs < last15.startUtc);
    CHECK(mixed.lastEventTs < last15.startUtc);
    CHECK_NEAR(mixed.editCountTotal, 1.0, 1e-9);
    bool mixedUpdated = false;
    for (const EntityRecord& u : ch.upserts) if (u.entityKey == mixed.entityKey) mixedUpdated = true;
    CHECK(mixedUpdated);                                   // consumers learn about the recomputation
    CHECK(!One(f.ents, EntityKey::FileKey(oldPath)).entityKey.empty());
    for (const EntityScore& s : f.ents.Top("file", 10))
        CHECK(s.rec.entityKey != EntityKey::FileKey(newPath));
}

TEST(Store, TimeRangePurgeLeavesOtherEntitiesAndScrubsHistory)
{
    Fixture f;
    REQUIRE(f.open);
    const int64_t now = wt::NowMs();
    const int64_t nowS = now / 1000;
    const std::wstring other = L"C:\\WarpTest\\t\\other.txt", oldDoc = L"C:\\WarpTest\\t\\old.txt", fresh = L"C:\\WarpTest\\t\\fresh.txt";
    // An autosave burst outside the range is credited once; a purge elsewhere in the day must not re-score it.
    f.store.RecordFile(FileEv(L"MODIFY", other, L"", L"etw", 1.0, now - 40 * 60 * 1000));
    f.store.RecordFile(FileEv(L"MODIFY", other, L"", L"etw", 1.0, now - 40 * 60 * 1000 + 30 * 1000));
    f.store.RecordFile(FileEv(L"MODIFY", oldDoc, L"", L"etw", 1.0, now - 3 * 3600 * 1000LL));
    f.store.RecordFile(FileEv(L"MODIFY", fresh, L"", L"etw", 1.0, now - 2 * 60 * 1000));
    PresenceRecord p;
    p.startMs = now - 60 * 60 * 1000;
    p.endMs = now - 60 * 1000;
    p.state = "active";
    f.store.RecordPresence(std::move(p));
    REQUIRE(f.Barrier());

    auto snap = [](int64_t ts, const std::wstring& path, const char* label)
    {
        SnapshotInput in;
        in.ts = ts;
        in.windowSecs = 300;
        in.label = label;
        SnapshotEntity e;
        e.entityKey = EntityKey::FileKey(path);
        e.entityType = "file";
        e.display = path;
        e.activeSecs = 120.0;
        in.entities.push_back(e);
        return in;
    };
    const int64_t kept = f.hist.RecordSnapshot(snap(nowS - 3 * 3600, oldDoc, "Budget review"));
    REQUIRE(kept != 0);
    const int64_t relabeled = f.hist.RecordSnapshot(snap(nowS - 5 * 60, oldDoc, "Secret plan"));
    CHECK_EQ(relabeled, kept);                                                       // same thread, relabeled
    const int64_t gone = f.hist.RecordSnapshot(snap(nowS - 2 * 60, fresh, "Only now"));
    REQUIRE(gone != 0 && gone != kept);

    const EntityRecord otherBefore = One(f.ents, EntityKey::FileKey(other));
    REQUIRE(!otherBefore.entityKey.empty());
    CHECK_NEAR(otherBefore.editCountTotal, 1.0, 1e-9);

    ActivityStore::PurgeSpec last15;
    last15.startUtc = nowS - 15 * 60;
    int64_t rows = 0;
    REQUIRE(f.store.Purge(last15, rows));

    // Same-day entities without activity inside the range keep their live rollups and are not re-scored.
    const EntityRecord otherAfter = One(f.ents, EntityKey::FileKey(other));
    CHECK_EQ(otherAfter.changeSeq, otherBefore.changeSeq);
    CHECK_NEAR(otherAfter.editCountTotal, 1.0, 1e-9);
    CHECK(One(f.ents, EntityKey::FileKey(fresh)).entityKey.empty());

    // A thread that only existed inside the range is gone; the other loses the purged span and the
    // label written from it (rebuilt from its remaining members).
    std::vector<WorkThreadInfo> threads = f.hist.GetThreads(0, 0, 10, true);
    REQUIRE(threads.size() == 1);
    CHECK_EQ(threads[0].id, kept);
    CHECK_EQ(threads[0].label, std::string("old.txt"));
    CHECK(threads[0].lastTs < last15.startUtc);

    // The work session now ends before the range, its seconds scaled to the part that remains.
    std::vector<WorkSessionInfo> sessions = f.hist.GetSessions(0, 0, 10);
    REQUIRE(sessions.size() == 1);
    CHECK_EQ(sessions[0].endTs, last15.startUtc - 1);
    CHECK(sessions[0].activeSecs > 2600 && sessions[0].activeSecs < 2800);
}

TEST(Store, LateRecordsDoNotRefillAPurgedRange)
{
    Fixture f;
    REQUIRE(f.open);
    const int64_t now = wt::NowMs();
    ActivityStore::PurgeSpec last5;
    last5.startUtc = now / 1000 - 5 * 60;
    int64_t rows = 0;
    REQUIRE(f.store.Purge(last5, rows));

    // Reported after the purge but about the purged time (an Office MRU poll, a held safe-save): not stored.
    const std::wstring late = L"C:\\WarpTest\\late\\secret.docx";
    f.store.RecordFile(FileEv(L"OPEN", late, L"", L"mru", 1.0, now - 2 * 60 * 1000));
    // A focus session that was still open during the purge keeps only its parts outside the purged time.
    FocusEventRecord fr;
    fr.startMs = now - 20 * 60 * 1000;
    fr.endMs = wt::NowMs() + 5 * 60 * 1000;
    fr.exeName = L"notepad.exe";
    fr.exePath = L"C:\\Windows\\notepad.exe";
    fr.windowTitle = L"notes.txt - Notepad";
    fr.activeSecs = 1500;
    f.store.RecordFocus(std::move(fr));
    // A presence interval that was open too, reported after it: its earlier part joins the earlier session.
    PresenceRecord pr;
    pr.startMs = now - 25 * 60 * 1000;
    pr.endMs = wt::NowMs() + 4 * 60 * 1000;
    pr.state = "active";
    f.store.RecordPresence(std::move(pr));
    // New activity after the purge is recorded normally.
    f.store.RecordFile(FileEv(L"MODIFY", L"C:\\WarpTest\\late\\after.txt"));
    REQUIRE(f.Barrier());

    CHECK(One(f.ents, EntityKey::FileKey(late)).entityKey.empty());
    std::vector<FileActivity> files = f.store.QueryFiles(0, wt::NowUtc() + 3600);
    REQUIRE(files.size() == 1);
    CHECK(files[0].path.find(L"after.txt") != std::wstring::npos);
    std::vector<AppFocusActivity> focus = f.store.QueryFocus(0, wt::NowUtc() + 3600);
    REQUIRE(focus.size() == 2);
    for (const AppFocusActivity& a : focus)
        CHECK(a.endTs < last5.startUtc || a.startTs >= now / 1000);     // nothing inside the purged time
    // The parts are 5 minutes apart, but the time between them was purged: exactly two work sessions, one on
    // each side of it (no duplicates from the out-of-order parts).
    std::vector<WorkSessionInfo> sessions = f.hist.GetSessions(0, 0, 10);
    REQUIRE(sessions.size() == 2);
    CHECK(sessions[1].endTs < last5.startUtc);                            // newest first
    CHECK_EQ(sessions[1].startTs, (now - 25 * 60 * 1000) / 1000);
    CHECK(sessions[0].startTs >= now / 1000);
}

TEST(Store, BrowseRowsKeepTheirWindowForDwellAttribution)
{
    Fixture f;
    REQUIRE(f.open);
    f.store.RecordBrowse(WebEv(L"https://a.example/1", L"a.example", L"First", 4242, 0));
    f.store.RecordBrowse(WebEv(L"https://a.example/2", L"a.example", L"Second", 4242, 95));
    REQUIRE(f.Barrier());
    std::vector<BrowsingActivity> rows = f.store.QueryBrowsing(0, wt::NowUtc() + 60);
    REQUIRE(rows.size() == 2);
    for (const BrowsingActivity& b : rows) CHECK_EQ(b.windowId, (int64_t)4242);
    CHECK_EQ(rows[0].dwellSecs, 95);                   // newest first: the row that closed "First"
}

TEST(Store, MaintenanceCheckpointEmptiesTheWal)
{
    Fixture f;
    REQUIRE(f.open);
    const std::wstring p = L"C:\\WarpTest\\wal\\notes.txt";
    f.store.RecordFile(FileEv(L"MODIFY", p));
    REQUIRE(f.Barrier());
    f.store.RecordFile(FileEv(L"OPEN", p, L"", L"recent"));   // known entity: cached lookups stop on a row
    REQUIRE(f.Barrier());
    REQUIRE(f.store.ScheduleMaintenance());
    REQUIRE(f.Barrier());
    // A statement left open on the writer connection makes wal_checkpoint(TRUNCATE) fail (WAL grows forever).
    CHECK_EQ(f.store.GetStats().walBytes, (int64_t)0);
}

TEST(Store, TopRanksSustainedEngagementAboveASingleTouch)
{
    Fixture f;
    REQUIRE(f.open);
    const int64_t now = wt::NowMs();
    const std::wstring busy = L"C:\\WarpTest\\rank\\busy.xlsx";
    const std::wstring once = L"C:\\WarpTest\\rank\\once.xlsx";
    for (int i = 0; i < 5; ++i)
        f.store.RecordFile(FileEv(L"MODIFY", busy, L"", L"etw", 1.0, now - (5 - i) * 10 * 60 * 1000LL));
    f.store.RecordFile(FileEv(L"OPEN", once, L"", L"etw", 0.5, now - 60 * 1000));
    REQUIRE(f.Barrier());
    std::vector<EntityScore> top = f.ents.Top("file", 10);
    REQUIRE(top.size() == 2);
    CHECK_EQ(top[0].rec.entityKey, EntityKey::FileKey(busy));
    CHECK(top[0].prior > top[1].prior);
    CHECK(!top[0].reasons.empty());
    std::vector<EntityScore> sc = f.ents.Score({ EntityKey::FileKey(busy), "c:\\warptest\\rank\\missing.txt" });
    REQUIRE(sc.size() == 2);
    CHECK(sc[0].found);
    CHECK(!sc[1].found);
    CHECK(sc[0].score > 0.0 && sc[0].score <= 1.0);
    // RANK-1: scores are evaluated at read time - the same entity decays when asked about later.
    std::vector<EntityScore> later = f.ents.Score({ EntityKey::FileKey(busy) }, "short", wt::NowUtc() + 14 * 86400);
    REQUIRE(later.size() == 1);
    CHECK(later[0].shortTerm < sc[0].shortTerm);
}

TEST(Store, SearchFindsEntitiesByWordsAndPrefixes)
{
    Fixture f;
    REQUIRE(f.open);
    f.store.RecordFile(FileEv(L"MODIFY", L"C:\\WarpTest\\Finance\\Quarterly Budget Plan.xlsx"));
    f.store.RecordFile(FileEv(L"MODIFY", L"C:\\WarpTest\\Finance\\Travel.xlsx"));
    REQUIRE(f.Barrier());
    std::vector<EntityScore> hits = f.ents.Search(L"budget", "", 10);
    REQUIRE(!hits.empty());
    CHECK(hits[0].rec.displayName.find(L"Quarterly Budget Plan") != std::wstring::npos);
    CHECK(!f.ents.Search(L"quart", "file", 10).empty());               // prefix match on the last token
    CHECK(f.ents.Search(L"budget travel", "", 10).empty());            // every token must match one entity
    CHECK(f.ents.Search(L"\"budget\" OR travel*", "", 10).size() <= 2); // FTS syntax is neutralized, no error
    CHECK(f.ents.Search(L"   ", "", 10).empty());
}

TEST(Store, ConcurrentWritersAndReadersNeverDeadlock)
{
    Fixture f;
    REQUIRE(f.open);
    std::atomic<bool> stop{ false };
    std::atomic<int> reads{ 0 };
    std::vector<std::thread> writers, readers;
    for (int t = 0; t < 6; ++t)
        writers.emplace_back([&f, t]
        {
            for (int i = 0; i < 400; ++i)
            {
                std::wstring p = L"C:\\WarpTest\\stress\\t" + std::to_wstring(t) + L"\\f" + std::to_wstring(i % 50) + L".txt";
                f.store.RecordFile(FileEv((i % 3) ? L"OPEN" : L"MODIFY", p, L"", L"etw", 0.9));
                if (i % 40 == 0) f.store.RecordBrowse(WebEv(L"https://stress.example/" + std::to_wstring(i), L"stress.example", L"Stress page"));
            }
        });
    for (int r = 0; r < 4; ++r)
        readers.emplace_back([&f, &stop, &reads]
        {
            while (!stop.load())
            {
                f.ents.Top("file", 20);
                f.ents.Search(L"f1", "", 10);
                f.ents.GetChanges(0, 200);
                f.store.QueryFiles(0, wt::NowUtc() + 60, 100);
                f.hist.GetThreads(0, 0, 10, true);
                ++reads;
            }
        });
    for (std::thread& w : writers) w.join();
    REQUIRE(f.Barrier());
    stop = true;
    for (std::thread& r : readers) r.join();
    CHECK(reads.load() > 0);
    ActivityStore::Stats st = f.store.GetStats();
    CHECK_EQ(st.dropped, (uint64_t)0);
    CHECK_EQ(st.writeErrors, (uint64_t)0);
    CHECK_EQ(f.ents.Top("file", 1000).size(), (size_t)300);   // 6 writers x 50 distinct files
}

TEST(Store, MigratesALegacyV1Database)
{
    wt::TempDir dir;
    const std::wstring path = dir.File(L"legacy.db");
    const std::string ts = std::to_string(wt::NowUtc() - 3600);
    {
        sqlite3* db = nullptr;
        REQUIRE(sqlite3_open16(path.c_str(), &db) == SQLITE_OK);
        const std::string ctx = ", source_pid INTEGER, source_exe TEXT, foreground_pid INTEGER, foreground_exe TEXT, ms_since_input INTEGER, "
                                "parent_pid INTEGER, parent_exe TEXT, created_window_ms INTEGER, confidence REAL DEFAULT 1.0";
        const std::vector<std::string> sql = {
            "CREATE TABLE file_activity (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL, action TEXT NOT NULL, path TEXT NOT NULL, old_path TEXT" + ctx + ");",
            "CREATE TABLE app_launch_activity (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL, exe_name TEXT NOT NULL, exe_path TEXT NOT NULL, pid INTEGER NOT NULL" + ctx + ");",
            "CREATE TABLE browsing_activity (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL, browser TEXT NOT NULL, title TEXT NOT NULL, url TEXT" + ctx + ");",
            "CREATE TABLE app_focus_activity (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp INTEGER NOT NULL, exe_name TEXT NOT NULL, exe_path TEXT NOT NULL, window_title TEXT NOT NULL, duration_secs INTEGER NOT NULL" + ctx + ");",
            "CREATE TABLE inference (entity_key TEXT PRIMARY KEY, entity_type TEXT, last_event_ts INTEGER, last_open_ts INTEGER, last_edit_ts INTEGER, "
            "open_count_7d INTEGER, open_count_30d INTEGER, open_count_total INTEGER, recency_score REAL, version INTEGER, updated_at INTEGER);",
            "INSERT INTO file_activity (timestamp, action, path, confidence) VALUES (" + ts + ", 'MODIFY', 'C:\\Legacy\\Plan.DOCX', 0.9);",
            "INSERT INTO browsing_activity (timestamp, browser, title, url, confidence) VALUES (" + ts +
            ", 'msedge.exe', 'Sign in', 'https://login.microsoftonline.com/common/oauth2/authorize?code=SECRET', 1.0);",
            "INSERT INTO browsing_activity (timestamp, browser, title, url, confidence) VALUES (" + ts +
            ", 'msedge.exe', 'News', 'https://news.example/story?utm_source=x', 1.0);",
            "INSERT INTO app_focus_activity (timestamp, exe_name, exe_path, window_title, duration_secs) VALUES (" + ts +
            ", 'WINWORD.EXE', 'C:\\Office\\WINWORD.EXE', 'Plan - Word', 120);",
            "INSERT INTO inference VALUES ('C:\\Legacy\\Plan.DOCX', 'file', " + ts + ", 0, " + ts + ", 0, 0, 0, 100.0, 1, " + ts + ");",
            "INSERT INTO inference VALUES ('https://login.microsoftonline.com/common/oauth2/authorize?code=SECRET', 'url', " + ts + ", " + ts +
            ", 0, 1, 1, 1, 50.0, 2, " + ts + ");",
        };
        for (const std::string& s : sql) REQUIRE(sqlite3_exec(db, s.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }

    ActivityStore store;
    EntityStore ents(&store);
    HistoryEngine hist(&store);
    store.AddSink(&ents);
    store.AddSink(&hist);
    REQUIRE(store.Open(path));
    CHECK_EQ(store.GetStats().schemaVersion, 4);

    EntityRecord plan = ents.Lookup({ EntityKey::FileKey(L"C:\\Legacy\\Plan.DOCX") })[0];
    REQUIRE(!plan.entityKey.empty());
    CHECK_EQ(plan.displayName, std::wstring(L"C:\\Legacy\\Plan.DOCX"));
    CHECK_NEAR(plan.editCountTotal, 0.9, 1e-6);
    CHECK(plan.engS > 0.0);
    CHECK_EQ(ents.Top("file", 10).size(), (size_t)1);   // re-keyed, not duplicated

    std::vector<BrowsingActivity> web = store.QueryBrowsing(0, wt::NowUtc() + 60);
    REQUIRE(web.size() == 1);                           // the sign-in URL stored by old builds is gone
    CHECK(web[0].url.find(L"utm_source") == std::wstring::npos);
    CHECK(ents.Top("url", 10).empty());

    std::vector<AppFocusActivity> focus = store.QueryFocus(0, wt::NowUtc() + 60);
    REQUIRE(focus.size() == 1);
    CHECK_EQ(focus[0].activeSecs, -1);
    CHECK_EQ(focus[0].unknownSecs, 120);
    CHECK_EQ(focus[0].endTs - focus[0].startTs, (int64_t)120);
    store.Close();
}

TEST(History, SnapshotsWithTheSameEntitiesShareAThread)
{
    Fixture f;
    REQUIRE(f.open);
    const std::wstring a = L"C:\\WarpTest\\proj\\spec.docx", b = L"C:\\WarpTest\\proj\\model.xlsx";
    const std::wstring x = L"C:\\WarpTest\\other\\tax.pdf", y = L"C:\\WarpTest\\other\\receipt.pdf";
    for (const std::wstring& p : { a, b, x, y }) f.store.RecordFile(FileEv(L"MODIFY", p));
    REQUIRE(f.Barrier());

    auto snap = [](int64_t ts, const std::vector<std::pair<std::wstring, double>>& items)
    {
        SnapshotInput in;
        in.ts = ts;
        in.windowSecs = 900;
        in.label = "test";
        for (const auto& it : items)
        {
            SnapshotEntity e;
            e.entityKey = EntityKey::FileKey(it.first);
            e.entityType = "file";
            e.display = it.first;
            e.activeSecs = it.second;
            in.entities.push_back(e);
        }
        return in;
    };
    const int64_t t = wt::NowUtc() - 600;
    const int64_t t1 = f.hist.RecordSnapshot(snap(t, { { a, 300.0 }, { b, 200.0 } }));
    const int64_t t2 = f.hist.RecordSnapshot(snap(t + 60, { { a, 250.0 }, { b, 250.0 } }));
    const int64_t t3 = f.hist.RecordSnapshot(snap(t + 120, { { x, 300.0 }, { y, 300.0 } }));
    CHECK(t1 != 0);
    CHECK_EQ(t2, t1);
    CHECK(t3 != 0 && t3 != t1);
    CHECK_EQ(f.hist.CurrentThreadId(), t3);
    CHECK_EQ(f.hist.GetThreads(0, 0, 10, true).size(), (size_t)2);

    std::vector<RelatedEntity> rel = f.hist.Related(EntityKey::FileKey(a), 10);
    REQUIRE(!rel.empty());
    CHECK_EQ(rel[0].entityKey, EntityKey::FileKey(b));

    // Returning to the first task resumes its thread instead of starting a third one.
    const int64_t t4 = f.hist.RecordSnapshot(snap(t + 180, { { a, 300.0 }, { b, 100.0 } }));
    CHECK_EQ(t4, t1);
    std::vector<double> boost = f.hist.ContextBoost({ EntityKey::FileKey(a), EntityKey::FileKey(x) });
    REQUIRE(boost.size() == 2);
    CHECK(boost[0] > boost[1]);
}

TEST(History, ProjectKeysAnchorOnRepositoriesAndSites)
{
    wt::TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(dir.Path() + L"\\repo\\.git", ec);
    std::filesystem::create_directories(dir.Path() + L"\\repo\\src", ec);
    CHECK_EQ(HistoryEngine::ProjectKeyFor(dir.Path() + L"\\repo\\src\\main.cpp", "file"),
             "git:" + EntityKey::Key(dir.Path() + L"\\repo"));
    CHECK_EQ(HistoryEngine::ProjectKeyFor(L"https://github.com/Org/Repo/pull/12", "url"), std::string("repo:github.com/org/repo"));
    CHECK_EQ(HistoryEngine::ProjectKeyFor(L"https://contoso.visualstudio.com/Fabrikam/_git/Web/pullrequest/7", "url"),
             std::string("repo:contoso.visualstudio.com/fabrikam/web"));
    CHECK_EQ(HistoryEngine::ProjectKeyFor(L"https://contoso.sharepoint.com/sites/Finance/Shared Documents/x.xlsx", "url"),
             std::string("sp:contoso.sharepoint.com/sites/finance"));
    CHECK(HistoryEngine::ProjectKeyFor(L"https://news.example/story", "url").empty());
}

TEST(History, WorkSessionsSplitOnLongAbsences)
{
    Fixture f;
    REQUIRE(f.open);
    const int64_t base = (wt::NowUtc() - 6 * 3600) * 1000;
    auto presence = [&f](int64_t startMs, int64_t endMs, const char* state)
    {
        PresenceRecord r;
        r.startMs = startMs;
        r.endMs = endMs;
        r.state = state;
        f.store.RecordPresence(std::move(r));
    };
    presence(base, base + 30 * 60 * 1000, "active");
    presence(base + 30 * 60 * 1000, base + 35 * 60 * 1000, "away");                 // short break: same session
    presence(base + 35 * 60 * 1000, base + 60 * 60 * 1000, "active");
    presence(base + 60 * 60 * 1000, base + 120 * 60 * 1000, "locked");               // long absence: new session
    presence(base + 120 * 60 * 1000, base + 150 * 60 * 1000, "active");
    REQUIRE(f.Barrier());
    std::vector<WorkSessionInfo> sessions = f.hist.GetSessions(0, 0, 10);
    REQUIRE(sessions.size() == 2);
    CHECK_EQ(sessions[1].startTs, base / 1000);                                      // newest first
    CHECK_EQ(sessions[1].endTs, (base + 60 * 60 * 1000) / 1000);
    CHECK_EQ(sessions[0].startTs, (base + 120 * 60 * 1000) / 1000);
}
