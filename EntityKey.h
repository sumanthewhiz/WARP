// ---------------------------------------------------------------------------
// EntityKey
//
// One canonicalization of entity identity (files, apps, URLs, documents)
// shared by the C++ pipeline and by SQL (through registered SQLite
// functions), so keys computed in either place always agree.
// ---------------------------------------------------------------------------

#pragma once

#include <string>

struct sqlite3;

namespace EntityKey
{
// Canonical display form of a Windows path (case PRESERVED):
//   - "\\?\C:\x" -> "C:\x"; "\\?\UNC\srv\share\x" -> "\\srv\share\x"; "\Device\Mup\srv\share\x" -> "\\srv\share\x"
//   - forward slashes -> backslashes; duplicate separators collapsed (leading "\\" of UNC kept)
//   - "." and ".." resolved lexically (never above the root/share)
//   - trailing separator stripped except for roots ("C:\", "\\srv\share\")
//   - NTFS alternate-data-stream suffix stripped from the last component ("C:\a\b.pdf:Zone.Identifier" -> "C:\a\b.pdf")
//   - components with '~' + digit (8.3 short names) expanded via GetLongPathNameW (best effort, local drives only)
//   - "\Device\HarddiskVolumeN\..." and other device-namespace paths are returned unchanged
std::wstring CanonicalPath(const std::wstring& path);

// Case-folded key of any identifier (path, canonical URL, AUMID, doc id): Unicode-correct lowercase via
// LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE), returned as UTF-8.
std::string Key(const std::wstring& s);

std::string FileKey(const std::wstring& path);            // Key(CanonicalPath(path))
std::string AppKey(const std::wstring& exePathOrAumid);   // path -> FileKey; otherwise "aumid:" + Key(aumid)
std::string UrlKey(const std::wstring& canonicalUrl);     // Key(canonicalUrl)
std::string DocKey(const std::wstring& docId);            // Key(docId), docId already prefixed ("gdoc:...")

// Last path component (display helper): "C:\a\Report.docx" -> "Report.docx". Roots are returned as-is.
std::wstring BaseName(const std::wstring& path);

// UTF conversions (CP_UTF8, explicit lengths, never fail: invalid sequences become U+FFFD).
std::string  ToUtf8(const std::wstring& w);
std::wstring FromUtf8(const std::string& s);

// Registers deterministic SQL functions so SQL and C++ agree on keys:
//   warp_key(text)      -> Key(text)
//   warp_file_key(text) -> FileKey(text)
// NULL in -> NULL out. Registration failures are reported via OutputDebugString.
void RegisterSqliteFunctions(sqlite3* db);
}
