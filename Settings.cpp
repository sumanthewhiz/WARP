#include "framework.h"
#include "Settings.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <unordered_set>

#pragma comment(lib, "advapi32.lib")

// Every registry value is described once in kValues (name, type, field, range);
// Load(), the setters and Describe() are all driven by that table.

namespace
{
    const wchar_t kUserKey[]   = L"Software\\WARP";
    const wchar_t kPolicyKey[] = L"Software\\Policies\\WARP";

    // Larger list values are ignored (the registry itself caps values at about 1 MB).
    const DWORD kMaxListBytes = 2u * 1024u * 1024u;

    // Always excluded in addition to configured ExcludedApps: these surface credentials.
    const wchar_t* const kBaseExcludedApps[] =
    {
        L"keepass.exe", L"keepassxc.exe", L"1password.exe", L"bitwarden.exe", L"dashlane.exe",
        L"enpass.exe", L"roboform.exe", L"lastpass.exe", L"nordpass.exe", L"keeper.exe",
        L"credentialuibroker.exe", L"consent.exe", L"logonui.exe", L"lockapp.exe",
    };

    // Default ExcludedTitlePatterns (they double as private-window detection fallbacks).
    const wchar_t* const kDefaultTitlePatterns[] =
    {
        L"InPrivate", L"Incognito", L"Private Browsing", L"- Private",
    };

    // ---- value table --------------------------------------------------------

    enum class Kind
    {
        Bool,
        Int,
        Int64,
        List
    };

    // How list entries are normalized on load.
    enum class ListKind
    {
        NotList,
        Text,           // trimmed, case preserved
        ExeNames,       // lowercase file-name part; ".exe" appended when there is no extension
        Hosts,          // lowercase host; scheme, userinfo, port, path and leading "*." removed
        PathPrefixes    // %VAR% expanded, '/' made '\', long-path prefix and trailing separators removed
    };

    struct ValueDef
    {
        const wchar_t*                            name;
        Kind                                      kind;
        bool WarpSettings::*                      pBool;
        int WarpSettings::*                       pInt;
        int64_t WarpSettings::*                   pInt64;
        std::vector<std::wstring> WarpSettings::* pList;
        int                                       minValue;
        int                                       maxValue;
        ListKind                                  listKind;
    };

    constexpr ValueDef BoolValue(const wchar_t* name, bool WarpSettings::* member)
    {
        return { name, Kind::Bool, member, nullptr, nullptr, nullptr, 0, 1, ListKind::NotList };
    }

    constexpr ValueDef IntValue(const wchar_t* name, int WarpSettings::* member, int minValue, int maxValue)
    {
        return { name, Kind::Int, nullptr, member, nullptr, nullptr, minValue, maxValue, ListKind::NotList };
    }

    constexpr ValueDef Int64Value(const wchar_t* name, int64_t WarpSettings::* member)
    {
        return { name, Kind::Int64, nullptr, nullptr, member, nullptr, 0, 0, ListKind::NotList };
    }

    constexpr ValueDef ListValue(const wchar_t* name, std::vector<std::wstring> WarpSettings::* member,
                                 ListKind listKind)
    {
        return { name, Kind::List, nullptr, nullptr, nullptr, member, 0, 0, listKind };
    }

