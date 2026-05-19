#include "framework.h"
#include "framework.h"
#include "FileMonitor.h"
#include "EventContext.h"
#include "SystemProcessClassifier.h"
#include <shlobj.h>
#include <algorithm>
#include <evntrace.h>
#include <evntcons.h>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <mutex>
#include <psapi.h>

#pragma comment(lib, "advapi32.lib")

// Static instance pointer for the ETW callback
static FileMonitor* g_pFileMonitor = nullptr;

static const wchar_t* WARP_ETW_SESSION_NAME = L"NT Kernel Logger";

// SystemTraceControlGuid � required as Wnode.Guid for NT Kernel Logger
// {9E814AAD-3204-11D2-9A82-006008A86939}
static const GUID SystemTraceControlGuid =
    { 0x9E814AAD, 0x3204, 0x11D2, { 0x9A, 0x82, 0x00, 0x60, 0x08, 0xA8, 0x69, 0x39 } };

// Classic FileIo GUID � events in the NT Kernel Logger use this provider GUID
// {90CBDC39-4A3E-11D1-84F4-0000F80464E3}
static const GUID FileIoGuid =
    { 0x90CBDC39, 0x4A3E, 0x11D1, { 0x84, 0xF4, 0x00, 0x00, 0xF8, 0x04, 0x64, 0xE3 } };

// Cached device-path-to-drive-letter map (built once, used in callback)
static std::unordered_map<std::wstring, std::wstring> g_deviceToDrive;
static std::once_flag g_deviceMapOnce;

// Our own PID � used to skip self-generated file-open events
static DWORD g_ownPid = 0;

// Cache of PIDs already classified as user (true) or system (false)
static std::unordered_map<DWORD, bool> g_pidCache;
static std::mutex g_pidCacheMtx;
static ULONGLONG g_pidCacheLastCleanup = 0;

// Returns true if the PID belongs to an interactive user session and is not
// a known noisy system process. Caches results to avoid repeated lookups.
static bool IsUserProcess(DWORD pid)
{
    if (pid == 0 || pid == 4) // System Idle / System
        return false;

    {
        std::lock_guard<std::mutex> lock(g_pidCacheMtx);

        // Periodic cleanup every 60 seconds
        ULONGLONG now = GetTickCount64();
        if (now - g_pidCacheLastCleanup > 60000)
        {
            g_pidCache.clear();
            g_pidCacheLastCleanup = now;
        }

        auto it = g_pidCache.find(pid);
        if (it != g_pidCache.end())
            return it->second;
    }

    bool isUser = false;

    // Check session ID � session 0 is services, session >= 1 is interactive
    DWORD sessionId = 0;
    if (ProcessIdToSessionId(pid, &sessionId) && sessionId >= 1)
    {
        isUser = true;

        // Further filter: block known noisy system processes that run in user session
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hProc)
        {
            wchar_t exePath[MAX_PATH] = {};
            DWORD exeLen = MAX_PATH;
            if (QueryFullProcessImageNameW(hProc, 0, exePath, &exeLen) && exeLen > 0)
            {
                // Extract just the filename
                std::wstring exe(exePath, exeLen);
                size_t slash = exe.rfind(L'\\');
                if (slash != std::wstring::npos)
                    exe = exe.substr(slash + 1);
                std::transform(exe.begin(), exe.end(), exe.begin(), ::towlower);

                static const wchar_t* const noisyProcesses[] = {
                    // Search / Indexer
                    L"searchprotocolhost.exe",
                    L"searchindexer.exe",
                    L"searchfilterhost.exe",
                    L"searchhost.exe",
                    L"searchapp.exe",
                    // Windows Defender / Security
                    L"msmpeng.exe",
                    L"mpcmdrun.exe",
                    L"nissrv.exe",
                    L"securityhealthservice.exe",
                    L"securityhealthsystray.exe",
                    L"smartscreen.exe",
                    L"sgrmbroker.exe",
                    // Core system
                    L"svchost.exe",
                    L"csrss.exe",
                    L"smss.exe",
                    L"lsass.exe",
                    L"services.exe",
                    L"wininit.exe",
                    L"winlogon.exe",
                    L"spoolsv.exe",
                    L"wmiprvse.exe",
                    L"wmiapsrv.exe",
                    // Shell infrastructure
                    L"taskhostw.exe",
                    L"runtimebroker.exe",
                    L"backgroundtaskhost.exe",
                    L"audiodg.exe",
                    L"fontdrvhost.exe",
                    L"dwm.exe",
                    L"conhost.exe",
                    L"dllhost.exe",
                    L"sihost.exe",
                    L"ctfmon.exe",
                    L"settingsynchost.exe",
                    L"shellexperiencehost.exe",
                    L"startmenuexperiencehost.exe",
                    L"applicationframehost.exe",
                    L"textinputhost.exe",
                    L"lockapp.exe",
                    // Update / Servicing
                    L"msiexec.exe",
                    L"trustedinstaller.exe",
                    L"tiworker.exe",
                    L"usoclient.exe",
                    L"usocoreworker.exe",
                    // Telemetry
                    L"compattelrunner.exe",
                    L"devicecensus.exe",
                    L"diagtrack.exe",
                    L"musnotification.exe",
                    L"windowsupdatebox.exe",
                    // Sync / helpers
                    L"onedrive.exe",
                    L"msedgewebview2.exe",
                    L"crashpad_handler.exe",
                    // Widgets / Phone / Xbox
                    L"phoneexperiencehost.exe",
                    L"widgetservice.exe",
                    L"widgets.exe",
                    L"gamebarpresencewriter.exe",
                    // System settings / UWP
                    L"systemsettings.exe",
                    L"systemsettingsbroker.exe",
                    // Runtime hosts
                    L"dashost.exe",
                    L"wsappx.exe",
                    // Error reporting
                    L"werfault.exe",
                    L"wermgr.exe",
                };

                for (const auto* noisy : noisyProcesses)
                {
                    if (exe == noisy)
                    {
                        isUser = false;
                        break;
                    }
                }
            }
            CloseHandle(hProc);
        }
        else
        {
            // Can't open the process � likely a system process, skip it
            isUser = false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_pidCacheMtx);
        g_pidCache[pid] = isUser;
    }
    return isUser;
}

static void BuildDeviceMap()
{
    wchar_t drives[512] = {};
    if (GetLogicalDriveStringsW(511, drives) == 0) return;
    for (const wchar_t* drv = drives; *drv; drv += wcslen(drv) + 1)
    {
        wchar_t letter[3] = { drv[0], drv[1], L'\0' };
        wchar_t target[MAX_PATH] = {};
        if (QueryDosDeviceW(letter, target, MAX_PATH) > 0)
        {
            std::wstring key(target);
            std::transform(key.begin(), key.end(), key.begin(), ::towlower);
            g_deviceToDrive[key] = letter;
        }
    }
}


