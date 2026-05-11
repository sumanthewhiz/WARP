#include "framework.h"
#include "BrowsingMonitor.h"
#include "ForegroundChangeBroker.h"
#include "UrlExtractor.h"
#include "EventContext.h"
#include <algorithm>
#include <psapi.h>

// Detect browser by process name; returns empty string if not a browser
static std::wstring IdentifyBrowser(DWORD pid)
{
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return L"";

    wchar_t exePath[MAX_PATH] = {};
    DWORD exeLen = MAX_PATH;
    if (!QueryFullProcessImageNameW(hProc, 0, exePath, &exeLen) || exeLen == 0)
    {
        CloseHandle(hProc);
        return L"";
    }
    CloseHandle(hProc);

    std::wstring exe(exePath, exeLen);
    size_t slash = exe.rfind(L'\\');
    if (slash != std::wstring::npos)
        exe = exe.substr(slash + 1);
    std::transform(exe.begin(), exe.end(), exe.begin(), ::towlower);

    if (exe == L"chrome.exe")       return L"chrome";
    if (exe == L"msedge.exe")       return L"msedge";
    if (exe == L"firefox.exe")      return L"firefox";
    if (exe == L"brave.exe")        return L"brave";
    if (exe == L"opera.exe")        return L"opera";
    if (exe == L"vivaldi.exe")      return L"vivaldi";
    if (exe == L"iexplore.exe")     return L"ie";
    if (exe == L"applicationframehost.exe") return L"msedge"; // UWP Edge

    return L"";
}

// Attempt to extract URL from the title bar.
// Browsers typically show: "Page Title - Browser Name" or "Page Title"
// Some configurations show URL in title.
static std::wstring ExtractTitleAndUrl(const std::wstring& rawTitle,
                                       const std::wstring& browser,
                                       std::wstring& outUrl)
{
    outUrl.clear();
    std::wstring title = rawTitle;

    // Strip common browser suffixes
    static const wchar_t* const suffixes[] = {
        L" - Google Chrome", L" - Chrome",
        L" - Microsoft Edge", L" - Edge",
        L" - Mozilla Firefox", L" - Firefox",
        L" - Brave", L" - Opera", L" - Vivaldi",
        L" - Internet Explorer",
    };

    for (const auto* suffix : suffixes)
    {
        size_t suffixLen = wcslen(suffix);
        if (title.size() > suffixLen)
        {
            std::wstring titleLower = title;
            std::wstring suffixLower(suffix);
            std::transform(titleLower.begin(), titleLower.end(), titleLower.begin(), ::towlower);
            std::transform(suffixLower.begin(), suffixLower.end(), suffixLower.begin(), ::towlower);

            if (titleLower.size() >= suffixLower.size() &&
                titleLower.compare(titleLower.size() - suffixLower.size(),
                                   suffixLower.size(), suffixLower) == 0)
            {
                title = title.substr(0, title.size() - suffixLen);
                break;
            }
        }
    }

    // Check if title looks like a URL
    if (title.size() > 8)
    {
        std::wstring lower = title;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        if (lower.substr(0, 8) == L"https://" || lower.substr(0, 7) == L"http://")
        {
            outUrl = title;
        }
    }

    return title;
}

// ---------------------------------------------------------------------------
// Singleton bridge from the per-PID NAMECHANGE hook (a free-function
// callback) back into the BrowsingMonitor instance. Only one BrowsingMonitor
// is expected; if that ever changes, this can become a hook->instance map.
// ---------------------------------------------------------------------------
static BrowsingMonitor* g_pBrowsingMonitor = nullptr;

BrowsingMonitor::BrowsingMonitor()
{
}

BrowsingMonitor::~BrowsingMonitor()
{
    Stop();
}

void BrowsingMonitor::SetCallback(BrowsingCallback cb)
{
    m_callback = std::move(cb);
}

void BrowsingMonitor::Start()
{
    if (m_running) return;
    m_running = true;
    g_pBrowsingMonitor = this;

    // The UrlExtractor worker thread does the cross-process UIA queries.
    // Forward its output to our user callback.
    UrlExtractor::Instance().SetEmitCallback(
        [this](const std::wstring& browser, const std::wstring& title,
               const std::wstring& url, const EventContext& ctx) {
            if (m_callback) m_callback(browser, title, url, ctx);
        });
    UrlExtractor::Instance().Start();

    m_brokerToken = ForegroundChangeBroker::Instance().Subscribe(
        [this](HWND hwnd, DWORD pid) { OnForegroundChanged(hwnd, pid); });
}

void BrowsingMonitor::Stop()
{
    if (!m_running) return;
    m_running = false;
    if (m_brokerToken)
    {
        ForegroundChangeBroker::Instance().Unsubscribe(m_brokerToken);
        m_brokerToken = 0;
    }
    UrlExtractor::Instance().Stop();
    {
        std::lock_guard<std::mutex> lk(m_stateMtx);
        if (m_nameHook)
        {
            UnhookWinEvent(m_nameHook);
            m_nameHook = nullptr;
        }
        m_currentBrowserPid  = 0;
        m_currentBrowserHwnd = nullptr;
        m_lastTitle.clear();
        m_lastBrowser.clear();
    }
    if (g_pBrowsingMonitor == this) g_pBrowsingMonitor = nullptr;
}

