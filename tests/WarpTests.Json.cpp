// JSON protocol tests (gap analysis API-4 / BUG-7): strict parsing, exact integers, escapes,
// invalid UTF-8, depth limits and a writer that can never emit invalid JSON.
#include "WarpTests.h"
#include "Json.h"

#include <limits>

using namespace WarpJson;

TEST(Json, ParsesStrictDocuments)
{
    ParseResult r = Parse("{\"a\":1,\"b\":[true,false,null],\"c\":\"x\",\"d\":-2.5e3}");
    REQUIRE(r.ok);
    CHECK(r.value.isObject());
    CHECK_EQ(r.value["a"].asInt64(), (int64_t)1);
    CHECK_EQ(r.value["b"].size(), (size_t)3);
    CHECK(r.value["b"][0].asBool());
    CHECK(!r.value["b"][1].asBool(true));
    CHECK(r.value["b"][2].isNull());
    CHECK_EQ(r.value["c"].asString(), std::string("x"));
    CHECK_NEAR(r.value["d"].asDouble(), -2500.0, 1e-9);
    CHECK(r.value["missing"].isNull());
    CHECK_EQ(r.value["missing"].asString(std::string("def")), std::string("def"));
}

TEST(Json, RejectsNonStrictInput)
{
    const char* const bad[] = {
        "{\"a\":1,}", "[1,2,]", "{'a':1}", "[01]", "[+1]", "[NaN]", "[Infinity]", "/*c*/{}", "{} x",
        "\xEF\xBB\xBF{}", "\"abc", "{\"a\" 1}", "[\"\x01\"]", "", "[1 2]", "{\"a\":}", "[.5]", "[1.]", "[1e]",
    };
    for (const char* s : bad)
    {
        ParseResult r = Parse(s);
        if (r.ok) wt::Fail(__FILE__, __LINE__, std::string("accepted invalid JSON: ") + s);
        else CHECK(!r.error.empty());
    }
}

TEST(Json, KeepsLargeIntegersExact)
{
    ParseResult r = Parse("[9007199254740993, -9223372036854775808, 1.5, 1e3]");
    REQUIRE(r.ok);
    CHECK(r.value[0].isInteger());
    CHECK_EQ(r.value[0].asInt64(), (int64_t)9007199254740993LL);
    CHECK_EQ(r.value[1].asInt64(), (std::numeric_limits<int64_t>::min)());
    CHECK(!r.value[2].isInteger());
    CHECK_EQ(r.value[2].asInt64(), (int64_t)1);           // truncated toward zero
    CHECK_EQ(r.value[3].asInt64(), (int64_t)1000);
}

TEST(Json, DecodesEscapesAndSurrogates)
{
    ParseResult r = Parse("[\"\\ud83d\\ude00\", \"\\ud800x\", \"a\\u0000b\", \"\\\"\\\\\\/\\b\\f\\n\\r\\t\"]");
    REQUIRE(r.ok);
    CHECK_EQ(r.value[0].asString(), std::string("\xF0\x9F\x98\x80"));
    CHECK_EQ(r.value[1].asString(), std::string("\xEF\xBF\xBD" "x"));
    CHECK_EQ(r.value[2].asString().size(), (size_t)3);
    CHECK_EQ(r.value[3].asString(), std::string("\"\\/\b\f\n\r\t"));
}

TEST(Json, RejectsInvalidUtf8Input)
{
    CHECK(!Parse(std::string("[\"\xC3\"]")).ok);              // truncated sequence
    CHECK(!Parse(std::string("[\"\xED\xA0\x80\"]")).ok);      // UTF-8-encoded surrogate
    CHECK(!Parse(std::string("[\"\xC0\xAF\"]")).ok);          // overlong encoding
    CHECK(Parse(std::string("[\"caf\xC3\xA9\"]")).ok);
}

TEST(Json, LastDuplicateKeyWins)
{
    ParseResult r = Parse("{\"a\":1,\"a\":2}");
    REQUIRE(r.ok);
    CHECK_EQ(r.value["a"].asInt64(), (int64_t)2);
    CHECK_EQ(r.value.members().size(), (size_t)2);
}

TEST(Json, LimitsNestingDepth)
{
    std::string deep(100, '[');
    deep += std::string(100, ']');
    CHECK(!Parse(deep, 64).ok);
    CHECK(Parse(deep, 200).ok);
    std::string bomb(200000, '[');
    CHECK(!Parse(bomb, 100000).ok);                            // clamped: must fail, not overflow the stack
}

TEST(Json, WriterAlwaysProducesValidJson)
{
    std::string out;
    Writer w(out);
    w.BeginObject()
     .KV("ctl", std::string("a\x01" "b\x1f"))
     .KV("bad_utf8", std::string("x\xC3"))
     .KV("lone_surrogate", std::wstring(L"y\xD83D"))
     .KV("nan", std::numeric_limits<double>::quiet_NaN())
     .KV("quote", "\"\\")
     .KV("wide", std::wstring(L"\x00E9t\x00E9"))
     .KV("big", (uint64_t)18446744073709551615ULL)
     .EndObject();
    ParseResult r = Parse(out);
    REQUIRE(r.ok);
    CHECK_EQ(r.value["ctl"].asString(), std::string("a\x01" "b\x1f"));
    CHECK_EQ(r.value["bad_utf8"].asString(), std::string("x\xEF\xBF\xBD"));
    CHECK_EQ(r.value["lone_surrogate"].asString(), std::string("y\xEF\xBF\xBD"));
    CHECK(r.value["nan"].isNull());
    CHECK_EQ(r.value["quote"].asString(), std::string("\"\\"));
    CHECK_EQ(r.value["wide"].asString(), std::string("\xC3\xA9t\xC3\xA9"));
    CHECK(out.find('\x01') == std::string::npos);
}

TEST(Json, WriterRecoversFromMisuse)
{
    std::string out;
    Writer w(out);
    w.BeginObject().Key("dangling");                // key without a value -> null
    w.EndObject().EndObject();                      // extra End*() is a no-op
    ParseResult r = Parse(out);
    REQUIRE(r.ok);
    CHECK(r.value["dangling"].isNull());
}

TEST(Json, SerializeRoundTrips)
{
    const std::string doc = "{\"k\":[1,2.5,\"s\",true,null,{}],\"e\":\"\\u0001\",\"u\":\"\xE2\x82\xAC\"}";
    ParseResult r = Parse(doc);
    REQUIRE(r.ok);
    std::string s = Serialize(r.value);
    ParseResult r2 = Parse(s);
    REQUIRE(r2.ok);
    CHECK_EQ(Serialize(r2.value), s);
    CHECK_EQ(r2.value["u"].asString(), std::string("\xE2\x82\xAC"));
}

TEST(Json, Utf16ConversionsReplaceInvalidSequences)
{
    CHECK_EQ(WideToUtf8(std::wstring(L"\xD83D\xDE00")), std::string("\xF0\x9F\x98\x80"));
    CHECK_EQ(Utf8ToWide(std::string("\xF0\x9F\x98\x80")), std::wstring(L"\xD83D\xDE00"));
    CHECK_EQ(WideToUtf8(std::wstring(L"a\xDC00")), std::string("a\xEF\xBF\xBD"));
    CHECK_EQ(Utf8ToWide(std::string("a\xFF")), std::wstring(L"a\xFFFD"));
}
