#include "framework.h"
#include "framework.h"
#include "FileMonitor.h"
#include <shlobj.h>
#include <algorithm>

// ---------- Path exclusion filter ----------
// Returns true if the path should be excluded from activity recording.
static bool ShouldExclude(const std::wstring& path)
{
    if (path.empty())
        return true;

    // Work with a lowercase copy for case-insensitive matching
    std::wstring lp = path;
    std::transform(lp.begin(), lp.end(), lp.begin(), ::towlower);

    // --- Excluded directory prefixes (system, cache, temp, build artifacts) ---
    static const wchar_t* const excludedDirs[] = {
        L"\\windows\\",
        L"\\$recycle.bin\\",
        L"\\system volume information\\",
        L"\\programdata\\microsoft\\",
        L"\\programdata\\package cache\\",
        L"\\appdata\\",
        L"\\.vs\\",
        L"\\.git\\",
        L"\\node_modules\\",
        L"\\__pycache__\\",
        L"\\.nuget\\",
        L"\\recovery\\",
        L"\\msocache\\",
        L"\\config.msi\\",
    };

    for (const auto* dir : excludedDirs)
    {
        if (lp.find(dir) != std::wstring::npos)
            return true;
    }

    // --- Excluded file extensions ---
    static const wchar_t* const excludedExts[] = {
        L".exe", L".dll", L".sys", L".drv", L".ocx",
        L".tmp", L".temp", L".etl", L".evtx",
        L".pf", L".cache", L".diagsession",
        L".obj", L".pch", L".ipch", L".ilk", L".pdb",
        L".tlog", L".idb", L".res", L".aps",
        L".suo", L".sdf", L".opensdf",
        L".log", L".bak",
        L".lock", L".lck",
        L".db", L".db-wal", L".db-shm", L".sqlite",
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
}

void FileMonitor::Stop()
{
    if (!m_running) return;
    m_running = false;
    SetEvent(m_stopEvent);

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
                !ShouldExclude(fullPath) && !ShouldExclude(oldPath))
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
                        !ShouldExclude(path) && !ShouldExclude(oldPath))
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
