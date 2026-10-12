#include "framework.h"
#include "PrintMonitor.h"

#include <winevt.h>

#include <cstdlib>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "wevtapi.lib")

namespace
{
    const wchar_t kChannel[] = L"Microsoft-Windows-PrintService/Operational";
    const wchar_t kQuery[]   = L"*[System[(EventID=307)]]";

    // True while a subscription callback runs on this thread: EvtClose must not be called then.
    thread_local bool t_inCallback = false;

    int64_t FileTimeToUnixMs(const FILETIME& ft)
    {
        const ULONGLONG ticks = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        return static_cast<int64_t>(ticks / 10000ULL) - 11644473600000LL;
    }

    int64_t NowUnixMs()
    {
        FILETIME ft = {};
        GetSystemTimeAsFileTime(&ft);
        return FileTimeToUnixMs(ft);
    }

    std::wstring WithError(const wchar_t* what, DWORD error)
    {
        return std::wstring(what) + L" (error " + std::to_wstring(error) + L")";
    }

    void AppendCodePoint(std::wstring& out, unsigned long cp)
    {
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        }
        else
        {
            out.push_back(static_cast<wchar_t>(cp));
        }
    }

    // Decodes &amp; &lt; &gt; &quot; &apos; and numeric character references.
    std::wstring XmlUnescape(const std::wstring& s)
    {
        std::wstring out;
        out.reserve(s.size());
        size_t i = 0;
        while (i < s.size())
        {
            const wchar_t c = s[i];
            const size_t semi = (c == L'&') ? s.find(L';', i + 1) : std::wstring::npos;
            if (semi == std::wstring::npos || semi - i > 10)
            {
                out.push_back(c);
                ++i;
                continue;
            }

            const std::wstring entity = s.substr(i + 1, semi - i - 1);
            bool decoded = true;
            if (entity == L"amp")
                out.push_back(L'&');
            else if (entity == L"lt")
                out.push_back(L'<');
            else if (entity == L"gt")
                out.push_back(L'>');
            else if (entity == L"quot")
                out.push_back(L'"');
            else if (entity == L"apos")
                out.push_back(L'\'');
            else if (entity.size() > 1 && entity[0] == L'#')
            {
                const bool hex = (entity[1] == L'x' || entity[1] == L'X');
                const wchar_t* digits = entity.c_str() + (hex ? 2 : 1);
                wchar_t* end = nullptr;
                const unsigned long cp = wcstoul(digits, &end, hex ? 16 : 10);
                decoded = *digits >= L'0' && end != nullptr && *end == L'\0' && cp != 0 && cp <= 0x10FFFF &&
                          !(cp >= 0xD800 && cp <= 0xDFFF);
                if (decoded)
                    AppendCodePoint(out, cp);
            }
            else
            {
                decoded = false;
            }

            if (decoded)
            {
                i = semi + 1;
            }
            else
            {
                out.push_back(c);   // not an entity we understand: keep the '&' literally
                ++i;
            }
        }
        return out;
    }

    // Unescaped text of the first <name>...</name> at or after 'from'; empty if absent or self-closing.
    std::wstring ElementText(const std::wstring& xml, size_t from, const wchar_t* name)
    {
        const std::wstring open = std::wstring(L"<") + name + L">";
        const size_t start = xml.find(open, from);
        if (start == std::wstring::npos)
            return std::wstring();
        const size_t valueStart = start + open.size();
        const std::wstring close = std::wstring(L"</") + name + L">";
        const size_t valueEnd = xml.find(close, valueStart);
        if (valueEnd == std::wstring::npos)
            return std::wstring();
        return XmlUnescape(xml.substr(valueStart, valueEnd - valueStart));
    }

    // Value of attribute 'attr' on the first <element ...> tag; EvtRender uses single quotes.
    std::wstring AttributeValue(const std::wstring& xml, const wchar_t* element, const wchar_t* attr)
    {
        const size_t tag = xml.find(std::wstring(L"<") + element);
        if (tag == std::wstring::npos)
            return std::wstring();
        const size_t tagEnd = xml.find(L'>', tag);
        const std::wstring key = std::wstring(attr) + L"=";
        size_t pos = xml.find(key, tag);
        if (pos == std::wstring::npos || (tagEnd != std::wstring::npos && pos > tagEnd))
            return std::wstring();
        pos += key.size();
        if (pos >= xml.size() || (xml[pos] != L'\'' && xml[pos] != L'"'))
            return std::wstring();
        const wchar_t quote = xml[pos];
        const size_t valueEnd = xml.find(quote, pos + 1);
        if (valueEnd == std::wstring::npos)
            return std::wstring();
        return XmlUnescape(xml.substr(pos + 1, valueEnd - pos - 1));
    }

    bool ParseDigits(const std::wstring& s, size_t pos, size_t count, int& value)
    {
        if (pos + count > s.size())
            return false;
        int v = 0;
        for (size_t i = pos; i < pos + count; ++i)
        {
            if (s[i] < L'0' || s[i] > L'9')
                return false;
            v = v * 10 + static_cast<int>(s[i] - L'0');
        }
        value = v;
        return true;
    }

    // "YYYY-MM-DDTHH:MM:SS[.fraction]Z" (UTC) -> Unix milliseconds.
    bool ParseUtcTimestamp(const std::wstring& s, int64_t& unixMs)
    {
        int year = 0;
        int month = 0;
        int day = 0;
        int hour = 0;
        int minute = 0;
        int second = 0;
        if (s.size() < 19 || s[4] != L'-' || s[7] != L'-' || s[10] != L'T' || s[13] != L':' || s[16] != L':' ||
            !ParseDigits(s, 0, 4, year) || !ParseDigits(s, 5, 2, month) || !ParseDigits(s, 8, 2, day) ||
            !ParseDigits(s, 11, 2, hour) || !ParseDigits(s, 14, 2, minute) || !ParseDigits(s, 17, 2, second))
            return false;

        int millis = 0;
        if (s.size() > 20 && s[19] == L'.')
        {
            int scale = 100;
            for (size_t i = 20; i < s.size() && scale > 0 && s[i] >= L'0' && s[i] <= L'9'; ++i)
            {
                millis += static_cast<int>(s[i] - L'0') * scale;
                scale /= 10;
            }
        }

        SYSTEMTIME st = {};
        st.wYear         = static_cast<WORD>(year);
        st.wMonth        = static_cast<WORD>(month);
        st.wDay          = static_cast<WORD>(day);
        st.wHour         = static_cast<WORD>(hour);
        st.wMinute       = static_cast<WORD>(minute);
        st.wSecond       = static_cast<WORD>(second);
        st.wMilliseconds = static_cast<WORD>(millis);
        FILETIME ft = {};
        if (!SystemTimeToFileTime(&st, &ft))
            return false;
        unixMs = FileTimeToUnixMs(ft);
        return true;
    }

    DWORD ParseDword(const std::wstring& s)
    {
        if (s.empty() || s[0] < L'0' || s[0] > L'9')
            return 0;
        wchar_t* end = nullptr;
        return static_cast<DWORD>(wcstoul(s.c_str(), &end, 10));
    }

    std::wstring RenderEventXml(EVT_HANDLE eventHandle)
    {
        DWORD used = 0;
        DWORD properties = 0;
        if (EvtRender(nullptr, eventHandle, EvtRenderEventXml, 0, nullptr, &used, &properties) ||
            GetLastError() != ERROR_INSUFFICIENT_BUFFER || used == 0)
            return std::wstring();

        std::vector<wchar_t> buffer(used / sizeof(wchar_t) + 1, L'\0');
        if (!EvtRender(nullptr, eventHandle, EvtRenderEventXml, static_cast<DWORD>(buffer.size() * sizeof(wchar_t)),
                       buffer.data(), &used, &properties))
            return std::wstring();
        buffer.back() = L'\0';
        return std::wstring(buffer.data());
    }

    // Makes sure the channel is enabled, enabling it when allowed. 'why' explains a false result.
    bool EnsureChannelEnabled(bool enableIfDisabled, std::wstring& why)
    {
        EVT_HANDLE config = EvtOpenChannelConfig(nullptr, kChannel, 0);
        if (!config)
        {
            const DWORD error = GetLastError();
            why = WithError(error == ERROR_EVT_CHANNEL_NOT_FOUND
                                ? L"PrintService/Operational channel not found (print spooler not installed?)"
                                : L"cannot open the PrintService/Operational channel configuration",
                            error);
            return false;
        }

        EVT_VARIANT value = {};
        DWORD used = 0;
        bool enabled = false;
        if (EvtGetChannelConfigProperty(config, EvtChannelConfigEnabled, 0, static_cast<DWORD>(sizeof(value)),
                                        &value, &used) &&
            value.Type == static_cast<DWORD>(EvtVarTypeBoolean))
        {
            enabled = value.BooleanVal != FALSE;
        }

        bool ok = enabled;
        if (!enabled)
        {
            if (!enableIfDisabled)
            {
                why = L"PrintService/Operational channel is disabled (enableChannelIfDisabled = false)";
            }
            else
            {
                EVT_VARIANT on = {};
                on.Type = EvtVarTypeBoolean;
                on.BooleanVal = TRUE;
                if (EvtSetChannelConfigProperty(config, EvtChannelConfigEnabled, 0, &on) &&
                    EvtSaveChannelConfig(config, 0))
                {
                    ok = true;
                }
                else
                {
                    const DWORD error = GetLastError();
                    why = WithError(error == ERROR_ACCESS_DENIED
                                        ? L"enabling the PrintService/Operational channel requires administrator rights"
                                        : L"failed to enable the PrintService/Operational channel",
                                    error);
                }
            }
        }
        EvtClose(config);
        return ok;
    }
}

