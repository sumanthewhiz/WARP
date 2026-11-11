// EntityKey.cpp - see EntityKey.h.

#include "framework.h"
#include "EntityKey.h"
#include "sqlite3.h"

#include <cstdint>
#include <new>
#include <vector>

namespace
{
// Win32 string APIs take int lengths; longer inputs are processed in pieces of at most this many units.
constexpr size_t kMaxApiChunk = 0x10000000;

inline bool IsSep(wchar_t c)
{
    return c == L'\\' || c == L'/';
}

inline bool IsAsciiAlpha(wchar_t c)
{
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}

inline bool IsAsciiDigit(wchar_t c)
{
    return c >= L'0' && c <= L'9';
}

inline wchar_t AsciiLower(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}

inline bool IsHighSurrogateUnit(wchar_t c)
{
    return c >= 0xD800 && c <= 0xDBFF;
}

// ASCII case-insensitive prefix test at s[pos].
bool HasPrefixNoCase(const std::wstring& s, size_t pos, const wchar_t* prefix)
{
    for (size_t i = 0; prefix[i] != L'\0'; ++i)
    {
        if (pos + i >= s.size() || AsciiLower(s[pos + i]) != AsciiLower(prefix[i]))
            return false;
    }
    return true;
}

// Piece length for an int-length API call that never splits a surrogate pair.
size_t WideChunk(const wchar_t* p, size_t remaining)
{
    if (remaining <= kMaxApiChunk)
        return remaining;
    return IsHighSurrogateUnit(p[kMaxApiChunk - 1]) ? kMaxApiChunk - 1 : kMaxApiChunk;
}

// Piece length for an int-length API call that never splits a UTF-8 sequence.
size_t Utf8Chunk(const char* p, size_t remaining)
{
    if (remaining <= kMaxApiChunk)
        return remaining;
    size_t n = kMaxApiChunk;
    for (int back = 0; back < 3 && (static_cast<unsigned char>(p[n]) & 0xC0) == 0x80; ++back)
        --n;
    return n;
}

// Unicode-correct, locale-independent lowercase (never the user's locale, so keys are stable everywhere).
std::wstring LowerInvariant(const std::wstring& s)
{
    bool ascii = true;
    for (wchar_t c : s)
    {
        if (c >= 0x80)
        {
            ascii = false;
            break;
        }
    }

    std::wstring out;
    if (ascii)
    {
        // Fast path; identical to LCMAP_LOWERCASE for ASCII input.
        out.resize(s.size());
        for (size_t i = 0; i < s.size(); ++i)
            out[i] = AsciiLower(s[i]);
        return out;
    }

    out.reserve(s.size());
    const wchar_t* p = s.data();
    size_t remaining = s.size();
    while (remaining > 0)
    {
        const size_t n = WideChunk(p, remaining);
        const int cch = static_cast<int>(n);
        const size_t old = out.size();
        out.resize(old + n);
        int written = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, p, cch,
                                    &out[old], cch, nullptr, nullptr, 0);
        if (written <= 0)
        {
            const int needed = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, p, cch,
                                             nullptr, 0, nullptr, nullptr, 0);
            if (needed > 0)
            {
                out.resize(old + static_cast<size_t>(needed));
                written = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, p, cch,
                                        &out[old], needed, nullptr, nullptr, 0);
            }
        }

        if (written > 0)
        {
            out.resize(old + static_cast<size_t>(written));
        }
        else
        {
            // Not expected for the invariant locale; ASCII folding keeps the key deterministic.
            out.resize(old);
            for (size_t i = 0; i < n; ++i)
                out.push_back(AsciiLower(p[i]));
        }
        p += n;
        remaining -= n;
    }
    return out;
}

struct Span
{
    size_t pos;
    size_t len;
};

bool HasShortNameMarker(const std::wstring& s)
{
    for (size_t i = 0; i + 1 < s.size(); ++i)
    {
        if (s[i] == L'~' && IsAsciiDigit(s[i + 1]))
            return true;
    }
    return false;
}

