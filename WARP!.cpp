// WARP!.cpp : Defines the entry point for the application.
// WARP!.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "WARP!.h"
#include "ActivityDatabase.h"
#include "FileMonitor.h"
#include "IdleDetector.h"
#include "QueryApi.h"

#include <string>
#include <shlobj.h>

#define MAX_LOADSTRING 100

// Global Variables:
HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];
NOTIFYICONDATAW nid = {};
HWND g_hWnd = nullptr;

// Subsystems
ActivityDatabase g_db;
FileMonitor     g_fileMonitor;
IdleDetector    g_idleDetector;
QueryApi        g_queryApi;

// Forward declarations
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
void                AddTrayIcon(HWND hWnd);
void                RemoveTrayIcon();
void                ShowTrayMenu(HWND hWnd);
void                MinimizeToTray(HWND hWnd);
void                RestoreFromTray(HWND hWnd);
std::wstring        GetDatabasePath();
void                StartSubsystems();
void                StopSubsystems();

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_WARP, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // Start minimized to tray – pass SW_HIDE so window is not shown
    if (!InitInstance(hInstance, SW_HIDE))
    {
        return FALSE;
    }

    // Start all background subsystems
    StartSubsystems();

    // Set a timer for eviction (every 6 hours)
    SetTimer(g_hWnd, IDT_EVICTION_TIMER, 6 * 60 * 60 * 1000, nullptr);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    StopSubsystems();
    CoUninitialize();

    return (int)msg.wParam;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_WARP));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName   = nullptr; // No menu bar
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    g_hWnd = CreateWindowW(szWindowClass, szTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, 0, 500, 200,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hWnd)
    {
        return FALSE;
    }

    // Create the static text label
    CreateWindowW(L"STATIC",
        L"Your friend WARP is trying to understand your needs in the background!",
        WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
        10, 10, 460, 120,
        g_hWnd, (HMENU)IDC_STATUS_TEXT, hInstance, nullptr);

    // Add tray icon immediately
    AddTrayIcon(g_hWnd);

    // Start hidden (minimized to tray)
    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    return TRUE;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP)
        {
            ShowTrayMenu(hWnd);
        }
        else if (lParam == WM_LBUTTONDBLCLK)
        {
            RestoreFromTray(hWnd);
        }
        break;

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        switch (wmId)
        {
        case IDM_TRAY_OPEN:
            RestoreFromTray(hWnd);
            break;
        case IDM_TRAY_EXIT:
            RemoveTrayIcon();
            DestroyWindow(hWnd);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
    break;

    case WM_SYSCOMMAND:
        // Intercept minimize and close to go to tray instead
        if (wParam == SC_MINIMIZE || wParam == SC_CLOSE)
        {
            MinimizeToTray(hWnd);
            return 0;
        }
        return DefWindowProc(hWnd, message, wParam, lParam);

    case WM_CLOSE:
        MinimizeToTray(hWnd);
        return 0;

    case WM_TIMER:
        if (wParam == IDT_EVICTION_TIMER)
        {
            g_db.EvictOlderThan30Days();
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
    }
    break;

    case WM_DESTROY:
        KillTimer(hWnd, IDT_EVICTION_TIMER);
        RemoveTrayIcon();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

void AddTrayIcon(HWND hWnd)
{
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_WARP));
    wcscpy_s(nid.szTip, L"WARP! - Monitoring file activity");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowTrayMenu(HWND hWnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_OPEN, L"Open");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit");

    // Required for the menu to disappear when clicking elsewhere
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, hWnd, nullptr);
    PostMessage(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

void MinimizeToTray(HWND hWnd)
{
    ShowWindow(hWnd, SW_HIDE);
}

void RestoreFromTray(HWND hWnd)
{
    ShowWindow(hWnd, SW_SHOW);
    ShowWindow(hWnd, SW_RESTORE);
    SetForegroundWindow(hWnd);
}

std::wstring GetDatabasePath()
{
    wchar_t appData[MAX_PATH] = {};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData);
    std::wstring dir = std::wstring(appData) + L"\\WARP";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\activity.db";
}

void StartSubsystems()
{
    // 1. Open database
    std::wstring dbPath = GetDatabasePath();
    g_db.Open(dbPath);

    // Initial eviction on startup
    g_db.EvictOlderThan30Days();

    // 2. Start file monitor
    g_fileMonitor.SetCallback([](const std::wstring& action,
                                 const std::wstring& path,
                                 const std::wstring& oldPath) {
        g_db.InsertActivity(action, path, oldPath);
    });
    g_fileMonitor.Start();

    // 3. Start idle detector – pause/resume file monitor
    g_idleDetector.SetCallbacks(
        []() { g_fileMonitor.Pause(); },
        []() { g_fileMonitor.Resume(); }
    );
    g_idleDetector.Start(120000); // 2 minute idle threshold

    // 4. Start query API (named pipe server)
    g_queryApi.Start(&g_db);
}

void StopSubsystems()
{
    g_queryApi.Stop();
    g_idleDetector.Stop();
    g_fileMonitor.Stop();
    g_db.Close();
}