// ---------- "Goop" folder / path exclusion filter ----------
// A "goop" path is a system-managed, application-internal, or transient location
// that users would never intentionally interact with as meaningful file activity.
// The categories below are derived from the "Intelligent Global File Searchability"
// spec (Section 3A - Definition of Goop Folders, Categories 1-10).
//
// Returns true if the path should be excluded from file-activity recording.
bool ShouldExclude(const std::wstring& path)
{
    if (path.empty())
        return true;

    // Work with a lowercase copy for case-insensitive matching
    std::wstring lp = path;
    std::transform(lp.begin(), lp.end(), lp.begin(), ::towlower);

    // =====================================================================
    // 0. AppData re-admit allowlist -- re-admit user-meaningful files
    //    that live under AppData and would otherwise be lost to the
    //    blanket \appdata\ exclusion below.
    //
    //    DESIGN: each entry is (directory-substring, allowed-extensions).
    //    The allowlist is **extension-gated**: only files whose extension
    //    matches the per-app allowed list are short-circuited (admitted)
    //    here. Anything else inside the directory falls through to the
    //    rest of ShouldExclude and is normally caught by the \appdata\
    //    goop dir, the excludedExts list, the ~prefix temp filter, etc.
    //
    //    Why extension-gated? The previous version short-circuited on
    //    bare directory match, which let through Outlook's transient
    //    artefacts (e.g. `~mailbox.ost.tmp`, `mailbox.nst` Quick Search
    //    Database) even though they hit excluded extensions / temp-
    //    prefix rules. Gating on extension keeps the canonical user
    //    files (`.pst` / `.ost` mail stores, `.one` notebook sections,
    //    Sticky Notes `.sqlite`) admitted while the noise gets caught
    //    by the normal pipeline.
    //
    //    Use anyExts (`{ nullptr }`) when the directory itself is so
    //    narrow that we trust everything inside it (Office UnsavedFiles
    //    is literally one folder for AutoRecover drafts).
    // =====================================================================
    static const wchar_t* const onenoteExts[] = { L".one", nullptr };
    static const wchar_t* const outlookExts[] = { L".pst", L".ost", nullptr };
    static const wchar_t* const stickyExts[]  = { L".sqlite", nullptr };
    static const wchar_t* const anyExts[]     = { nullptr };

    struct AppDataReadmit {
        const wchar_t*        dirSubstring;
        // Optional additional substring that must also be present
        // (in addition to dirSubstring). Use nullptr for "no extra
        // requirement". Lets us narrow VS to its versioned Backup
        // Files subtree without enumerating every version.
        const wchar_t*        alsoRequired;
        const wchar_t* const* allowedExts; // nullptr-terminated
    };

    static const AppDataReadmit appdataAllowlist[] = {
        // OneNote -- notebook section files only. .onetoc2 is a
        // regenerable table-of-contents cache, not user content.
        { L"\\microsoft\\onenote\\",              nullptr,                onenoteExts },
        // Outlook -- mail stores only. Excludes the Quick Search
        // Database (.nst), Offline Address Book (.oab), QuickAccess
        // Toolbar (.qat), and the entire family of *.ost.tmp / temp
        // sync artefacts -- all of which used to leak through.
        { L"\\microsoft\\outlook\\",              nullptr,                outlookExts },
        // Sticky Notes (legacy desktop store + modern Plumsail).
        // Only the SQLite content file; the -wal / -shm journals
        // remain filtered as low-value noise.
        { L"\\microsoft\\sticky notes\\",         nullptr,                stickyExts  },
        { L"\\microsoft\\windows\\stickynotes\\", nullptr,                stickyExts  },
        // Visual Studio recoverable backups -- narrowed from the
        // entire VS profile dir to the Backup Files subtree, which
        // is the actual AutoRecover destination. The path is
        //   \Microsoft\VisualStudio\<version>_<id>\Backup Files\
        // with a versioned intermediate dir, so we require BOTH
        // substrings instead of trying to enumerate versions.
        { L"\\microsoft\\visualstudio\\",         L"\\backup files\\",    anyExts     },
        // Office unsaved drafts (Word/Excel/PPT AutoRecover).
        { L"\\microsoft\\office\\unsavedfiles\\", nullptr,                anyExts     },
        // Notepad++ session backups.
        { L"\\notepad++\\backup\\",               nullptr,                anyExts     },
    };

    for (const auto& aw : appdataAllowlist)
    {
        if (lp.find(aw.dirSubstring) == std::wstring::npos)
            continue;
        if (aw.alsoRequired && lp.find(aw.alsoRequired) == std::wstring::npos)
            continue;

        // anyExts: directory is narrow enough to trust unconditionally.
        if (aw.allowedExts[0] == nullptr)
            return false;

        // Extension-specific: extract extension and compare.
        size_t dotPos = lp.rfind(L'.');
        if (dotPos != std::wstring::npos)
        {
            std::wstring ext = lp.substr(dotPos);
            for (const wchar_t* const* p = aw.allowedExts; *p; ++p)
            {
                if (ext == *p)
                    return false;
            }
        }

        // Inside an allowlisted dir but extension didn't match -- fall
        // through to the rest of ShouldExclude so \appdata\ goop /
        // excludedExts / ~prefix can do their job. Stop iterating; the
        // dirs do not overlap.
        break;
    }

    // =====================================================================
    // 1. Directory-substring matching (goop Categories 1-9)
    //    Any path containing one of these substrings is excluded.
    //    Patterns use trailing backslash so they match files *inside* the dir.
    // =====================================================================
    static const wchar_t* const goopDirs[] = {
        // --- Cat 1: Windows OS & System Directories ---
        L"\\windows\\",

        // --- Cat 2: Program Files & Application Binaries ---
        L"\\program files\\",
        L"\\program files (x86)\\",

        // --- Cat 3: ProgramData (System & Application State) ---
        L"\\programdata\\",

        // --- Cat 4: User Profile - AppData ---
        L"\\appdata\\",

        // --- Cat 5: Per-Volume System & Recovery Folders ---
        L"\\system volume information\\",
        L"\\$recycle.bin\\",
        L"\\recovery\\",
        L"\\$windows.~bt\\",
        L"\\$windows.~ws\\",
        L"\\$windows.~q\\",
        L"\\$winreagent\\",
        L"\\$getcurrent\\",
        L"\\$sysreset\\",
        L"\\config.msi\\",
        L"\\msocache\\",
        L"\\perflogs\\",
        L"\\dfsrprivate\\",

        // --- Cat 6: Developer Toolchain & Build Artifacts ---
        L"\\node_modules\\",
        L"\\__pycache__\\",
        L"\\bower_components\\",
        L"\\obj\\",
        L"\\bin\\debug\\",
        L"\\bin\\release\\",
        L"\\target\\",
        L"\\cmakefiles\\",
        L"\\pods\\",
        L"\\venv\\",
        L"\\env\\",
        L"\\coverage\\",

        // --- Cat 7: User Profile - Hidden/System Shell Folders ---
        L"\\application data\\",
        L"\\local settings\\",
        L"\\cookies\\",
        L"\\nethood\\",
        L"\\printhood\\",
        L"\\recent\\",
        L"\\sendto\\",
        L"\\start menu\\",
        L"\\templates\\",
        L"\\my documents\\",
        L"\\microsoftedgebackups\\",
        L"\\intelgraphicsprofiles\\",

        // --- Cat 8: Application-Specific Caches & Databases (by name) ---
        L"\\cache\\",
        L"\\cachestorage\\",
        L"\\code cache\\",
        L"\\gpucache\\",
        L"\\dawncache\\",
        L"\\grshadercache\\",
        L"\\shadercache\\",
        L"\\crash reports\\",
        L"\\crashdumps\\",
        L"\\crashpad\\",
        L"\\blob_storage\\",
        L"\\indexeddb\\",
        L"\\local storage\\",
        L"\\session storage\\",
        L"\\service worker\\",
        L"\\webstorage\\",
        L"\\databases\\",
        L"\\logs\\",
        L"\\log\\",
        L"\\temp\\",
        L"\\tmp\\",

        // --- Cat 9: OS Upgrade, Recovery & Rollback Artifacts ---
        L"\\windows.old\\",
        L"\\onedrivetemp\\",
        L"\\inetpub\\",

        // --- Additional (not in spec but consistently noisy) ---
        L"\\windowsapps\\",
        L"\\packages\\",
        L"\\intel\\",
        L"\\amd\\",
        L"\\nvidia\\",
        L"\\boot\\",
        L"\\~snapshot\\",
        L"\\servicing\\",
        L"\\winsxs\\",

        // --- Extension goop (added 2025) ---
        // Modern build caches not on the original list
        L"\\.gradle\\caches\\",
        L"\\.gradle\\daemon\\",
        L"\\.gradle\\native\\",
        L"\\.cargo\\registry\\",
        L"\\.cargo\\git\\",
        L"\\.rustup\\toolchains\\",
        L"\\.nuget\\packages\\",
        L"\\.npm\\_cacache\\",
        L"\\.yarn\\cache\\",
        L"\\.pnpm-store\\",
        L"\\.bazel\\",
        L"\\bazel-out\\",
        L"\\bazel-bin\\",
        L"\\bazel-testlogs\\",
        L"\\.ccache\\",
        L"\\.dotnet\\",
        L"\\.gradle-cache\\",
        L"\\.vagrant\\",
        L"\\.terraform\\",
        L"\\.pulumi\\",

        // IDE caches
        L"\\.idea\\caches\\",
        L"\\.idea\\shelf\\",
        L"\\.idea\\workspace\\",
        L"\\jetbrains\\",
        L"\\.vscode-server\\",
        L"\\.vscode\\extensions\\",
        L"\\.cursor\\",

        // VCS internals (the dot-prefix loop catches \.git\ etc, but
        // these are noisy enough to call out explicitly)
        L"\\.git\\objects\\",
        L"\\.git\\refs\\",
        L"\\.git\\logs\\",
        L"\\.git\\index",
        L"\\.git\\head",
        L"\\.git\\packed-refs",

        // Container / VM I/O -- the host sees enormous overlay activity
        L"\\docker\\containers\\",
        L"\\docker\\overlay2\\",
        L"\\docker\\image\\",
        L"\\docker\\volumes\\",
        L"\\.docker\\desktop\\",
        L"\\hyper-v\\",
        L"\\virtualization\\",
        L"\\rootfs\\",                  // WSL distro root
        L"\\wsl$\\",                    // WSL UNC path inside an NT path
        L"\\wsl.localhost\\",
        L"\\appcontainerprofiles\\",

        // Cloud sync clients (high-volume background reconciliation)
        L"\\onedrive\\.849c9593-d756-4e56-8d6e-42412f2a707b\\", // staging
        L"\\onedrive\\setup\\",
        L"\\onedrive\\filecoauth\\",
        L"\\onedrivetemp\\",
        L"\\dropbox\\.dropbox.cache\\",
        L"\\dropbox\\config\\",
        L"\\dropbox\\instance",
        L"\\google\\drive\\",
        L"\\googledriveFS\\",
        L"\\box\\",                     // matches Box.com sync directory
        L"\\sharepoint\\",

        // PowerShell module discovery: PowerShell auto-loads modules on
        // shell startup and on first command, enumerating every .psd1 /
        // .psm1 / .ps1xml under PSModulePath. None of this is user-
        // initiated. The substrings below catch:
        //   * System: \Program Files\WindowsPowerShell\Modules\,
        //             \Program Files\PowerShell\7\Modules\
        //             (already filtered by \program files\, but kept for
        //              robustness against custom install roots)
        //   * User:   \Users\<u>\Documents\WindowsPowerShell\Modules\
        //             \Users\<u>\Documents\PowerShell\Modules\
        //   * OneDrive-synced user profile (the case the user reported):
        //             \Users\<u>\OneDrive - Org\Documents\WindowsPowerShell\Modules
        L"\\windowspowershell\\modules\\",
        L"\\powershell\\modules\\",
    };

    for (const auto* dir : goopDirs)
    {
        if (lp.find(dir) != std::wstring::npos)
            return true;
    }

    // =====================================================================
    // 2. Directory-suffix matching (path ends with folder name, no trailing \)
    //    Catches the directory *itself* being opened (e.g. via ETW).
    // =====================================================================
    static const wchar_t* const goopDirSuffixes[] = {
        // Cat 1
        L"\\windows",
        // Cat 2
        L"\\program files",
        L"\\program files (x86)",
        // Cat 3
        L"\\programdata",
        // Cat 4
        L"\\appdata",
        // Cat 5
        L"\\system volume information",
        L"\\$recycle.bin",
        L"\\recovery",
        L"\\$windows.~bt",
        L"\\$windows.~ws",
        L"\\$windows.~q",
        L"\\$winreagent",
        L"\\$getcurrent",
        L"\\$sysreset",
        L"\\config.msi",
        L"\\msocache",
        L"\\perflogs",
        L"\\dfsrprivate",
        // Cat 6
        L"\\node_modules",
        L"\\__pycache__",
        L"\\bower_components",
        L"\\obj",
        L"\\target",
        L"\\cmakefiles",
        L"\\pods",
        L"\\venv",
        L"\\env",
        L"\\coverage",
        // Cat 7
        L"\\application data",
        L"\\local settings",
        L"\\cookies",
        L"\\nethood",
        L"\\printhood",
        L"\\recent",
        L"\\sendto",
        L"\\start menu",
        L"\\templates",
        L"\\my documents",
        L"\\microsoftedgebackups",
        L"\\intelgraphicsprofiles",
        // Cat 8
        L"\\cache",
        L"\\cachestorage",
        L"\\code cache",
        L"\\gpucache",
        L"\\dawncache",
        L"\\grshadercache",
        L"\\shadercache",
        L"\\crash reports",
        L"\\crashdumps",
        L"\\crashpad",
        L"\\blob_storage",
        L"\\indexeddb",
        L"\\local storage",
        L"\\session storage",
        L"\\service worker",
        L"\\webstorage",
        L"\\databases",
        L"\\logs",
        L"\\log",
        L"\\temp",
        L"\\tmp",
        // Cat 9
        L"\\windows.old",
        L"\\onedrivetemp",
        L"\\inetpub",
        // Additional
        L"\\windowsapps",
        L"\\intel",
        L"\\amd",
        L"\\nvidia",
        L"\\boot",
        L"\\servicing",
        L"\\winsxs",
    };

    for (const auto* dn : goopDirSuffixes)
    {
        size_t dnLen = wcslen(dn);
        if (lp.size() >= dnLen && lp.compare(lp.size() - dnLen, dnLen, dn) == 0)
            return true;
    }

    // =====================================================================
    // 3. Dot-prefixed folder names at any depth (Cat 6 catch-all)
    //    Matches \.git\, \.vs\, \.ssh\, \.vscode\, \.hg\, \.svn\, \.cargo\,
    //    \.gradle\, \.m2\, \.nuget\, \.npm\, \.yarn\, \.conda\, \.tox\, etc.
    // =====================================================================
    {
        size_t pos = 0;
        while ((pos = lp.find(L"\\.", pos)) != std::wstring::npos)
        {
            if (pos + 2 < lp.size() && lp[pos + 2] != L'\\')
                return true;
            pos += 2;
        }
    }

    // =====================================================================
    // 4. CHKDSK recovered-fragment folders (Cat 5): \found.000\ .. \found.999\
    // =====================================================================
    if (lp.find(L"\\found.") != std::wstring::npos)
    {
        size_t fp = 0;
        while ((fp = lp.find(L"\\found.", fp)) != std::wstring::npos)
        {
            size_t numStart = fp + 7; // length of "\\found."
            if (numStart + 3 <= lp.size())
            {
                bool allDigits = true;
                for (size_t d = 0; d < 3 && numStart + d < lp.size(); ++d)
                {
                    wchar_t ch = lp[numStart + d];
                    if (ch < L'0' || ch > L'9') { allDigits = false; break; }
                }
                if (allDigits)
                {
                    size_t afterDigits = numStart + 3;
                    if (afterDigits >= lp.size() || lp[afterDigits] == L'\\')
                        return true;
                }
            }
            fp += 7;
        }
    }

    // =====================================================================
    // 5. Excluded file extensions
    // =====================================================================
    static const wchar_t* const excludedExts[] = {
        L".exe", L".dll", L".sys", L".drv", L".ocx",
        L".tmp", L".temp", L".etl", L".evtx",
        L".pf", L".cache", L".diagsession",
        L".obj", L".pch", L".ipch", L".ilk", L".pdb",
        L".tlog", L".idb", L".res", L".aps",
        L".suo", L".sdf", L".opensdf",
        L".log", L".log1", L".bak",
        L".lock", L".lck",
        L".db", L".db-wal", L".db-shm", L".sqlite",
        L".dat", L".metadata",
        L".blf", L".regtrans-ms",
        L".mui", L".cat", L".man", L".mof",
        L".nls", L".ttf", L".ttc", L".otf",
        L".efi", L".wim",
        L".jar",
        L"thumbs.db", L"desktop.ini",
        L".partial", L".crdownload", L".opdownload",
        L".journal", L".chk",
        L".dmp", L".hdmp", L".mdmp",
        L".diagcab", L".diagpkg",
        L".manifest",
        L".pol",
        // VM / container disk images -- enormous, opaque, never user-meaningful
        L".vhd", L".vhdx", L".vmdk", L".vdi", L".qcow2",
        L".iso", L".img",
        // Build / cache artifacts not yet listed
        L".o", L".a", L".lib", L".so", L".dylib",
        L".class", L".pyc", L".pyo",
        L".rlib", L".rmeta",       // Rust artifacts
        L".gch",                   // GCC precompiled header
        L".d",                     // make-dep file
        L".map",                   // linker map
        L".swo", L".swp",          // Vim swap
        // ETW / profiler outputs
        L".btr", L".vsp", L".vspx",

        // PowerShell framework files. These are declarative (manifests,
        // type/format definitions, console / session / role config) and
        // are written by module-author tooling such as
        // New-ModuleManifest -- not by interactive editing. They surface
        // here only because PowerShell's auto-load enumerates every
        // module on PSModulePath at shell startup. .psm1 (script module)
        // is intentionally NOT excluded by extension because authored
        // module work would touch them; the \windowspowershell\modules\
        // path filter above suppresses .psm1 noise from third-party
        // modules without dropping legitimate authoring activity.
        L".psd1", L".ps1xml", L".psc1", L".cdxml", L".psrc", L".pssc",

        // .NET diagnostic tool outputs (dotnet-trace, dotnet-gcdump,
        // dotnet-counters with --output --format nettrace, etc.).
        L".nettrace", L".gcdump", L".netperf",

        // Outlook ancillary files (NOT mail content):
        //   .nst -- Outlook Quick Search Database (search index)
        //   .oab -- Offline Address Book (auto-synced)
        // These live under \AppData\Local\Microsoft\Outlook\ where the
        // extension-gated allowlist now refuses to admit them, but we
        // exclude by extension too as defense-in-depth.
        L".nst", L".oab",
    };

    size_t dotPos = lp.rfind(L'.');
    if (dotPos != std::wstring::npos)
    {
        std::wstring ext = lp.substr(dotPos);
        for (const auto* e : excludedExts)
        {
            if (ext == e)
                return true;
        }
    }

    // =====================================================================
    // 6. Known system / NTFS metadata filenames (Cat 7, 10)
    // =====================================================================
    size_t slashPos = lp.rfind(L'\\');
    if (slashPos != std::wstring::npos)
    {
        std::wstring filename = lp.substr(slashPos + 1);

        // Cat 10: NTFS metadata artifacts
        if (!filename.empty() && filename[0] == L'$')
        {
            static const wchar_t* const ntfsFiles[] = {
                L"$mft", L"$mftmirr", L"$logfile", L"$bitmap",
                L"$boot", L"$badclus", L"$secure", L"$upcase",
                L"$attrdef", L"$extend",
            };
            for (const auto* nf : ntfsFiles)
            {
                if (filename == nf)
                    return true;
            }
        }

        // Well-known system files (Cat 7 + legacy)
        if (filename == L"thumbs.db" ||
            filename == L"desktop.ini" ||
            filename == L"ntuser.dat" ||
            filename == L"usrclass.dat" ||
            filename == L"pagefile.sys" ||
            filename == L"swapfile.sys" ||
            filename == L"hiberfil.sys" ||
            filename == L"bootmgr" ||
            filename == L"bootnxt" ||
            filename == L"autoexec.bat" ||
            filename == L"config.sys" ||
            filename == L"iconcache.db")
        {
            return true;
        }

        // Cat 7: NTUSER.DAT* and ntuser.* pattern (user registry hives)
        if (filename.size() >= 10 && filename.compare(0, 10, L"ntuser.dat") == 0)
            return true;
        if (filename.size() >= 7 && filename.compare(0, 7, L"ntuser.") == 0)
            return true;

        // Office lock files (e.g. ~$Document.docx)
        if (filename.size() > 2 && filename[0] == L'~' && filename[1] == L'$')
            return true;

        // Generic temp files (e.g. ~DF1234.tmp, ~WRS{...}.tmp)
        if (!filename.empty() && filename[0] == L'~')
            return true;

        // .NET diagnostic tool outputs that land at the user profile
        // root (or any working dir): dotnet-diagnostic-{pid} marker
        // files, dotnet-trace / dotnet-dump / dotnet-counters /
        // dotnet-gcdump / dotnet-stack / dotnet-monitor / dotnet-symbol
        // / dotnet-sos artifact files. These are emitted by SDK
        // diagnostic IPC and command-line tooling, never by the user.
        static const wchar_t* const dotnetDiagPrefixes[] = {
            L"dotnet-diagnostic-",
            L"dotnet-counters-",
            L"dotnet-trace-",
            L"dotnet-dump-",
            L"dotnet-gcdump-",
            L"dotnet-stack-",
            L"dotnet-monitor-",
            L"dotnet-symbol-",
            L"dotnet-sos-",
        };
        for (const auto* p : dotnetDiagPrefixes)
        {
            size_t pl = wcslen(p);
            if (filename.size() >= pl &&
                filename.compare(0, pl, p) == 0)
                return true;
        }

        // Generic diagnostic / telemetry dump suffix.
        // Catches OTEL_DIAGNOSTICS.json (OpenTelemetry .NET auto-
        // instrumentation), Azure SDK *_diagnostics.json drops, and
        // similar SDK-emitted diagnostic markers. These are auto-
        // generated when an SDK detects a startup or runtime fault,
        // not authored content.
        static const wchar_t* const diagSuffixes[] = {
            L"_diagnostics.json",
            L"-diagnostics.json",
            L".diagnostics.json",
        };
        for (const auto* sfx : diagSuffixes)
        {
            size_t sl = wcslen(sfx);
            if (filename.size() >= sl &&
                filename.compare(filename.size() - sl, sl, sfx) == 0)
                return true;
        }
    }

    return false;
}

