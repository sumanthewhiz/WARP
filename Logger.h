#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace Log {
enum class Level { Debug = 0, Info = 1, Warn = 2, Error = 3 };
// Rotating UTF-8 file log: <logDir>\warp.log, rolled to warp.1.log ... warp.<maxFiles-1>.log when size exceeds maxBytes.
// Creates logDir if missing. Safe to call multiple times (re-init closes previous file).
void Init(const std::wstring& logDir, Level minLevel = Level::Info, uint64_t maxBytes = 5ull * 1024 * 1024, int maxFiles = 5);
void Shutdown();
void SetMinLevel(Level lvl);
// printf-style, UTF-8 format string and args. Thread-safe. Never throws, never blocks for long.
// Line format: "2026-10-09T13:05:01.123Z [INFO ] [component] tid=1234 message\r\n"
void Write(Level lvl, const char* component, const char* fmt, ...);
void WriteW(Level lvl, const char* component, const std::wstring& message);   // convenience for wide text
// Last N Warn/Error lines (most recent last), kept in a bounded in-memory ring (e.g. 64 entries).
std::vector<std::string> RecentProblems(size_t max = 20);
// Counters since Init.
uint64_t CountAtLevel(Level lvl);
std::wstring LogFilePath();
}
#define WARP_LOG_DEBUG(comp, ...) ::Log::Write(::Log::Level::Debug, comp, __VA_ARGS__)
#define WARP_LOG_INFO(comp, ...)  ::Log::Write(::Log::Level::Info,  comp, __VA_ARGS__)
#define WARP_LOG_WARN(comp, ...)  ::Log::Write(::Log::Level::Warn,  comp, __VA_ARGS__)
#define WARP_LOG_ERROR(comp, ...) ::Log::Write(::Log::Level::Error, comp, __VA_ARGS__)