// Best-effort 8.3 -> long name expansion of a canonical "X:\..." path; returns the input on any failure.
std::wstring ExpandShortNames(const std::wstring& path)
{
    if (path.size() < 3 || path.size() > 32767)
        return path;

    const wchar_t root[4] = { path[0], L':', L'\\', L'\0' };
    if (GetDriveTypeW(root) == DRIVE_REMOTE)
        return path;   // never block event processing on network I/O

    const bool extended = path.size() >= MAX_PATH;
    const std::wstring query = extended ? (L"\\\\?\\" + path) : path;

    std::wstring buf(query.size() + 64, L'\0');
    DWORD len = GetLongPathNameW(query.c_str(), &buf[0], static_cast<DWORD>(buf.size()));
    if (len >= buf.size())
    {
        // Buffer too small: len is the required size including the terminator.
        buf.assign(len, L'\0');
        len = GetLongPathNameW(query.c_str(), &buf[0], static_cast<DWORD>(buf.size()));
    }
    if (len == 0 || len >= buf.size())
        return path;

    buf.resize(len);
    if (extended && buf.compare(0, 4, L"\\\\?\\") == 0)
        buf.erase(0, 4);
    return buf;
}

std::wstring CanonicalPathImpl(const std::wstring& path)
{
    if (path.empty())
        return path;

    std::wstring s(path);
    for (wchar_t& c : s)
    {
        if (c == L'/')
            c = L'\\';
    }

    // Unwrap Win32 / NT namespace prefixes.
    size_t off = 0;
    bool unc = false;
    if (HasPrefixNoCase(s, 0, L"\\\\?\\") || HasPrefixNoCase(s, 0, L"\\\\.\\") || HasPrefixNoCase(s, 0, L"\\??\\"))
    {
        if (HasPrefixNoCase(s, 4, L"UNC\\"))
        {
            unc = true;
            off = 8;
        }
        else if (s.size() >= 6 && IsAsciiAlpha(s[4]) && s[5] == L':')
        {
            off = 4;
        }
        else
        {
            return path;   // volume GUID, pipe or other device-namespace path
        }
    }
    else if (HasPrefixNoCase(s, 0, L"\\Device\\Mup\\"))
    {
        unc = true;
        off = 12;
    }
    else if (HasPrefixNoCase(s, 0, L"\\Device\\"))
    {
        return path;   // kernel volume paths are mapped to drive letters by the caller
    }
    else if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\')
    {
        unc = true;
        off = 2;
    }

    // Root prefix.
    std::wstring out;
    size_t minParts = 0;   // UNC server and share can never be removed by ".."
    bool absolute = false;
    bool driveAbsolute = false;
    if (unc)
    {
        out = L"\\\\";
        absolute = true;
        minParts = 2;
    }
    else if (s.size() >= off + 2 && IsAsciiAlpha(s[off]) && s[off + 1] == L':')
    {
        out.assign(s, off, 2);
        off += 2;
        if (off < s.size() && s[off] == L'\\')
        {
            out.push_back(L'\\');
            absolute = true;
            driveAbsolute = true;
        }
    }
    else if (off < s.size() && s[off] == L'\\')
    {
        out = L"\\";
        absolute = true;
    }

    // Components: collapse separators, resolve "." and "..".
    std::vector<Span> parts;
    const size_t n = s.size();
    size_t i = off;
    while (i < n)
    {
        while (i < n && s[i] == L'\\')
            ++i;
        const size_t start = i;
        while (i < n && s[i] != L'\\')
            ++i;
        const size_t len = i - start;
        if (len == 0)
            break;

        if (unc && parts.size() < 2)
        {
            parts.push_back(Span{ start, len });   // server and share are kept verbatim
            continue;
        }
        if (len == 1 && s[start] == L'.')
            continue;
        if (len == 2 && s[start] == L'.' && s[start + 1] == L'.')
        {
            const bool topIsDotDot = !parts.empty() && parts.back().len == 2 &&
                                     s[parts.back().pos] == L'.' && s[parts.back().pos + 1] == L'.';
            if (parts.size() > minParts && !topIsDotDot)
                parts.pop_back();
            else if (!absolute)
                parts.push_back(Span{ start, len });   // relative paths keep leading ".."
            continue;
        }
        parts.push_back(Span{ start, len });
    }

    // Alternate data stream: a ':' inside the last component ("b.pdf:Zone.Identifier:$DATA").
    if (parts.size() > minParts)
    {
        Span& last = parts.back();
        for (size_t k = 0; k < last.len; ++k)
        {
            if (s[last.pos + k] == L':')
            {
                last.len = k;
                break;
            }
        }
        if (last.len == 0)
            parts.pop_back();
    }

    for (size_t k = 0; k < parts.size(); ++k)
    {
        if (k > 0)
            out.push_back(L'\\');
        out.append(s, parts[k].pos, parts[k].len);
    }
    if (unc && parts.size() == 2)
        out.push_back(L'\\');   // share root keeps its separator: "\\srv\share\"
    if (out.empty())
        return std::wstring(L".");

    if (driveAbsolute && HasShortNameMarker(out))
        out = ExpandShortNames(out);
    return out;
}

