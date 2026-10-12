#include "framework.h"
#include "RecentItemsWatcher.h"

#include <objbase.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cwchar>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "uuid.lib")

namespace
{
    const ULONGLONG kDebounceMs        = 500;
    const ULONGLONG kReopenIntervalMs  = 5000;
    const ULONGLONG kMaxWaitMs         = 60000;
    const int64_t   kRescanSlackMs     = 2000;
    const DWORD     kNotifyBufferBytes = 64 * 1024;
    const size_t    kMaxTargetChars    = 4096;
    const size_t    kMaxReported       = 8192;
    const int64_t   kFileTimeUnixEpoch = 116444736000000000LL;

    // Case-insensitive ordering for file names (ordinal, like NTFS).
    struct NameLess
    {
        bool operator()(const std::wstring& a, const std::wstring& b) const
        {
            return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                        b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
        }
    };

    int64_t FileTimeToUnixMs(const FILETIME& fileTime)
    {
        ULARGE_INTEGER value = {};
        value.LowPart = fileTime.dwLowDateTime;
        value.HighPart = fileTime.dwHighDateTime;
        const int64_t ticks = static_cast<int64_t>(value.QuadPart);
        return ticks <= kFileTimeUnixEpoch ? 0 : (ticks - kFileTimeUnixEpoch) / 10000;
    }

    int64_t NowUtcMs()
    {
        FILETIME now = {};
        GetSystemTimeAsFileTime(&now);
        return FileTimeToUnixMs(now);
    }

    bool StartsWithNoCase(const std::wstring& text, const wchar_t* prefix)
    {
        const size_t length = wcslen(prefix);
        return text.size() >= length &&
               CompareStringOrdinal(text.c_str(), static_cast<int>(length),
                                    prefix, static_cast<int>(length), TRUE) == CSTR_EQUAL;
    }

    bool HasLnkExtension(const std::wstring& name)
    {
        return name.size() > 4 &&
               CompareStringOrdinal(name.c_str() + name.size() - 4, 4, L".lnk", 4, TRUE) == CSTR_EQUAL;
    }

