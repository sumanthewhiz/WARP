#include "framework.h"
#include "WebCapture.h"

#include "AddressBarReader.h"
#include "EventContext.h"
#include "ForegroundChangeBroker.h"
#include "Logger.h"
#include "Settings.h"
#include "UrlCanonicalizer.h"

#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")

namespace
{
    constexpr ULONGLONG kDebounceMs         = 250;     // per-window trailing debounce (WEB-6)
    constexpr ULONGLONG kMaxDebounceMs      = 1000;    // cap: a constantly changing title is still read
    constexpr ULONGLONG kFailRetryDelayMs   = 1000;
    constexpr ULONGLONG kFocusRetryDelayMs  = 1500;
    constexpr int       kMaxFailRetries     = 2;
    constexpr int       kMaxIncompleteWalks = 3;
    constexpr int       kMaxProbeFailures   = 3;
    constexpr DWORD     kUiaTimeoutMs       = 500;
    constexpr size_t    kMaxWindows         = 64;
    constexpr size_t    kMaxRequests        = 16;
    constexpr size_t    kMaxCmdCache        = 64;
    constexpr size_t    kMaxExeVerdicts     = 256;
    constexpr size_t    kMaxTitleChars      = 4096;
    constexpr size_t    kMaxClockMarks      = 16;
    constexpr ULONGLONG kPollForegroundMs   = 2000;    // pause-state polling while a browser is foreground
    constexpr ULONGLONG kPollIdleMs         = 15000;
    constexpr ULONGLONG kSweepMs            = 10000;
    constexpr UINT      kMsgShutdown        = WM_APP + 0x131;
    constexpr UINT      kMsgReevaluate      = WM_APP + 0x132;
    const wchar_t       kMsgWindowClass[]   = L"WARP_WebCapture_MessageWindow";

