#include "framework.h"
#include "FileCapture.h"

#include "EntityKey.h"
#include "EventContext.h"
#include "Logger.h"
#include "OfficeMruReader.h"
#include "OneDriveMapper.h"
#include "RecentItemsWatcher.h"
#include "Settings.h"
#include "SystemProcessClassifier.h"
#include "UrlCanonicalizer.h"
#include "UsnJournalReader.h"
#include "ZoneIdentifier.h"

#include <evntprov.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <winioctl.h>
#include <winnetwk.h>
#include <cfgmgr32.h>
#include <knownfolders.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mpr.lib")
#pragma comment(lib, "cfgmgr32.lib")

#ifndef EVENT_ENABLE_PROPERTY_PROCESS_START_KEY
#define EVENT_ENABLE_PROPERTY_PROCESS_START_KEY 0x00000080
#endif
#ifndef EVENT_HEADER_EXT_TYPE_PROCESS_START_KEY
#define EVENT_HEADER_EXT_TYPE_PROCESS_START_KEY 0x000D
#endif

namespace
{
const char* const kComp = "FileCapture";

// Microsoft-Windows-Kernel-File {EDD08927-9CC4-4E65-B970-C2560FB5C289}
const GUID kKernelFileProvider =
    { 0xEDD08927, 0x9CC4, 0x4E65, { 0xB9, 0x70, 0xC2, 0x56, 0x0F, 0xB5, 0xC2, 0x89 } };
// In-band "real-time events/buffers lost" notifications {6A399AE0-4BC6-4DE9-870B-3657F8947E7E}
const GUID kRtLostEventGuid =
    { 0x6A399AE0, 0x4BC6, 0x4DE9, { 0x87, 0x0B, 0x36, 0x57, 0xF8, 0x94, 0x7E, 0x7E } };

// Kernel-File keywords. WRITE (0x200) is intentionally not enabled (FILE-1): edits come
// from create dispositions and the USN journal.
const ULONGLONG kKwFileName          = 0x10;
const ULONGLONG kKwOpEnd             = 0x40;
const ULONGLONG kKwCreate            = 0x80;
const ULONGLONG kKwDeletePath        = 0x400;
const ULONGLONG kKwRenameSetLinkPath = 0x800;

const USHORT kEvCreate       = 12;
const USHORT kEvOperationEnd = 24;
const USHORT kEvDeletePath   = 26;
const USHORT kEvRenamePath   = 27;

// Create.CreateOptions: low 24 bits are NtCreateFile options, the high byte the disposition.
const uint64_t kFileDirectoryFile         = 0x00000001;
const uint64_t kFileOpenByFileId          = 0x00002000;
const uint64_t kFileOpenForFreeSpaceQuery = 0x00800000;

// OperationEnd.ExtraInformation of a create = IO_STATUS_BLOCK.Information.
const uint8_t kFileSuperseded  = 0;
const uint8_t kFileOpened      = 1;
const uint8_t kFileCreated     = 2;
const uint8_t kFileOverwritten = 3;
const uint8_t kResultUnknown   = 0xFF;

const LONG kNtStatusPending = 0x00000103;
const LONG kNtStatusReparse = 0x00000104;

// FILE_INFORMATION_CLASS values expected in RenamePath / DeletePath.
const uint64_t kInfoClassRename        = 10;
const uint64_t kInfoClassDisposition   = 13;
const uint64_t kInfoClassDispositionEx = 64;
const uint64_t kInfoClassRenameEx      = 65;

const size_t    kEtwQueueCap          = 50000;
const size_t    kPullQueueCap         = 10000;
const size_t    kBatchMax             = 512;
const size_t    kMaxPendingIrps       = 32768;
const ULONGLONG kPendingTimeoutMs     = 2000;
const uint64_t  kUnconfirmedThreshold = 2000;     // expired creates with no OperationEnd match at all
const int64_t   kSaveCoalesceMs       = 3000;
const int64_t   kDupWindowMs          = 3000;
const int64_t   kOpenRepeatMs         = 30000;
const int64_t   kOpenAfterWriteMs     = 2000;
const int64_t   kExplicitDedupMs      = 120000;
const int64_t   kOpenerWindowMs       = 60000;
const int64_t   kOpenerSlackMs        = 50;
const int64_t   kReplaceWindowMs      = 5000;
const int64_t   kFileObjectTtlMs      = 600000;
const ULONGLONG kHoldOpenMs           = 4000;
const ULONGLONG kHoldCreateMs         = 2500;
const ULONGLONG kHoldDeleteMs         = 3000;
const ULONGLONG kRdcwDelayMs          = 1500;
const size_t    kMaxHeld              = 8192;
const size_t    kMaxRdcwDelay         = 8192;
const size_t    kPidRingCap           = 16;
const size_t    kMaxPidRings          = 512;
const size_t    kExplicitRingCap      = 128;
const ULONGLONG kDeviceMapRefreshMs   = 30000;
const ULONGLONG kEtwBackoffMinMs      = 30000;
const ULONGLONG kEtwBackoffMaxMs      = 600000;
const ULONGLONG kEtwHealthyMs         = 600000;
const ULONGLONG kSourceRetryMs        = 300000;
const ULONGLONG kUsnVolumeCheckMs     = 300000;
const ULONGLONG kWatermarkSaveMs      = 30000;
const ULONGLONG kOneDriveRefreshMs    = 600000;
const ULONGLONG kSettingsSnapshotMs   = 5000;
const ULONGLONG kPausedCheckMs        = 1000;
const ULONGLONG kProcNoKeyTtlMs       = 60000;
const ULONGLONG kStopDrainMs          = 1500;
const int       kBackfillHours        = 72;
const int64_t   kBackfillMs           = static_cast<int64_t>(kBackfillHours) * 3600 * 1000;
const DWORD     kRdcwBufferBytes      = 64 * 1024;   // network redirectors reject larger buffers
const DWORD     kMaxWorkerWaitMs      = 500;
const DWORD     kMruPollMs            = 60000;
const wchar_t   kRecentCursorKey[]    = L"recent-items";
const wchar_t   kMruCursorKey[]       = L"office-mru";

// ------------------------------------------------------------------------------------------
// Small utilities
// ------------------------------------------------------------------------------------------
int64_t FileTimeToUnixMs(int64_t fileTime)
{
    const int64_t kEpoch = 116444736000000000LL;   // 1970-01-01 as FILETIME
    return fileTime > kEpoch ? (fileTime - kEpoch) / 10000 : 0;
}

int64_t FileTimeToUnixMs(const FILETIME& ft)
{
    ULARGE_INTEGER v = {};
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return FileTimeToUnixMs(static_cast<int64_t>(v.QuadPart));
}

int64_t NowUnixMs()
{
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    return FileTimeToUnixMs(ft);
}

int64_t AbsDiff(int64_t a, int64_t b)
{
    return a >= b ? a - b : b - a;
}

std::string U8(const std::wstring& w)
{
    return EntityKey::ToUtf8(w);
}

// Case folding for keys and rule matching (invariant lowercase; ASCII fast path).
std::wstring FoldCase(const std::wstring& s)
{
    std::wstring out(s);
    bool ascii = true;
    for (wchar_t& c : out)
    {
        if (c >= L'A' && c <= L'Z')
            c = static_cast<wchar_t>(c + (L'a' - L'A'));
        else if (c >= 0x80)
            ascii = false;
    }
    if (ascii || s.size() > static_cast<size_t>(INT_MAX))
        return out;
    std::wstring mapped(s.size(), L'\0');
    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), static_cast<int>(s.size()),
                                &mapped[0], static_cast<int>(mapped.size()), nullptr, nullptr, 0);
    if (n > 0 && static_cast<size_t>(n) == mapped.size())
        return mapped;
    return out;
}

bool StartsWith(const std::wstring& s, const wchar_t* prefix)
{
    const size_t n = wcslen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool EndsWith(const std::wstring& s, const wchar_t* suffix)
{
    const size_t n = wcslen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// ASCII-insensitive prefix test (prefixes are ASCII literals).
bool StartsWithNoCase(const std::wstring& s, const wchar_t* prefix)
{
    const size_t n = wcslen(prefix);
    return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

std::wstring LastComponent(const std::wstring& p)
{
    const size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? p : p.substr(pos + 1);
}

// Both arguments folded; folder without a trailing separator.
bool IsUnder(const std::wstring& lowerPath, const std::wstring& lowerFolder)
{
    if (lowerFolder.empty() || lowerPath.size() < lowerFolder.size())
        return false;
    if (lowerPath.compare(0, lowerFolder.size(), lowerFolder) != 0)
        return false;
    return lowerPath.size() == lowerFolder.size() || lowerPath[lowerFolder.size()] == L'\\';
}

template <size_t N>
bool InTable(const std::wstring& s, const wchar_t* const (&table)[N])
{
    for (const wchar_t* t : table)
    {
        if (s == t)
            return true;
    }
    return false;
}

bool IsAsciiLetter(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

bool IsHexLower(wchar_t c)
{
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
}

bool IsDrivePath(const std::wstring& p)
{
    return p.size() >= 3 && IsAsciiLetter(p[0]) && p[1] == L':' && p[2] == L'\\';
}

bool IsUncPath(const std::wstring& p)
{
    return p.size() > 2 && p[0] == L'\\' && p[1] == L'\\' && p[2] != L'?' && p[2] != L'.' && p[2] != L'\\';
}

bool IsFileSystemDosPath(const std::wstring& p)
{
    return IsDrivePath(p) || IsUncPath(p);
}

// Win32 APIs without long-path awareness need the \\?\ form beyond MAX_PATH.
std::wstring ApiPath(const std::wstring& p)
{
    if (p.size() < MAX_PATH - 12)
        return p;
    if (IsUncPath(p))
        return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

// 1 = directory, 0 = file, -1 = unknown (gone / inaccessible).
int ProbeDirectory(const std::wstring& path)
{
    const DWORD a = GetFileAttributesW(ApiPath(path).c_str());
    if (a == INVALID_FILE_ATTRIBUTES)
        return -1;
    return (a & FILE_ATTRIBUTE_DIRECTORY) != 0 ? 1 : 0;
}

bool ParseUnsigned(const std::string& s, size_t& pos, uint64_t& out)
{
    const size_t start = pos;
    uint64_t v = 0;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
    {
        const uint64_t d = static_cast<uint64_t>(s[pos] - '0');
        if (v > (UINT64_MAX - d) / 10)
            return false;
        v = v * 10 + d;
        ++pos;
    }
    if (pos == start)
        return false;
    out = v;
    return true;
}

bool ParseSigned(const std::string& s, size_t& pos, int64_t& out)
{
    bool negative = false;
    if (pos < s.size() && s[pos] == '-')
    {
        negative = true;
        ++pos;
    }
    uint64_t u = 0;
    if (!ParseUnsigned(s, pos, u) || u > static_cast<uint64_t>(INT64_MAX))
        return false;
    out = negative ? -static_cast<int64_t>(u) : static_cast<int64_t>(u);
    return true;
}

// Bounded LRU map (single-threaded use).
template <typename K, typename V, typename H = std::hash<K>>
class LruMap
{
public:
    explicit LruMap(size_t capacity) : m_capacity(capacity == 0 ? 1 : capacity) {}

    V* Get(const K& key)
    {
        auto it = m_index.find(key);
        if (it == m_index.end())
            return nullptr;
        m_list.splice(m_list.begin(), m_list, it->second);
        return &it->second->second;
    }

    V* Peek(const K& key)
    {
        auto it = m_index.find(key);
        return it == m_index.end() ? nullptr : &it->second->second;
    }

    V& Put(const K& key, V value)
    {
        auto it = m_index.find(key);
        if (it != m_index.end())
        {
            it->second->second = std::move(value);
            m_list.splice(m_list.begin(), m_list, it->second);
            return it->second->second;
        }
        m_list.emplace_front(key, std::move(value));
        try
        {
            m_index.emplace(key, m_list.begin());
        }
        catch (...)
        {
            m_list.pop_front();
            throw;
        }
        while (m_index.size() > m_capacity)
        {
            m_index.erase(m_list.back().first);
            m_list.pop_back();
        }
        return m_list.front().second;
    }

    void Erase(const K& key)
    {
        auto it = m_index.find(key);
        if (it == m_index.end())
            return;
        m_list.erase(it->second);
        m_index.erase(it);
    }

    size_t Size() const { return m_index.size(); }

    void Clear()
    {
        m_index.clear();
        m_list.clear();
    }

private:
    using List = std::list<std::pair<K, V>>;
    size_t m_capacity;
    List m_list;
    std::unordered_map<K, typename List::iterator, H> m_index;
};

// ------------------------------------------------------------------------------------------
// Path rules (FILE-2, FILE-6). Tables are folded (lowercase).
// ------------------------------------------------------------------------------------------
const wchar_t* const kTransientPrefixes[] = { L"~$", L"~wrl", L"~wrd", L"~wrs", L"~rf", L"~df", L".#" };
const wchar_t* const kTransientSuffixes[] = {
    L".tmp", L".temp", L".crdownload", L".partial", L".part", L".opdownload", L".download",
    L".swp", L".swo", L".swx", L".~tmp", L".bak", L"___jb_tmp___", L"___jb_old___",
};
const wchar_t* const kDownloadTempSuffixes[] = { L".crdownload", L".partial", L".opdownload", L".download" };

// First component under a volume root ("X:\<dir>\...").
const wchar_t* const kVolumeRootDirs[] = {
    L"$recycle.bin", L"recycler", L"recycled", L"system volume information", L"$extend",
    L"$windows.~bt", L"$windows.~ws", L"$windows.~q", L"$winreagent", L"$getcurrent", L"$sysreset",
    L"$av_asw", L"config.msi", L"msocache", L"perflogs", L"recovery", L"windows.old", L"onedrivetemp",
    L"boot", L"intel", L"amd", L"nvidia", L"inetpub", L"swsetup", L"program files", L"program files (x86)",
    L"programdata", L"windows", L"windowsapps", L"wpsystem",
};

// Directories directly under a user profile ("C:\Users\<u>\<dir>\...").
const wchar_t* const kProfileChildDirs[] = {
    L"appdata", L"application data", L"local settings", L"cookies", L"nethood", L"printhood", L"recent",
    L"sendto", L"start menu", L"templates", L"microsoftedgebackups", L"intelgraphicsprofiles",
};

// Tool / VCS state directories (dot-FILES and other dot-directories such as .github stay visible).
const wchar_t* const kToolDotDirs[] = {
    L".git", L".svn", L".hg", L".bzr", L".vs", L".idea", L".gradle", L".m2", L".nuget", L".npm", L".yarn",
    L".pnpm-store", L".cargo", L".rustup", L".conda", L".tox", L".venv", L".mypy_cache", L".pytest_cache",
    L".ruff_cache", L".cache", L".ipynb_checkpoints", L".terraform", L".vagrant", L".docker",
    L".vscode-server", L".vscode-test", L".dotnet", L".android", L".next", L".nuxt", L".angular",
    L".svelte-kit", L".parcel-cache", L".sass-cache", L".turbo", L".nx", L".eggs", L".dart_tool",
    L".pub-cache", L".expo", L".serverless", L".history", L".metadata", L".ccache", L".pulumi",
    L".ssh", L".gnupg", L".aws", L".azure", L".kube", L".dropbox.cache", L".tmp.drivedownload",
    L".tmp.driveupload", L".trash", L".trashes", L".spotlight-v100", L".fseventsd", L".sync",
    L".stversions", L".stfolder",
};

const wchar_t* const kBuildDirs[] = {
    L"node_modules", L"__pycache__", L"bower_components", L"jspm_packages", L"cmakefiles", L"obj",
    L"cmake-build-debug", L"cmake-build-release",
};
// "<arch>\<config>\" build outputs, e.g. bin\Debug, x64\Release, target\debug.
const wchar_t* const kBuildArchDirs[] = { L"bin", L"x64", L"x86", L"win32", L"arm64", L"arm", L"anycpu", L"target" };
const wchar_t* const kBuildConfigDirs[] = { L"debug", L"release" };

const wchar_t* const kExcludedExts[] = {
    // mailbox / database stores and their journals
    L".ost", L".pst", L".nst", L".oab", L".db-wal", L".db-shm", L".db-journal", L".sqlite-wal",
    L".sqlite-shm", L".sqlite-journal", L".ldb", L".laccdb",
    // logs, traces, dumps
    L".etl", L".evtx", L".blf", L".regtrans-ms", L".log", L".log1", L".log2", L".dmp", L".mdmp",
    L".hdmp", L".nettrace", L".gcdump", L".diagsession",
    // shortcuts: explicit opens come from the Recent-items watcher instead
    L".lnk",
    // binaries, system files, build intermediates
    L".dll", L".sys", L".drv", L".ocx", L".mui", L".mun", L".nls", L".cat", L".efi", L".pf",
    L".pdb", L".ilk", L".obj", L".pch", L".ipch", L".idb", L".iobj", L".ipdb", L".tlog",
    L".lastbuildstate", L".o", L".pyc", L".pyo", L".class", L".rlib", L".rmeta", L".gch", L".cache",
    L".vsidx", L".suo", L".sdf", L".opensdf", L".lck",
    // virtual disks (opaque, written continuously by hypervisors)
    L".vhd", L".vhdx", L".avhdx", L".vmdk", L".vdi", L".qcow2", L".vmcx", L".vmrs", L".vmgs",
};

const wchar_t* const kExcludedNames[] = {
    L"desktop.ini", L"thumbs.db", L"ehthumbs.db", L"ehthumbs_vista.db", L"iconcache.db", L".ds_store",
    L"pagefile.sys", L"hiberfil.sys", L"swapfile.sys", L"dumpstack.log", L"dumpstack.log.tmp",
    L"bootmgr", L"bootnxt", L"$mft", L"$mftmirr", L"$logfile", L"$volume", L"$attrdef", L"$bitmap",
    L"$boot", L"$badclus", L"$secure", L"$upcase", L"$extend",
};
const wchar_t* const kExcludedNamePrefixes[] = {
    L"ntuser.", L"usrclass.dat", L"iconcache_", L"thumbcache_",
    L"dotnet-diagnostic-", L"dotnet-counters-", L"dotnet-trace-", L"dotnet-dump-", L"dotnet-gcdump-",
    L"dotnet-stack-", L"dotnet-monitor-", L"dotnet-symbol-", L"dotnet-sos-",
};
const wchar_t* const kExcludedNameSuffixes[] = {
    L"_diagnostics.json", L"-diagnostics.json", L".diagnostics.json", L".vc.db", L".vc.opendb",
};

// User content that lives under AppData (extension-gated where the folder also holds noise).
struct Readmit
{
    const wchar_t* contains;
    const wchar_t* alsoContains;   // nullptr = no extra requirement
    const wchar_t* ext;            // nullptr = any extension
};
const Readmit kAppDataReadmits[] = {
    { L"\\microsoft\\office\\unsavedfiles\\", nullptr, nullptr },
    { L"\\microsoft\\visualstudio\\", L"\\backup files\\", nullptr },
    { L"\\notepad++\\backup\\", nullptr, nullptr },
    { L"\\microsoft\\onenote\\", nullptr, L".one" },
};

struct KnownFolders
{
    std::vector<std::wstring> systemRoots;    // folded, no trailing separator
    std::vector<std::wstring> appDataRoots;
    std::vector<std::wstring> tempRoots;
    std::wstring profilesRoot;                // e.g. c:\users
    std::wstring ownProfile;
    std::wstring publicProfile;
};

std::wstring NormalizeFolder(std::wstring p)
{
    if (StartsWith(p, L"\\\\?\\UNC\\"))
        p = L"\\\\" + p.substr(8);
    else if (StartsWith(p, L"\\\\?\\"))
        p = p.substr(4);
    p = FoldCase(p);
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/'))
        p.pop_back();
    return p;
}

std::wstring GetKnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &raw)) && raw != nullptr)
        out = raw;
    CoTaskMemFree(raw);
    return out;
}

void AddFolder(std::vector<std::wstring>& v, const std::wstring& raw)
{
    if (raw.empty())
        return;
    const std::wstring n = NormalizeFolder(raw);
    if (n.size() <= 3)
        return;   // never anchor a rule on a bare volume root
    if (std::find(v.begin(), v.end(), n) == v.end())
        v.push_back(n);
}

std::wstring TempPathRaw()
{
    std::vector<wchar_t> buf(MAX_PATH + 1, L'\0');
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const DWORD n = GetTempPathW(static_cast<DWORD>(buf.size()), buf.data());
        if (n == 0)
            return std::wstring();
        if (n < buf.size())
            return std::wstring(buf.data(), n);
        buf.assign(static_cast<size_t>(n) + 1, L'\0');
    }
    return std::wstring();
}

