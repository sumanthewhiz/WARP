// Logger.cpp : rotating UTF-8 file log + ETW (TraceLogging) for WARP.
#include "framework.h"
#include "Logger.h"

#include <TraceLoggingProvider.h>

#include <stdio.h>
#include <stdarg.h>
#include <locale.h>
#include <share.h>
#include <string.h>
#include <utility>

#pragma comment(lib, "advapi32.lib")

// -----------------------------------------------------------------------------
// TraceLogging provider.  Fixed GUID so external tools (tracelog/WPR/xperf) can
// subscribe by provider id instead of name:
//     Provider "WARP"  ->  {C9E7B2A4-3F1D-4E6B-8A52-7D9C0E1F2A3B}
// -----------------------------------------------------------------------------
TRACELOGGING_DEFINE_PROVIDER(
    g_warpProvider,
    "WARP",
    (0xc9e7b2a4, 0x3f1d, 0x4e6b, 0x8a, 0x52, 0x7d, 0x9c, 0x0e, 0x1f, 0x2a, 0x3b));

namespace
{
    // --- Lazily-initialised lock, usable even before Log::Init() -----------
    CRITICAL_SECTION g_cs;
    INIT_ONCE        g_csOnce = INIT_ONCE_STATIC_INIT;

    BOOL CALLBACK InitCsOnce(PINIT_ONCE, PVOID, PVOID*)
    {
        InitializeCriticalSection(&g_cs);
        return TRUE;
    }

    CRITICAL_SECTION* Cs()
    {
        InitOnceExecuteOnce(&g_csOnce, InitCsOnce, nullptr, nullptr);
        return &g_cs;
    }

    struct Guard
    {
        Guard()  { EnterCriticalSection(Cs()); }
        ~Guard() { LeaveCriticalSection(&g_cs); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };

    // --- State (guarded by g_cs) -------------------------------------------
    bool         g_inited   = false;
    bool         g_provReg  = false;
    FILE*        g_file     = nullptr;
    std::wstring g_logDir;
    std::wstring g_logPath;
    Log::Level   g_minLevel = Log::Level::Info;
    uint64_t     g_maxBytes = 5ull * 1024 * 1024;
    int          g_maxFiles = 5;
    uint64_t     g_curSize  = 0;
    ULONGLONG    g_rotateRetryTick = 0;   // after a failed rotation: earliest GetTickCount64() of the next try
    uint64_t     g_counts[4] = { 0, 0, 0, 0 };
    std::vector<std::string> g_ring;
    const size_t kRingCap = 64;

    const char* LevelStr(Log::Level l)
    {
        switch (l)
        {
            case Log::Level::Debug: return "DEBUG";
            case Log::Level::Info:  return "INFO ";
            case Log::Level::Warn:  return "WARN ";
            case Log::Level::Error: return "ERROR";
        }
        return "?????";
    }

    int LevelIdx(Log::Level l)
    {
        int i = static_cast<int>(l);
        if (i < 0) i = 0;
        if (i > 3) i = 3;
        return i;
    }