    int64_t NowUnixMs()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000ULL);
    }

    std::wstring Lower(std::wstring s)
    {
        if (!s.empty())
            CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
        return s;
    }

    bool EndsWith(const std::wstring& s, const wchar_t* suffix)
    {
        const size_t n = wcslen(suffix);
        return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    }

    bool IsAsciiDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

    bool IsInvisibleChar(wchar_t c)
    {
        return c == 0x00AD || c == 0x061C || c == 0x180E || (c >= 0x200B && c <= 0x200F) ||
               (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x2064) || (c >= 0x2066 && c <= 0x2069) ||
               c == 0xFEFF;
    }

    bool IsSpaceChar(wchar_t c)
    {
        return c <= 0x20 || c == 0x7F || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
               c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
    }

    bool IsDashChar(wchar_t c) { return c == L'-' || (c >= 0x2012 && c <= 0x2015) || c == 0x2212; }

    // Zero-width / bidi / soft-hyphen characters removed; every Unicode space or control character
    // becomes one ' '; runs collapsed; trimmed (WEB-7).
    std::wstring CleanText(const std::wstring& s)
    {
        std::wstring out;
        out.reserve(s.size());
        bool pendingSpace = false;
        for (wchar_t c : s)
        {
            if (IsInvisibleChar(c))
                continue;
            if (IsSpaceChar(c))
            {
                pendingSpace = !out.empty();
                continue;
            }
            if (pendingSpace)
            {
                out.push_back(L' ');
                pendingSpace = false;
            }
            out.push_back(c);
        }
        return out;
    }

    // Lowercase browser names / suffixes a browser appends after " - " (or an en / em dash).
    const wchar_t* const kBrandSuffixes[] =
    {
        L"google chrome", L"google chrome beta", L"google chrome dev", L"google chrome canary", L"chrome",
        L"chromium", L"microsoft edge", L"microsoft edge beta", L"microsoft edge dev", L"microsoft edge canary",
        L"edge", L"mozilla firefox", L"firefox", L"firefox developer edition", L"firefox nightly",
        L"mozilla firefox private browsing", L"firefox private browsing", L"private browsing", L"brave",
        L"opera", L"opera gx", L"vivaldi", L"arc", L"zen", L"zen browser", L"floorp", L"librewolf", L"waterfox",
        L"yandex", L"yandex browser", L"thorium", L"duckduckgo", L"whale", L"naver whale", L"sidekick",
        L"mercury", L"tor browser", L"[inprivate]", L"inprivate",
    };

    // Trailing private / guest markers that follow the brand ("... - Google Chrome (Incognito)").
    const wchar_t* const kTrailingMarkers[] =
    {
        L" (incognito)", L" (private)", L" (guest)", L" (inkognito)", L" (inc\u00F3gnito)",
    };

    bool IsBrand(const std::wstring& lowerText, const std::wstring& lowerBrowser)
    {
        if (lowerText.empty())
            return false;
        if (!lowerBrowser.empty() && lowerText == lowerBrowser)
            return true;
        for (const wchar_t* brand : kBrandSuffixes)
        {
            if (lowerText == brand)
                return true;
        }
        return false;
    }

    // Index of the space that starts the LAST " <dash> " separator, or npos.
    size_t LastSeparator(const std::wstring& s)
    {
        if (s.size() < 3)
            return std::wstring::npos;
        for (size_t i = s.size() - 2; ; --i)
        {
            if (IsDashChar(s[i]) && s[i - 1] == L' ' && s[i + 1] == L' ')
                return i - 1;
            if (i == 1)
                break;
        }
        return std::wstring::npos;
    }

    // Edge appends " and N more page(s)" (English UI) to the active tab's title.
    void StripMorePages(std::wstring& s)
    {
        const std::wstring l = Lower(s);    // CharLowerBuffW keeps the length, so offsets match
        static const wchar_t* const kTails[] = { L" more page", L" more tab" };
        size_t pos = 0;
        while ((pos = l.find(L" and ", pos)) != std::wstring::npos)
        {
            size_t i = pos + 5, digits = 0;
            while (i < l.size() && IsAsciiDigit(l[i]))
            {
                ++i;
                ++digits;
            }
            for (const wchar_t* tail : kTails)
            {
                const size_t n = wcslen(tail);
                if (digits == 0 || l.compare(i, n, tail) != 0)
                    continue;
                size_t end = i + n;
                if (end < l.size() && l[end] == L's')
                    ++end;
                if (end == l.size() || l.compare(end, 3, L" - ") == 0)
                {
                    s.erase(pos, end - pos);
                    return;
                }
            }
            ++pos;
        }
    }

    // Leading unread counters / markers: "(3) Inbox", "(99+) Chat", "[2] Feed", "* Slack", "\u2022 Teams".
    void StripLeadingCounters(std::wstring& s)
    {
        for (int guard = 0; guard < 4 && !s.empty(); ++guard)
        {
            size_t i = 0;
            if (s[0] == L'(' || s[0] == L'[')
            {
                const wchar_t close = s[0] == L'(' ? L')' : L']';
                size_t j = 1, digits = 0;
                while (j < s.size() && IsAsciiDigit(s[j]))
                {
                    ++j;
                    ++digits;
                }
                if (j < s.size() && s[j] == L'+')
                    ++j;
                if (digits == 0 || digits > 6 || j >= s.size() || s[j] != close)
                    break;
                i = j + 1;
            }
            else if (s.size() >= 2 && (s[0] == L'*' || s[0] == 0x2022 || s[0] == 0x25CF) && s[1] == L' ')
            {
                i = 1;
            }
            else
            {
                break;
            }
            while (i < s.size() && s[i] == L' ')
                ++i;
            if (i >= s.size())
                break;                       // the title is only a counter: keep it
            s.erase(0, i);
        }
    }

    // ---- browser identification (WEB-3, WEB-5) ----
    struct BrowserDef
    {
        const wchar_t* exe;          // lowercase file name
        const wchar_t* name;         // friendly name
        const wchar_t* pathPart;     // required lowercase path fragment, or nullptr
    };

    const BrowserDef kBrowsers[] =
    {
        { L"chrome.exe",     L"Google Chrome",  nullptr },
        { L"msedge.exe",     L"Microsoft Edge", nullptr },
        { L"firefox.exe",    L"Firefox",        nullptr },
        { L"brave.exe",      L"Brave",          nullptr },
        { L"opera.exe",      L"Opera",          nullptr },
        { L"vivaldi.exe",    L"Vivaldi",        nullptr },
        { L"arc.exe",        L"Arc",            nullptr },
        { L"zen.exe",        L"Zen",            nullptr },
        { L"floorp.exe",     L"Floorp",         nullptr },
        { L"librewolf.exe",  L"LibreWolf",      nullptr },
        { L"waterfox.exe",   L"Waterfox",       nullptr },
        { L"browser.exe",    L"Yandex Browser", L"\\yandex\\" },
        { L"thorium.exe",    L"Thorium",        nullptr },
        { L"chromium.exe",   L"Chromium",       nullptr },
        { L"duckduckgo.exe", L"DuckDuckGo",     nullptr },
        { L"whale.exe",      L"Naver Whale",    nullptr },
        { L"sidekick.exe",   L"Sidekick",       nullptr },
        { L"mercury.exe",    L"Mercury",        nullptr },
    };

    // Chromium (Electron / CEF) and Gecko apps that are not browsers: never probed.
    const wchar_t* const kNonBrowserExes[] =
    {
        L"code.exe", L"code - insiders.exe", L"cursor.exe", L"windsurf.exe", L"slack.exe", L"discord.exe",
        L"teams.exe", L"ms-teams.exe", L"msteams.exe", L"spotify.exe", L"notion.exe", L"figma.exe",
        L"obsidian.exe", L"postman.exe", L"whatsapp.exe", L"signal.exe", L"githubdesktop.exe", L"gitkraken.exe",
        L"claude.exe", L"chatgpt.exe", L"element.exe", L"skype.exe", L"steamwebhelper.exe", L"electron.exe",
        L"msedgewebview2.exe", L"olk.exe", L"thunderbird.exe", L"betterbird.exe", L"applicationframehost.exe",
    };

    std::wstring FileNameLower(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        return Lower(slash == std::wstring::npos ? path : path.substr(slash + 1));
    }

    bool IdentifyBrowserExe(const std::wstring& exePath, std::wstring* name, bool* alwaysPrivate)
    {
        if (alwaysPrivate)
            *alwaysPrivate = false;
        const std::wstring file = FileNameLower(exePath);
        if (file.empty() || file == L"applicationframehost.exe" || file == L"msedgewebview2.exe")
            return false;                                       // WEB-3: UWP frame host / WebView2 are not browsers
        const std::wstring path = Lower(exePath);
        for (const BrowserDef& b : kBrowsers)
        {
            if (file != b.exe || (b.pathPart && path.find(b.pathPart) == std::wstring::npos))
                continue;
            std::wstring friendly = b.name;
            if (file == L"firefox.exe" && path.find(L"\\tor browser\\") != std::wstring::npos)
            {
                friendly = L"Tor Browser";
                if (alwaysPrivate)
                    *alwaysPrivate = true;
            }
            else if (file == L"chrome.exe" && path.find(L"\\chromium\\") != std::wstring::npos)
            {
                friendly = L"Chromium";
            }
            else if (file == L"opera.exe" && path.find(L"\\opera gx\\") != std::wstring::npos)
            {
                friendly = L"Opera GX";
            }
            if (name)
                *name = friendly;
            return true;
        }
        return false;
    }

    // Process-wide verdicts for unknown executables owning Chromium / Gecko windows (probed once).
    struct ExeVerdict
    {
        int          state = 0;          // 0 probing, 1 browser, 2 not a browser, 3 probe failed: retry
        std::wstring name;
        int          failures = 0;
    };
    std::mutex                                    g_verdictMtx;
    std::unordered_map<std::wstring, ExeVerdict>  g_verdicts;     // key: lowercase exe path

    bool LearnedBrowser(const std::wstring& exePath, std::wstring* name)
    {
        std::lock_guard<std::mutex> lk(g_verdictMtx);
        auto it = g_verdicts.find(Lower(exePath));
        if (it == g_verdicts.end() || it->second.state != 1)
            return false;
        if (name)
            *name = it->second.name;
        return true;
    }

    // A queued probe was dropped (queue bound / pause): allow probing again later.
    void ForgetVerdict(const std::wstring& exePath)
    {
        std::lock_guard<std::mutex> lk(g_verdictMtx);
        auto it = g_verdicts.find(Lower(exePath));
        if (it != g_verdicts.end() && it->second.state == 0)
            it->second.state = 3;
    }

    // Friendly name for a probed browser: "C:\...\dragon.exe" -> "Dragon".
    std::wstring NameFromExe(const std::wstring& exePath)
    {
        const size_t slash = exePath.find_last_of(L"\\/");
        std::wstring file = slash == std::wstring::npos ? exePath : exePath.substr(slash + 1);
        if (file.size() > 4 && Lower(file.substr(file.size() - 4)) == L".exe")
            file.resize(file.size() - 4);
        if (!file.empty() && file[0] >= L'a' && file[0] <= L'z')
            file[0] = static_cast<wchar_t>(file[0] - L'a' + L'A');
        return file;
    }

    void SetVerdict(const std::wstring& exePath, bool isBrowser, bool probeFailed)
    {
        std::lock_guard<std::mutex> lk(g_verdictMtx);
        ExeVerdict& v = g_verdicts[Lower(exePath)];
        if (probeFailed)
        {
            v.state = (++v.failures >= kMaxProbeFailures) ? 2 : 3;
            return;
        }
        v.state = isBrowser ? 1 : 2;
        if (isBrowser)
            v.name = NameFromExe(exePath);
    }

    std::wstring ClassOf(HWND hwnd)
    {
        wchar_t buf[256] = {};
        const int n = GetClassNameW(hwnd, buf, static_cast<int>(ARRAYSIZE(buf)));
        return n > 0 ? std::wstring(buf, static_cast<size_t>(n)) : std::wstring();
    }

    // True when an unknown exe owns a Chromium / Gecko top-level window and was never probed; marks it
    // "probing" so that it is probed once (Chrome_WidgetWin_1 is shared with Electron apps).
    bool NeedsProbe(HWND root, const std::wstring& exePath)
    {
        const std::wstring cls = ClassOf(root);
        if (cls != L"Chrome_WidgetWin_1" && cls != L"MozillaWindowClass")
            return false;
        const std::wstring file = FileNameLower(exePath);
        for (const wchar_t* exe : kNonBrowserExes)
        {
            if (file == exe)
                return false;
        }
        std::lock_guard<std::mutex> lk(g_verdictMtx);
        const std::wstring key = Lower(exePath);
        auto it = g_verdicts.find(key);
        if (it != g_verdicts.end())
        {
            if (it->second.state != 3)
                return false;                        // probing, browser (caller already knows) or not a browser
            it->second.state = 0;                    // an earlier probe failed: try again
            return true;
        }
        if (g_verdicts.size() >= kMaxExeVerdicts)
            return false;
        g_verdicts.emplace(key, ExeVerdict());
        return true;
    }

    // GetWindowTextW never sends a message to a window of another process, so it cannot block.
    std::wstring WindowText(HWND hwnd)
    {
        if (!hwnd)
            return std::wstring();
        std::vector<wchar_t> buf(kMaxTitleChars + 1, L'\0');
        const int n = GetWindowTextW(hwnd, buf.data(), static_cast<int>(buf.size()));
        return n > 0 ? std::wstring(buf.data(), static_cast<size_t>(n)) : std::wstring();
    }

    // Best-effort command line of another process: NtQueryInformationProcess(ProcessCommandLineInformation)
    // works with PROCESS_QUERY_LIMITED_INFORMATION (Windows 8.1+).
    typedef LONG (NTAPI* PFN_NtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    struct CountedUnicodeString { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };

    std::wstring ReadCommandLine(DWORD pid)
    {
        static const PFN_NtQueryInformationProcess query = []() -> PFN_NtQueryInformationProcess {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            return ntdll ? reinterpret_cast<PFN_NtQueryInformationProcess>(GetProcAddress(ntdll, "NtQueryInformationProcess"))
                         : nullptr;
        }();
        if (!query || pid == 0)
            return std::wstring();
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process)
            return std::wstring();
        std::wstring out;
        const ULONG kProcessCommandLineInformation = 60;
        ULONG needed = 0;
        query(process, kProcessCommandLineInformation, nullptr, 0, &needed);
        if (needed >= sizeof(CountedUnicodeString) && needed <= 256 * 1024)
        {
            std::vector<ULONG_PTR> buf(needed / sizeof(ULONG_PTR) + 2, 0);    // pointer-aligned
            const ULONG cb = static_cast<ULONG>(buf.size() * sizeof(ULONG_PTR));
            if (query(process, kProcessCommandLineInformation, buf.data(), cb, &needed) >= 0)
            {
                const CountedUnicodeString* us = reinterpret_cast<const CountedUnicodeString*>(buf.data());
                const BYTE* lo = reinterpret_cast<const BYTE*>(buf.data());
                const BYTE* hi = lo + cb;
                const BYTE* p = reinterpret_cast<const BYTE*>(us->Buffer);
                if (p && us->Length > 0 && p >= lo && p + us->Length <= hi)
                    out.assign(us->Buffer, us->Length / sizeof(wchar_t));
            }
        }
        CloseHandle(process);
        return out;
    }

    // Lowercase canonical URLs of new-tab / blank pages (trailing '/' removed before comparing).
    bool IsPlaceholderUrl(const std::wstring& canonicalUrl)
    {
        if (canonicalUrl.empty())
            return false;
        std::wstring u = Lower(canonicalUrl);
        while (!u.empty() && u.back() == L'/')
            u.pop_back();
        static const wchar_t* const kUrls[] =
        {
            L"about:blank", L"about:newtab", L"about:home", L"about:privatebrowsing", L"chrome://newtab",
            L"chrome://new-tab-page", L"chrome://new-tab-page-third-party", L"chrome-search://local-ntp/local-ntp.html",
            L"edge://newtab", L"brave://newtab", L"vivaldi://startpage", L"opera://startpage",
        };
        for (const wchar_t* k : kUrls)
        {
            if (u == k)
                return true;
        }
        return false;
    }
}   // namespace

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------