KnownFolders BuildKnownFolders()
{
    KnownFolders k;
    AddFolder(k.systemRoots, GetKnownFolder(FOLDERID_Windows));
    AddFolder(k.systemRoots, GetKnownFolder(FOLDERID_ProgramFiles));
    AddFolder(k.systemRoots, GetKnownFolder(FOLDERID_ProgramFilesX86));
    AddFolder(k.systemRoots, GetKnownFolder(FOLDERID_ProgramFilesX64));   // fails for 32-bit builds
    AddFolder(k.systemRoots, GetKnownFolder(FOLDERID_ProgramData));
    AddFolder(k.appDataRoots, GetKnownFolder(FOLDERID_LocalAppData));
    AddFolder(k.appDataRoots, GetKnownFolder(FOLDERID_RoamingAppData));
    AddFolder(k.appDataRoots, GetKnownFolder(FOLDERID_LocalAppDataLow));
    AddFolder(k.tempRoots, TempPathRaw());
    k.profilesRoot = NormalizeFolder(GetKnownFolder(FOLDERID_UserProfiles));
    k.ownProfile = NormalizeFolder(GetKnownFolder(FOLDERID_Profile));
    k.publicProfile = NormalizeFolder(GetKnownFolder(FOLDERID_Public));
    if (k.profilesRoot.size() <= 3)
        k.profilesRoot.clear();
    return k;
}

const KnownFolders& Folders()
{
    static const KnownFolders k = BuildKnownFolders();
    return k;
}

// Length of the root of a folded DOS/UNC path: "c:\" -> 3, "\\srv\share\" -> past the share.
// 0 if the path is neither.
size_t RootLength(const std::wstring& p)
{
    if (p.size() >= 2 && IsAsciiLetter(p[0]) && p[1] == L':')
        return (p.size() >= 3 && p[2] == L'\\') ? 3 : 2;
    if (p.size() > 2 && p[0] == L'\\' && p[1] == L'\\')
    {
        const size_t server = p.find(L'\\', 2);
        if (server == std::wstring::npos || server == 2)
            return 0;
        const size_t share = p.find(L'\\', server + 1);
        if (share == std::wstring::npos)
            return p.size();
        return share + 1;
    }
    return 0;
}

struct Comp
{
    size_t pos;
    size_t len;
};

std::vector<Comp> SplitComponents(const std::wstring& p, size_t start)
{
    std::vector<Comp> out;
    size_t i = start;
    while (i < p.size())
    {
        while (i < p.size() && p[i] == L'\\')
            ++i;
        const size_t b = i;
        while (i < p.size() && p[i] != L'\\')
            ++i;
        if (i > b)
            out.push_back(Comp{ b, i - b });
    }
    return out;
}

std::wstring CompStr(const std::wstring& p, const Comp& c)
{
    return p.substr(c.pos, c.len);
}

bool IsTransientLower(const std::wstring& n)
{
    if (n.empty())
        return false;
    for (const wchar_t* p : kTransientPrefixes)
    {
        if (StartsWith(n, p))
            return true;
    }
    for (const wchar_t* s : kTransientSuffixes)
    {
        if (EndsWith(n, s))
            return true;
    }
    if (n.back() == L'~')
        return true;                                   // editor backups: name~
    if (StartsWith(n, L".~lock.") && n.back() == L'#')
        return true;                                   // LibreOffice lock files
    if (n.find(L".sb-") != std::wstring::npos)
        return true;                                   // safe-save temporaries: name.sb-xxxx-yyyy
    if (n.size() == 8 && std::all_of(n.begin(), n.end(), IsHexLower))
        return true;                                   // Excel save temporaries: 8 hex digits, no extension
    return false;
}

bool IsDownloadTempLower(const std::wstring& n)
{
    for (const wchar_t* s : kDownloadTempSuffixes)
    {
        if (EndsWith(n, s))
            return true;
    }
    return false;
}

bool IsFoundDir(const std::wstring& c)
{
    return c.size() == 9 && StartsWith(c, L"found.") &&
           c[6] >= L'0' && c[6] <= L'9' && c[7] >= L'0' && c[7] <= L'9' && c[8] >= L'0' && c[8] <= L'9';
}

// ".849c9593-d756-4e56-8d6e-42412f2a707b" (OneDrive staging and similar hidden GUID folders).
bool IsGuidDotDir(const std::wstring& c)
{
    if (c.size() != 37 || c[0] != L'.')
        return false;
    for (size_t i = 1; i < c.size(); ++i)
    {
        const size_t k = i - 1;
        if (k == 8 || k == 13 || k == 18 || k == 23)
        {
            if (c[i] != L'-')
                return false;
        }
        else if (!IsHexLower(c[i]))
        {
            return false;
        }
    }
    return true;
}

// NuGet "packages\<Id>.<major>.<minor>[...]" folder (a user's own "packages" folder stays visible).
bool LooksLikePackageDir(const std::wstring& c)
{
    std::vector<std::wstring> segs;
    size_t b = 0;
    for (;;)
    {
        const size_t d = c.find(L'.', b);
        segs.push_back(c.substr(b, d == std::wstring::npos ? std::wstring::npos : d - b));
        if (d == std::wstring::npos)
            break;
        b = d + 1;
    }
    size_t numeric = 0;
    for (size_t i = segs.size(); i > 0; --i)
    {
        const std::wstring& s = segs[i - 1];
        if (s.empty() || s[0] < L'0' || s[0] > L'9')
            break;
        ++numeric;
    }
    return numeric >= 2 && numeric < segs.size();
}

bool IsExcludedFileName(const std::wstring& name)
{
    if (InTable(name, kExcludedNames))
        return true;
    for (const wchar_t* p : kExcludedNamePrefixes)
    {
        if (StartsWith(name, p))
            return true;
    }
    for (const wchar_t* s : kExcludedNameSuffixes)
    {
        if (EndsWith(name, s))
            return true;
    }
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0 && InTable(name.substr(dot), kExcludedExts))
        return true;
    return false;
}

// Folded path below some user's AppData (own known folders, or <profiles>\<user>\AppData).
bool IsUnderAppData(const std::wstring& p, const KnownFolders& kf)
{
    for (const std::wstring& r : kf.appDataRoots)
    {
        if (IsUnder(p, r))
            return true;
    }
    if (!kf.profilesRoot.empty() && IsUnder(p, kf.profilesRoot))
    {
        const std::vector<Comp> rel = SplitComponents(p, kf.profilesRoot.size());
        if (rel.size() >= 2 && CompStr(p, rel[1]) == L"appdata")
            return true;
    }
    return false;
}

// USN and RDCW are machine-wide: never report files in another user's profile (SIG-5).
bool IsOtherUsersProfile(const std::wstring& foldedPath)
{
    const KnownFolders& kf = Folders();
    if (kf.profilesRoot.empty() || !IsUnder(foldedPath, kf.profilesRoot))
        return false;
    if (!kf.ownProfile.empty() && IsUnder(foldedPath, kf.ownProfile))
        return false;
    if (!kf.publicProfile.empty() && IsUnder(foldedPath, kf.publicProfile))
        return false;
    return SplitComponents(foldedPath, kf.profilesRoot.size()).size() >= 2;
}
} // namespace

// ------------------------------------------------------------------------------------------
// Public path rules
// ------------------------------------------------------------------------------------------
bool FileCapture::IsTransientName(const std::wstring& nameOrPath)
{
    return IsTransientLower(FoldCase(LastComponent(nameOrPath)));
}

bool FileCapture::IsDownloadTempName(const std::wstring& nameOrPath)
{
    return IsDownloadTempLower(FoldCase(LastComponent(nameOrPath)));
}

bool FileCapture::IsDownloadRename(const std::wstring& srcPath, const std::wstring& dstPath)
{
    const std::wstring s = FoldCase(LastComponent(srcPath));
    if (IsDownloadTempLower(s))
        return true;
    const std::wstring d = FoldCase(LastComponent(dstPath));
    return !d.empty() && EndsWith(s, L".part") && s.size() == d.size() + 5 && s.compare(0, d.size(), d) == 0;
}

bool FileCapture::IsRecycleBinPath(const std::wstring& path)
{
    const std::wstring p = FoldCase(path);
    const size_t root = RootLength(p);
    if (root == 0)
        return false;
    for (const Comp& c : SplitComponents(p, root))
    {
        const std::wstring s = CompStr(p, c);
        if (s == L"$recycle.bin" || s == L"recycler" || s == L"recycled")
            return true;
    }
    return false;
}

bool FileCapture::IsExcludedPath(const std::wstring& dosPath)
{
    const std::wstring p = FoldCase(dosPath);
    const size_t root = RootLength(p);
    if (root == 0)
        return true;
    const std::vector<Comp> comps = SplitComponents(p, root);
    if (comps.empty())
        return true;
    const std::wstring name = CompStr(p, comps.back());
    if (IsTransientLower(name))
        return true;

    const KnownFolders& kf = Folders();
    if (IsUnderAppData(p, kf))
    {
        for (const Readmit& r : kAppDataReadmits)
        {
            if (p.find(r.contains) == std::wstring::npos)
                continue;
            if (r.alsoContains != nullptr && p.find(r.alsoContains) == std::wstring::npos)
                continue;
            if (r.ext != nullptr && !EndsWith(name, r.ext))
                continue;
            return IsExcludedFileName(name);
        }
        return true;
    }
    for (const std::wstring& r : kf.systemRoots)
    {
        if (IsUnder(p, r))
            return true;
    }
    for (const std::wstring& r : kf.tempRoots)
    {
        if (IsUnder(p, r))
            return true;
    }

    if (root <= 3)
    {
        const std::wstring first = CompStr(p, comps[0]);
        if (InTable(first, kVolumeRootDirs) || IsFoundDir(first))
            return true;
    }

    // Legacy profile junctions and per-user tool state (C:\Users\<u>\.vscode\..., ...\.cache\...).
    if (!kf.profilesRoot.empty() && IsUnder(p, kf.profilesRoot))
    {
        const std::vector<Comp> rel = SplitComponents(p, kf.profilesRoot.size());
        if (rel.size() >= 3)
        {
            const std::wstring child = CompStr(p, rel[1]);
            if (InTable(child, kProfileChildDirs) || child[0] == L'.')
                return true;
        }
    }

    // Directory rules also test the last component, so events for the folders themselves
    // (a new obj\ or .git\) are excluded too.
    for (size_t i = 0; i < comps.size(); ++i)
    {
        const std::wstring c = CompStr(p, comps[i]);
        if (c == L"$recycle.bin" || c == L"recycler" || c == L"recycled" || c == L"system volume information")
            return true;
        if (c[0] == L'.' && (InTable(c, kToolDotDirs) || IsGuidDotDir(c)))
            return true;
        if (InTable(c, kBuildDirs))
            return true;
        if (i + 1 < comps.size())
        {
            const std::wstring next = CompStr(p, comps[i + 1]);
            if (InTable(c, kBuildArchDirs) && InTable(next, kBuildConfigDirs))
                return true;
            if (c == L"packages" && LooksLikePackageDir(next))
                return true;
        }
    }
    return IsExcludedFileName(name);
}

FileCapture::PathClass FileCapture::ClassifyPath(const std::wstring& dosPath)
{
    if (IsTransientName(dosPath))
        return PathClass::Transient;
    return IsExcludedPath(dosPath) ? PathClass::Excluded : PathClass::User;
}

FileCapture::RenameOutcome FileCapture::DecideRename(PathClass src, PathClass dst, bool dstInRecycleBin, bool isDownloadRename)
{
    if (dstInRecycleBin)
        return src == PathClass::User ? RenameOutcome::DeleteSource : RenameOutcome::Drop;
    if (src == PathClass::User)
    {
        if (dst == PathClass::User)
            return RenameOutcome::Rename;
        if (dst == PathClass::Transient)
            return RenameOutcome::Drop;          // safe-save step: the original is parked under a temp name
        return RenameOutcome::DeleteSource;      // moved out of the user's space
    }
    if (dst != PathClass::User)
        return RenameOutcome::Drop;
    if (src == PathClass::Transient)
        return isDownloadRename ? RenameOutcome::DownloadDest : RenameOutcome::ModifyDest;
    return RenameOutcome::CreateDest;            // excluded (non-transient) -> user: appears in user space
}

namespace
{
// ------------------------------------------------------------------------------------------
// Kernel device path -> DOS path (FILE-5)
// ------------------------------------------------------------------------------------------
struct DeviceEntry
{
    std::wstring device;     // folded NT device name, e.g. \device\harddiskvolume3
    std::wstring dos;        // "C:" or a mount folder such as "C:\Mount\Data"
    UINT         driveType = DRIVE_UNKNOWN;
    wchar_t      letter = 0; // uppercase drive letter when dos is "X:"
};

struct DeviceMap
{
    std::vector<DeviceEntry> entries;   // longest device name first
};

struct VolInfo
{
    UINT driveType = DRIVE_UNKNOWN;
    bool remote = false;
    bool usn = false;        // covered by the USN reader: ETW then reports opens only
};

// Devices whose files the ETW callback forwards; others (pipes, sockets, consoles...) are dropped.
const wchar_t* const kNonFsDevicePrefixes[] = {
    L"\\Device\\HarddiskVolumeShadowCopy", L"\\Device\\NamedPipe", L"\\Device\\Mailslot", L"\\Device\\Afd",
    L"\\Device\\ConDrv", L"\\Device\\Nsi", L"\\Device\\KsecDD", L"\\Device\\CNG", L"\\Device\\DeviceApi",
    L"\\Device\\MountPointManager", L"\\Device\\Null", L"\\Device\\Tcp", L"\\Device\\Udp", L"\\Device\\RawIp",
};
const wchar_t* const kFsDevicePrefixes[] = {
    L"\\Device\\HarddiskVolume", L"\\Device\\Mup\\", L"\\Device\\LanmanRedirector\\",
    L"\\Device\\WebDavRedirector\\", L"\\Device\\RdpDr\\", L"\\Device\\CdRom", L"\\Device\\Floppy",
};
const wchar_t* const kNetworkDevicePrefixes[] = {
    L"\\Device\\Mup\\", L"\\Device\\LanmanRedirector\\", L"\\Device\\WebDavRedirector\\", L"\\Device\\RdpDr\\",
};

wchar_t UpperLetter(wchar_t c)
{
    return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - (L'a' - L'A')) : c;
}

std::wstring QueryDosDeviceFirst(const std::wstring& name)
{
    std::vector<wchar_t> buf(1024, L'\0');
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        const DWORD n = QueryDosDeviceW(name.c_str(), buf.data(), static_cast<DWORD>(buf.size()));
        if (n != 0)
        {
            buf.back() = L'\0';
            return std::wstring(buf.data());   // first string of the multi-string
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            break;
        buf.assign(buf.size() * 4, L'\0');
    }
    return std::wstring();
}

std::vector<std::wstring> VolumePathNames(const std::wstring& volumeName)
{
    std::vector<std::wstring> out;
    std::vector<wchar_t> buf(MAX_PATH + 1, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        DWORD needed = 0;
        if (GetVolumePathNamesForVolumeNameW(volumeName.c_str(), buf.data(), static_cast<DWORD>(buf.size()), &needed))
        {
            buf.push_back(L'\0');
            buf.push_back(L'\0');
            for (const wchar_t* p = buf.data(); *p != L'\0'; p += wcslen(p) + 1)
                out.emplace_back(p);
            return out;
        }
        if (GetLastError() != ERROR_MORE_DATA || needed <= buf.size())
            break;
        buf.assign(static_cast<size_t>(needed) + 2, L'\0');
    }
    return out;
}

std::shared_ptr<const DeviceMap> BuildDeviceMap()
{
    auto map = std::make_shared<DeviceMap>();

    // Drive letters. Network letters are not listed: their files arrive as \Device\Mup\... and
    // are mapped to UNC. SUBST letters point at \??\ paths and are skipped too.
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
    {
        if ((drives & (1u << i)) == 0)
            continue;
        const wchar_t letter = static_cast<wchar_t>(L'A' + i);
        const std::wstring name{ letter, L':' };
        const std::wstring root = name + L"\\";
        const UINT type = GetDriveTypeW(root.c_str());
        if (type == DRIVE_REMOTE || type == DRIVE_NO_ROOT_DIR || type == DRIVE_UNKNOWN)
            continue;
        const std::wstring target = QueryDosDeviceFirst(name);
        if (target.empty() || !StartsWithNoCase(target, L"\\Device\\"))
            continue;
        DeviceEntry e;
        e.device = FoldCase(target);
        e.dos = name;
        e.driveType = type;
        e.letter = letter;
        map->entries.push_back(std::move(e));
    }

    // Volumes mounted on folders (and volumes whose letter was not listed above).
    wchar_t volume[MAX_PATH] = {};
    HANDLE find = FindFirstVolumeW(volume, ARRAYSIZE(volume));
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring volumeName(volume);
            if (volumeName.size() < 6 || volumeName.back() != L'\\' || !StartsWith(volumeName, L"\\\\?\\"))
                continue;
            const std::wstring device = FoldCase(QueryDosDeviceFirst(volumeName.substr(4, volumeName.size() - 5)));
            if (device.empty())
                continue;
            bool known = false;
            for (const DeviceEntry& e : map->entries)
            {
                if (e.device == device)
                {
                    known = true;
                    break;
                }
            }
            if (known)
                continue;
            const std::vector<std::wstring> mounts = VolumePathNames(volumeName);
            if (mounts.empty())
                continue;
            DeviceEntry e;
            e.device = device;
            e.dos = mounts.front();
            e.driveType = GetDriveTypeW(mounts.front().c_str());
            while (e.dos.size() > 2 && e.dos.back() == L'\\')
                e.dos.pop_back();
            if (e.dos.size() == 2 && e.dos[1] == L':')
                e.letter = UpperLetter(e.dos[0]);
            map->entries.push_back(std::move(e));
        } while (FindNextVolumeW(find, volume, ARRAYSIZE(volume)));
        FindVolumeClose(find);
    }

    std::sort(map->entries.begin(), map->entries.end(),
              [](const DeviceEntry& a, const DeviceEntry& b) { return a.device.size() > b.device.size(); });
    return map;
}

// Device names in the map that the ETW callback's static allow-list does not cover
// (ImDisk, VeraCrypt, other third-party volume drivers).
std::vector<std::wstring> ExtraDevicePrefixes(const DeviceMap& map)
{
    std::vector<std::wstring> out;
    for (const DeviceEntry& e : map.entries)
    {
        bool covered = false;
        for (const wchar_t* p : kFsDevicePrefixes)
        {
            if (StartsWithNoCase(e.device, p))
            {
                covered = true;
                break;
            }
        }
        if (!covered && std::find(out.begin(), out.end(), e.device) == out.end())
            out.push_back(e.device);
    }
    return out;
}

// "\Device\Mup\;LanmanRedirector\;Z:000000000001a2b3\server\share\x" -> "\\server\share\x".
// `rest` is the part after the redirector prefix; components starting with ';' are skipped.
bool NetworkDeviceToUnc(const std::wstring& rest, std::wstring& out)
{
    size_t i = 0;
    for (;;)
    {
        while (i < rest.size() && rest[i] == L'\\')
            ++i;
        if (i < rest.size() && rest[i] == L';')
        {
            const size_t end = rest.find(L'\\', i);
            if (end == std::wstring::npos)
                return false;
            i = end;
            continue;
        }
        break;
    }
    if (i >= rest.size())
        return false;
    const std::wstring tail = rest.substr(i);
    const size_t serverEnd = tail.find(L'\\');
    if (serverEnd == std::wstring::npos || serverEnd == 0 || serverEnd + 1 >= tail.size())
        return false;   // need at least server and share
    out = L"\\\\" + tail;
    return true;
}

void DosVolumeInfo(const DeviceMap& map, DWORD usnMask, const std::wstring& dos, VolInfo& vi)
{
    vi = VolInfo();
    if (IsUncPath(dos))
    {
        vi.driveType = DRIVE_REMOTE;
        vi.remote = true;
        return;
    }
    if (dos.size() < 2 || !IsAsciiLetter(dos[0]) || dos[1] != L':')
        return;
    const wchar_t letter = UpperLetter(dos[0]);
    for (const DeviceEntry& e : map.entries)
    {
        if (e.letter == letter)
        {
            vi.driveType = e.driveType;
            vi.usn = (usnMask & (1u << (letter - L'A'))) != 0;
            break;
        }
    }
    if (vi.driveType == DRIVE_UNKNOWN)
    {
        const wchar_t root[4] = { letter, L':', L'\\', L'\0' };
        vi.driveType = GetDriveTypeW(root);
    }
    vi.remote = vi.driveType == DRIVE_REMOTE;
}

