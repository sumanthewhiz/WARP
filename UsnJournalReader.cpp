#include "framework.h"
#include "UsnJournalReader.h"

#include <winioctl.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <list>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "kernel32.lib")

namespace
{
    const DWORD     kPollIntervalMs       = 2000;
    const ULONGLONG kCursorSaveIntervalMs = 30000;
    const ULONGLONG kRetryIntervalMs      = 30000;
    const DWORD     kReadBufferBytes      = 128 * 1024;
    const int       kMaxReadsPerPass      = 32;      // per volume, then yield to the other volumes
    const int       kMaxConsecutiveErrors = 3;
    const size_t    kDirCacheCapacity     = 8192;
    const size_t    kMaxPendingRenames    = 4096;
    const DWORD     kRecordHeaderBytes    = 8;       // RecordLength + MajorVersion + MinorVersion
    const int64_t   kFileTimeUnixEpoch    = 116444736000000000LL;
    const int64_t   kMsPerHour            = 3600LL * 1000LL;

    const DWORD kDataReasons = USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION;
    const DWORD kReasonMask  = kDataReasons | USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE |
                               USN_REASON_RENAME_OLD_NAME | USN_REASON_RENAME_NEW_NAME | USN_REASON_CLOSE;
    // Writers that mark their changes as not altering application-visible data
    // (HSM recalls, antivirus/indexer private streams) are not user modifications.
    const DWORD kNonUserSources = USN_SOURCE_DATA_MANAGEMENT | USN_SOURCE_AUXILIARY_DATA;

    int64_t FileTimeToUnixMs(int64_t fileTime)
    {
        if (fileTime <= kFileTimeUnixEpoch)
        {
            return 0;
        }
        return (fileTime - kFileTimeUnixEpoch) / 10000;
    }

    int64_t NowUtcMs()
    {
        FILETIME now = {};
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER value = {};
        value.LowPart = now.dwLowDateTime;
        value.HighPart = now.dwHighDateTime;
        return FileTimeToUnixMs(static_cast<int64_t>(value.QuadPart));
    }

    std::wstring Hex64(uint64_t value)
    {
        wchar_t text[24] = {};
        swprintf_s(text, _countof(text), L"0x%llX", static_cast<unsigned long long>(value));
        return std::wstring(text);
    }

    bool EqualsNoCase(const wchar_t* a, size_t aLength, const wchar_t* b, size_t bLength)
    {
        return CompareStringOrdinal(a, static_cast<int>(aLength), b, static_cast<int>(bLength), TRUE) == CSTR_EQUAL;
    }

    // True when path equals prefix or lies below it (case-insensitive, separator aware).
    bool IsSameOrUnder(const std::wstring& path, const std::wstring& prefix)
    {
        if (prefix.empty() || path.size() < prefix.size())
        {
            return false;
        }
        if (!EqualsNoCase(path.c_str(), prefix.size(), prefix.c_str(), prefix.size()))
        {
            return false;
        }
        return path.size() == prefix.size() || prefix.back() == L'\\' || path[prefix.size()] == L'\\';
    }

    void StripWin32Prefix(std::wstring& path)
    {
        if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        {
            path.replace(0, 8, L"\\\\");
        }
        else if (path.compare(0, 4, L"\\\\?\\") == 0)
        {
            path.erase(0, 4);
        }
    }