std::wstring WebCapture::NormalizeTitle(const std::wstring& rawTitle, const std::wstring& browserName)
{
    std::wstring s = CleanText(rawTitle.size() > kMaxTitleChars ? rawTitle.substr(0, kMaxTitleChars) : rawTitle);
    const std::wstring lowerBrowser = Lower(CleanText(browserName));
    if (IsBrand(Lower(s), lowerBrowser))
        return std::wstring();                          // bare browser name: there is no page title
    for (int guard = 0; guard < 6; ++guard)
    {
        const std::wstring l = Lower(s);
        bool stripped = false;
        for (const wchar_t* marker : kTrailingMarkers)
        {
            if (EndsWith(l, marker))
            {
                s.erase(s.size() - wcslen(marker));
                stripped = true;
                break;
            }
        }
        if (stripped)
            continue;
        const size_t sep = LastSeparator(s);
        if (sep == std::wstring::npos || !IsBrand(l.substr(sep + 3), lowerBrowser))
            break;
        s.erase(sep);                                    // " - Google Chrome", " \u2014 Mozilla Firefox", " - [InPrivate]"
    }
    StripMorePages(s);
    StripLeadingCounters(s);
    return CleanText(s);
}

std::wstring WebCapture::TitleDedupKey(const std::wstring& normalizedTitle)
{
    const std::wstring l = Lower(CleanText(normalizedTitle));
    std::wstring out;
    out.reserve(l.size());
    for (wchar_t c : l)
    {
        if (!IsAsciiDigit(c))
            out.push_back(c);
    }
    // "(3)" / "[12+]" become "()" / "[+]" once digits are gone: drop the empty shells too.
    static const wchar_t* const kShells[] = { L"(+)", L"[+]", L"()", L"[]" };
    for (const wchar_t* shell : kShells)
    {
        const size_t n = wcslen(shell);
        size_t pos = 0;
        while ((pos = out.find(shell, pos)) != std::wstring::npos)
            out.erase(pos, n);
    }
    return CleanText(out);
}

bool WebCapture::TitleMarksPrivate(const std::wstring& rawTitle)
{
    if (AddressBarReader::RootNameMarksPrivate(rawTitle))    // "[InPrivate]", "... (Incognito)", "... Private Browsing"
        return true;
    // The private new-tab page names itself; compared on the whole page title, never a substring.
    const std::wstring page = Lower(NormalizeTitle(rawTitle));
    return page == L"new incognito tab" || page == L"new inprivate tab" || page == L"new private tab";
}

bool WebCapture::CommandLineMarksPrivate(const std::wstring& commandLine)
{
    size_t i = 0;
    const size_t n = commandLine.size();
    bool program = true;
    while (i < n)
    {
        while (i < n && IsSpaceChar(commandLine[i]))
            ++i;
        if (i >= n)
            break;
        std::wstring token;
        bool quoted = false;
        while (i < n && (quoted || !IsSpaceChar(commandLine[i])))
        {
            if (commandLine[i] == L'"')
                quoted = !quoted;
            else
                token.push_back(commandLine[i]);
            ++i;
        }
        if (program)
        {
            program = false;                            // the image path itself is never a switch
            continue;
        }
        if (token.size() < 2 || token[0] != L'-')
            continue;
        size_t k = 0;
        while (k < token.size() && token[k] == L'-')
            ++k;
        std::wstring name = Lower(token.substr(k));
        const size_t eq = name.find(L'=');
        if (eq != std::wstring::npos)
            name.resize(eq);
        // Chromium --incognito, Edge --inprivate, Firefox -private / -private-window, Opera --private, Brave --tor
        if (name == L"incognito" || name == L"inprivate" || name == L"private" || name == L"private-window" ||
            name == L"tor")
            return true;
    }
    return false;
}

bool WebCapture::IsPlaceholderTitle(const std::wstring& normalizedTitle)
{
    const std::wstring l = Lower(CleanText(normalizedTitle));
    if (l.empty())
        return true;
    static const wchar_t* const kTitles[] =
    {
        L"new tab", L"new tab page", L"untitled", L"start page", L"speed dial", L"blank page", L"about:blank",
        L"new private tab", L"new incognito tab", L"new inprivate tab", L"private browsing", L"loading",
        L"loading...", L"loading\u2026", L"new window",
    };
    for (const wchar_t* t : kTitles)
    {
        if (l == t)
            return true;
    }
    return false;
}