bool ResolveNtPath(const DeviceMap& map, DWORD usnMask, const std::wstring& nt, std::wstring& dos, VolInfo& vi)
{
    vi = VolInfo();
    if (IsDrivePath(nt))
    {
        dos = nt;
        DosVolumeInfo(map, usnMask, dos, vi);
        return true;
    }
    if (StartsWithNoCase(nt, L"\\??\\UNC\\"))
    {
        dos = L"\\\\" + nt.substr(8);
        vi.driveType = DRIVE_REMOTE;
        vi.remote = true;
        return dos.size() > 2;
    }
    if (StartsWithNoCase(nt, L"\\??\\"))
    {
        dos = nt.substr(4);
        if (!IsDrivePath(dos))
            return false;
        DosVolumeInfo(map, usnMask, dos, vi);
        return true;
    }
    for (const wchar_t* prefix : kNetworkDevicePrefixes)
    {
        if (StartsWithNoCase(nt, prefix))
        {
            if (!NetworkDeviceToUnc(nt.substr(wcslen(prefix)), dos))
                return false;
            vi.driveType = DRIVE_REMOTE;
            vi.remote = true;
            return true;
        }
    }
    for (const DeviceEntry& e : map.entries)
    {
        const size_t n = e.device.size();
        if (nt.size() < n || _wcsnicmp(nt.c_str(), e.device.c_str(), n) != 0)
            continue;
        if (nt.size() > n && nt[n] != L'\\')
            continue;   // \device\harddiskvolume1 must not match \device\harddiskvolume12
        dos = e.dos + nt.substr(n);
        vi.driveType = e.driveType;
        vi.remote = e.driveType == DRIVE_REMOTE;
        vi.usn = e.letter != 0 && (usnMask & (1u << (e.letter - L'A'))) != 0;
        return true;
    }
    return false;
}

// Drive letters (bit 0 = A:) of fixed NTFS/ReFS volumes with an active change journal: the
// same rule UsnJournalReader uses to pick its volumes.
DWORD ProbeUsnDrives()
{
    DWORD mask = 0;
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
    {
        if ((drives & (1u << i)) == 0)
            continue;
        const wchar_t letter = static_cast<wchar_t>(L'A' + i);
        const wchar_t root[4] = { letter, L':', L'\\', L'\0' };
        if (GetDriveTypeW(root) != DRIVE_FIXED)
            continue;
        wchar_t fileSystem[MAX_PATH + 1] = {};
        if (!GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fileSystem, ARRAYSIZE(fileSystem)))
            continue;
        if (_wcsicmp(fileSystem, L"NTFS") != 0 && _wcsicmp(fileSystem, L"ReFS") != 0)
            continue;
        const wchar_t device[7] = { L'\\', L'\\', L'.', L'\\', letter, L':', L'\0' };
        HANDLE h = CreateFileW(device, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        USN_JOURNAL_DATA_V0 data = {};
        DWORD bytes = 0;
        if (DeviceIoControl(h, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &data, sizeof(data), &bytes, nullptr))
            mask |= 1u << i;
        CloseHandle(h);
    }
    return mask;
}

// "Z:" -> "\\server\share" for a mapped network drive.
bool GetUncForDrive(wchar_t letter, std::wstring& unc)
{
    const wchar_t local[3] = { letter, L':', L'\0' };
    DWORD length = 512;
    std::vector<wchar_t> buf(length, L'\0');
    DWORD rc = WNetGetConnectionW(local, buf.data(), &length);
    if (rc == ERROR_MORE_DATA && length > buf.size())
    {
        buf.assign(length, L'\0');
        rc = WNetGetConnectionW(local, buf.data(), &length);
    }
    if (rc != NO_ERROR)
        return false;
    buf.back() = L'\0';
    unc = buf.data();
    while (unc.size() > 2 && unc.back() == L'\\')
        unc.pop_back();
    return IsUncPath(unc);
}
} // namespace

namespace
{
// ------------------------------------------------------------------------------------------
// Process roles (FILE-7): role-based hints instead of an executable denylist.
// ------------------------------------------------------------------------------------------
// Never hard-dropped even if SystemProcessClassifier::IsHardBlocked lists them: they broker or
// perform real user I/O (packaged-app file pickers, Photo Viewer, WebView2 downloads, sync).
const wchar_t* const kNeverHardDrop[] = {
    L"runtimebroker.exe", L"dllhost.exe", L"msedgewebview2.exe", L"onedrive.exe",
    L"backgroundtaskhost.exe", L"explorer.exe",
};
const wchar_t* const kSyncClients[] = {
    L"onedrive.exe", L"filecoauth.exe", L"microsoft.sharepoint.exe", L"dropbox.exe", L"googledrivefs.exe",
    L"googledrivesync.exe", L"box.exe", L"boxsync.exe", L"boxui.exe", L"megasync.exe", L"nextcloud.exe",
    L"owncloud.exe", L"pcloud.exe", L"seadrive.exe", L"seafile-applet.exe", L"syncthing.exe",
    L"resilio sync.exe", L"tresorit.exe", L"insync.exe", L"sync-worker.exe", L"egnytedrive.exe",
};
const wchar_t* const kSyncClientPrefixes[] = { L"icloud", L"cloud-drive-", L"synology" };
const wchar_t* const kShellInfra[] = {
    L"searchprotocolhost.exe", L"searchfilterhost.exe", L"searchindexer.exe", L"searchhost.exe",
    L"searchapp.exe", L"prevhost.exe", L"dllhost.exe", L"msmpeng.exe", L"mpcmdrun.exe", L"nissrv.exe",
    L"mssense.exe", L"smartscreen.exe", L"backgroundtaskhost.exe", L"taskhostw.exe", L"sihost.exe",
    L"startmenuexperiencehost.exe", L"shellexperiencehost.exe", L"wmiprvse.exe", L"compattelrunner.exe",
    L"officeclicktorun.exe", L"sdxhelper.exe", L"msoia.exe", L"mousocoreworker.exe",
};
// Creators whose new files are copies (keep CREATE even when the copy carries Mark-of-the-Web).
const wchar_t* const kCopyTools[] = { L"explorer.exe", L"robocopy.exe", L"xcopy.exe" };

// ------------------------------------------------------------------------------------------
// Internal types
// ------------------------------------------------------------------------------------------
enum class EtwOpKind : uint8_t { Create, Delete, Rename };

// Decoded Kernel-File operation handed from the ETW consumer to the worker.
struct EtwOp
{
    EtwOpKind    kind = EtwOpKind::Create;
    uint8_t      result = kResultUnknown;   // Create: FILE_SUPERSEDED..FILE_OVERWRITTEN
    bool         confirmed = false;         // success confirmed by OperationEnd
    bool         tsKnown = false;           // TS_ID present (and equal to our session)
    bool         sidService = false;        // token user is LocalSystem / LocalService / NetworkService
    DWORD        pid = 0;
    uint64_t     startKey = 0;
    int64_t      eventMs = 0;
    uint64_t     fileObject = 0;
    std::wstring name;                      // kernel path as logged
};

struct PendingOp
{
    EtwOp     op;
    ULONGLONG tick = 0;
};

enum class PullKind : uint8_t { Usn, Rdcw, Recent, Mru };
enum RdcwAction : uint8_t { kRdcwAdded = 1, kRdcwRemoved = 2, kRdcwModified = 3, kRdcwRenamed = 4 };

// Item from a polling source (USN, RDCW, Recent, MRU).
struct PullItem
{
    PullKind     kind = PullKind::Usn;
    uint8_t      sub = 0;          // UsnFileEvent::Kind or RdcwAction
    bool         isDir = false;
    bool         backfill = false;
    bool         isUrl = false;
    int64_t      utcMs = 0;
    std::wstring path;
    std::wstring oldPath;
};

// Where a property lives in UserData, derived from the TDH schema (cached per id/version/bitness).
struct PropLoc
{
    bool  present = false;
    bool  offsetKnown = false;   // all preceding properties have a fixed size
    bool  isString = false;      // null-terminated UTF-16
    ULONG offset = 0;
    ULONG size = 0;              // 0 = variable
};

struct EventLayout
{
    bool           valid = false;
    PropLoc        irp, fileObject, createOptions, extraInfo, status, infoClass, name;
    const wchar_t* nameProp = L"FileName";
};

struct EtwExt
{
    bool     hasTs = false;
    ULONG    ts = 0;
    uint64_t startKey = 0;
    bool     sidService = false;
};

enum class Act : uint8_t { Open, Create, Modify, Delete, Rename, Download };
enum class Src : uint8_t { Etw, Usn, Rdcw, Recent, Mru };

// A decided file event on its way to the callback (possibly held for coalescing).
struct Output
{
    Act          act = Act::Open;
    Src          src = Src::Etw;
    std::wstring path;
    std::wstring oldPath;
    std::wstring originUrl;
    int64_t      eventMs = 0;
    DWORD        pid = 0;
    uint64_t     startKey = 0;
    bool         isDir = false;
    bool         backfill = false;
    bool         remote = false;
    bool         unconfirmed = false;   // ETW degraded mode: success not confirmed
    bool         zoneCheck = false;     // read Zone.Identifier before emitting (CREATE -> DOWNLOAD)
    bool         noDownload = false;    // restored from the Recycle Bin: never a download
    bool         isExplicit = false;    // Recent / MRU
};

struct ProcKey
{
    DWORD    pid = 0;
    uint64_t startKey = 0;
    bool operator==(const ProcKey& o) const { return pid == o.pid && startKey == o.startKey; }
};

struct ProcKeyHash
{
    size_t operator()(const ProcKey& k) const
    {
        uint64_t h = static_cast<uint64_t>(k.pid) * 0x9E3779B97F4A7C15ULL;
        h ^= k.startKey + 0x632BE59BD9B4E019ULL + (h << 6) + (h >> 2);
        return static_cast<size_t>(h ^ (h >> 32));
    }
};

// One process instance (pid + start key): image, session and role, resolved once.
struct ProcEntry
{
    std::wstring exePath;
    std::wstring exeName;       // folded file name
    bool         ownSession = true;
    bool         hardDrop = false;
    bool         sync = false;
    bool         shellInfra = false;
    bool         explorer = false;
    bool         copyTool = false;
    bool         sidService = false;
    bool         classified = false;
    bool         isSystem = false;
    ULONGLONG    createdTick = 0;
};

struct Opener
{
    DWORD    pid = 0;
    uint64_t startKey = 0;
    int64_t  ms = 0;
    bool     weak = false;      // a read-only open by shell/indexer/sync infrastructure
};

struct FoName
{
    std::wstring path;
    int64_t      ms = 0;
};

struct RingItem
{
    std::wstring path;
    std::wstring key;
    int64_t      ms = 0;
};

struct PidRing
{
    uint64_t             startKey = 0;
    ULONGLONG            tick = 0;
    std::deque<RingItem> items;
};

struct RdcwRoot
{
    std::wstring      root;            // "E:\"
    std::wstring      prefix;          // "E:" or "\\server\share" (no trailing separator)
    bool              network = false;
    HANDLE            wakeEvent = nullptr;       // stop this root / release the handle
    HANDLE            releasedEvent = nullptr;   // set once the directory handle is closed
    std::atomic<bool> exited{ false };
    std::atomic<bool> releaseRequested{ false };
    std::thread       thread;

    RdcwRoot() = default;
    RdcwRoot(const RdcwRoot&) = delete;
    RdcwRoot& operator=(const RdcwRoot&) = delete;
    ~RdcwRoot()
    {
        if (thread.joinable())
            thread.join();
        if (wakeEvent != nullptr)
            CloseHandle(wakeEvent);
        if (releasedEvent != nullptr)
            CloseHandle(releasedEvent);
    }
};

const wchar_t* ActName(Act a)
{
    switch (a)
    {
    case Act::Open:     return L"OPEN";
    case Act::Create:   return L"CREATE";
    case Act::Modify:   return L"MODIFY";
    case Act::Delete:   return L"DELETE";
    case Act::Rename:   return L"RENAME";
    case Act::Download: return L"DOWNLOAD";
    }
    return L"OPEN";
}

const wchar_t* SrcName(Src s)
{
    switch (s)
    {
    case Src::Etw:    return L"etw";
    case Src::Usn:    return L"usn";
    case Src::Rdcw:   return L"rdcw";
    case Src::Recent: return L"recent";
    case Src::Mru:    return L"mru";
    }
    return L"etw";
}

EventContextUtil::EventKind KindOf(Act a)
{
    switch (a)
    {
    case Act::Open:     return EventContextUtil::EventKind::FileOpen;
    case Act::Create:   return EventContextUtil::EventKind::FileCreate;
    case Act::Modify:   return EventContextUtil::EventKind::FileEdit;
    case Act::Delete:   return EventContextUtil::EventKind::FileDelete;
    case Act::Rename:   return EventContextUtil::EventKind::FileRename;
    case Act::Download: return EventContextUtil::EventKind::Download;
    }
    return EventContextUtil::EventKind::FileOpen;
}

EventContextUtil::SourceClass ClassOf(Src s)
{
    switch (s)
    {
    case Src::Etw:    return EventContextUtil::SourceClass::Etw;
    case Src::Usn:    return EventContextUtil::SourceClass::Usn;
    case Src::Rdcw:   return EventContextUtil::SourceClass::Rdcw;
    case Src::Recent: return EventContextUtil::SourceClass::Recent;
    case Src::Mru:    return EventContextUtil::SourceClass::Mru;
    }
    return EventContextUtil::SourceClass::Etw;
}
} // namespace

// ==========================================================================================
// Implementation object
// ==========================================================================================
struct FileCapture::Impl
{
    using LoadFn  = std::function<bool(const std::wstring&, std::string&)>;
    using SaveFn  = std::function<void(const std::wstring&, const std::string&)>;
    using HeldMap = std::multimap<ULONGLONG, Output>;

    // ---- configuration ----
    std::mutex cbMtx;
    Callback   callback;
    std::mutex cursorMtx;
    LoadFn     cursorLoad;
    SaveFn     cursorSave;

    // ---- lifecycle (lifecycleMtx serializes Start / Stop / Heartbeat) ----
    std::mutex         lifecycleMtx;
    std::atomic<bool>  running{ false };
    std::atomic<bool>  producersStopping{ false };
    std::atomic<bool>  userPaused{ false };
    std::atomic<bool>  pausedCache{ false };
    std::atomic<DWORD> workerThreadId{ 0 };
    HANDLE             stopEvent = nullptr;   // manual reset; set by Stop
    DWORD              ownPid = 0;
    DWORD              ownSession = 0;

    // ---- queues: ETW consumer / pull sources -> worker ----
    std::mutex              qMtx;
    std::condition_variable workCv;
    std::condition_variable spaceCv;
    std::deque<EtwOp>       etwQ;
    std::deque<PullItem>    pullQ;
    bool                    workerStop = false;   // guarded by qMtx
    std::thread             worker;

    // ---- statistics ----
    std::atomic<uint64_t> statEtw{ 0 }, statUsn{ 0 }, statRdcw{ 0 }, statRecent{ 0 }, statMru{ 0 };
    std::atomic<uint64_t> statEmitted{ 0 }, statFiltered{ 0 }, statDropped{ 0 };
    std::atomic<uint64_t> statEventsLost{ 0 }, statBuffersLost{ 0 }, statRdcwOverflows{ 0 };
    std::atomic<uint64_t> statOtherSession{ 0 }, statUnresolved{ 0 }, statRtLost{ 0 };
    std::atomic<int64_t>  statLastEventMs{ 0 };
    mutable std::mutex    statusMtx;
    std::wstring          lastError;
    std::wstring          sessionNameStat;

    // ---- ETW session ----
    std::wstring         sessionName;
    std::vector<wchar_t> loggerNameBuf;
    std::mutex           traceMtx;
    TRACEHANDLE          traceHandle = INVALID_PROCESSTRACE_HANDLE;
    std::thread          etwThread;
    std::atomic<bool>    etwThreadExited{ true };
    std::atomic<bool>    etwActive{ false };
    std::atomic<bool>    etwUnconfirmed{ false };
    ULONGLONG            etwRetryAtTick = 0;
    ULONGLONG            etwBackoffMs = kEtwBackoffMinMs;
    ULONGLONG            etwStartedTick = 0;
    uint64_t             etwLostLogged = 0;

    // ETW consumer-thread state (only the ProcessTrace thread touches these while it runs).
    std::unordered_map<uint32_t, EventLayout> layouts;
    std::unordered_map<uint64_t, PendingOp>   pending;   // Irp -> start event awaiting OperationEnd
    ULONGLONG lastSweepTick = 0;
    uint64_t  matchedOps = 0;
    uint64_t  expiredOps = 0;

    // ---- device map ----
    std::mutex                       devMtx;
    std::shared_ptr<const DeviceMap> devMap;
    ULONGLONG                        devMapTick = 0;
    std::vector<std::wstring>        extraDevices;     // guarded by devMtx; read by the ETW callback
    std::atomic<DWORD>               usnDriveMask{ 0 };

    // ---- pull sources ----
    UsnJournalReader     usn;
    RecentItemsWatcher   recent;
    OfficeMruReader      mru;
    std::atomic<bool>    usnRunning{ false }, recentRunning{ false }, mruRunning{ false };
    ULONGLONG            usnRetryAtTick = 0, recentRetryAtTick = 0, mruRetryAtTick = 0;
    ULONGLONG            usnVolumeCheckTick = 0, oneDriveRefreshTick = 0, watermarkSaveTick = 0;
    std::atomic<int64_t> recentWatermark{ 0 }, mruWatermark{ 0 };
    int64_t              recentSaved = 0, mruSaved = 0;

    // ---- ReadDirectoryChangesW roots (removable + network) ----
    std::mutex                             rdcwMtx;
    std::vector<std::unique_ptr<RdcwRoot>> rdcwRoots;

    // ---- worker-only state ----
    LruMap<ProcKey, ProcEntry, ProcKeyHash>    procs{ 2048 };
    LruMap<std::wstring, Opener>               openers{ 8192 };        // path -> last opener (USN attribution)
    LruMap<uint64_t, FoName>                   fileObjects{ 16384 };   // FileObject -> opened name (renames)
    LruMap<std::wstring, int64_t>              lastWrite{ 4096 };      // path -> last CREATE/MODIFY/RENAME-in
    LruMap<std::wstring, int64_t>              recentDeletes{ 4096 };  // path -> DELETE / parked by a safe-save
    LruMap<std::wstring, int64_t>              emitKeys{ 8192 };       // duplicate suppression across sources
    LruMap<std::wstring, int64_t>              openSeen{ 8192 };       // pid|path -> last OPEN
    LruMap<std::wstring, int64_t>              explicitSeen{ 1024 };   // path -> last explicit OPEN
    HeldMap                                    held;
    LruMap<std::wstring, HeldMap::iterator>    heldCreates{ 4096 };
    std::deque<std::pair<ULONGLONG, PullItem>> rdcwDelay;
    std::shared_ptr<const DeviceMap>           workerMap;
    WarpSettings                               snapshot;
    ULONGLONG                                  snapshotTick = 0;
    bool                                       snapshotValid = false;
    ULONGLONG                                  pausedCheckTick = 0;

    // ---- rings for RecentPathsForPid / RecentExplicitOpens ----
    mutable std::mutex                 ringMtx;
    std::unordered_map<DWORD, PidRing> pidRings;
    std::deque<RingItem>               explicitRing;

    std::mutex                      onceMtx;
    std::unordered_set<std::string> onceKeys;

    Impl();
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // lifecycle
    bool Start();
    void Stop();
    void StopLocked();
    void Heartbeat();
    void ReconcileSources(const WarpSettings& s, bool initial);
    bool AnySourceRunning();
    void SetError(const std::wstring& text);
    bool FirstTime(const std::string& key);
    void UpdatePausedCache(bool force);
    bool IsPausedNow() const;

    // cursor store
    bool    LoadBlob(const std::wstring& key, std::string& blob);
    void    SaveBlob(const std::wstring& key, const std::string& blob);
    int64_t LoadWatermark(const wchar_t* key);
    void    SaveWatermarks(bool force);
    static void AdvanceWatermark(std::atomic<int64_t>& mark, int64_t ms);

    // device map
    std::shared_ptr<const DeviceMap> CurrentMap();
    std::shared_ptr<const DeviceMap> RefreshDeviceMap(bool force);