    bool GetPathFromHandle(HANDLE handle, std::wstring& path)
    {
        // A normalized name can fail when an ancestor denies traversal; fall back to the opened name.
        const DWORD flagSets[] = { FILE_NAME_NORMALIZED | VOLUME_NAME_DOS, FILE_NAME_OPENED | VOLUME_NAME_DOS };
        for (DWORD flags : flagSets)
        {
            std::vector<wchar_t> buffer(MAX_PATH + 1);
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(),
                                                               static_cast<DWORD>(buffer.size()), flags);
                if (length == 0)
                {
                    break;
                }
                if (length < buffer.size())
                {
                    path.assign(buffer.data(), length);
                    StripWin32Prefix(path);
                    return !path.empty();
                }
                buffer.resize(static_cast<size_t>(length) + 1);
            }
        }
        return false;
    }

    struct FileIdKey
    {
        uint64_t lo = 0;
        uint64_t hi = 0;

        bool operator==(const FileIdKey& other) const
        {
            return lo == other.lo && hi == other.hi;
        }
    };

    struct FileIdKeyHash
    {
        size_t operator()(const FileIdKey& key) const
        {
            uint64_t h = key.lo * 0x9E3779B97F4A7C15ULL;
            h ^= (key.hi + 0x632BE59BD9B4E019ULL) + (h << 6) + (h >> 2);
            return static_cast<size_t>(h ^ (h >> 32));
        }
    };

    // 64-bit NTFS references pass through unchanged; 128-bit ReFS ids are folded.
    uint64_t FoldFileId(const FileIdKey& key)
    {
        return key.hi == 0 ? key.lo : (key.lo ^ (key.hi * 0x9E3779B97F4A7C15ULL));
    }

    // Bounded LRU map: directory file id -> full DOS path.
    class DirCache
    {
    public:
        explicit DirCache(size_t capacity) : m_capacity(capacity)
        {
        }

        bool Get(const FileIdKey& key, std::wstring& path)
        {
            auto it = m_index.find(key);
            if (it == m_index.end())
            {
                return false;
            }
            m_entries.splice(m_entries.begin(), m_entries, it->second);
            path = it->second->second;
            return true;
        }

        bool Peek(const FileIdKey& key, std::wstring& path) const
        {
            auto it = m_index.find(key);
            if (it == m_index.end())
            {
                return false;
            }
            path = it->second->second;
            return true;
        }

        void Put(const FileIdKey& key, const std::wstring& path)
        {
            auto it = m_index.find(key);
            if (it != m_index.end())
            {
                it->second->second = path;
                m_entries.splice(m_entries.begin(), m_entries, it->second);
                return;
            }
            m_entries.emplace_front(key, path);
            try
            {
                m_index.emplace(key, m_entries.begin());
            }
            catch (...)
            {
                m_entries.pop_front();
                throw;
            }
            if (m_index.size() > m_capacity)
            {
                m_index.erase(m_entries.back().first);
                m_entries.pop_back();
            }
        }

        void Erase(const FileIdKey& key)
        {
            auto it = m_index.find(key);
            if (it != m_index.end())
            {
                m_entries.erase(it->second);
                m_index.erase(it);
            }
        }

        // Drops the directory and every cached directory below it (after a rename/move).
        void EraseSameOrUnder(const std::wstring& directory)
        {
            for (auto it = m_entries.begin(); it != m_entries.end();)
            {
                if (IsSameOrUnder(it->second, directory))
                {
                    m_index.erase(it->first);
                    it = m_entries.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        void Clear()
        {
            m_index.clear();
            m_entries.clear();
        }

    private:
        typedef std::pair<FileIdKey, std::wstring> Entry;

        std::list<Entry> m_entries;   // most recently used first
        std::unordered_map<FileIdKey, std::list<Entry>::iterator, FileIdKeyHash> m_index;
        size_t m_capacity;
    };

    // Version-independent view of a USN_RECORD_V2 / USN_RECORD_V3 inside the read buffer.
    struct UsnRecordView
    {
        FileIdKey      fileId;
        FileIdKey      parentId;
        bool           is128 = false;
        int64_t        usn = 0;
        int64_t        timeStamp = 0;   // FILETIME, UTC
        DWORD          reason = 0;
        DWORD          sourceInfo = 0;
        DWORD          attributes = 0;
        const wchar_t* name = nullptr;  // not null-terminated
        size_t         nameLength = 0;  // characters
    };

    bool ParseRecord(const BYTE* record, DWORD length, UsnRecordView& view)
    {
        WORD majorVersion = 0;
        memcpy(&majorVersion, record + 4, sizeof(majorVersion));
        DWORD nameOffset = 0;
        DWORD nameBytes = 0;
        if (majorVersion == 2)
        {
            if (length < offsetof(USN_RECORD_V2, FileName))
            {
                return false;
            }
            const USN_RECORD_V2* r = reinterpret_cast<const USN_RECORD_V2*>(record);
            view.fileId.lo = r->FileReferenceNumber;
            view.fileId.hi = 0;
            view.parentId.lo = r->ParentFileReferenceNumber;
            view.parentId.hi = 0;
            view.is128 = false;
            view.usn = r->Usn;
            view.timeStamp = r->TimeStamp.QuadPart;
            view.reason = r->Reason;
            view.sourceInfo = r->SourceInfo;
            view.attributes = r->FileAttributes;
            nameOffset = r->FileNameOffset;
            nameBytes = r->FileNameLength;
        }
        else if (majorVersion == 3)
        {
            if (length < offsetof(USN_RECORD_V3, FileName))
            {
                return false;
            }
            const USN_RECORD_V3* r = reinterpret_cast<const USN_RECORD_V3*>(record);
            memcpy(&view.fileId.lo, r->FileReferenceNumber.Identifier, sizeof(uint64_t));
            memcpy(&view.fileId.hi, r->FileReferenceNumber.Identifier + sizeof(uint64_t), sizeof(uint64_t));
            memcpy(&view.parentId.lo, r->ParentFileReferenceNumber.Identifier, sizeof(uint64_t));
            memcpy(&view.parentId.hi, r->ParentFileReferenceNumber.Identifier + sizeof(uint64_t), sizeof(uint64_t));
            view.is128 = true;
            view.usn = r->Usn;
            view.timeStamp = r->TimeStamp.QuadPart;
            view.reason = r->Reason;
            view.sourceInfo = r->SourceInfo;
            view.attributes = r->FileAttributes;
            nameOffset = r->FileNameOffset;
            nameBytes = r->FileNameLength;
        }
        else
        {
            return false;   // V4 range-tracking or unknown layout
        }
        if (nameOffset < kRecordHeaderBytes || (nameBytes % sizeof(wchar_t)) != 0 || nameOffset + nameBytes > length)
        {
            return false;
        }
        view.name = reinterpret_cast<const wchar_t*>(record + nameOffset);
        view.nameLength = nameBytes / sizeof(wchar_t);
        return view.nameLength > 0;
    }

    struct Volume
    {
        std::wstring driveRoot;          // "C:\"
        std::wstring devicePath;         // "\\.\C:"
        std::wstring volumeKey;          // "\\?\Volume{GUID}\"
        std::wstring fileSystem;         // "NTFS" / "ReFS"
        std::wstring extendPrefix;       // "C:\$Extend"
        std::wstring sviPrefix;          // "C:\System Volume Information"
        HANDLE       handle = INVALID_HANDLE_VALUE;
        bool         active = false;
        bool         useReadV1 = false;
        bool         triedReadV1 = false;
        WORD         minMajor = 2;
        WORD         maxMajor = 2;
        uint64_t     journalId = 0;
        int64_t      firstUsn = 0;
        int64_t      lowestValidUsn = 0;
        int64_t      headUsn = 0;            // NextUsn reported by the last journal query
        int64_t      nextUsn = 0;            // next USN to read
        int64_t      backfillUntilUsn = 0;   // records below this USN predate Start()
        bool         cursorValid = false;
        bool         cursorPersisted = false;
        uint64_t     persistedJournalId = 0;
        int64_t      persistedUsn = 0;
        int          consecutiveErrors = 0;
        ULONGLONG    retryAtTick = 0;
        DWORD        lastError = ERROR_SUCCESS;
        uint64_t     eventsEmitted = 0;
        DirCache     dirCache{ kDirCacheCapacity };
        std::unordered_map<FileIdKey, std::wstring, FileIdKeyHash> pendingRenames;   // id -> old path

        Volume() = default;
        Volume(const Volume&) = delete;
        Volume& operator=(const Volume&) = delete;

        ~Volume()
        {
            CloseVolumeHandle();
        }

        void CloseVolumeHandle()
        {
            if (handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle);
                handle = INVALID_HANDLE_VALUE;
            }
        }
    };

    bool IsExcluded(const Volume& v, const std::wstring& path)
    {
        return IsSameOrUnder(path, v.extendPrefix) || IsSameOrUnder(path, v.sviPrefix);
    }

    struct JournalInfo
    {
        uint64_t id = 0;
        int64_t  firstUsn = 0;
        int64_t  nextUsn = 0;
        int64_t  lowestValidUsn = 0;
        WORD     minMajor = 2;
    };

    bool QueryJournal(HANDLE volume, JournalInfo& info, DWORD& error)
    {
        USN_JOURNAL_DATA_V1 data = {};
        DWORD bytes = 0;
        if (!DeviceIoControl(volume, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &data, sizeof(data), &bytes, nullptr))
        {
            error = GetLastError();
            return false;
        }
        if (bytes < sizeof(USN_JOURNAL_DATA_V0))
        {
            error = ERROR_INVALID_DATA;
            return false;
        }
        info.id = data.UsnJournalID;
        info.firstUsn = data.FirstUsn;
        info.nextUsn = data.NextUsn;
        info.lowestValidUsn = data.LowestValidUsn;
        info.minMajor = 2;
        if (bytes >= sizeof(USN_JOURNAL_DATA_V1) && data.MinSupportedMajorVersion != 0)
        {
            info.minMajor = data.MinSupportedMajorVersion;
        }
        error = ERROR_SUCCESS;
        return true;
    }

    // READ_USN_JOURNAL_DATA_V0 yields USN_RECORD_V2. Journals that cannot produce V2
    // records (ReFS reports MinSupportedMajorVersion = 3) need the V1 request instead.
    bool ApplyJournal(Volume& v, const JournalInfo& info)
    {
        if (info.minMajor > 3)
        {
            v.lastError = ERROR_NOT_SUPPORTED;
            return false;
        }
        v.journalId = info.id;
        v.firstUsn = info.firstUsn;
        v.lowestValidUsn = info.lowestValidUsn;
        v.headUsn = info.nextUsn;
        if (info.minMajor > 2)
        {
            v.useReadV1 = true;
            v.minMajor = info.minMajor;
            v.maxMajor = 3;
        }
        return true;
    }

    HANDLE OpenVolume(const std::wstring& devicePath, DWORD& error)
    {
        HANDLE handle = CreateFileW(devicePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        error = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        return handle;
    }

    bool GetVolumeGuidPath(const std::wstring& root, std::wstring& guidPath)
    {
        wchar_t buffer[64] = {};
        if (!GetVolumeNameForVolumeMountPointW(root.c_str(), buffer, static_cast<DWORD>(_countof(buffer))))
        {
            return false;
        }
        guidPath.assign(buffer);
        return !guidPath.empty();
    }

    void AppendNote(std::wstring& notes, const std::wstring& label, const wchar_t* text, DWORD error)
    {
        notes += L"; ";
        notes += label;
        notes += L' ';
        notes += text;
        if (error != ERROR_SUCCESS)
        {
            notes += L" [error ";
            notes += std::to_wstring(error);
            notes += L']';
        }
    }

    // Opens every fixed NTFS/ReFS drive that has an active change journal (never creates one).
    void EnumerateVolumes(std::vector<std::unique_ptr<Volume>>& volumes, std::wstring& notes)
    {
        const DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; ++i)
        {
            if ((drives & (1u << i)) == 0)
            {
                continue;
            }
            std::wstring root(1, static_cast<wchar_t>(L'A' + i));
            root += L":\\";
            if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED)
            {
                continue;
            }
            const std::wstring label = root.substr(0, 2);

            wchar_t fileSystem[MAX_PATH + 1] = {};
            if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
                                       fileSystem, static_cast<DWORD>(_countof(fileSystem))))
            {
                AppendNote(notes, label, L"skipped (volume unavailable)", GetLastError());
                continue;
            }
            if (_wcsicmp(fileSystem, L"NTFS") != 0 && _wcsicmp(fileSystem, L"ReFS") != 0)
            {
                continue;
            }

            std::wstring guidPath;
            if (!GetVolumeGuidPath(root, guidPath))
            {
                AppendNote(notes, label, L"skipped (no volume GUID)", GetLastError());
                continue;
            }
            bool duplicate = false;
            for (const auto& existing : volumes)
            {
                if (_wcsicmp(existing->volumeKey.c_str(), guidPath.c_str()) == 0)
                {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
            {
                continue;
            }

            std::unique_ptr<Volume> volume = std::make_unique<Volume>();
            volume->driveRoot = root;
            volume->devicePath = L"\\\\.\\" + label;
            volume->volumeKey = guidPath;
            volume->fileSystem = fileSystem;
            volume->extendPrefix = root + L"$Extend";
            volume->sviPrefix = root + L"System Volume Information";

            DWORD error = ERROR_SUCCESS;
            volume->handle = OpenVolume(volume->devicePath, error);
            if (volume->handle == INVALID_HANDLE_VALUE)
            {
                AppendNote(notes, label, L"skipped (cannot open volume)", error);
                continue;
            }
            JournalInfo info;
            if (!QueryJournal(volume->handle, info, error))
            {
                AppendNote(notes, label, L"skipped (no active change journal)", error);
                continue;
            }
            if (!ApplyJournal(*volume, info))
            {
                AppendNote(notes, label, L"skipped (unsupported journal record version)", ERROR_NOT_SUPPORTED);
                continue;
            }
            volume->active = true;
            volumes.push_back(std::move(volume));
        }
    }
}

