// Test runner for WarpTests.exe (see WarpTests.h).
#include "WarpTests.h"
#include "Json.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
    int g_failures = 0;
}

int RunQualityReplay(const std::wstring& input, const std::wstring& output, const std::wstring& models);

namespace wt
{
    std::vector<TestCase>& Registry()
    {
        static std::vector<TestCase> r;
        return r;
    }

    void Fail(const char* file, int line, const std::string& message)
    {
        ++g_failures;
        const char* base = std::strrchr(file, '\\');
        std::printf("    %s(%d): %s\n", base ? base + 1 : file, line, message.c_str());
        std::fflush(stdout);
    }

    std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s((size_t)(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
        return s;
    }

    int64_t NowMs()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        return (int64_t)((t - 116444736000000000ULL) / 10000ULL);
    }

    int64_t NowUtc() { return NowMs() / 1000; }

    TempDir::TempDir()
    {
        static std::atomic<int> counter{ 0 };
        wchar_t tmp[MAX_PATH + 1] = {};
        DWORD n = GetTempPathW(MAX_PATH, tmp);
        std::wstring base = n ? std::wstring(tmp, n) : std::wstring(L".\\");
        wchar_t longBuf[MAX_PATH + 1] = {};
        if (GetLongPathNameW(base.c_str(), longBuf, MAX_PATH)) base = longBuf;
        if (!base.empty() && base.back() != L'\\') base.push_back(L'\\');
        m_path = base + L"warp-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter) +
                 L"-" + std::to_wstring(GetTickCount64());
        std::error_code ec;
        std::filesystem::create_directories(m_path, ec);
    }

    TempDir::~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
}

int wmain(int argc, wchar_t** argv)
{
    std::string filter;
    std::wstring replay, output, report, models;
    unsigned timeoutSec = 600;
    // Options may be spelled --name, -name or /name.
    auto is = [](const wchar_t* arg, const wchar_t* name)
    {
        while (*arg == L'-' || *arg == L'/') ++arg;
        return _wcsicmp(arg, name) == 0;
    };
    for (int i = 1; i < argc; ++i)
    {
        if (is(argv[i], L"filter") && i + 1 < argc) filter = wt::Narrow(argv[++i]);
        else if (is(argv[i], L"timeout") && i + 1 < argc) timeoutSec = (unsigned)_wtoi(argv[++i]);
        else if (is(argv[i], L"replay") && i + 1 < argc) replay = argv[++i];
        else if (is(argv[i], L"output") && i + 1 < argc) output = argv[++i];
        else if (is(argv[i], L"report") && i + 1 < argc) report = argv[++i];
        else if (is(argv[i], L"models") && i + 1 < argc) models = argv[++i];
        else if (is(argv[i], L"list"))
        {
            for (const wt::TestCase& t : wt::Registry()) std::printf("%s.%s\n", t.suite, t.name);
            return 0;
        }
    }
    SetConsoleOutputCP(CP_UTF8);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    // Watchdog: a deadlock (the BUG-1 class of failure) must fail the run instead of hanging CI.
    std::thread([timeoutSec]
    {
        Sleep(timeoutSec * 1000);
        std::printf("\nTIMEOUT: the test run exceeded %u s (deadlock?)\n", timeoutSec);
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);
    }).detach();

    if (!replay.empty())
    {
        try
        {
            if (output.empty()) throw std::runtime_error("/replay requires /output");
            return RunQualityReplay(replay, output, models);
        }
        catch (const std::exception& e) { std::fprintf(stderr, "Replay failed: %s\n", e.what()); return 1; }
    }

    int run = 0, failedTests = 0;
    auto results = WarpJson::Value::Array();
    for (const wt::TestCase& t : wt::Registry())
    {
        std::string full = std::string(t.suite) + "." + t.name;
        if (!filter.empty() && full.find(filter) == std::string::npos) continue;
        ++run;
        const int before = g_failures;
        std::printf("[ RUN  ] %s\n", full.c_str());
        std::fflush(stdout);
        const auto t0 = std::chrono::steady_clock::now();
        try
        {
            t.fn();
        }
        catch (const wt::Abort&) {}
        catch (const std::exception& e) { wt::Fail(__FILE__, __LINE__, std::string("unhandled exception: ") + e.what()); }
        catch (...) { wt::Fail(__FILE__, __LINE__, "unhandled non-standard exception"); }
        const long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        const bool ok = g_failures == before;
        if (!ok) ++failedTests;
        std::printf("[%s] %s (%lld ms)\n", ok ? "  OK  " : " FAIL ", full.c_str(), ms);
        std::fflush(stdout);
        auto result = WarpJson::Value::Object();
        result.set("name", WarpJson::Value::String(full));
        result.set("passed", WarpJson::Value::Bool(ok));
        result.set("duration_ms", WarpJson::Value::Int(ms));
        results.push(std::move(result));
    }
    std::printf("\n%d test(s) run, %d failed\n", run, failedTests);
    if (!report.empty())
    {
        auto value = WarpJson::Value::Object();
        value.set("tests", std::move(results));
        value.set("run", WarpJson::Value::Int(run));
        value.set("failed", WarpJson::Value::Int(failedTests));
        std::ofstream out{ std::filesystem::path(report), std::ios::binary };
        out << WarpJson::Serialize(value);
        if (!out) return 2;
    }
    if (run == 0) return 2;
    return failedTests ? 1 : 0;
}