    // ETW
    bool StartEtwLocked();
    void StopEtwLocked();
    void StopSessionByName();
    void QueryEtwLoss();
    void EtwThreadProc();
    static VOID WINAPI  EtwRecordThunk(PEVENT_RECORD rec);
    static ULONG WINAPI EtwBufferThunk(PEVENT_TRACE_LOGFILEW log);
    void OnEtwRecord(PEVENT_RECORD rec);
    const EventLayout* GetLayout(PEVENT_RECORD rec, USHORT id);
    void OnOperationEnd(PEVENT_RECORD rec, const EventLayout& layout);
    void InsertPending(uint64_t irp, EtwOp&& op);
    void SweepPending(ULONGLONG now);
    void PushEtw(EtwOp&& op);
    bool IsFileSystemDevicePath(const std::wstring& name);

    // pull sources
    bool PushPull(PullItem&& item);
    void OnUsnEvent(const UsnFileEvent& ev);
    void OnRecentItem(const RecentItemEvent& ev);
    void OnMruItem(const OfficeMruItem& item);
    bool StartUsnLocked();
    void StopUsnLocked();
    bool StartRecentLocked();
    void StopRecentLocked();
    bool StartMruLocked();
    void StopMruLocked();

    // ReadDirectoryChangesW
    void RefreshRdcwRootsLocked();
    void StopAllRdcwLocked();
    void StartRdcwRoot(const std::wstring& root);
    void RdcwThread(RdcwRoot* r);
    void RdcwLoop(RdcwRoot* r, HANDLE dir);
    void ParseRdcwBuffer(RdcwRoot* r, const BYTE* buf, DWORD bytes, std::wstring& pendingOld);
    static DWORD CALLBACK RdcwDeviceCallback(HCMNOTIFICATION notify, PVOID context, CM_NOTIFY_ACTION action,
                                             PCM_NOTIFY_EVENT_DATA data, DWORD size);

    // worker
    void  WorkerMain();
    bool  TakeBatch(std::vector<EtwOp>& etw, std::vector<PullItem>& pull, DWORD waitMs, bool& stopping);
    void  RunBatch(std::vector<EtwOp>& etw, std::vector<PullItem>& pull);
    DWORD NextDueWaitMs() const;
    void  ProcessEtwOp(EtwOp& op);
    void  ProcessPull(PullItem& item);
    void  ProcessUsn(PullItem& item);
    void  ProcessRdcw(PullItem& item);
    void  ProcessExplicitOpen(const std::wstring& rawPath, Src src, int64_t utcMs, bool backfill);
    void  ProcessDue(ULONGLONG now, bool flushAll);
    void  DecideAndSubmitRename(const std::wstring& src, const std::wstring& dst, const Output& base);
    void  Submit(Output&& o);
    void  SubmitModify(Output&& o, const std::wstring& key);
    void  Hold(Output&& o, ULONGLONG due);
    void  ForgetHeldCreate(HeldMap::iterator it);
    void  FinishHeld(Output&& o);
    void  EmitNow(Output& o);
    PathClass  ClassifyFull(const std::wstring& path);
    ProcEntry* GetProc(DWORD pid, uint64_t startKey, int64_t eventMs, bool sessionVerified, bool sidService);
    Opener     LookupOpener(const std::wstring& key, int64_t eventMs);
    void       NoteOpener(const std::wstring& key, DWORD pid, uint64_t startKey, int64_t ms, bool weak);
    const WarpSettings& Snapshot();
    std::wstring OriginUrl(const ZoneIdentifier::Info& zi);
    void NotePidPath(DWORD pid, uint64_t startKey, const std::wstring& path, const std::wstring& key, int64_t ms);
    void NoteExplicit(const std::wstring& path, const std::wstring& key, int64_t ms);
};

FileCapture::Impl::Impl()
{
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

FileCapture::Impl::~Impl()
{
    Stop();
    if (stopEvent != nullptr)
        CloseHandle(stopEvent);
}

void FileCapture::Impl::SetError(const std::wstring& text)
{
    std::lock_guard<std::mutex> lk(statusMtx);
    lastError = text;
}

bool FileCapture::Impl::FirstTime(const std::string& key)
{
    std::lock_guard<std::mutex> lk(onceMtx);
    if (onceKeys.size() >= 256)
        return false;
    return onceKeys.insert(key).second;
}

bool FileCapture::Impl::IsPausedNow() const
{
    if (userPaused.load())
        return true;
    return Settings::Instance().IsPaused(static_cast<int64_t>(std::time(nullptr)));
}

void FileCapture::Impl::UpdatePausedCache(bool force)
{
    if (!force)
    {
        // Rate-limited path: worker thread only.
        const ULONGLONG now = GetTickCount64();
        if (now - pausedCheckTick < kPausedCheckMs)
            return;
        pausedCheckTick = now;
    }
    pausedCache.store(IsPausedNow());
}

// ------------------------------------------------------------------------------------------
// Cursor store: USN cursors per volume, Recent / MRU high-water marks
// ------------------------------------------------------------------------------------------
namespace
{
std::string EncodeUsnCursor(uint64_t journalId, int64_t nextUsn)
{
    return "usn1:" + std::to_string(journalId) + ":" + std::to_string(nextUsn);
}

bool DecodeUsnCursor(const std::string& blob, uint64_t& journalId, int64_t& nextUsn)
{
    if (blob.size() < 5 || blob.compare(0, 5, "usn1:") != 0)
        return false;
    size_t pos = 5;
    uint64_t j = 0;
    int64_t n = 0;
    if (!ParseUnsigned(blob, pos, j) || pos >= blob.size() || blob[pos] != ':')
        return false;
    ++pos;
    if (!ParseSigned(blob, pos, n) || pos != blob.size())
        return false;
    journalId = j;
    nextUsn = n;
    return true;
}
} // namespace

bool FileCapture::Impl::LoadBlob(const std::wstring& key, std::string& blob)
{
    std::lock_guard<std::mutex> lk(cursorMtx);
    if (!cursorLoad)
        return false;
    try
    {
        return cursorLoad(key, blob);
    }
    catch (...)
    {
        return false;
    }
}

void FileCapture::Impl::SaveBlob(const std::wstring& key, const std::string& blob)
{
    std::lock_guard<std::mutex> lk(cursorMtx);
    if (!cursorSave)
        return;
    try
    {
        cursorSave(key, blob);
    }
    catch (...)
    {
    }
}

int64_t FileCapture::Impl::LoadWatermark(const wchar_t* key)
{
    std::string blob;
    if (!LoadBlob(key, blob))
        return 0;
    size_t pos = 0;
    int64_t value = 0;
    return ParseSigned(blob, pos, value) ? value : 0;
}

void FileCapture::Impl::AdvanceWatermark(std::atomic<int64_t>& mark, int64_t ms)
{
    int64_t cur = mark.load();
    while (ms > cur && !mark.compare_exchange_weak(cur, ms))
    {
    }
}

// Caller holds lifecycleMtx.
void FileCapture::Impl::SaveWatermarks(bool force)
{
    const ULONGLONG now = GetTickCount64();
    if (!force && now - watermarkSaveTick < kWatermarkSaveMs)
        return;
    watermarkSaveTick = now;
    const int64_t r = recentWatermark.load();
    if (r > recentSaved)
    {
        SaveBlob(kRecentCursorKey, std::to_string(r));
        recentSaved = r;
    }
    const int64_t m = mruWatermark.load();
    if (m > mruSaved)
    {
        SaveBlob(kMruCursorKey, std::to_string(m));
        mruSaved = m;
    }
}

// ------------------------------------------------------------------------------------------
// Device map (rebuilt lazily on unresolved paths and from Heartbeat, at most every 30 s)
// ------------------------------------------------------------------------------------------
std::shared_ptr<const DeviceMap> FileCapture::Impl::CurrentMap()
{
    std::lock_guard<std::mutex> lk(devMtx);
    return devMap;
}

std::shared_ptr<const DeviceMap> FileCapture::Impl::RefreshDeviceMap(bool force)
{
    {
        std::lock_guard<std::mutex> lk(devMtx);
        const ULONGLONG now = GetTickCount64();
        if (!force && devMap && now - devMapTick < kDeviceMapRefreshMs)
            return devMap;
        devMapTick = now;   // claim the rebuild; concurrent callers keep using the current map
    }
    std::shared_ptr<const DeviceMap> fresh = BuildDeviceMap();   // I/O outside the lock
    std::vector<std::wstring> extras = ExtraDevicePrefixes(*fresh);
    std::lock_guard<std::mutex> lk(devMtx);
    devMap = fresh;
    extraDevices.swap(extras);
    return devMap;
}

// ==========================================================================================
// ETW: Microsoft-Windows-Kernel-File (FILE-1, FILE-3, FILE-4, FILE-9, FILE-10, RUN-3, SIG-5)
// ==========================================================================================
namespace
{
ULONG FixedSizeOf(USHORT inType, ULONG ptrSize, USHORT length, bool fixedLength)
{
    switch (inType)
    {
    case TDH_INTYPE_INT8:
    case TDH_INTYPE_UINT8:
    case TDH_INTYPE_ANSICHAR:
        return 1;
    case TDH_INTYPE_INT16:
    case TDH_INTYPE_UINT16:
    case TDH_INTYPE_UNICODECHAR:
        return 2;
    case TDH_INTYPE_INT32:
    case TDH_INTYPE_UINT32:
    case TDH_INTYPE_HEXINT32:
    case TDH_INTYPE_BOOLEAN:
    case TDH_INTYPE_FLOAT:
        return 4;
    case TDH_INTYPE_INT64:
    case TDH_INTYPE_UINT64:
    case TDH_INTYPE_HEXINT64:
    case TDH_INTYPE_DOUBLE:
    case TDH_INTYPE_FILETIME:
        return 8;
    case TDH_INTYPE_POINTER:
    case TDH_INTYPE_SIZET:
        return ptrSize;
    case TDH_INTYPE_GUID:
    case TDH_INTYPE_SYSTEMTIME:
        return 16;
    case TDH_INTYPE_BINARY:
        return fixedLength ? length : 0;
    default:
        return 0;
    }
}

void AssignProp(EventLayout& layout, const std::wstring& name, const PropLoc& loc)
{
    const wchar_t* n = name.c_str();
    if (_wcsicmp(n, L"Irp") == 0)
        layout.irp = loc;
    else if (_wcsicmp(n, L"FileObject") == 0)
        layout.fileObject = loc;
    else if (_wcsicmp(n, L"CreateOptions") == 0)
        layout.createOptions = loc;
    else if (_wcsicmp(n, L"ExtraInformation") == 0)
        layout.extraInfo = loc;
    else if (_wcsicmp(n, L"Status") == 0)
        layout.status = loc;
    else if (_wcsicmp(n, L"InfoClass") == 0)
        layout.infoClass = loc;
    else if (_wcsicmp(n, layout.nameProp) == 0)
        layout.name = loc;
}

// Locates the properties we need by NAME from the TDH schema. Offsets are only trusted while
// every preceding property has a fixed size; otherwise TdhGetProperty is used per event.
EventLayout BuildLayout(PEVENT_RECORD rec, USHORT id)
{
    EventLayout layout;
    layout.nameProp = (id == kEvDeletePath || id == kEvRenamePath) ? L"FilePath" : L"FileName";
    ULONG size = 0;
    const ULONG status = TdhGetEventInformation(rec, 0, nullptr, nullptr, &size);
    if (status != ERROR_INSUFFICIENT_BUFFER || size < sizeof(TRACE_EVENT_INFO))
        return layout;
    std::vector<ULONGLONG> storage((size + sizeof(ULONGLONG) - 1) / sizeof(ULONGLONG), 0);
    PTRACE_EVENT_INFO info = reinterpret_cast<PTRACE_EVENT_INFO>(storage.data());
    if (TdhGetEventInformation(rec, 0, nullptr, info, &size) != ERROR_SUCCESS)
        return layout;
    const BYTE* base = reinterpret_cast<const BYTE*>(info);
    const ULONG ptrSize = (rec->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) != 0 ? 4 : 8;
    const ULONG count = (std::min)(info->TopLevelPropertyCount, info->PropertyCount);
    ULONG offset = 0;
    bool offsetKnown = true;
    for (ULONG i = 0; i < count; ++i)
    {
        const EVENT_PROPERTY_INFO& pi = info->EventPropertyInfoArray[i];
        const ULONG flags = static_cast<ULONG>(pi.Flags);
        // PropertyStruct | PropertyParamLength | PropertyParamCount => not a plain scalar.
        const bool scalar = (flags & 0x7u) == 0 && pi.count <= 1;
        const USHORT inType = scalar ? pi.nonStructType.InType : static_cast<USHORT>(0);
        const ULONG psize = scalar ? FixedSizeOf(inType, ptrSize, pi.length, (flags & 0x10u) != 0) : 0;   // 0x10 = PropertyParamFixedLength

        PropLoc loc;
        loc.present = true;
        loc.offsetKnown = offsetKnown;
        loc.offset = offset;
        loc.size = psize;
        loc.isString = scalar && inType == TDH_INTYPE_UNICODESTRING;
        if (pi.NameOffset != 0 && pi.NameOffset < size)
        {
            const wchar_t* name = reinterpret_cast<const wchar_t*>(base + pi.NameOffset);
            const size_t maxChars = (size - pi.NameOffset) / sizeof(wchar_t);
            AssignProp(layout, std::wstring(name, wcsnlen(name, maxChars)), loc);
        }
        if (psize == 0)
            offsetKnown = false;
        else
            offset += psize;
    }
    layout.valid = true;
    return layout;
}

bool TdhReadBytes(PEVENT_RECORD rec, const wchar_t* name, std::vector<BYTE>& out)
{
    PROPERTY_DATA_DESCRIPTOR d = {};
    d.PropertyName = static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(name));
    d.ArrayIndex = ULONG_MAX;
    ULONG size = 0;
    if (TdhGetPropertySize(rec, 0, nullptr, 1, &d, &size) != ERROR_SUCCESS || size == 0 || size > 65536)
        return false;
    out.assign(size, 0);
    return TdhGetProperty(rec, 0, nullptr, 1, &d, size, out.data()) == ERROR_SUCCESS;
}

bool BytesToInt(const BYTE* d, size_t size, uint64_t& out)
{
    switch (size)
    {
    case 1: { uint8_t v = 0;  memcpy(&v, d, sizeof(v)); out = v; return true; }
    case 2: { uint16_t v = 0; memcpy(&v, d, sizeof(v)); out = v; return true; }
    case 4: { uint32_t v = 0; memcpy(&v, d, sizeof(v)); out = v; return true; }
    case 8: { uint64_t v = 0; memcpy(&v, d, sizeof(v)); out = v; return true; }
    default: return false;
    }
}

bool ReadIntProp(PEVENT_RECORD rec, const PropLoc& p, const wchar_t* name, uint64_t& out)
{
    if (!p.present)
        return false;
    if (p.offsetKnown && p.size != 0 && rec->UserData != nullptr &&
        static_cast<size_t>(p.offset) + p.size <= rec->UserDataLength)
    {
        if (BytesToInt(static_cast<const BYTE*>(rec->UserData) + p.offset, p.size, out))
            return true;
    }
    std::vector<BYTE> b;
    return TdhReadBytes(rec, name, b) && BytesToInt(b.data(), b.size(), out);
}

bool ReadNameProp(PEVENT_RECORD rec, const EventLayout& layout, std::wstring& out)
{
    out.clear();
    const PropLoc& p = layout.name;
    if (!p.present)
        return false;
    if (p.offsetKnown && p.isString && rec->UserData != nullptr && p.offset < rec->UserDataLength)
    {
        const BYTE* d = static_cast<const BYTE*>(rec->UserData) + p.offset;
        const size_t maxChars = (rec->UserDataLength - p.offset) / sizeof(wchar_t);
        size_t n = 0;
        while (n < maxChars)
        {
            wchar_t c = 0;
            memcpy(&c, d + n * sizeof(wchar_t), sizeof(c));
            if (c == L'\0')
                break;
            ++n;
        }
        out.resize(n);
        if (n != 0)
            memcpy(&out[0], d, n * sizeof(wchar_t));
        return true;
    }
    std::vector<BYTE> b;
    if (!TdhReadBytes(rec, layout.nameProp, b))
        return false;
    const size_t n = b.size() / sizeof(wchar_t);
    out.resize(n);
    if (n != 0)
        memcpy(&out[0], b.data(), n * sizeof(wchar_t));
    while (!out.empty() && out.back() == L'\0')
        out.pop_back();
    return true;
}

EtwExt ReadExt(PEVENT_RECORD rec)
{
    EtwExt e;
    for (USHORT i = 0; i < rec->ExtendedDataCount && rec->ExtendedData != nullptr; ++i)
    {
        const EVENT_HEADER_EXTENDED_DATA_ITEM& item = rec->ExtendedData[i];
        const BYTE* data = reinterpret_cast<const BYTE*>(static_cast<ULONG_PTR>(item.DataPtr));
        if (data == nullptr)
            continue;
        switch (item.ExtType)
        {
        case EVENT_HEADER_EXT_TYPE_TS_ID:
            if (item.DataSize >= sizeof(ULONG))
            {
                ULONG v = 0;
                memcpy(&v, data, sizeof(v));
                e.hasTs = true;
                e.ts = v;
            }
            break;
        case EVENT_HEADER_EXT_TYPE_PROCESS_START_KEY:
            if (item.DataSize >= sizeof(ULONG64))
            {
                ULONG64 v = 0;
                memcpy(&v, data, sizeof(v));
                e.startKey = v;
            }
            break;
        case EVENT_HEADER_EXT_TYPE_SID:
            // Revision, SubAuthorityCount, 6-byte authority, then 4 bytes per sub-authority.
            if (item.DataSize >= 8 && 8u + 4u * static_cast<size_t>(data[1]) <= item.DataSize)
            {
                PSID sid = const_cast<BYTE*>(data);
                if (IsValidSid(sid))
                {
                    e.sidService = IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinLocalServiceSid) ||
                                   IsWellKnownSid(sid, WinNetworkServiceSid);
                }
            }
            break;
        default:
            break;
        }
    }
    return e;
}

// Real-time consumers receive FILETIME stamps (ProcessTrace converts unless raw stamps are requested).
int64_t EtwTimeToUnixMs(int64_t timeStamp)
{
    const int64_t now = NowUnixMs();
    const int64_t ms = FileTimeToUnixMs(timeStamp);
    if (ms <= 0 || ms > now + 60000 || ms < now - 86400000LL)
        return now;
    return ms;
}

EVENT_TRACE_PROPERTIES* InitTraceProps(std::vector<BYTE>& buf, const std::wstring& name)
{
    const size_t bytes = sizeof(EVENT_TRACE_PROPERTIES) + (name.size() + 1) * sizeof(wchar_t) + 1024;
    buf.assign(bytes, 0);
    EVENT_TRACE_PROPERTIES* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    props->Wnode.BufferSize = static_cast<ULONG>(bytes);
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return props;
}
} // namespace

VOID WINAPI FileCapture::Impl::EtwRecordThunk(PEVENT_RECORD rec)
{
    if (rec == nullptr || rec->UserContext == nullptr)
        return;
    Impl* self = static_cast<Impl*>(rec->UserContext);
    try
    {
        self->OnEtwRecord(rec);
    }
    catch (...)
    {
        self->statDropped.fetch_add(1, std::memory_order_relaxed);   // out of memory: drop the event
    }
}

ULONG WINAPI FileCapture::Impl::EtwBufferThunk(PEVENT_TRACE_LOGFILEW log)
{
    if (log == nullptr || log->Context == nullptr)
        return TRUE;
    const Impl* self = static_cast<const Impl*>(log->Context);
    return self->producersStopping.load() ? FALSE : TRUE;   // FALSE makes ProcessTrace return
}

const EventLayout* FileCapture::Impl::GetLayout(PEVENT_RECORD rec, USHORT id)
{
    const bool is32 = (rec->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) != 0;
    const uint32_t key = (static_cast<uint32_t>(id) << 16) |
                         (static_cast<uint32_t>(rec->EventHeader.EventDescriptor.Version) << 8) | (is32 ? 1u : 0u);
    auto it = layouts.find(key);
    if (it == layouts.end())
    {
        if (layouts.size() >= 64)
            return nullptr;   // bounded: only four ids and a few versions are expected
        EventLayout layout = BuildLayout(rec, id);
        if (!layout.valid)
        {
            WARP_LOG_WARN(kComp, "TDH has no schema for Kernel-File event %u v%u; ignoring it",
                          static_cast<unsigned>(id), static_cast<unsigned>(rec->EventHeader.EventDescriptor.Version));
        }
        else
        {
            WARP_LOG_INFO(kComp, "Kernel-File event %u v%u: Irp=%d FileObject=%d CreateOptions=%d ExtraInformation=%d Status=%d InfoClass=%d %s=%d (fixed offsets: name %d)",
                          static_cast<unsigned>(id), static_cast<unsigned>(rec->EventHeader.EventDescriptor.Version),
                          layout.irp.present ? 1 : 0, layout.fileObject.present ? 1 : 0, layout.createOptions.present ? 1 : 0,
                          layout.extraInfo.present ? 1 : 0, layout.status.present ? 1 : 0, layout.infoClass.present ? 1 : 0,
                          U8(layout.nameProp).c_str(), layout.name.present ? 1 : 0, layout.name.offsetKnown ? 1 : 0);
        }
        it = layouts.emplace(key, layout).first;
    }
    return it->second.valid ? &it->second : nullptr;
}