struct UsnJournalReader::Impl
{
    std::mutex            lifecycleMutex;    // serializes Start/Stop
    std::thread           worker;
    HANDLE                stopEvent = nullptr;
    std::atomic<bool>     stopRequested{ false };
    std::atomic<DWORD>    workerThreadId{ 0 };
    std::atomic<uint64_t> recordsProcessed{ 0 };

    mutable std::mutex    statusMutex;
    std::wstring          status = L"stopped";

    // Written by Start() before the worker exists, then owned by the worker until joined.
    Callback              callback;
    CursorLoad            load;
    CursorSave            save;
    int                   maxBackfillHours = 0;
    int64_t               backfillCutoffMs = 0;
    std::wstring          startupNotes;
    std::vector<std::unique_ptr<Volume>> volumes;

    Impl()
    {
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~Impl()
    {
        volumes.clear();
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

    void SetStatus(std::wstring text)
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        status.swap(text);
    }

    void Run();
    void RunLoop();
    void InitCursors();
    bool PollVolume(Volume& v, std::vector<uint64_t>& buffer, ULONGLONG& lastSaveTick);
    bool ProcessBuffer(Volume& v, const BYTE* data, DWORD size, int64_t& resumeUsn);
    void HandleRecord(Volume& v, const UsnRecordView& r);
    bool BuildPath(Volume& v, const UsnRecordView& r, std::wstring& path);
    bool ResolveDirectory(Volume& v, const FileIdKey& id, bool is128, std::wstring& path);
    void Emit(Volume& v, UsnFileEvent::Kind kind, std::wstring&& path, std::wstring&& oldPath,
              const UsnRecordView& r, int64_t utcMs, bool isDirectory, bool backfill);
    void OnReadError(Volume& v, DWORD error);
    bool ResyncToHead(Volume& v);
    void Deactivate(Volume& v, bool closeHandle);
    void TryReactivate(Volume& v);
    void SaveCursors();
    void PublishStatus();
};

void UsnJournalReader::Impl::Run()
{
    workerThreadId.store(GetCurrentThreadId());
    try
    {
        RunLoop();
    }
    catch (...)
    {
        // Never let an exception escape the thread (std::terminate).
    }
    try
    {
        SaveCursors();
    }
    catch (...)
    {
    }
    for (auto& v : volumes)
    {
        v->CloseVolumeHandle();
        v->active = false;
    }
    try
    {
        SetStatus(L"stopped");
    }
    catch (...)
    {
    }
    workerThreadId.store(0);
}

void UsnJournalReader::Impl::RunLoop()
{
    std::vector<uint64_t> buffer(kReadBufferBytes / sizeof(uint64_t));   // 8-byte aligned records
    InitCursors();
    PublishStatus();

    ULONGLONG lastSaveTick = GetTickCount64();
    while (!IsStopRequested())
    {
        bool moreData = false;
        for (auto& v : volumes)
        {
            if (IsStopRequested())
            {
                break;
            }
            if (PollVolume(*v, buffer, lastSaveTick))
            {
                moreData = true;
            }
        }
        if (IsStopRequested())
        {
            break;
        }
        const ULONGLONG now = GetTickCount64();
        if (now - lastSaveTick >= kCursorSaveIntervalMs)
        {
            SaveCursors();
            lastSaveTick = now;
        }
        PublishStatus();
        if (WaitForSingleObject(stopEvent, moreData ? 0 : kPollIntervalMs) != WAIT_TIMEOUT)
        {
            break;
        }
    }
}

void UsnJournalReader::Impl::InitCursors()
{
    for (auto& vp : volumes)
    {
        Volume& v = *vp;
        v.nextUsn = v.headUsn;
        v.backfillUntilUsn = 0;
        v.cursorValid = true;
        if (!load)
        {
            continue;
        }
        uint64_t savedJournalId = 0;
        int64_t savedUsn = 0;
        bool haveCursor = false;
        try
        {
            haveCursor = load(v.volumeKey, savedJournalId, savedUsn);
        }
        catch (...)
        {
            haveCursor = false;
        }
        if (!haveCursor)
        {
            continue;
        }
        v.cursorPersisted = true;
        v.persistedJournalId = savedJournalId;
        v.persistedUsn = savedUsn;
        if (maxBackfillHours <= 0 || savedJournalId != v.journalId)
        {
            continue;   // backfill disabled, or the journal was re-created: start at the head
        }
        if (savedUsn < v.firstUsn || savedUsn < v.lowestValidUsn || savedUsn > v.headUsn)
        {
            continue;   // records were purged (or the cursor is bogus): start at the head
        }
        v.nextUsn = savedUsn;
        v.backfillUntilUsn = v.headUsn;
    }
}

// Returns true when the per-pass read budget was exhausted and more records are pending.
bool UsnJournalReader::Impl::PollVolume(Volume& v, std::vector<uint64_t>& buffer, ULONGLONG& lastSaveTick)
{
    if (!v.active)
    {
        if (GetTickCount64() < v.retryAtTick)
        {
            return false;
        }
        TryReactivate(v);
        if (!v.active)
        {
            return false;
        }
    }

    BYTE* const data = reinterpret_cast<BYTE*>(buffer.data());
    const DWORD capacity = static_cast<DWORD>(buffer.size() * sizeof(uint64_t));
    for (int read = 0; read < kMaxReadsPerPass; ++read)
    {
        if (IsStopRequested())
        {
            return false;
        }
        const int64_t startUsn = v.nextUsn;
        DWORD bytes = 0;
        BOOL ok = FALSE;
        if (v.useReadV1)
        {
            READ_USN_JOURNAL_DATA_V1 request = {};
            request.StartUsn = startUsn;
            request.ReasonMask = kReasonMask;
            request.ReturnOnlyOnClose = FALSE;
            request.Timeout = 0;
            request.BytesToWaitFor = 0;
            request.UsnJournalID = v.journalId;
            request.MinMajorVersion = v.minMajor;
            request.MaxMajorVersion = v.maxMajor;
            ok = DeviceIoControl(v.handle, FSCTL_READ_USN_JOURNAL, &request, sizeof(request),
                                 data, capacity, &bytes, nullptr);
        }
        else
        {
            READ_USN_JOURNAL_DATA_V0 request = {};
            request.StartUsn = startUsn;
            request.ReasonMask = kReasonMask;
            request.ReturnOnlyOnClose = FALSE;
            request.Timeout = 0;
            request.BytesToWaitFor = 0;
            request.UsnJournalID = v.journalId;
            ok = DeviceIoControl(v.handle, FSCTL_READ_USN_JOURNAL, &request, sizeof(request),
                                 data, capacity, &bytes, nullptr);
        }
        if (!ok)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_HANDLE_EOF)   // EOF only means "no more records"
            {
                OnReadError(v, error);
            }
            return false;
        }
        v.consecutiveErrors = 0;
        if (bytes < sizeof(USN))
        {
            return false;
        }
        USN next = 0;
        memcpy(&next, data, sizeof(USN));