FileMonitor::FileMonitor()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

FileMonitor::~FileMonitor()
{
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

void FileMonitor::SetCallback(FileActivityCallback cb)
{
    m_callback = std::move(cb);
}

std::vector<std::wstring> FileMonitor::GetDriveRoots()
{
    // -------------------------------------------------------------------
    // RDCW (ReadDirectoryChangesW) policy:
    //   * Fixed drives (DRIVE_FIXED): SKIP. The Microsoft-Windows-Kernel-File
    //     ETW provider already gives us every Create/Write/Delete/Rename
    //     event on every NTFS volume and gives us the source PID. Running
    //     RDCW recursively on C:\ and the like was the dominant source of
    //     duplicate events (RDCW + ETW + shell notifications would all
    //     fire for the same write) AND a major battery/CPU regressor.
    //   * Removable drives (DRIVE_REMOVABLE) and network mounts
    //     (DRIVE_REMOTE): KEEP. ETW for removable USB volumes can be
    //     spotty (the kernel-file provider may or may not fire on
    //     hot-plugged FAT32 volumes depending on the FS filter stack), and
    //     network redirector traffic does not always surface with a usable
    //     device-path mapping. RDCW remains the safety net for those.
    // -------------------------------------------------------------------
    std::vector<std::wstring> roots;
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
    {
        if (drives & (1 << i))
        {
            wchar_t root[4] = { static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0' };
            UINT type = GetDriveTypeW(root);
            if (type == DRIVE_REMOVABLE || type == DRIVE_REMOTE)
            {
                roots.push_back(root);
            }
        }
    }
    return roots;
}

void FileMonitor::Start()
{
    if (m_running) return;
    m_running = true;
    ResetEvent(m_stopEvent);

    auto roots = GetDriveRoots();
    for (const auto& root : roots)
    {
        m_threads.emplace_back(&FileMonitor::MonitorDrive, this, root);
    }

    // Also start a shell notification watcher thread
    m_threads.emplace_back(&FileMonitor::MonitorShellNotifications, this);

    // Start ETW trace for file-open events
    m_threads.emplace_back(&FileMonitor::StartEtwTrace, this);
}

void FileMonitor::Stop()
{
    if (!m_running) return;
    m_running = false;
    SetEvent(m_stopEvent);

    // Stop ETW trace so ProcessTrace unblocks
    StopEtwTrace();

    for (auto& t : m_threads)
    {
        if (t.joinable()) t.join();
    }
    m_threads.clear();
}

void FileMonitor::Pause()
{
    m_paused = true;
}

void FileMonitor::Resume()
{
    m_paused = false;
}

void FileMonitor::MonitorDrive(const std::wstring& root)
{
    HANDLE hDir = CreateFileW(
        root.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (hDir == INVALID_HANDLE_VALUE)
        return;

    const DWORD bufSize = 64 * 1024;
    std::vector<BYTE> buffer(bufSize);

    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent)
    {
        CloseHandle(hDir);
        return;
    }

    HANDLE waitHandles[2] = { overlapped.hEvent, m_stopEvent };

    // Only watch for user-visible file operations.
    // Excluded: ATTRIBUTES, SIZE, SECURITY � these fire constantly from
    // search indexing, antivirus scans, system metadata updates, etc.
    const DWORD filter =
        FILE_NOTIFY_CHANGE_FILE_NAME |
        FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_CREATION;

    while (m_running)
    {
        ResetEvent(overlapped.hEvent);
        DWORD bytesReturned = 0;

        BOOL ok = ReadDirectoryChangesW(
            hDir, buffer.data(), bufSize, TRUE,
            filter, &bytesReturned, &overlapped, nullptr);

        if (!ok)
            break;

        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0 + 1) // stop event
            break;
        if (waitResult != WAIT_OBJECT_0)
            break;

        if (!GetOverlappedResult(hDir, &overlapped, &bytesReturned, FALSE))
            continue;
        if (bytesReturned == 0)
            continue;

        if (m_paused)
            continue;

        BYTE* ptr = buffer.data();
        while (true)
        {
            auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(ptr);
            std::wstring fileName(info->FileName, info->FileNameLength / sizeof(wchar_t));
            std::wstring fullPath = root + fileName;

            std::wstring action;
            std::wstring oldPath;

            switch (info->Action)
            {
            case FILE_ACTION_ADDED:
                action = L"CREATE";
                break;
            case FILE_ACTION_REMOVED:
                action = L"DELETE";
                break;
            case FILE_ACTION_MODIFIED:
                action = L"MODIFY";
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                oldPath = fullPath;
                // The next entry will be RENAMED_NEW_NAME
                if (info->NextEntryOffset != 0)
                {
                    auto* next = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(ptr + info->NextEntryOffset);
                    if (next->Action == FILE_ACTION_RENAMED_NEW_NAME)
                    {
                        std::wstring newName(next->FileName, next->FileNameLength / sizeof(wchar_t));
                        fullPath = root + newName;
                        action = L"RENAME";
                    }
                }
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                // Handled as part of OLD_NAME above; skip standalone
                action.clear();
                break;
            default:
                action.clear();
                break;
            }

            if (!action.empty() && m_callback &&
                !ShouldExclude(fullPath) &&
                (oldPath.empty() || !ShouldExclude(oldPath)))
            {
                // RDCW gives us no PID -- producer must guess. Carry an
                // unattributed context (sourcePid=0) plus current foreground.
                EventContext ctx = EventContextUtil::CaptureContext(0);
                m_callback(action, fullPath, oldPath, ctx);
            }

            if (info->NextEntryOffset == 0)
                break;

            // Skip ahead � but if we already consumed the next entry for a rename, skip it
            if (info->Action == FILE_ACTION_RENAMED_OLD_NAME && info->NextEntryOffset != 0)
            {
                auto* next = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(ptr + info->NextEntryOffset);
                if (next->Action == FILE_ACTION_RENAMED_NEW_NAME)
                {
                    if (next->NextEntryOffset == 0)
                        break;
                    ptr = ptr + info->NextEntryOffset + next->NextEntryOffset;
                    continue;
                }
            }

            ptr += info->NextEntryOffset;
        }
    }

    CancelIo(hDir);
    CloseHandle(overlapped.hEvent);
    CloseHandle(hDir);
}

void FileMonitor::MonitorShellNotifications()
{
    // Use SHChangeNotifyRegister to catch shell-level operations (copy, move, etc.)
    // This requires a hidden message-only window.

    const wchar_t* className = L"WarpShellMonWnd";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hWnd) return;

    SHChangeNotifyEntry entries[1] = {};
    PIDLIST_ABSOLUTE pidlDesktop = nullptr;
    SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &pidlDesktop);

    // Watch entire namespace
    LPITEMIDLIST pidlRoot = nullptr;
    SHGetFolderLocation(nullptr, CSIDL_DESKTOP, nullptr, 0, &pidlRoot);

    entries[0].pidl = pidlRoot;
    entries[0].fRecursive = TRUE;

    ULONG regId = SHChangeNotifyRegister(
        hWnd,
        SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
        SHCNE_ALLEVENTS,
        WM_USER + 100,
        1, entries);

    MSG msg;
    while (m_running)
    {
        // Use MsgWaitForMultipleObjects so we can also detect the stop event
        DWORD result = MsgWaitForMultipleObjects(1, &m_stopEvent, FALSE, 500, QS_ALLINPUT);
        if (result == WAIT_OBJECT_0) // stop event
            break;

        while (PeekMessageW(&msg, hWnd, 0, 0, PM_REMOVE))
        {
            if (m_paused) continue;

            if (msg.message == WM_USER + 100)
            {
                LONG event = 0;
                PIDLIST_ABSOLUTE* pidls = nullptr;
                HANDLE hLock = SHChangeNotification_Lock(
                    reinterpret_cast<HANDLE>(msg.wParam),
                    static_cast<DWORD>(msg.lParam), &pidls, &event);

                if (hLock)
                {
                    std::wstring action;
                    std::wstring path;
                    std::wstring oldPath;

                    wchar_t szPath1[MAX_PATH] = {};
                    wchar_t szPath2[MAX_PATH] = {};

                    if (pidls && pidls[0])
                        SHGetPathFromIDListW(pidls[0], szPath1);
                    if (pidls && pidls[1])
                        SHGetPathFromIDListW(pidls[1], szPath2);

                    switch (event)
                    {
                    case SHCNE_CREATE:
                    case SHCNE_MKDIR:
                        action = L"CREATE";
                        path = szPath1;
                        break;
                    case SHCNE_DELETE:
                    case SHCNE_RMDIR:
                        action = L"DELETE";
                        path = szPath1;
                        break;
                    case SHCNE_RENAMEITEM:
                    case SHCNE_RENAMEFOLDER:
                        action = L"RENAME";
                        oldPath = szPath1;
                        path = szPath2;
                        break;
                    case SHCNE_UPDATEITEM:
                    case SHCNE_UPDATEDIR:
                        action = L"MODIFY";
                        path = szPath1;
                        break;
                    case SHCNE_ATTRIBUTES:
                        action = L"MODIFY";
                        path = szPath1;
                        break;
                    default:
                        break;
                    }

                    if (!action.empty() && !path.empty() && m_callback &&
                        !ShouldExclude(path) &&
                        (oldPath.empty() || !ShouldExclude(oldPath)))
                    {
                        // SHChangeNotifyRegister doesn't give us a PID either;
                        // the shell hides the originating process behind
                        // its broker. Capture neutral source, real foreground.
                        EventContext ctx = EventContextUtil::CaptureContext(0);
                        m_callback(action, path, oldPath, ctx);
                    }

                    SHChangeNotification_Unlock(hLock);
                }
            }

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (regId)
        SHChangeNotifyDeregister(regId);

    if (pidlRoot)
        CoTaskMemFree(pidlRoot);
    if (pidlDesktop)
        CoTaskMemFree(pidlDesktop);

    DestroyWindow(hWnd);
    UnregisterClassW(className, wc.hInstance);
}

// ---------- ETW: Capture file events via Microsoft-Windows-Kernel-File ----------
//
// Why we no longer use the NT Kernel Logger:
//   * Singleton: only ONE consumer in the whole system can run it at a time.
//     Asking for it directly conflicted with EDR products (Defender for
//     Endpoint, CrowdStrike, Sysmon...) that already own the session, and
//     the previous code's "stop the existing session and start ours" was
//     a real footgun that DoSed those tools while WARP was running.
//   * Opcode-64 hand-parsing of the FileIo create record was brittle to
//     32/64-bit and to layout changes; the Microsoft-Windows-Kernel-File
//     manifested provider exposes well-defined event IDs (12 = Create,
//     14 = CreateNewFile, 15 = SetInformation, 16 = SetDelete, 22 = Read,
//     23 = Write, 26 = DeletePath, 27 = RenamePath) with stable layout.
//   * Private session lets us live alongside any other consumer.
//
// EnableTraceEx2 with a keyword mask scopes the firehose at provider level
// (we ask for KERNEL_FILE_KEYWORD_FILEIO 0x10 plus the four primitive op
// keywords) so the kernel never even forwards uninteresting events to us.

// Microsoft-Windows-Kernel-File provider GUID
//   {EDD08927-9CC4-4E65-B970-C2560FB5C289}
static const GUID KernelFileProviderGuid =
    { 0xEDD08927, 0x9CC4, 0x4E65, { 0xB9, 0x70, 0xC2, 0x56, 0x0F, 0xB5, 0xC2, 0x89 } };

// Keyword bits (from Microsoft-Windows-Kernel-File manifest)
static const ULONGLONG KERNEL_FILE_KEYWORD_FILENAME       = 0x10;
static const ULONGLONG KERNEL_FILE_KEYWORD_FILEIO         = 0x20;
static const ULONGLONG KERNEL_FILE_KEYWORD_OP_END         = 0x40;
static const ULONGLONG KERNEL_FILE_KEYWORD_CREATE         = 0x80;
static const ULONGLONG KERNEL_FILE_KEYWORD_READ           = 0x100;
static const ULONGLONG KERNEL_FILE_KEYWORD_WRITE          = 0x200;
static const ULONGLONG KERNEL_FILE_KEYWORD_DELETE_PATH    = 0x400;
static const ULONGLONG KERNEL_FILE_KEYWORD_RENAME_SETLINK = 0x800;
static const ULONGLONG KERNEL_FILE_KEYWORD_CREATE_NEW_FILE = 0x1000;

// Manifest event IDs we care about
enum KernelFileEventId : USHORT
{
    KFE_Create        = 12,    // Open existing file (NtCreateFile / NtOpenFile)
    KFE_CreateNewFile = 30,    // FILE_CREATE / FILE_OVERWRITE_IF (true creation)
    KFE_SetInfo       = 15,    // SetFileInformation (size/timestamps/etc.)
    KFE_SetDelete     = 16,    // FileDispositionInformation marking pending delete
    KFE_Rename        = 19,    // SetFileInformation(Rename) without target path
    KFE_DeletePath    = 26,    // Path-resolved delete (preferred for our purposes)
    KFE_RenamePath    = 27,    // Path-resolved rename (carries old + new path)
    KFE_Write         = 16,    // Write completion is a different op than SetDelete?
    // NOTE: 16 is overloaded across versions; we dispatch by EventDescriptor.Task
    // when in doubt (see the callback). For the strategy in this commit we
    // primarily consume Create / CreateNewFile / DeletePath / RenamePath.
};

void FileMonitor::StartEtwTrace()
{
    g_pFileMonitor = this;
    g_ownPid = GetCurrentProcessId();

    // Always tear down our previous session (if a prior WARP run crashed).
    // CRITICAL: this ONLY targets our private "WARP-FileTrace" session by
    // name -- it never touches "NT Kernel Logger" or other consumers.
    StopEtwTrace();

    static const wchar_t* const kSessionName = L"WARP-FileTrace";

    const size_t sessionNameBytes = (wcslen(kSessionName) + 1) * sizeof(wchar_t);
    const size_t bufSize = sizeof(EVENT_TRACE_PROPERTIES) + sessionNameBytes + 1024;
    std::vector<BYTE> propsBuf(bufSize, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuf.data());

    props->Wnode.BufferSize    = static_cast<ULONG>(bufSize);
    props->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1;                       // QPC clock
    // Wnode.Guid for a PRIVATE session is just a unique identifier we
    // pick (any non-system GUID works; do NOT use SystemTraceControlGuid).
    // This GUID was generated for WARP and never shipped.
    static const GUID WarpFileSessionGuid =
        { 0x9d2c97a3, 0x6e57, 0x4d2b, { 0xa9, 0xc1, 0x21, 0xf6, 0xc6, 0x67, 0x12, 0x55 } };
    props->Wnode.Guid          = WarpFileSessionGuid;
    props->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
    props->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);
    props->BufferSize          = 64;                      // KB; small bursts OK
    props->MinimumBuffers      = 4;
    props->MaximumBuffers      = 16;
    props->FlushTimer          = 1;                       // seconds
    // EnableFlags is ignored for private/manifested-provider sessions; the
    // kernel-file provider is enabled via EnableTraceEx2 below.

    ULONG status = StartTraceW(&m_etwSessionHandle, kSessionName, props);
    if (status == ERROR_ALREADY_EXISTS)
    {
        // Another WARP instance left a session behind. Stop ours and retry.
        ControlTraceW(0, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
        memset(propsBuf.data(), 0, bufSize);
        props->Wnode.BufferSize    = static_cast<ULONG>(bufSize);
        props->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
        props->Wnode.ClientContext = 1;
        props->Wnode.Guid          = WarpFileSessionGuid;
        props->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
        props->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);
        props->BufferSize          = 64;
        props->MinimumBuffers      = 4;
        props->MaximumBuffers      = 16;
        props->FlushTimer          = 1;
        status = StartTraceW(&m_etwSessionHandle, kSessionName, props);
    }
    if (status != ERROR_SUCCESS)
    {
        m_etwSessionHandle = 0;
        return;
    }

    // Subscribe to the manifested Microsoft-Windows-Kernel-File provider.
    // Keyword scoping: only the IRP completions we actually use, never
    // FILENAME/FILEIO bulk metadata events that fire on every open hint.
    const ULONGLONG keywords =
        KERNEL_FILE_KEYWORD_CREATE          |
        KERNEL_FILE_KEYWORD_CREATE_NEW_FILE |
        KERNEL_FILE_KEYWORD_DELETE_PATH     |
        KERNEL_FILE_KEYWORD_RENAME_SETLINK  |
        KERNEL_FILE_KEYWORD_WRITE;
    status = EnableTraceEx2(
        m_etwSessionHandle,
        &KernelFileProviderGuid,
        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
        TRACE_LEVEL_INFORMATION,
        keywords,
        0,                          // MatchAnyKeyword==keywords above; MatchAll=0
        0,                          // EnableProperty
        nullptr);                   // EnableParameters
    if (status != ERROR_SUCCESS)
    {
        // Most common failure here: the caller doesn't have SeSystemProfilePrivilege
        // or the provider isn't installed (Server Core w/o the relevant package).
        // Fail loud-but-soft: tear down the session so we don't leak it.
        StopEtwTrace();
        return;
    }

    // Open the trace for real-time consuming
    EVENT_TRACE_LOGFILEW logFile = {};
    logFile.LoggerName        = const_cast<LPWSTR>(kSessionName);
    logFile.ProcessTraceMode  = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = &FileMonitor::EtwEventCallback;

    m_etwTraceHandle = OpenTraceW(&logFile);
    if (m_etwTraceHandle == INVALID_PROCESSTRACE_HANDLE)
    {
        StopEtwTrace();
        return;
    }

    // ProcessTrace blocks until the session is stopped
    ProcessTrace(&m_etwTraceHandle, 1, nullptr, nullptr);

    if (m_etwTraceHandle != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(m_etwTraceHandle);
        m_etwTraceHandle = INVALID_PROCESSTRACE_HANDLE;
    }
}