void ResultKey(sqlite3_context* ctx, sqlite3_value* arg, bool fileKey)
{
    if (sqlite3_value_type(arg) == SQLITE_NULL)
    {
        sqlite3_result_null(ctx);
        return;
    }
    // Read text before its length (SQLite's documented order).
    const unsigned char* text = sqlite3_value_text(arg);
    if (text == nullptr)
    {
        sqlite3_result_error_nomem(ctx);
        return;
    }
    const int bytes = sqlite3_value_bytes(arg);

    try
    {
        const std::string utf8(reinterpret_cast<const char*>(text), static_cast<size_t>(bytes > 0 ? bytes : 0));
        const std::wstring wide = EntityKey::FromUtf8(utf8);
        const std::string key = fileKey ? EntityKey::FileKey(wide) : EntityKey::Key(wide);
        sqlite3_result_text64(ctx, key.data(), static_cast<sqlite3_uint64>(key.size()),
                              SQLITE_TRANSIENT, static_cast<unsigned char>(SQLITE_UTF8));
    }
    catch (const std::bad_alloc&)
    {
        sqlite3_result_error_nomem(ctx);
    }
    catch (...)
    {
        sqlite3_result_error(ctx, "warp key function failed", -1);
    }
}

void SqlWarpKey(sqlite3_context* ctx, int argc, sqlite3_value** argv)
{
    (void)argc;   // registered with nArg = 1
    ResultKey(ctx, argv[0], false);
}

void SqlWarpFileKey(sqlite3_context* ctx, int argc, sqlite3_value** argv)
{
    (void)argc;   // registered with nArg = 1
    ResultKey(ctx, argv[0], true);
}
}