        int64_t resumeUsn = 0;
        if (ProcessBuffer(v, data + sizeof(USN), bytes - static_cast<DWORD>(sizeof(USN)), resumeUsn))
        {
            v.nextUsn = resumeUsn;   // stopped mid-buffer: resume at the first unprocessed record
            return false;
        }
        if (next <= startUsn)
        {
            return false;   // caught up
        }
        v.nextUsn = next;

        const ULONGLONG now = GetTickCount64();
        if (now - lastSaveTick >= kCursorSaveIntervalMs)
        {
            SaveCursors();
            lastSaveTick = now;
        }
    }
    return true;
}

// Returns true when interrupted by Stop(); resumeUsn then names the first unprocessed record.
bool UsnJournalReader::Impl::ProcessBuffer(Volume& v, const BYTE* data, DWORD size, int64_t& resumeUsn)
{
    DWORD offset = 0;
    while (size - offset >= kRecordHeaderBytes)
    {
        const BYTE* record = data + offset;
        DWORD length = 0;
        memcpy(&length, record, sizeof(length));
        if (length < kRecordHeaderBytes || length > size - offset)
        {
            break;   // malformed; the cursor still advances to the returned next USN
        }
        UsnRecordView view;
        if (ParseRecord(record, length, view))
        {
            if (IsStopRequested())
            {
                resumeUsn = view.usn;
                return true;
            }
            recordsProcessed.fetch_add(1, std::memory_order_relaxed);
            try
            {
                HandleRecord(v, view);
            }
            catch (...)
            {
                // A record that cannot be handled (e.g. out of memory) is skipped.
            }
        }
        offset += length;
    }
    return false;
}