    // Registry value names, types and clamping ranges. Order is the Describe() order.
    const ValueDef kValues[] =
    {
        BoolValue(L"CaptureFiles",                &WarpSettings::captureFiles),
        BoolValue(L"CaptureAppLaunch",            &WarpSettings::captureAppLaunch),
        BoolValue(L"CaptureFocus",                &WarpSettings::captureFocus),
        BoolValue(L"CaptureWeb",                  &WarpSettings::captureWeb),
        BoolValue(L"CapturePresence",             &WarpSettings::capturePresence),
        BoolValue(L"CaptureClipboardMetadata",    &WarpSettings::captureClipboardMetadata),
        BoolValue(L"CapturePrint",                &WarpSettings::capturePrint),
        BoolValue(L"CaptureUsnJournal",           &WarpSettings::captureUsnJournal),
        BoolValue(L"CaptureRecentItems",          &WarpSettings::captureRecentItems),
        BoolValue(L"CaptureOfficeMru",            &WarpSettings::captureOfficeMru),
        BoolValue(L"DropPrivateBrowsing",         &WarpSettings::dropPrivateBrowsing),
        BoolValue(L"EncryptDataDir",              &WarpSettings::encryptDataDir),
        ListValue(L"ExcludedApps",                &WarpSettings::excludedApps,          ListKind::ExeNames),
        ListValue(L"ExcludedTitlePatterns",       &WarpSettings::excludedTitlePatterns, ListKind::Text),
        ListValue(L"ExcludedDomains",             &WarpSettings::excludedDomains,       ListKind::Hosts),
        ListValue(L"ExcludedPathPrefixes",        &WarpSettings::excludedPathPrefixes,  ListKind::PathPrefixes),
        ListValue(L"UrlQueryAllowlist",           &WarpSettings::urlQueryAllowlist,     ListKind::Text),
        IntValue(L"RawRetentionDays",             &WarpSettings::rawRetentionDays,      1, 3650),
        IntValue(L"EntityRetentionDays",          &WarpSettings::entityRetentionDays,   7, 3650),
        IntValue(L"AwayAfterSeconds",             &WarpSettings::awayAfterSeconds,      30, 86400),
        BoolValue(L"LlmEnabled",                  &WarpSettings::llmEnabled),
        BoolValue(L"LlmAllowedOnBattery",         &WarpSettings::llmAllowedOnBattery),
        IntValue(L"ContextCadenceSeconds",        &WarpSettings::contextCadenceSeconds, 15, 3600),
        BoolValue(L"ApiAllowAppContainerClients", &WarpSettings::apiAllowAppContainerClients),
        ListValue(L"ApiAllowedClients",           &WarpSettings::apiAllowedClients,     ListKind::ExeNames),
        ListValue(L"ApiActivityClients",          &WarpSettings::apiActivityClients,    ListKind::ExeNames),
        BoolValue(L"ApiRestrictActivityScope",    &WarpSettings::apiRestrictActivityScope),
        BoolValue(L"ConsentAcknowledged",         &WarpSettings::consentAcknowledged),
        Int64Value(L"PausedUntilUtc",             &WarpSettings::pausedUntilUtc),
        IntValue(L"LogLevel",                     &WarpSettings::logLevel,              0, 3),
    };

    // ---- locking / handles --------------------------------------------------

    // Serializes Load() so a slower, older registry read can never replace a newer snapshot.
    // Constant-initialized, so it is valid even before dynamic initialization runs.
    SRWLOCK g_loadLock = SRWLOCK_INIT;

    class ExclusiveSrwGuard
    {
    public:
        explicit ExclusiveSrwGuard(SRWLOCK& lock) : m_lock(lock)
        {
            AcquireSRWLockExclusive(&m_lock);
        }

        ~ExclusiveSrwGuard()
        {
            ReleaseSRWLockExclusive(&m_lock);
        }

        ExclusiveSrwGuard(const ExclusiveSrwGuard&) = delete;
        ExclusiveSrwGuard& operator=(const ExclusiveSrwGuard&) = delete;

    private:
        SRWLOCK& m_lock;
    };

    class RegKey
    {
    public:
        RegKey() = default;

        ~RegKey()
        {
            if (m_key != nullptr) RegCloseKey(m_key);
        }

        RegKey(const RegKey&) = delete;
        RegKey& operator=(const RegKey&) = delete;

