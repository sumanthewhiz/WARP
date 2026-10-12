#pragma once
// Minimal, dependency-free test harness for WARP's C++ unit tests (gap analysis ENG-1).
//
//   TEST(Suite, Name) { CHECK(...); REQUIRE(...); CHECK_EQ(a, b); CHECK_NEAR(a, b, eps); }
//
// CHECK* records a failure and continues; REQUIRE aborts the current test. Tests run in
// registration order; `WarpTests.exe /filter Store.` runs a subset (also --filter / -filter). A
// watchdog terminates the process if the run exceeds /timeout seconds (default 600), so a
// deadlock fails CI instead of hanging it.

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace wt
{
    using TestFn = void (*)();

    struct TestCase
    {
        const char* suite;
        const char* name;
        TestFn      fn;
    };

    std::vector<TestCase>& Registry();

    struct Registrar
    {
        Registrar(const char* suite, const char* name, TestFn fn) { Registry().push_back({ suite, name, fn }); }
    };

    struct Abort {};

    void        Fail(const char* file, int line, const std::string& message);
    std::string Narrow(const std::wstring& w);
    int64_t     NowMs();
    int64_t     NowUtc();

    inline std::string Show(const std::string& s) { return "\"" + s + "\""; }
    inline std::string Show(const char* s) { return s ? "\"" + std::string(s) + "\"" : std::string("(null)"); }
    inline std::string Show(const std::wstring& s) { return "L\"" + Narrow(s) + "\""; }
    inline std::string Show(const wchar_t* s) { return s ? Show(std::wstring(s)) : std::string("(null)"); }
    inline std::string Show(bool b) { return b ? "true" : "false"; }
    template <class T>
    std::string Show(const T& v)
    {
        if constexpr (std::is_enum<T>::value) return std::to_string(static_cast<long long>(v));
        else if constexpr (std::is_arithmetic<T>::value) return std::to_string(v);
        else return "<value>";
    }

    // Unique scratch directory under %TEMP%, removed (best effort) when destroyed.
    class TempDir
    {
    public:
        TempDir();
        ~TempDir();
        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;
        const std::wstring& Path() const { return m_path; }
        std::wstring File(const std::wstring& name) const { return m_path + L"\\" + name; }

    private:
        std::wstring m_path;
    };
}

#define WT_TEST_FN(suite, name) wt_test_##suite##_##name
#define TEST(suite, name)                                                                     \
    static void WT_TEST_FN(suite, name)();                                                    \
    static ::wt::Registrar wt_reg_##suite##_##name(#suite, #name, &WT_TEST_FN(suite, name));  \
    static void WT_TEST_FN(suite, name)()

#define CHECK(cond)                                                                           \
    do { if (!(cond)) ::wt::Fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)

#define REQUIRE(cond)                                                                         \
    do {                                                                                      \
        if (!(cond)) { ::wt::Fail(__FILE__, __LINE__, "REQUIRE(" #cond ")"); throw ::wt::Abort(); } \
    } while (0)

#define CHECK_EQ(a, b)                                                                        \
    do {                                                                                      \
        const auto& wt_a = (a);                                                               \
        const auto& wt_b = (b);                                                               \
        if (!(wt_a == wt_b))                                                                  \
            ::wt::Fail(__FILE__, __LINE__, std::string("CHECK_EQ(" #a ", " #b "): ") +        \
                                               ::wt::Show(wt_a) + " != " + ::wt::Show(wt_b)); \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                 \
    do {                                                                                      \
        const double wt_x = static_cast<double>(a);                                           \
        const double wt_y = static_cast<double>(b);                                           \
        if (!(std::fabs(wt_x - wt_y) <= (eps)))                                               \
            ::wt::Fail(__FILE__, __LINE__, std::string("CHECK_NEAR(" #a ", " #b "): ") +      \
                                               std::to_string(wt_x) + " vs " + std::to_string(wt_y)); \
    } while (0)
