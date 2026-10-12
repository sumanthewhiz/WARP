#pragma once
#include <string>
namespace Installer {
struct Result { bool ok = false; std::wstring message; };
// Task path: folder "\WARP", task name L"WARP Agent - " + <user SID string>.
std::wstring TaskName();
// Name of a per-session named event that a running WARP instance waits on; signaling it requests a graceful exit.
// Must return exactly: L"Local\\WARP-ShutdownRequest".
const wchar_t* ShutdownEventName();
// %ProgramFiles%\WARP (via SHGetKnownFolderPath(FOLDERID_ProgramFiles)).
std::wstring InstallDir();
std::wstring InstalledExePath();   // InstallDir() + L"\\" + <current exe file name>
// 1) If the running exe is not already in InstallDir(): copy the WARP package from the directory that contains the
//    running exe into InstallDir() (overwrite): the exe, onnxruntime*.dll, NOTICE.md, API.md, "WARP - Readme.txt",
//    SHA256SUMS.txt and the models\ and licenses\ folders - nothing else, and never through junctions/symlinks.
//    InstallDir under Program Files is admin-writable only, which is what we want (an elevated process must not load
//    code/models from user-writable folders).
// 2) Register (create or update) the scheduled task:
//    - Principal: the CURRENT user (UserId = user SID string or DOMAIN\user), LogonType TASK_LOGON_INTERACTIVE_TOKEN, RunLevel TASK_RUNLEVEL_HIGHEST.
//    - Trigger: TASK_TRIGGER_LOGON for that same user (UserId set), no delay or PT15S delay.
//    - Action: Exec InstalledExePath() with Arguments L"--headless", WorkingDirectory InstallDir().
//    - Settings: StartWhenAvailable=VARIANT_TRUE, DisallowStartIfOnBatteries=VARIANT_FALSE, StopIfGoingOnBatteries=VARIANT_FALSE,
//      ExecutionTimeLimit=L"PT0S" (unlimited), MultipleInstances=TASK_INSTANCES_IGNORE_NEW, RestartCount=3,
//      RestartInterval=L"PT1M", AllowHardTerminate=VARIANT_TRUE, Hidden=VARIANT_FALSE, Priority=6 (below normal is 7; 6 is fine), Enabled.
//    - RegisterTaskDefinition with TASK_CREATE_OR_UPDATE, logon type TASK_LOGON_INTERACTIVE_TOKEN.
// 3) If startNow: run the task (IRegisteredTask::Run) -- but only after copying succeeded.
// The process calling Install() is elevated (`WARP!.exe --install` self-elevates); running instances
// in this session are asked to exit first so an upgrade can replace their files.
Result Install(bool startNow);
// Signals running instances in THIS session (ShutdownEventName) and waits up to 10 s, deletes the task,
// and if removeFiles deletes InstallDir() contents (if the running exe is inside InstallDir, schedule deletion via
// MoveFileExW(..., MOVEFILE_DELAY_UNTIL_REBOOT) for files that cannot be deleted now). If removeData, deletes
// %LOCALAPPDATA%\WARP (the per-user data directory) recursively.
Result Uninstall(bool removeFiles, bool removeData);
bool IsTaskInstalled();
// Runs the registered task now (used by --start). Result.ok=false if not installed.
Result StartTask();
}