        bool Open(HKEY root, const wchar_t* subKey, REGSAM access)
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, subKey, 0, access, &key) != ERROR_SUCCESS) return false;
            m_key = key;
            return true;
        }

        HKEY Get() const
        {
            return m_key;
        }

    private:
        HKEY m_key = nullptr;
    };

    // ---- strings ------------------------------------------------------------

    bool ToInt(size_t value, int& out)
    {
        if (value > static_cast<size_t>(INT_MAX)) return false;
        out = static_cast<int>(value);
        return true;
    }

    // Ordinal case-insensitive equality (OS uppercase table, as used by the file system).
    bool EqualNoCase(const wchar_t* a, size_t aLen, const wchar_t* b, size_t bLen)
    {
        if (aLen != bLen) return false;   // ordinal case mapping is 1:1 per UTF-16 unit
        if (aLen == 0) return true;
        int len = 0;
        if (!ToInt(aLen, len)) return false;
        return CompareStringOrdinal(a, len, b, len, TRUE) == CSTR_EQUAL;
    }

    bool EqualNoCase(const std::wstring& a, const wchar_t* b)
    {
        return EqualNoCase(a.c_str(), a.size(), b, std::wcslen(b));
    }

    bool IsBlank(wchar_t c)
    {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\v' || c == L'\f' ||
               c == 0x00A0 || c == 0x3000 || c == 0xFEFF;
    }

    bool IsSep(wchar_t c)
    {
        return c == L'\\' || c == L'/';
    }

    // Narrows [p, p + n) by dropping blanks (and double quotes if requested) at both ends.
    void TrimView(const wchar_t*& p, size_t& n, bool quotes)
    {
        while (n > 0 && (IsBlank(p[0]) || (quotes && p[0] == L'"')))
        {
            ++p;
            --n;
        }
        while (n > 0 && (IsBlank(p[n - 1]) || (quotes && p[n - 1] == L'"')))
        {
            --n;
        }
    }

    std::wstring Trimmed(const std::wstring& s, bool quotes)
    {
        const wchar_t* p = s.c_str();
        size_t n = s.size();
        TrimView(p, n, quotes);
        return std::wstring(p, n);
    }

    // Culture-invariant LCMAP_LOWERCASE / LCMAP_UPPERCASE; returns the input unchanged on failure.
    std::wstring MapCase(const std::wstring& s, DWORD flag)
    {
        int len = 0;
        if (s.empty() || !ToInt(s.size(), len)) return s;
        std::wstring out(s.size(), L'\0');
        const int written = LCMapStringEx(LOCALE_NAME_INVARIANT, flag, s.c_str(), len, &out[0], len,
                                          nullptr, nullptr, 0);
        if (written <= 0) return s;
        out.resize(static_cast<size_t>(written));
        return out;
    }

    // ---- paths --------------------------------------------------------------

    // Expands %VAR% references; returns the input unchanged on failure.
    std::wstring ExpandEnvironment(const std::wstring& s)
    {
        DWORD needed = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
        for (int attempt = 0; attempt < 3 && needed > 0; ++attempt)
        {
            std::wstring out(needed, L'\0');
            const DWORD written = ExpandEnvironmentStringsW(s.c_str(), &out[0], needed);
            if (written == 0) break;
            if (written <= needed)
            {
                out.resize(written - 1);   // drop the terminating NUL
                return out;
            }
            needed = written;              // the environment changed between the two calls
        }
        return s;
    }

    // Folds Win32 long-path / NT prefixes and reduces UNC paths to one leading separator,
    // so "C:\x" matches "\\?\C:\x" and "\\srv\share" matches "\\?\UNC\srv\share".
    void CanonicalPathView(const wchar_t*& p, size_t& n)
    {
        if (n >= 4 && IsSep(p[0]) && (IsSep(p[1]) || p[1] == L'?') && p[2] == L'?' && IsSep(p[3]))
        {
            p += 4;
            n -= 4;
            if (n >= 4 && EqualNoCase(p, 3, L"UNC", 3) && IsSep(p[3]))
            {
                p += 3;
                n -= 3;
            }
        }
        else if (n >= 2 && IsSep(p[0]) && IsSep(p[1]))
        {
            p += 1;
            n -= 1;
        }
    }

    // True if 'path' starts with 'prefix' and the match ends at a separator or at the end of 'path'.
    // '/' and '\' are interchangeable; the segments between them compare ordinal case-insensitively.
    bool PathHasPrefix(const wchar_t* path, size_t pathLen, const wchar_t* prefix, size_t prefixLen)
    {
        if (prefixLen == 0 || pathLen < prefixLen) return false;
        if (pathLen > prefixLen && !IsSep(path[prefixLen])) return false;

        size_t i = 0;
        while (i < prefixLen)
        {
            if (IsSep(prefix[i]))
            {
                if (!IsSep(path[i])) return false;
                ++i;
                continue;
            }
            size_t end = i;
            while (end < prefixLen && !IsSep(prefix[end])) ++end;
            if (!EqualNoCase(path + i, end - i, prefix + i, end - i)) return false;
            i = end;
        }
        return true;
    }

    std::wstring NormalizePathPrefix(const std::wstring& raw)
    {
        std::wstring s = Trimmed(raw, true);
        if (s.find(L'%') != std::wstring::npos) s = ExpandEnvironment(s);
        std::replace(s.begin(), s.end(), L'/', L'\\');

        const bool longPath = s.size() >= 4 &&
                              (s.compare(0, 4, L"\\\\?\\") == 0 || s.compare(0, 4, L"\\??\\") == 0);
        if (longPath && s.size() >= 8 && EqualNoCase(s.c_str() + 4, 4, L"UNC\\", 4))
        {
            s = L"\\\\" + s.substr(8);   // \\?\UNC\srv\share => \\srv\share
        }
        else if (longPath)
        {
            s.erase(0, 4);               // \\?\C:\dir => C:\dir
        }
        while (!s.empty() && s.back() == L'\\') s.pop_back();
        return s;
    }

    // ---- hosts --------------------------------------------------------------

    // Narrows a host, "host:port" or URL to the bare host (no scheme, userinfo, port, path or trailing dot).
    void HostView(const wchar_t*& p, size_t& n)
    {
        TrimView(p, n, true);
        for (size_t i = 0; i + 2 < n; ++i)
        {
            if (p[i] == L'/' || p[i] == L'?' || p[i] == L'#' || p[i] == L'@') break;
            if (p[i] == L':' && p[i + 1] == L'/' && p[i + 2] == L'/')
            {
                p += i + 3;
                n -= i + 3;
                break;
            }
        }
        for (size_t i = 0; i < n; ++i)
        {
            if (p[i] == L'/' || p[i] == L'\\' || p[i] == L'?' || p[i] == L'#')
            {
                n = i;
                break;
            }
        }
        for (size_t i = n; i > 0; --i)
        {
            if (p[i - 1] == L'@')
            {
                p += i;
                n -= i;
                break;
            }
        }
        if (n > 0 && p[0] == L'[')
        {
            const wchar_t* bracket = std::find(p, p + n, L']');   // IPv6 literal: keep the brackets
            if (bracket != p + n) n = static_cast<size_t>(bracket - p) + 1;
        }
        else
        {
            n = static_cast<size_t>(std::find(p, p + n, L':') - p);
        }
        while (n > 0 && p[n - 1] == L'.') --n;
    }

    std::wstring NormalizeHost(const std::wstring& raw)
    {
        const wchar_t* p = raw.c_str();
        size_t n = raw.size();
        HostView(p, n);
        while (n > 0 && (p[0] == L'*' || p[0] == L'.'))   // "*.example.com": subdomains match anyway
        {
            ++p;
            --n;
        }
        return MapCase(std::wstring(p, n), LCMAP_LOWERCASE);
    }

    // ---- exe names ----------------------------------------------------------

    // Narrows an exe path or name to its trimmed file-name part.
    void FileNameView(const wchar_t*& p, size_t& n)
    {
        TrimView(p, n, true);
        for (size_t i = n; i > 0; --i)
        {
            if (IsSep(p[i - 1]))
            {
                p += i;
                n -= i;
                break;
            }
        }
        TrimView(p, n, true);
    }

    bool HasExtension(const wchar_t* p, size_t n)
    {
        return std::find(p, p + n, L'.') != p + n;
    }

    // "C:\Apps\KeePass.EXE" => "keepass.exe"; "KeePass" => "keepass.exe".
    std::wstring NormalizeExeName(const std::wstring& raw)
    {
        const wchar_t* p = raw.c_str();
        size_t n = raw.size();
        FileNameView(p, n);
        if (n == 0) return std::wstring();
        std::wstring name(p, n);
        if (!HasExtension(p, n)) name += L".exe";
        return MapCase(name, LCMAP_LOWERCASE);
    }

    // ---- lists --------------------------------------------------------------

    std::wstring NormalizeEntry(ListKind kind, const std::wstring& raw)
    {
        switch (kind)
        {
        case ListKind::ExeNames:     return NormalizeExeName(raw);
        case ListKind::Hosts:        return NormalizeHost(raw);
        case ListKind::PathPrefixes: return NormalizePathPrefix(raw);
        default:                     return Trimmed(raw, false);
        }
    }

    // Normalizes entries and drops empty ones and case-insensitive duplicates (first occurrence wins).
    std::vector<std::wstring> NormalizeList(ListKind kind, const std::vector<std::wstring>& raw)
    {
        std::vector<std::wstring> out;
        std::unordered_set<std::wstring> seen;
        out.reserve(raw.size());
        for (const std::wstring& item : raw)
        {
            std::wstring entry = NormalizeEntry(kind, item);
            if (!entry.empty() && seen.insert(MapCase(entry, LCMAP_UPPERCASE)).second)
            {
                out.push_back(std::move(entry));
            }
        }
        return out;
    }

    std::wstring Join(const std::vector<std::wstring>& items, wchar_t separator)
    {
        std::wstring out;
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i > 0) out.push_back(separator);
            out += items[i];
        }
        return out;
    }

    // ---- registry reads -----------------------------------------------------

    // Lenient REG_SZ number: decimal integer or "true"/"false", optionally padded with blanks/quotes.
    bool ParseNumber(const wchar_t* s, int64_t& out)
    {
        const wchar_t* p = s;
        size_t n = std::wcslen(s);
        TrimView(p, n, true);
        if (n == 0) return false;
        if (EqualNoCase(p, n, L"true", 4))
        {
            out = 1;
            return true;
        }
        if (EqualNoCase(p, n, L"false", 5))
        {
            out = 0;
            return true;
        }
        wchar_t* end = nullptr;
        const long long value = std::wcstoll(p, &end, 10);   // saturates on overflow; callers clamp
        if (end == nullptr || static_cast<size_t>(end - p) != n) return false;
        out = value;
        return true;
    }

    // Number stored as REG_DWORD, REG_QWORD or (leniently) REG_SZ. Missing or malformed => false.
    bool ReadNumber(HKEY key, const wchar_t* name, int64_t& out)
    {
        wchar_t buf[64] = {};
        DWORD type = REG_NONE;
        DWORD cb = static_cast<DWORD>(sizeof(buf));
        if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD | RRF_RT_REG_QWORD | RRF_RT_REG_SZ,
                         &type, buf, &cb) != ERROR_SUCCESS)
        {
            return false;
        }

        switch (type)
        {
        case REG_DWORD:
        {
            if (cb != sizeof(DWORD)) return false;
            DWORD value = 0;
            std::memcpy(&value, buf, sizeof(value));
            out = static_cast<int64_t>(value);
            return true;
        }
        case REG_QWORD:
        {
            if (cb != sizeof(ULONGLONG)) return false;
            ULONGLONG value = 0;
            std::memcpy(&value, buf, sizeof(value));
            out = value > static_cast<ULONGLONG>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(value);
            return true;
        }
        case REG_SZ:
            buf[ARRAYSIZE(buf) - 1] = L'\0';
            return ParseNumber(buf, out);
        default:
            return false;
        }
    }

    // REG_MULTI_SZ: NUL-separated strings; an empty string terminates the list.
    void SplitMultiSz(const wchar_t* p, size_t n, std::vector<std::wstring>& out)
    {
        size_t i = 0;
        while (i < n)
        {
            size_t end = i;
            while (end < n && p[end] != L'\0') ++end;
            if (end == i) break;
            out.emplace_back(p + i, end - i);
            i = end + 1;
        }
    }

    // Lenient REG_SZ list: entries separated by 'separator'.
    void SplitDelimited(const wchar_t* p, size_t n, wchar_t separator, std::vector<std::wstring>& out)
    {
        n = static_cast<size_t>(std::find(p, p + n, L'\0') - p);
        size_t start = 0;
        for (size_t i = 0; i <= n; ++i)
        {
            if (i == n || p[i] == separator)
            {
                if (i > start) out.emplace_back(p + start, i - start);
                start = i + 1;
            }
        }
    }

    // List stored as REG_MULTI_SZ or (leniently) a ';'-separated REG_SZ. Missing or malformed => false.
    bool ReadList(HKEY key, const wchar_t* name, std::vector<std::wstring>& out)
    {
        std::vector<wchar_t> buf(256);
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            DWORD type = REG_NONE;
            DWORD cb = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
            const LSTATUS status = RegGetValueW(key, nullptr, name, RRF_RT_REG_MULTI_SZ | RRF_RT_REG_SZ,
                                                &type, buf.data(), &cb);
            if (status == ERROR_MORE_DATA)
            {
                const size_t needed = cb / sizeof(wchar_t) + 2;
                if (cb > kMaxListBytes || needed <= buf.size()) return false;
                buf.assign(needed, L'\0');
                continue;   // retry: the value may also change in between
            }
            if (status != ERROR_SUCCESS) return false;

            size_t count = cb / sizeof(wchar_t);
            if (count > buf.size()) count = buf.size();
            out.clear();
            if (type == REG_MULTI_SZ)
            {
                SplitMultiSz(buf.data(), count, out);
            }
            else
            {
                SplitDelimited(buf.data(), count, L';', out);
            }
            return true;
        }
        return false;
    }

    int ClampToInt(int64_t value, int minValue, int maxValue)
    {
        if (value < minValue) return minValue;
        if (value > maxValue) return maxValue;
        return static_cast<int>(value);
    }

    // Applies one registry value to 's'. Returns true if a valid value was present.
    bool ReadValue(HKEY key, const ValueDef& def, WarpSettings& s)
    {
        if (def.kind == Kind::List)
        {
            std::vector<std::wstring> raw;
            if (!ReadList(key, def.name, raw)) return false;
            (s.*def.pList) = NormalizeList(def.listKind, raw);
            return true;
        }

        int64_t value = 0;
        if (!ReadNumber(key, def.name, value)) return false;
        switch (def.kind)
        {
        case Kind::Bool:
            (s.*def.pBool) = (value != 0);
            break;
        case Kind::Int:
            (s.*def.pInt) = ClampToInt(value, def.minValue, def.maxValue);
            break;
        case Kind::Int64:
            (s.*def.pInt64) = (value < 0) ? 0 : value;   // PausedUntilUtc: negative => not paused
            break;
        case Kind::List:
            break;
        }
        return true;
    }

    // Overlays every valid value found under the key onto 's'; names applied are appended to 'applied'.
    void ApplyLayer(HKEY root, const wchar_t* subKey, REGSAM view, WarpSettings& s,
                    std::vector<std::wstring>* applied)
    {
        RegKey key;
        if (!key.Open(root, subKey, KEY_QUERY_VALUE | view)) return;   // absent key => nothing to overlay
        for (const ValueDef& def : kValues)
        {
            if (ReadValue(key.Get(), def, s) && applied != nullptr) applied->emplace_back(def.name);
        }
    }

    // ---- misc ---------------------------------------------------------------

    WarpSettings MakeDefaults()
    {
        WarpSettings s;
        s.excludedTitlePatterns.assign(std::begin(kDefaultTitlePatterns), std::end(kDefaultTitlePatterns));
        return s;
    }

    bool IsBaseExcludedApp(const std::wstring& name)
    {
        for (const wchar_t* app : kBaseExcludedApps)
        {
            if (EqualNoCase(name, app)) return true;
        }
        return false;
    }

    bool ContainsName(const std::vector<std::wstring>& names, const wchar_t* name)
    {
        for (const std::wstring& candidate : names)
        {
            if (EqualNoCase(candidate, name)) return true;
        }
        return false;
    }

    // Registry value names are case-insensitive; returns the table entry of the requested kind.
    const ValueDef* FindValue(const wchar_t* name, Kind kind)
    {
        if (name == nullptr) return nullptr;
        const size_t len = std::wcslen(name);
        for (const ValueDef& def : kValues)
        {
            if (def.kind == kind && EqualNoCase(def.name, std::wcslen(def.name), name, len)) return &def;
        }
        return nullptr;
    }

    // Writes one HKCU value, then reloads the snapshot. Refused when policy owns the value.
    bool CommitUserValue(Settings& settings, const ValueDef& def, DWORD type, const void* data, DWORD cb)
    {
        if (settings.IsLocked(def.name)) return false;
        LSTATUS status = RegSetKeyValueW(HKEY_CURRENT_USER, kUserKey, def.name, type, data, cb);
        if (status == ERROR_FILE_NOT_FOUND)
        {
            // Defensive: make sure the key exists (first write on this profile), then retry once.
            HKEY key = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, kUserKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                nullptr, &key, nullptr) == ERROR_SUCCESS)
            {
                RegCloseKey(key);
                status = RegSetKeyValueW(HKEY_CURRENT_USER, kUserKey, def.name, type, data, cb);
            }
        }
        if (status != ERROR_SUCCESS) return false;
        settings.Load();
        return !settings.IsLocked(def.name);   // policy may have been applied in the meantime
    }
}   // namespace

