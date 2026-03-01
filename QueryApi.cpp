#include "framework.h"
#include "QueryApi.h"
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

void QueryApi::Start(ActivityDatabase* db)
{
    if (m_running) return;
    m_db = db;
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

        // Handle client in-line (pipe instances handle concurrency)
        std::thread([this, hPipe]() {
            HandleClient(hPipe);
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
        }).detach();
    }
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

static std::string EscapeJson(const std::string& s)
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

    // Simple JSON parsing – look for "window" or "seconds"
    std::vector<FileActivity> results;

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
               && request[end] != ',' && request[end] != ' ')
            end++;
        return request.substr(pos, end - pos);
    };

    std::string windowVal = findValue("window");
    std::string secondsVal = findValue("seconds");

    if (!windowVal.empty())
    {
        TimeWindow tw = TimeWindow::Hours1;
        if (windowVal == "15m")  tw = TimeWindow::Minutes15;
        else if (windowVal == "30m")  tw = TimeWindow::Minutes30;
        else if (windowVal == "1h")   tw = TimeWindow::Hours1;
        else if (windowVal == "2h")   tw = TimeWindow::Hours2;
        else if (windowVal == "6h")   tw = TimeWindow::Hours6;
        else if (windowVal == "24h")  tw = TimeWindow::Hours24;
        else if (windowVal == "7d")   tw = TimeWindow::Days7;
        else if (windowVal == "15d")  tw = TimeWindow::Days15;
        else if (windowVal == "30d")  tw = TimeWindow::Days30;

        results = m_db->Query(tw);
    }
    else if (!secondsVal.empty())
    {
        int64_t secs = _atoi64(secondsVal.c_str());
        if (secs > 0)
            results = m_db->QueryCustomSeconds(secs);
    }
    else
    {
        // Default: last 1 hour
        results = m_db->Query(TimeWindow::Hours1);
    }

    std::string json = BuildJsonResponse(results);
    DWORD written = 0;
    WriteFile(hPipe, json.c_str(), static_cast<DWORD>(json.size()), &written, nullptr);
    FlushFileBuffers(hPipe);
}

std::string QueryApi::BuildJsonResponse(const std::vector<FileActivity>& activities)
{
    std::ostringstream oss;
    oss << "{\"count\":" << activities.size() << ",\"activities\":[";

    for (size_t i = 0; i < activities.size(); ++i)
    {
        const auto& a = activities[i];
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
    return oss.str();
}
