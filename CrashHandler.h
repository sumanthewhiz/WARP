#pragma once
#include <string>
namespace CrashHandler {
// Installs: SetUnhandledExceptionFilter (writes a minidump), _set_invalid_parameter_handler, _set_purecall_handler,
// std::set_terminate (calling thread) and a SIGABRT handler (abort() on any thread); these write a dump, then
// fail fast into Windows Error Reporting. It also calls
// RegisterApplicationRestart(restartArgs, RESTART_NO_REBOOT | RESTART_NO_PATCH) so Windows Error Reporting restarts
// the process after a crash/hang (WER only restarts processes that ran >= 60 s).
// Dumps go to <dumpDir>\warp-<yyyyMMdd-HHmmss>-<pid>.dmp; keeps only the newest `keepDumps` files.
// Dump type: MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithHandleData.
void Install(const std::wstring& dumpDir, const std::wstring& restartArgs = L"--headless --restarted", int keepDumps = 5);
// Writes a dump of the current process on demand (e.g. from a watchdog that detected a hung thread). Returns path or empty.
std::wstring WriteDumpNow(const char* reason);
}
