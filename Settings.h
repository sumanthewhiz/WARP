// Settings: effective WARP configuration, privacy exclusions and pause state.
// Layers (later wins): defaults, HKCU\Software\WARP, HKLM\Software\Policies\WARP
// (IT policy, 64-bit view; such values are locked). All members are thread-safe.
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <shared_mutex>
#include <utility>

struct WarpSettings
{
    // --- capture toggles ---
    bool captureFiles       = true;
    bool captureAppLaunch   = true;
    bool captureFocus       = true;
    bool captureWeb         = true;
    bool capturePresence    = true;
    bool captureClipboardMetadata = false;   // metadata only (source app + formats), never content
    bool capturePrint       = true;
    bool captureUsnJournal  = true;
    bool captureRecentItems = true;
    bool captureOfficeMru   = true;
    // --- privacy ---
    bool dropPrivateBrowsing = true;          // InPrivate / Incognito / Private Browsing windows are never recorded
    bool encryptDataDir      = true;          // best-effort EFS encryption of %LOCALAPPDATA%\WARP
    std::vector<std::wstring> excludedApps;           // lowercase exe file names, e.g. L"keepass.exe"
    std::vector<std::wstring> excludedTitlePatterns;  // case-insensitive substrings matched against window titles
    std::vector<std::wstring> excludedDomains;        // lowercase hosts; subdomains match
    std::vector<std::wstring> excludedPathPrefixes;   // case-insensitive path prefixes (boundary-aware)
    std::vector<std::wstring> urlQueryAllowlist;      // extra query keys kept in canonical URLs
    // --- retention ---
    int rawRetentionDays    = 30;     // raw event rows (clamped 1..3650)
    int entityRetentionDays = 395;    // derived per-entity aggregates / rollups / threads (clamped 7..3650)
    // --- presence ---
    int awayAfterSeconds    = 300;    // no input for this long (and no passive-media/meeting signal) => "away" (clamped 30..86400)
    // --- models / performance ---
    bool llmEnabled            = false;   // optional Qwen refiner (off by default: costly, no measured quality gain)
    bool llmAllowedOnBattery   = false;
    int  contextCadenceSeconds = 60;      // clamped 15..3600
    // --- API access control ---
    bool apiAllowAppContainerClients = false;
    std::vector<std::wstring> apiAllowedClients;     // lowercase exe file names; empty = any local client that passes the pipe DACL
    std::vector<std::wstring> apiActivityClients;    // lowercase exe names allowed the "activity" scope (titles/URLs/timeline) when apiRestrictActivityScope
    bool apiRestrictActivityScope = false;
    // --- state ---
    bool    consentAcknowledged = false;
    int64_t pausedUntilUtc      = 0;      // capture paused until this Unix time (0 = not paused; INT64_MAX = until resumed)
    // --- diagnostics ---
    int logLevel = 1;                     // 0 debug, 1 info, 2 warn, 3 error
};

class Settings
{
public:
    static Settings& Instance();

    // (Re)load: defaults <- HKCU\Software\WARP <- HKLM\Software\Policies\WARP (policy values win and are "locked").
    void Load();
    WarpSettings Get() const;                         // thread-safe snapshot copy
    bool IsLocked(const wchar_t* valueName) const;    // true if set by policy

    // Persisted user-level setters (HKCU\Software\WARP). Return false if locked by policy or on registry failure.
    // Also false for unknown names / type mismatch. SetInt clamps to range. Success reloads the snapshot.
    bool SetPausedUntil(int64_t utc);
    bool SetConsentAcknowledged(bool v);
    bool SetBool(const wchar_t* valueName, bool v);        // e.g. L"LlmEnabled"
    bool SetInt(const wchar_t* valueName, int v);
    bool SetList(const wchar_t* valueName, const std::vector<std::wstring>& v);

    bool IsPaused(int64_t nowUtc) const;

    // Matching helpers (use the current snapshot; all case-insensitive; never throw)
    bool IsExcludedApp(const std::wstring& exePathOrName) const;      // compares file name part
    bool IsExcludedTitle(const std::wstring& windowTitle) const;
    bool IsExcludedDomain(const std::wstring& host) const;            // exact or subdomain at '.' boundary
    bool IsExcludedPath(const std::wstring& path) const;              // prefix match at a path-separator boundary

    // Name/value pairs describing effective settings for a status/diagnostics endpoint
    // (lists rendered as "a;b;c", bools as "true"/"false"; append " (policy)" to value if locked).
    std::vector<std::pair<std::wstring, std::wstring>> Describe() const;

private:
    Settings();
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;

    mutable std::shared_mutex m_mtx;
    WarpSettings m_s;
    std::vector<std::wstring> m_locked;   // value names set by policy
};
