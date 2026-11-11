// Identity / privacy canonicalizers (gap analysis RANK-4, WEB-2/3, FILE-1/2/3, APP-5, PRIV-3):
// URL canonicalization and document identities, entity keys shared by C++ and SQL, window-title
// grammars, and FileCapture's pure path rules.
#include "WarpTests.h"

#include "DocResolver.h"
#include "EntityKey.h"
#include "EntityStore.h"
#include "FileCapture.h"
#include "FocusTracker.h"
#include "PrintMonitor.h"
#include "UrlCanonicalizer.h"
#include "sqlite3.h"

#include <shlobj.h>

namespace
{
    std::wstring KnownFolder(REFKNOWNFOLDERID id)
    {
        PWSTR p = nullptr;
        std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) && p) out = p;
        if (p) CoTaskMemFree(p);
        return out;
    }

    bool Has(const std::wstring& s, const wchar_t* part) { return s.find(part) != std::wstring::npos; }
}

// ---------------------------------------------------------------- UrlCanon
TEST(UrlCanon, StripsCredentialsFragmentAndTrackingQuery)
{
    // Built from pieces: tools and secret scanners redact a literal "scheme://user:pass@host" URL.
    const std::wstring userInfo = std::wstring(L"bob") + L":" + L"secret" + L"@";
    UrlCanon::Result r = UrlCanon::Canonicalize(std::wstring(L"https:") + L"//" + userInfo + L"Example.COM/a/b?utm_source=mail&q=1#frag");
    CHECK(!r.excluded);
    CHECK_EQ(r.host, std::wstring(L"example.com"));
    CHECK_EQ(r.scheme, std::wstring(L"https"));
    CHECK(r.hadQuery);
    CHECK(!Has(r.canonical, L"bob"));
    CHECK(!Has(r.canonical, L"secret"));
    CHECK(!Has(r.canonical, L"#"));
    CHECK(!Has(r.canonical, L"utm_source"));
    CHECK(Has(r.canonical, L"example.com/a/b"));
}

TEST(UrlCanon, RecognizesDocumentIdentities)
{
    UrlCanon::Result yt = UrlCanon::Canonicalize(L"https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=42s");
    CHECK_EQ(yt.docKind, std::wstring(L"youtube"));
    CHECK_EQ(yt.docId, std::wstring(L"yt:dQw4w9WgXcQ"));
    CHECK_EQ(yt.canonical, std::wstring(L"https://www.youtube.com/watch?v=dQw4w9WgXcQ"));

    UrlCanon::Result gd = UrlCanon::Canonicalize(
        L"https://docs.google.com/document/d/1AbCdEfGhIjKlMnOpQrStUvWxYz0123456789/edit#heading=h.1");
    CHECK_EQ(gd.docKind, std::wstring(L"google-docs"));
    CHECK_EQ(gd.docId, std::wstring(L"gdoc:1AbCdEfGhIjKlMnOpQrStUvWxYz0123456789"));

    UrlCanon::Result pr = UrlCanon::Canonicalize(L"https://github.com/Microsoft/WARP/pull/42/files");
    CHECK_EQ(pr.docKind, std::wstring(L"github-pr"));
    CHECK_EQ(pr.docId, std::wstring(L"gh:microsoft/warp#pr42"));

    // Same document through different URLs => same identity.
    UrlCanon::Result yt2 = UrlCanon::Canonicalize(L"https://youtu.be/dQw4w9WgXcQ?si=abc");
    CHECK_EQ(yt2.docId, yt.docId);
}

TEST(UrlCanon, ExcludesSignInAndUserListedDomains)
{
    UrlCanon::Result a = UrlCanon::Canonicalize(L"https://login.microsoftonline.com/common/oauth2/authorize?client_id=x&code=secret");
    CHECK(a.excluded);
    CHECK(a.canonical.empty());
    UrlCanon::Result b = UrlCanon::Canonicalize(L"https://wiki.contoso.com/page", { L"contoso.com" });
    CHECK(b.excluded);
    UrlCanon::Result c = UrlCanon::Canonicalize(L"https://notcontoso.com/page", { L"contoso.com" });
    CHECK(!c.excluded);
}

