#include "framework.h"
#include "BrowsingMonitor.h"
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

BrowsingMonitor::BrowsingMonitor()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

BrowsingMonitor::~BrowsingMonitor()
{
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

void BrowsingMonitor::SetCallback(BrowsingCallback cb)
{
    m_callback = std::move(cb);
}

void BrowsingMonitor::Start()
{
    if (m_running) return;
    m_running = true;
    ResetEvent(m_stopEvent);
    m_thread = std::thread(&BrowsingMonitor::MonitorLoop, this);
}

void BrowsingMonitor::Stop()
{
    if (!m_running) return;
    m_running = false;
    SetEvent(m_stopEvent);
    if (m_thread.joinable()) m_thread.join();
}

void BrowsingMonitor::Pause()
{
    m_paused = true;
}

void BrowsingMonitor::Resume()
{
    m_paused = false;
}

void BrowsingMonitor::MonitorLoop()
{
    std::wstring lastTitle;
    std::wstring lastBrowser;

    while (m_running)
    {
        // Poll every 3 seconds
        DWORD waitResult = WaitForSingleObject(m_stopEvent, 3000);
        if (waitResult == WAIT_OBJECT_0 || !m_running)
            break;

        if (m_paused)
            continue;

        // Get the foreground window
        HWND hFg = GetForegroundWindow();
        if (!hFg)
            continue;

        DWORD fgPid = 0;
        GetWindowThreadProcessId(hFg, &fgPid);
        if (fgPid == 0)
            continue;

        std::wstring browser = IdentifyBrowser(fgPid);
        if (browser.empty())
        {
            // Foreground window is not a browser; reset tracking
            lastTitle.clear();
            lastBrowser.clear();
            continue;
        }

        // Get window title
        wchar_t titleBuf[1024] = {};
        int titleLen = GetWindowTextW(hFg, titleBuf, 1024);
        if (titleLen <= 0)
            continue;

        std::wstring rawTitle(titleBuf, titleLen);

        // Skip empty or generic titles
        if (rawTitle.empty() || rawTitle == L"New Tab" || rawTitle == L"Untitled")
            continue;

        // Deduplicate: only report when title actually changes
        if (rawTitle == lastTitle && browser == lastBrowser)
            continue;

        lastTitle = rawTitle;
        lastBrowser = browser;

        std::wstring url;
        std::wstring pageTitle = ExtractTitleAndUrl(rawTitle, browser, url);

        if (!pageTitle.empty() && m_callback)
        {
            // Browser navigation events are attributed to the foreground
            // browser PID. ms-since-input is critical here -- a title change
            // > 5s after the last user input is almost always a background
            // refresh / SPA timer rather than human navigation.
            EventContext ctx = EventContextUtil::CaptureContext(fgPid);
            m_callback(browser, pageTitle, url, ctx);
        }
    }
}