void BrowsingMonitor::Pause() { m_paused = true; }
void BrowsingMonitor::Resume() { m_paused = false; }

// Called by ForegroundChangeBroker on the main UI thread when the
// foreground window changes. We install / uninstall a per-PID NAMECHANGE
// hook so we get push notifications for in-window navigation -- without
// the global NAMECHANGE firehose (which fires on every UI text update on
// every window in every process).
void BrowsingMonitor::OnForegroundChanged(HWND hwnd, DWORD pid)
{
    if (!m_running) return;

    std::wstring browser = (pid != 0) ? IdentifyBrowser(pid) : std::wstring();
    bool wasBrowser, isBrowser = !browser.empty();
    DWORD oldPid;
    {
        std::lock_guard<std::mutex> lk(m_stateMtx);
        wasBrowser = (m_currentBrowserPid != 0);
        oldPid     = m_currentBrowserPid;

        if (wasBrowser && (!isBrowser || pid != oldPid))
        {
            // Foreground left the browser, or switched to a different
            // browser PID. Tear down the old NAMECHANGE hook.
            if (m_nameHook)
            {
                UnhookWinEvent(m_nameHook);
                m_nameHook = nullptr;
            }
            m_currentBrowserPid  = 0;
            m_currentBrowserHwnd = nullptr;
            m_lastTitle.clear();
            m_lastBrowser.clear();
        }

        if (isBrowser && (!wasBrowser || pid != oldPid))
        {
            // Foreground entered a (new) browser. Install per-PID
            // NAMECHANGE hook scoped to this PID -- much lower volume
            // than a global hook.
            DWORD threadId = GetWindowThreadProcessId(hwnd, nullptr);
            m_nameHook = SetWinEventHook(
                EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
                nullptr,
                &BrowsingMonitor::NameChangeProc,
                pid, threadId,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            m_currentBrowserPid  = pid;
            m_currentBrowserHwnd = hwnd;
        }
    }

    // Always evaluate the current title once when the foreground changes,
    // even if it's the same browser PID -- the user may have switched tabs
    // between windows.
    if (isBrowser)
        EvaluateBrowserState(hwnd, pid);
}

// Static member -- can be passed as a SetWinEventHook callback. Routes
// events back into the singleton BrowsingMonitor instance.
void CALLBACK BrowsingMonitor::NameChangeProc(
    HWINEVENTHOOK, DWORD event, HWND hwnd,
    LONG idObject, LONG idChild, DWORD, DWORD)
{
    if (event != EVENT_OBJECT_NAMECHANGE) return;
    if (idObject != OBJID_WINDOW)         return;
    if (idChild  != CHILDID_SELF)         return;
    if (!hwnd)                            return;
    if (!g_pBrowsingMonitor || !g_pBrowsingMonitor->m_running) return;
    if (g_pBrowsingMonitor->m_paused) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return;

    // Only react if this name change is on the current browser HWND --
    // browsers fire many NAMECHANGE events for sub-controls; we only
    // care about the top-level window's title.
    HWND currentHwnd;
    DWORD currentPid;
    {
        std::lock_guard<std::mutex> lk(g_pBrowsingMonitor->m_stateMtx);
        currentHwnd = g_pBrowsingMonitor->m_currentBrowserHwnd;
        currentPid  = g_pBrowsingMonitor->m_currentBrowserPid;
    }
    if (hwnd != currentHwnd || pid != currentPid) return;

    g_pBrowsingMonitor->EvaluateBrowserState(hwnd, pid);
}

// Read the current title from `hwnd`, deduplicate against the previous
// title, and submit a request to UrlExtractor. UrlExtractor reads the
// URL on a worker thread (UIA crosses process boundaries and can block)
// and then invokes m_callback.
void BrowsingMonitor::EvaluateBrowserState(HWND hwnd, DWORD pid)
{
    if (m_paused) return;

    std::wstring browser = IdentifyBrowser(pid);
    if (browser.empty()) return;

    wchar_t titleBuf[1024] = {};
    int titleLen = GetWindowTextW(hwnd, titleBuf, 1024);
    if (titleLen <= 0) return;
    std::wstring rawTitle(titleBuf, titleLen);

    if (rawTitle.empty() || rawTitle == L"New Tab" || rawTitle == L"Untitled")
        return;

    {
        std::lock_guard<std::mutex> lk(m_stateMtx);
        if (rawTitle == m_lastTitle && browser == m_lastBrowser) return;
        m_lastTitle   = rawTitle;
        m_lastBrowser = browser;
    }

    // Strip "- Browser" suffixes from the title for cleaner downstream
    // display, but do NOT try to parse a URL out of it -- UIA gives us
    // the actual address. Keep ExtractTitleAndUrl for backward-compat
    // title cleaning but ignore its URL output.
    std::wstring titleUrl;
    std::wstring pageTitle = ExtractTitleAndUrl(rawTitle, browser, titleUrl);
    if (pageTitle.empty()) return;

    EventContext ctx = EventContextUtil::CaptureContext(pid);

    // Submit to the UrlExtractor worker thread. The worker calls
    // m_callback (registered via UrlExtractor::SetEmitCallback in Start)
    // once UIA returns the address-bar value. If UIA fails or times out
    // due to a hung browser, the worker emits with url="" so we don't
    // lose the title-only signal.
    UrlExtractor::Instance().Submit(hwnd, browser, pageTitle, ctx);
}