Settings& Settings::Instance()
{
    static Settings instance;
    return instance;
}

Settings::Settings()
{
    Load();   // never expose the bare struct defaults: they lack the built-in exclusions
}

void Settings::Load()
{
    ExclusiveSrwGuard serialize(g_loadLock);

    WarpSettings s = MakeDefaults();
    std::vector<std::wstring> locked;
    ApplyLayer(HKEY_CURRENT_USER, kUserKey, 0, s, nullptr);
    // Policy is read from the 64-bit view so 32-bit builds see what GPO / MDM wrote.
    ApplyLayer(HKEY_LOCAL_MACHINE, kPolicyKey, KEY_WOW64_64KEY, s, &locked);

    // Configured ExcludedApps extend, never replace, the built-in credential set.
    std::vector<std::wstring> apps(std::begin(kBaseExcludedApps), std::end(kBaseExcludedApps));
    apps.insert(apps.end(), s.excludedApps.begin(), s.excludedApps.end());
    s.excludedApps = NormalizeList(ListKind::ExeNames, apps);

    {
        std::unique_lock<std::shared_mutex> lock(m_mtx);
        std::swap(m_s, s);
        m_locked.swap(locked);
    }
    // The previous snapshot is freed here, after readers have been released.
}

WarpSettings Settings::Get() const
{
    std::shared_lock<std::shared_mutex> lock(m_mtx);
    return m_s;
}