void FileCapture::Impl::OnEtwRecord(PEVENT_RECORD rec)
{
    const EVENT_HEADER& h = rec->EventHeader;
    if (IsEqualGUID(h.ProviderId, kRtLostEventGuid))
    {
        statRtLost.fetch_add(1, std::memory_order_relaxed);
        if (FirstTime("etw-rt-lost"))
            WARP_LOG_WARN(kComp, "ETW real-time delivery reported lost events/buffers (opcode %u)", static_cast<unsigned>(h.EventDescriptor.Opcode));
        return;
    }
    if (!IsEqualGUID(h.ProviderId, kKernelFileProvider))
        return;
    statEtw.fetch_add(1, std::memory_order_relaxed);

    const USHORT id = h.EventDescriptor.Id;
    if (id != kEvCreate && id != kEvOperationEnd && id != kEvDeletePath && id != kEvRenamePath)
        return;
    const EventLayout* layout = GetLayout(rec, id);
    if (layout == nullptr)
        return;
    if (id == kEvOperationEnd)
    {
        OnOperationEnd(rec, *layout);   // completions run in arbitrary contexts: no session/pid filter
        return;
    }
    if (pausedCache.load(std::memory_order_relaxed))
    {
        statFiltered.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const EtwExt ext = ReadExt(rec);
    if (ext.hasTs && ext.ts != ownSession)
    {
        statOtherSession.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const DWORD pid = h.ProcessId;
    if (pid == 0 || pid == 4 || pid == ownPid)
        return;

    EtwOp op;
    op.pid = pid;
    op.tsKnown = ext.hasTs;
    op.startKey = ext.startKey;
    op.sidService = ext.sidService;
    op.eventMs = EtwTimeToUnixMs(h.TimeStamp.QuadPart);

    uint64_t value = 0;
    if (id == kEvCreate)
    {
        op.kind = EtwOpKind::Create;
        if (ReadIntProp(rec, layout->createOptions, L"CreateOptions", value))
        {
            if ((value & (kFileDirectoryFile | kFileOpenByFileId | kFileOpenForFreeSpaceQuery)) != 0)
                return;   // directory, by-id or volume-space opens are never document activity
        }
        else if (FirstTime("etw-no-createoptions"))
        {
            WARP_LOG_WARN(kComp, "Kernel-File Create has no readable CreateOptions; directories are filtered by probing instead");
        }
    }
    else
    {
        op.kind = id == kEvDeletePath ? EtwOpKind::Delete : EtwOpKind::Rename;
        if (ReadIntProp(rec, layout->infoClass, L"InfoClass", value))
        {
            const bool expected = op.kind == EtwOpKind::Delete
                ? (value == kInfoClassDisposition || value == kInfoClassDispositionEx)
                : (value == kInfoClassRename || value == kInfoClassRenameEx);
            if (!expected)
                return;
        }
    }
    if (ReadIntProp(rec, layout->fileObject, L"FileObject", value))
        op.fileObject = value;

    if (!ReadNameProp(rec, *layout, op.name) || op.name.empty())
    {
        if (FirstTime("etw-no-name-" + std::to_string(id)))
            WARP_LOG_WARN(kComp, "Kernel-File event %u carries no readable %s; event ignored", static_cast<unsigned>(id), U8(layout->nameProp).c_str());
        return;
    }
    if (!IsFileSystemDevicePath(op.name))
        return;

    uint64_t irp = 0;
    const bool haveIrp = ReadIntProp(rec, layout->irp, L"Irp", irp);
    if (!haveIrp && FirstTime("etw-no-irp-" + std::to_string(id)))
        WARP_LOG_WARN(kComp, "Kernel-File event %u has no readable Irp; success cannot be confirmed", static_cast<unsigned>(id));
    if (!haveIrp || etwUnconfirmed.load(std::memory_order_relaxed))
    {
        op.result = kResultUnknown;
        op.confirmed = false;
        PushEtw(std::move(op));
        return;
    }
    InsertPending(irp, std::move(op));
}

void FileCapture::Impl::OnOperationEnd(PEVENT_RECORD rec, const EventLayout& layout)
{
    if (pending.empty())
        return;
    uint64_t irp = 0;
    if (!ReadIntProp(rec, layout.irp, L"Irp", irp))
    {
        if (!etwUnconfirmed.exchange(true))
            WARP_LOG_WARN(kComp, "Kernel-File OperationEnd has no readable Irp; degrading to unconfirmed opens");
        return;
    }
    auto it = pending.find(irp);
    if (it == pending.end())
        return;
    EtwOp op = std::move(it->second.op);
    pending.erase(it);
    ++matchedOps;

    uint64_t status = 0;
    if (!ReadIntProp(rec, layout.status, L"Status", status))
    {
        if (FirstTime("etw-no-status"))
            WARP_LOG_WARN(kComp, "Kernel-File OperationEnd has no readable Status; operations are reported unconfirmed");
        op.result = kResultUnknown;
        PushEtw(std::move(op));
        return;
    }
    const LONG nt = static_cast<LONG>(static_cast<uint32_t>(status));
    if (nt < 0 || nt == kNtStatusPending || nt == kNtStatusReparse)
        return;   // failed, or the create was reparsed and will be logged again
    op.confirmed = true;
    if (op.kind == EtwOpKind::Create)
    {
        uint64_t info = 0;
        if (ReadIntProp(rec, layout.extraInfo, L"ExtraInformation", info))
        {
            if (info > kFileOverwritten)
                return;   // FILE_EXISTS / FILE_DOES_NOT_EXIST: nothing was opened
            op.result = static_cast<uint8_t>(info);
        }
        else
        {
            if (FirstTime("etw-no-extrainfo"))
                WARP_LOG_WARN(kComp, "Kernel-File OperationEnd has no readable ExtraInformation; creates reported as opens");
            op.result = kResultUnknown;
        }
    }
    PushEtw(std::move(op));
}

void FileCapture::Impl::InsertPending(uint64_t irp, EtwOp&& op)
{
    const ULONGLONG now = GetTickCount64();
    if (now - lastSweepTick >= 500 || pending.size() >= kMaxPendingIrps)
        SweepPending(now);
    if (pending.size() >= kMaxPendingIrps)
    {
        statDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    PendingOp& slot = pending[irp];   // a reused Irp replaces a start whose completion was lost
    slot.op = std::move(op);
    slot.tick = now;
}

void FileCapture::Impl::SweepPending(ULONGLONG now)
{
    lastSweepTick = now;
    for (auto it = pending.begin(); it != pending.end();)
    {
        if (now - it->second.tick > kPendingTimeoutMs)
        {
            ++expiredOps;
            it = pending.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (matchedOps == 0 && expiredOps >= kUnconfirmedThreshold && !etwUnconfirmed.exchange(true))
    {
        WARP_LOG_WARN(kComp, "Kernel-File OperationEnd never matched a start event (%llu expired); degrading to unconfirmed opens",
                      static_cast<unsigned long long>(expiredOps));
    }
}

void FileCapture::Impl::PushEtw(EtwOp&& op)
{
    bool wake = false;
    {
        std::lock_guard<std::mutex> lk(qMtx);
        if (etwQ.size() >= kEtwQueueCap)
        {
            statDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        wake = etwQ.empty();
        etwQ.push_back(std::move(op));
    }
    if (wake)
        workCv.notify_one();
}

bool FileCapture::Impl::IsFileSystemDevicePath(const std::wstring& n)
{
    if (n.size() < 4 || n.back() == L'\\')
        return false;   // volume or directory open
    const size_t slash = n.find_last_of(L'\\');
    if (slash != std::wstring::npos && n.find(L':', slash + 1) != std::wstring::npos)
        return false;   // alternate data stream
    if (IsDrivePath(n) || StartsWithNoCase(n, L"\\??\\"))
        return true;
    if (!StartsWithNoCase(n, L"\\Device\\"))
        return false;
    for (const wchar_t* p : kNonFsDevicePrefixes)
    {
        if (StartsWithNoCase(n, p))
            return false;
    }
    for (const wchar_t* p : kFsDevicePrefixes)
    {
        if (StartsWithNoCase(n, p))
            return true;
    }
    std::lock_guard<std::mutex> lk(devMtx);
    for (const std::wstring& d : extraDevices)
    {
        if (n.size() > d.size() && n[d.size()] == L'\\' && _wcsnicmp(n.c_str(), d.c_str(), d.size()) == 0)
            return true;
    }
    return false;
}

void FileCapture::Impl::StopSessionByName()
{
    std::vector<BYTE> buf;
    EVENT_TRACE_PROPERTIES* props = InitTraceProps(buf, sessionName);
    const ULONG status = ControlTraceW(0, sessionName.c_str(), props, EVENT_TRACE_CONTROL_STOP);
    if (status != ERROR_SUCCESS && status != ERROR_WMI_INSTANCE_NOT_FOUND)
        WARP_LOG_WARN(kComp, "ControlTrace(STOP, %s) failed: %lu", U8(sessionName).c_str(), status);
}

// Caller holds lifecycleMtx.
bool FileCapture::Impl::StartEtwLocked()
{
    if (etwThread.joinable())
        etwThread.join();   // a consumer that already returned from ProcessTrace
    std::vector<BYTE> propsBuf;
    EVENT_TRACE_PROPERTIES* props = nullptr;
    auto prepare = [&]()
    {
        props = InitTraceProps(propsBuf, sessionName);
        props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        props->Wnode.ClientContext = 1;          // QPC resolution; delivered to us as FILETIME
        props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        props->BufferSize = 64;                  // KB
        props->MinimumBuffers = 64;
        props->MaximumBuffers = 256;
        props->FlushTimer = 1;                   // seconds
    };
    prepare();
    TRACEHANDLE session = 0;
    ULONG status = StartTraceW(&session, sessionName.c_str(), props);
    if (status == ERROR_ALREADY_EXISTS)
    {
        // The name is per Windows session, so this is our own session left behind by a crash.
        WARP_LOG_WARN(kComp, "ETW session %s already exists; stopping that stale session and retrying", U8(sessionName).c_str());
        StopSessionByName();
        prepare();
        status = StartTraceW(&session, sessionName.c_str(), props);
    }
    if (status != ERROR_SUCCESS)
    {
        SetError(L"StartTrace(" + sessionName + L") failed: " + std::to_wstring(status));
        WARP_LOG_ERROR(kComp, "StartTraceW(%s) failed: %lu", U8(sessionName).c_str(), status);
        return false;
    }

    // Provider-side event-ID filter (Windows 8.1+): nothing but these four events is logged.
    const USHORT ids[] = { kEvCreate, kEvOperationEnd, kEvDeletePath, kEvRenamePath };
    std::vector<BYTE> filterBuf(offsetof(EVENT_FILTER_EVENT_ID, Events) + sizeof(ids), 0);
    EVENT_FILTER_EVENT_ID* filter = reinterpret_cast<EVENT_FILTER_EVENT_ID*>(filterBuf.data());
    filter->FilterIn = TRUE;
    filter->Reserved = 0;
    filter->Count = static_cast<USHORT>(ARRAYSIZE(ids));
    memcpy(filterBuf.data() + offsetof(EVENT_FILTER_EVENT_ID, Events), ids, sizeof(ids));
    EVENT_FILTER_DESCRIPTOR desc = {};
    desc.Ptr = static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(filterBuf.data()));
    desc.Size = static_cast<ULONG>(filterBuf.size());
    desc.Type = EVENT_FILTER_TYPE_EVENT_ID;

    ENABLE_TRACE_PARAMETERS params = {};
    params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    params.EnableProperty = EVENT_ENABLE_PROPERTY_TS_ID | EVENT_ENABLE_PROPERTY_SID | EVENT_ENABLE_PROPERTY_PROCESS_START_KEY;
    params.EnableFilterDesc = &desc;
    params.FilterDescCount = 1;

    // FILENAME is enabled together with CREATE because manifests differ in which of the two
    // gates event 12; the event-ID filter keeps NameCreate/NameDelete out of the buffers.
    const ULONGLONG keywords = kKwFileName | kKwCreate | kKwOpEnd | kKwDeletePath | kKwRenameSetLinkPath;
    status = EnableTraceEx2(session, &kKernelFileProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            TRACE_LEVEL_VERBOSE, keywords, 0, 0, &params);
    if (status != ERROR_SUCCESS)
    {
        WARP_LOG_WARN(kComp, "EnableTraceEx2 with an event-ID filter failed (%lu); retrying without it", status);
        params.EnableFilterDesc = nullptr;
        params.FilterDescCount = 0;
        status = EnableTraceEx2(session, &kKernelFileProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                TRACE_LEVEL_VERBOSE, keywords, 0, 0, &params);
    }
    if (status != ERROR_SUCCESS)
    {
        WARP_LOG_WARN(kComp, "EnableTraceEx2 with PROCESS_START_KEY failed (%lu); retrying with TS_ID|SID only", status);
        params.EnableProperty = EVENT_ENABLE_PROPERTY_TS_ID | EVENT_ENABLE_PROPERTY_SID;
        status = EnableTraceEx2(session, &kKernelFileProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                TRACE_LEVEL_VERBOSE, keywords, 0, 0, &params);
    }
    if (status != ERROR_SUCCESS)
    {
        SetError(L"EnableTraceEx2(Kernel-File) failed: " + std::to_wstring(status));
        WARP_LOG_ERROR(kComp, "EnableTraceEx2(Microsoft-Windows-Kernel-File) failed: %lu", status);
        StopSessionByName();
        return false;
    }

    loggerNameBuf.assign(sessionName.begin(), sessionName.end());
    loggerNameBuf.push_back(L'\0');
    EVENT_TRACE_LOGFILEW logFile = {};
    logFile.LoggerName = loggerNameBuf.data();
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = &Impl::EtwRecordThunk;
    logFile.BufferCallback = &Impl::EtwBufferThunk;
    logFile.Context = this;
    const TRACEHANDLE trace = OpenTraceW(&logFile);
    if (trace == INVALID_PROCESSTRACE_HANDLE)
    {
        const DWORD err = GetLastError();
        SetError(L"OpenTrace(" + sessionName + L") failed: " + std::to_wstring(err));
        WARP_LOG_ERROR(kComp, "OpenTraceW(%s) failed: %lu", U8(sessionName).c_str(), err);
        StopSessionByName();
        return false;
    }

    // No consumer thread is running: reset its private state.
    layouts.clear();
    pending.clear();
    lastSweepTick = 0;
    matchedOps = 0;
    expiredOps = 0;
    etwUnconfirmed.store(false);
    {
        std::lock_guard<std::mutex> lk(traceMtx);
        traceHandle = trace;
    }
    etwThreadExited.store(false);
    try
    {
        etwThread = std::thread(&Impl::EtwThreadProc, this);
    }
    catch (...)
    {
        {
            std::lock_guard<std::mutex> lk(traceMtx);
            CloseTrace(traceHandle);
            traceHandle = INVALID_PROCESSTRACE_HANDLE;
        }
        etwThreadExited.store(true);
        StopSessionByName();
        SetError(L"cannot start the ETW consumer thread");
        return false;
    }
    etwStartedTick = GetTickCount64();
    etwActive.store(true);
    WARP_LOG_INFO(kComp, "ETW session %s started (Kernel-File keywords 0x%llx, properties 0x%lx)",
                  U8(sessionName).c_str(), static_cast<unsigned long long>(keywords), static_cast<unsigned long>(params.EnableProperty));
    return true;
}

// Caller holds lifecycleMtx.
void FileCapture::Impl::StopEtwLocked()
{
    {
        // CloseTrace while ProcessTrace runs makes ProcessTrace return after the current buffer.
        std::lock_guard<std::mutex> lk(traceMtx);
        if (traceHandle != INVALID_PROCESSTRACE_HANDLE)
        {
            CloseTrace(traceHandle);
            traceHandle = INVALID_PROCESSTRACE_HANDLE;
        }
    }
    StopSessionByName();
    if (etwThread.joinable())
        etwThread.join();
    etwActive.store(false);
}

void FileCapture::Impl::EtwThreadProc()
{
    try
    {
        TRACEHANDLE trace = INVALID_PROCESSTRACE_HANDLE;
        {
            std::lock_guard<std::mutex> lk(traceMtx);
            trace = traceHandle;
        }
        if (trace != INVALID_PROCESSTRACE_HANDLE)
        {
            const ULONG status = ProcessTrace(&trace, 1, nullptr, nullptr);
            if (!producersStopping.load() && status != ERROR_SUCCESS && status != ERROR_CANCELLED)
                WARP_LOG_WARN(kComp, "ProcessTrace(%s) returned %lu", U8(sessionName).c_str(), status);
        }
    }
    catch (...)
    {
    }
    {
        std::lock_guard<std::mutex> lk(traceMtx);
        if (traceHandle != INVALID_PROCESSTRACE_HANDLE)
        {
            CloseTrace(traceHandle);
            traceHandle = INVALID_PROCESSTRACE_HANDLE;
        }
    }
    etwThreadExited.store(true);
}

// Caller holds lifecycleMtx.
void FileCapture::Impl::QueryEtwLoss()
{
    if (!etwActive.load())
        return;
    std::vector<BYTE> buf;
    EVENT_TRACE_PROPERTIES* props = InitTraceProps(buf, sessionName);
    const ULONG status = ControlTraceW(0, sessionName.c_str(), props, EVENT_TRACE_CONTROL_QUERY);
    if (status != ERROR_SUCCESS)
        return;
    const uint64_t lost = props->EventsLost;
    const uint64_t buffers = static_cast<uint64_t>(props->RealTimeBuffersLost) + props->LogBuffersLost;
    statEventsLost.store(lost);
    statBuffersLost.store(buffers);
    if (lost > etwLostLogged)
    {
        WARP_LOG_WARN(kComp, "ETW session %s has lost %llu events (%llu buffers) since it started",
                      U8(sessionName).c_str(), static_cast<unsigned long long>(lost), static_cast<unsigned long long>(buffers));
        etwLostLogged = lost;
    }
}

// ==========================================================================================
// Polling sources: USN journal, Recent items, Office MRU (FILE-11)
// ==========================================================================================
bool FileCapture::Impl::PushPull(PullItem&& item)
{
    std::unique_lock<std::mutex> lk(qMtx);
    while (pullQ.size() >= kPullQueueCap)
    {
        // Back-pressure only ever stalls a polling thread (never the ETW consumer).
        if (producersStopping.load() || workerStop)
        {
            statDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        spaceCv.wait_for(lk, std::chrono::milliseconds(200));
    }
    const bool wake = pullQ.empty();
    pullQ.push_back(std::move(item));
    lk.unlock();
    if (wake)
        workCv.notify_one();
    return true;
}

void FileCapture::Impl::OnUsnEvent(const UsnFileEvent& ev)
{
    if (pausedCache.load(std::memory_order_relaxed))
    {
        statFiltered.fetch_add(1, std::memory_order_relaxed);   // discarded; the cursor still advances
        return;
    }
    // Pure path rules on the reader thread keep backfill bursts out of the queue.
    const bool userSide = FileCapture::ClassifyPath(ev.path) == PathClass::User ||
                          (ev.kind == UsnFileEvent::Kind::Renamed && FileCapture::ClassifyPath(ev.oldPath) == PathClass::User);
    if (!userSide)
    {
        statFiltered.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    PullItem item;
    item.kind = PullKind::Usn;
    item.sub = static_cast<uint8_t>(ev.kind);
    item.isDir = ev.isDirectory;
    item.backfill = ev.backfill;
    item.utcMs = ev.utcMs;
    item.path = ev.path;
    item.oldPath = ev.oldPath;
    PushPull(std::move(item));
}

void FileCapture::Impl::OnRecentItem(const RecentItemEvent& ev)
{
    statRecent.fetch_add(1, std::memory_order_relaxed);
    AdvanceWatermark(recentWatermark, ev.utcMs);
    if (ev.isUrl || ev.isFolder || !IsFileSystemDosPath(ev.target) || pausedCache.load(std::memory_order_relaxed))
    {
        statFiltered.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    PullItem item;
    item.kind = PullKind::Recent;
    item.utcMs = ev.utcMs;
    item.backfill = ev.backfill;
    item.path = ev.target;
    PushPull(std::move(item));
}

void FileCapture::Impl::OnMruItem(const OfficeMruItem& mruItem)
{
    statMru.fetch_add(1, std::memory_order_relaxed);
    AdvanceWatermark(mruWatermark, mruItem.utcMs);
    if (pausedCache.load(std::memory_order_relaxed))
    {
        statFiltered.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    PullItem item;
    item.kind = PullKind::Mru;
    item.utcMs = mruItem.utcMs;
    item.backfill = mruItem.backfill;
    item.isUrl = mruItem.isUrl;
    item.path = mruItem.pathOrUrl;
    PushPull(std::move(item));
}

// Caller holds lifecycleMtx.
bool FileCapture::Impl::StartUsnLocked()
{
    // Set before the reader starts so ETW defers creates/edits on these volumes to the journal.
    usnDriveMask.store(ProbeUsnDrives());
    UsnJournalReader::CursorLoad load = [this](const std::wstring& volume, uint64_t& journalId, int64_t& nextUsn) -> bool
    {
        std::string blob;
        return LoadBlob(volume, blob) && DecodeUsnCursor(blob, journalId, nextUsn);
    };
    UsnJournalReader::CursorSave save = [this](const std::wstring& volume, uint64_t journalId, int64_t nextUsn)
    {
        SaveBlob(volume, EncodeUsnCursor(journalId, nextUsn));
    };
    if (!usn.Start([this](const UsnFileEvent& ev)
                   {
                       try { OnUsnEvent(ev); }
                       catch (...) { statDropped.fetch_add(1, std::memory_order_relaxed); }
                   },
                   load, save, kBackfillHours))
    {
        usnDriveMask.store(0);
        WARP_LOG_WARN(kComp, "USN journal reader not started: %s", U8(usn.Status()).c_str());
        return false;
    }
    usnRunning.store(true);
    usnVolumeCheckTick = GetTickCount64();
    WARP_LOG_INFO(kComp, "USN journal reader started (drive mask 0x%lx)", static_cast<unsigned long>(usnDriveMask.load()));
    return true;
}

void FileCapture::Impl::StopUsnLocked()
{
    usn.Stop();   // persists the cursors through the save function
    usnRunning.store(false);
    usnDriveMask.store(0);
}

bool FileCapture::Impl::StartRecentLocked()
{
    const int64_t saved = LoadWatermark(kRecentCursorKey);
    recentSaved = saved;
    const int64_t since = (std::max)(saved, NowUnixMs() - kBackfillMs);
    AdvanceWatermark(recentWatermark, since);
    if (!recent.Start([this](const RecentItemEvent& ev)
                      {
                          try { OnRecentItem(ev); }
                          catch (...) { statDropped.fetch_add(1, std::memory_order_relaxed); }
                      },
                      since))
    {
        WARP_LOG_WARN(kComp, "Recent-items watcher not started");
        return false;
    }
    recentRunning.store(true);
    return true;
}

void FileCapture::Impl::StopRecentLocked()
{
    recent.Stop();
    recentRunning.store(false);
}

bool FileCapture::Impl::StartMruLocked()
{
    const int64_t saved = LoadWatermark(kMruCursorKey);
    mruSaved = saved;
    const int64_t since = (std::max)(saved, NowUnixMs() - kBackfillMs);
    AdvanceWatermark(mruWatermark, since);
    if (!mru.Start([this](const OfficeMruItem& item)
                   {
                       try { OnMruItem(item); }
                       catch (...) { statDropped.fetch_add(1, std::memory_order_relaxed); }
                   },
                   since, kMruPollMs))
    {
        WARP_LOG_WARN(kComp, "Office MRU reader not started");
        return false;
    }
    mruRunning.store(true);
    return true;
}

void FileCapture::Impl::StopMruLocked()
{
    mru.Stop();
    mruRunning.store(false);
}

// ==========================================================================================
// Worker: resolution, filtering, attribution, coalescing, scoring, callback (FILE-10)
// ==========================================================================================
namespace
{
std::wstring QueryImagePath(HANDLE process)
{
    std::vector<wchar_t> buf(MAX_PATH + 1, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        DWORD size = static_cast<DWORD>(buf.size());
        if (QueryFullProcessImageNameW(process, 0, buf.data(), &size))
            return std::wstring(buf.data(), size);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            break;
        buf.assign(buf.size() * 2, L'\0');
    }
    return std::wstring();
}

// Image of the process instance that logged an event at eventMs (FILE-9): a live process
// created after the event holds a recycled PID; the process table (fed by Kernel-Process
// start/stop) covers processes that already exited.
std::wstring ResolveProcessImage(DWORD pid, uint64_t startKey, int64_t eventMs)
{
    if (pid == 0 || pid == 4)
        return std::wstring();
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process != nullptr)
    {
        bool sameInstance = true;
        FILETIME created = {}, exited = {}, kernel = {}, user = {};
        if (eventMs > 0 && GetProcessTimes(process, &created, &exited, &kernel, &user))
            sameInstance = FileTimeToUnixMs(created) <= eventMs + 2000;
        const std::wstring path = sameInstance ? QueryImagePath(process) : std::wstring();
        CloseHandle(process);
        if (!path.empty())
            return path;
    }
    EventContextUtil::ProcessInfo info;
    if (EventContextUtil::LookupProcess(pid, info) && !info.imagePath.empty())
    {
        const bool sameKey = startKey != 0 && info.startKey == startKey;
        const bool startedBefore = info.startMs == 0 || eventMs == 0 || info.startMs <= eventMs + 2000;
        const bool aliveThen = info.exitMs == 0 || eventMs == 0 || info.exitMs + 10000 >= eventMs;
        if (sameKey || (startedBefore && aliveThen))
            return info.imagePath;
    }
    return std::wstring();
}

bool SameRoot(const std::wstring& a, const std::wstring& b)
{
    const size_t ra = RootLength(a);
    return ra != 0 && ra == RootLength(b) && a.compare(0, ra, b, 0, ra) == 0;
}

bool IsRemoteDosPath(const std::wstring& path)
{
    if (IsUncPath(path))
        return true;
    if (!IsDrivePath(path))
        return false;
    const wchar_t root[4] = { path[0], L':', L'\\', L'\0' };
    return GetDriveTypeW(root) == DRIVE_REMOTE;
}

// Device part of a kernel path for diagnostics (never the file name): "\Device\HarddiskVolume9".
std::string DevicePart(const std::wstring& nt)
{
    size_t pos = 0;
    for (int i = 0; i < 2 && pos != std::wstring::npos; ++i)
        pos = nt.find(L'\\', pos + 1);
    return U8(pos == std::wstring::npos ? nt : nt.substr(0, pos));
}
} // namespace

bool FileCapture::Impl::TakeBatch(std::vector<EtwOp>& etw, std::vector<PullItem>& pull, DWORD waitMs, bool& stopping)
{
    std::unique_lock<std::mutex> lk(qMtx);
    if (waitMs != 0 && etwQ.empty() && pullQ.empty() && !workerStop)
        workCv.wait_for(lk, std::chrono::milliseconds(waitMs));
    stopping = workerStop;
    while (!etwQ.empty() && etw.size() < kBatchMax)
    {
        etw.push_back(std::move(etwQ.front()));
        etwQ.pop_front();
    }
    const bool tookPull = !pullQ.empty();
    while (!pullQ.empty() && pull.size() < kBatchMax)
    {
        pull.push_back(std::move(pullQ.front()));
        pullQ.pop_front();
    }
    lk.unlock();
    if (tookPull)
        spaceCv.notify_all();
    return !etw.empty() || !pull.empty();
}

void FileCapture::Impl::WorkerMain()
{
    workerThreadId.store(GetCurrentThreadId());
    std::vector<EtwOp> etw;
    std::vector<PullItem> pull;
    bool stopping = false;
    while (!stopping)
    {
        try
        {
            TakeBatch(etw, pull, NextDueWaitMs(), stopping);
            RunBatch(etw, pull);
        }
        catch (...)
        {
            etw.clear();
            pull.clear();
            if (FirstTime("worker-exception"))
                WARP_LOG_ERROR(kComp, "file capture worker: unexpected exception (batch dropped)");
        }
    }
    try
    {
        // Producers are already stopped: drain what they left (bounded), then flush held events.
        const ULONGLONG deadline = GetTickCount64() + kStopDrainMs;
        bool ignored = false;
        while (TakeBatch(etw, pull, 0, ignored))
        {
            RunBatch(etw, pull);
            if (GetTickCount64() > deadline)
            {
                std::lock_guard<std::mutex> lk(qMtx);
                statDropped.fetch_add(etwQ.size() + pullQ.size(), std::memory_order_relaxed);
                etwQ.clear();
                pullQ.clear();
                break;
            }
        }
        for (int pass = 0; pass < 3 && (!held.empty() || !rdcwDelay.empty()); ++pass)
            ProcessDue(GetTickCount64(), true);
    }
    catch (...)
    {
    }
    workerThreadId.store(0);
}

void FileCapture::Impl::RunBatch(std::vector<EtwOp>& etw, std::vector<PullItem>& pull)
{
    UpdatePausedCache(false);
    workerMap = CurrentMap();
    for (EtwOp& op : etw)
    {
        try
        {
            ProcessEtwOp(op);
        }
        catch (...)
        {
            statDropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    etw.clear();
    for (PullItem& item : pull)
    {
        try
        {
            ProcessPull(item);
        }
        catch (...)
        {
            statDropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    pull.clear();
    ProcessDue(GetTickCount64(), false);
}

DWORD FileCapture::Impl::NextDueWaitMs() const
{
    ULONGLONG due = ULLONG_MAX;
    if (!held.empty())
        due = held.begin()->first;
    if (!rdcwDelay.empty() && rdcwDelay.front().first < due)
        due = rdcwDelay.front().first;
    if (due == ULLONG_MAX)
        return kMaxWorkerWaitMs;
    const ULONGLONG now = GetTickCount64();
    if (due <= now)
        return 1;
    return static_cast<DWORD>((std::min)(due - now, static_cast<ULONGLONG>(kMaxWorkerWaitMs)));
}

FileCapture::PathClass FileCapture::Impl::ClassifyFull(const std::wstring& path)
{
    const PathClass c = FileCapture::ClassifyPath(path);
    if (c == PathClass::User && Settings::Instance().IsExcludedPath(path))
        return PathClass::Excluded;   // user / policy exclusion
    return c;
}

ProcEntry* FileCapture::Impl::GetProc(DWORD pid, uint64_t startKey, int64_t eventMs, bool sessionVerified, bool sidService)
{
    const ProcKey key{ pid, startKey };
    ProcEntry* cached = procs.Get(key);
    if (cached != nullptr && (startKey != 0 || GetTickCount64() - cached->createdTick < kProcNoKeyTtlMs))
    {
        if (sessionVerified)
            cached->ownSession = true;
        if (sidService)
            cached->sidService = true;
        return cached;
    }
    ProcEntry e;
    e.createdTick = GetTickCount64();
    e.sidService = sidService;
    e.exePath = ResolveProcessImage(pid, startKey, eventMs);
    e.exeName = FoldCase(LastComponent(e.exePath));
    e.ownSession = sessionVerified || EventContextUtil::IsOwnSessionPid(pid);
    e.explorer = e.exeName == L"explorer.exe";
    e.sync = InTable(e.exeName, kSyncClients);
    for (const wchar_t* prefix : kSyncClientPrefixes)
    {
        if (StartsWith(e.exeName, prefix))
            e.sync = true;
    }
    e.shellInfra = InTable(e.exeName, kShellInfra);
    e.copyTool = InTable(e.exeName, kCopyTools);
    e.hardDrop = pid == 4 || (!e.exeName.empty() && WARP::SystemProcessClassifier::IsHardBlocked(e.exeName) &&
                              !InTable(e.exeName, kNeverHardDrop));
    return &procs.Put(key, std::move(e));
}

Opener FileCapture::Impl::LookupOpener(const std::wstring& key, int64_t eventMs)
{
    const Opener* o = openers.Peek(key);
    if (o != nullptr && o->ms <= eventMs + kOpenerSlackMs && eventMs - o->ms <= kOpenerWindowMs)
        return *o;
    return Opener();
}

void FileCapture::Impl::NoteOpener(const std::wstring& key, DWORD pid, uint64_t startKey, int64_t ms, bool weak)
{
    const Opener* cur = openers.Peek(key);
    if (weak && cur != nullptr && !cur->weak && cur->pid != pid && ms - cur->ms < kOpenerWindowMs)
        return;   // an indexer/thumbnailer/sync read must not displace the app that wrote the file
    Opener o;
    o.pid = pid;
    o.startKey = startKey;
    o.ms = ms;
    o.weak = weak;
    openers.Put(key, o);
}

void FileCapture::Impl::ProcessEtwOp(EtwOp& op)
{
    ProcEntry* pe = GetProc(op.pid, op.startKey, op.eventMs, op.tsKnown, op.sidService);
    if (!pe->ownSession || pe->hardDrop || (!pe->exePath.empty() && Settings::Instance().IsExcludedApp(pe->exePath)))
    {
        statFiltered.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool infraRole = pe->shellInfra || pe->sync || pe->explorer;

    std::wstring dos;
    VolInfo vi;
    const DWORD usnMask = usnDriveMask.load();
    if (!workerMap || !ResolveNtPath(*workerMap, usnMask, op.name, dos, vi))
    {
        workerMap = RefreshDeviceMap(false);   // a volume mounted since the last refresh (rate-limited)
        if (!workerMap || !ResolveNtPath(*workerMap, usnMask, op.name, dos, vi))
        {
            statUnresolved.fetch_add(1, std::memory_order_relaxed);
            if (FirstTime("unresolved-" + DevicePart(op.name)))
                WARP_LOG_WARN(kComp, "cannot map kernel device %s to a DOS path", DevicePart(op.name).c_str());
            return;
        }
    }
    dos = EntityKey::CanonicalPath(dos);
    if (RootLength(dos) >= dos.size())
        return;   // volume or share root
    const std::wstring key = FoldCase(dos);

    Output o;
    o.src = Src::Etw;
    o.eventMs = op.eventMs;
    o.pid = op.pid;
    o.startKey = op.startKey;
    o.remote = vi.remote;
    o.unconfirmed = !op.confirmed;

    switch (op.kind)
    {
    case EtwOpKind::Create:
    {
        const PathClass cls = ClassifyFull(dos);
        if (op.fileObject != 0)
        {
            FoName fo;
            fo.path = dos;
            fo.ms = op.eventMs;
            fileObjects.Put(op.fileObject, std::move(fo));
        }
        const bool readOnly = op.result == kFileOpened || op.result == kResultUnknown;
        if (cls != PathClass::Excluded || FileCapture::IsRecycleBinPath(dos))
            NoteOpener(key, op.pid, op.startKey, op.eventMs, infraRole && readOnly);
        if (cls != PathClass::User)
            return;

        if (op.result == kFileCreated)
            o.act = Act::Create;
        else if (op.result == kFileOverwritten || op.result == kFileSuperseded)
            o.act = Act::Modify;
        else
            o.act = Act::Open;   // FILE_OPENED, or unconfirmed (degraded mode)

        if (o.act != Act::Open && vi.usn)
            return;   // the journal reports creates/edits on this volume at close, with attribution from here
        if (o.act == Act::Open)
        {
            // Directories opened without FILE_DIRECTORY_FILE; never probe network paths.
            if (!vi.remote)
            {
                const int kind = ProbeDirectory(dos);
                if (kind == 1 || (kind == -1 && op.result == kResultUnknown))
                    return;
            }
            else if (LastComponent(dos).find(L'.') == std::wstring::npos)
            {
                return;
            }
        }
        o.path = dos;
        o.zoneCheck = o.act == Act::Create;
        Submit(std::move(o));
        break;
    }
    case EtwOpKind::Delete:
    {
        NoteOpener(key, op.pid, op.startKey, op.eventMs, false);
        if (vi.usn || ClassifyFull(dos) != PathClass::User)
            return;
        o.act = Act::Delete;
        o.path = dos;
        Submit(std::move(o));
        break;
    }
    case EtwOpKind::Rename:
    {
        NoteOpener(key, op.pid, op.startKey, op.eventMs, false);
        FoName* fo = op.fileObject != 0 ? fileObjects.Peek(op.fileObject) : nullptr;
        if (fo == nullptr || op.eventMs - fo->ms > kFileObjectTtlMs)
            return;   // name of the renamed handle unknown; USN / RDCW report this rename
        const std::wstring known = fo->path;
        const std::wstring knownKey = FoldCase(known);
        NoteOpener(knownKey, op.pid, op.startKey, op.eventMs, false);
        if (knownKey == key)
        {
            if (FirstTime("etw-renamepath-source"))
                WARP_LOG_INFO(kComp, "Kernel-File RenamePath.FilePath is the SOURCE name here; renames come from USN / RDCW");
            return;
        }
        if (!SameRoot(knownKey, key))
            return;   // stale FileObject entry (renames never cross volumes)
        if (FirstTime("etw-renamepath-target"))
            WARP_LOG_INFO(kComp, "Kernel-File RenamePath.FilePath is the TARGET name here");
        fo->path = dos;
        fo->ms = op.eventMs;
        if (vi.usn)
            return;
        DecideAndSubmitRename(known, dos, o);
        break;
    }
    }
}

void FileCapture::Impl::ProcessPull(PullItem& item)
{
    switch (item.kind)
    {
    case PullKind::Usn:
        ProcessUsn(item);
        break;
    case PullKind::Rdcw:
        // Held briefly so the attributed ETW view of the same change wins the duplicate check.
        statRdcw.fetch_add(1, std::memory_order_relaxed);
        if (rdcwDelay.size() >= kMaxRdcwDelay)
            ProcessRdcw(item);
        else
            rdcwDelay.emplace_back(GetTickCount64() + kRdcwDelayMs, std::move(item));
        break;
    case PullKind::Recent:
        ProcessExplicitOpen(item.path, Src::Recent, item.utcMs, item.backfill);
        break;
    case PullKind::Mru:
        if (item.isUrl)
        {
            std::wstring local;
            if (!OneDriveMapper::Instance().UrlToLocalPath(item.path, local))
            {
                statFiltered.fetch_add(1, std::memory_order_relaxed);   // cloud-only document
                break;
            }
            ProcessExplicitOpen(local, Src::Mru, item.utcMs, item.backfill);
        }
        else
        {
            ProcessExplicitOpen(item.path, Src::Mru, item.utcMs, item.backfill);
        }
        break;
    }
}

void FileCapture::Impl::ProcessUsn(PullItem& item)
{
    statUsn.fetch_add(1, std::memory_order_relaxed);
    const std::wstring path = EntityKey::CanonicalPath(item.path);
    const std::wstring oldPath = item.oldPath.empty() ? std::wstring() : EntityKey::CanonicalPath(item.oldPath);
    const std::wstring key = FoldCase(path);
    const std::wstring oldKey = FoldCase(oldPath);
    if (IsOtherUsersProfile(key) || (!oldKey.empty() && IsOtherUsersProfile(oldKey)))
    {
        statFiltered.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int64_t eventMs = item.utcMs > 0 ? item.utcMs : NowUnixMs();

    // Attribution: the process that most recently opened/created/renamed the path (from ETW).
    Opener who;
    if (!item.backfill)
    {
        if (!oldKey.empty())
            who = LookupOpener(oldKey, eventMs);
        if (who.pid == 0)
            who = LookupOpener(key, eventMs);
    }
    if (who.pid != 0)
    {
        const ProcEntry* pe = GetProc(who.pid, who.startKey, eventMs, true, false);
        if (pe->hardDrop || (!pe->exePath.empty() && Settings::Instance().IsExcludedApp(pe->exePath)))
        {
            statFiltered.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    Output base;
    base.src = Src::Usn;
    base.eventMs = eventMs;
    base.pid = who.pid;
    base.startKey = who.startKey;
    base.isDir = item.isDir;
    base.backfill = item.backfill;
    switch (static_cast<UsnFileEvent::Kind>(item.sub))
    {
    case UsnFileEvent::Kind::Created:
        if (ClassifyFull(path) != PathClass::User)
            return;
        base.act = Act::Create;
        base.path = path;
        base.zoneCheck = !item.isDir;
        Submit(std::move(base));
        break;
    case UsnFileEvent::Kind::Modified:
        if (item.isDir || ClassifyFull(path) != PathClass::User)
            return;
        base.act = Act::Modify;
        base.path = path;
        Submit(std::move(base));
        break;
    case UsnFileEvent::Kind::Deleted:
        if (ClassifyFull(path) != PathClass::User)
            return;
        base.act = Act::Delete;
        base.path = path;
        Submit(std::move(base));
        break;
    case UsnFileEvent::Kind::Renamed:
        if (!oldPath.empty())
            DecideAndSubmitRename(oldPath, path, base);
        break;
    }
}

void FileCapture::Impl::ProcessRdcw(PullItem& item)
{
    const std::wstring path = EntityKey::CanonicalPath(item.path);
    const std::wstring oldPath = item.oldPath.empty() ? std::wstring() : EntityKey::CanonicalPath(item.oldPath);
    const std::wstring key = FoldCase(path);
    const std::wstring oldKey = FoldCase(oldPath);
    if (IsOtherUsersProfile(key) || (!oldKey.empty() && IsOtherUsersProfile(oldKey)))
        return;

    // RDCW carries no PID: attribute through the ETW opener map like the journal does.
    Opener who;
    if (!oldKey.empty())
        who = LookupOpener(oldKey, item.utcMs);
    if (who.pid == 0)
        who = LookupOpener(key, item.utcMs);
    if (who.pid != 0)
    {
        const ProcEntry* pe = GetProc(who.pid, who.startKey, item.utcMs, true, false);
        if (pe->hardDrop || (!pe->exePath.empty() && Settings::Instance().IsExcludedApp(pe->exePath)))
        {
            statFiltered.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    Output base;
    base.src = Src::Rdcw;
    base.eventMs = item.utcMs;
    base.pid = who.pid;
    base.startKey = who.startKey;
    base.remote = IsRemoteDosPath(path);
    switch (item.sub)
    {
    case kRdcwAdded:
        if (ClassifyFull(path) != PathClass::User)
            return;
        base.act = Act::Create;
        base.path = path;
        base.zoneCheck = true;
        Submit(std::move(base));
        break;
    case kRdcwRemoved:
        if (ClassifyFull(path) != PathClass::User)
            return;
        base.act = Act::Delete;
        base.path = path;
        Submit(std::move(base));
        break;
    case kRdcwModified:
        if (ClassifyFull(path) != PathClass::User)
            return;
        base.act = Act::Modify;
        base.path = path;
        Submit(std::move(base));
        break;
    case kRdcwRenamed:
        if (!oldPath.empty())
            DecideAndSubmitRename(oldPath, path, base);
        break;
    default:
        break;
    }
}

// Recent .lnk and Office MRU entries: explicit user intent (FILE-11). Only privacy exclusions and
// transient names apply; built-in "noise" rules are for kernel-observed activity.
void FileCapture::Impl::ProcessExplicitOpen(const std::wstring& rawPath, Src src, int64_t utcMs, bool backfill)
{
    if (!IsFileSystemDosPath(rawPath))
    {
        statFiltered.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::wstring path = EntityKey::CanonicalPath(rawPath);
    if (RootLength(path) >= path.size() || FileCapture::IsTransientName(path) || Settings::Instance().IsExcludedPath(path))
    {
        statFiltered.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::wstring key = FoldCase(path);
    const int64_t eventMs = utcMs > 0 ? utcMs : NowUnixMs();
    const int64_t* seen = explicitSeen.Peek(key);
    if (seen != nullptr && AbsDiff(*seen, eventMs) < kExplicitDedupMs)
    {
        statFiltered.fetch_add(1, std::memory_order_relaxed);   // the same open seen by Recent and MRU
        return;
    }
    explicitSeen.Put(key, eventMs);
    NoteExplicit(path, key, eventMs);

    Output o;
    o.act = Act::Open;
    o.src = src;
    o.path = path;
    o.eventMs = eventMs;
    o.backfill = backfill;
    o.isExplicit = true;
    EmitNow(o);
}

void FileCapture::Impl::DecideAndSubmitRename(const std::wstring& src, const std::wstring& dst, const Output& base)
{
    const PathClass sc = ClassifyFull(src);
    const PathClass dc = ClassifyFull(dst);
    const RenameOutcome outcome = FileCapture::DecideRename(sc, dc, FileCapture::IsRecycleBinPath(dst),
                                                            FileCapture::IsDownloadRename(src, dst));
    Output o = base;
    switch (outcome)
    {
    case RenameOutcome::Drop:
        if (sc == PathClass::User && dc == PathClass::Transient)
            recentDeletes.Put(FoldCase(src), base.eventMs);   // parked by a safe-save: a re-create is a MODIFY
        else
            statFiltered.fetch_add(1, std::memory_order_relaxed);
        return;
    case RenameOutcome::Rename:
        o.act = Act::Rename;
        o.path = dst;
        o.oldPath = src;
        break;
    case RenameOutcome::ModifyDest:
        o.act = o.isDir ? Act::Create : Act::Modify;   // a temp-named folder renamed into place appears
        o.path = dst;
        break;
    case RenameOutcome::DownloadDest:
        o.act = o.isDir ? Act::Create : Act::Download;
        o.path = dst;
        o.zoneCheck = !o.isDir;
        break;
    case RenameOutcome::CreateDest:
        o.act = Act::Create;
        o.path = dst;
        o.noDownload = FileCapture::IsRecycleBinPath(src);   // restored from the Recycle Bin
        o.zoneCheck = !o.isDir && !o.noDownload;
        break;
    case RenameOutcome::DeleteSource:
        o.act = Act::Delete;
        o.path = src;
        break;
    }
    Submit(std::move(o));
}

// ------------------------------------------------------------------------------------------
// Coalescing (FILE-2): saves, safe-save renames, replace-deletes, temporary files
// ------------------------------------------------------------------------------------------
void FileCapture::Impl::ProcessDue(ULONGLONG now, bool flushAll)
{
    while (!rdcwDelay.empty() && (flushAll || rdcwDelay.front().first <= now))
    {
        PullItem item = std::move(rdcwDelay.front().second);
        rdcwDelay.pop_front();
        try
        {
            ProcessRdcw(item);
        }
        catch (...)
        {
            statDropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    while (!held.empty() && (flushAll || held.begin()->first <= now))
    {
        HeldMap::iterator it = held.begin();
        ForgetHeldCreate(it);
        Output o = std::move(it->second);
        held.erase(it);
        try
        {
            FinishHeld(std::move(o));
        }
        catch (...)
        {
            statDropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void FileCapture::Impl::Submit(Output&& o)
{
    const std::wstring key = FoldCase(o.path);
    const ULONGLONG now = GetTickCount64();
    switch (o.act)
    {
    case Act::Open:
    {
        const int64_t* lw = lastWrite.Peek(key);
        if (lw != nullptr && *lw >= o.eventMs - kOpenAfterWriteMs)
        {
            statFiltered.fetch_add(1, std::memory_order_relaxed);   // the open belongs to a save
            return;
        }
        const std::wstring seenKey = std::to_wstring(o.pid) + L"|" + key;
        const int64_t* seen = openSeen.Peek(seenKey);
        if (seen != nullptr && o.eventMs >= *seen && o.eventMs - *seen < kOpenRepeatMs)
        {
            statFiltered.fetch_add(1, std::memory_order_relaxed);   // apps reopen documents many times while loading
            return;
        }
        openSeen.Put(seenKey, o.eventMs);
        NotePidPath(o.pid, o.startKey, o.path, key, o.eventMs);
        Hold(std::move(o), now + kHoldOpenMs);   // dropped if an edit of the same path follows
        return;
    }
    case Act::Create:
    case Act::Download:
    {
        if (o.act == Act::Create)
        {
            const int64_t* rd = recentDeletes.Peek(key);
            if (rd != nullptr && o.eventMs >= *rd - 500 && o.eventMs - *rd <= kReplaceWindowMs)
            {
                // Deleted (or parked under a temp name) and written again: an edit, not a new file.
                o.act = Act::Modify;
                o.zoneCheck = false;
                SubmitModify(std::move(o), key);
                return;
            }
        }
        lastWrite.Put(key, o.eventMs);
        NotePidPath(o.pid, o.startKey, o.path, key, o.eventMs);
        Hold(std::move(o), now + kHoldCreateMs);   // Zone.Identifier is read when released
        return;
    }
    case Act::Modify:
        SubmitModify(std::move(o), key);
        return;
    case Act::Rename:
    {
        const std::wstring oldKey = FoldCase(o.oldPath);
        HeldMap::iterator* pendingCreate = heldCreates.Peek(oldKey);
        if (pendingCreate != nullptr)
        {
            // Created moments ago and renamed right away: report one creation under the final name.
            const HeldMap::iterator victim = *pendingCreate;
            Output created = std::move(victim->second);
            heldCreates.Erase(oldKey);
            held.erase(victim);
            created.path = o.path;
            created.isDir = o.isDir;
            Submit(std::move(created));
            return;
        }
        lastWrite.Put(key, o.eventMs);
        EmitNow(o);
        return;
    }
    case Act::Delete:
    {
        HeldMap::iterator* pendingCreate = heldCreates.Peek(key);
        if (pendingCreate != nullptr)
        {
            // Created and deleted within the hold window: a temporary file; report neither.
            const HeldMap::iterator victim = *pendingCreate;
            heldCreates.Erase(key);
            held.erase(victim);
            statFiltered.fetch_add(2, std::memory_order_relaxed);
            return;
        }
        const int64_t* lw = lastWrite.Peek(key);
        if (lw != nullptr && AbsDiff(*lw, o.eventMs) < kSaveCoalesceMs)
        {
            statFiltered.fetch_add(1, std::memory_order_relaxed);   // the old version replaced by a save
            return;
        }
        recentDeletes.Put(key, o.eventMs);
        Hold(std::move(o), now + kHoldDeleteMs);
        return;
    }
    }
}

void FileCapture::Impl::SubmitModify(Output&& o, const std::wstring& key)
{
    const int64_t* lw = lastWrite.Peek(key);
    if (lw != nullptr && AbsDiff(*lw, o.eventMs) < kSaveCoalesceMs)
    {
        statFiltered.fetch_add(1, std::memory_order_relaxed);   // the same save seen again (USN/ETW/RDCW, repeated writes)
        return;
    }
    lastWrite.Put(key, o.eventMs);
    NotePidPath(o.pid, o.startKey, o.path, key, o.eventMs);
    EmitNow(o);
}

void FileCapture::Impl::Hold(Output&& o, ULONGLONG due)
{
    if (held.size() >= kMaxHeld)
    {
        HeldMap::iterator first = held.begin();
        ForgetHeldCreate(first);
        Output early = std::move(first->second);
        held.erase(first);
        FinishHeld(std::move(early));
    }
    const bool isCreate = o.act == Act::Create || o.act == Act::Download;
    const std::wstring key = isCreate ? FoldCase(o.path) : std::wstring();
    const HeldMap::iterator it = held.emplace(due, std::move(o));
    if (isCreate)
        heldCreates.Put(key, it);
}

// Must run before the node is moved from or erased (keeps heldCreates free of dangling iterators).
void FileCapture::Impl::ForgetHeldCreate(HeldMap::iterator it)
{
    if (it->second.act != Act::Create && it->second.act != Act::Download)
        return;
    const std::wstring key = FoldCase(it->second.path);
    const HeldMap::iterator* recorded = heldCreates.Peek(key);
    if (recorded != nullptr && *recorded == it)
        heldCreates.Erase(key);
}

void FileCapture::Impl::FinishHeld(Output&& o)
{
    const std::wstring key = FoldCase(o.path);
    switch (o.act)
    {
    case Act::Open:
    {
        const int64_t* lw = lastWrite.Peek(key);
        if (lw != nullptr && *lw >= o.eventMs - kOpenAfterWriteMs)
        {
            statFiltered.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        break;
    }
    case Act::Delete:
    {
        const int64_t* lw = lastWrite.Peek(key);
        if (lw != nullptr && *lw >= o.eventMs - 500)
        {
            statFiltered.fetch_add(1, std::memory_order_relaxed);   // re-created or replaced right after
            return;
        }
        break;
    }
    case Act::Create:
    case Act::Download:
        if (o.zoneCheck && !o.remote && !o.isDir)
        {
            // Mark-of-the-Web: Internet (3) / Restricted (4) zone => a download (never probed on network paths).
            const ZoneIdentifier::Info zi = ZoneIdentifier::Read(o.path);
            if (zi.present && zi.zoneId >= 3)
            {
                bool copied = o.noDownload;
                if (o.act == Act::Create && o.pid != 0)
                    copied = copied || GetProc(o.pid, o.startKey, o.eventMs, true, false)->copyTool;
                if (o.act == Act::Download || !copied)
                {
                    o.act = Act::Download;
                    o.originUrl = OriginUrl(zi);
                }
            }
        }
        break;
    default:
        break;
    }
    EmitNow(o);
}

void FileCapture::Impl::EmitNow(Output& o)
{
    if (IsPausedNow())
    {
        statFiltered.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Settings& settings = Settings::Instance();
    if (settings.IsExcludedPath(o.path) || (!o.oldPath.empty() && settings.IsExcludedPath(o.oldPath)))
    {
        statFiltered.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    ProcEntry* pe = nullptr;
    if (o.pid != 0)
    {
        pe = GetProc(o.pid, o.startKey, o.eventMs, true, false);
        if (!pe->classified)
        {
            // Once per process instance (pid + start key), never on the ETW thread (FILE-10, SIG-6).
            pe->classified = true;
            if (!pe->exePath.empty())
            {
                const WARP::ClassificationResult cls = WARP::SystemProcessClassifier::Instance().Classify(pe->exePath, o.pid);
                pe->isSystem = cls.isSystem;
                if (cls.isSystem && cls.voteCount >= 5 && !InTable(pe->exeName, kNeverHardDrop))
                    pe->hardDrop = true;   // unambiguous system infrastructure
            }
        }
        if (pe->hardDrop)
        {
            statFiltered.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // Same action on the same path from another source (or a repeat) within the window. Observed
    // opens are keyed per process so an Explorer/indexer read cannot mask the app's own open.
    std::wstring dupKey = std::wstring(ActName(o.act)) + (o.isExplicit ? L"|x|" : L"|o|");
    if (o.act == Act::Open && !o.isExplicit)
        dupKey += std::to_wstring(o.pid) + L"|";
    dupKey += FoldCase(o.path);
    if (!o.oldPath.empty())
        dupKey += L"|" + FoldCase(o.oldPath);
    const int64_t* prev = emitKeys.Peek(dupKey);
    if (prev != nullptr && AbsDiff(*prev, o.eventMs) < kDupWindowMs)
    {
        statFiltered.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    emitKeys.Put(dupKey, o.eventMs);

    FileEventRecord rec;
    rec.eventMs = o.eventMs;
    rec.action = ActName(o.act);
    rec.path = o.path;
    rec.oldPath = o.oldPath;
    rec.source = SrcName(o.src);
    rec.isDir = o.isDir;
    rec.originUrl = o.originUrl;
    rec.ctx = EventContextUtil::CaptureContext(o.pid);
    if (pe != nullptr)
        rec.ctx.sourceExe = pe->exePath;   // start-key aware: a recycled PID must not lend its image
    if (o.backfill)
    {
        // Happened while WARP was not running: the current foreground / input state is unrelated.
        rec.ctx.foregroundPid = 0;
        rec.ctx.foregroundExe.clear();
        rec.ctx.msSinceInput = 0xFFFFFFFFu;
    }
    if (o.unconfirmed)
        rec.ctx.confidence *= 0.5;
    EventContextUtil::ScoreHints hints;
    if (pe != nullptr)
    {
        hints.sourceIsSystem = pe->isSystem || pe->sidService;
        hints.sourceIsSyncClient = pe->sync;
        hints.sourceIsShellInfra = pe->shellInfra || (pe->explorer && o.act == Act::Open);   // thumbnails, previews
    }
    rec.ctx.confidence = EventContextUtil::ScoreEvent(rec.ctx, KindOf(o.act), ClassOf(o.src), hints);

    {
        std::lock_guard<std::mutex> lk(cbMtx);
        if (!callback)
        {
            statDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        try
        {
            callback(std::move(rec));
        }
        catch (...)
        {
            if (FirstTime("callback-exception"))
                WARP_LOG_ERROR(kComp, "file event callback threw; event dropped");
            return;
        }
    }
    statEmitted.fetch_add(1, std::memory_order_relaxed);
    int64_t last = statLastEventMs.load();
    while (o.eventMs > last && !statLastEventMs.compare_exchange_weak(last, o.eventMs))
    {
    }
}

const WarpSettings& FileCapture::Impl::Snapshot()
{
    const ULONGLONG now = GetTickCount64();
    if (!snapshotValid || now - snapshotTick >= kSettingsSnapshotMs)
    {
        snapshot = Settings::Instance().Get();
        snapshotTick = now;
        snapshotValid = true;
    }
    return snapshot;
}

// Canonical, privacy-filtered origin of a download: HostUrl, else ReferrerUrl (FILE-11).
std::wstring FileCapture::Impl::OriginUrl(const ZoneIdentifier::Info& zi)
{
    const WarpSettings& s = Snapshot();
    const std::wstring* candidates[] = { &zi.hostUrl, &zi.referrerUrl };
    for (const std::wstring* raw : candidates)
    {
        if (raw->empty())
            continue;
        const UrlCanon::Result r = UrlCanon::Canonicalize(*raw, s.excludedDomains, s.urlQueryAllowlist);
        if (r.excluded)
            return std::wstring();
        if (!r.isInternal && !r.canonical.empty())
            return r.canonical;
    }
    return std::wstring();
}

void FileCapture::Impl::NotePidPath(DWORD pid, uint64_t startKey, const std::wstring& path, const std::wstring& key, int64_t ms)
{
    if (pid == 0)
        return;
    std::lock_guard<std::mutex> lk(ringMtx);
    auto it = pidRings.find(pid);
    if (it == pidRings.end())
    {
        if (pidRings.size() >= kMaxPidRings)
        {
            auto oldest = pidRings.begin();
            for (auto i = pidRings.begin(); i != pidRings.end(); ++i)
            {
                if (i->second.tick < oldest->second.tick)
                    oldest = i;
            }
            pidRings.erase(oldest);
        }
        it = pidRings.emplace(pid, PidRing()).first;
    }
    PidRing& ring = it->second;
    if (startKey != 0 && ring.startKey != 0 && ring.startKey != startKey)
        ring.items.clear();   // the PID now belongs to another process
    if (startKey != 0)
        ring.startKey = startKey;
    ring.tick = GetTickCount64();
    for (auto i = ring.items.begin(); i != ring.items.end(); ++i)
    {
        if (i->key == key)
        {
            ring.items.erase(i);
            break;
        }
    }
    RingItem item;
    item.path = path;
    item.key = key;
    item.ms = ms;
    ring.items.push_front(std::move(item));
    while (ring.items.size() > kPidRingCap)
        ring.items.pop_back();
}

void FileCapture::Impl::NoteExplicit(const std::wstring& path, const std::wstring& key, int64_t ms)
{
    std::lock_guard<std::mutex> lk(ringMtx);
    for (auto i = explicitRing.begin(); i != explicitRing.end(); ++i)
    {
        if (i->key == key)
        {
            explicitRing.erase(i);
            break;
        }
    }
    RingItem item;
    item.path = path;
    item.key = key;
    item.ms = ms;
    explicitRing.push_front(std::move(item));
    while (explicitRing.size() > kExplicitRingCap)
        explicitRing.pop_back();
}

// ==========================================================================================
// ReadDirectoryChangesW: removable and network drives only (FILE-5)
// ==========================================================================================
DWORD CALLBACK FileCapture::Impl::RdcwDeviceCallback(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
                                                     PCM_NOTIFY_EVENT_DATA, DWORD)
{
    RdcwRoot* r = static_cast<RdcwRoot*>(context);
    if (r == nullptr)
        return ERROR_SUCCESS;
    if (action == CM_NOTIFY_ACTION_DEVICEQUERYREMOVE || action == CM_NOTIFY_ACTION_DEVICEREMOVEPENDING ||
        action == CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE)
    {
        // Release the directory handle so WARP never blocks "safely remove hardware".
        r->releaseRequested.store(true);
        SetEvent(r->wakeEvent);
        if (action == CM_NOTIFY_ACTION_DEVICEQUERYREMOVE)
            WaitForSingleObject(r->releasedEvent, 3000);
    }
    return ERROR_SUCCESS;
}

void FileCapture::Impl::RdcwThread(RdcwRoot* r)
{
    try
    {
        HANDLE dir = CreateFileW(r->root.c_str(), FILE_LIST_DIRECTORY,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (dir == INVALID_HANDLE_VALUE)
        {
            const DWORD err = GetLastError();
            if (FirstTime("rdcw-open-" + U8(r->root)))
                WARP_LOG_WARN(kComp, "cannot watch %s: error %lu", U8(r->root).c_str(), err);
        }
        else
        {
            HCMNOTIFICATION notification = nullptr;
            if (!r->network)
            {
                CM_NOTIFY_FILTER filter = {};
                filter.cbSize = sizeof(filter);
                filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
                filter.u.DeviceHandle.hTarget = dir;
                if (CM_Register_Notification(&filter, r, &Impl::RdcwDeviceCallback, &notification) != CR_SUCCESS)
                    notification = nullptr;
            }
            WARP_LOG_INFO(kComp, "watching %s (%s)", U8(r->prefix).c_str(), r->network ? "network" : "removable");
            RdcwLoop(r, dir);
            CloseHandle(dir);
            SetEvent(r->releasedEvent);
            if (notification != nullptr)
                CM_Unregister_Notification(notification);   // never from the callback itself
        }
    }
    catch (...)
    {
    }
    r->exited.store(true);
}

void FileCapture::Impl::RdcwLoop(RdcwRoot* r, HANDLE dir)
{
    std::vector<DWORD> storage(kRdcwBufferBytes / sizeof(DWORD), 0);   // FILE_NOTIFY_INFORMATION is DWORD-aligned
    BYTE* buffer = reinterpret_cast<BYTE*>(storage.data());
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr)
        return;
    const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE;   // files only
    std::wstring pendingOld;
    for (;;)
    {
        ResetEvent(ov.hEvent);
        DWORD unused = 0;
        if (!ReadDirectoryChangesW(dir, buffer, kRdcwBufferBytes, TRUE, filter, &unused, &ov, nullptr))
        {
            const DWORD err = GetLastError();
            if (FirstTime("rdcw-read-" + U8(r->root)))
                WARP_LOG_WARN(kComp, "ReadDirectoryChangesW(%s) failed: %lu", U8(r->root).c_str(), err);
            break;
        }
        HANDLE waits[3] = { ov.hEvent, stopEvent, r->wakeEvent };
        const DWORD w = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0)
        {
            CancelIoEx(dir, &ov);
            DWORD n = 0;
            GetOverlappedResult(dir, &ov, &n, TRUE);   // the kernel owns the buffer until the read completes
            break;
        }
        DWORD bytes = 0;
        if (!GetOverlappedResult(dir, &ov, &bytes, FALSE))
        {
            if (GetLastError() == ERROR_NOTIFY_ENUM_DIR)
            {
                statRdcwOverflows.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            break;   // volume removed or network gone; Heartbeat restarts the watch if the drive returns
        }
        if (bytes == 0)
        {
            statRdcwOverflows.fetch_add(1, std::memory_order_relaxed);   // overflow: these changes are lost
            continue;
        }
        ParseRdcwBuffer(r, buffer, bytes, pendingOld);
    }
    CloseHandle(ov.hEvent);
}

void FileCapture::Impl::ParseRdcwBuffer(RdcwRoot* r, const BYTE* buf, DWORD bytes, std::wstring& pendingOld)
{
    const size_t header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
    size_t offset = 0;
    for (;;)
    {
        if (offset + header > bytes)
            break;
        const FILE_NOTIFY_INFORMATION* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buf + offset);
        const size_t chars = info->FileNameLength / sizeof(wchar_t);
        if (offset + header + chars * sizeof(wchar_t) > bytes)
            break;
        std::wstring rel(chars, L'\0');
        if (chars != 0)
            memcpy(&rel[0], buf + offset + header, chars * sizeof(wchar_t));
        const std::wstring full = r->prefix + L"\\" + rel;

        PullItem item;
        item.kind = PullKind::Rdcw;
        item.utcMs = NowUnixMs();
        bool push = false;
        switch (info->Action)
        {
        case FILE_ACTION_ADDED:
        case FILE_ACTION_MODIFIED:
            if (ProbeDirectory(full) == 1)
                break;   // a folder's timestamp changed
            item.sub = info->Action == FILE_ACTION_ADDED ? kRdcwAdded : kRdcwModified;
            item.path = full;
            push = true;
            break;
        case FILE_ACTION_REMOVED:
            item.sub = kRdcwRemoved;
            item.path = full;
            push = true;
            break;
        case FILE_ACTION_RENAMED_OLD_NAME:
            pendingOld = full;   // the NEW_NAME record follows (possibly in the next buffer)
            break;
        case FILE_ACTION_RENAMED_NEW_NAME:
            item.path = full;
            if (pendingOld.empty())
            {
                item.sub = kRdcwAdded;
            }
            else
            {
                item.sub = kRdcwRenamed;
                item.oldPath = pendingOld;
                pendingOld.clear();
            }
            push = true;
            break;
        default:
            break;
        }
        if (push)
        {
            if (pausedCache.load(std::memory_order_relaxed))
                statFiltered.fetch_add(1, std::memory_order_relaxed);
            else
                PushPull(std::move(item));
        }
        if (info->NextEntryOffset == 0)
            break;
        offset += info->NextEntryOffset;
    }
}

// Caller holds rdcwMtx.
void FileCapture::Impl::StartRdcwRoot(const std::wstring& root)
{
    std::unique_ptr<RdcwRoot> r = std::make_unique<RdcwRoot>();
    r->root = root;
    r->network = GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
    r->prefix = root.substr(0, 2);
    std::wstring unc;
    if (r->network && GetUncForDrive(root[0], unc))
        r->prefix = unc;   // same UNC form as the ETW view (\Device\Mup\...)
    r->wakeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    r->releasedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (r->wakeEvent == nullptr || r->releasedEvent == nullptr)
        return;
    rdcwRoots.reserve(rdcwRoots.size() + 1);   // push_back below must not throw once the thread runs
    RdcwRoot* raw = r.get();
    try
    {
        r->thread = std::thread(&Impl::RdcwThread, this, raw);
    }
    catch (...)
    {
        return;
    }
    rdcwRoots.push_back(std::move(r));
}

// Caller holds lifecycleMtx. Starts watches on new removable/network drives, reaps finished ones.
void FileCapture::Impl::RefreshRdcwRootsLocked()
{
    std::vector<std::wstring> desired;
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
    {
        if ((drives & (1u << i)) == 0)
            continue;
        const std::wstring root{ static_cast<wchar_t>(L'A' + i), L':', L'\\' };
        const UINT type = GetDriveTypeW(root.c_str());
        if (type == DRIVE_REMOVABLE || type == DRIVE_REMOTE)
            desired.push_back(root);
    }
    std::lock_guard<std::mutex> lk(rdcwMtx);
    for (auto it = rdcwRoots.begin(); it != rdcwRoots.end();)
    {
        RdcwRoot& r = **it;
        const bool wanted = std::find(desired.begin(), desired.end(), r.root) != desired.end();
        if (wanted && !r.exited.load())
        {
            ++it;
            continue;
        }
        SetEvent(r.wakeEvent);
        if (r.thread.joinable())
        {
            CancelSynchronousIo(r.thread.native_handle());   // e.g. an attribute probe on a dead share
            r.thread.join();
        }
        it = rdcwRoots.erase(it);
    }
    for (const std::wstring& root : desired)
    {
        bool active = false;
        for (const std::unique_ptr<RdcwRoot>& r : rdcwRoots)
        {
            if (r->root == root)
            {
                active = true;
                break;
            }
        }
        if (!active)
            StartRdcwRoot(root);
    }
}

void FileCapture::Impl::StopAllRdcwLocked()
{
    std::lock_guard<std::mutex> lk(rdcwMtx);
    for (const std::unique_ptr<RdcwRoot>& r : rdcwRoots)
        SetEvent(r->wakeEvent);
    for (const std::unique_ptr<RdcwRoot>& r : rdcwRoots)
    {
        if (r->thread.joinable())
        {
            CancelSynchronousIo(r->thread.native_handle());
            r->thread.join();
        }
    }
    rdcwRoots.clear();
}

// ==========================================================================================
// Lifecycle
// ==========================================================================================
bool FileCapture::Impl::AnySourceRunning()
{
    if ((etwActive.load() && !etwThreadExited.load()) || usnRunning.load() || recentRunning.load() || mruRunning.load())
        return true;
    std::lock_guard<std::mutex> lk(rdcwMtx);
    for (const std::unique_ptr<RdcwRoot>& r : rdcwRoots)
    {
        if (!r->exited.load())
            return true;
    }
    return false;
}

// Caller holds lifecycleMtx. Starts / stops / retries each source according to Settings.
void FileCapture::Impl::ReconcileSources(const WarpSettings& s, bool initial)
{
    const ULONGLONG now = GetTickCount64();
    const bool files = s.captureFiles;

    // ETW: a consumer that died (session stopped externally, ProcessTrace error) is restarted
    // with exponential backoff.
    if (etwActive.load() && etwThreadExited.load())
    {
        WARP_LOG_WARN(kComp, "ETW consumer for %s stopped unexpectedly; restarting with backoff", U8(sessionName).c_str());
        StopEtwLocked();
        if (now - etwStartedTick >= kEtwHealthyMs)
            etwBackoffMs = kEtwBackoffMinMs;
        etwRetryAtTick = now + etwBackoffMs;
        etwBackoffMs = (std::min)(etwBackoffMs * 2, kEtwBackoffMaxMs);
    }
    if (files && !etwActive.load())
    {
        if (initial || now >= etwRetryAtTick)
        {
            if (!StartEtwLocked())
            {
                etwRetryAtTick = now + etwBackoffMs;
                etwBackoffMs = (std::min)(etwBackoffMs * 2, kEtwBackoffMaxMs);
            }
        }
    }
    else if (!files && etwActive.load())
    {
        StopEtwLocked();
    }

    // USN journal: restarted when a new journaled volume appears (cursors make this lossless).
    const bool wantUsn = files && s.captureUsnJournal;
    if (wantUsn && !usnRunning.load())
    {
        if (initial || now >= usnRetryAtTick)
        {
            if (!StartUsnLocked())
                usnRetryAtTick = now + kSourceRetryMs;
        }
    }
    else if (wantUsn && now - usnVolumeCheckTick >= kUsnVolumeCheckMs)
    {
        usnVolumeCheckTick = now;
        const DWORD mask = ProbeUsnDrives();
        const DWORD current = usnDriveMask.load();
        if ((mask & ~current) != 0)
        {
            WARP_LOG_INFO(kComp, "new journaled volume (drive mask 0x%lx); restarting the USN reader", static_cast<unsigned long>(mask));
            StopUsnLocked();
            if (!StartUsnLocked())
                usnRetryAtTick = now + kSourceRetryMs;
        }
        else if (mask != current)
        {
            usnDriveMask.store(current & mask);   // a journaled volume went away
        }
    }
    else if (!wantUsn && usnRunning.load())
    {
        StopUsnLocked();
    }

    const bool wantRecent = files && s.captureRecentItems;
    if (wantRecent && !recentRunning.load())
    {
        if ((initial || now >= recentRetryAtTick) && !StartRecentLocked())
            recentRetryAtTick = now + kSourceRetryMs;
    }
    else if (!wantRecent && recentRunning.load())
    {
        StopRecentLocked();
    }

    const bool wantMru = files && s.captureOfficeMru;
    if (wantMru && !mruRunning.load())
    {
        if ((initial || now >= mruRetryAtTick) && !StartMruLocked())
            mruRetryAtTick = now + kSourceRetryMs;
    }
    else if (!wantMru && mruRunning.load())
    {
        StopMruLocked();
    }

    if (files)
        RefreshRdcwRootsLocked();
    else
        StopAllRdcwLocked();
}

bool FileCapture::Impl::Start()
{
    std::lock_guard<std::mutex> lk(lifecycleMtx);
    if (running.load())
        return true;
    if (stopEvent == nullptr)
    {
        SetError(L"cannot create the stop event");
        return false;
    }
    if (worker.joinable())
        worker.join();   // a previous Stop() requested from inside the callback
    ResetEvent(stopEvent);
    producersStopping.store(false);
    {
        std::lock_guard<std::mutex> q(qMtx);
        workerStop = false;
        etwQ.clear();
        pullQ.clear();
    }
    ownPid = GetCurrentProcessId();
    ownSession = EventContextUtil::OwnSessionId();
    sessionName = L"WARP-KernelFile-" + std::to_wstring(ownSession);
    {
        std::lock_guard<std::mutex> s(statusMtx);
        sessionNameStat = sessionName;
        lastError.clear();
    }
    etwBackoffMs = kEtwBackoffMinMs;
    etwRetryAtTick = 0;
    usnRetryAtTick = 0;
    recentRetryAtTick = 0;
    mruRetryAtTick = 0;
    (void)Folders();   // resolve known folders before events arrive
    RefreshDeviceMap(true);
    UpdatePausedCache(true);
    try
    {
        worker = std::thread(&Impl::WorkerMain, this);
    }
    catch (...)
    {
        SetError(L"cannot start the worker thread");
        return false;
    }
    running.store(true);

    ReconcileSources(Settings::Instance().Get(), true);
    if (!AnySourceRunning())
    {
        WARP_LOG_ERROR(kComp, "no file-activity source could be started");
        StopLocked();
        std::lock_guard<std::mutex> s(statusMtx);
        if (lastError.empty())
            lastError = L"no file-activity source could be started";
        return false;
    }
    WARP_LOG_INFO(kComp, "file capture started: etw=%d usn=%d recent=%d mru=%d",
                  etwActive.load() ? 1 : 0, usnRunning.load() ? 1 : 0, recentRunning.load() ? 1 : 0, mruRunning.load() ? 1 : 0);
    return true;
}

// Caller holds lifecycleMtx. Producers first (their callbacks return promptly once
// producersStopping is set), then the worker, which drains the queue and flushes held events.
void FileCapture::Impl::StopLocked()
{
    producersStopping.store(true);
    if (stopEvent != nullptr)
        SetEvent(stopEvent);
    spaceCv.notify_all();
    if (etwActive.load() || etwThread.joinable())
        StopEtwLocked();
    StopUsnLocked();
    StopRecentLocked();
    StopMruLocked();
    StopAllRdcwLocked();
    {
        std::lock_guard<std::mutex> q(qMtx);
        workerStop = true;
    }
    workCv.notify_all();
    if (worker.joinable())
        worker.join();
    SaveWatermarks(true);
    running.store(false);
    WARP_LOG_INFO(kComp, "file capture stopped (emitted %llu, filtered %llu, dropped %llu)",
                  static_cast<unsigned long long>(statEmitted.load()), static_cast<unsigned long long>(statFiltered.load()),
                  static_cast<unsigned long long>(statDropped.load()));
}

void FileCapture::Impl::Stop()
{
    if (workerThreadId.load() == GetCurrentThreadId())
    {
        // From inside the callback: the worker cannot join itself; it exits after this batch and
        // the next Stop() from another thread (or the destructor) completes the shutdown.
        producersStopping.store(true);
        {
            std::lock_guard<std::mutex> q(qMtx);
            workerStop = true;
        }
        workCv.notify_all();
        return;
    }
    std::lock_guard<std::mutex> lk(lifecycleMtx);
    if (!running.load())
        return;
    StopLocked();
}

void FileCapture::Impl::Heartbeat()
{
    std::lock_guard<std::mutex> lk(lifecycleMtx);
    if (!running.load())
        return;
    try
    {
        UpdatePausedCache(true);
        RefreshDeviceMap(true);
        ReconcileSources(Settings::Instance().Get(), false);
        QueryEtwLoss();
        SaveWatermarks(false);
        const ULONGLONG now = GetTickCount64();
        if (oneDriveRefreshTick == 0 || now - oneDriveRefreshTick >= kOneDriveRefreshMs)
        {
            oneDriveRefreshTick = now;
            OneDriveMapper::Instance().Refresh();   // new sync roots for MRU URL -> local path
        }
    }
    catch (...)
    {
        WARP_LOG_ERROR(kComp, "Heartbeat failed with an exception");
    }
}

// ==========================================================================================
// Public API
// ==========================================================================================
FileCapture::FileCapture()
    : m_impl(std::make_unique<Impl>())
{
}

FileCapture::~FileCapture() = default;   // Impl::~Impl stops and joins everything

void FileCapture::SetCallback(Callback cb)
{
    std::lock_guard<std::mutex> lk(m_impl->cbMtx);
    m_impl->callback = std::move(cb);
}

void FileCapture::SetCursorStore(std::function<bool(const std::wstring& volume, std::string& blob)> load,
                                 std::function<void(const std::wstring& volume, const std::string& blob)> save)
{
    std::lock_guard<std::mutex> lk(m_impl->cursorMtx);
    m_impl->cursorLoad = std::move(load);
    m_impl->cursorSave = std::move(save);
}

bool FileCapture::Start()
{
    return m_impl->Start();
}

void FileCapture::Stop()
{
    m_impl->Stop();
}

bool FileCapture::IsRunning() const
{
    return m_impl->running.load();
}

void FileCapture::SetPaused(bool paused)
{
    m_impl->userPaused.store(paused);
    m_impl->UpdatePausedCache(true);
    WARP_LOG_INFO(kComp, "%s", paused ? "file capture paused (privacy): events are read and discarded" : "file capture resumed");
}

void FileCapture::Heartbeat()
{
    m_impl->Heartbeat();
}

FileCapture::Stats FileCapture::GetStats() const
{
    Impl& i = *m_impl;
    Stats s;
    s.etwEvents = i.statEtw.load();
    s.usnEvents = i.statUsn.load();
    s.rdcwEvents = i.statRdcw.load();
    s.recentEvents = i.statRecent.load();
    s.mruEvents = i.statMru.load();
    s.emitted = i.statEmitted.load();
    s.filtered = i.statFiltered.load();
    s.queueDropped = i.statDropped.load();
    s.etwEventsLost = i.statEventsLost.load();
    s.etwBuffersLost = i.statBuffersLost.load();
    s.lastEventMs = i.statLastEventMs.load();
    s.etwRunning = i.etwActive.load() && !i.etwThreadExited.load();
    s.usnRunning = i.usnRunning.load();
    s.recentRunning = i.recentRunning.load();
    s.mruRunning = i.mruRunning.load();
    {
        std::lock_guard<std::mutex> lk(i.statusMtx);
        s.etwSessionName = i.sessionNameStat;
        s.lastError = i.lastError;
    }
    s.rdcwOverflows = i.statRdcwOverflows.load();
    s.etwOtherSession = i.statOtherSession.load();
    s.etwUnresolved = i.statUnresolved.load();
    s.etwRealTimeLost = i.statRtLost.load();
    s.etwUnconfirmed = i.etwUnconfirmed.load();
    {
        std::lock_guard<std::mutex> lk(i.qMtx);
        s.queueDepth = i.etwQ.size() + i.pullQ.size();
    }
    {
        std::lock_guard<std::mutex> lk(i.rdcwMtx);
        s.rdcwRoots = i.rdcwRoots.size();
    }
    s.usnStatus = i.usn.Status();
    return s;
}

std::vector<std::wstring> FileCapture::RecentPathsForPid(DWORD pid, DWORD maxAgeMs, size_t max) const
{
    std::vector<std::wstring> out;
    if (pid == 0 || max == 0)
        return out;
    std::vector<RingItem> items;
    {
        std::lock_guard<std::mutex> lk(m_impl->ringMtx);
        const auto it = m_impl->pidRings.find(pid);
        if (it == m_impl->pidRings.end())
            return out;
        items.assign(it->second.items.begin(), it->second.items.end());
    }
    std::stable_sort(items.begin(), items.end(), [](const RingItem& a, const RingItem& b) { return a.ms > b.ms; });
    const int64_t now = NowUnixMs();
    for (const RingItem& item : items)
    {
        if (now - item.ms > static_cast<int64_t>(maxAgeMs))
            continue;
        out.push_back(item.path);
        if (out.size() >= max)
            break;
    }
    return out;
}

std::vector<std::wstring> FileCapture::RecentExplicitOpens(DWORD maxAgeMs, size_t max) const
{
    std::vector<std::wstring> out;
    if (max == 0)
        return out;
    std::vector<RingItem> items;
    {
        std::lock_guard<std::mutex> lk(m_impl->ringMtx);
        items.assign(m_impl->explicitRing.begin(), m_impl->explicitRing.end());
    }
    std::stable_sort(items.begin(), items.end(), [](const RingItem& a, const RingItem& b) { return a.ms > b.ms; });
    const int64_t now = NowUnixMs();
    for (const RingItem& item : items)
    {
        if (now - item.ms > static_cast<int64_t>(maxAgeMs))
            continue;
        out.push_back(item.path);
        if (out.size() >= max)
            break;
    }
    return out;
}