    bool GetRecentFolder(std::wstring& folder)
    {
        PWSTR path = nullptr;
        const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Recent, KF_FLAG_DEFAULT, nullptr, &path);
        if (SUCCEEDED(hr) && path != nullptr)
        {
            folder.assign(path);
        }
        CoTaskMemFree(path);   // required even on failure
        while (folder.size() > 3 && folder.back() == L'\\')
        {
            folder.pop_back();
        }
        return SUCCEEDED(hr) && !folder.empty();
    }

    class FindHandle
    {
    public:
        explicit FindHandle(HANDLE handle) : m_handle(handle)
        {
        }

        ~FindHandle()
        {
            if (m_handle != INVALID_HANDLE_VALUE)
            {
                FindClose(m_handle);
            }
        }

        FindHandle(const FindHandle&) = delete;
        FindHandle& operator=(const FindHandle&) = delete;

        HANDLE Get() const
        {
            return m_handle;
        }

    private:
        HANDLE m_handle;
    };

    struct LinkFile
    {
        std::wstring name;
        int64_t      writeMs = 0;
    };

    std::vector<LinkFile> ListLinks(const std::wstring& folder)
    {
        std::vector<LinkFile> links;
        const std::wstring pattern = folder + L"\\*.lnk";
        WIN32_FIND_DATAW data = {};
        FindHandle find(FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                         nullptr, FIND_FIRST_EX_LARGE_FETCH));
        if (find.Get() == INVALID_HANDLE_VALUE)
        {
            return links;
        }
        do
        {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                continue;
            }
            LinkFile link;
            link.name = data.cFileName;
            // "*.lnk" also matches long names whose 8.3 alias ends in .LNK (e.g. "a.lnk2").
            if (!HasLnkExtension(link.name))
            {
                continue;
            }
            link.writeMs = FileTimeToUnixMs(data.ftLastWriteTime);
            links.push_back(std::move(link));
        } while (FindNextFileW(find.Get(), &data));
        return links;
    }

    std::wstring ExpandEnvironment(const std::wstring& text)
    {
        const DWORD needed = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
        if (needed == 0)
        {
            return text;
        }
        std::vector<wchar_t> buffer(static_cast<size_t>(needed) + 1);
        const DWORD written = ExpandEnvironmentStringsW(text.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0 || written > buffer.size())
        {
            return text;
        }
        return std::wstring(buffer.data());
    }

    // Reads the shortcut without resolving it (no target probing, no network access).
    bool ResolveShortcut(const std::wstring& lnkPath, std::wstring& target, DWORD& targetAttributes)
    {
        target.clear();
        targetAttributes = 0;

        Microsoft::WRL::ComPtr<IShellLinkW> link;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        {
            return false;
        }
        Microsoft::WRL::ComPtr<IPersistFile> file;
        if (FAILED(link.As(&file)) || FAILED(file->Load(lnkPath.c_str(), STGM_READ)))
        {
            return false;
        }

        std::vector<wchar_t> buffer(kMaxTargetChars, L'\0');
        WIN32_FIND_DATAW data = {};
        if (link->GetPath(buffer.data(), static_cast<int>(buffer.size()), &data, SLGP_RAWPATH) == S_OK &&
            buffer[0] != L'\0')
        {
            buffer.back() = L'\0';
            target.assign(buffer.data());
            targetAttributes = data.dwFileAttributes;
            if (target.find(L'%') != std::wstring::npos)
            {
                target = ExpandEnvironment(target);   // raw paths may hold %VARS%
            }
        }
        if (target.empty())
        {
            // Not a file-system target (URL, shell item): use its desktop-absolute parsing name.
            PIDLIST_ABSOLUTE idList = nullptr;
            if (SUCCEEDED(link->GetIDList(&idList)) && idList != nullptr)
            {
                PWSTR name = nullptr;
                if (SUCCEEDED(SHGetNameFromIDList(idList, SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name != nullptr)
                {
                    target.assign(name);
                }
                CoTaskMemFree(name);
                CoTaskMemFree(idList);
            }
        }
        return !target.empty();
    }

    bool IsNetworkPath(const std::wstring& path)
    {
        if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
        {
            return true;
        }
        if (path.size() >= 2 && path[1] == L':')
        {
            const wchar_t root[] = { path[0], L':', L'\\', L'\0' };
            return GetDriveTypeW(root) == DRIVE_REMOTE;
        }
        return false;
    }

    bool IsFolderTarget(const std::wstring& target, DWORD linkAttributes)
    {
        // Probing a network target can block for many seconds (and delay Stop); for those,
        // trust the target attributes recorded in the shortcut instead.
        if (!IsNetworkPath(target))
        {
            const DWORD attributes = GetFileAttributesW(target.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES)
            {
                return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            }
        }
        return (linkAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    void PumpMessages()
    {
        MSG message = {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

struct RecentItemsWatcher::Impl
{
    std::mutex          lifecycleMutex;   // serializes Start/Stop
    std::thread         worker;
    HANDLE              stopEvent = nullptr;
    std::atomic<bool>   stopRequested{ false };
    std::atomic<DWORD>  workerThreadId{ 0 };

    // Written by Start() before the worker exists, then owned by the worker until joined.
    Callback            callback;
    int64_t             backfillSinceUtcMs = 0;
    int64_t             watchStartUtcMs = 0;
    std::wstring        folder;
    HANDLE              directory = INVALID_HANDLE_VALUE;
    std::map<std::wstring, int64_t, NameLess>   reported;   // lnk name -> last-write ms already reported
    std::map<std::wstring, ULONGLONG, NameLess> pending;    // lnk name -> debounce deadline (tick)

    Impl()
    {
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~Impl()
    {
        CloseDirectory();
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

    bool OpenDirectory()
    {
        CloseDirectory();
        directory = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        return directory != INVALID_HANDLE_VALUE;
    }

    void CloseDirectory()
    {
        if (directory != INVALID_HANDLE_VALUE)
        {
            CloseHandle(directory);
            directory = INVALID_HANDLE_VALUE;
        }
    }

    bool IssueRead(OVERLAPPED& overlapped, BYTE* buffer, DWORD size)
    {
        if (directory == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        ResetEvent(overlapped.hEvent);
        return ReadDirectoryChangesW(directory, buffer, size, FALSE,
                                     FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE,
                                     nullptr, &overlapped, nullptr) != FALSE;
    }

    void Run();
    void Watch();
    void WatchLoop(OVERLAPPED& overlapped, BYTE* buffer, DWORD bufferSize, bool& ioPending);
    void Backfill();
    void Rescan();
    void ParseNotifications(const BYTE* buffer, DWORD size);
    void NormalizeName(std::wstring& name) const;
    ULONGLONG EarliestDue() const;
    void ProcessDue();
    void EmitLink(const std::wstring& name, bool backfill);
};

void RecentItemsWatcher::Impl::Run()
{
    workerThreadId.store(GetCurrentThreadId());
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, nullptr);
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    try
    {
        Watch();
    }
    catch (...)
    {
        // Never let an exception escape the thread (std::terminate).
    }
    if (SUCCEEDED(comInit))
    {
        CoUninitialize();
    }
    workerThreadId.store(0);
}

void RecentItemsWatcher::Impl::Watch()
{
    std::vector<DWORD> storage(kNotifyBufferBytes / sizeof(DWORD));   // FILE_NOTIFY_INFORMATION is DWORD aligned
    BYTE* const buffer = reinterpret_cast<BYTE*>(storage.data());
    const DWORD bufferSize = static_cast<DWORD>(storage.size() * sizeof(DWORD));

    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr)
    {
        CloseDirectory();
        return;
    }

    bool ioPending = false;
    try
    {
        WatchLoop(overlapped, buffer, bufferSize, ioPending);
    }
    catch (...)
    {
    }

    // The kernel owns the buffer and OVERLAPPED until the pending read completes.
    if (ioPending)
    {
        CancelIoEx(directory, &overlapped);
        DWORD ignored = 0;
        GetOverlappedResult(directory, &overlapped, &ignored, TRUE);
    }
    CloseHandle(overlapped.hEvent);
    CloseDirectory();
}

void RecentItemsWatcher::Impl::WatchLoop(OVERLAPPED& overlapped, BYTE* buffer, DWORD bufferSize, bool& ioPending)
{
    // Arm the watch before enumerating so changes made during the backfill are not missed.
    ioPending = IssueRead(overlapped, buffer, bufferSize);
    ULONGLONG reopenAtTick = 0;
    if (!ioPending)
    {
        CloseDirectory();
        reopenAtTick = GetTickCount64() + kReopenIntervalMs;
    }

    Backfill();

    while (!IsStopRequested())
    {
        const ULONGLONG now = GetTickCount64();
        ULONGLONG wakeAt = pending.empty() ? 0 : EarliestDue();
        if (!ioPending && (wakeAt == 0 || reopenAtTick < wakeAt))
        {
            wakeAt = reopenAtTick;
        }
        DWORD timeout = INFINITE;
        if (wakeAt != 0)
        {
            const ULONGLONG delta = wakeAt > now ? wakeAt - now : 0;
            timeout = static_cast<DWORD>(delta < kMaxWaitMs ? delta : kMaxWaitMs);
        }

        // STA thread: keep pumping messages while waiting.
        HANDLE handles[2] = { stopEvent, overlapped.hEvent };
        const DWORD count = ioPending ? 2 : 1;
        const DWORD wait = MsgWaitForMultipleObjectsEx(count, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED)
        {
            break;
        }
        if (ioPending && wait == WAIT_OBJECT_0 + 1)
        {
            ioPending = false;
            DWORD bytes = 0;
            if (GetOverlappedResult(directory, &overlapped, &bytes, FALSE))
            {
                if (bytes == 0)
                {
                    Rescan();   // the change buffer overflowed
                }
                else
                {
                    ParseNotifications(buffer, bytes);
                }
            }
            else if (GetLastError() == ERROR_NOTIFY_ENUM_DIR)
            {
                Rescan();
            }
            else
            {
                CloseDirectory();
            }
            ioPending = IssueRead(overlapped, buffer, bufferSize);
            if (!ioPending)
            {
                CloseDirectory();
                reopenAtTick = GetTickCount64() + kReopenIntervalMs;
            }
        }
        else if (wait == WAIT_OBJECT_0 + count)
        {
            PumpMessages();
        }

        if (!ioPending && GetTickCount64() >= reopenAtTick)
        {
            if (OpenDirectory())
            {
                ioPending = IssueRead(overlapped, buffer, bufferSize);
            }
            if (ioPending)
            {
                Rescan();   // pick up anything written while the watch was down
            }
            else
            {
                CloseDirectory();
                reopenAtTick = GetTickCount64() + kReopenIntervalMs;
            }
        }
        ProcessDue();
    }
}

void RecentItemsWatcher::Impl::Backfill()
{
    std::vector<LinkFile> links = ListLinks(folder);
    links.erase(std::remove_if(links.begin(), links.end(),
                               [this](const LinkFile& link) { return link.writeMs <= backfillSinceUtcMs; }),
                links.end());
    std::stable_sort(links.begin(), links.end(),
                     [](const LinkFile& a, const LinkFile& b) { return a.writeMs < b.writeMs; });
    for (const LinkFile& link : links)
    {
        if (IsStopRequested())
        {
            return;
        }
        EmitLink(link.name, true);
    }
}

void RecentItemsWatcher::Impl::Rescan()
{
    const ULONGLONG due = GetTickCount64() + kDebounceMs;
    const std::vector<LinkFile> links = ListLinks(folder);
    for (const LinkFile& link : links)
    {
        const auto it = reported.find(link.name);
        const bool changed = it != reported.end()
            ? it->second != link.writeMs
            : (link.writeMs > backfillSinceUtcMs && link.writeMs >= watchStartUtcMs - kRescanSlackMs);
        if (changed)
        {
            pending[link.name] = due;
        }
    }
}

void RecentItemsWatcher::Impl::ParseNotifications(const BYTE* buffer, DWORD size)
{
    const ULONGLONG due = GetTickCount64() + kDebounceMs;
    const size_t header = offsetof(FILE_NOTIFY_INFORMATION, FileName);
    size_t offset = 0;
    for (;;)
    {
        if (offset + header > size)
        {
            break;
        }
        const FILE_NOTIFY_INFORMATION* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer + offset);
        if (offset + header + info->FileNameLength > size)
        {
            break;
        }
        if (info->Action == FILE_ACTION_ADDED || info->Action == FILE_ACTION_MODIFIED ||
            info->Action == FILE_ACTION_RENAMED_NEW_NAME)
        {
            std::wstring name(info->FileName, info->FileNameLength / sizeof(WCHAR));
            if (HasLnkExtension(name) && name.find(L'\\') == std::wstring::npos)
            {
                NormalizeName(name);
                pending[name] = due;   // trailing-edge debounce per shortcut
            }
        }
        if (info->NextEntryOffset == 0)
        {
            break;
        }
        offset += info->NextEntryOffset;
    }
}

// Change notifications can carry 8.3 names; key everything by the long name.
void RecentItemsWatcher::Impl::NormalizeName(std::wstring& name) const
{
    if (name.find(L'~') == std::wstring::npos)
    {
        return;
    }
    const std::wstring shortPath = folder + L"\\" + name;
    std::vector<wchar_t> buffer(MAX_PATH * 2);
    const DWORD length = GetLongPathNameW(shortPath.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
    {
        return;
    }
    const std::wstring longPath(buffer.data(), length);
    const size_t slash = longPath.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash + 1 < longPath.size())
    {
        name = longPath.substr(slash + 1);
    }
}

ULONGLONG RecentItemsWatcher::Impl::EarliestDue() const
{
    ULONGLONG earliest = 0;
    for (const auto& entry : pending)
    {
        if (earliest == 0 || entry.second < earliest)
        {
            earliest = entry.second;
        }
    }
    return earliest;
}

void RecentItemsWatcher::Impl::ProcessDue()
{
    if (pending.empty())
    {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    std::vector<std::wstring> due;
    for (auto it = pending.begin(); it != pending.end();)
    {
        if (it->second <= now)
        {
            due.push_back(it->first);
            it = pending.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (const std::wstring& name : due)
    {
        if (IsStopRequested())
        {
            return;
        }
        EmitLink(name, false);
    }
}

void RecentItemsWatcher::Impl::EmitLink(const std::wstring& name, bool backfill)
{
    try
    {
        std::wstring lnkPath = folder + L"\\" + name;
        WIN32_FILE_ATTRIBUTE_DATA attributes = {};
        if (!GetFileAttributesExW(lnkPath.c_str(), GetFileExInfoStandard, &attributes) ||
            (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return;   // removed before the debounce elapsed
        }
        const int64_t writeMs = FileTimeToUnixMs(attributes.ftLastWriteTime);
        const auto it = reported.find(name);
        if (it != reported.end() && it->second == writeMs)
        {
            return;   // this version of the shortcut was already reported
        }
        if (reported.size() >= kMaxReported)
        {
            reported.clear();
        }
        reported[name] = writeMs;

        RecentItemEvent ev;
        DWORD targetAttributes = 0;
        if (!ResolveShortcut(lnkPath, ev.target, targetAttributes))
        {
            return;
        }
        ev.isUrl = StartsWithNoCase(ev.target, L"http");
        ev.isFolder = !ev.isUrl && IsFolderTarget(ev.target, targetAttributes);
        ev.lnkPath = std::move(lnkPath);
        ev.utcMs = writeMs;
        ev.backfill = backfill;
        callback(ev);
    }
    catch (...)
    {
        // Out of memory or a throwing consumer: skip this shortcut, keep watching.
    }
}

RecentItemsWatcher::RecentItemsWatcher()
    : m_impl(new Impl())
{
}

RecentItemsWatcher::~RecentItemsWatcher()
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

bool RecentItemsWatcher::Start(Callback cb, int64_t backfillSinceUtcMs)
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

    std::wstring folder;
    if (!GetRecentFolder(folder))
    {
        return false;
    }
    impl->folder = folder;
    if (!impl->OpenDirectory())
    {
        return false;
    }
    impl->callback = std::move(cb);
    impl->backfillSinceUtcMs = backfillSinceUtcMs;
    impl->watchStartUtcMs = NowUtcMs();
    impl->reported.clear();
    impl->pending.clear();
    impl->stopRequested.store(false);
    ResetEvent(impl->stopEvent);

    try
    {
        impl->worker = std::thread(&Impl::Run, impl);
    }
    catch (const std::system_error&)
    {
        impl->CloseDirectory();
        impl->callback = nullptr;
        return false;
    }
    return true;
}

void RecentItemsWatcher::Stop()
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
    impl->CloseDirectory();
    impl->callback = nullptr;
    impl->pending.clear();
}