bool Settings::IsLocked(const wchar_t* valueName) const
{
    if (valueName == nullptr) return false;
    std::shared_lock<std::shared_mutex> lock(m_mtx);
    return ContainsName(m_locked, valueName);
}

bool Settings::SetPausedUntil(int64_t utc)
{
    const ValueDef* def = FindValue(L"PausedUntilUtc", Kind::Int64);
    if (def == nullptr) return false;
    const ULONGLONG data = static_cast<ULONGLONG>(utc < 0 ? 0 : utc);   // 0 = resume, INT64_MAX = until resumed
    return CommitUserValue(*this, *def, REG_QWORD, &data, static_cast<DWORD>(sizeof(data)));
}

bool Settings::SetConsentAcknowledged(bool v)
{
    return SetBool(L"ConsentAcknowledged", v);
}

bool Settings::SetBool(const wchar_t* valueName, bool v)
{
    const ValueDef* def = FindValue(valueName, Kind::Bool);
    if (def == nullptr) return false;
    const DWORD data = v ? 1u : 0u;
    return CommitUserValue(*this, *def, REG_DWORD, &data, static_cast<DWORD>(sizeof(data)));
}

bool Settings::SetInt(const wchar_t* valueName, int v)
{
    const ValueDef* def = FindValue(valueName, Kind::Int);
    if (def == nullptr) return false;
    const DWORD data = static_cast<DWORD>(ClampToInt(v, def->minValue, def->maxValue));
    return CommitUserValue(*this, *def, REG_DWORD, &data, static_cast<DWORD>(sizeof(data)));
}

