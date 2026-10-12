#include "framework.h"
#include "Json.h"

#include <cmath>
#include <cstring>
#include <exception>
#include <functional>
#include <locale.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>

#if defined(_MSVC_LANG) && _MSVC_LANG >= 201703L
#include <charconv>
#include <system_error>
#endif

// std::to_chars(double) yields the shortest round-trip form but is C++17-only.
// Older language modes use a locale-independent _snprintf_s_l search instead.
#if defined(_MSVC_LANG) && _MSVC_LANG >= 201703L && defined(__cpp_lib_to_chars)
#define WARPJSON_HAS_TO_CHARS 1
#else
#define WARPJSON_HAS_TO_CHARS 0
#endif

namespace WarpJson
{

// Friend of Value: lets the parser/serializer below touch the representation
// directly (e.g. append object members without the duplicate-key scan of set()).
class ValueAccess
{
public:
    static void SetBool(Value& v, bool b)
    {
        v.m_type = Type::Bool;
        v.m_bool = b;
    }

    static void SetInt(Value& v, int64_t i)
    {
        v.m_type  = Type::Number;
        v.m_isInt = true;
        v.m_int   = i;
    }

    static void SetDouble(Value& v, double d)
    {
        v.m_type   = Type::Number;
        v.m_isInt  = false;
        v.m_double = d;
    }

    static std::string& MakeString(Value& v)
    {
        v.m_type = Type::String;
        return v.m_string;
    }

    static std::vector<Value>& MakeArray(Value& v)
    {
        v.m_type = Type::Array;
        return v.m_items;
    }

    static std::vector<std::pair<std::string, Value>>& MakeObject(Value& v)
    {
        v.m_type = Type::Object;
        return v.m_members;
    }

    static bool    IsExactInt(const Value& v) { return v.m_isInt; }
    static int64_t ExactInt(const Value& v)   { return v.m_int; }
    static double  RawDouble(const Value& v)  { return v.m_double; }
};

namespace
{

// Hard cap on nesting so parser recursion (and the recursive destruction of
// the resulting tree) can never exhaust a thread's stack.
const size_t kMaxDepthLimit = 512;

// Chunk size for the Win32 UTF conversions: keeps every length passed to the
// API (input and worst-case output, <= 3 bytes per UTF-16 unit) inside int.
const size_t kConvertChunk = static_cast<size_t>(1) << 28;

const char kHex[] = "0123456789abcdef";
const char kReplacementChar[] = "\xEF\xBF\xBD";   // U+FFFD encoded as UTF-8

const Value& NullValue()
{
    static const Value s_null;
    return s_null;
}

const std::string& EmptyString()
{
    static const std::string s_empty;
    return s_empty;
}

const std::vector<Value>& EmptyItems()
{
    static const std::vector<Value> s_empty;
    return s_empty;
}

const std::vector<std::pair<std::string, Value>>& EmptyMembers()
{
    static const std::vector<std::pair<std::string, Value>> s_empty;
    return s_empty;
}

// Returns the length (1-4) of the well-formed UTF-8 sequence starting at s[0],
// or 0 if it is ill-formed. In that case *badLen receives the length of the
// maximal ill-formed subpart (>= 1), to be replaced by a single U+FFFD.
size_t Utf8SequenceLength(const unsigned char* s, size_t avail, size_t* badLen)
{
    const unsigned int c = s[0];
    if (c < 0x80)
    {
        return 1;
    }

    size_t need = 0;
    unsigned int lo = 0x80;
    unsigned int hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF)
    {
        need = 1;
    }
    else if (c == 0xE0)
    {
        need = 2;
        lo = 0xA0;   // overlong
    }
    else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF)
    {
        need = 2;
    }
    else if (c == 0xED)
    {
        need = 2;
        hi = 0x9F;   // UTF-16 surrogates
    }
    else if (c == 0xF0)
    {
        need = 3;
        lo = 0x90;   // overlong
    }
    else if (c >= 0xF1 && c <= 0xF3)
    {
        need = 3;
    }
    else if (c == 0xF4)
    {
        need = 3;
        hi = 0x8F;   // above U+10FFFF
    }
    else
    {
        *badLen = 1;   // 0x80..0xC1 and 0xF5..0xFF never start a sequence
        return 0;
    }

    size_t i = 1;
    while (i <= need && i < avail)
    {
        const unsigned int cc = s[i];
        const unsigned int lower = (i == 1) ? lo : 0x80u;
        const unsigned int upper = (i == 1) ? hi : 0xBFu;
        if (cc < lower || cc > upper)
        {
            break;
        }
        ++i;
    }
    if (i == need + 1)
    {
        return i;
    }
    *badLen = i;
    return 0;
}