TEST(UrlCanon, NeverKeepsSensitiveQueryKeys)
{
    UrlCanon::Result r = UrlCanon::Canonicalize(L"https://example.com/cb?access_token=abc123&state=xyz789&page=2",
                                                {}, { L"access_token", L"state", L"page" });
    CHECK(!Has(r.canonical, L"abc123"));
    CHECK(!Has(r.canonical, L"xyz789"));
    CHECK(Has(r.canonical, L"page=2"));
}

TEST(UrlCanon, RedactsTokenLikePathSegments)
{
    UrlCanon::Result r = UrlCanon::Canonicalize(L"https://example.com/reset/8f14e45fceea167a5a36dedd4bea2543c9d1f0b7a1e2");
    CHECK(!Has(r.canonical, L"8f14e45f"));
    CHECK(Has(r.canonical, L":token"));
}

TEST(UrlCanon, ClassifiesInternalAndNonUrls)
{
    CHECK(UrlCanon::Canonicalize(L"about:blank").isInternal);
    CHECK(UrlCanon::Canonicalize(L"edge://settings/privacy").isInternal);
    CHECK(UrlCanon::Canonicalize(L"how to bake bread").canonical.empty());
    CHECK(!UrlCanon::LooksLikeUrl(L"how to bake bread"));
    CHECK(UrlCanon::LooksLikeUrl(L"example.com/a"));
    CHECK(UrlCanon::LooksLikeUrl(L"https://example.com"));
    CHECK_EQ(UrlCanon::HostOf(L"https://User@Sub.Example.com:8443/x"), std::wstring(L"sub.example.com"));
    CHECK_EQ(UrlCanon::PercentDecode(L"a%20b%C3%A9"), std::wstring(L"a b\x00E9"));
}

// ---------------------------------------------------------------- EntityKey
TEST(EntityKey, CanonicalizesWindowsPaths)
{
    CHECK_EQ(EntityKey::CanonicalPath(L"\\\\?\\C:\\a\\.\\b\\..\\c.txt"), std::wstring(L"C:\\a\\c.txt"));
    CHECK_EQ(EntityKey::CanonicalPath(L"C:/a//b/"), std::wstring(L"C:\\a\\b"));
    CHECK_EQ(EntityKey::CanonicalPath(L"\\\\?\\UNC\\srv\\share\\x\\y.docx"), std::wstring(L"\\\\srv\\share\\x\\y.docx"));
    CHECK_EQ(EntityKey::CanonicalPath(L"C:\\a\\b.pdf:Zone.Identifier"), std::wstring(L"C:\\a\\b.pdf"));
    CHECK_EQ(EntityKey::CanonicalPath(L"C:\\"), std::wstring(L"C:\\"));
    CHECK_EQ(EntityKey::CanonicalPath(L"C:\\a\\..\\.."), std::wstring(L"C:\\"));
    CHECK_EQ(EntityKey::CanonicalPath(L"\\\\srv\\share\\..\\x"), std::wstring(L"\\\\srv\\share\\x"));
}

TEST(EntityKey, KeysAreCaseFoldedUtf8)
{
    CHECK_EQ(EntityKey::FileKey(L"C:\\Users\\X\\Doc.TXT"), std::string("c:\\users\\x\\doc.txt"));
    CHECK_EQ(EntityKey::FileKey(L"c:\\A\\b.txt"), EntityKey::FileKey(L"\\\\?\\C:\\a\\B.TXT"));
    CHECK_EQ(EntityKey::Key(std::wstring(L"\x00C4") + L"BC"), std::string("\xC3\xA4") + "bc");
    CHECK_EQ(EntityKey::AppKey(L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App"),
             std::string("aumid:microsoft.windowsnotepad_8wekyb3d8bbwe!app"));
    CHECK_EQ(EntityKey::AppKey(L"C:\\Windows\\Notepad.exe"), std::string("c:\\windows\\notepad.exe"));
    CHECK_EQ(EntityKey::BaseName(L"C:\\a\\Report.docx"), std::wstring(L"Report.docx"));
    CHECK_EQ(EntityKey::FromUtf8(EntityKey::ToUtf8(L"\x00E9t\x00E9")), std::wstring(L"\x00E9t\x00E9"));
}

TEST(EntityKey, SqlFunctionsAgreeWithCpp)
{
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open(":memory:", &db) == SQLITE_OK);
    EntityKey::RegisterSqliteFunctions(db);
    sqlite3_stmt* st = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT warp_file_key(?1), warp_key(?1), warp_key(NULL);", -1, &st, nullptr) == SQLITE_OK);
    const std::string in = "C:\\Data\\..\\Docs\\R\xC3\x89SUM\xC3\x89.DOCX";
    sqlite3_bind_text(st, 1, in.c_str(), -1, SQLITE_TRANSIENT);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW);
    const char* fk = (const char*)sqlite3_column_text(st, 0);
    const char* k = (const char*)sqlite3_column_text(st, 1);
    CHECK_EQ(std::string(fk ? fk : ""), EntityKey::FileKey(EntityKey::FromUtf8(in)));
    CHECK_EQ(std::string(k ? k : ""), EntityKey::Key(EntityKey::FromUtf8(in)));
    CHECK(sqlite3_column_type(st, 2) == SQLITE_NULL);
    CHECK_EQ(std::string(fk ? fk : ""), std::string("c:\\docs\\r\xC3\xA9sum\xC3\xA9.docx"));
    sqlite3_finalize(st);
    sqlite3_close(db);
}