bool WebCapture::IsBrowserProcess(const std::wstring& exePath, std::wstring* browserName)
{
    if (exePath.empty())
        return false;
    return IdentifyBrowserExe(exePath, browserName, nullptr) || LearnedBrowser(exePath, browserName);
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

class WebCapture::Impl
{
public:
    Impl() = default;
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void  SetCallback(Callback cb);
    void  Start();
    void  Stop();
    void  SetPaused(bool paused);
    void  Heartbeat();
    bool  LookupPage(HWND root, PageInfo& out) const;
    bool  IsPrivateWindow(HWND root) const;
    Stats GetStats() const;

private:
    // Foreground-time clock of one window: At(t) = milliseconds the window was foreground (and capture
    // not paused) up to tick t. Recent transitions are kept so At() is exact for slightly older ticks.
    struct FgClock
    {
        struct Mark { ULONGLONG t; int64_t f; bool running; };
        std::deque<Mark> marks;

        void Set(bool run, ULONGLONG t)
        {
            if (marks.empty())
            {
                marks.push_back(Mark{ t, 0, run });
                return;
            }
            if (marks.back().running == run)
                return;
            if (t < marks.back().t)
                t = marks.back().t;
            const int64_t f = At(t);
            marks.push_back(Mark{ t, f, run });
            while (marks.size() > kMaxClockMarks)
                marks.pop_front();
        }

        int64_t At(ULONGLONG t) const
        {
            if (marks.empty())
                return 0;
            for (auto it = marks.rbegin(); it != marks.rend(); ++it)
            {
                if (it->t <= t)
                    return it->f + (it->running ? static_cast<int64_t>(t - it->t) : 0);
            }
            return marks.front().f;
        }
    };

    struct WindowState
    {
        DWORD        pid = 0;
        std::wstring exePath;
        std::wstring browser;
        bool         appExcluded = false;
        bool         quickPrivate = false;   // title marker, command line or Tor (sticky for the window)
        int          uiaPrivate = -1;        // -1 unknown, 0 no marker, 1 private marker (sticky)
        int          incompleteWalks = 0;
        bool         hasPage = false;
        PageInfo     page;
        bool         queuedOnce = false;
        std::wstring lastQueuedTitle;        // normalized title last handed to the worker (trigger de-dup)
        bool         pageOpen = false;       // the last page shown was recorded; its dwell is still running
        int64_t      pageStartF = 0;         // clock value when the current page (or gap) started
        int64_t      pendingDwellMs = 0;     // dwell of a recorded page closed by an unrecorded one
        bool         hasEmitted = false;
        std::wstring lastEmitUrl;
        std::wstring lastEmitKey;
        FgClock      clock;
        ULONGLONG    lastSeenTick = 0;

        bool IsPrivate() const { return quickPrivate || uiaPrivate == 1; }
    };

    enum class Kind { Capture, Probe };

    struct Request
    {
        Kind         kind = Kind::Capture;
        HWND         root = nullptr;
        DWORD        pid = 0;
        std::wstring exePath;
        std::wstring browser;
        std::wstring rawTitle;
        std::wstring normTitle;
        int64_t      observedMs = 0;         // Unix ms of the page change (event time)
        ULONGLONG    observedTick = 0;
        ULONGLONG    firstTick = 0;          // first enqueue of this burst (debounce cap)
        ULONGLONG    dueTick = 0;
        int          failRetries = 0;
        bool         focusRetried = false;
    };

    enum class Unrecorded { Placeholder, Private, Excluded };

    struct CmdEntry
    {
        std::wstring exe;
        bool         isPrivate = false;
    };

    // ---- UI thread ----
    static std::recursive_mutex& UiMutex();
    static std::vector<Impl*>&   Live();
    static bool                  IsLive(const Impl* p);       // UiMutex held
    static void                  BrokerThunk(Impl* self, HWND hwnd, DWORD pid);
    static void CALLBACK         NameChangeProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject,
                                                LONG idChild, DWORD thread, DWORD time);
    static LRESULT CALLBACK      MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    HWND CreateMessageWindow();
    void OnForeground(HWND hwnd, DWORD pid);
    void OnNameChange(HWND hwnd);
    void Reevaluate();
    void EnsureHook(DWORD pid);
    void RemoveHook();
    void UiTeardown();
    void EnqueueCapture(HWND root, const std::wstring& rawTitle, const std::wstring& browser, int64_t nowMs,
                        ULONGLONG tick);
    void EnqueueProbe(HWND root, DWORD pid, const std::wstring& exePath, ULONGLONG tick);

    // ---- any thread ----
    bool IsPausedNow(int64_t nowMs) const;
    void ApplyPause(bool paused, ULONGLONG tick);
    void Wake();
    void PostToUi(UINT msg);
    bool CommandLinePrivate(DWORD pid, const std::wstring& exePath) const;
    WindowState& GetOrCreateLocked(HWND root, DWORD pid, const std::wstring& exePath, const std::wstring& browser,
                                   ULONGLONG tick);
    void SyncClocksLocked(ULONGLONG tick);
    void TrimRequestsLocked();

    // ---- worker thread ----
    void WorkerMain();
    void WorkerLoop(AddressBarReader& reader, bool uiaReady);
    void Sweep(AddressBarReader& reader);
    void ProcessProbe(AddressBarReader& reader, bool uiaReady, const Request& req);
    void ProcessCapture(AddressBarReader& reader, bool uiaReady, Request req);
    void CountUia(const AddressBarReader::Result& r);
    bool HasPending(HWND root) const;
    void Requeue(Request req, ULONGLONG delayMs);
    void ResetQueued(HWND root, const std::wstring& normTitle);
    void RecordUiaVerdict(HWND root, int verdict, bool incomplete);
    void MarkUnrecorded(HWND root, Unrecorded why, int64_t observedMs, ULONGLONG observedTick);
    void Emit(BrowseEventRecord&& rec);

    // ---- shared state (m_mtx) ----
    mutable std::mutex                       m_mtx;
    std::condition_variable                  m_cv;
    std::unordered_map<HWND, WindowState>    m_windows;
    std::unordered_map<HWND, Request>        m_requests;     // latest request per window (debounce)
    mutable std::unordered_map<DWORD, CmdEntry> m_cmdCache;
    HWND                                     m_fgRoot = nullptr;
    HWND                                     m_msgWnd = nullptr;
    bool                                     m_pausedLatched = false;
    bool                                     m_stop = false;
    bool                                     m_wake = false;
    Stats                                    m_stats;

    std::mutex                               m_cbMtx;
    Callback                                 m_callback;

    std::atomic<bool>                        m_running{ false };
    std::atomic<bool>                        m_paused{ false };
    std::atomic<bool>                        m_cancel{ false };
    std::atomic<bool>                        m_dropPrivate{ true };
    std::thread                              m_worker;

    // ---- UI thread only ----
    DWORD                                    m_uiThreadId = 0;
    size_t                                   m_brokerToken = 0;
    HWINEVENTHOOK                            m_nameHook = nullptr;
    DWORD                                    m_hookPid = 0;
};

// ---- lifetime / registry --------------------------------------------------
// UI-thread entry points (broker callback, WinEvent hook, message window) run with UiMutex held and
// only touch an instance that is still registered, so Stop() from any thread is safe.

std::recursive_mutex& WebCapture::Impl::UiMutex()
{
    static std::recursive_mutex m;
    return m;
}

std::vector<WebCapture::Impl*>& WebCapture::Impl::Live()
{
    static std::vector<Impl*> v;
    return v;
}

bool WebCapture::Impl::IsLive(const Impl* p)
{
    const std::vector<Impl*>& v = Live();
    return std::find(v.begin(), v.end(), p) != v.end();
}

WebCapture::Impl::~Impl()
{
    Stop();
    if (m_worker.joinable())
    {
        if (m_worker.get_id() == std::this_thread::get_id())
            m_worker.detach();                          // destroyed from its own callback (unsupported)
        else
            m_worker.join();
    }
}

void WebCapture::Impl::SetCallback(Callback cb)
{
    std::lock_guard<std::mutex> lk(m_cbMtx);
    m_callback = std::move(cb);
}

void WebCapture::Impl::Start()
{
    if (m_running.load())
        return;
    if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id())
        m_worker.join();                                // a previous Stop() was issued from the worker

    m_uiThreadId = GetCurrentThreadId();
    m_cancel.store(false);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stop = false;
        m_wake = false;
        m_requests.clear();
        m_fgRoot = nullptr;
        m_pausedLatched = false;
    }
    const HWND wnd = CreateMessageWindow();
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_msgWnd = wnd;
    }
    {
        std::lock_guard<std::recursive_mutex> g(UiMutex());
        Live().push_back(this);
    }
    m_running.store(true);
    try
    {
        m_worker = std::thread(&Impl::WorkerMain, this);
    }
    catch (...)
    {
        WARP_LOG_ERROR("web", "cannot start the web capture worker thread");
        m_running.store(false);
        std::lock_guard<std::recursive_mutex> g(UiMutex());
        UiTeardown();
        Live().erase(std::remove(Live().begin(), Live().end(), this), Live().end());
        return;
    }

    Impl* self = this;
    m_brokerToken = ForegroundChangeBroker::Instance().Subscribe([self](HWND hwnd, DWORD pid) { BrokerThunk(self, hwnd, pid); });
    ApplyPause(IsPausedNow(NowUnixMs()), GetTickCount64());

    // Seed with the current foreground window (APP-9): the user may already be on a page.
    const HWND fg = GetForegroundWindow();
    if (fg)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        BrokerThunk(this, fg, pid);
    }
    WARP_LOG_INFO("web", "web capture started");
}

void WebCapture::Impl::Stop()
{
    if (!m_running.exchange(false))
        return;
    if (m_brokerToken)
    {
        ForegroundChangeBroker::Instance().Unsubscribe(m_brokerToken);
        m_brokerToken = 0;
    }

    // Stop flag set under the mutex before notifying: the worker cannot miss the wake-up.
    m_cancel.store(true);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_stop = true;
        for (const auto& kv : m_requests)
        {
            if (kv.second.kind == Kind::Probe)
                ForgetVerdict(kv.second.exePath);       // dropped probe: may be probed again after a restart
        }
        m_requests.clear();
    }
    m_cv.notify_all();
    if (m_worker.joinable() && m_worker.get_id() != std::this_thread::get_id())
        m_worker.join();

    // Hooks and the message window belong to the UI thread.
    if (GetCurrentThreadId() == m_uiThreadId)
    {
        std::lock_guard<std::recursive_mutex> g(UiMutex());
        UiTeardown();
    }
    else
    {
        HWND wnd = nullptr;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            wnd = m_msgWnd;
        }
        DWORD_PTR result = 0;
        if (wnd && !SendMessageTimeoutW(wnd, kMsgShutdown, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result))
            WARP_LOG_WARN("web", "UI thread did not process the shutdown request; its hook ends with that thread");
    }
    {
        std::lock_guard<std::recursive_mutex> g(UiMutex());
        Live().erase(std::remove(Live().begin(), Live().end(), this), Live().end());
    }
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_windows.clear();
        m_cmdCache.clear();
        m_fgRoot = nullptr;
    }
    WARP_LOG_INFO("web", "web capture stopped");
}