bool Settings::SetList(const wchar_t* valueName, const std::vector<std::wstring>& v)
{
    const ValueDef* def = FindValue(valueName, Kind::List);
    if (def == nullptr) return false;

    // REG_MULTI_SZ image: "a", NUL, "b", NUL, NUL (empty list: NUL, NUL). Entries are normalized on load.
    std::wstring data;
    for (const std::wstring& item : v)
    {
        const std::wstring entry = Trimmed(item.substr(0, item.find(L'\0')), false);
        if (entry.empty()) continue;
        if (def->pList == &WarpSettings::excludedApps && IsBaseExcludedApp(NormalizeExeName(entry)))
        {
            continue;   // always excluded anyway; keep HKCU to the user's own additions
        }
        data += entry;
        data.push_back(L'\0');
    }
    if (data.empty()) data.push_back(L'\0');
    data.push_back(L'\0');
    if (data.size() > kMaxListBytes / sizeof(wchar_t)) return false;
    return CommitUserValue(*this, *def, REG_MULTI_SZ, data.c_str(),
                           static_cast<DWORD>(data.size() * sizeof(wchar_t)));
}

bool Settings::IsPaused(int64_t nowUtc) const
{
    std::shared_lock<std::shared_mutex> lock(m_mtx);
    const int64_t until = m_s.pausedUntilUtc;
    return until != 0 && (until == INT64_MAX || nowUtc < until);
}

