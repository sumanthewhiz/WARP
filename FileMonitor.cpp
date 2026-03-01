#include "framework.h"
#include "framework.h"
#include "FileMonitor.h"
#include <shlobj.h>
#include <algorithm>
#include <evntrace.h>
#include <evntcons.h>
#include <unordered_map>
#include <mutex>

#pragma comment(lib, "advapi32.lib")

// Static instance pointer for the ETW callback
static FileMonitor* g_pFileMonitor = nullptr;

static const wchar_t* WARP_ETW_SESSION_NAME = L"NT Kernel Logger";

// SystemTraceControlGuid — required as Wnode.Guid for NT Kernel Logger
// {9E814AAD-3204-11D2-9A82-006008A86939}
static const GUID SystemTraceControlGuid =
    { 0x9E814AAD, 0x3204, 0x11D2, { 0x9A, 0x82, 0x00, 0x60, 0x08, 0xA8, 0x69, 0x39 } };

// Classic FileIo GUID — events in the NT Kernel Logger use this provider GUID
// {90CBDC39-4A3E-11D1-84F4-0000F80464E3}
static const GUID FileIoGuid =
    { 0x90CBDC39, 0x4A3E, 0x11D1, { 0x84, 0xF4, 0x00, 0x00, 0xF8, 0x04, 0x64, 0xE3 } };

// Cached device-path-to-drive-letter map (built once, used in callback)
static std::unordered_map<std::wstring, std::wstring> g_deviceToDrive;
static std::once_flag g_deviceMapOnce;

// Our own PID — used to skip self-generated file-open events
static DWORD g_ownPid = 0;

static void BuildDeviceMap()
{
    wchar_t drives[512] = {};
    if (GetLogicalDriveStringsW(511, drives) == 0) return;
    for (const wchar_t* drv = drives; *drv; drv += wcslen(drv) + 1)
    {
        wchar_t letter[3] = { drv[0], drv[1], L'\0' };
        wchar_t target[MAX_PATH] = {};
        if (QueryDosDeviceW(letter, target, MAX_PATH) > 0)
        {
            std::wstring key(target);
            std::transform(key.begin(), key.end(), key.begin(), ::towlower);
            g_deviceToDrive[key] = letter;
        }
    }
}


// ---------- Path exclusion filter ----------
// Returns true if the path should be excluded from activity recording.
bool ShouldExclude(const std::wstring& path)
{
    if (path.empty())
        return true;

    // Work with a lowercase copy for case-insensitive matching
    std::wstring lp = path;
    std::transform(lp.begin(), lp.end(), lp.begin(), ::towlower);

    // --- Excluded directory prefixes (system, cache, temp, build artifacts) ---
    // Patterns with trailing backslash match files/folders INSIDE these directories.
    // Patterns without trailing backslash match the directory itself (for folder-open events).
    static const wchar_t* const excludedDirs[] = {
        L"\\windows\\",
        L"\\$recycle.bin\\",
        L"\\system volume information\\",
        L"\\programdata\\",
        L"\\program files\\",
        L"\\program files (x86)\\",
        L"\\appdata\\",
        L"\\node_modules\\",
        L"\\__pycache__\\",
        L"\\recovery\\",
        L"\\msocache\\",
        L"\\config.msi\\",
    };

    for (const auto* dir : excludedDirs)
    {
        if (lp.find(dir) != std::wstring::npos)
            return true;
    }

    // Also exclude the system folders themselves (path ends with the folder name,
    // no trailing backslash — happens when the folder is opened directly via ETW).
    static const wchar_t* const excludedDirNames[] = {
        L"\\windows",
        L"\\$recycle.bin",
        L"\\system volume information",
        L"\\programdata",
        L"\\program files",
        L"\\program files (x86)",
        L"\\appdata",
        L"\\node_modules",
        L"\\__pycache__",
        L"\\recovery",
        L"\\msocache",
        L"\\config.msi",
    };

    for (const auto* dn : excludedDirNames)
    {
        size_t dnLen = wcslen(dn);
        if (lp.size() >= dnLen && lp.compare(lp.size() - dnLen, dnLen, dn) == 0)
            return true;
    }

    // Exclude any folder whose name starts with a dot (e.g. \.git\, \.vs\, \.ssh\, \.vscode\)
    {
        size_t pos = 0;
        while ((pos = lp.find(L"\\.", pos)) != std::wstring::npos)
        {
            // Make sure there is at least one more char after the dot that isn't a backslash
            if (pos + 2 < lp.size() && lp[pos + 2] != L'\\')
                return true;
            pos += 2;
        }
    }

    // --- Excluded file extensions ---
    static const wchar_t* const excludedExts[] = {
        L".exe", L".dll", L".sys", L".drv", L".ocx",
        L".tmp", L".temp", L".etl", L".evtx",
        L".pf", L".cache", L".diagsession",
        L".obj", L".pch", L".ipch", L".ilk", L".pdb",
        L".tlog", L".idb", L".res", L".aps",
        L".suo", L".sdf", L".opensdf",
        L".log", L".log1", L".bak",
        L".lock", L".lck",
        L".db", L".db-wal", L".db-shm", L".sqlite",
        L".dat", L".metadata",
        L"thumbs.db", L"desktop.ini",
    };

    // Find the last dot for extension check
    size_t dotPos = lp.rfind(L'.');
    if (dotPos != std::wstring::npos)
    {
        std::wstring ext = lp.substr(dotPos);
        for (const auto* e : excludedExts)
        {
            if (ext == e)
                return true;
        }
    }

    // Also check full filename for known system files without extensions containing a dot
    size_t slashPos = lp.rfind(L'\\');
    if (slashPos != std::wstring::npos)
    {
        std::wstring filename = lp.substr(slashPos + 1);
        if (filename == L"thumbs.db" ||
            filename == L"desktop.ini" ||
            filename == L"ntuser.dat" ||
            filename == L"usrclass.dat" ||
            filename == L"pagefile.sys" ||
            filename == L"swapfile.sys" ||
            filename == L"hiberfil.sys")
        {
            return true;
        }
    }

    return false;
}