TEST(EntityKey, ApiKeysRoundTrip)
{
    // Whatever WARP hands out as an entity key must normalize to itself (API ScoreEntities/Purge).
    const std::string app = EntityKey::AppKey(L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App");
    CHECK_EQ(EntityStore::NormalizeKey(L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App"), app);
    CHECK_EQ(EntityStore::NormalizeKey(EntityKey::FromUtf8(app)), app);
    const std::string yt = EntityStore::NormalizeKey(L"https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    CHECK_EQ(yt, std::string("yt:dQw4w9WgXcQ"));
    CHECK_EQ(EntityStore::NormalizeKey(EntityKey::FromUtf8(yt)), yt);
    const std::string pr = EntityStore::NormalizeKey(L"https://github.com/Microsoft/WARP/pull/42");
    CHECK_EQ(EntityStore::NormalizeKey(EntityKey::FromUtf8(pr)), pr);
    CHECK_EQ(EntityStore::NormalizeKey(L"C:\\Data\\..\\Docs\\X.TXT"), std::string("c:\\docs\\x.txt"));
    CHECK_EQ(EntityStore::NormalizeKey(L"\\\\srv\\share\\Plan.docx"), std::string("\\\\srv\\share\\plan.docx"));
    CHECK(EntityStore::NormalizeKey(L"https://login.microsoftonline.com/common/oauth2/authorize").empty());
    CHECK(EntityStore::NormalizeKey(L"").empty());
}

// ---------------------------------------------------------------- DocResolver
TEST(DocResolver, NormalizesTitles)
{
    CHECK_EQ(DocResolver::NormalizeTitle(L"(3) Inbox - Outlook"), std::wstring(L"Inbox - Outlook"));
    CHECK_EQ(DocResolver::NormalizeTitle(L"*notes.txt - Notepad"), std::wstring(L"notes.txt - Notepad"));
    CHECK_EQ(DocResolver::NormalizeTitle(L"\x25CF auth.cpp - repo - Visual Studio Code"),
             std::wstring(L"auth.cpp - repo - Visual Studio Code"));
    CHECK_EQ(DocResolver::NormalizeTitle(L"Report *  -   Word\x200B"), std::wstring(L"Report - Word"));
    CHECK_EQ(DocResolver::NormalizeTitle(L"[99+] Chat \x202E" L"x"), std::wstring(L"Chat x"));
}

TEST(DocResolver, ExtractsDocumentNamesPerAppGrammar)
{
    CHECK_EQ(DocResolver::DocNameFromTitle(L"winword.exe", L"Report - Word"), std::wstring(L"Report"));
    CHECK_EQ(DocResolver::DocNameFromTitle(L"code.exe", L"auth.cpp - repo - Visual Studio Code"), std::wstring(L"auth.cpp"));
    CHECK_EQ(DocResolver::DocNameFromTitle(L"notepad.exe", DocResolver::NormalizeTitle(L"*notes.txt - Notepad")),
             std::wstring(L"notes.txt"));
    CHECK_EQ(DocResolver::DocNameFromTitle(L"excel.exe", L"Budget [Compatibility Mode] - Excel"), std::wstring(L"Budget"));
    CHECK(DocResolver::DocNameFromTitle(L"winword.exe", L"Word").empty());
    CHECK_EQ(DocResolver::DocNameFromTitle(L"someviewer.exe", L"Viewer - plan.pdf"), std::wstring(L"plan.pdf"));
    CHECK(DocResolver::DocNameFromTitle(L"someapp.exe", L"Settings - Some App").empty());
    CHECK(DocResolver::IsDocumentApp(L"winword.exe"));
    CHECK(!DocResolver::IsDocumentApp(L"explorer.exe"));
}

TEST(DocResolver, AbstainsOnAmbiguousNameOrStem)
{
    std::wstring out;
    const std::vector<std::wstring> c = { L"C:\\new\\Report.docx", L"C:\\old\\Report.docx", L"C:\\x\\notes.txt" };
    bool ambiguous = false;
    CHECK(!DocResolver::MatchPath(L"Report", c, out, &ambiguous));
    CHECK(ambiguous);
    CHECK(out.empty());
    CHECK(DocResolver::MatchPath(L"NOTES.TXT", c, out));
    CHECK_EQ(out, std::wstring(L"C:\\x\\notes.txt"));
    CHECK(!DocResolver::MatchPath(L"notes.md", c, out));
    CHECK(!DocResolver::MatchPath(L"", c, out));
    CHECK(DocResolver::MatchPath(L"Report", { L"C:\\new\\Report.docx", L"c:\\NEW\\REPORT.DOCX" }, out, &ambiguous));
    CHECK(!ambiguous);
    CHECK(!DocResolver::MatchPath(L"report", { L"C:\\x\\report.docx", L"C:\\x\\report.pdf" }, out, &ambiguous));
    CHECK(ambiguous);
}

TEST(EntityKey, PreservesCaseSensitiveWebIdentity)
{
    CHECK(EntityKey::UrlKey(L"https://example.org/PlanA") != EntityKey::UrlKey(L"https://example.org/plana"));
    CHECK_EQ(EntityKey::UrlKey(L"HTTPS://EXAMPLE.ORG/PlanA?q=AbC"), std::string("https://example.org/PlanA?q=AbC"));
    CHECK(EntityKey::DocKey(L"gdoc:AbC123") != EntityKey::DocKey(L"gdoc:abc123"));
    CHECK_EQ(EntityStore::NormalizeKey(L"GDOC:AbC123"), std::string("gdoc:AbC123"));
    CHECK_EQ(EntityStore::NormalizeKey(L"https://docs.google.com/document/d/1AbCdEfGhIjKlMnOpQrStUvWxYz0123456789/edit"),
             std::string("gdoc:1AbCdEfGhIjKlMnOpQrStUvWxYz0123456789"));
}

TEST(Focus, DistinguishesPassiveIdleAndUnknownInput)
{
    CHECK_EQ(FocusTracker::AttentionState("active", 0), std::string("active"));
    CHECK_EQ(FocusTracker::AttentionState("active", 180000), std::string("idle"));
    CHECK_EQ(FocusTracker::AttentionState("passive", 180000), std::string("passive"));
    CHECK_EQ(FocusTracker::AttentionState("passive", 100), std::string("active"));
    CHECK_EQ(FocusTracker::AttentionState("active", 0xFFFFFFFFu), std::string("unknown"));
    CHECK_EQ(FocusTracker::AttentionState("locked", 100), std::string("idle"));
}

TEST(Print, RequiresQualifiedOwnUserAndLocalClient)
{
    const std::vector<std::wstring> own{ L"CONTOSO\\Alice", L"alice@contoso.example", L"S-1-5-21-1-1001" };
    const std::vector<std::wstring> local{ L"WORKPC", L"workpc.contoso.example" };
    CHECK(PrintMonitor::IsOwnLocalJob(L"contoso\\alice", L"\\\\workpc", own, local));
    CHECK(PrintMonitor::IsOwnLocalJob(L"alice@contoso.example", L"workpc.contoso.example", own, local));
    CHECK(!PrintMonitor::IsOwnLocalJob(L"CONTOSO\\Bob", L"WORKPC", own, local));
    CHECK(!PrintMonitor::IsOwnLocalJob(L"OTHER\\Alice", L"WORKPC", own, local));
    CHECK(!PrintMonitor::IsOwnLocalJob(L"Alice", L"WORKPC", own, local));
    CHECK(!PrintMonitor::IsOwnLocalJob(L"", L"WORKPC", own, local));
    CHECK(!PrintMonitor::IsOwnLocalJob(L"CONTOSO\\Alice", L"", own, local));
    CHECK(!PrintMonitor::IsOwnLocalJob(L"CONTOSO\\Alice", L"OTHERPC", own, local));
}
// ---------------------------------------------------------------- FileCapture path rules
TEST(FileRules, RecognizesTransientAndDownloadNames)
{
    CHECK(FileCapture::IsTransientName(L"~$Report.docx"));
    CHECK(FileCapture::IsTransientName(L"C:\\d\\~WRL0001.tmp"));
    CHECK(FileCapture::IsTransientName(L"report.docx.crdownload"));
    CHECK(FileCapture::IsTransientName(L"C:\\d\\4A3F1B2C"));
    CHECK(FileCapture::IsTransientName(L".~lock.budget.xlsx#"));
    CHECK(!FileCapture::IsTransientName(L"C:\\d\\Report.docx"));
    CHECK(!FileCapture::IsTransientName(L"C:\\d\\Makefile"));
    CHECK(FileCapture::IsDownloadTempName(L"setup.exe.crdownload"));
    CHECK(!FileCapture::IsDownloadTempName(L"setup.exe"));
    CHECK(FileCapture::IsDownloadRename(L"C:\\Dl\\Unconfirmed 123.crdownload", L"C:\\Dl\\setup.exe"));
    CHECK(FileCapture::IsDownloadRename(L"C:\\Dl\\setup.exe.part", L"C:\\Dl\\setup.exe"));
    CHECK(!FileCapture::IsDownloadRename(L"C:\\d\\a.txt", L"C:\\d\\b.txt"));
    CHECK(FileCapture::IsRecycleBinPath(L"C:\\$Recycle.Bin\\S-1-5-21-1\\$R123.docx"));
    CHECK(!FileCapture::IsRecycleBinPath(L"C:\\Users\\x\\Recycle notes.txt"));
}

TEST(FileRules, ExcludesSystemLocationsButNotUserDocuments)
{
    const std::wstring win = KnownFolder(FOLDERID_Windows);
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    const std::wstring docs = KnownFolder(FOLDERID_Documents);
    REQUIRE(!win.empty() && !local.empty() && !docs.empty());

    CHECK(FileCapture::IsExcludedPath(win + L"\\System32\\drivers\\etc\\hosts"));
    CHECK(FileCapture::IsExcludedPath(local + L"\\Temp\\scratch.txt"));
    CHECK(FileCapture::IsExcludedPath(docs + L"\\repo\\.git\\index"));
    CHECK(FileCapture::IsExcludedPath(docs + L"\\repo\\node_modules\\x\\index.js"));
    CHECK(!FileCapture::IsExcludedPath(docs + L"\\Plan.docx"));
    CHECK(FileCapture::ClassifyPath(docs + L"\\Plan.docx") == FileCapture::PathClass::User);
    CHECK(FileCapture::ClassifyPath(docs + L"\\~$Plan.docx") == FileCapture::PathClass::Transient);
    CHECK(FileCapture::ClassifyPath(win + L"\\notepad.exe") == FileCapture::PathClass::Excluded);
}

TEST(FileRules, DecidesRenamesOnTheDestination)
{
    using PC = FileCapture::PathClass;
    using RO = FileCapture::RenameOutcome;
    CHECK(FileCapture::DecideRename(PC::Transient, PC::User, false, false) == RO::ModifyDest);    // atomic save
    CHECK(FileCapture::DecideRename(PC::Transient, PC::User, false, true) == RO::DownloadDest);   // download completes
    CHECK(FileCapture::DecideRename(PC::User, PC::Transient, false, false) == RO::Drop);         // safe-save step
    CHECK(FileCapture::DecideRename(PC::User, PC::User, false, false) == RO::Rename);
    CHECK(FileCapture::DecideRename(PC::User, PC::Excluded, false, false) == RO::DeleteSource);
    CHECK(FileCapture::DecideRename(PC::User, PC::Excluded, true, false) == RO::DeleteSource);   // moved to the Recycle Bin
    CHECK(FileCapture::DecideRename(PC::Excluded, PC::User, false, false) == RO::CreateDest);
    CHECK(FileCapture::DecideRename(PC::Excluded, PC::Excluded, false, false) == RO::Drop);
    CHECK(FileCapture::DecideRename(PC::Transient, PC::Transient, false, false) == RO::Drop);
}