// Bridges the C callback to the (private) members; declared a friend in PrintMonitor.h.
struct PrintMonitorThunk
{
    static DWORD WINAPI OnSubscription(EVT_SUBSCRIBE_NOTIFY_ACTION action, PVOID context, EVT_HANDLE eventHandle)
    {
        PrintMonitor* self = static_cast<PrintMonitor*>(context);
        if (!self)
            return ERROR_SUCCESS;
        t_inCallback = true;
        try
        {
            if (action == EvtSubscribeActionError)
                self->OnSubscriptionError(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(eventHandle)));
            else if (action == EvtSubscribeActionDeliver)
                self->OnEvent(eventHandle);
        }
        catch (...)
        {
            // Never let an exception escape into the event log service's thread pool.
        }
        t_inCallback = false;
        return ERROR_SUCCESS;   // the service owns and closes eventHandle
    }
};

PrintMonitor::~PrintMonitor()
{
    Stop();
}

bool PrintMonitor::Start(Callback cb, bool enableChannelIfDisabled)
{
    if (t_inCallback)
        return IsActive();                  // cannot (re)subscribe from inside our own callback

    std::lock_guard<std::mutex> life(m_lifecycleMutex);
    if (m_subscription)
    {
        if (m_accepting.load() && m_healthy.load())
            return true;                    // already running
        CloseSubscriptionLocked();          // errored, or stopped from the callback: start over
    }
    if (!cb)
    {
        SetStatus(L"no callback supplied");
        return false;
    }

    std::wstring why;
    if (!EnsureChannelEnabled(enableChannelIfDisabled, why))
    {
        SetStatus(why);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callback = std::move(cb);
        m_status = std::wstring(L"active: ") + kChannel + L", EventID 307";
    }
    m_healthy.store(true);
    m_accepting.store(true);                // before EvtSubscribe so the first event is not dropped
    EVT_HANDLE subscription = EvtSubscribe(nullptr, nullptr, kChannel, kQuery, nullptr, this,
                                           &PrintMonitorThunk::OnSubscription, EvtSubscribeToFutureEvents);
    if (!subscription)
    {
        const DWORD error = GetLastError();
        m_accepting.store(false);
        m_healthy.store(false);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callback = nullptr;
        m_status = WithError(L"EvtSubscribe failed", error);
        return false;
    }
    m_subscription = subscription;
    return true;
}