void AppendUtf8(std::string& out, uint32_t cp)
{
    char buf[4];
    size_t len = 0;
    if (cp < 0x80)
    {
        buf[0] = static_cast<char>(cp);
        len = 1;
    }
    else if (cp < 0x800)
    {
        buf[0] = static_cast<char>(0xC0 | (cp >> 6));
        buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
        len = 2;
    }
    else if (cp < 0x10000)
    {
        buf[0] = static_cast<char>(0xE0 | (cp >> 12));
        buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
        len = 3;
    }
    else
    {
        buf[0] = static_cast<char>(0xF0 | (cp >> 18));
        buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
        len = 4;
    }
    out.append(buf, len);
}

// Appends the JSON-escaped form of [data, data + n) without quotes. Runs of
// bytes that need no escaping are copied in bulk. `data` must not point into
// `out` (see AppendQuotedRaw).
void AppendEscaped(std::string& out, const char* data, size_t n)
{
    const unsigned char* s = reinterpret_cast<const unsigned char*>(data);
    size_t run = 0;   // start of the pending verbatim run
    size_t i = 0;
    while (i < n)
    {
        const unsigned int c = s[i];
        if (c >= 0x20 && c != '"' && c != '\\' && c != 0x7F)
        {
            if (c < 0x80)
            {
                ++i;
                continue;
            }
            size_t bad = 0;
            const size_t len = Utf8SequenceLength(s + i, n - i, &bad);
            if (len != 0)
            {
                i += len;
                continue;
            }
            out.append(data + run, i - run);
            out.append(kReplacementChar, 3);
            i += bad;
            run = i;
            continue;
        }

        out.append(data + run, i - run);
        switch (c)
        {
        case '"':  out.append("\\\"", 2); break;
        case '\\': out.append("\\\\", 2); break;
        case '\b': out.append("\\b", 2);  break;
        case '\f': out.append("\\f", 2);  break;
        case '\n': out.append("\\n", 2);  break;
        case '\r': out.append("\\r", 2);  break;
        case '\t': out.append("\\t", 2);  break;
        default:
        {
            const char esc[6] = { '\\', 'u', '0', '0', kHex[(c >> 4) & 0x0F], kHex[c & 0x0F] };
            out.append(esc, 6);
            break;
        }
        }
        ++i;
        run = i;
    }
    out.append(data + run, n - run);
}

bool PointsInto(const std::string& s, const char* p)
{
    if (s.empty() || p == nullptr)
    {
        return false;
    }
    const char* begin = s.data();
    return !std::less<const char*>()(p, begin) && std::less<const char*>()(p, begin + s.size());
}

void AppendQuotedRaw(std::string& out, const char* p, size_t n)
{
    if (n != 0 && PointsInto(out, p))
    {
        // The source aliases the destination (e.g. AppendQuoted(s, s)) and
        // appending may reallocate it, so escape from a copy.
        const std::string copy(p, n);
        AppendQuotedRaw(out, copy.data(), copy.size());
        return;
    }
    out += '"';
    AppendEscaped(out, p, n);
    out += '"';
}

