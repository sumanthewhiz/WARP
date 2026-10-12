// ---------------------------------------------------------------------------
// Json.h -- small, strict, safe JSON support for WARP (namespace WarpJson).
//
//   Value   : DOM node (null / bool / number / string / array / object).
//             Objects keep insertion order. Duplicate keys produced by
//             Parse() are all kept in members(); get() returns the LAST one.
//   Parse   : strict RFC 8259 parser. Rejects comments, trailing commas,
//             single quotes, NaN/Infinity, leading zeros, '+' signs, BOMs and
//             trailing garbage. Raw input must be valid UTF-8; \uXXXX escapes
//             are decoded to UTF-8 and unpaired surrogates become U+FFFD.
//             Integer literals that fit in int64 are kept exact.
//   Serialize / Escape / AppendQuoted / Writer
//           : always produce valid UTF-8 JSON. Invalid UTF-8 inside strings is
//             replaced by U+FFFD and NaN/Inf numbers are written as null.
//             Number parsing/formatting never depends on the CRT locale.
//
// Nothing in this module throws except std::bad_alloc. A Value may be read
// from several threads at once; mutation needs external synchronization.
// ---------------------------------------------------------------------------

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace WarpJson
{

enum class Type { Null, Bool, Number, String, Array, Object };

class ValueAccess;   // Json.cpp internal: direct access for the parser/serializer

class Value
{
public:
    Value();                                   // null
    static Value Null();
    static Value Bool(bool b);
    static Value Number(double d);
    static Value Int(int64_t i);               // exact integer number
    static Value String(std::string utf8);
    static Value Array();
    static Value Object();

    Value(const Value&) = default;
    Value(Value&&) = default;
    Value& operator=(const Value& other);      // safe even if `other` is a sub-value of *this
    Value& operator=(Value&&) = default;
    ~Value() = default;

    Type type() const;
    bool isNull() const;
    bool isBool() const;
    bool isNumber() const;
    bool isString() const;
    bool isArray() const;
    bool isObject() const;
    bool isInteger() const;                    // number that is integral and fits int64

    bool        asBool(bool def = false) const;          // def if not bool
    double      asDouble(double def = 0.0) const;        // def if not number
    int64_t     asInt64(int64_t def = 0) const;          // def if not number; doubles truncated toward zero and clamped to int64 range; NaN -> def
    const std::string& asString() const;                 // reference to empty static string if not string
    std::string asString(const std::string& def) const;  // def if not string

    // Arrays and objects
    size_t size() const;                                 // element/member count; 0 otherwise
    const Value& at(size_t i) const;                     // static null Value if out of range / not array
    // Any integral index type, so that v[0] is not ambiguous with operator[](const char*).
    template <typename Index, typename std::enable_if<std::is_integral<Index>::value, int>::type = 0>
    const Value& operator[](Index i) const { return at(static_cast<size_t>(i)); }
    void push(Value v);                                  // converts a null Value into an array first; no-op on other non-arrays
    const std::vector<Value>& items() const;             // static empty vector if not array

    bool has(const std::string& key) const;              // object member exists
    const Value& get(const std::string& key) const;      // static null Value if missing / not object (last occurrence wins for duplicate keys)
    const Value& operator[](const std::string& key) const { return get(key); }
    const Value& operator[](const char* key) const;      // nullptr -> static null Value
    void set(const std::string& key, Value v);           // converts a null Value into an object first; replaces existing key in place; no-op on other non-objects
    const std::vector<std::pair<std::string, Value>>& members() const; // insertion-ordered; static empty if not object

    // Convenience: array of strings (non-string elements skipped); empty if not array.
    std::vector<std::string> asStringArray() const;

private:
    friend class ValueAccess;

    // Plain tagged representation. The recursive containers are valid because
    // std::vector supports incomplete element types (C++17 [vector.overview],
    // implemented by MSVC in every language mode) and the class body never
    // needs std::pair<std::string, Value> to be complete; it is only
    // instantiated by member function bodies, after Value is complete.
    Type        m_type   = Type::Null;
    bool        m_bool   = false;
    bool        m_isInt  = false;   // number is held exactly in m_int
    double      m_double = 0.0;
    int64_t     m_int    = 0;
    std::string m_string;
    std::vector<Value> m_items;
    std::vector<std::pair<std::string, Value>> m_members;
};

struct ParseResult
{
    bool        ok = false;
    Value       value;
    std::string error;        // human-readable, e.g. "unexpected character 'x'"
    size_t      errorOffset = 0;
};

// maxDepth limits array/object nesting; it is clamped to 512 so recursion can
// never exhaust the stack.
ParseResult Parse(const std::string& utf8Text, size_t maxDepth = 64);

std::string Serialize(const Value& v);                         // compact JSON, member order preserved
void        AppendQuoted(std::string& out, const std::string& utf8);   // appends "..." escaped
std::string Escape(const std::string& utf8);                   // escaped content WITHOUT surrounding quotes

std::string  WideToUtf8(const std::wstring& w);                // invalid UTF-16 -> U+FFFD
std::wstring Utf8ToWide(const std::string& s);                 // invalid UTF-8 -> U+FFFD

// Streaming builder that always emits valid JSON when used correctly.
// Misuse never crashes and keeps the output well-formed where possible:
// End*() with nothing open is a no-op and otherwise closes the innermost open
// container, a Key() left without a value gets null, Key() outside an object
// is ignored, and a value written directly in an object without Key() gets
// an empty key. Several top-level values may be written back to back (e.g.
// NDJSON); separating them is the caller's job.
class Writer
{
public:
    explicit Writer(std::string& out);
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    Writer& BeginObject();
    Writer& EndObject();
    Writer& BeginArray();
    Writer& EndArray();
    Writer& Key(const std::string& k);
    Writer& String(const std::string& utf8);
    Writer& String(const std::wstring& w);       // converted to UTF-8
    Writer& Int(int64_t v);
    Writer& UInt(uint64_t v);
    Writer& Double(double v);                    // NaN/Inf written as null
    Writer& Bool(bool v);
    Writer& Null();
    Writer& Raw(const std::string& validJsonFragment); // inserted verbatim as one value; empty -> null
    Writer& Value(const WarpJson::Value& v);
    // key + value helpers
    Writer& KV(const std::string& k, const std::string& v);
    Writer& KV(const std::string& k, const char* v);           // nullptr -> null
    Writer& KV(const std::string& k, const std::wstring& v);
    Writer& KV(const std::string& k, const wchar_t* v);        // nullptr -> null
    Writer& KV(const std::string& k, int64_t v);
    Writer& KV(const std::string& k, int v);
    Writer& KV(const std::string& k, long v);                  // LONG
    Writer& KV(const std::string& k, uint32_t v);
    Writer& KV(const std::string& k, unsigned long v);         // DWORD
    Writer& KV(const std::string& k, uint64_t v);
    Writer& KV(const std::string& k, double v);
    Writer& KV(const std::string& k, bool v);
    Writer& KNull(const std::string& k);

private:
    std::string&      m_out;
    std::vector<bool> m_needComma;   // one entry per open container
    std::vector<bool> m_isObject;    // parallel to m_needComma
    bool              m_afterKey = false;
    void Sep();                      // emit comma if needed (not after a key)
    void Close();                    // close the innermost open container, if any
};

} // namespace WarpJson