void PrintMonitor::Stop()
{
    m_accepting.store(false);               // stop delivery immediately, whatever thread we are on
    if (t_inCallback)
    {
        SetStatus(L"stopped");
        return;                             // EvtClose from inside the callback could deadlock
    }

    std::lock_guard<std::mutex> life(m_lifecycleMutex);
    const bool wasSubscribed = (m_subscription != nullptr);
    CloseSubscriptionLocked();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_callback = nullptr;
    if (wasSubscribed)
        m_status = L"stopped";
}

bool PrintMonitor::IsActive() const
{
    return m_accepting.load() && m_healthy.load();
}

std::wstring PrintMonitor::Status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void PrintMonitor::SetStatus(const std::wstring& status)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status = status;
}

void PrintMonitor::CloseSubscriptionLocked()
{
    m_accepting.store(false);
    m_healthy.store(false);
    if (m_subscription)
    {
        // Waits for an in-flight callback; m_mutex is not held here, so that callback can finish.
        EvtClose(m_subscription);
        m_subscription = nullptr;
    }
}

void PrintMonitor::OnSubscriptionError(DWORD error)
{
    m_healthy.store(false);
    SetStatus(WithError(L"subscription error; call Stop() then Start() to resubscribe", error));
}

void PrintMonitor::OnEvent(HANDLE eventHandle)
{
    if (!m_accepting.load())
        return;
    const std::wstring xml = RenderEventXml(eventHandle);
    if (xml.empty())
        return;

    // <UserData><DocumentPrinted ...><Param1>JobId</Param1>...<Param8>Pages</Param8>
    size_t from = xml.find(L"<DocumentPrinted");
    if (from == std::wstring::npos)
        from = 0;

    PrintEvent ev;
    ev.jobId         = ParseDword(ElementText(xml, from, L"Param1"));
    ev.documentName  = ElementText(xml, from, L"Param2");
    ev.owner         = ElementText(xml, from, L"Param3");
    ev.clientMachine = ElementText(xml, from, L"Param4");
    ev.printer       = ElementText(xml, from, L"Param5");
    ev.pages         = ParseDword(ElementText(xml, from, L"Param8"));
    int64_t createdMs = 0;
    ev.utcMs = ParseUtcTimestamp(AttributeValue(xml, L"TimeCreated", L"SystemTime"), createdMs)
                   ? createdMs
                   : NowUnixMs();

    Callback cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        cb = m_callback;
    }
    if (cb && m_accepting.load())
        cb(ev);
}