FileMonitor::FileMonitor()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

FileMonitor::~FileMonitor()
{
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

void FileMonitor::SetCallback(FileActivityCallback cb)
{
    m_callback = std::move(cb);
}

std::vector<std::wstring> FileMonitor::GetDriveRoots()
{
    std::vector<std::wstring> roots;
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
    {
        if (drives & (1 << i))
        {
            wchar_t root[4] = { static_cast<wchar_t>(L'A' + i), L':', L'\\', L'\0' };
            UINT type = GetDriveTypeW(root);
            if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_REMOTE)
            {
                roots.push_back(root);
            }
        }
    }
    return roots;
}

void FileMonitor::Start()
{
    if (m_running) return;
    m_running = true;
    ResetEvent(m_stopEvent);

    auto roots = GetDriveRoots();
    for (const auto& root : roots)
    {
        m_threads.emplace_back(&FileMonitor::MonitorDrive, this, root);
    }

    // Also start a shell notification watcher thread
    m_threads.emplace_back(&FileMonitor::MonitorShellNotifications, this);

    // Start ETW trace for file-open events
    m_threads.emplace_back(&FileMonitor::StartEtwTrace, this);
}

void FileMonitor::Stop()
{
    if (!m_running) return;
    m_running = false;
    SetEvent(m_stopEvent);

    // Stop ETW trace so ProcessTrace unblocks
    StopEtwTrace();

    for (auto& t : m_threads)
    {
        if (t.joinable()) t.join();
    }
    m_threads.clear();
}

void FileMonitor::Pause()
{
    m_paused = true;
}

void FileMonitor::Resume()
{
    m_paused = false;
}