    void FormatTimestamp(char* buf, size_t cap)
    {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        SYSTEMTIME st;
        FileTimeToSystemTime(&ft, &st);
        snprintf(buf, cap, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
            (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
            (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
            (unsigned)st.wMilliseconds);
    }

    uint64_t FileSizeOf(const std::wstring& p)
    {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d))
            return (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        return 0;
    }

    std::wstring RollName(int i)
    {
        if (i <= 0)
            return g_logDir.empty() ? std::wstring(L"warp.log") : (g_logDir + L"\\warp.log");
        wchar_t num[16];
        swprintf_s(num, L"%d", i);
        std::wstring base = g_logDir.empty() ? std::wstring() : (g_logDir + L"\\");
        return base + L"warp." + num + L".log";
    }

    void CloseFile()
    {
        if (g_file)
        {
            fclose(g_file);
            g_file = nullptr;
        }
    }

    void OpenFile()
    {
        // Binary append + deny-write sharing: exactly one writer, explicit CRLF.
        g_file = _wfsopen(g_logPath.c_str(), L"ab", _SH_DENYWR);
        g_curSize = g_file ? FileSizeOf(g_logPath) : 0;
    }

    void Rotate()
    {
        CloseFile();
        bool rotated = false;
        if (g_maxFiles >= 2)
        {
            // Move the live file aside first: if it cannot be renamed (e.g. a viewer holds it
            // without FILE_SHARE_DELETE) the archives must stay untouched, otherwise every
            // retry would shift - and finally delete - all of them.
            const std::wstring aside = RollName(0) + L".rotating";
            if (MoveFileExW(RollName(0).c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                DeleteFileW(RollName(g_maxFiles - 1).c_str());
                for (int i = g_maxFiles - 2; i >= 1; --i)
                {
                    MoveFileExW(RollName(i).c_str(), RollName(i + 1).c_str(),
                                MOVEFILE_REPLACE_EXISTING);
                }
                MoveFileExW(aside.c_str(), RollName(1).c_str(),
                            MOVEFILE_REPLACE_EXISTING);
                rotated = true;
            }
        }
        else
        {
            rotated = DeleteFileW(RollName(0).c_str()) != FALSE;
        }
        // A failed rotation is retried a minute later instead of on every line.
        g_rotateRetryTick = rotated ? 0 : GetTickCount64() + 60 * 1000;
        OpenFile();
    }

    void EmitLine(Log::Level lvl, const char* component, const char* msg)
    {
        if (!component) component = "";
        if (!msg)       msg = "";

        char ts[32];
        FormatTimestamp(ts, sizeof(ts));

        char line[4600];
        int n = snprintf(line, sizeof(line), "%s [%s] [%s] tid=%lu %s\r\n",
                         ts, LevelStr(lvl), component,
                         (unsigned long)GetCurrentThreadId(), msg);

        size_t len;
        if (n < 0)
        {
            line[0] = '\0';
            len = 0;
        }
        else if ((size_t)n >= sizeof(line))
        {
            // Truncated: guarantee a CRLF terminator on the stored line.
            line[sizeof(line) - 3] = '\r';
            line[sizeof(line) - 2] = '\n';
            line[sizeof(line) - 1] = '\0';
            len = sizeof(line) - 1;
        }
        else
        {
            len = (size_t)n;
        }

        Guard g;

        g_counts[LevelIdx(lvl)]++;

        if (lvl >= Log::Level::Warn)
        {
            try
            {
                std::string entry(line, len);
                while (!entry.empty() && (entry.back() == '\n' || entry.back() == '\r'))
                    entry.pop_back();
                if (g_ring.size() >= kRingCap)
                    g_ring.erase(g_ring.begin());
                g_ring.push_back(std::move(entry));
            }
            catch (...)
            {
                // Dropping a ring entry must never crash the logger.
            }
        }

        if (g_inited && g_file && lvl >= g_minLevel)
        {
            if (g_curSize > 0 && g_curSize + len > g_maxBytes && GetTickCount64() >= g_rotateRetryTick)
                Rotate();
            if (g_file)
            {
                size_t w = fwrite(line, 1, len, g_file);
                g_curSize += w;
                fflush(g_file);
            }
        }

        // Safe no-op until the provider is registered in Init().
        TraceLoggingWrite(g_warpProvider, "Log",
            TraceLoggingInt32((INT32)lvl, "Level"),
            TraceLoggingString(component, "Component"),
            TraceLoggingString(msg, "Message"));
    }
}

void Log::Init(const std::wstring& logDir, Level minLevel, uint64_t maxBytes, int maxFiles)
{
    Guard g;

    CloseFile();

    g_logDir   = logDir;
    g_minLevel = minLevel;
    g_maxBytes = (maxBytes == 0) ? 1 : maxBytes;
    g_maxFiles = (maxFiles < 1) ? 1 : maxFiles;
    g_rotateRetryTick = 0;
    g_counts[0] = g_counts[1] = g_counts[2] = g_counts[3] = 0;

    if (!g_logDir.empty())
        SHCreateDirectoryExW(nullptr, g_logDir.c_str(), nullptr);

    g_logPath = g_logDir.empty() ? std::wstring(L"warp.log") : (g_logDir + L"\\warp.log");
    OpenFile();

    if (!g_provReg)
    {
        if (TraceLoggingRegister(g_warpProvider) == ERROR_SUCCESS)
            g_provReg = true;
    }

    g_inited = true;
}

void Log::Shutdown()
{
    Guard g;
    if (g_file)
    {
        fflush(g_file);
        CloseFile();
    }
    if (g_provReg)
    {
        TraceLoggingUnregister(g_warpProvider);
        g_provReg = false;
    }
    g_inited = false;
}

void Log::SetMinLevel(Level lvl)
{
    Guard g;
    g_minLevel = lvl;
}

void Log::Write(Level lvl, const char* component, const char* fmt, ...)
{
    char msg[4096];
    if (!fmt) fmt = "";

    // A UTF-8 locale makes "%S" (wide) arguments convert to UTF-8: in the default "C" locale a path
    // with characters above U+00FF fails the whole format and the line would be logged empty.
    static const _locale_t utf8 = _create_locale(LC_ALL, ".UTF8");   // Windows 10 1803+, else nullptr

    va_list ap;
    va_start(ap, fmt);
    int n = utf8 ? _vsnprintf_s_l(msg, sizeof(msg), _TRUNCATE, fmt, utf8, ap)
                 : vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (n < 0 && msg[0] == '\0')
        strncpy_s(msg, sizeof(msg), fmt, _TRUNCATE);   // conversion failed: keep at least the format text
    else if (n < 0 || (size_t)n >= sizeof(msg))
        msg[sizeof(msg) - 1] = '\0';                   // truncated (already NUL-terminated)

    EmitLine(lvl, component, msg);
}

void Log::WriteW(Level lvl, const char* component, const std::wstring& message)
{
    std::string utf8;
    if (!message.empty())
    {
        int need = WideCharToMultiByte(CP_UTF8, 0, message.c_str(), (int)message.size(),
                                       nullptr, 0, nullptr, nullptr);
        if (need > 0)
        {
            try
            {
                utf8.resize((size_t)need);
            }
            catch (...)
            {
                EmitLine(lvl, component, "");
                return;
            }
            WideCharToMultiByte(CP_UTF8, 0, message.c_str(), (int)message.size(),
                                &utf8[0], need, nullptr, nullptr);
        }
    }
    EmitLine(lvl, component, utf8.c_str());
}

std::vector<std::string> Log::RecentProblems(size_t max)
{
    Guard g;
    std::vector<std::string> out;
    size_t n = g_ring.size();
    size_t take = (max < n) ? max : n;
    out.reserve(take);
    for (size_t i = n - take; i < n; ++i)
        out.push_back(g_ring[i]);
    return out;
}

uint64_t Log::CountAtLevel(Level lvl)
{
    Guard g;
    return g_counts[LevelIdx(lvl)];
}

std::wstring Log::LogFilePath()
{
    Guard g;
    return g_logPath;
}
