// UrlCanonicalizer.cpp - see UrlCanonicalizer.h.

#include "framework.h"
#include "UrlCanonicalizer.h"

#include <cstdint>
#include <cwchar>
#include <new>
#include <utility>

namespace UrlCanon
{
namespace
{
using std::wstring;

// ---------------------------------------------------------------------------
// Character classes
// ---------------------------------------------------------------------------

inline bool IsAsciiAlpha(wchar_t c)
{
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z');
}

inline bool IsAsciiDigit(wchar_t c)
{
    return c >= L'0' && c <= L'9';
}

inline bool IsAsciiAlnum(wchar_t c)
{
    return IsAsciiAlpha(c) || IsAsciiDigit(c);
}

inline bool IsAsciiUpper(wchar_t c)
{
    return c >= L'A' && c <= L'Z';
}

inline bool IsHexDigit(wchar_t c)
{
    return IsAsciiDigit(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

inline unsigned int HexValue(wchar_t c)
{
    if (c >= L'0' && c <= L'9')
        return static_cast<unsigned int>(c - L'0');
    if (c >= L'a' && c <= L'f')
        return static_cast<unsigned int>(c - L'a' + 10);
    return static_cast<unsigned int>(c - L'A' + 10);
}

inline wchar_t AsciiLower(wchar_t c)
{
    return IsAsciiUpper(c) ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}

inline wchar_t AsciiUpper(wchar_t c)
{
    return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - (L'a' - L'A')) : c;
}

// base64url alphabet; also the alphabet of most opaque ids.
inline bool IsIdChar(wchar_t c)
{
    return IsAsciiAlnum(c) || c == L'-' || c == L'_';
}

inline bool IsHighSurrogateUnit(wchar_t c)
{
    return c >= 0xD800 && c <= 0xDBFF;
}

inline bool IsLowSurrogateUnit(wchar_t c)
{
    return c >= 0xDC00 && c <= 0xDFFF;
}

bool IsWhitespaceCp(uint32_t c)
{
    switch (c)
    {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
    case 0x85: case 0xA0: case 0x1680: case 0x180E: case 0x200B:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

bool IsControlCp(uint32_t c)
{
    return c < 0x20 || (c >= 0x7F && c <= 0x9F);
}

// Bidi controls enable visual spoofing; they are never emitted unescaped.
bool IsBidiCp(uint32_t c)
{
    return c == 0x061C || c == 0x200E || c == 0x200F ||
           (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
}

bool IsInvisibleCp(uint32_t c)
{
    return IsBidiCp(c) || c == 0x00AD || (c >= 0x200B && c <= 0x200D) ||
           (c >= 0x2060 && c <= 0x2064) || c == 0xFEFF;
}

// Code points allowed unescaped in a canonical URL.
bool IsCanonicalSafeCp(uint32_t c)
{
    return !IsControlCp(c) && !IsWhitespaceCp(c) && !IsInvisibleCp(c);
}

// Code points PercentDecode may produce for display.
bool IsDisplaySafeCp(uint32_t c)
{
    return !IsControlCp(c) && !IsBidiCp(c);
}

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

wstring AsciiLowerCopy(const wstring& s)
{
    wstring out(s);
    for (wchar_t& c : out)
        c = AsciiLower(c);
    return out;
}

wstring AsciiUpperCopy(const wstring& s)
{
    wstring out(s);
    for (wchar_t& c : out)
        c = AsciiUpper(c);
    return out;
}

// ASCII case-insensitive prefix test at s[pos].
bool StartsWithNoCase(const wstring& s, size_t pos, const wchar_t* prefix)
{
    for (size_t i = 0; prefix[i] != L'\0'; ++i)
    {
        if (pos + i >= s.size() || AsciiLower(s[pos + i]) != AsciiLower(prefix[i]))
            return false;
    }
    return true;
}

bool EqualsNoCase(const wstring& a, const wchar_t* b)
{
    return a.size() == std::wcslen(b) && StartsWithNoCase(a, 0, b);
}

bool EndsWith(const wstring& s, const wchar_t* suffix)
{
    const size_t len = std::wcslen(suffix);
    return s.size() >= len && s.compare(s.size() - len, len, suffix) == 0;
}

bool IsAllDigits(const wstring& s)
{
    if (s.empty())
        return false;
    for (wchar_t c : s)
    {
        if (!IsAsciiDigit(c))
            return false;
    }
    return true;
}

bool IsHexString(const wstring& s)
{
    if (s.empty())
        return false;
    for (wchar_t c : s)
    {
        if (!IsHexDigit(c))
            return false;
    }
    return true;
}

bool IsIdString(const wstring& s, size_t minLen, size_t maxLen)
{
    if (s.size() < minLen || s.size() > maxLen)
        return false;
    for (wchar_t c : s)
    {
        if (!IsIdChar(c))
            return false;
    }
    return true;
}

bool HasSpaceOrControl(const wstring& s)
{
    for (wchar_t c : s)
    {
        if (IsWhitespaceCp(c) || IsControlCp(c))
            return true;
    }
    return false;
}

wstring TrimUrlText(const wstring& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (IsWhitespaceCp(s[b]) || IsControlCp(s[b])))
        ++b;
    while (e > b && (IsWhitespaceCp(s[e - 1]) || IsControlCp(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

// Lowercases a host. Non-ASCII (IDN) hosts use the invariant locale, never the user's locale.
wstring LowerHost(const wstring& s)
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
    if (ascii || s.size() > 4096)
        return AsciiLowerCopy(s);

    const int cch = static_cast<int>(s.size());
    const int needed = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), cch,
                                     nullptr, 0, nullptr, nullptr, 0);
    if (needed <= 0)
        return AsciiLowerCopy(s);
    wstring out(static_cast<size_t>(needed), L'\0');
    const int written = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), cch,
                                      &out[0], needed, nullptr, nullptr, 0);
    if (written <= 0)
        return AsciiLowerCopy(s);
    out.resize(static_cast<size_t>(written));
    return out;
}

// True if host equals domain or is a subdomain of it (match on a label boundary).
bool HostIsOrUnder(const wstring& host, const wchar_t* domain, size_t len)
{
    if (len == 0 || host.size() < len)
        return false;
    const size_t off = host.size() - len;
    if (host.compare(off, len, domain, len) != 0)
        return false;
    return off == 0 || host[off - 1] == L'.';
}

bool HostIsOrUnder(const wstring& host, const wchar_t* domain)
{
    return HostIsOrUnder(host, domain, std::wcslen(domain));
}

// ---------------------------------------------------------------------------
// Percent-encoding and UTF-8
// ---------------------------------------------------------------------------

void AppendPercentByte(wstring& out, unsigned int b)
{
    static const wchar_t kHex[] = L"0123456789ABCDEF";
    out.push_back(L'%');
    out.push_back(kHex[(b >> 4) & 0xF]);
    out.push_back(kHex[b & 0xF]);
}

void AppendCodePoint(wstring& out, uint32_t cp)
{
    if (cp >= 0x10000)
    {
        const uint32_t v = cp - 0x10000;
        out.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
        out.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
    }
    else
    {
        out.push_back(static_cast<wchar_t>(cp));
    }
}

// Appends cp as percent-escaped UTF-8.
void AppendPercentEncodedCp(wstring& out, uint32_t cp)
{
    if (cp < 0x80)
    {
        AppendPercentByte(out, cp);
    }
    else if (cp < 0x800)
    {
        AppendPercentByte(out, 0xC0 | (cp >> 6));
        AppendPercentByte(out, 0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        AppendPercentByte(out, 0xE0 | (cp >> 12));
        AppendPercentByte(out, 0x80 | ((cp >> 6) & 0x3F));
        AppendPercentByte(out, 0x80 | (cp & 0x3F));
    }
    else
    {
        AppendPercentByte(out, 0xF0 | (cp >> 18));
        AppendPercentByte(out, 0x80 | ((cp >> 12) & 0x3F));
        AppendPercentByte(out, 0x80 | ((cp >> 6) & 0x3F));
        AppendPercentByte(out, 0x80 | (cp & 0x3F));
    }
}

// Decodes the UTF-8 sequence starting at bytes[k]; returns its length, or 0 if malformed/overlong/surrogate.
size_t DecodeUtf8At(const std::string& bytes, size_t k, uint32_t& cp)
{
    const unsigned int b0 = static_cast<unsigned char>(bytes[k]);
    size_t len = 0;
    uint32_t minCp = 0;
    if (b0 < 0x80)
    {
        cp = b0;
        return 1;
    }
    if ((b0 & 0xE0) == 0xC0)
    {
        len = 2;
        cp = b0 & 0x1F;
        minCp = 0x80;
    }
    else if ((b0 & 0xF0) == 0xE0)
    {
        len = 3;
        cp = b0 & 0x0F;
        minCp = 0x800;
    }
    else if ((b0 & 0xF8) == 0xF0)
    {
        len = 4;
        cp = b0 & 0x07;
        minCp = 0x10000;
    }
    else
    {
        return 0;
    }

    if (k + len > bytes.size())
        return 0;
    for (size_t m = 1; m < len; ++m)
    {
        const unsigned int b = static_cast<unsigned char>(bytes[k + m]);
        if ((b & 0xC0) != 0x80)
            return 0;
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0;
    return len;
}

// Consumes the run of "%XX" escapes at s[i] and appends its decoded form; returns the index after the run
// (== i when s[i] does not start a valid escape).
//   canonical == true : decode unreserved ASCII and safe non-ASCII text; re-emit other bytes as uppercase escapes.
//   canonical == false: (display) decode all but controls, bidi marks and invalid UTF-8, which stay verbatim.
size_t AppendEscapeRun(const wstring& s, size_t i, bool canonical, wstring& out)
{
    const size_t n = s.size();
    std::string bytes;
    size_t j = i;
    while (j + 2 < n && s[j] == L'%' && IsHexDigit(s[j + 1]) && IsHexDigit(s[j + 2]))
    {
        bytes.push_back(static_cast<char>((HexValue(s[j + 1]) << 4) | HexValue(s[j + 2])));
        j += 3;
    }

    size_t k = 0;
    while (k < bytes.size())
    {
        uint32_t cp = 0;
        size_t len = DecodeUtf8At(bytes, k, cp);
        bool decode = false;
        if (len == 0)
        {
            len = 1;
        }
        else if (cp < 0x80)
        {
            const wchar_t c = static_cast<wchar_t>(cp);
            decode = canonical ? (IsAsciiAlnum(c) || c == L'-' || c == L'.' || c == L'_' || c == L'~')
                               : !IsControlCp(cp);
        }
        else
        {
            decode = canonical ? IsCanonicalSafeCp(cp) : IsDisplaySafeCp(cp);
        }

        if (decode)
        {
            AppendCodePoint(out, cp);
        }
        else if (canonical)
        {
            for (size_t m = 0; m < len; ++m)
                AppendPercentByte(out, static_cast<unsigned char>(bytes[k + m]));
        }
        else
        {
            out.append(s, i + 3 * k, 3 * len);
        }
        k += len;
    }
    return j;
}

// ASCII characters never valid raw in a path or query; escaping them gives one canonical spelling.
bool MustEscapeAscii(wchar_t c)
{
    if (c <= 0x20 || c == 0x7F)
        return true;
    switch (c)
    {
    case L'"': case L'#': case L'<': case L'>': case L'[': case L'\\': case L']':
    case L'^': case L'`': case L'{': case L'|': case L'}':
        return true;
    default:
        return false;
    }
}

// RFC 3986 normalization of one path segment or query component: uppercase escapes, decode unreserved and
// safe UTF-8 escapes, escape characters that are invalid or visually deceptive.
wstring NormalizeComponent(const wstring& s)
{
    wstring out;
    out.reserve(s.size());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n)
    {
        const wchar_t c = s[i];
        if (c == L'%')
        {
            const size_t next = AppendEscapeRun(s, i, true, out);
            if (next == i)
            {
                out.append(L"%25");   // stray '%'
                ++i;
            }
            else
            {
                i = next;
            }
        }
        else if (c < 0x80)
        {
            if (MustEscapeAscii(c))
                AppendPercentByte(out, c);
            else
                out.push_back(c);
            ++i;
        }
        else if (IsHighSurrogateUnit(c) && i + 1 < n && IsLowSurrogateUnit(s[i + 1]))
        {
            const uint32_t cp = 0x10000 + ((static_cast<uint32_t>(c) - 0xD800) << 10) +
                                (static_cast<uint32_t>(s[i + 1]) - 0xDC00);
            if (IsCanonicalSafeCp(cp))
            {
                out.push_back(c);
                out.push_back(s[i + 1]);
            }
            else
            {
                AppendPercentEncodedCp(out, cp);
            }
            i += 2;
        }
        else
        {
            if (IsHighSurrogateUnit(c) || IsLowSurrogateUnit(c))
                AppendPercentEncodedCp(out, 0xFFFD);   // unpaired surrogate
            else if (IsCanonicalSafeCp(c))
                out.push_back(c);
            else
                AppendPercentEncodedCp(out, c);
            ++i;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Token redaction
// ---------------------------------------------------------------------------

// A dictionary-shaped slug piece: digits ("2024"), a lower/Capitalized word or short ACRONYM, optionally with
// a short numeric suffix ("v2", "win11").
bool IsWordPiece(const wchar_t* b, size_t len)
{
    if (len == 0 || len > 20)
        return false;

    size_t digits = 0;
    for (size_t k = 0; k < len; ++k)
    {
        if (IsAsciiDigit(b[k]))
            ++digits;
    }
    if (digits == len)
        return true;

    size_t letters = 0;
    bool allUpper = true;
    while (letters < len && IsAsciiAlpha(b[letters]))
    {
        if (!IsAsciiUpper(b[letters]))
            allUpper = false;
        ++letters;
    }
    if (letters == 0)
        return false;
    if (allUpper)
    {
        if (letters > 5)
            return false;
    }
    else
    {
        for (size_t k = 1; k < letters; ++k)
        {
            if (IsAsciiUpper(b[k]))
                return false;   // mixed case inside a word is random-looking
        }
    }
    for (size_t k = letters; k < len; ++k)
    {
        if (!IsAsciiDigit(b[k]))
            return false;
    }
    return len - letters <= 4;
}

// Human-readable slugs ("top-10-tips-for-2024") are not secrets; random tokens essentially never split into
// three or more dictionary-shaped pieces.
bool IsReadableSlug(const wstring& p)
{
    size_t pieces = 0;
    size_t start = 0;
    for (size_t k = 0; k <= p.size(); ++k)
    {
        if (k == p.size() || p[k] == L'-' || p[k] == L'_')
        {
            const size_t len = k - start;
            if (len > 0)
            {
                if (!IsWordPiece(p.data() + start, len))
                    return false;
                ++pieces;
            }
            start = k + 1;
        }
    }
    return pieces >= 3;
}

// JWT-like "header.payload.signature" made of base64url parts.
bool IsJwtLike(const wstring& p)
{
    const size_t d1 = p.find(L'.');
    if (d1 == wstring::npos)
        return false;
    const size_t d2 = p.find(L'.', d1 + 1);
    if (d2 == wstring::npos || p.find(L'.', d2 + 1) != wstring::npos)
        return false;
    for (size_t k = 0; k < p.size(); ++k)
    {
        if (k != d1 && k != d2 && !IsIdChar(p[k]))
            return false;
    }
    const size_t len1 = d1;
    const size_t len2 = d2 - d1 - 1;
    const size_t len3 = p.size() - d2 - 1;
    const bool jwtHeader = len1 >= 10 && len2 >= 10 && p.compare(0, 3, L"eyJ") == 0;
    const bool longParts = len1 >= 16 && len2 >= 16 && len3 >= 16;
    return jwtHeader || longParts;
}

bool IsTokenLike(const wstring& p)
{
    if (p.size() >= 32 && IsHexString(p))
        return true;
    if (p.size() >= 24)
    {
        size_t digits = 0;
        size_t letters = 0;
        bool idChars = true;
        for (wchar_t c : p)
        {
            if (IsAsciiDigit(c))
                ++digits;
            else if (IsAsciiAlpha(c))
                ++letters;
            else if (c != L'-' && c != L'_')
            {
                idChars = false;
                break;
            }
        }
        if (idChars && digits >= 2 && letters >= 2 && !IsReadableSlug(p))
            return true;
    }
    return IsJwtLike(p);
}

// "<token>" -> ":token"; "<token>.pdf" -> ":token.pdf".
wstring RedactPiece(const wstring& piece)
{
    if (IsTokenLike(piece))
        return L":token";
    const size_t dot = piece.rfind(L'.');
    if (dot != wstring::npos && dot > 0)
    {
        const size_t extLen = piece.size() - dot - 1;
        bool extOk = extLen >= 1 && extLen <= 5;
        for (size_t k = dot + 1; extOk && k < piece.size(); ++k)
            extOk = IsAsciiAlnum(piece[k]);
        if (extOk && IsTokenLike(piece.substr(0, dot)))
            return L":token" + piece.substr(dot);
    }
    return piece;
}

// Redacts each ';', '=' or ',' separated piece of a segment ("cart;jsessionid=<token>").
wstring RedactSegment(const wstring& seg)
{
    wstring out;
    size_t start = 0;
    for (size_t k = 0; k <= seg.size(); ++k)
    {
        if (k == seg.size() || seg[k] == L';' || seg[k] == L'=' || seg[k] == L',')
        {
            out += RedactPiece(seg.substr(start, k - start));
            if (k < seg.size())
                out.push_back(seg[k]);
            start = k + 1;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Path and query
// ---------------------------------------------------------------------------

// Normalized path segments: '\' treated as '/', empty segments collapsed, "." and ".." (including escaped
// forms such as "%2e%2e") resolved without climbing above the root.
std::vector<wstring> NormalizePathSegments(const wstring& rawPath)
{
    std::vector<wstring> segs;
    const size_t n = rawPath.size();
    size_t i = 0;
    while (i < n)
    {
        while (i < n && (rawPath[i] == L'/' || rawPath[i] == L'\\'))
            ++i;
        const size_t start = i;
        while (i < n && rawPath[i] != L'/' && rawPath[i] != L'\\')
            ++i;
        if (i == start)
            break;

        wstring seg = NormalizeComponent(rawPath.substr(start, i - start));
        if (seg == L".")
            continue;
        if (seg == L"..")
        {
            if (!segs.empty())
                segs.pop_back();
            continue;
        }
        segs.push_back(std::move(seg));
    }
    return segs;
}

wstring JoinPath(const std::vector<wstring>& segs)
{
    if (segs.empty())
        return wstring(L"/");
    wstring out;
    for (const wstring& seg : segs)
    {
        out.push_back(L'/');
        out += seg;
    }
    return out;
}

struct QueryParam
{
    wstring rawKey;
    wstring rawValue;
    wstring key;            // percent-decoded, ASCII-lowercased; for matching only
    bool    hasValue = false;
};

std::vector<QueryParam> ParseQuery(const wstring& query)
{
    std::vector<QueryParam> params;
    size_t start = 0;
    while (start < query.size())
    {
        size_t end = query.find(L'&', start);
        if (end == wstring::npos)
            end = query.size();
        if (end > start)
        {
            QueryParam p;
            const size_t eq = query.find(L'=', start);
            if (eq != wstring::npos && eq < end)
            {
                p.rawKey = query.substr(start, eq - start);
                p.rawValue = query.substr(eq + 1, end - eq - 1);
                p.hasValue = true;
            }
            else
            {
                p.rawKey = query.substr(start, end - start);
            }
            p.key = AsciiLowerCopy(PercentDecode(p.rawKey));
            params.push_back(std::move(p));
        }
        start = end + 1;
    }
    return params;
}

const QueryParam* FindParam(const std::vector<QueryParam>& params, const wchar_t* key)
{
    for (const QueryParam& p : params)
    {
        if (p.key == key)
            return &p;
    }
    return nullptr;
}

// Keys that carry credentials or session state are never kept, whatever the allowlists say.
bool IsSensitiveKey(const wstring& key)
{
    static const wchar_t* const kExact[] =
    {
        L"token", L"access_token", L"id_token", L"refresh_token", L"code", L"state", L"sig", L"signature",
        L"se", L"sp", L"sv", L"sr", L"skoid", L"sktid", L"password", L"pwd", L"pass", L"secret",
        L"client_secret", L"key", L"apikey", L"api_key", L"session", L"sessionid", L"session_id", L"sid",
        L"auth", L"authorization", L"otp"
    };
    for (const wchar_t* k : kExact)
    {
        if (key == k)
            return true;
    }
    static const wchar_t* const kParts[] = { L"token", L"secret", L"passw", L"signature", L"credential", L"sessionid" };
    for (const wchar_t* part : kParts)
    {
        if (key.find(part) != wstring::npos)
            return true;
    }
    return false;
}

bool IsYouTubeHost(const wstring& host)
{
    return host == L"youtube.com" || host == L"www.youtube.com" || host == L"m.youtube.com";
}

bool IsSearchHost(const wstring& host)
{
    return HostIsOrUnder(host, L"google.com") || HostIsOrUnder(host, L"bing.com") ||
           HostIsOrUnder(host, L"duckduckgo.com");
}

bool IsKeptQueryKey(const wstring& host, const wstring& key, const std::vector<wstring>& extraKeys)
{
    if (key.empty() || IsSensitiveKey(key))
        return false;
    if (IsSearchHost(host) && (key == L"q" || key == L"query" || key == L"oq"))
        return false;   // search terms are sensitive, even if a caller allow-lists "q"
    if (IsYouTubeHost(host) && (key == L"v" || key == L"list"))
        return true;
    if (HostIsOrUnder(host, L"sharepoint.com") && (key == L"sourcedoc" || key == L"id" || key == L"file"))
        return true;
    for (const wstring& extra : extraKeys)
    {
        if (EqualsNoCase(key, extra.c_str()))
            return true;
    }
    return false;
}

// Allow-listed parameters in original order, minimally re-encoded.
wstring BuildQuery(const wstring& host, const std::vector<QueryParam>& params, const std::vector<wstring>& extraKeys)
{
    wstring out;
    for (const QueryParam& p : params)
    {
        if (!IsKeptQueryKey(host, p.key, extraKeys))
            continue;
        if (!out.empty())
            out.push_back(L'&');
        out += NormalizeComponent(p.rawKey);
        if (p.hasValue)
        {
            out.push_back(L'=');
            out += NormalizeComponent(p.rawValue);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Exclusions
// ---------------------------------------------------------------------------

// Accepts "example.com", ".example.com", "*.example.com" or a full URL.
wstring NormalizeDomain(const wstring& d)
{
    wstring v = TrimUrlText(d);
    if (v.find(L"://") != wstring::npos)
        return HostOf(v);
    v = LowerHost(v);
    size_t lead = 0;
    while (lead < v.size() && (v[lead] == L'*' || v[lead] == L'.'))
        ++lead;
    v.erase(0, lead);
    while (!v.empty() && v.back() == L'.')
        v.pop_back();
    return v;
}

bool IsExcluded(const wstring& host, const wstring& path, const std::vector<wstring>& callerDomains)
{
    if (host.empty())
        return false;

    // Authentication and password-manager hosts: sign-in pages carry tokens.
    static const wchar_t* const kAuthHosts[] =
    {
        L"vault.bitwarden.com", L"my.1password.com", L"my.1password.eu", L"accounts.google.com",
        L"login.microsoftonline.com", L"login.live.com", L"login.windows.net"
    };
    for (const wchar_t* d : kAuthHosts)
    {
        if (HostIsOrUnder(host, d))
            return true;
    }
    if (HostIsOrUnder(host, L"lastpass.com") && StartsWithNoCase(path, 0, L"/vault"))
        return true;
    if (HostIsOrUnder(host, L"account.microsoft.com") && StartsWithNoCase(path, 0, L"/security"))
        return true;

    // Online banking and patient portals.
    static const wchar_t* const kHostWords[] = { L"netbanking", L"onlinebanking", L"ebanking", L"mychart", L"patientportal" };
    for (const wchar_t* w : kHostWords)
    {
        if (host.find(w) != wstring::npos)
            return true;
    }

    for (const wstring& d : callerDomains)
    {
        const wstring norm = NormalizeDomain(d);
        if (HostIsOrUnder(host, norm.c_str(), norm.size()))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Document identity
// ---------------------------------------------------------------------------

struct DocIdentity
{
    wstring kind;
    wstring id;
    wstring canonical;      // replacement canonical URL (YouTube), else empty
    wstring canonicalHost;
};

bool MatchYouTube(const wstring& host, const std::vector<wstring>& segs,
                  const std::vector<QueryParam>& params, DocIdentity& doc)
{
    wstring id;
    if (IsYouTubeHost(host))
    {
        if (segs.size() == 1 && segs[0] == L"watch")
        {
            const QueryParam* v = FindParam(params, L"v");
            if (v != nullptr)
                id = PercentDecode(v->rawValue);
        }
        else if (segs.size() >= 2 && (segs[0] == L"shorts" || segs[0] == L"embed" || segs[0] == L"live"))
        {
            id = segs[1];
        }
    }
    else if (host == L"youtu.be" && !segs.empty())
    {
        id = segs[0];
    }

    if (!IsIdString(id, 11, 11))
        return false;
    doc.kind = L"youtube";
    doc.id = L"yt:" + id;
    doc.canonical = L"https://www.youtube.com/watch?v=" + id;
    doc.canonicalHost = L"www.youtube.com";
    return true;
}

bool MatchGoogle(const wstring& host, const std::vector<wstring>& segs,
                 const std::vector<QueryParam>& params, DocIdentity& doc)
{
    const size_t n = segs.size();
    if (host == L"docs.google.com")
    {
        size_t i = (n >= 2 && segs[0] == L"a") ? 2 : 0;   // /a/<domain>/document/...
        if (i >= n)
            return false;
        const wstring& type = segs[i];
        if (type != L"document" && type != L"spreadsheets" && type != L"presentation" && type != L"forms")
            return false;
        ++i;
        if (i + 1 < n && segs[i] == L"u")
            i += 2;                                       // /u/<account>/
        if (i + 1 >= n || segs[i] != L"d")
            return false;
        ++i;
        if (segs[i] == L"e" && i + 1 < n)
            ++i;                                          // published forms: /forms/d/e/<id>/viewform
        if (!IsIdString(segs[i], 10, 200))
            return false;
        doc.kind = L"google-docs";
        doc.id = L"gdoc:" + segs[i];
        return true;
    }

    if (host == L"drive.google.com")
    {
        if (n >= 1 && segs[0] == L"file")
        {
            size_t i = 1;
            if (i + 1 < n && segs[i] == L"u")
                i += 2;
            if (i + 1 < n && segs[i] == L"d" && IsIdString(segs[i + 1], 10, 200))
            {
                doc.kind = L"google-drive";
                doc.id = L"gdrive:" + segs[i + 1];
                return true;
            }
            return false;
        }
        if (n == 1 && (segs[0] == L"open" || segs[0] == L"uc"))
        {
            const QueryParam* p = FindParam(params, L"id");
            if (p != nullptr)
            {
                const wstring id = PercentDecode(p->rawValue);
                if (IsIdString(id, 10, 200))
                {
                    doc.kind = L"google-drive";
                    doc.id = L"gdrive:" + id;
                    return true;
                }
            }
        }
    }
    return false;
}

// 32 hex digits, optionally in 8-4-4-4-12 form.
bool IsGuid(const wstring& s)
{
    if (s.size() == 32)
        return IsHexString(s);
    if (s.size() != 36)
        return false;
    for (size_t k = 0; k < s.size(); ++k)
    {
        const bool dash = (k == 8 || k == 13 || k == 18 || k == 23);
        if (dash ? (s[k] != L'-') : !IsHexDigit(s[k]))
            return false;
    }
    return true;
}

// SharePoint sharing-link type marker: ":w:", ":x:", ":p:", ":b:", ":f:", ":u:", ":li:" ...
bool IsShareTypeSegment(const wstring& seg)
{
    if (seg.size() < 3 || seg.size() > 5 || seg.front() != L':' || seg.back() != L':')
        return false;
    for (size_t k = 1; k + 1 < seg.size(); ++k)
    {
        if (!IsAsciiAlpha(seg[k]))
            return false;
    }
    return true;
}

bool MatchSharePoint(const wstring& host, const std::vector<wstring>& segs,
                     const std::vector<QueryParam>& params, DocIdentity& doc)
{
    if (!HostIsOrUnder(host, L"sharepoint.com"))
        return false;

    const QueryParam* p = FindParam(params, L"sourcedoc");
    if (p != nullptr)
    {
        wstring guid = PercentDecode(p->rawValue);
        if (guid.size() >= 2 && guid.front() == L'{' && guid.back() == L'}')
            guid = guid.substr(1, guid.size() - 2);
        if (IsGuid(guid))
        {
            doc.kind = L"sharepoint";
            doc.id = L"spo:" + AsciiLowerCopy(guid);
            return true;
        }
    }

    // Sharing links: /:w:/r/<server-relative path>
    if (segs.size() >= 3 && IsShareTypeSegment(segs[0]) && EqualsNoCase(segs[1], L"r"))
    {
        doc.kind = L"sharepoint";
        doc.id = L"spo-path:" + host + PercentDecode(JoinPath(segs));
        return true;
    }
    return false;
}

bool MatchOneDrive(const wstring& host, const std::vector<QueryParam>& params, DocIdentity& doc)
{
    if (host != L"onedrive.live.com")
        return false;
    const QueryParam* p = FindParam(params, L"resid");
    if (p == nullptr)
        p = FindParam(params, L"id");
    if (p == nullptr)
        return false;
    const wstring id = AsciiLowerCopy(PercentDecode(p->rawValue));
    if (id.empty() || id.size() > 256 || HasSpaceOrControl(id))
        return false;
    doc.kind = L"onedrive";
    doc.id = L"od:" + id;
    return true;
}

bool IsGitHubName(const wstring& s)
{
    if (s.empty() || s.size() > 100)
        return false;
    for (wchar_t c : s)
    {
        if (!IsAsciiAlnum(c) && c != L'-' && c != L'_' && c != L'.')
            return false;
    }
    return true;
}

bool IsReservedGitHubRoot(const wstring& first)
{
    static const wchar_t* const kReserved[] =
    {
        L"about", L"account", L"apps", L"codespaces", L"collections", L"contact", L"customer-stories",
        L"dashboard", L"enterprise", L"events", L"explore", L"features", L"issues", L"join", L"login",
        L"logout", L"marketplace", L"new", L"notifications", L"orgs", L"organizations", L"pricing",
        L"pulls", L"search", L"security", L"sessions", L"settings", L"signup", L"site", L"sponsors",
        L"stars", L"team", L"topics", L"trending", L"users", L"watching"
    };
    for (const wchar_t* r : kReserved)
    {
        if (EqualsNoCase(first, r))
            return true;
    }
    return false;
}

bool MatchGitHub(const wstring& host, const std::vector<wstring>& segs, DocIdentity& doc)
{
    if (host != L"github.com" && host != L"www.github.com")
        return false;
    if (segs.size() < 2 || IsReservedGitHubRoot(segs[0]))
        return false;

    wstring repo = segs[1];
    if (EndsWith(AsciiLowerCopy(repo), L".git"))
        repo.resize(repo.size() - 4);
    if (!IsGitHubName(segs[0]) || !IsGitHubName(repo))
        return false;

    const wstring base = L"gh:" + AsciiLowerCopy(segs[0]) + L"/" + AsciiLowerCopy(repo);
    if (segs.size() >= 4 && IsAllDigits(segs[3]))
    {
        if (segs[2] == L"pull")
        {
            doc.kind = L"github-pr";
            doc.id = base + L"#pr" + segs[3];
            return true;
        }
        if (segs[2] == L"issues")
        {
            doc.kind = L"github-issue";
            doc.id = base + L"#" + segs[3];
            return true;
        }
    }
    doc.kind = L"github-repo";
    doc.id = base;
    return true;
}

// dev.azure.com/<org>/<project>/... and <org>.visualstudio.com/[DefaultCollection/]<project>/...
bool MatchAzureDevOps(const wstring& host, const std::vector<wstring>& segs, DocIdentity& doc)
{
    static const wchar_t kVsSuffix[] = L".visualstudio.com";
    const size_t vsSuffixLen = (sizeof(kVsSuffix) / sizeof(kVsSuffix[0])) - 1;

    wstring org;
    size_t first = 0;
    if (host == L"dev.azure.com")
    {
        if (segs.empty())
            return false;
        org = segs[0];
        first = 1;
    }
    else if (host.size() > vsSuffixLen && EndsWith(host, kVsSuffix))
    {
        org = host.substr(0, host.size() - vsSuffixLen);
        if (org.find(L'.') != wstring::npos)
            return false;
    }
    else
    {
        return false;
    }
    org = AsciiLowerCopy(PercentDecode(org));

    for (size_t i = first; i < segs.size(); ++i)
    {
        if (EqualsNoCase(segs[i], L"_workitems") && i + 2 < segs.size() &&
            EqualsNoCase(segs[i + 1], L"edit") && IsAllDigits(segs[i + 2]))
        {
            doc.kind = L"ado-workitem";
            doc.id = L"ado:" + org + L"/wi" + segs[i + 2];
            return true;
        }
        if (EqualsNoCase(segs[i], L"_git") && i + 3 < segs.size() &&
            EqualsNoCase(segs[i + 2], L"pullrequest") && IsAllDigits(segs[i + 3]))
        {
            doc.kind = L"ado-pr";
            doc.id = L"ado:" + org + L"/" + AsciiLowerCopy(PercentDecode(segs[i + 1])) + L"/pr" + segs[i + 3];
            return true;
        }
    }
    return false;
}

bool MatchFigma(const wstring& host, const std::vector<wstring>& segs, DocIdentity& doc)
{
    if ((host != L"figma.com" && host != L"www.figma.com") || segs.size() < 2)
        return false;
    const wstring& type = segs[0];
    if (type != L"file" && type != L"design" && type != L"proto" && type != L"board")
        return false;
    const wstring& key = segs[1];
    if (key.size() < 10 || key.size() > 64)
        return false;
    for (wchar_t c : key)
    {
        if (!IsAsciiAlnum(c))
            return false;
    }
    doc.kind = L"figma";
    doc.id = L"figma:" + key;
    return true;
}

// Jira issue key: PROJ-123 (project key starts with a letter).
bool IsJiraKey(const wstring& s)
{
    const size_t dash = s.find(L'-');
    if (dash == wstring::npos || dash == 0 || dash + 1 >= s.size() || !IsAsciiAlpha(s[0]))
        return false;
    for (size_t k = 1; k < dash; ++k)
    {
        if (!IsAsciiAlnum(s[k]) && s[k] != L'_')
            return false;
    }
    for (size_t k = dash + 1; k < s.size(); ++k)
    {
        if (!IsAsciiDigit(s[k]))
            return false;
    }
    return true;
}

bool MatchJira(const wstring& host, const std::vector<wstring>& segs, DocIdentity& doc)
{
    for (size_t i = 0; i + 1 < segs.size(); ++i)
    {
        if (EqualsNoCase(segs[i], L"browse") && IsJiraKey(segs[i + 1]))
        {
            doc.kind = L"jira";
            doc.id = L"jira:" + host + L"/" + AsciiUpperCopy(segs[i + 1]);
            return true;
        }
    }
    return false;
}

void ExtractDocIdentity(const wstring& host, const std::vector<wstring>& segs,
                        const std::vector<QueryParam>& params, DocIdentity& doc)
{
    if (MatchYouTube(host, segs, params, doc) || MatchGoogle(host, segs, params, doc) ||
        MatchSharePoint(host, segs, params, doc) || MatchOneDrive(host, params, doc) ||
        MatchGitHub(host, segs, doc) || MatchAzureDevOps(host, segs, doc) || MatchFigma(host, segs, doc))
    {
        return;
    }
    MatchJira(host, segs, doc);
}

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------

enum class UrlKind
{
    Invalid,        // not a URL (user typing, malformed)
    Unsupported,    // a URL whose scheme we do not record (mailto:, ms-settings:, steam://, ...)
    Internal,       // browser-internal or local scheme
    Web             // http, https, ftp
};

struct ParsedUrl
{
    wstring scheme;         // lowercase
    wstring host;           // lowercase; may be empty for internal URLs
    wstring rest;           // internal/unsupported: text after "scheme:" (internal: without query/fragment)
    wstring path;           // web: raw path
    wstring query;          // web: raw query without '?'
    int     port = -1;      // explicit port, -1 if absent
    bool    hasQuery = false;
    bool    viewSource = false;
};

bool IsWebScheme(const wstring& scheme)
{
    return scheme == L"http" || scheme == L"https" || scheme == L"ftp";
}

bool IsInternalScheme(const wstring& scheme)
{
    static const wchar_t* const kInternal[] =
    {
        L"about", L"chrome", L"edge", L"brave", L"vivaldi", L"opera", L"file", L"data", L"blob",
        L"javascript", L"chrome-extension", L"moz-extension", L"chrome-untrusted", L"chrome-search",
        L"devtools", L"resource"
    };
    for (const wchar_t* k : kInternal)
    {
        if (scheme == k)
            return true;
    }
    return false;
}

int DefaultPort(const wstring& scheme)
{
    if (scheme == L"http")
        return 80;
    if (scheme == L"https")
        return 443;
    if (scheme == L"ftp")
        return 21;
    return -1;
}

// RFC 3986 scheme at s[pos] followed by ':'. Parsing the scheme (rather than comparing fixed-length
// prefixes) is what makes "about:" match on exactly its 6 characters.
bool ParseSchemeAt(const wstring& s, size_t pos, wstring& scheme, size_t& afterColon)
{
    if (pos >= s.size() || !IsAsciiAlpha(s[pos]))
        return false;
    size_t i = pos + 1;
    while (i < s.size() && (IsAsciiAlnum(s[i]) || s[i] == L'+' || s[i] == L'-' || s[i] == L'.'))
        ++i;
    if (i >= s.size() || s[i] != L':')
        return false;
    scheme = AsciiLowerCopy(s.substr(pos, i - pos));
    afterColon = i + 1;
    return true;
}

bool IsIPv4(const wstring& h)
{
    int parts = 0;
    size_t start = 0;
    for (size_t k = 0; k <= h.size(); ++k)
    {
        if (k == h.size() || h[k] == L'.')
        {
            const size_t len = k - start;
            if (len == 0 || len > 3)
                return false;
            int value = 0;
            for (size_t m = start; m < k; ++m)
            {
                if (!IsAsciiDigit(h[m]))
                    return false;
                value = value * 10 + (h[m] - L'0');
            }
            if (value > 255)
                return false;
            ++parts;
            start = k + 1;
        }
    }
    return parts == 4;
}

bool IsLocalOrIp(const wstring& host)
{
    return host == L"localhost" || EndsWith(host, L".localhost") || IsIPv4(host) ||
           (!host.empty() && host[0] == L'[');
}

// TLD of a schemeless "bare domain": letters (>= 2), punycode or IDN; common file extensions are not sites.
bool IsPlausibleTld(const wstring& tld)
{
    if (tld.size() < 2)
        return false;
    if (StartsWithNoCase(tld, 0, L"xn--"))
        return tld.size() > 4;
    bool ascii = true;
    for (wchar_t c : tld)
    {
        if (c >= 0x80)
            ascii = false;
        else if (!IsAsciiAlpha(c))
            return false;
    }
    if (!ascii)
        return true;

    static const wchar_t* const kNotTlds[] =
    {
        L"txt", L"log", L"ini", L"cfg", L"json", L"xml", L"csv", L"tsv", L"yaml", L"yml", L"doc", L"docx",
        L"docm", L"xls", L"xlsx", L"xlsm", L"ppt", L"pptx", L"pdf", L"rtf", L"odt", L"exe", L"dll", L"sys",
        L"msi", L"bat", L"cmd", L"vbs", L"cpp", L"hpp", L"cxx", L"java", L"jpg", L"jpeg", L"png", L"gif",
        L"bmp", L"tif", L"tiff", L"webp", L"wav", L"avi", L"mkv", L"rar", L"tar", L"gz", L"iso", L"htm",
        L"html", L"lnk", L"tmp", L"bak", L"dat", L"db", L"sln", L"pdb", L"obj", L"lib"
    };
    for (const wchar_t* e : kNotTlds)
    {
        if (EqualsNoCase(tld, e))
            return false;
    }
    return true;
}

// Schemeless input must look like a site: localhost, an IP literal, a dotted name with a plausible TLD,
// or a single-label host with an explicit port ("devbox:8080").
bool IsPlausibleBareHost(const wstring& host, bool hasExplicitPort)
{
    if (IsLocalOrIp(host))
        return true;
    const size_t dot = host.rfind(L'.');
    if (dot == wstring::npos)
        return hasExplicitPort;
    return IsPlausibleTld(host.substr(dot + 1));
}

bool IsHostChar(wchar_t c)
{
    if (c >= 0x80)
        return !IsInvisibleCp(c);   // IDN is allowed (left as-is); invisible/bidi characters are not
    return IsAsciiAlnum(c) || c == L'-' || c == L'.' || c == L'_';
}

// Parses "[userinfo@]host[:port]". Userinfo is discarded; a schemeless string with userinfo is not a URL
// (it is almost always an e-mail address).
bool ParseAuthority(const wstring& authority, bool allowUserInfo, wstring& host, int& port)
{
    wstring a = authority;
    const size_t at = a.rfind(L'@');
    if (at != wstring::npos)
    {
        if (!allowUserInfo)
            return false;
        a.erase(0, at + 1);
    }

    wstring h;
    wstring portText;
    if (!a.empty() && a[0] == L'[')
    {
        const size_t close = a.find(L']');
        if (close == wstring::npos || close < 2)
            return false;
        for (size_t k = 1; k < close; ++k)
        {
            if (!IsHexDigit(a[k]) && a[k] != L':' && a[k] != L'.')
                return false;
        }
        h = AsciiLowerCopy(a.substr(0, close + 1));
        if (close + 1 < a.size())
        {
            if (a[close + 1] != L':')
                return false;
            portText = a.substr(close + 2);
        }
    }
    else
    {
        const size_t colon = a.find(L':');
        if (colon != wstring::npos)
        {
            portText = a.substr(colon + 1);
            a.resize(colon);
        }
        for (wchar_t c : a)
        {
            if (!IsHostChar(c))
                return false;
        }
        h = LowerHost(a);
        while (!h.empty() && h.back() == L'.')
            h.pop_back();   // FQDN form "example.com."
        if (h.empty() || h[0] == L'.' || h.find(L"..") != wstring::npos)
            return false;
    }

    int value = -1;
    if (!portText.empty())
    {
        value = 0;
        for (wchar_t c : portText)
        {
            if (!IsAsciiDigit(c))
                return false;
            value = value * 10 + (c - L'0');
            if (value > 65535)
                return false;
        }
    }
    host = std::move(h);
    port = value;
    return true;
}

// Parses authority, path and query of a web URL starting at s[pos] (after "scheme://").
bool ParseWebRemainder(const wstring& s, size_t pos, bool allowUserInfo, ParsedUrl& u)
{
    size_t authEnd = s.find_first_of(L"/\\?#", pos);
    if (authEnd == wstring::npos)
        authEnd = s.size();
    if (!ParseAuthority(s.substr(pos, authEnd - pos), allowUserInfo, u.host, u.port))
        return false;

    size_t pathEnd = s.find_first_of(L"?#", authEnd);
    if (pathEnd == wstring::npos)
        pathEnd = s.size();
    u.path = s.substr(authEnd, pathEnd - authEnd);
    if (pathEnd < s.size() && s[pathEnd] == L'?')
    {
        size_t hash = s.find(L'#', pathEnd + 1);
        if (hash == wstring::npos)
            hash = s.size();
        u.query = s.substr(pathEnd + 1, hash - pathEnd - 1);
        u.hasQuery = !u.query.empty();
    }
    return true;
}

// Host of "//authority/..." or "scheme://authority/..." (lowercase, userinfo and port removed).
wstring LooseAuthorityHost(const wstring& rest)
{
    size_t pos = 0;
    if (rest.compare(0, 2, L"//") == 0)
    {
        pos = 2;
    }
    else
    {
        wstring inner;
        size_t afterColon = 0;
        if (!ParseSchemeAt(rest, 0, inner, afterColon) || rest.compare(afterColon, 2, L"//") != 0)
            return wstring();
        pos = afterColon + 2;
    }
    size_t end = rest.find_first_of(L"/\\?#", pos);
    if (end == wstring::npos)
        end = rest.size();
    wstring a = rest.substr(pos, end - pos);
    const size_t at = a.rfind(L'@');
    if (at != wstring::npos)
        a.erase(0, at + 1);
    if (!a.empty() && a[0] == L'[')
    {
        const size_t close = a.find(L']');
        if (close != wstring::npos)
            a.resize(close + 1);
    }
    else
    {
        const size_t colon = a.find(L':');
        if (colon != wstring::npos)
            a.resize(colon);
    }
    return LowerHost(a);
}

// Undoes a historical UrlExtractor bug that prefixed "https://" to non-web URLs ("https://about:blank").
void RepairLegacyPrefix(wstring& s)
{
    size_t skip = 0;
    if (StartsWithNoCase(s, 0, L"https://"))
        skip = 8;
    else if (StartsWithNoCase(s, 0, L"http://"))
        skip = 7;
    else
        return;

    wstring scheme;
    size_t afterColon = 0;
    if (!ParseSchemeAt(s, skip, scheme, afterColon))
        return;
    if (!IsInternalScheme(scheme) && scheme != L"view-source")
        return;
    if (afterColon < s.size() && IsAsciiDigit(s[afterColon]))
        return;   // "https://chrome:9222/" is host:port
    const size_t authEnd = s.find_first_of(L"/\\?#", skip);
    const size_t authLen = ((authEnd == wstring::npos) ? s.size() : authEnd) - skip;
    if (s.find(L'@', skip) < skip + authLen)
        return;   // "https://data:pw@host/" carries userinfo, it is a real web URL
    s.erase(0, skip);
}

UrlKind ParseUrl(const wstring& raw, ParsedUrl& u)
{
    wstring s = TrimUrlText(raw);
    if (s.empty() || HasSpaceOrControl(s))
        return UrlKind::Invalid;

    RepairLegacyPrefix(s);
    while (StartsWithNoCase(s, 0, L"view-source:"))
    {
        s.erase(0, 12);
        u.viewSource = true;
    }
    if (s.empty())
        return UrlKind::Invalid;

    wstring scheme;
    size_t afterColon = 0;
    bool hasScheme = ParseSchemeAt(s, 0, scheme, afterColon);
    if (hasScheme && !IsWebScheme(scheme) && !IsInternalScheme(scheme) &&
        afterColon < s.size() && IsAsciiDigit(s[afterColon]))
    {
        hasScheme = false;   // "localhost:3000/x", "example.com:8080" are host:port, not a scheme
    }

    if (!hasScheme)
    {
        if (!ParseWebRemainder(s, 0, false, u) || !IsPlausibleBareHost(u.host, u.port >= 0))
            return UrlKind::Invalid;
        u.scheme = IsLocalOrIp(u.host) ? L"http" : L"https";
        return UrlKind::Web;
    }

    u.scheme = scheme;
    if (IsWebScheme(scheme))
    {
        // Like browsers, accept any run of '/' or '\' after the scheme ("https:/x", "http:\\x").
        size_t pos = afterColon;
        while (pos < s.size() && (s[pos] == L'/' || s[pos] == L'\\'))
            ++pos;
        return ParseWebRemainder(s, pos, true, u) ? UrlKind::Web : UrlKind::Invalid;
    }

    if (!IsInternalScheme(scheme))
    {
        u.rest = s.substr(afterColon);
        return UrlKind::Unsupported;
    }

    if (scheme == L"data")
    {
        // Media type only: the payload can be large and private.
        size_t k = afterColon;
        while (k < s.size() && (IsAsciiAlnum(s[k]) || s[k] == L'/' || s[k] == L'-' || s[k] == L'+' ||
                                s[k] == L'.' || s[k] == L'_'))
        {
            ++k;
        }
        u.rest = AsciiLowerCopy(s.substr(afterColon, k - afterColon));
    }
    else if (scheme != L"javascript")   // script text is never recorded
    {
        const size_t cut = s.find_first_of(L"?#", afterColon);
        const size_t end = (cut == wstring::npos) ? s.size() : cut;
        u.rest = s.substr(afterColon, end - afterColon);
        if (cut != wstring::npos && s[cut] == L'?')
        {
            const size_t hash = s.find(L'#', cut + 1);
            u.hasQuery = ((hash == wstring::npos) ? s.size() : hash) > cut + 1;
        }
        u.host = LooseAuthorityHost(u.rest);
    }
    return UrlKind::Internal;
}

Result CanonicalizeImpl(const wstring& rawUrl, const std::vector<wstring>& excludedDomains,
                        const std::vector<wstring>& extraAllowedQueryKeys)
{
    Result r;
    ParsedUrl u;
    const UrlKind kind = ParseUrl(rawUrl, u);
    if (kind == UrlKind::Invalid)
        return r;

    r.scheme = u.scheme;
    r.host = u.host;
    r.hadQuery = u.hasQuery;
    if (kind == UrlKind::Unsupported)
        return r;   // nothing we can identify (mailto:, ms-settings:, ...)

    if (kind == UrlKind::Internal)
    {
        r.isInternal = true;
        if (IsExcluded(u.host, wstring(), excludedDomains))
            r.excluded = true;
        else
            r.canonical = u.scheme + L":" + u.rest;
        return r;
    }

    if (u.port == DefaultPort(u.scheme))
        u.port = -1;
    const std::vector<wstring> segs = NormalizePathSegments(u.path);
    const wstring path = JoinPath(segs);
    if (IsExcluded(u.host, path, excludedDomains))
    {
        r.excluded = true;   // fail closed: no canonical URL or document identity is returned
        return r;
    }

    const std::vector<QueryParam> params = ParseQuery(u.query);
    DocIdentity doc;
    ExtractDocIdentity(u.host, segs, params, doc);

    if (!doc.canonical.empty())
    {
        r.canonical = doc.canonical;
        r.scheme = L"https";
        r.host = doc.canonicalHost;
    }
    else
    {
        wstring canonical = u.scheme + L"://" + u.host;
        if (u.port >= 0)
        {
            canonical += L':';
            canonical += std::to_wstring(u.port);
        }
        if (doc.kind.empty())
        {
            // Recognized document sites keep their ids in the path; everything else gets token redaction.
            if (segs.empty())
                canonical += L'/';
            for (const wstring& seg : segs)
            {
                canonical += L'/';
                canonical += RedactSegment(seg);
            }
        }
        else
        {
            canonical += path;
        }
        const wstring query = BuildQuery(u.host, params, extraAllowedQueryKeys);
        if (!query.empty())
        {
            canonical += L'?';
            canonical += query;
        }
        r.canonical = std::move(canonical);
    }

    if (!u.viewSource)
    {
        r.docKind = doc.kind;
        r.docId = doc.id;
    }
    return r;
}
}   // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result Canonicalize(const std::wstring& rawUrl,
                    const std::vector<std::wstring>& excludedDomains,
                    const std::vector<std::wstring>& extraAllowedQueryKeys)
{
    Result result;
    try
    {
        result = CanonicalizeImpl(rawUrl, excludedDomains, extraAllowedQueryKeys);
    }
    catch (const std::bad_alloc&)
    {
        throw;
    }
    catch (...)
    {
        result = Result();
    }
    return result;
}

bool LooksLikeUrl(const std::wstring& s)
{
    bool result = false;
    try
    {
        ParsedUrl u;
        const UrlKind kind = ParseUrl(s, u);
        result = kind == UrlKind::Web || kind == UrlKind::Internal ||
                 (kind == UrlKind::Unsupported && u.rest.compare(0, 2, L"//") == 0);
    }
    catch (const std::bad_alloc&)
    {
        throw;
    }
    catch (...)
    {
        result = false;
    }
    return result;
}

std::wstring HostOf(const std::wstring& url)
{
    std::wstring host;
    try
    {
        ParsedUrl u;
        const UrlKind kind = ParseUrl(url, u);
        if (kind == UrlKind::Web || kind == UrlKind::Internal)
            host = u.host;
    }
    catch (const std::bad_alloc&)
    {
        throw;
    }
    catch (...)
    {
        host.clear();
    }
    return host;
}

std::wstring PercentDecode(const std::wstring& s)
{
    if (s.find(L'%') == std::wstring::npos)
        return s;
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size())
    {
        if (s[i] == L'%')
        {
            const size_t next = AppendEscapeRun(s, i, false, out);
            if (next == i)
            {
                out.push_back(L'%');   // invalid escape kept verbatim
                ++i;
            }
            else
            {
                i = next;
            }
        }
        else
        {
            out.push_back(s[i]);
            ++i;
        }
    }
    return out;
}
}   // namespace UrlCanon