void FileMonitor::MonitorDrive(const std::wstring& root)
{
    HANDLE hDir = CreateFileW(
        root.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (hDir == INVALID_HANDLE_VALUE)
        return;

    const DWORD bufSize = 64 * 1024;
    std::vector<BYTE> buffer(bufSize);

    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent)
    {
        CloseHandle(hDir);
        return;
    }

    HANDLE waitHandles[2] = { overlapped.hEvent, m_stopEvent };

    const DWORD filter =
        FILE_NOTIFY_CHANGE_FILE_NAME |
        FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_ATTRIBUTES |
        FILE_NOTIFY_CHANGE_SIZE |
        FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_CREATION |
        FILE_NOTIFY_CHANGE_SECURITY;

    while (m_running)
    {
        ResetEvent(overlapped.hEvent);
        DWORD bytesReturned = 0;

        BOOL ok = ReadDirectoryChangesW(
            hDir, buffer.data(), bufSize, TRUE,
            filter, &bytesReturned, &overlapped, nullptr);

        if (!ok)
            break;

        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0 + 1) // stop event
            break;
        if (waitResult != WAIT_OBJECT_0)
            break;

        if (!GetOverlappedResult(hDir, &overlapped, &bytesReturned, FALSE))
            continue;
        if (bytesReturned == 0)
            continue;

        if (m_paused)
            continue;

        BYTE* ptr = buffer.data();
        while (true)
        {
            auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(ptr);
            std::wstring fileName(info->FileName, info->FileNameLength / sizeof(wchar_t));
            std::wstring fullPath = root + fileName;

            std::wstring action;
            std::wstring oldPath;

            switch (info->Action)
            {
            case FILE_ACTION_ADDED:
                action = L"CREATE";
                break;
            case FILE_ACTION_REMOVED:
                action = L"DELETE";
                break;
            case FILE_ACTION_MODIFIED:
                action = L"MODIFY";
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                oldPath = fullPath;
                // The next entry will be RENAMED_NEW_NAME
                if (info->NextEntryOffset != 0)
                {
                    auto* next = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(ptr + info->NextEntryOffset);
                    if (next->Action == FILE_ACTION_RENAMED_NEW_NAME)
                    {
                        std::wstring newName(next->FileName, next->FileNameLength / sizeof(wchar_t));
                        fullPath = root + newName;
                        action = L"RENAME";
                    }
                }
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                // Handled as part of OLD_NAME above; skip standalone
                action.clear();
                break;
            default:
                action.clear();
                break;
            }

            if (!action.empty() && m_callback &&
                !ShouldExclude(fullPath) &&
                (oldPath.empty() || !ShouldExclude(oldPath)))
            {
                m_callback(action, fullPath, oldPath);
            }

            if (info->NextEntryOffset == 0)
                break;

            // Skip ahead – but if we already consumed the next entry for a rename, skip it
            if (info->Action == FILE_ACTION_RENAMED_OLD_NAME && info->NextEntryOffset != 0)
            {
                auto* next = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(ptr + info->NextEntryOffset);
                if (next->Action == FILE_ACTION_RENAMED_NEW_NAME)
                {
                    if (next->NextEntryOffset == 0)
                        break;
                    ptr = ptr + info->NextEntryOffset + next->NextEntryOffset;
                    continue;
                }
            }

            ptr += info->NextEntryOffset;
        }
    }

    CancelIo(hDir);
    CloseHandle(overlapped.hEvent);
    CloseHandle(hDir);
}