void UsnJournalReader::Impl::HandleRecord(Volume& v, const UsnRecordView& r)
{
    const bool renameOld = (r.reason & USN_REASON_RENAME_OLD_NAME) != 0;
    const bool renameNew = (r.reason & USN_REASON_RENAME_NEW_NAME) != 0;
    const bool closed = (r.reason & USN_REASON_CLOSE) != 0;
    if (!renameOld && !renameNew && !closed)
    {
        return;
    }

    const bool backfill = r.usn < v.backfillUntilUsn;
    const int64_t utcMs = FileTimeToUnixMs(r.timeStamp);
    if (backfill && utcMs < backfillCutoffMs)
    {
        return;   // older than the backfill window
    }
    const bool isDirectory = (r.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if (renameOld)
    {
        std::wstring oldPath;
        if (BuildPath(v, r, oldPath))
        {
            if (v.pendingRenames.size() >= kMaxPendingRenames)
            {
                v.pendingRenames.clear();
            }
            v.pendingRenames[r.fileId] = std::move(oldPath);
        }
        else
        {
            v.pendingRenames.erase(r.fileId);
        }
    }

    if (renameNew)
    {
        auto pending = v.pendingRenames.find(r.fileId);
        if (pending != v.pendingRenames.end())
        {
            std::wstring oldPath = std::move(pending->second);
            v.pendingRenames.erase(pending);
            std::wstring newPath;
            if (BuildPath(v, r, newPath))
            {
                if (isDirectory)
                {
                    // Cached paths of the directory and everything below it are now stale.
                    v.dirCache.EraseSameOrUnder(oldPath);
                    v.dirCache.Put(r.fileId, newPath);
                }
                if (!IsExcluded(v, oldPath) && !IsExcluded(v, newPath))
                {
                    Emit(v, UsnFileEvent::Kind::Renamed, std::move(newPath), std::move(oldPath),
                         r, utcMs, isDirectory, backfill);
                }
            }
        }
        else if (isDirectory && !closed)
        {
            // The old-name record was not seen (e.g. started mid-rename): drop stale cache entries.
            std::wstring stale;
            if (v.dirCache.Peek(r.fileId, stale))
            {
                v.dirCache.EraseSameOrUnder(stale);
            }
        }
    }

    if (!closed)
    {
        return;
    }

    // One event per close; reason flags accumulate over the open session.
    UsnFileEvent::Kind kind = UsnFileEvent::Kind::Modified;
    if ((r.reason & USN_REASON_FILE_DELETE) != 0)
    {
        kind = UsnFileEvent::Kind::Deleted;
    }
    else if ((r.reason & USN_REASON_FILE_CREATE) != 0)
    {
        kind = UsnFileEvent::Kind::Created;
    }
    else if ((r.reason & kDataReasons) == 0 || (r.sourceInfo & kNonUserSources) != 0)
    {
        return;
    }

    std::wstring path;
    if (!BuildPath(v, r, path))
    {
        return;
    }
    if (isDirectory)
    {
        if (kind == UsnFileEvent::Kind::Deleted)
        {
            v.dirCache.Erase(r.fileId);
        }
        else
        {
            v.dirCache.Put(r.fileId, path);
        }
    }
    if (IsExcluded(v, path))
    {
        return;
    }
    Emit(v, kind, std::move(path), std::wstring(), r, utcMs, isDirectory, backfill);
}

bool UsnJournalReader::Impl::BuildPath(Volume& v, const UsnRecordView& r, std::wstring& path)
{
    std::wstring directory;
    if (!ResolveDirectory(v, r.parentId, r.is128, directory))
    {
        return false;
    }
    path.swap(directory);
    if (path.empty() || path.back() != L'\\')
    {
        path.push_back(L'\\');
    }
    path.append(r.name, r.nameLength);
    return true;
}

bool UsnJournalReader::Impl::ResolveDirectory(Volume& v, const FileIdKey& id, bool is128, std::wstring& path)
{
    if (v.dirCache.Get(id, path))
    {
        return true;
    }
    if (v.handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    FILE_ID_DESCRIPTOR descriptor = {};
    descriptor.dwSize = sizeof(descriptor);
    if (is128)
    {
        descriptor.Type = ExtendedFileIdType;
        memcpy(descriptor.ExtendedFileId.Identifier, &id.lo, sizeof(uint64_t));
        memcpy(descriptor.ExtendedFileId.Identifier + sizeof(uint64_t), &id.hi, sizeof(uint64_t));
    }
    else
    {
        descriptor.Type = FileIdType;
        descriptor.FileId.QuadPart = static_cast<LONGLONG>(id.lo);
    }

    // No access rights are needed to query the name; backup semantics allow opening directories.
    HANDLE directory = OpenFileById(v.handle, &descriptor, 0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, FILE_FLAG_BACKUP_SEMANTICS);
    if (directory == INVALID_HANDLE_VALUE)
    {
        return false;   // parent already deleted, or inaccessible
    }
    std::wstring resolved;
    const bool ok = GetPathFromHandle(directory, resolved);
    CloseHandle(directory);
    if (!ok)
    {
        return false;
    }
    v.dirCache.Put(id, resolved);
    path.swap(resolved);
    return true;
}

void UsnJournalReader::Impl::Emit(Volume& v, UsnFileEvent::Kind kind, std::wstring&& path, std::wstring&& oldPath,
                                  const UsnRecordView& r, int64_t utcMs, bool isDirectory, bool backfill)
{
    UsnFileEvent ev;
    ev.kind = kind;
    ev.path = std::move(path);
    ev.oldPath = std::move(oldPath);
    ev.utcMs = utcMs;
    ev.fileRef = FoldFileId(r.fileId);
    ev.volume = v.volumeKey;
    ev.isDirectory = isDirectory;
    ev.backfill = backfill;
    ++v.eventsEmitted;
    try
    {
        callback(ev);
    }
    catch (...)
    {
        // Consumer exceptions must not stop the reader.
    }
}

void UsnJournalReader::Impl::OnReadError(Volume& v, DWORD error)
{
    v.lastError = error;
    if (error == ERROR_INVALID_PARAMETER && !v.useReadV1 && !v.triedReadV1)
    {
        // The journal rejected the V0 request (cannot produce V2 records): retry with V1.
        v.triedReadV1 = true;
        v.useReadV1 = true;
        v.minMajor = 2;
        v.maxMajor = 3;
        return;
    }
    if (++v.consecutiveErrors >= kMaxConsecutiveErrors)
    {
        v.consecutiveErrors = 0;
        Deactivate(v, false);
        return;
    }
    if (error == ERROR_JOURNAL_ENTRY_DELETED || error == ERROR_JOURNAL_DELETE_IN_PROGRESS ||
        error == ERROR_JOURNAL_NOT_ACTIVE || error == ERROR_INVALID_PARAMETER)
    {
        // Cursor purged, journal reset or re-created (new id): continue from the current head.
        if (!ResyncToHead(v))
        {
            Deactivate(v, false);
        }
        return;
    }
    Deactivate(v, true);   // volume gone or handle unusable: reopen later
}

bool UsnJournalReader::Impl::ResyncToHead(Volume& v)
{
    if (v.handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    JournalInfo info;
    DWORD error = ERROR_SUCCESS;
    if (!QueryJournal(v.handle, info, error))
    {
        v.lastError = error;
        return false;
    }
    if (!ApplyJournal(v, info))
    {
        return false;
    }
    v.nextUsn = info.nextUsn;
    v.backfillUntilUsn = 0;
    v.cursorValid = true;
    v.pendingRenames.clear();
    v.active = true;
    return true;
}

void UsnJournalReader::Impl::Deactivate(Volume& v, bool closeHandle)
{
    v.active = false;
    v.retryAtTick = GetTickCount64() + kRetryIntervalMs;
    v.pendingRenames.clear();
    if (closeHandle)
    {
        v.CloseVolumeHandle();
        v.dirCache.Clear();
    }
}

void UsnJournalReader::Impl::TryReactivate(Volume& v)
{
    v.retryAtTick = GetTickCount64() + kRetryIntervalMs;
    if (v.handle == INVALID_HANDLE_VALUE)
    {
        // Only reopen if the drive letter still names the same volume.
        std::wstring guidPath;
        if (!GetVolumeGuidPath(v.driveRoot, guidPath) || _wcsicmp(guidPath.c_str(), v.volumeKey.c_str()) != 0)
        {
            v.lastError = ERROR_DEV_NOT_EXIST;
            return;
        }
        DWORD error = ERROR_SUCCESS;
        v.handle = OpenVolume(v.devicePath, error);
        if (v.handle == INVALID_HANDLE_VALUE)
        {
            v.lastError = error;
            return;
        }
    }
    ResyncToHead(v);
}

void UsnJournalReader::Impl::SaveCursors()
{
    if (!save)
    {
        return;
    }
    for (auto& vp : volumes)
    {
        Volume& v = *vp;
        if (!v.cursorValid)
        {
            continue;
        }
        if (v.cursorPersisted && v.persistedJournalId == v.journalId && v.persistedUsn == v.nextUsn)
        {
            continue;
        }
        try
        {
            save(v.volumeKey, v.journalId, v.nextUsn);
        }
        catch (...)
        {
        }
        v.cursorPersisted = true;
        v.persistedJournalId = v.journalId;
        v.persistedUsn = v.nextUsn;
    }
}

void UsnJournalReader::Impl::PublishStatus()
{
    std::wstring text = L"running";
    for (const auto& vp : volumes)
    {
        const Volume& v = *vp;
        text += L"; ";
        text += v.driveRoot.substr(0, 2);
        text += L' ';
        text += v.fileSystem;
        if (!v.active)
        {
            text += L" inactive [error ";
            text += std::to_wstring(v.lastError);
            text += L']';
        }
        else
        {
            text += v.nextUsn < v.backfillUntilUsn ? L" backfilling" : L" live";
            text += L" journal=";
            text += Hex64(v.journalId);
            text += L" usn=";
            text += Hex64(static_cast<uint64_t>(v.nextUsn));
        }
        text += L" events=";
        text += std::to_wstring(v.eventsEmitted);
    }
    text += L"; records=";
    text += std::to_wstring(recordsProcessed.load());
    text += startupNotes;
    SetStatus(std::move(text));
}

UsnJournalReader::UsnJournalReader()
    : m_impl(new Impl())
{
}

UsnJournalReader::~UsnJournalReader()
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

bool UsnJournalReader::Start(Callback cb, CursorLoad load, CursorSave save, int maxBackfillHours)
{
    Impl* impl = m_impl.get();
    if (impl == nullptr || !cb)
    {
        return false;
    }
    if (impl->workerThreadId.load() == GetCurrentThreadId())
    {
        return false;   // called from one of our own callbacks
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
    if (impl->stopEvent == nullptr)
    {
        impl->SetStatus(L"error: cannot create stop event");
        return false;
    }

    std::vector<std::unique_ptr<Volume>> volumes;
    std::wstring notes;
    EnumerateVolumes(volumes, notes);
    if (volumes.empty())
    {
        impl->SetStatus(L"idle: no fixed NTFS/ReFS volume with an active change journal" + notes);
        return false;
    }

    impl->volumes = std::move(volumes);
    impl->startupNotes = std::move(notes);
    impl->callback = std::move(cb);
    impl->load = std::move(load);
    impl->save = std::move(save);
    impl->maxBackfillHours = maxBackfillHours;
    impl->backfillCutoffMs = maxBackfillHours > 0
        ? NowUtcMs() - static_cast<int64_t>(maxBackfillHours) * kMsPerHour
        : NowUtcMs();
    impl->recordsProcessed.store(0);
    impl->stopRequested.store(false);
    ResetEvent(impl->stopEvent);
    impl->SetStatus(L"starting");

    try
    {
        impl->worker = std::thread(&Impl::Run, impl);
    }
    catch (const std::system_error&)
    {
        impl->volumes.clear();
        impl->callback = nullptr;
        impl->load = nullptr;
        impl->save = nullptr;
        impl->SetStatus(L"error: cannot start worker thread");
        return false;
    }
    return true;
}

void UsnJournalReader::Stop()
{
    Impl* impl = m_impl.get();
    if (impl == nullptr)
    {
        return;
    }
    if (impl->workerThreadId.load() == GetCurrentThreadId())
    {
        impl->RequestStop();   // from a callback: the worker exits after it returns
        return;
    }

    std::lock_guard<std::mutex> lock(impl->lifecycleMutex);
    impl->RequestStop();
    if (!impl->worker.joinable())
    {
        return;
    }
    impl->JoinWorker();
    impl->volumes.clear();
    impl->callback = nullptr;
    impl->load = nullptr;
    impl->save = nullptr;
}

uint64_t UsnJournalReader::RecordsProcessed() const
{
    return m_impl ? m_impl->recordsProcessed.load() : 0;
}

std::wstring UsnJournalReader::Status() const
{
    if (!m_impl)
    {
        return std::wstring(L"stopped");
    }
    std::lock_guard<std::mutex> lock(m_impl->statusMutex);
    return m_impl->status;
}