void WebCapture::Impl::SetPaused(bool paused)
{
    m_paused.store(paused);
    ApplyPause(IsPausedNow(NowUnixMs()), GetTickCount64());
    Wake();
}

void WebCapture::Impl::Heartbeat()
{
    ApplyPause(IsPausedNow(NowUnixMs()), GetTickCount64());
    Wake();
}

void WebCapture::Impl::Wake()
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_wake = true;
    }
    m_cv.notify_all();
}

bool WebCapture::Impl::IsPausedNow(int64_t nowMs) const
{
    return m_paused.load() || Settings::Instance().IsPaused(nowMs / 1000);
}

// Latches the pause state. Foreground clocks stop while paused (dwell excludes paused time), queued
// requests are dropped, and on resume the current foreground window is re-evaluated on the UI thread.
void WebCapture::Impl::ApplyPause(bool paused, ULONGLONG tick)
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (paused == m_pausedLatched)
            return;
        m_pausedLatched = paused;
        SyncClocksLocked(tick);
        if (paused)
        {
            for (auto& kv : m_requests)
            {
                auto w = m_windows.find(kv.first);
                if (w != m_windows.end())
                    w->second.queuedOnce = false;          // re-captured after resume
                if (kv.second.kind == Kind::Probe)
                    ForgetVerdict(kv.second.exePath);
            }
            m_requests.clear();
        }
    }
    WARP_LOG_INFO("web", paused ? "web capture paused" : "web capture resumed");
    if (!paused)
        PostToUi(kMsgReevaluate);
}

void WebCapture::Impl::PostToUi(UINT msg)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_msgWnd)
        PostMessageW(m_msgWnd, msg, 0, 0);
}

// ---- UI thread --------------------------------------------------------------

HWND WebCapture::Impl::CreateMessageWindow()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Impl::MsgWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kMsgWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        WARP_LOG_WARN("web", "RegisterClassEx failed (%lu); cross-thread Stop() cannot unhook", GetLastError());
        return nullptr;
    }
    const HWND wnd = CreateWindowExW(0, kMsgWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!wnd)
    {
        WARP_LOG_WARN("web", "message window creation failed (%lu)", GetLastError());
        return nullptr;
    }
    SetWindowLongPtrW(wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return wnd;
}

LRESULT CALLBACK WebCapture::Impl::MsgWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == kMsgShutdown || msg == kMsgReevaluate)
    {
        try
        {
            std::lock_guard<std::recursive_mutex> g(UiMutex());
            Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self && IsLive(self))
            {
                if (msg == kMsgShutdown)
                    self->UiTeardown();
                else
                    self->Reevaluate();
            }
            else if (msg == kMsgShutdown)
            {
                DestroyWindow(hwnd);
            }
        }
        catch (...)
        {
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void WebCapture::Impl::BrokerThunk(Impl* self, HWND hwnd, DWORD pid)
{
    try
    {
        std::lock_guard<std::recursive_mutex> g(UiMutex());
        if (IsLive(self))
            self->OnForeground(hwnd, pid);
    }
    catch (const std::exception& e)
    {
        WARP_LOG_ERROR("web", "foreground handler failed: %s", e.what());
    }
    catch (...)
    {
        WARP_LOG_ERROR("web", "foreground handler failed");
    }
}

void CALLBACK WebCapture::Impl::NameChangeProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject,
                                               LONG idChild, DWORD, DWORD)
{
    if (event != EVENT_OBJECT_NAMECHANGE || idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd)
        return;
    try
    {
        std::lock_guard<std::recursive_mutex> g(UiMutex());
        for (Impl* p : Live())
        {
            if (p->m_nameHook == hook)
            {
                p->OnNameChange(hwnd);
                break;
            }
        }
    }
    catch (...)
    {
    }
}

void WebCapture::Impl::UiTeardown()
{
    RemoveHook();
    HWND wnd = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        wnd = m_msgWnd;
        m_msgWnd = nullptr;
    }
    if (wnd)
    {
        SetWindowLongPtrW(wnd, GWLP_USERDATA, 0);
        DestroyWindow(wnd);
    }
}

void WebCapture::Impl::EnsureHook(DWORD pid)
{
    if (m_nameHook && m_hookPid == pid)
        return;
    RemoveHook();
    // Scoped to the browser process: no global NAMECHANGE firehose (WEB-5).
    m_nameHook = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, &Impl::NameChangeProc,
                                 pid, 0, WINEVENT_OUTOFCONTEXT);
    if (m_nameHook)
        m_hookPid = pid;
    else
        WARP_LOG_WARN("web", "SetWinEventHook(NAMECHANGE, pid=%lu) failed (%lu)", pid, GetLastError());
}

void WebCapture::Impl::RemoveHook()
{
    if (m_nameHook)
    {
        UnhookWinEvent(m_nameHook);
        m_nameHook = nullptr;
    }
    m_hookPid = 0;
}

// Runs on every foreground change (not only PID changes) so that two windows of one browser are
// distinguished: the tracked unit is the ROOT window (WEB-5).
void WebCapture::Impl::OnForeground(HWND hwnd, DWORD pid)
{
    if (!m_running.load())
        return;
    const ULONGLONG tick = GetTickCount64();
    const int64_t nowMs = NowUnixMs();
    HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
    if (!root)
        root = hwnd;
    DWORD rootPid = 0;
    if (root)
        GetWindowThreadProcessId(root, &rootPid);
    if (!rootPid)
        rootPid = pid;

    bool isBrowser = false, alwaysPrivate = false, probe = false;
    std::wstring exePath, browser;
    // Session hygiene: windows of processes outside WARP's session are ignored (RUN-3).
    if (root && rootPid && rootPid != GetCurrentProcessId() && EventContextUtil::IsOwnSessionPid(rootPid))
    {
        exePath = EventContextUtil::GetExePathByPid(rootPid);
        if (!exePath.empty())
        {
            isBrowser = IdentifyBrowserExe(exePath, &browser, &alwaysPrivate) || LearnedBrowser(exePath, &browser);
            if (!isBrowser)
                probe = NeedsProbe(root, exePath);
        }
    }
    const bool excludedApp = isBrowser && Settings::Instance().IsExcludedApp(exePath);
    const std::wstring rawTitle = isBrowser ? WindowText(root) : std::wstring();
    const bool cmdPrivate = isBrowser && !excludedApp && !alwaysPrivate && CommandLinePrivate(rootPid, exePath);
    ApplyPause(IsPausedNow(nowMs), tick);

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_fgRoot = isBrowser ? root : nullptr;
        if (isBrowser)
        {
            WindowState& ws = GetOrCreateLocked(root, rootPid, exePath, browser, tick);
            ws.appExcluded = excludedApp;
            if (alwaysPrivate || cmdPrivate || WebCapture::TitleMarksPrivate(rawTitle))
                ws.quickPrivate = true;
            if (excludedApp && !(ws.hasPage && ws.page.excluded))
            {
                ws.hasPage = true;
                ws.page = PageInfo();
                ws.page.browser = browser;
                ws.page.pid = rootPid;
                ws.page.excluded = true;
                ws.page.isPrivate = ws.IsPrivate();
                ws.page.sinceMs = nowMs;
            }
        }
        SyncClocksLocked(tick);
    }

    if (isBrowser && !excludedApp)
    {
        EnsureHook(rootPid);
        EnqueueCapture(root, rawTitle, browser, nowMs, tick);
    }
    else
    {
        RemoveHook();                                   // foreground left the browser
        if (probe)
            EnqueueProbe(root, rootPid, exePath, tick);
    }
}

void WebCapture::Impl::OnNameChange(HWND hwnd)
{
    if (!m_running.load())
        return;
    std::wstring browser;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (hwnd != m_fgRoot)
            return;                                     // only the tracked root window's title matters
        auto it = m_windows.find(hwnd);
        if (it == m_windows.end() || it->second.appExcluded)
            return;
        browser = it->second.browser;
    }
    const ULONGLONG tick = GetTickCount64();
    const int64_t nowMs = NowUnixMs();
    const std::wstring rawTitle = WindowText(hwnd);
    if (WebCapture::TitleMarksPrivate(rawTitle))
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_windows.find(hwnd);
        if (it != m_windows.end())
            it->second.quickPrivate = true;
    }
    EnqueueCapture(hwnd, rawTitle, browser, nowMs, tick);
}

void WebCapture::Impl::Reevaluate()
{
    if (!m_running.load())
        return;
    const HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg)
        GetWindowThreadProcessId(fg, &pid);
    OnForeground(fg, pid);
}