void AppendUInt64(std::string& out, uint64_t v)
{
    char buf[24];
    char* p = buf + sizeof(buf);
    do
    {
        *--p = static_cast<char>('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    out.append(p, static_cast<size_t>((buf + sizeof(buf)) - p));
}

void AppendInt64(std::string& out, int64_t v)
{
    if (v < 0)
    {
        out += '-';
        // Negate in unsigned arithmetic so INT64_MIN is handled.
        AppendUInt64(out, 0 - static_cast<uint64_t>(v));
    }
    else
    {
        AppendUInt64(out, static_cast<uint64_t>(v));
    }
}

_locale_t CLocale()
{
    // Created once and intentionally never freed so it outlives every caller,
    // including code that runs during static destruction.
    static const _locale_t s_locale = _create_locale(LC_NUMERIC, "C");
    return s_locale;
}

// Converts a validated JSON number literal with "C" locale semantics.
// Returns false if the value overflows a double.
bool ParseDoubleC(const char* p, size_t len, double& out)
{
    char local[128];
    std::string heap;
    const char* s = local;
    if (len < sizeof(local))
    {
        memcpy(local, p, len);
        local[len] = '\0';
    }
    else
    {
        heap.assign(p, len);
        s = heap.c_str();
    }

    char* end = nullptr;
    const double d = _strtod_l(s, &end, CLocale());
    if (end != s + len || !std::isfinite(d))
    {
        return false;
    }
    out = d;
    return true;
}

void AppendDouble(std::string& out, double d)
{
    if (!std::isfinite(d))
    {
        out.append("null", 4);
        return;
    }

#if WARPJSON_HAS_TO_CHARS
    {
        char shortest[64];
        const std::to_chars_result r = std::to_chars(shortest, shortest + sizeof(shortest), d);
        if (r.ec == std::errc())
        {
            out.append(shortest, static_cast<size_t>(r.ptr - shortest));
            return;
        }
    }
#endif

    // Shortest of %.15g / %.16g / %.17g that round-trips (%.17g always does).
    char buf[64];
    for (int precision = 15; precision <= 17; ++precision)
    {
        const int len = _snprintf_s_l(buf, sizeof(buf), _TRUNCATE, "%.*g", CLocale(), precision, d);
        if (len <= 0)
        {
            continue;
        }
        for (int i = 0; i < len; ++i)
        {
            if (buf[i] == ',')
            {
                buf[i] = '.';   // defensive: never emit a locale decimal comma
            }
        }
        double back = 0.0;
        if (precision == 17 || (ParseDoubleC(buf, static_cast<size_t>(len), back) && back == d))
        {
            out.append(buf, static_cast<size_t>(len));
            return;
        }
    }
    out.append("null", 4);
}

const Value* FindLast(const std::vector<std::pair<std::string, Value>>& members, const char* key, size_t len)
{
    for (size_t i = members.size(); i > 0; --i)
    {
        const std::pair<std::string, Value>& m = members[i - 1];
        if (m.first.size() == len && (len == 0 || memcmp(m.first.data(), key, len) == 0))
        {
            return &m.second;
        }
    }
    return nullptr;
}

void SerializeTo(std::string& out, const Value& v)
{
    switch (v.type())
    {
    case Type::Null:
        out.append("null", 4);
        break;
    case Type::Bool:
        if (v.asBool())
        {
            out.append("true", 4);
        }
        else
        {
            out.append("false", 5);
        }
        break;
    case Type::Number:
        if (ValueAccess::IsExactInt(v))
        {
            AppendInt64(out, ValueAccess::ExactInt(v));
        }
        else
        {
            AppendDouble(out, ValueAccess::RawDouble(v));
        }
        break;
    case Type::String:
    {
        const std::string& s = v.asString();
        AppendQuotedRaw(out, s.data(), s.size());
        break;
    }
    case Type::Array:
    {
        const std::vector<Value>& items = v.items();
        out += '[';
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i != 0)
            {
                out += ',';
            }
            SerializeTo(out, items[i]);
        }
        out += ']';
        break;
    }
    case Type::Object:
    {
        const std::vector<std::pair<std::string, Value>>& members = v.members();
        out += '{';
        for (size_t i = 0; i < members.size(); ++i)
        {
            if (i != 0)
            {
                out += ',';
            }
            AppendQuotedRaw(out, members[i].first.data(), members[i].first.size());
            out += ':';
            SerializeTo(out, members[i].second);
        }
        out += '}';
        break;
    }
    }
}

bool IsDigit(char c)
{
    return c >= '0' && c <= '9';
}

// Converts the digits of an integer literal (no sign, no leading zeros) to an
// exact int64. Returns false if the value does not fit.
bool ParseExactInt(const char* digits, size_t count, bool negative, int64_t& out)
{
    const uint64_t limit = negative ? static_cast<uint64_t>(INT64_MAX) + 1 : static_cast<uint64_t>(INT64_MAX);
    uint64_t mag = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const uint64_t digit = static_cast<uint64_t>(digits[i] - '0');
        if (mag > (limit - digit) / 10)
        {
            return false;
        }
        mag = mag * 10 + digit;
    }

    if (!negative)
    {
        out = static_cast<int64_t>(mag);
    }
    else if (mag == static_cast<uint64_t>(INT64_MAX) + 1)
    {
        out = INT64_MIN;
    }
    else
    {
        out = -static_cast<int64_t>(mag);
    }
    return true;
}

// Single-pass recursive-descent parser. Values are built in place inside
// their parent container; recursion depth is bounded by m_maxDepth.
class Parser
{
public:
    Parser(const std::string& text, size_t maxDepth)
        : m_text(text.data())
        , m_size(text.size())
        , m_maxDepth(maxDepth < kMaxDepthLimit ? maxDepth : kMaxDepthLimit)
    {
    }

    bool ParseDocument(Value& out)
    {
        if (!ParseValue(out, 0))
        {
            return false;
        }
        SkipWhitespace();
        if (m_pos != m_size)
        {
            return FailUnexpected("end of input");
        }
        return true;
    }

    const std::string& Error() const { return m_error; }
    size_t ErrorOffset() const { return m_errorOffset; }

private:
    const char* m_text;
    size_t      m_size;
    size_t      m_pos = 0;
    size_t      m_maxDepth;
    std::string m_error;
    size_t      m_errorOffset = 0;

    bool Fail(const char* message, size_t offset)
    {
        m_error = message;
        m_errorOffset = offset;
        return false;
    }