// The matching helpers below never allocate, so they cannot throw.

bool Settings::IsExcludedApp(const std::wstring& exePathOrName) const
{
    const wchar_t* name = exePathOrName.c_str();
    size_t len = exePathOrName.size();
    FileNameView(name, len);
    if (len == 0) return false;
    const bool bare = !HasExtension(name, len);   // "keepass" also matches "keepass.exe"

    std::shared_lock<std::shared_mutex> lock(m_mtx);
    for (const std::wstring& app : m_s.excludedApps)
    {
        if (EqualNoCase(app.c_str(), app.size(), name, len)) return true;
        if (bare && app.size() == len + 4 && EqualNoCase(app.c_str(), len, name, len) &&
            EqualNoCase(app.c_str() + len, 4, L".exe", 4))
        {
            return true;
        }
    }
    return false;
}

bool Settings::IsExcludedTitle(const std::wstring& windowTitle) const
{
    int titleLen = 0;
    if (windowTitle.empty() || !ToInt(windowTitle.size(), titleLen)) return false;

    std::shared_lock<std::shared_mutex> lock(m_mtx);
    for (const std::wstring& pattern : m_s.excludedTitlePatterns)
    {
        int patternLen = 0;
        if (pattern.empty() || !ToInt(pattern.size(), patternLen) || patternLen > titleLen) continue;
        if (FindStringOrdinal(FIND_FROMSTART, windowTitle.c_str(), titleLen,
                              pattern.c_str(), patternLen, TRUE) >= 0)
        {
            return true;
        }
    }
    return false;
}