// Hands the latest title of a window to the worker. Keeps only the newest request per window and
// debounces it (trailing kDebounceMs, capped at kMaxDebounceMs since the first change of the burst).
void WebCapture::Impl::EnqueueCapture(HWND root, const std::wstring& rawTitle, const std::wstring& browser,
                                      int64_t nowMs, ULONGLONG tick)
{
    if (IsPausedNow(nowMs))
        return;                                         // nothing is recorded while paused
    const std::wstring norm = WebCapture::NormalizeTitle(rawTitle, browser);
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_stop || m_pausedLatched)
            return;
        auto it = m_windows.find(root);
        if (it == m_windows.end() || it->second.appExcluded)
            return;
        WindowState& ws = it->second;
        if (ws.queuedOnce && norm == ws.lastQueuedTitle)
            return;                                     // same page title (unread counters already stripped)
        ws.queuedOnce = true;
        ws.lastQueuedTitle = norm;

        Request r;
        r.kind = Kind::Capture;
        r.root = root;
        r.pid = ws.pid;
        r.exePath = ws.exePath;
        r.browser = ws.browser;
        r.rawTitle = rawTitle;
        r.normTitle = norm;
        r.observedMs = nowMs;
        r.observedTick = tick;
        r.firstTick = tick;
        auto old = m_requests.find(root);
        if (old != m_requests.end() && old->second.kind == Kind::Capture)
            r.firstTick = old->second.firstTick;
        r.dueTick = (std::min)(tick + kDebounceMs, r.firstTick + kMaxDebounceMs);
        m_requests[root] = std::move(r);
        TrimRequestsLocked();
    }
    m_cv.notify_one();
}

void WebCapture::Impl::EnqueueProbe(HWND root, DWORD pid, const std::wstring& exePath, ULONGLONG tick)
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_stop || m_requests.count(root))
        {
            ForgetVerdict(exePath);
            return;
        }
        Request r;
        r.kind = Kind::Probe;
        r.root = root;
        r.pid = pid;
        r.exePath = exePath;
        r.observedTick = tick;
        r.firstTick = tick;
        r.dueTick = tick + kDebounceMs;
        m_requests[root] = std::move(r);
        TrimRequestsLocked();
    }
    m_cv.notify_one();
}

WebCapture::Impl::WindowState& WebCapture::Impl::GetOrCreateLocked(HWND root, DWORD pid, const std::wstring& exePath,
                                                                   const std::wstring& browser, ULONGLONG tick)
{
    auto it = m_windows.find(root);
    if (it != m_windows.end() && it->second.pid != pid)
    {
        m_windows.erase(it);                            // recycled HWND: never inherit another window's verdicts
        it = m_windows.end();
    }
    if (it == m_windows.end())
    {
        if (m_windows.size() >= kMaxWindows)
        {
            auto victim = m_windows.end();
            for (auto w = m_windows.begin(); w != m_windows.end(); ++w)
            {
                if (w->first == m_fgRoot || m_requests.count(w->first))
                    continue;
                if (victim == m_windows.end() || w->second.lastSeenTick < victim->second.lastSeenTick)
                    victim = w;
            }
            if (victim != m_windows.end())
                m_windows.erase(victim);
        }
        WindowState ws;
        ws.pid = pid;
        ws.exePath = exePath;
        ws.browser = browser;
        it = m_windows.emplace(root, std::move(ws)).first;
    }
    it->second.lastSeenTick = tick;
    return it->second;
}

// A window's clock runs only while it is the foreground root and capture is not paused.
void WebCapture::Impl::SyncClocksLocked(ULONGLONG tick)
{
    for (auto& kv : m_windows)
        kv.second.clock.Set(kv.first == m_fgRoot && !m_pausedLatched, tick);
}

void WebCapture::Impl::TrimRequestsLocked()
{
    while (m_requests.size() > kMaxRequests)
    {
        auto victim = m_requests.end();
        for (auto it = m_requests.begin(); it != m_requests.end(); ++it)
        {
            if (it->first == m_fgRoot)
                continue;
            if (victim == m_requests.end() || it->second.firstTick < victim->second.firstTick)
                victim = it;
        }
        if (victim == m_requests.end())
            break;
        if (victim->second.kind == Kind::Probe)
        {
            ForgetVerdict(victim->second.exePath);
        }
        else
        {
            auto w = m_windows.find(victim->first);
            if (w != m_windows.end())
                w->second.queuedOnce = false;
        }
        m_requests.erase(victim);
        ++m_stats.queueDropped;
    }
}

bool WebCapture::Impl::CommandLinePrivate(DWORD pid, const std::wstring& exePath) const
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_cmdCache.find(pid);
        if (it != m_cmdCache.end() && it->second.exe == exePath)
            return it->second.isPrivate;
    }
    const bool isPrivate = WebCapture::CommandLineMarksPrivate(ReadCommandLine(pid));
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_cmdCache.size() >= kMaxCmdCache)
        m_cmdCache.clear();
    CmdEntry e;
    e.exe = exePath;
    e.isPrivate = isPrivate;
    m_cmdCache[pid] = std::move(e);
    return isPrivate;
}

// ---- worker thread ------------------------------------------------------------

void WebCapture::Impl::WorkerMain()
{
    try
    {
        struct ComScope
        {
            HRESULT hr;
            ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
            ~ComScope()
            {
                if (SUCCEEDED(hr))
                    CoUninitialize();
            }
            ComScope(const ComScope&) = delete;
            ComScope& operator=(const ComScope&) = delete;
        } com;

        AddressBarReader reader;                        // destroyed before CoUninitialize
        const bool uiaReady = SUCCEEDED(com.hr) && reader.Init(kUiaTimeoutMs);
        if (!uiaReady)
            WARP_LOG_ERROR("web", "UI Automation unavailable (CoInitializeEx hr=0x%08lX): browser windows cannot be "
                                  "verified as non-private, so no browsing is recorded while DropPrivateBrowsing is on",
                           static_cast<unsigned long>(com.hr));
        WorkerLoop(reader, uiaReady);
        reader.Shutdown();
    }
    catch (const std::exception& e)
    {
        WARP_LOG_ERROR("web", "web capture worker terminated: %s", e.what());
    }
    catch (...)
    {
        WARP_LOG_ERROR("web", "web capture worker terminated by an unknown exception");
    }
}

void WebCapture::Impl::WorkerLoop(AddressBarReader& reader, bool uiaReady)
{
    ULONGLONG lastSweep = GetTickCount64();
    for (;;)
    {
        Request req;
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            if (m_stop)
                break;
            const ULONGLONG now = GetTickCount64();
            auto due = m_requests.end();
            for (auto it = m_requests.begin(); it != m_requests.end(); ++it)
            {
                if (due == m_requests.end() || it->second.dueTick < due->second.dueTick)
                    due = it;
            }
            if (due != m_requests.end() && due->second.dueTick <= now)
            {
                req = std::move(due->second);
                m_requests.erase(due);
                have = true;
            }
            else if (!m_wake)
            {
                ULONGLONG waitMs = m_fgRoot ? kPollForegroundMs : kPollIdleMs;
                if (due != m_requests.end())
                    waitMs = (std::min)(waitMs, due->second.dueTick - now);
                m_cv.wait_for(lk, std::chrono::milliseconds(static_cast<long long>(waitMs)));
            }
            m_wake = false;
            if (m_stop)
                break;
        }

        try
        {
            const ULONGLONG now = GetTickCount64();
            ApplyPause(IsPausedNow(NowUnixMs()), now);   // also notices a Settings pause expiring
            if (now - lastSweep >= kSweepMs)
            {
                lastSweep = now;
                Sweep(reader);
            }
            if (have)
            {
                if (req.kind == Kind::Probe)
                    ProcessProbe(reader, uiaReady, req);
                else
                    ProcessCapture(reader, uiaReady, std::move(req));
            }
        }
        catch (const std::exception& e)
        {
            WARP_LOG_ERROR("web", "web capture request failed: %s", e.what());
        }
        catch (...)
        {
            WARP_LOG_ERROR("web", "web capture request failed: unknown exception");
        }
    }
}