void FileMonitor::StopEtwTrace()
{
    if (m_etwTraceHandle != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(m_etwTraceHandle);
        m_etwTraceHandle = INVALID_PROCESSTRACE_HANDLE;
    }

    static const wchar_t* const kSessionName = L"WARP-FileTrace";
    const size_t sessionNameBytes = (wcslen(kSessionName) + 1) * sizeof(wchar_t);
    const size_t bufSize = sizeof(EVENT_TRACE_PROPERTIES) + sessionNameBytes + 1024;
    std::vector<BYTE> propsBuf(bufSize, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuf.data());
    props->Wnode.BufferSize = static_cast<ULONG>(bufSize);
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    // ONLY stops the session we own. By design this no longer touches the
    // NT Kernel Logger, so EDR / Sysmon / other consumers are unaffected.
    ControlTraceW(0, kSessionName, props, EVENT_TRACE_CONTROL_STOP);
    m_etwSessionHandle = 0;
}

namespace
{
    // Microsoft-Windows-Kernel-File events of interest carry a single
    // wide-string FileName property. The simplest reliable extraction
    // strategy without dragging in the TDH library is to scan the
    // EVENT_RECORD UserData for the first sane null-terminated wide string
    // that lives at one of two known offsets in the supported manifest
    // versions. For the events we actually care about (Create, CreateNewFile,
    // DeletePath, RenamePath) the FileName field is always the first OR
    // second wide-string property. We try both offsets and return the one
    // that yields a path-shaped string.
    //
    // CreateArgs layout (Win10+):
    //   PVOID    Irp;             // 8
    //   PVOID    FileObject;      // 8
    //   PVOID    IssuingThreadId; // 8 (or 4 on x86)
    //   UINT32   CreateOptions;   // 4
    //   UINT32   CreateAttributes;// 4
    //   UINT32   ShareAccess;     // 4
    //   UNICODE_STRING FileName;  // length-prefixed, then chars
    //
    // We also handle the simpler RenamePath case (FileKey + FileName + new
    // FileName) and DeletePath (FileKey + FileName).