bool Settings::IsExcludedDomain(const std::wstring& host) const
{
    const wchar_t* h = host.c_str();
    size_t len = host.size();
    HostView(h, len);
    if (len == 0) return false;

    std::shared_lock<std::shared_mutex> lock(m_mtx);
    for (const std::wstring& domain : m_s.excludedDomains)
    {
        const size_t domainLen = domain.size();
        if (domainLen == 0 || domainLen > len) continue;
        if (domainLen < len && h[len - domainLen - 1] != L'.') continue;   // subdomain only at a label boundary
        if (EqualNoCase(h + (len - domainLen), domainLen, domain.c_str(), domainLen)) return true;
    }
    return false;
}

bool Settings::IsExcludedPath(const std::wstring& path) const
{
    const wchar_t* p = path.c_str();
    size_t len = path.size();
    CanonicalPathView(p, len);
    if (len == 0) return false;

    std::shared_lock<std::shared_mutex> lock(m_mtx);
    for (const std::wstring& prefix : m_s.excludedPathPrefixes)
    {
        const wchar_t* q = prefix.c_str();
        size_t prefixLen = prefix.size();
        CanonicalPathView(q, prefixLen);
        if (PathHasPrefix(p, len, q, prefixLen)) return true;
    }
    return false;
}

std::vector<std::pair<std::wstring, std::wstring>> Settings::Describe() const
{
    std::vector<std::pair<std::wstring, std::wstring>> out;
    out.reserve(ARRAYSIZE(kValues));

    std::shared_lock<std::shared_mutex> lock(m_mtx);
    for (const ValueDef& def : kValues)
    {
        std::wstring value;
        switch (def.kind)
        {
        case Kind::Bool:
            value = (m_s.*def.pBool) ? L"true" : L"false";
            break;
        case Kind::Int:
            value = std::to_wstring(m_s.*def.pInt);
            break;
        case Kind::Int64:
            value = std::to_wstring(m_s.*def.pInt64);
            break;
        case Kind::List:
            value = Join(m_s.*def.pList, L';');
            break;
        }
        if (ContainsName(m_locked, def.name)) value += L" (policy)";
        out.emplace_back(def.name, std::move(value));
    }
    return out;
}