namespace EntityKey
{
std::wstring CanonicalPath(const std::wstring& path)
{
    std::wstring result;
    try
    {
        result = CanonicalPathImpl(path);
    }
    catch (const std::bad_alloc&)
    {
        throw;
    }
    catch (...)
    {
        result = path;
    }
    return result;
}

std::string Key(const std::wstring& s)
{
    return ToUtf8(LowerInvariant(s));
}

std::string FileKey(const std::wstring& path)
{
    return Key(CanonicalPath(path));
}

std::string AppKey(const std::wstring& exePathOrAumid)
{
    const std::wstring& s = exePathOrAumid;
    if (s.empty())
        return std::string();

    // Paths: "X:\..." or anything rooted ("\\srv\...", "\\?\...", "\Device\..."). AUMIDs never start with '\'.
    const bool hasDrive = s.size() >= 3 && IsAsciiAlpha(s[0]) && s[1] == L':' && IsSep(s[2]);
    if (hasDrive || IsSep(s[0]))
        return FileKey(s);
    return "aumid:" + Key(s);
}

std::string UrlKey(const std::wstring& canonicalUrl)
{
    const size_t scheme = canonicalUrl.find(L"://");
    if (scheme == std::wstring::npos) return ToUtf8(canonicalUrl);
    const size_t end = canonicalUrl.find_first_of(L"/?#", scheme + 3);
    const size_t authorityEnd = end == std::wstring::npos ? canonicalUrl.size() : end;
    return Key(canonicalUrl.substr(0, authorityEnd)) + ToUtf8(canonicalUrl.substr(authorityEnd));
}

std::string DocKey(const std::wstring& docId)
{
    const size_t colon = docId.find(L':');
    if (colon == std::wstring::npos) return ToUtf8(docId);
    // Provider-specific normalization belongs in UrlCanonicalizer. Opaque payloads
    // (Google/YouTube/Figma IDs, for example) must never be case-folded here.
    return Key(docId.substr(0, colon + 1)) + ToUtf8(docId.substr(colon + 1));
}

std::wstring BaseName(const std::wstring& path)
{
    size_t end = path.size();
    while (end > 0 && IsSep(path[end - 1]))
        --end;
    size_t start = end;
    while (start > 0 && !IsSep(path[start - 1]))
        --start;
    if (start == 0 && end >= 2 && IsAsciiAlpha(path[0]) && path[1] == L':')
        start = 2;   // "C:name" -> "name"
    if (start >= end)
        return path;   // roots ("C:\", "\\") are their own display name
    return path.substr(start, end - start);
}

std::string ToUtf8(const std::wstring& w)
{
    std::string out;
    const wchar_t* p = w.data();
    size_t remaining = w.size();
    while (remaining > 0)
    {
        const size_t n = WideChunk(p, remaining);
        const int cch = static_cast<int>(n);
        // Flags 0: unpaired surrogates are replaced with U+FFFD instead of failing.
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, p, cch, nullptr, 0, nullptr, nullptr);
        if (bytes > 0)
        {
            const size_t old = out.size();
            out.resize(old + static_cast<size_t>(bytes));
            const int written = WideCharToMultiByte(CP_UTF8, 0, p, cch, &out[old], bytes, nullptr, nullptr);
            out.resize(old + static_cast<size_t>(written > 0 ? written : 0));
        }
        p += n;
        remaining -= n;
    }
    return out;
}

std::wstring FromUtf8(const std::string& s)
{
    std::wstring out;
    const char* p = s.data();
    size_t remaining = s.size();
    while (remaining > 0)
    {
        const size_t n = Utf8Chunk(p, remaining);
        const int cb = static_cast<int>(n);
        // Flags 0: invalid sequences are replaced with U+FFFD instead of failing.
        const int chars = MultiByteToWideChar(CP_UTF8, 0, p, cb, nullptr, 0);
        if (chars > 0)
        {
            const size_t old = out.size();
            out.resize(old + static_cast<size_t>(chars));
            const int written = MultiByteToWideChar(CP_UTF8, 0, p, cb, &out[old], chars);
            out.resize(old + static_cast<size_t>(written > 0 ? written : 0));
        }
        p += n;
        remaining -= n;
    }
    return out;
}

void RegisterSqliteFunctions(sqlite3* db)
{
    if (db == nullptr)
        return;

    const int flags = SQLITE_UTF8 | SQLITE_DETERMINISTIC;
    if (sqlite3_create_function_v2(db, "warp_key", 1, flags, nullptr,
                                   &SqlWarpKey, nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        OutputDebugStringW(L"WARP: failed to register SQL function warp_key\n");
    }
    if (sqlite3_create_function_v2(db, "warp_file_key", 1, flags, nullptr,
                                   &SqlWarpFileKey, nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        OutputDebugStringW(L"WARP: failed to register SQL function warp_file_key\n");
    }
}
}
