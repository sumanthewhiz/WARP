#include "framework.h"
#include "OfficeMruReader.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#pragma comment(lib, "advapi32.lib")

namespace
{
    const wchar_t* const kOfficeVersions[] = { L"16.0", L"15.0" };
    const wchar_t* const kOfficeApps[] = { L"Word", L"Excel", L"PowerPoint", L"Visio", L"Access", L"Publisher" };
    const DWORD   kMinPollMs           = 1000;
    const DWORD   kMaxValueBytes       = 64 * 1024;
    const DWORD   kMaxEnumeratedItems  = 4096;
    const int64_t kFileTimeUnixEpoch   = 116444736000000000LL;

    struct KeyLess
    {
        bool operator()(const std::wstring& a, const std::wstring& b) const
        {
            return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                        b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
        }
    };

    class RegKey
    {
    public:
        RegKey() = default;

        ~RegKey()
        {
            Close();
        }

        RegKey(const RegKey&) = delete;
        RegKey& operator=(const RegKey&) = delete;

        bool Open(HKEY parent, const std::wstring& subKey)
        {
            Close();
            HKEY key = nullptr;
            if (RegOpenKeyExW(parent, subKey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
            {
                return false;
            }
            m_key = key;
            return true;
        }

        HKEY Get() const
        {
            return m_key;
        }

        void Close()
        {
            if (m_key != nullptr)
            {
                RegCloseKey(m_key);
                m_key = nullptr;
            }
        }

    private:
        HKEY m_key = nullptr;
    };

    bool IsBlank(wchar_t c)
    {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\0' || c == 0xFEFF;
    }

    void Trim(std::wstring& text)
    {
        size_t end = text.size();
        while (end > 0 && IsBlank(text[end - 1]))
        {
            --end;
        }
        size_t begin = 0;
        while (begin < end && IsBlank(text[begin]))
        {
            ++begin;
        }
        text = text.substr(begin, end - begin);
    }

    bool StartsWithNoCase(const std::wstring& text, const wchar_t* prefix, int prefixLength)
    {
        return text.size() >= static_cast<size_t>(prefixLength) &&
               CompareStringOrdinal(text.c_str(), prefixLength, prefix, prefixLength, TRUE) == CSTR_EQUAL;
    }

    bool ParseHex(const std::wstring& text, size_t begin, size_t end, uint64_t& value)
    {
        if (end <= begin || end - begin > 16)
        {
            return false;
        }
        uint64_t result = 0;
        for (size_t i = begin; i < end; ++i)
        {
            const wchar_t c = text[i];
            uint64_t digit = 0;
            if (c >= L'0' && c <= L'9')
            {
                digit = static_cast<uint64_t>(c - L'0');
            }
            else if (c >= L'a' && c <= L'f')
            {
                digit = static_cast<uint64_t>(c - L'a') + 10;
            }
            else if (c >= L'A' && c <= L'F')
            {
                digit = static_cast<uint64_t>(c - L'A') + 10;
            }
            else
            {
                return false;
            }
            result = (result << 4) | digit;
        }
        value = result;
        return true;
    }

    // "[F00000000][T01D9C2A3B4C5D6E7][O00000000]*C:\path\file.docx"
    bool ParseMruValue(const std::wstring& value, int64_t& utcMs, std::wstring& target)
    {
        size_t pos = 0;
        bool haveTime = false;
        uint64_t fileTime = 0;
        while (pos < value.size() && value[pos] == L'[')
        {
            const size_t close = value.find(L']', pos + 1);
            if (close == std::wstring::npos)
            {
                return false;
            }
            if (close > pos + 1 && (value[pos + 1] == L'T' || value[pos + 1] == L't'))
            {
                haveTime = ParseHex(value, pos + 2, close, fileTime);
            }
            pos = close + 1;
        }
        if (!haveTime || pos >= value.size() || value[pos] != L'*')
        {
            return false;
        }
        target = value.substr(pos + 1);
        Trim(target);
        if (target.empty() || fileTime <= static_cast<uint64_t>(kFileTimeUnixEpoch) ||
            fileTime > static_cast<uint64_t>(INT64_MAX))
        {
            return false;
        }
        utcMs = (static_cast<int64_t>(fileTime) - kFileTimeUnixEpoch) / 10000;
        return true;
    }

    bool IsItemValueName(const wchar_t* name, DWORD length)
    {
        if (length < 6 || CompareStringOrdinal(name, 5, L"Item ", 5, TRUE) != CSTR_EQUAL)
        {
            return false;   // also skips "Item Metadata N"
        }
        for (DWORD i = 5; i < length; ++i)
        {
            if (name[i] < L'0' || name[i] > L'9')
            {
                return false;
            }
        }
        return true;
    }

    void ReadFileMruKey(const std::wstring& subKey, const wchar_t* app, std::vector<OfficeMruItem>& items)
    {
        RegKey key;
        if (!key.Open(HKEY_CURRENT_USER, subKey))
        {
            return;
        }
        DWORD maxNameChars = 0;
        DWORD maxDataBytes = 0;
        if (RegQueryInfoKeyW(key.Get(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                             &maxNameChars, &maxDataBytes, nullptr, nullptr) != ERROR_SUCCESS)
        {
            return;
        }
        if (maxDataBytes > kMaxValueBytes)
        {
            maxDataBytes = kMaxValueBytes;   // larger values fail with ERROR_MORE_DATA and are skipped
        }
        std::vector<wchar_t> name(static_cast<size_t>(maxNameChars) + 2);
        std::vector<wchar_t> data(static_cast<size_t>(maxDataBytes) / sizeof(wchar_t) + 2);
        for (DWORD index = 0; index < kMaxEnumeratedItems; ++index)
        {
            DWORD nameChars = static_cast<DWORD>(name.size());
            DWORD dataBytes = static_cast<DWORD>((data.size() - 1) * sizeof(wchar_t));
            DWORD type = 0;
            const LSTATUS status = RegEnumValueW(key.Get(), index, name.data(), &nameChars, nullptr, &type,
                                                 reinterpret_cast<LPBYTE>(data.data()), &dataBytes);
            if (status == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            if (status != ERROR_SUCCESS)
            {
                continue;   // e.g. a value that grew since RegQueryInfoKeyW; picked up next poll
            }
            if ((type != REG_SZ && type != REG_EXPAND_SZ) || !IsItemValueName(name.data(), nameChars))
            {
                continue;
            }
            const std::wstring value(data.data(), dataBytes / sizeof(wchar_t));
            OfficeMruItem item;
            if (!ParseMruValue(value, item.utcMs, item.pathOrUrl))
            {
                continue;
            }
            item.app = app;
            item.isUrl = StartsWithNoCase(item.pathOrUrl, L"http", 4);
            items.push_back(std::move(item));
        }
    }

    std::vector<std::wstring> EnumSubKeys(HKEY key)
    {
        std::vector<std::wstring> names;
        wchar_t name[256] = {};   // registry key names are at most 255 characters
        for (DWORD index = 0; index < kMaxEnumeratedItems; ++index)
        {
            DWORD length = static_cast<DWORD>(_countof(name));
            const LSTATUS status = RegEnumKeyExW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            if (status == ERROR_SUCCESS)
            {
                names.emplace_back(name, length);
            }
        }
        return names;
    }

    std::wstring MakeKey(const OfficeMruItem& item)
    {
        return item.app + L"|" + item.pathOrUrl;
    }
}

struct OfficeMruReader::Impl
{
    std::mutex         lifecycleMutex;   // serializes Start/Stop
    std::thread        worker;
    HANDLE             stopEvent = nullptr;
    std::atomic<bool>  stopRequested{ false };
    std::atomic<DWORD> workerThreadId{ 0 };

    // Written by Start() before the worker exists, then owned by the worker until joined.
    Callback           callback;
    int64_t            backfillSinceUtcMs = 0;
    DWORD              pollMs = 120000;
    std::map<std::wstring, int64_t, KeyLess> lastReported;   // "app|path" -> newest time reported

    Impl()
    {
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~Impl()
    {
        if (stopEvent != nullptr)
        {
            CloseHandle(stopEvent);
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    bool IsStopRequested() const
    {
        return stopRequested.load(std::memory_order_relaxed);
    }

    void RequestStop()
    {
        stopRequested.store(true);
        if (stopEvent != nullptr)
        {
            SetEvent(stopEvent);
        }
    }

    void JoinWorker()
    {
        if (worker.joinable())
        {
            try
            {
                worker.join();
            }
            catch (const std::system_error&)
            {
            }
        }
    }

    void Run();
    void Poll(bool firstPass);
};

void OfficeMruReader::Impl::Run()
{
    workerThreadId.store(GetCurrentThreadId());
    bool firstPass = true;
    while (!IsStopRequested())
    {
        try
        {
            Poll(firstPass);
        }
        catch (...)
        {
            // Out of memory: try again on the next poll.
        }
        firstPass = false;
        if (WaitForSingleObject(stopEvent, pollMs) != WAIT_TIMEOUT)
        {
            break;
        }
    }
    workerThreadId.store(0);
}

void OfficeMruReader::Impl::Poll(bool firstPass)
{
    std::vector<OfficeMruItem> items = OfficeMruReader::ReadAll();
    // Rebuilt every poll so entries that fell off the MRU lists do not accumulate.
    std::map<std::wstring, int64_t, KeyLess> current;
    for (OfficeMruItem& item : items)
    {
        if (IsStopRequested())
        {
            return;
        }
        std::wstring key = MakeKey(item);
        const auto previous = lastReported.find(key);
        bool known = previous != lastReported.end();
        int64_t reportedMs = known ? previous->second : 0;
        if (item.utcMs > backfillSinceUtcMs && (!known || item.utcMs > reportedMs))
        {
            item.backfill = firstPass;
            try
            {
                callback(item);
            }
            catch (...)
            {
                // Consumer exceptions must not stop the reader.
            }
            known = true;
            reportedMs = item.utcMs;
        }
        if (known)
        {
            current[std::move(key)] = reportedMs;
        }
    }
    lastReported.swap(current);
}

OfficeMruReader::OfficeMruReader()
    : m_impl(new Impl())
{
}

OfficeMruReader::~OfficeMruReader()
{
    Stop();
    if (m_impl && m_impl->worker.joinable())
    {
        // Destroyed from inside its own callback: a thread cannot join itself. Detach and
        // intentionally leak the state so the unwinding worker never touches freed memory.
        m_impl->worker.detach();
        Impl* leaked = m_impl.release();
        (void)leaked;
    }
}

bool OfficeMruReader::Start(Callback cb, int64_t backfillSinceUtcMs, DWORD pollMs)
{
    Impl* impl = m_impl.get();
    if (impl == nullptr || !cb || impl->stopEvent == nullptr)
    {
        return false;
    }
    if (impl->workerThreadId.load() == GetCurrentThreadId())
    {
        return false;   // called from our own callback
    }

    std::lock_guard<std::mutex> lock(impl->lifecycleMutex);
    if (impl->worker.joinable())
    {
        if (!impl->stopRequested.load())
        {
            return false;   // already running
        }
        impl->JoinWorker();   // a previous run was stopped from its own callback
    }

    impl->callback = std::move(cb);
    impl->backfillSinceUtcMs = backfillSinceUtcMs;
    impl->pollMs = pollMs < kMinPollMs ? kMinPollMs : pollMs;
    impl->lastReported.clear();
    impl->stopRequested.store(false);
    ResetEvent(impl->stopEvent);

    try
    {
        impl->worker = std::thread(&Impl::Run, impl);
    }
    catch (const std::system_error&)
    {
        impl->callback = nullptr;
        return false;
    }
    return true;
}

void OfficeMruReader::Stop()
{
    Impl* impl = m_impl.get();
    if (impl == nullptr)
    {
        return;
    }
    if (impl->workerThreadId.load() == GetCurrentThreadId())
    {
        impl->RequestStop();   // from the callback: the worker exits after it returns
        return;
    }

    std::lock_guard<std::mutex> lock(impl->lifecycleMutex);
    impl->RequestStop();
    if (!impl->worker.joinable())
    {
        return;
    }
    impl->JoinWorker();
    impl->callback = nullptr;
}

std::vector<OfficeMruItem> OfficeMruReader::ReadAll()
{
    std::vector<OfficeMruItem> raw;
    for (const wchar_t* version : kOfficeVersions)
    {
        for (const wchar_t* app : kOfficeApps)
        {
            const std::wstring appKey = std::wstring(L"Software\\Microsoft\\Office\\") + version + L"\\" + app;
            ReadFileMruKey(appKey + L"\\File MRU", app, raw);

            RegKey userMru;
            if (userMru.Open(HKEY_CURRENT_USER, appKey + L"\\User MRU"))
            {
                // One subkey per signed-in identity (LiveId_..., ADAL_..., AD_...).
                const std::vector<std::wstring> identities = EnumSubKeys(userMru.Get());
                for (const std::wstring& identity : identities)
                {
                    ReadFileMruKey(appKey + L"\\User MRU\\" + identity + L"\\File MRU", app, raw);
                }
            }
        }
    }

    // The same document can be listed per version and per identity: keep the newest time.
    std::map<std::wstring, size_t, KeyLess> index;
    std::vector<OfficeMruItem> items;
    items.reserve(raw.size());
    for (OfficeMruItem& item : raw)
    {
        std::wstring key = MakeKey(item);
        const auto found = index.find(key);
        if (found == index.end())
        {
            index.emplace(std::move(key), items.size());
            items.push_back(std::move(item));
        }
        else if (item.utcMs > items[found->second].utcMs)
        {
            items[found->second].utcMs = item.utcMs;
        }
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const OfficeMruItem& a, const OfficeMruItem& b) { return a.utcMs < b.utcMs; });
    return items;
}