void FileMonitor::MonitorShellNotifications()
{
    // Use SHChangeNotifyRegister to catch shell-level operations (copy, move, etc.)
    // This requires a hidden message-only window.

    const wchar_t* className = L"WarpShellMonWnd";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!hWnd) return;

    SHChangeNotifyEntry entries[1] = {};
    PIDLIST_ABSOLUTE pidlDesktop = nullptr;
    SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &pidlDesktop);

    // Watch entire namespace
    LPITEMIDLIST pidlRoot = nullptr;
    SHGetFolderLocation(nullptr, CSIDL_DESKTOP, nullptr, 0, &pidlRoot);

    entries[0].pidl = pidlRoot;
    entries[0].fRecursive = TRUE;

    ULONG regId = SHChangeNotifyRegister(
        hWnd,
        SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
        SHCNE_ALLEVENTS,
        WM_USER + 100,
        1, entries);

    MSG msg;
    while (m_running)
    {
        // Use MsgWaitForMultipleObjects so we can also detect the stop event
        DWORD result = MsgWaitForMultipleObjects(1, &m_stopEvent, FALSE, 500, QS_ALLINPUT);
        if (result == WAIT_OBJECT_0) // stop event
            break;

        while (PeekMessageW(&msg, hWnd, 0, 0, PM_REMOVE))
        {
            if (m_paused) continue;

            if (msg.message == WM_USER + 100)
            {
                LONG event = 0;
                PIDLIST_ABSOLUTE* pidls = nullptr;
                HANDLE hLock = SHChangeNotification_Lock(
                    reinterpret_cast<HANDLE>(msg.wParam),
                    static_cast<DWORD>(msg.lParam), &pidls, &event);

                if (hLock)
                {
                    std::wstring action;
                    std::wstring path;
                    std::wstring oldPath;

                    wchar_t szPath1[MAX_PATH] = {};
                    wchar_t szPath2[MAX_PATH] = {};

                    if (pidls && pidls[0])
                        SHGetPathFromIDListW(pidls[0], szPath1);
                    if (pidls && pidls[1])
                        SHGetPathFromIDListW(pidls[1], szPath2);

                    switch (event)
                    {
                    case SHCNE_CREATE:
                    case SHCNE_MKDIR:
                        action = L"CREATE";
                        path = szPath1;
                        break;
                    case SHCNE_DELETE:
                    case SHCNE_RMDIR:
                        action = L"DELETE";
                        path = szPath1;
                        break;
                    case SHCNE_RENAMEITEM:
                    case SHCNE_RENAMEFOLDER:
                        action = L"RENAME";
                        oldPath = szPath1;
                        path = szPath2;
                        break;
                    case SHCNE_UPDATEITEM:
                    case SHCNE_UPDATEDIR:
                        action = L"MODIFY";
                        path = szPath1;
                        break;
                    case SHCNE_ATTRIBUTES:
                        action = L"MODIFY";
                        path = szPath1;
                        break;
                    default:
                        break;
                    }

                    if (!action.empty() && !path.empty() && m_callback &&
                        !ShouldExclude(path) &&
                        (oldPath.empty() || !ShouldExclude(oldPath)))
                    {
                        m_callback(action, path, oldPath);
                    }

                    SHChangeNotification_Unlock(hLock);
                }
            }

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (regId)
        SHChangeNotifyDeregister(regId);

    if (pidlRoot)
        CoTaskMemFree(pidlRoot);
    if (pidlDesktop)
        CoTaskMemFree(pidlDesktop);

    DestroyWindow(hWnd);
    UnregisterClassW(className, wc.hInstance);
}

// ---------- ETW: Capture file-open events via Microsoft-Windows-Kernel-File ----------

void FileMonitor::StartEtwTrace()
{
    g_pFileMonitor = this;
    g_ownPid = GetCurrentProcessId();

    // Stop any stale NT Kernel Logger session
    StopEtwTrace();

    // NT Kernel Logger requires a larger properties buffer for the session name
    const size_t sessionNameLen = (wcslen(WARP_ETW_SESSION_NAME) + 1) * sizeof(wchar_t);
    const size_t bufSize = sizeof(EVENT_TRACE_PROPERTIES) + sessionNameLen + 1024;
    std::vector<BYTE> propsBuf(bufSize, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuf.data());

    props->Wnode.BufferSize = static_cast<ULONG>(bufSize);
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1; // QPC clock
    props->Wnode.Guid = SystemTraceControlGuid;
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    // Enable file I/O init events — this captures every NtCreateFile / NtOpenFile call
    props->EnableFlags = EVENT_TRACE_FLAG_FILE_IO_INIT;

    ULONG status = StartTraceW(&m_etwSessionHandle, WARP_ETW_SESSION_NAME, props);
    if (status == ERROR_ALREADY_EXISTS)
    {
        // Session already running (possibly from a previous crash) — stop and retry
        StopEtwTrace();
        memset(propsBuf.data(), 0, bufSize);
        props->Wnode.BufferSize = static_cast<ULONG>(bufSize);
        props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        props->Wnode.ClientContext = 1;
        props->Wnode.Guid = SystemTraceControlGuid;
        props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        props->EnableFlags = EVENT_TRACE_FLAG_FILE_IO_INIT;
        status = StartTraceW(&m_etwSessionHandle, WARP_ETW_SESSION_NAME, props);
    }
    if (status != ERROR_SUCCESS)
    {
        m_etwSessionHandle = 0;
        return;
    }

    // Open the trace for real-time consuming
    EVENT_TRACE_LOGFILEW logFile = {};
    logFile.LoggerName = const_cast<LPWSTR>(WARP_ETW_SESSION_NAME);
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = &FileMonitor::EtwEventCallback;

    m_etwTraceHandle = OpenTraceW(&logFile);
    if (m_etwTraceHandle == INVALID_PROCESSTRACE_HANDLE)
    {
        StopEtwTrace();
        return;
    }

    // ProcessTrace blocks until the session is stopped
    ProcessTrace(&m_etwTraceHandle, 1, nullptr, nullptr);

    if (m_etwTraceHandle != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(m_etwTraceHandle);
        m_etwTraceHandle = INVALID_PROCESSTRACE_HANDLE;
    }
}

void FileMonitor::StopEtwTrace()
{
    if (m_etwTraceHandle != INVALID_PROCESSTRACE_HANDLE)
    {
        CloseTrace(m_etwTraceHandle);
        m_etwTraceHandle = INVALID_PROCESSTRACE_HANDLE;
    }

    const size_t sessionNameLen = (wcslen(WARP_ETW_SESSION_NAME) + 1) * sizeof(wchar_t);
    const size_t bufSize = sizeof(EVENT_TRACE_PROPERTIES) + sessionNameLen + 1024;
    std::vector<BYTE> propsBuf(bufSize, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuf.data());
    props->Wnode.BufferSize = static_cast<ULONG>(bufSize);
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    ControlTraceW(0, WARP_ETW_SESSION_NAME, props, EVENT_TRACE_CONTROL_STOP);
    m_etwSessionHandle = 0;
}

void WINAPI FileMonitor::EtwEventCallback(PEVENT_RECORD pEvent)
{
    FileMonitor* self = g_pFileMonitor;
    if (!self || !self->m_running || self->m_paused || !self->m_callback)
        return;

    // NT Kernel Logger FileIo events use the classic FileIo GUID
    if (!IsEqualGUID(pEvent->EventHeader.ProviderId, FileIoGuid))
        return;

    // Opcode 64 = FileIoCreate (fires on every NtCreateFile / NtOpenFile)
    if (pEvent->EventHeader.EventDescriptor.Opcode != 64)
        return;

    // Skip events from our own process
    if (pEvent->EventHeader.ProcessId == g_ownPid)
        return;

    // FileIoCreate UserData layout (64-bit):
    //   UINT_PTR IrpPtr;          // 8 bytes
    //   UINT_PTR FileObject;      // 8 bytes
    //   UINT32   TTID;            // 4 bytes
    //   UINT32   CreateOptions;   // 4 bytes
    //   UINT32   FileAttributes;  // 4 bytes
    //   UINT32   ShareAccess;     // 4 bytes
    //   WCHAR    OpenPath[];      // null-terminated
    //
    // On 32-bit, IrpPtr and FileObject are 4 bytes each.
    DWORD ptrSize = (pEvent->EventHeader.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER) ? 8 : 4;
    DWORD headerSize = ptrSize + ptrSize + 4 + 4 + 4 + 4; // IrpPtr + FileObject + TTID + CreateOptions + FileAttributes + ShareAccess

    if (!pEvent->UserData || pEvent->UserDataLength <= headerSize + sizeof(wchar_t))
        return;

    const wchar_t* namePtr = reinterpret_cast<const wchar_t*>(
        static_cast<BYTE*>(pEvent->UserData) + headerSize);
    size_t maxChars = (pEvent->UserDataLength - headerSize) / sizeof(wchar_t);

    size_t nameLen = 0;
    while (nameLen < maxChars && namePtr[nameLen] != L'\0')
        ++nameLen;

    if (nameLen == 0)
        return;

    std::wstring filePath(namePtr, nameLen);

    // Convert kernel device paths (\Device\HarddiskVolumeN\...) to DOS paths (C:\...)
    if (filePath.size() > 8 && filePath[0] == L'\\')
    {
        size_t thirdSlash = filePath.find(L'\\', 8);
        if (thirdSlash == std::wstring::npos)
            return;

        std::wstring devicePart = filePath.substr(0, thirdSlash);
        std::transform(devicePart.begin(), devicePart.end(), devicePart.begin(), ::towlower);

        std::call_once(g_deviceMapOnce, BuildDeviceMap);

        auto it = g_deviceToDrive.find(devicePart);
        if (it == g_deviceToDrive.end())
            return;

        filePath = it->second + filePath.substr(thirdSlash);
    }
    else if (filePath.size() > 2 && filePath[1] == L':')
    {
        // Already a DOS path
    }
    else
    {
        return;
    }

    // Skip bare drive roots (e.g. "C:\") — not meaningful user activity
    if (filePath.size() <= 3)
        return;

    // Normalize: strip trailing backslash for consistent dedup and exclusion checks
    if (filePath.size() > 3 && filePath.back() == L'\\')
        filePath.pop_back();

    // Apply exclusion filter
    extern bool ShouldExclude(const std::wstring& path);
    if (ShouldExclude(filePath))
        return;

    // Deduplicate: suppress repeated OPEN events for the same path within 2 seconds
    {
        static std::mutex dedup_mtx;
        static std::unordered_map<std::wstring, ULONGLONG> dedup_map;
        static ULONGLONG dedup_lastCleanup = 0;

        ULONGLONG now = GetTickCount64();
        std::lock_guard<std::mutex> lock(dedup_mtx);

        // Periodic cleanup every 30 seconds to prevent unbounded growth
        if (now - dedup_lastCleanup > 30000)
        {
            for (auto it = dedup_map.begin(); it != dedup_map.end(); )
            {
                if (now - it->second > 5000)
                    it = dedup_map.erase(it);
                else
                    ++it;
            }
            dedup_lastCleanup = now;
        }

        // Case-insensitive key for dedup
        std::wstring key = filePath;
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);

        auto it = dedup_map.find(key);
        if (it != dedup_map.end() && (now - it->second) < 2000)
        {
            // Duplicate within 2 seconds — skip
            it->second = now;
            return;
        }
        dedup_map[key] = now;
    }

    self->m_callback(L"OPEN", filePath, L"");
}