    // Helper: extract FileName from a manifested-provider EVENT_RECORD.
    // Returns empty string on failure.
    std::wstring ExtractFilenameFromKernelFileEvent(PEVENT_RECORD pEvent,
                                                    bool          wantSecondName,
                                                    std::wstring& outNewName)
    {
        outNewName.clear();
        if (!pEvent || !pEvent->UserData || pEvent->UserDataLength < 4)
            return L"";

        // We accept any null-terminated wide-string substring that looks
        // like a path AND is at least 4 chars long (e.g. "C:\\X" or
        // "\\Device\\..."). The manifest properties guarantee exactly one
        // such string per event for Create/CreateNewFile/DeletePath, and
        // exactly two for RenamePath (old then new).
        const BYTE*  data    = static_cast<const BYTE*>(pEvent->UserData);
        const size_t dataLen = pEvent->UserDataLength;

        std::wstring first, second;

        for (size_t off = 0; off + sizeof(wchar_t) <= dataLen; off += sizeof(wchar_t))
        {
            // Look for wstring start: a printable wide char where alignment is even.
            const wchar_t* p = reinterpret_cast<const wchar_t*>(data + off);
            wchar_t c = *p;
            if (c == L'\\' || c == L'C' || c == L'D' || c == L'/' ||
                (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'))
            {
                size_t maxChars = (dataLen - off) / sizeof(wchar_t);
                size_t len = 0;
                while (len < maxChars && p[len] >= 0x20 && p[len] < 0xFFFF)
                    ++len;
                // Need null terminator inside data
                if (len == maxChars) continue;
                if (len < 4)         continue;

                // Path-shape sanity: contains '\\' or starts with X:.
                std::wstring candidate(p, len);
                bool looksLikePath =
                    candidate.find(L'\\') != std::wstring::npos ||
                    (candidate.size() >= 2 && candidate[1] == L':');
                if (!looksLikePath) continue;

                if (first.empty())
                {
                    first = std::move(candidate);
                    if (!wantSecondName) break;
                    off += (len + 1) * sizeof(wchar_t);
                    off -= sizeof(wchar_t);     // loop will += again
                }
                else if (second.empty())
                {
                    second = std::move(candidate);
                    break;
                }
            }
        }

        outNewName = std::move(second);
        return first;
    }
}

void WINAPI FileMonitor::EtwEventCallback(PEVENT_RECORD pEvent)
{
    FileMonitor* self = g_pFileMonitor;
    if (!self || !self->m_running || self->m_paused || !self->m_callback)
        return;

    // We subscribe ONLY to Microsoft-Windows-Kernel-File now; defensive check.
    if (!IsEqualGUID(pEvent->EventHeader.ProviderId, KernelFileProviderGuid))
        return;

    // Skip events from our own process
    if (pEvent->EventHeader.ProcessId == g_ownPid)
        return;

    if (!IsUserProcess(pEvent->EventHeader.ProcessId))
        return;

    // Dispatch by manifest event ID. Unknown IDs are ignored (the kernel
    // never sends them at our keyword level, but defensive).
    const USHORT id = pEvent->EventHeader.EventDescriptor.Id;
    std::wstring action;
    bool wantSecond = false;
    switch (id)
    {
        case 12:    // Create (open)
            action = L"OPEN";
            break;
        case 30:    // CreateNewFile
            action = L"CREATE";
            break;
        case 26:    // DeletePath
            action = L"DELETE";
            break;
        case 27:    // RenamePath
            action = L"RENAME";
            wantSecond = true;
            break;
        case 23:    // Write completion (Write keyword)
            action = L"MODIFY";
            break;
        default:
            return;
    }

    std::wstring newName;
    std::wstring filePath = ExtractFilenameFromKernelFileEvent(pEvent, wantSecond, newName);
    if (filePath.empty())
        return;

    // Resolve kernel device paths (\Device\HarddiskVolumeN\...) to DOS paths.
    auto Resolve = [](std::wstring& p) -> bool {
        if (p.size() > 8 && p[0] == L'\\')
        {
            size_t thirdSlash = p.find(L'\\', 8);
            if (thirdSlash == std::wstring::npos) return false;

            std::wstring devicePart = p.substr(0, thirdSlash);
            std::transform(devicePart.begin(), devicePart.end(), devicePart.begin(), ::towlower);

            std::call_once(g_deviceMapOnce, BuildDeviceMap);
            auto it = g_deviceToDrive.find(devicePart);
            if (it == g_deviceToDrive.end()) return false;

            p = it->second + p.substr(thirdSlash);
            return true;
        }
        else if (p.size() > 2 && p[1] == L':')
        {
            return true;
        }
        return false;
    };

    if (!Resolve(filePath)) return;
    if (!newName.empty() && !Resolve(newName))
    {
        // We have a rename but couldn't resolve the new name; fall back to delete
        // semantics on the old name.
        action = L"DELETE";
        newName.clear();
    }

    // Skip bare drive roots (e.g. "C:\") -- not meaningful user activity
    if (filePath.size() <= 3) return;

    // Normalize: strip trailing backslash for consistent dedup and exclusion
    if (filePath.size() > 3 && filePath.back() == L'\\')
        filePath.pop_back();

    // Apply exclusion filter
    extern bool ShouldExclude(const std::wstring& path);
    if (ShouldExclude(filePath))
        return;
    if (!newName.empty() && ShouldExclude(newName))
        return;

    // Deduplicate: suppress repeated events for the same path within 2 seconds
    {
        static std::mutex dedup_mtx;
        static std::unordered_map<std::wstring, ULONGLONG> dedup_map;
        static ULONGLONG dedup_lastCleanup = 0;

        ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lock(dedup_mtx);

        if (now - dedup_lastCleanup > 30000)
        {
            for (auto it = dedup_map.begin(); it != dedup_map.end(); )
            {
                if (now - it->second > 5000)
                    it = dedup_map.erase(it);
                else
                    ++it;
            }
            dedup_lastCleanup = now;
        }

        std::wstring key = filePath + L"|" + action;
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);

        auto it = dedup_map.find(key);
        if (it != dedup_map.end() && (now - it->second) < 2000)
        {
            it->second = now;
            return;
        }
        dedup_map[key] = now;
    }

    EventContext ctx = EventContextUtil::CaptureContext(pEvent->EventHeader.ProcessId);

    // -----------------------------------------------------------------
    // Composite system-process verdict and per-PID burst suppression.
    //
    // (1) IsUserProcess() above only checked the session ID + a fixed
    //     name list. The composite SystemProcessClassifier additionally
    //     considers parent (services.exe / svchost), token integrity
    //     (high = installer / system), Authenticode signer (Microsoft),
    //     and image path (Windows tree). We consult it here using the
    //     resolved exe path captured by EventContext.
    //
    //     If classified as system, we don't drop the event -- some
    //     genuinely-user-driven activity flows through MS-signed
    //     helpers (cmd.exe redirecting into a file the user wants;
    //     Office crash recovery touching a doc). Instead we cap the
    //     event's confidence so InferenceEngine's threshold (>= 0.5)
    //     filters it out of popularity counts but the row still lands
    //     in the DB for forensic queries.
    //
    // (2) Per-PID token-bucket: a single PID firing > 50 events / sec
    //     is almost certainly noise (a build, a sync, an anti-virus
    //     scan, a scripted indexer). We refill at 50 tokens/sec with a
    //     burst capacity of 100 tokens. When the bucket runs dry, we
    //     downgrade further events to confidence 0.1 (DB-only). LRU-
    //     bound the bucket map at 1024 entries.
    // -----------------------------------------------------------------
    {
        WARP::ClassificationResult cls =
            WARP::SystemProcessClassifier::Instance().Classify(
                ctx.sourceExe, pEvent->EventHeader.ProcessId);
        if (cls.isSystem)
            ctx.confidence = (std::min)(ctx.confidence, 0.4);
    }

    {
        struct Bucket { ULONGLONG lastRefillTick; double tokens; };
        constexpr double kCap        = 100.0;
        constexpr double kRefillRate = 50.0;        // tokens / sec
        constexpr size_t kMaxPids    = 1024;

        static std::mutex                            burstMtx;
        static std::unordered_map<DWORD, Bucket>     buckets;
        static std::deque<DWORD>                     lru;

        DWORD pid = pEvent->EventHeader.ProcessId;
        ULONGLONG nowTick = GetTickCount64();

        std::lock_guard<std::mutex> lk(burstMtx);
        auto it = buckets.find(pid);
        if (it == buckets.end())
        {
            if (buckets.size() >= kMaxPids && !lru.empty())
            {
                buckets.erase(lru.front());
                lru.pop_front();
            }
            buckets[pid] = { nowTick, kCap };
            lru.push_back(pid);
            it = buckets.find(pid);
        }

        Bucket& b = it->second;
        double elapsedSec = (nowTick - b.lastRefillTick) / 1000.0;
        b.tokens = (std::min)(kCap, b.tokens + elapsedSec * kRefillRate);
        b.lastRefillTick = nowTick;

        if (b.tokens < 1.0)
            ctx.confidence = (std::min)(ctx.confidence, 0.1);
        else
            b.tokens -= 1.0;
    }

    // For RENAME the consumer expects (action="RENAME", path=NEW, oldPath=OLD).
    if (action == L"RENAME" && !newName.empty())
        self->m_callback(action, newName, filePath, ctx);
    else
        self->m_callback(action, filePath, L"", ctx);
}