    // "unexpected character 'x'" / "unexpected byte 0x01" / "unexpected end of
    // input", optionally followed by what was expected instead.
    bool FailUnexpected(const char* expected)
    {
        std::string msg;
        if (m_pos >= m_size)
        {
            msg = "unexpected end of input";
        }
        else
        {
            const unsigned int c = static_cast<unsigned char>(m_text[m_pos]);
            if (c >= 0x20 && c < 0x7F)
            {
                msg = "unexpected character '";
                msg += static_cast<char>(c);
                msg += '\'';
            }
            else
            {
                msg = "unexpected byte 0x";
                msg += kHex[(c >> 4) & 0x0F];
                msg += kHex[c & 0x0F];
            }
        }
        if (expected != nullptr)
        {
            msg += ", expected ";
            msg += expected;
        }
        m_error.swap(msg);
        m_errorOffset = m_pos;
        return false;
    }

    void SkipWhitespace()
    {
        while (m_pos < m_size)
        {
            const char c = m_text[m_pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            {
                return;
            }
            ++m_pos;
        }
    }

    // `out` is a fresh null Value; `depth` is the nesting level of the
    // container holding it (0 for the document root).
    bool ParseValue(Value& out, size_t depth)
    {
        SkipWhitespace();
        if (m_pos >= m_size)
        {
            return FailUnexpected(nullptr);
        }

        const char c = m_text[m_pos];
        switch (c)
        {
        case '{':
            return ParseObject(out, depth + 1);
        case '[':
            return ParseArray(out, depth + 1);
        case '"':
            return ParseString(ValueAccess::MakeString(out));
        case 't':
            if (!ParseLiteral("true", 4))
            {
                return false;
            }
            ValueAccess::SetBool(out, true);
            return true;
        case 'f':
            if (!ParseLiteral("false", 5))
            {
                return false;
            }
            ValueAccess::SetBool(out, false);
            return true;
        case 'n':
            return ParseLiteral("null", 4);
        default:
            break;
        }

        if (c == '-' || IsDigit(c))
        {
            return ParseNumber(out);
        }
        return FailUnexpected(nullptr);
    }

    bool ParseLiteral(const char* literal, size_t len)
    {
        if (m_size - m_pos < len || memcmp(m_text + m_pos, literal, len) != 0)
        {
            return Fail("invalid literal", m_pos);
        }
        m_pos += len;
        return true;
    }

    bool ParseNumber(Value& out)
    {
        const size_t start = m_pos;
        bool negative = false;
        if (m_text[m_pos] == '-')
        {
            negative = true;
            ++m_pos;
        }
        if (m_pos >= m_size || !IsDigit(m_text[m_pos]))
        {
            return Fail("invalid number", start);
        }

        const size_t digitsBegin = m_pos;
        if (m_text[m_pos] == '0')
        {
            ++m_pos;
            if (m_pos < m_size && IsDigit(m_text[m_pos]))
            {
                return Fail("invalid number: leading zeros are not allowed", start);
            }
        }
        else
        {
            while (m_pos < m_size && IsDigit(m_text[m_pos]))
            {
                ++m_pos;
            }
        }
        const size_t digitsEnd = m_pos;

        bool integral = true;
        if (m_pos < m_size && m_text[m_pos] == '.')
        {
            integral = false;
            ++m_pos;
            if (m_pos >= m_size || !IsDigit(m_text[m_pos]))
            {
                return Fail("invalid number", start);
            }
            while (m_pos < m_size && IsDigit(m_text[m_pos]))
            {
                ++m_pos;
            }
        }
        if (m_pos < m_size && (m_text[m_pos] == 'e' || m_text[m_pos] == 'E'))
        {
            integral = false;
            ++m_pos;
            if (m_pos < m_size && (m_text[m_pos] == '+' || m_text[m_pos] == '-'))
            {
                ++m_pos;
            }
            if (m_pos >= m_size || !IsDigit(m_text[m_pos]))
            {
                return Fail("invalid number", start);
            }
            while (m_pos < m_size && IsDigit(m_text[m_pos]))
            {
                ++m_pos;
            }
        }

        if (integral)
        {
            int64_t exact = 0;
            if (ParseExactInt(m_text + digitsBegin, digitsEnd - digitsBegin, negative, exact))
            {
                ValueAccess::SetInt(out, exact);
                return true;
            }
            // Too large for int64: fall through and keep it as a double.
        }

        double d = 0.0;
        if (!ParseDoubleC(m_text + start, m_pos - start, d))
        {
            return Fail("number out of range", start);
        }
        ValueAccess::SetDouble(out, d);
        return true;
    }

    bool ReadHex4(size_t at, uint32_t& value) const
    {
        if (at > m_size || m_size - at < 4)
        {
            return false;
        }
        uint32_t v = 0;
        for (size_t i = 0; i < 4; ++i)
        {
            const char c = m_text[at + i];
            uint32_t digit = 0;
            if (c >= '0' && c <= '9')
            {
                digit = static_cast<uint32_t>(c - '0');
            }
            else if (c >= 'a' && c <= 'f')
            {
                digit = static_cast<uint32_t>(c - 'a' + 10);
            }
            else if (c >= 'A' && c <= 'F')
            {
                digit = static_cast<uint32_t>(c - 'A' + 10);
            }
            else
            {
                return false;
            }
            v = (v << 4) | digit;
        }
        value = v;
        return true;
    }

    // m_pos is at a backslash inside a string.
    bool ParseEscape(std::string& out)
    {
        const size_t start = m_pos;
        if (m_size - m_pos < 2)
        {
            return Fail("unterminated string", start);
        }
        const char e = m_text[m_pos + 1];
        m_pos += 2;
        switch (e)
        {
        case '"':  out += '"';  return true;
        case '\\': out += '\\'; return true;
        case '/':  out += '/';  return true;
        case 'b':  out += '\b'; return true;
        case 'f':  out += '\f'; return true;
        case 'n':  out += '\n'; return true;
        case 'r':  out += '\r'; return true;
        case 't':  out += '\t'; return true;
        case 'u':  break;
        default:   return Fail("invalid escape sequence", start);
        }

        uint32_t cp = 0;
        if (!ReadHex4(m_pos, cp))
        {
            return Fail("invalid \\u escape", start);
        }
        m_pos += 4;

        if (cp >= 0xD800 && cp <= 0xDBFF)
        {
            // High surrogate: only valid when a low-surrogate escape follows.
            uint32_t low = 0;
            if (m_size - m_pos >= 6 && m_text[m_pos] == '\\' && m_text[m_pos + 1] == 'u'
                && ReadHex4(m_pos + 2, low) && low >= 0xDC00 && low <= 0xDFFF)
            {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                m_pos += 6;
            }
            else
            {
                cp = 0xFFFD;
            }
        }
        else if (cp >= 0xDC00 && cp <= 0xDFFF)
        {
            cp = 0xFFFD;   // unpaired low surrogate
        }
        AppendUtf8(out, cp);
        return true;
    }

    // m_pos is at the opening quote.
    bool ParseString(std::string& out)
    {
        const size_t open = m_pos;
        ++m_pos;
        size_t run = m_pos;   // start of the pending verbatim run
        for (;;)
        {
            if (m_pos >= m_size)
            {
                return Fail("unterminated string", open);
            }
            const unsigned int c = static_cast<unsigned char>(m_text[m_pos]);
            if (c == '"')
            {
                out.append(m_text + run, m_pos - run);
                ++m_pos;
                return true;
            }
            if (c == '\\')
            {
                out.append(m_text + run, m_pos - run);
                if (!ParseEscape(out))
                {
                    return false;
                }
                run = m_pos;
            }
            else if (c < 0x20)
            {
                return Fail("unescaped control character in string", m_pos);
            }
            else if (c < 0x80)
            {
                ++m_pos;
            }
            else
            {
                size_t bad = 0;
                const size_t len = Utf8SequenceLength(
                    reinterpret_cast<const unsigned char*>(m_text) + m_pos, m_size - m_pos, &bad);
                if (len == 0)
                {
                    return Fail("invalid UTF-8", m_pos);
                }
                m_pos += len;
            }
        }
    }

    // m_pos is at '['; `depth` is the nesting level of this array.
    bool ParseArray(Value& out, size_t depth)
    {
        if (depth > m_maxDepth)
        {
            return Fail("nesting too deep", m_pos);
        }
        std::vector<Value>& items = ValueAccess::MakeArray(out);
        ++m_pos;
        SkipWhitespace();
        if (m_pos < m_size && m_text[m_pos] == ']')
        {
            ++m_pos;
            return true;
        }
        for (;;)
        {
            items.emplace_back();
            if (!ParseValue(items.back(), depth))
            {
                return false;
            }
            SkipWhitespace();
            if (m_pos < m_size && m_text[m_pos] == ',')
            {
                ++m_pos;
            }
            else if (m_pos < m_size && m_text[m_pos] == ']')
            {
                ++m_pos;
                return true;
            }
            else
            {
                return FailUnexpected("',' or ']'");
            }
        }
    }

    // m_pos is at '{'; `depth` is the nesting level of this object. Members
    // are appended without a duplicate scan (keeps parsing linear); get()
    // resolves duplicates as "last one wins".
    bool ParseObject(Value& out, size_t depth)
    {
        if (depth > m_maxDepth)
        {
            return Fail("nesting too deep", m_pos);
        }
        std::vector<std::pair<std::string, Value>>& members = ValueAccess::MakeObject(out);
        ++m_pos;
        SkipWhitespace();
        if (m_pos < m_size && m_text[m_pos] == '}')
        {
            ++m_pos;
            return true;
        }
        for (;;)
        {
            SkipWhitespace();
            if (m_pos >= m_size || m_text[m_pos] != '"')
            {
                return FailUnexpected("string key");
            }
            members.emplace_back();
            std::pair<std::string, Value>& member = members.back();
            if (!ParseString(member.first))
            {
                return false;
            }
            SkipWhitespace();
            if (m_pos >= m_size || m_text[m_pos] != ':')
            {
                return FailUnexpected("':'");
            }
            ++m_pos;
            if (!ParseValue(member.second, depth))
            {
                return false;
            }
            SkipWhitespace();
            if (m_pos < m_size && m_text[m_pos] == ',')
            {
                ++m_pos;
            }
            else if (m_pos < m_size && m_text[m_pos] == '}')
            {
                ++m_pos;
                return true;
            }
            else
            {
                return FailUnexpected("',' or '}'");
            }
        }
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Value
// ---------------------------------------------------------------------------

Value::Value()
{
}

Value Value::Null()
{
    return Value();
}

Value Value::Bool(bool b)
{
    Value v;
    v.m_type = Type::Bool;
    v.m_bool = b;
    return v;
}

Value Value::Number(double d)
{
    Value v;
    v.m_type   = Type::Number;
    v.m_double = d;
    return v;
}

Value Value::Int(int64_t i)
{
    Value v;
    v.m_type  = Type::Number;
    v.m_isInt = true;
    v.m_int   = i;
    return v;
}

Value Value::String(std::string utf8)
{
    Value v;
    v.m_type   = Type::String;
    v.m_string = std::move(utf8);
    return v;
}

Value Value::Array()
{
    Value v;
    v.m_type = Type::Array;
    return v;
}

Value Value::Object()
{
    Value v;
    v.m_type = Type::Object;
    return v;
}

Value& Value::operator=(const Value& other)
{
    // Copy first: `other` may live inside *this (e.g. v = v["child"]), and a
    // member-wise copy would destroy it while it is still being read.
    if (this != &other)
    {
        Value copy(other);
        *this = std::move(copy);
    }
    return *this;
}

Type Value::type() const
{
    return m_type;
}

bool Value::isNull() const
{
    return m_type == Type::Null;
}

bool Value::isBool() const
{
    return m_type == Type::Bool;
}

bool Value::isNumber() const
{
    return m_type == Type::Number;
}

bool Value::isString() const
{
    return m_type == Type::String;
}

bool Value::isArray() const
{
    return m_type == Type::Array;
}

bool Value::isObject() const
{
    return m_type == Type::Object;
}

bool Value::isInteger() const
{
    if (m_type != Type::Number)
    {
        return false;
    }
    if (m_isInt)
    {
        return true;
    }
    const double d = m_double;   // NaN and +/-Inf fail the range test
    return d >= -9223372036854775808.0 && d < 9223372036854775808.0 && std::floor(d) == d;
}

bool Value::asBool(bool def) const
{
    return m_type == Type::Bool ? m_bool : def;
}

double Value::asDouble(double def) const
{
    if (m_type != Type::Number)
    {
        return def;
    }
    return m_isInt ? static_cast<double>(m_int) : m_double;
}

int64_t Value::asInt64(int64_t def) const
{
    if (m_type != Type::Number)
    {
        return def;
    }
    if (m_isInt)
    {
        return m_int;
    }
    const double d = m_double;
    if (std::isnan(d))
    {
        return def;
    }
    if (d >= 9223372036854775808.0)
    {
        return INT64_MAX;
    }
    if (d <= -9223372036854775808.0)
    {
        return INT64_MIN;
    }
    return static_cast<int64_t>(d);   // truncates toward zero
}

const std::string& Value::asString() const
{
    return m_type == Type::String ? m_string : EmptyString();
}

std::string Value::asString(const std::string& def) const
{
    return m_type == Type::String ? m_string : def;
}

size_t Value::size() const
{
    if (m_type == Type::Array)
    {
        return m_items.size();
    }
    if (m_type == Type::Object)
    {
        return m_members.size();
    }
    return 0;
}

const Value& Value::at(size_t i) const
{
    if (m_type == Type::Array && i < m_items.size())
    {
        return m_items[i];
    }
    return NullValue();
}

void Value::push(Value v)
{
    if (m_type == Type::Null)
    {
        m_type = Type::Array;
    }
    if (m_type != Type::Array)
    {
        return;
    }
    m_items.push_back(std::move(v));
}

const std::vector<Value>& Value::items() const
{
    return m_type == Type::Array ? m_items : EmptyItems();
}

bool Value::has(const std::string& key) const
{
    return m_type == Type::Object && FindLast(m_members, key.data(), key.size()) != nullptr;
}

const Value& Value::get(const std::string& key) const
{
    if (m_type != Type::Object)
    {
        return NullValue();
    }
    const Value* found = FindLast(m_members, key.data(), key.size());
    return found != nullptr ? *found : NullValue();
}

const Value& Value::operator[](const char* key) const
{
    if (key == nullptr || m_type != Type::Object)
    {
        return NullValue();
    }
    const Value* found = FindLast(m_members, key, strlen(key));
    return found != nullptr ? *found : NullValue();
}

void Value::set(const std::string& key, Value v)
{
    if (m_type == Type::Null)
    {
        m_type = Type::Object;
    }
    if (m_type != Type::Object)
    {
        return;
    }
    // Replace the last occurrence so get() observes the new value even if
    // the object came from a parse with duplicate keys.
    for (size_t i = m_members.size(); i > 0; --i)
    {
        if (m_members[i - 1].first == key)
        {
            m_members[i - 1].second = std::move(v);
            return;
        }
    }
    m_members.emplace_back(key, std::move(v));
}

const std::vector<std::pair<std::string, Value>>& Value::members() const
{
    return m_type == Type::Object ? m_members : EmptyMembers();
}

std::vector<std::string> Value::asStringArray() const
{
    std::vector<std::string> out;
    if (m_type != Type::Array)
    {
        return out;
    }
    out.reserve(m_items.size());
    for (size_t i = 0; i < m_items.size(); ++i)
    {
        if (m_items[i].m_type == Type::String)
        {
            out.push_back(m_items[i].m_string);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

ParseResult Parse(const std::string& utf8Text, size_t maxDepth)
{
    ParseResult result;
    try
    {
        Parser parser(utf8Text, maxDepth);
        if (parser.ParseDocument(result.value))
        {
            result.ok = true;
        }
        else
        {
            result.value = Value();
            result.error = parser.Error();
            result.errorOffset = parser.ErrorOffset();
        }
    }
    catch (const std::bad_alloc&)
    {
        throw;
    }
    catch (const std::exception&)
    {
        // Defensive only (e.g. std::length_error); the API never throws.
        result.ok = false;
        result.value = Value();
        result.error = "internal error";
        result.errorOffset = 0;
    }
    return result;
}

std::string Serialize(const Value& v)
{
    std::string out;
    SerializeTo(out, v);
    return out;
}

void AppendQuoted(std::string& out, const std::string& utf8)
{
    AppendQuotedRaw(out, utf8.data(), utf8.size());
}

std::string Escape(const std::string& utf8)
{
    std::string out;
    out.reserve(utf8.size());
    AppendEscaped(out, utf8.data(), utf8.size());
    return out;
}

std::string WideToUtf8(const std::wstring& w)
{
    std::string out;
    const size_t total = w.size();
    size_t pos = 0;
    while (pos < total)
    {
        size_t n = total - pos;
        if (n > kConvertChunk)
        {
            n = kConvertChunk;
            // Do not split a surrogate pair across chunks.
            const wchar_t last = w[pos + n - 1];
            if (last >= 0xD800 && last <= 0xDBFF)
            {
                --n;
            }
        }
        // Flags 0: unpaired surrogates become U+FFFD instead of failing.
        const int srcLen = static_cast<int>(n);
        const int needed = WideCharToMultiByte(CP_UTF8, 0, w.data() + pos, srcLen, nullptr, 0, nullptr, nullptr);
        if (needed > 0)
        {
            const size_t oldSize = out.size();
            out.resize(oldSize + static_cast<size_t>(needed));
            const int written = WideCharToMultiByte(CP_UTF8, 0, w.data() + pos, srcLen,
                                                    &out[oldSize], needed, nullptr, nullptr);
            out.resize(oldSize + static_cast<size_t>(written > 0 ? written : 0));
        }
        pos += n;
    }
    return out;
}

std::wstring Utf8ToWide(const std::string& s)
{
    std::wstring out;
    const size_t total = s.size();
    size_t pos = 0;
    while (pos < total)
    {
        size_t n = total - pos;
        if (n > kConvertChunk)
        {
            n = kConvertChunk;
            // Move the boundary back onto a lead byte so no sequence is split.
            size_t back = 0;
            while (back < 3 && (static_cast<unsigned char>(s[pos + n - back]) & 0xC0) == 0x80)
            {
                ++back;
            }
            n -= back;
        }
        // Flags 0: invalid sequences become U+FFFD instead of failing.
        const int srcLen = static_cast<int>(n);
        const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data() + pos, srcLen, nullptr, 0);
        if (needed > 0)
        {
            const size_t oldSize = out.size();
            out.resize(oldSize + static_cast<size_t>(needed));
            const int written = MultiByteToWideChar(CP_UTF8, 0, s.data() + pos, srcLen,
                                                    &out[oldSize], needed);
            out.resize(oldSize + static_cast<size_t>(written > 0 ? written : 0));
        }
        pos += n;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Writer
//
// Inside Writer members the unqualified name `Value` is the member function
// Writer::Value, so the DOM type is always spelled WarpJson::Value here.
// ---------------------------------------------------------------------------

Writer::Writer(std::string& out)
    : m_out(out)
{
}

void Writer::Sep()
{
    if (m_afterKey)
    {
        m_afterKey = false;   // this value completes the pending "key":
        return;
    }
    if (m_needComma.empty())
    {
        return;               // top-level value
    }
    if (m_needComma.back())
    {
        m_out += ',';
    }
    m_needComma.back() = true;
    if (m_isObject.back())
    {
        m_out.append("\"\":", 3);   // value without Key(): keep the object well-formed
    }
}

void Writer::Close()
{
    if (m_needComma.empty())
    {
        return;
    }
    if (m_afterKey)
    {
        m_out.append("null", 4);    // Key() that never received a value
        m_afterKey = false;
    }
    m_out += (m_isObject.back() ? '}' : ']');
    m_needComma.pop_back();
    m_isObject.pop_back();
}

Writer& Writer::BeginObject()
{
    Sep();
    m_out += '{';
    m_needComma.push_back(false);
    m_isObject.push_back(true);
    return *this;
}

Writer& Writer::EndObject()
{
    Close();
    return *this;
}

Writer& Writer::BeginArray()
{
    Sep();
    m_out += '[';
    m_needComma.push_back(false);
    m_isObject.push_back(false);
    return *this;
}

Writer& Writer::EndArray()
{
    Close();
    return *this;
}

Writer& Writer::Key(const std::string& k)
{
    if (m_isObject.empty() || !m_isObject.back())
    {
        return *this;   // keys are only meaningful directly inside an object
    }
    if (m_afterKey)
    {
        m_out.append("null", 4);    // previous Key() never received a value
        m_afterKey = false;
    }
    if (m_needComma.back())
    {
        m_out += ',';
    }
    m_needComma.back() = true;
    AppendQuotedRaw(m_out, k.data(), k.size());
    m_out += ':';
    m_afterKey = true;
    return *this;
}

Writer& Writer::String(const std::string& utf8)
{
    Sep();
    AppendQuotedRaw(m_out, utf8.data(), utf8.size());
    return *this;
}

Writer& Writer::String(const std::wstring& w)
{
    const std::string utf8 = WideToUtf8(w);
    Sep();
    AppendQuotedRaw(m_out, utf8.data(), utf8.size());
    return *this;
}

Writer& Writer::Int(int64_t v)
{
    Sep();
    AppendInt64(m_out, v);
    return *this;
}

Writer& Writer::UInt(uint64_t v)
{
    Sep();
    AppendUInt64(m_out, v);
    return *this;
}

Writer& Writer::Double(double v)
{
    Sep();
    AppendDouble(m_out, v);
    return *this;
}

Writer& Writer::Bool(bool v)
{
    Sep();
    if (v)
    {
        m_out.append("true", 4);
    }
    else
    {
        m_out.append("false", 5);
    }
    return *this;
}

Writer& Writer::Null()
{
    Sep();
    m_out.append("null", 4);
    return *this;
}

Writer& Writer::Raw(const std::string& validJsonFragment)
{
    Sep();
    if (validJsonFragment.empty())
    {
        m_out.append("null", 4);    // an empty fragment would break the syntax
    }
    else
    {
        m_out.append(validJsonFragment);
    }
    return *this;
}

Writer& Writer::Value(const WarpJson::Value& v)
{
    Sep();
    SerializeTo(m_out, v);
    return *this;
}

Writer& Writer::KV(const std::string& k, const std::string& v)
{
    Key(k);
    return String(v);
}

Writer& Writer::KV(const std::string& k, const char* v)
{
    Key(k);
    if (v == nullptr)
    {
        return Null();
    }
    Sep();
    AppendQuotedRaw(m_out, v, strlen(v));
    return *this;
}

Writer& Writer::KV(const std::string& k, const std::wstring& v)
{
    Key(k);
    return String(v);
}

Writer& Writer::KV(const std::string& k, const wchar_t* v)
{
    Key(k);
    if (v == nullptr)
    {
        return Null();
    }
    return String(std::wstring(v));
}

Writer& Writer::KV(const std::string& k, int64_t v)
{
    Key(k);
    return Int(v);
}

Writer& Writer::KV(const std::string& k, int v)
{
    Key(k);
    return Int(v);
}

Writer& Writer::KV(const std::string& k, long v)
{
    Key(k);
    return Int(v);
}

Writer& Writer::KV(const std::string& k, uint32_t v)
{
    Key(k);
    return UInt(v);
}

Writer& Writer::KV(const std::string& k, unsigned long v)
{
    Key(k);
    return UInt(v);
}

Writer& Writer::KV(const std::string& k, uint64_t v)
{
    Key(k);
    return UInt(v);
}

Writer& Writer::KV(const std::string& k, double v)
{
    Key(k);
    return Double(v);
}

Writer& Writer::KV(const std::string& k, bool v)
{
    Key(k);
    return Bool(v);
}

Writer& Writer::KNull(const std::string& k)
{
    Key(k);
    return Null();
}

} // namespace WarpJson
