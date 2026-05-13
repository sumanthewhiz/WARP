#include "framework.h"
#include "QueryApi.h"
#include "InferenceEngine.h"
#include "ContextInference.h"
#include <sstream>
#include <string>
#include <vector>
#include <ctime>

QueryApi::QueryApi()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

QueryApi::~QueryApi()
{
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

void QueryApi::Start(ActivityDatabase* db, InferenceEngine* inference, ContextInference* ctxInf)
{
    if (m_running) return;
    m_db = db;
    m_inference = inference;
    m_ctxInf = ctxInf;
    m_running = true;
    ResetEvent(m_stopEvent);
    m_thread = std::thread(&QueryApi::Run, this);
}

void QueryApi::Stop()
{
    if (!m_running) return;
    m_running = false;
    SetEvent(m_stopEvent);

    // Create a dummy connection to unblock ConnectNamedPipe
    HANDLE hDummy = CreateFileW(L"\\\\.\\pipe\\WarpFileActivityAPI",
                                GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (hDummy != INVALID_HANDLE_VALUE)
        CloseHandle(hDummy);

    if (m_thread.joinable()) m_thread.join();
}

void QueryApi::Run()
{
    while (m_running)
    {
        HANDLE hPipe = CreateNamedPipeW(
            L"\\\\.\\pipe\\WarpFileActivityAPI",
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536, 65536, 0, nullptr);

        if (hPipe == INVALID_HANDLE_VALUE)
        {
            Sleep(1000);
            continue;
        }

        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ConnectNamedPipe(hPipe, &ov);

        HANDLE waitHandles[2] = { ov.hEvent, m_stopEvent };
        DWORD result = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

        CloseHandle(ov.hEvent);

        if (result == WAIT_OBJECT_0 + 1 || !m_running)
        {
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            break;
        }

        std::thread([this, hPipe]() {
            HandleClient(hPipe);
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }).detach();
    }
}

std::string QueryApi::WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

std::string QueryApi::EscapeJson(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}

void QueryApi::HandleClient(HANDLE hPipe)
{
    char buf[4096] = {};
    DWORD bytesRead = 0;
    if (!ReadFile(hPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr))
        return;

    std::string request(buf, bytesRead);

    // Simple JSON value extractor
    auto findValue = [&](const std::string& key) -> std::string {
        auto pos = request.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = request.find(':', pos);
        if (pos == std::string::npos) return "";
        pos++;
        while (pos < request.size() && (request[pos] == ' ' || request[pos] == '"'))
            pos++;
        size_t end = pos;
        while (end < request.size() && request[end] != '"' && request[end] != '}'
               && request[end] != ',' && request[end] != ' ' && request[end] != ']')
            end++;
        return request.substr(pos, end - pos);
    };

    // Extract strings from a JSON array value, e.g. ["a","b","c"]
    auto extractStringArray = [&](const std::string& key) -> std::vector<std::string> {
        std::vector<std::string> result;
        auto keyPos = request.find("\"" + key + "\"");
        if (keyPos == std::string::npos) return result;
        auto arrStart = request.find('[', keyPos);
        auto arrEnd = request.find(']', arrStart != std::string::npos ? arrStart : 0);
        if (arrStart == std::string::npos || arrEnd == std::string::npos) return result;
        std::string arr = request.substr(arrStart + 1, arrEnd - arrStart - 1);
        // Parse quoted strings out of arr
        size_t p = 0;
        while (p < arr.size())
        {
            auto q1 = arr.find('"', p);
            if (q1 == std::string::npos) break;
            auto q2 = arr.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            result.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
            p = q2 + 1;
        }
        return result;
    };

    // --- Check for inference operations ---
    std::string opVal = findValue("op");

    if (opVal == "QueryInferences" && m_inference)
    {
        auto paths  = extractStringArray("paths");
        auto fields = extractStringArray("fields");
        std::string json = m_inference->HandleQueryInferences(paths, fields);
        DWORD written = 0;
        WriteFile(hPipe, json.c_str(), static_cast<DWORD>(json.size()), &written, nullptr);
        FlushFileBuffers(hPipe);
        return;
    }

    if (opVal == "GetRecentContext" && m_ctxInf)
    {
        std::string category = findValue("category");
        std::string winStr   = findValue("window_seconds");
        if (winStr.empty()) winStr = findValue("window_secs");
        int64_t winSecs = 0;
        if (!winStr.empty())
            winSecs = static_cast<int64_t>(strtoll(winStr.c_str(), nullptr, 10));
        std::string json = m_ctxInf->GetRecentContext(category, winSecs);
        DWORD written = 0;
        WriteFile(hPipe, json.c_str(), static_cast<DWORD>(json.size()), &written, nullptr);
        FlushFileBuffers(hPipe);
        return;
    }

    if (opVal == "GetRecentContexts" && m_ctxInf)
    {
        std::string countStr = findValue("count");
        int count = 10;
        if (!countStr.empty())
            count = static_cast<int>(strtol(countStr.c_str(), nullptr, 10));
        std::string category = findValue("category");
        std::string winStr   = findValue("window_seconds");
        if (winStr.empty()) winStr = findValue("window_secs");
        int64_t winSecs = 0;
        if (!winStr.empty())
            winSecs = static_cast<int64_t>(strtoll(winStr.c_str(), nullptr, 10));
        std::string json = m_ctxInf->GetRecentContexts(count, category, winSecs);
        DWORD written = 0;
        WriteFile(hPipe, json.c_str(), static_cast<DWORD>(json.size()), &written, nullptr);
        FlushFileBuffers(hPipe);
        return;
    }

    if (opVal == "GetInferenceDeltas" && m_inference)
    {
        std::string sinceStr = findValue("since_version");
        uint32_t sinceVer = 0;
        if (!sinceStr.empty())
            sinceVer = static_cast<uint32_t>(strtoul(sinceStr.c_str(), nullptr, 10));
        std::string json = m_inference->HandleGetInferenceDeltas(sinceVer);
        DWORD written = 0;
        WriteFile(hPipe, json.c_str(), static_cast<DWORD>(json.size()), &written, nullptr);
        FlushFileBuffers(hPipe);
        return;
    }

    // --- Existing event-query logic ---

    // Parse event types from "types" array if present
    uint32_t eventTypes = 0;
    {
        auto typesPos = request.find("\"types\"");
        if (typesPos != std::string::npos)
        {
            auto arrStart = request.find('[', typesPos);
            auto arrEnd = request.find(']', arrStart != std::string::npos ? arrStart : 0);
            if (arrStart != std::string::npos && arrEnd != std::string::npos)
            {
                std::string arr = request.substr(arrStart, arrEnd - arrStart + 1);
                if (arr.find("file") != std::string::npos)
                    eventTypes |= EVENT_TYPE_FILE;
                if (arr.find("app_launch") != std::string::npos)
                    eventTypes |= EVENT_TYPE_APP_LAUNCH;
                if (arr.find("app_focus") != std::string::npos)
                    eventTypes |= EVENT_TYPE_APP_FOCUS;
                if (arr.find("browsing") != std::string::npos)
                    eventTypes |= EVENT_TYPE_BROWSING;
            }
        }

        // Default: all types if none specified
        if (eventTypes == 0)
            eventTypes = EVENT_TYPE_ALL;
    }

    // Parse time window
    std::string windowVal = findValue("window");
    std::string secondsVal = findValue("seconds");

    int64_t secs = 3600; // default: 1 hour
    if (!windowVal.empty())
    {
        if (windowVal == "15m")       secs = 15LL * 60;
        else if (windowVal == "30m")  secs = 30LL * 60;
        else if (windowVal == "1h")   secs = 3600LL;
        else if (windowVal == "2h")   secs = 2LL * 3600;
        else if (windowVal == "6h")   secs = 6LL * 3600;
        else if (windowVal == "24h")  secs = 24LL * 3600;
        else if (windowVal == "7d")   secs = 7LL * 24 * 3600;
        else if (windowVal == "15d")  secs = 15LL * 24 * 3600;
        else if (windowVal == "30d")  secs = 30LL * 24 * 3600;
    }
    else if (!secondsVal.empty())
    {
        int64_t parsed = _atoi64(secondsVal.c_str());
        if (parsed > 0) secs = parsed;
    }

    std::string json = BuildJsonResponse(eventTypes, secs);
    DWORD written = 0;
    WriteFile(hPipe, json.c_str(), static_cast<DWORD>(json.size()), &written, nullptr);
    FlushFileBuffers(hPipe);
}

std::string QueryApi::BuildJsonResponse(uint32_t eventTypes, int64_t seconds)
{
    std::ostringstream oss;
    oss << "{";

    bool firstSection = true;

    // File activities
    if (eventTypes & EVENT_TYPE_FILE)
    {
        auto files = m_db->QueryFilesCustomSeconds(seconds);
        if (!firstSection) oss << ",";
        firstSection = false;

        oss << "\"file_activities\":{\"count\":" << files.size() << ",\"events\":[";
        for (size_t i = 0; i < files.size(); ++i)
        {
            const auto& a = files[i];
            if (i > 0) oss << ",";
            oss << "{\"id\":" << a.id
                << ",\"timestamp\":" << a.timestampUtc
                << ",\"action\":\"" << EscapeJson(WideToUtf8(a.action)) << "\""
                << ",\"path\":\"" << EscapeJson(WideToUtf8(a.path)) << "\"";
            if (!a.oldPath.empty())
                oss << ",\"old_path\":\"" << EscapeJson(WideToUtf8(a.oldPath)) << "\"";
            oss << "}";
        }
        oss << "]}";
    }

    // App launch activities
    if (eventTypes & EVENT_TYPE_APP_LAUNCH)
    {
        auto apps = m_db->QueryAppLaunchesCustomSeconds(seconds);
        if (!firstSection) oss << ",";
        firstSection = false;

        oss << "\"app_launch_activities\":{\"count\":" << apps.size() << ",\"events\":[";
        for (size_t i = 0; i < apps.size(); ++i)
        {
            const auto& a = apps[i];
            if (i > 0) oss << ",";
            oss << "{\"id\":" << a.id
                << ",\"timestamp\":" << a.timestampUtc
                << ",\"exe_name\":\"" << EscapeJson(WideToUtf8(a.exeName)) << "\""
                << ",\"exe_path\":\"" << EscapeJson(WideToUtf8(a.exePath)) << "\""
                << ",\"pid\":" << a.pid
                << "}";
        }
        oss << "]}";
    }

    // Browsing activities
    if (eventTypes & EVENT_TYPE_BROWSING)
    {
        auto browse = m_db->QueryBrowsingCustomSeconds(seconds);
        if (!firstSection) oss << ",";
        firstSection = false;

        oss << "\"browsing_activities\":{\"count\":" << browse.size() << ",\"events\":[";
        for (size_t i = 0; i < browse.size(); ++i)
        {
            const auto& b = browse[i];
            if (i > 0) oss << ",";
            oss << "{\"id\":" << b.id
                << ",\"timestamp\":" << b.timestampUtc
                << ",\"browser\":\"" << EscapeJson(WideToUtf8(b.browser)) << "\""
                << ",\"title\":\"" << EscapeJson(WideToUtf8(b.title)) << "\"";
            if (!b.url.empty())
                oss << ",\"url\":\"" << EscapeJson(WideToUtf8(b.url)) << "\"";
            oss << "}";
        }
        oss << "]}";
    }

    // App focus activities
    if (eventTypes & EVENT_TYPE_APP_FOCUS)
    {
        auto focus = m_db->QueryAppFocusCustomSeconds(seconds);
        if (!firstSection) oss << ",";
        firstSection = false;

        oss << "\"app_focus_activities\":{\"count\":" << focus.size() << ",\"events\":[";
        for (size_t i = 0; i < focus.size(); ++i)
        {
            const auto& f = focus[i];
            if (i > 0) oss << ",";
            oss << "{\"id\":" << f.id
                << ",\"timestamp\":" << f.timestampUtc
                << ",\"exe_name\":\"" << EscapeJson(WideToUtf8(f.exeName)) << "\""
                << ",\"exe_path\":\"" << EscapeJson(WideToUtf8(f.exePath)) << "\""
                << ",\"window_title\":\"" << EscapeJson(WideToUtf8(f.windowTitle)) << "\""
                << ",\"duration_secs\":" << f.durationSecs
                << "}";
        }
        oss << "]}";
    }

    oss << "}";
    return oss.str();
}