// Drops state of destroyed windows (and recycled HWNDs) plus their cached UIA elements.
void WebCapture::Impl::Sweep(AddressBarReader& reader)
{
    std::vector<HWND> dead;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto it = m_windows.begin(); it != m_windows.end();)
        {
            DWORD pid = 0;
            const bool alive = IsWindow(it->first) && GetWindowThreadProcessId(it->first, &pid) != 0 &&
                               pid == it->second.pid;
            if (alive)
            {
                ++it;
                continue;
            }
            dead.push_back(it->first);
            if (m_fgRoot == it->first)
                m_fgRoot = nullptr;
            it = m_windows.erase(it);
        }
    }
    for (HWND h : dead)
        reader.Forget(h);
    reader.ForgetDeadWindows();
}

void WebCapture::Impl::ProcessProbe(AddressBarReader& reader, bool uiaReady, const Request& req)
{
    if (!uiaReady)
    {
        SetVerdict(req.exePath, false, false);
        return;
    }
    if (!IsWindow(req.root))
    {
        ForgetVerdict(req.exePath);
        return;
    }
    const AddressBarReader::Result r = reader.Probe(req.root, &m_cancel);
    CountUia(r);
    if (r.status == AddressBarReader::Status::Cancelled)
    {
        ForgetVerdict(req.exePath);
        return;
    }
    if (r.foundAddressBar)
    {
        SetVerdict(req.exePath, true, false);
        Log::WriteW(Log::Level::Info, "web", L"browser recognized by its address bar: " + req.exePath);
        PostToUi(kMsgReevaluate);                       // start capturing if it is still the foreground window
    }
    else if (r.status == AddressBarReader::Status::Ok)
    {
        SetVerdict(req.exePath, false, false);          // Electron / CEF app without an omnibox
    }
    else
    {
        SetVerdict(req.exePath, false, true);           // timeout / failure: retried on a later foreground entry
    }
}

void WebCapture::Impl::CountUia(const AddressBarReader::Result& r)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (r.status == AddressBarReader::Status::Cancelled)
        return;
    ++m_stats.uiaRequests;
    if (r.status == AddressBarReader::Status::Timeout)
        ++m_stats.uiaTimeouts;
    else if (r.status == AddressBarReader::Status::Failed || r.status == AddressBarReader::Status::Unavailable)
        ++m_stats.uiaFailures;
}

bool WebCapture::Impl::HasPending(HWND root) const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_requests.count(root) != 0;
}

// Re-queues a request unless a newer one for the same window already exists (latest wins).
void WebCapture::Impl::Requeue(Request req, ULONGLONG delayMs)
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_stop || m_pausedLatched || m_requests.count(req.root))
            return;
        const ULONGLONG now = GetTickCount64();
        req.firstTick = now;
        req.dueTick = now + delayMs;
        const HWND root = req.root;
        m_requests[root] = std::move(req);
        TrimRequestsLocked();
    }
    m_cv.notify_one();
}

// The request was dropped without a verdict: let the next title event re-queue the same title (WEB-7).
void WebCapture::Impl::ResetQueued(HWND root, const std::wstring& normTitle)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    if (m_requests.count(root))
        return;
    auto it = m_windows.find(root);
    if (it != m_windows.end() && it->second.lastQueuedTitle == normTitle)
        it->second.queuedOnce = false;
}

void WebCapture::Impl::RecordUiaVerdict(HWND root, int verdict, bool incomplete)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_windows.find(root);
    if (it == m_windows.end())
        return;
    WindowState& ws = it->second;
    if (verdict == 1)
        ws.uiaPrivate = 1;                              // sticky: a window never stops being private
    else if (verdict == 0 && ws.uiaPrivate != 1)
        ws.uiaPrivate = 0;
    else if (verdict < 0 && incomplete)
        ++ws.incompleteWalks;
}

void WebCapture::Impl::ProcessCapture(AddressBarReader& reader, bool uiaReady, Request req)
{
    {
        DWORD pid = 0;
        if (!IsWindow(req.root) || GetWindowThreadProcessId(req.root, &pid) == 0 || pid != req.pid)
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_windows.erase(req.root);
            reader.Forget(req.root);
            return;
        }
    }
    if (IsPausedNow(NowUnixMs()))
    {
        ResetQueued(req.root, req.normTitle);
        return;
    }
    const WarpSettings settings = Settings::Instance().Get();
    m_dropPrivate.store(settings.dropPrivateBrowsing);
    if (Settings::Instance().IsExcludedApp(req.exePath))
    {
        MarkUnrecorded(req.root, Unrecorded::Excluded, req.observedMs, req.observedTick);
        return;
    }

    bool quickPrivate = false;
    int uiaPrivate = -1, incompleteWalks = 0;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_windows.find(req.root);
        if (it == m_windows.end())
            return;
        quickPrivate = it->second.quickPrivate;
        uiaPrivate = it->second.uiaPrivate;
        incompleteWalks = it->second.incompleteWalks;
    }
    const bool wantUrl = settings.captureWeb;
    bool checkPrivate = !quickPrivate && uiaPrivate < 0;
    if (!wantUrl && !checkPrivate)
        return;                                         // capture off and the window's privacy is already known

    // ---- UI Automation read with title verification (fast tab-switch protection, WEB-4) ----
    std::wstring title = req.rawTitle;
    int64_t observedMs = req.observedMs;
    ULONGLONG observedTick = req.observedTick;
    AddressBarReader::Result rr;
    rr.status = AddressBarReader::Status::Unavailable;
    bool verified = false;
    if (uiaReady)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            rr = reader.Read(req.root, checkPrivate, &m_cancel);
            CountUia(rr);
            if (rr.status == AddressBarReader::Status::Cancelled)
                return;
            if (rr.status == AddressBarReader::Status::NoWindow)
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_windows.erase(req.root);
                return;
            }
            if (checkPrivate)
            {
                const bool incomplete = rr.privateVerdict < 0 && rr.status == AddressBarReader::Status::Ok;
                RecordUiaVerdict(req.root, rr.privateVerdict, incomplete);
                if (rr.privateVerdict >= 0)
                {
                    uiaPrivate = rr.privateVerdict;
                    checkPrivate = false;               // window-level verdict: valid for any later title
                }
                else if (incomplete)
                {
                    ++incompleteWalks;
                }
            }
            const std::wstring current = WindowText(req.root);
            if (WebCapture::TitleDedupKey(WebCapture::NormalizeTitle(current, req.browser)) ==
                WebCapture::TitleDedupKey(WebCapture::NormalizeTitle(title, req.browser)))
            {
                title = current;
                verified = true;
                break;
            }
            if (HasPending(req.root))
                return;                                 // a newer title change is queued and supersedes this one
            title = current;                            // retry once with the new title
            observedMs = NowUnixMs();
            observedTick = GetTickCount64();
        }
    }

    // ---- privacy (WEB-1) ----
    if (WebCapture::TitleMarksPrivate(title))
    {
        quickPrivate = true;
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_windows.find(req.root);
        if (it != m_windows.end())
            it->second.quickPrivate = true;
    }
    const bool isPrivate = quickPrivate || uiaPrivate == 1;
    // An unverifiable window is treated as private (fail closed), except that after repeated walks that
    // completed without a timeout but could not cover the whole frame, the frame is accepted as normal.
    const bool privacyKnown = isPrivate || uiaPrivate == 0 ||
                              (uiaReady && incompleteWalks >= kMaxIncompleteWalks && rr.status == AddressBarReader::Status::Ok);
    if (!wantUrl)
        return;
    if (isPrivate && settings.dropPrivateBrowsing)
    {
        MarkUnrecorded(req.root, Unrecorded::Private, observedMs, observedTick);
        return;
    }
    if (!privacyKnown && settings.dropPrivateBrowsing)
    {
        if (req.failRetries < kMaxFailRetries)
        {
            ++req.failRetries;
            Requeue(std::move(req), kFailRetryDelayMs);
        }
        else
        {
            ResetQueued(req.root, req.normTitle);
        }
        return;
    }

    // ---- title (WEB-7) and exclusions (WEB-2) ----
    const std::wstring display = WebCapture::NormalizeTitle(title, req.browser);
    if (Settings::Instance().IsExcludedTitle(title) || Settings::Instance().IsExcludedTitle(display))
    {
        MarkUnrecorded(req.root, Unrecorded::Excluded, observedMs, observedTick);
        return;
    }

    std::wstring url, host, docId, docKind;
    bool lossy = false;
    if (verified && rr.foundAddressBar && !rr.value.empty())
    {
        if (rr.valueHasFocus)
        {
            // The omnibox has keyboard focus: its text may be the user's typing, not the page URL.
            if (HasPending(req.root))
                return;                                 // a newer request supersedes this one
            if (!req.focusRetried)
            {
                req.focusRetried = true;                // look again shortly; afterwards record title-only
                req.observedMs = observedMs;
                req.observedTick = observedTick;
                req.rawTitle = title;
                Requeue(std::move(req), kFocusRetryDelayMs);
                return;
            }
        }
        else
        {
            const UrlCanon::Result cr = UrlCanon::Canonicalize(rr.value, settings.excludedDomains, settings.urlQueryAllowlist);
            if (cr.excluded)
            {
                MarkUnrecorded(req.root, Unrecorded::Excluded, observedMs, observedTick);
                return;
            }
            if (!cr.canonical.empty())
            {
                url = cr.canonical;
                host = cr.host;
                docId = cr.docId;                       // WEB-8: document identity passed through
                docKind = cr.docKind;
                lossy = cr.hadQuery || url.find(L":token") != std::wstring::npos;
            }
        }
    }
    if (WebCapture::IsPlaceholderTitle(display) || IsPlaceholderUrl(url))
    {
        MarkUnrecorded(req.root, Unrecorded::Placeholder, observedMs, observedTick);
        return;
    }
    if (IsPausedNow(NowUnixMs()))
    {
        ResetQueued(req.root, req.normTitle);
        return;
    }

    // ---- de-dup, dwell, emit ----
    BrowseEventRecord rec;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_windows.find(req.root);
        if (it == m_windows.end())
            return;
        WindowState& ws = it->second;
        const std::wstring key = WebCapture::TitleDedupKey(display);
        bool samePage = false;
        if (ws.hasEmitted && ws.pageOpen)
        {
            if (!url.empty() && !ws.lastEmitUrl.empty())
                samePage = url == ws.lastEmitUrl && (!lossy || key == ws.lastEmitKey);
            else
                samePage = key == ws.lastEmitKey;
        }
        ws.hasPage = true;
        if (samePage)
        {
            // Title churn on the same page (counters, timers, typing indicators): refresh, do not emit.
            ws.page.title = display;
            if (ws.page.url.empty() && !url.empty())
            {
                ws.page.url = url;
                ws.page.host = host;
                ws.page.docId = docId;
                ws.page.docKind = docKind;
            }
            return;
        }

        const int64_t f = ws.clock.At(observedTick);
        const int64_t dwellMs = ws.pageOpen ? f - ws.pageStartF : ws.pendingDwellMs;
        ws.pendingDwellMs = 0;
        ws.pageOpen = true;
        ws.pageStartF = f;
        ws.hasEmitted = true;
        ws.lastEmitUrl = url;
        ws.lastEmitKey = key;

        ws.page = PageInfo();
        ws.page.browser = req.browser;
        ws.page.title = display;
        ws.page.url = url;
        ws.page.host = host;
        ws.page.docId = docId;
        ws.page.docKind = docKind;
        ws.page.isPrivate = isPrivate;
        ws.page.sinceMs = observedMs;
        ws.page.pid = req.pid;

        rec.eventMs = observedMs;
        rec.browser = req.browser;
        rec.title = display;
        rec.url = url;
        rec.host = host;
        rec.docId = docId;
        rec.docKind = docKind;
        rec.dwellSecs = static_cast<int>((std::min)((std::max)(dwellMs, static_cast<int64_t>(0)) / 1000,
                                                    static_cast<int64_t>(INT_MAX)));
        rec.windowId = static_cast<int64_t>(reinterpret_cast<intptr_t>(req.root));
        ++m_stats.pages;
        m_stats.lastEventMs = observedMs;
    }
    rec.ctx = EventContextUtil::CaptureContext(req.pid);
    rec.ctx.confidence = EventContextUtil::ScoreEvent(rec.ctx, EventContextUtil::EventKind::Browse,
                                                      EventContextUtil::SourceClass::Uia);
    Emit(std::move(rec));
}

// A page that is not recorded (placeholder, private, excluded) closes the dwell of the previous recorded
// page; the window's PageInfo then carries only the browser and the flags.
void WebCapture::Impl::MarkUnrecorded(HWND root, Unrecorded why, int64_t observedMs, ULONGLONG observedTick)
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_windows.find(root);
    if (it == m_windows.end())
        return;
    WindowState& ws = it->second;
    const int64_t f = ws.clock.At(observedTick);
    if (ws.pageOpen)
    {
        ws.pendingDwellMs = (std::max)(f - ws.pageStartF, static_cast<int64_t>(0));
        ws.pageOpen = false;
    }
    ws.pageStartF = f;
    ws.hasPage = true;
    ws.page = PageInfo();
    ws.page.browser = ws.browser;
    ws.page.pid = ws.pid;
    ws.page.sinceMs = observedMs;
    ws.page.isPrivate = why == Unrecorded::Private || ws.IsPrivate();
    ws.page.excluded = why == Unrecorded::Excluded;
    if (why == Unrecorded::Private)
        ++m_stats.privateDropped;
    else if (why == Unrecorded::Excluded)
        ++m_stats.excludedDropped;
}

void WebCapture::Impl::Emit(BrowseEventRecord&& rec)
{
    Callback cb;
    {
        std::lock_guard<std::mutex> lk(m_cbMtx);
        cb = m_callback;
    }
    if (!cb)
        return;
    try
    {
        cb(std::move(rec));
    }
    catch (const std::exception& e)
    {
        WARP_LOG_ERROR("web", "browse callback failed: %s", e.what());
    }
    catch (...)
    {
        WARP_LOG_ERROR("web", "browse callback failed: unknown exception");
    }
}

// ---- queries (any thread) --------------------------------------------------------

bool WebCapture::Impl::LookupPage(HWND root, PageInfo& out) const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    auto it = m_windows.find(root);
    if (it == m_windows.end())
        return false;
    const WindowState& ws = it->second;
    const bool isPrivate = ws.IsPrivate();
    if (!ws.hasPage)
    {
        if (!isPrivate && !ws.appExcluded)
            return false;                               // nothing seen yet
        out = PageInfo();
        out.browser = ws.browser;
        out.pid = ws.pid;
        out.isPrivate = isPrivate;
        out.excluded = ws.appExcluded;
        return true;
    }
    out = ws.page;
    out.isPrivate = out.isPrivate || isPrivate;
    if ((out.isPrivate && m_dropPrivate.load()) || out.excluded)
    {
        out.title.clear();                              // never hand out text of redacted pages
        out.url.clear();
        out.host.clear();
        out.docId.clear();
        out.docKind.clear();
    }
    return true;
}

bool WebCapture::Impl::IsPrivateWindow(HWND root) const
{
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        auto it = m_windows.find(root);
        if (it != m_windows.end())
            return it->second.IsPrivate();
    }
    // Not tracked yet (e.g. asked before our own foreground handler ran): quick heuristics only.
    if (!root || !IsWindow(root))
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(root, &pid);
    if (pid == 0 || pid == GetCurrentProcessId())
        return false;
    const std::wstring exePath = EventContextUtil::GetExePathByPid(pid);
    bool alwaysPrivate = false;
    if (exePath.empty() ||
        !(IdentifyBrowserExe(exePath, nullptr, &alwaysPrivate) || LearnedBrowser(exePath, nullptr)))
        return false;
    if (alwaysPrivate || WebCapture::TitleMarksPrivate(WindowText(root)))
        return true;
    return CommandLinePrivate(pid, exePath);
}

WebCapture::Stats WebCapture::Impl::GetStats() const
{
    std::lock_guard<std::mutex> lk(m_mtx);
    return m_stats;
}

// ---------------------------------------------------------------------------
// WebCapture
// ---------------------------------------------------------------------------

WebCapture::WebCapture() : m_impl(new Impl())
{
}

WebCapture::~WebCapture()
{
    m_impl.reset();                                     // Impl::~Impl stops capture and joins the worker
}

void WebCapture::SetCallback(Callback cb)
{
    m_impl->SetCallback(std::move(cb));
}

void WebCapture::Start()
{
    m_impl->Start();
}

void WebCapture::Stop()
{
    m_impl->Stop();
}

void WebCapture::SetPaused(bool paused)
{
    m_impl->SetPaused(paused);
}

void WebCapture::Heartbeat()
{
    m_impl->Heartbeat();
}

bool WebCapture::LookupPage(HWND root, PageInfo& out) const
{
    return m_impl->LookupPage(root, out);
}

bool WebCapture::IsPrivateWindow(HWND root) const
{
    return m_impl->IsPrivateWindow(root);
}

WebCapture::Stats WebCapture::GetStats() const
{
    return m_impl->GetStats();
}
