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
#include <thread>
#include <vector>

#define MAX_LOADSTRING 100

// ---------- Theme system ----------
struct Theme
{
    COLORREF bg;
    COLORREF panel;
    COLORREF text;
    COLORREF accent;
    COLORREF btnBg;
    COLORREF btnHot;
    COLORREF btnBorder;
    COLORREF btnBorderHot;
    COLORREF btnFocusBg;
    COLORREF response;
    COLORREF responseText;
    COLORREF separator;
};

static const Theme THEME_LIGHT = {
    RGB(243, 243, 243),     // bg
    RGB(255, 255, 255),     // panel
    RGB(30, 30, 30),        // text
    RGB(0, 122, 204),       // accent
    RGB(225, 225, 228),     // btnBg
    RGB(200, 200, 205),     // btnHot
    RGB(190, 190, 195),     // btnBorder
    RGB(0, 122, 204),       // btnBorderHot
    RGB(210, 210, 215),     // btnFocusBg
    RGB(255, 255, 255),     // response
    RGB(20, 20, 20),        // responseText
    RGB(0, 122, 204),       // separator
};

static const Theme THEME_DARK = {
    RGB(30, 30, 30),        // bg
    RGB(45, 45, 48),        // panel
    RGB(220, 220, 220),     // text
    RGB(0, 122, 204),       // accent
    RGB(62, 62, 66),        // btnBg
    RGB(80, 80, 85),        // btnHot
    RGB(70, 70, 74),        // btnBorder
    RGB(0, 122, 204),       // btnBorderHot
    RGB(55, 55, 60),        // btnFocusBg
    RGB(38, 38, 42),        // response
    RGB(180, 220, 255),     // responseText
    RGB(0, 122, 204),       // separator
};

static bool   g_isDarkMode = false;   // default: light
static Theme  g_theme      = THEME_LIGHT;

// ---------- GDI objects (recreated on theme switch) ----------
static HBRUSH g_hBrBg       = nullptr;
static HBRUSH g_hBrPanel    = nullptr;
static HBRUSH g_hBrResponse = nullptr;
static HFONT  g_hFontUI     = nullptr;
static HFONT  g_hFontTitle  = nullptr;
static HFONT  g_hFontMono   = nullptr;

static void RecreateThemeBrushes()
{
    if (g_hBrBg)       { DeleteObject(g_hBrBg);       g_hBrBg = nullptr; }
    if (g_hBrPanel)    { DeleteObject(g_hBrPanel);     g_hBrPanel = nullptr; }
    if (g_hBrResponse) { DeleteObject(g_hBrResponse);  g_hBrResponse = nullptr; }

    g_hBrBg       = CreateSolidBrush(g_theme.bg);
    g_hBrPanel    = CreateSolidBrush(g_theme.panel);
    g_hBrResponse = CreateSolidBrush(g_theme.response);
}

// ---------- Global Variables ----------
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

// Child window handles for repositioning on resize
static HWND g_hStatusLabel  = nullptr;
static HWND g_hSectionLabel = nullptr;
static HWND g_hCustomLabel  = nullptr;
static HWND g_hEditSeconds  = nullptr;
static HWND g_hBtnCustom    = nullptr;
static HWND g_hDefaultLabel = nullptr;
static HWND g_hBtnDefault   = nullptr;
static HWND g_hResponseLabel= nullptr;
static HWND g_hResponse     = nullptr;
static HWND g_hBtnTheme    = nullptr;
static HWND g_hBtnClear    = nullptr;
static std::vector<HWND> g_hWindowBtns;

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
void                CreateUIControls(HWND hWnd, HINSTANCE hInstance);
void                LayoutControls(HWND hWnd);
void                SendApiQuery(HWND hWnd, const std::string& jsonRequest);
void                OnQueryButton(HWND hWnd, int id);
void                ToggleTheme(HWND hWnd);
void                ClearHistory(HWND hWnd);

// ---------- Owner-draw button helper ----------
static void DrawModernButton(LPDRAWITEMSTRUCT dis)
{
    BOOL isHot = (dis->itemState & ODS_SELECTED) || (dis->itemState & ODS_HOTLIGHT);
    BOOL isFocused = (dis->itemState & ODS_FOCUS);

    COLORREF bg = isHot ? g_theme.btnHot : g_theme.btnBg;
    if (isFocused && !isHot) bg = g_theme.btnFocusBg;

    HBRUSH hBr = CreateSolidBrush(bg);
    FillRect(dis->hDC, &dis->rcItem, hBr);
    DeleteObject(hBr);

    // Accent border
    HPEN hPen = CreatePen(PS_SOLID, 1, isHot ? g_theme.btnBorderHot : g_theme.btnBorder);
    HPEN hOldPen = (HPEN)SelectObject(dis->hDC, hPen);
    HBRUSH hOldBr = (HBRUSH)SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top,
              dis->rcItem.right, dis->rcItem.bottom, 4, 4);
    SelectObject(dis->hDC, hOldBr);
    SelectObject(dis->hDC, hOldPen);
    DeleteObject(hPen);

    // Text
    wchar_t text[128] = {};
    GetWindowTextW(dis->hwndItem, text, 128);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, g_theme.text);
    SelectObject(dis->hDC, g_hFontUI);
    DrawTextW(dis->hDC, text, -1, &dis->rcItem,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// =================================================================
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Enable visual styles
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_WARP, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    if (!InitInstance(hInstance, SW_HIDE))
    {
        return FALSE;
    }

    StartSubsystems();
    SetTimer(g_hWnd, IDT_EVICTION_TIMER, 6 * 60 * 60 * 1000, nullptr);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    StopSubsystems();

    // Cleanup GDI
    if (g_hBrBg)       DeleteObject(g_hBrBg);
    if (g_hBrPanel)    DeleteObject(g_hBrPanel);
    if (g_hBrResponse) DeleteObject(g_hBrResponse);
    if (g_hFontUI)     DeleteObject(g_hFontUI);
    if (g_hFontTitle)  DeleteObject(g_hFontTitle);
    if (g_hFontMono)   DeleteObject(g_hFontMono);

    CoUninitialize();
    return (int)msg.wParam;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex = {};
    wcex.cbSize        = sizeof(WNDCLASSEX);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = WndProc;
    wcex.hInstance     = hInstance;
    wcex.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_WARP));
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = nullptr;  // We paint everything ourselves
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm       = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL));
    return RegisterClassExW(&wcex);
}

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    // Create GDI objects
    RecreateThemeBrushes();

    g_hFontUI = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    g_hFontTitle = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    g_hFontMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH, L"Cascadia Mono");

    g_hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        szWindowClass, szTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1024, 700,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hWnd)
        return FALSE;

    CreateUIControls(g_hWnd, hInstance);
    AddTrayIcon(g_hWnd);

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);
    return TRUE;
}

// ---------- Create all child controls ----------
void CreateUIControls(HWND hWnd, HINSTANCE hInstance)
{
    // Status label at top
    g_hStatusLabel = CreateWindowW(L"STATIC",
        L"\x25CF  Your friend WARP is trying to understand your needs in the background!",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hWnd, (HMENU)IDC_STATUS_TEXT, hInstance, nullptr);
    SendMessageW(g_hStatusLabel, WM_SETFONT, (WPARAM)g_hFontTitle, TRUE);

    // Section: Predefined window queries
    g_hSectionLabel = CreateWindowW(L"STATIC",
        L"Query by Predefined Time Window",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hWnd, nullptr, hInstance, nullptr);
    SendMessageW(g_hSectionLabel, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    struct { int id; const wchar_t* label; } windowBtns[] = {
        { IDB_QUERY_15M,  L"Last 15 min"  },
        { IDB_QUERY_30M,  L"Last 30 min"  },
        { IDB_QUERY_1H,   L"Last 1 hour"  },
        { IDB_QUERY_2H,   L"Last 2 hours" },
        { IDB_QUERY_6H,   L"Last 6 hours" },
        { IDB_QUERY_24H,  L"Last 24 hours"},
        { IDB_QUERY_7D,   L"Last 7 days"  },
        { IDB_QUERY_15D,  L"Last 15 days" },
        { IDB_QUERY_30D,  L"Last 30 days" },
    };

    g_hWindowBtns.clear();
    for (const auto& b : windowBtns)
    {
        HWND h = CreateWindowW(L"BUTTON", b.label,
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
            0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)b.id, hInstance, nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        g_hWindowBtns.push_back(h);
    }

    // Section: Custom seconds
    g_hCustomLabel = CreateWindowW(L"STATIC",
        L"Query by Custom Seconds",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hWnd, nullptr, hInstance, nullptr);
    SendMessageW(g_hCustomLabel, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    g_hEditSeconds = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"300",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDC_EDIT_SECONDS, hInstance, nullptr);
    SendMessageW(g_hEditSeconds, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    g_hBtnCustom = CreateWindowW(L"BUTTON", L"Send Custom Query",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDB_QUERY_CUSTOM, hInstance, nullptr);
    SendMessageW(g_hBtnCustom, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    // Section: Default (empty) query
    g_hDefaultLabel = CreateWindowW(L"STATIC",
        L"Query with Default (empty body — returns last 1 hour)",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hWnd, nullptr, hInstance, nullptr);
    SendMessageW(g_hDefaultLabel, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    g_hBtnDefault = CreateWindowW(L"BUTTON", L"Send Default Query",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDB_QUERY_DEFAULT, hInstance, nullptr);
    SendMessageW(g_hBtnDefault, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    // Response area
    g_hResponseLabel = CreateWindowW(L"STATIC",
        L"API Response",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hWnd, nullptr, hInstance, nullptr);
    SendMessageW(g_hResponseLabel, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    g_hResponse = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
        ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
        0, 0, 0, 0, hWnd, (HMENU)IDC_RESPONSE, hInstance, nullptr);
    SendMessageW(g_hResponse, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);

    // Clear History button (top-right, left of theme toggle)
    g_hBtnClear = CreateWindowW(L"BUTTON", L"Clear History",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDB_CLEAR_HISTORY, hInstance, nullptr);
    SendMessageW(g_hBtnClear, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    // Theme toggle button (top-right corner)
    g_hBtnTheme = CreateWindowW(L"BUTTON", L"Dark Mode",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDB_TOGGLE_THEME, hInstance, nullptr);
    SendMessageW(g_hBtnTheme, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    LayoutControls(hWnd);
}

// ---------- Position controls based on client size ----------
void LayoutControls(HWND hWnd)
{
    RECT rc;
    GetClientRect(hWnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    int pad = 16;
    int y = pad;

    // Theme toggle button in top-right corner
    int themeBtnW = 110;
    int themeBtnH = 30;
    int clearBtnW = 120;
    int btnGap = 8;
    MoveWindow(g_hBtnTheme, W - pad - themeBtnW, y, themeBtnW, themeBtnH, TRUE);

    // Clear History button to the left of theme toggle
    MoveWindow(g_hBtnClear, W - pad - themeBtnW - btnGap - clearBtnW, y, clearBtnW, themeBtnH, TRUE);

    // Status text (leave room for both buttons)
    int headerBtnsW = themeBtnW + btnGap + clearBtnW + 8;
    MoveWindow(g_hStatusLabel, pad, y, W - 2 * pad - headerBtnsW, 30, TRUE);
    y += 40;

    // Separator (we'll paint it; just track y)
    y += 4;

    // Section: predefined
    MoveWindow(g_hSectionLabel, pad, y, W - 2 * pad, 20, TRUE);
    y += 26;

    int btnW = 120;
    int btnH = 34;
    int gap = 8;
    int x = pad;
    for (size_t i = 0; i < g_hWindowBtns.size(); ++i)
    {
        if (x + btnW > W - pad)
        {
            x = pad;
            y += btnH + gap;
        }
        MoveWindow(g_hWindowBtns[i], x, y, btnW, btnH, TRUE);
        x += btnW + gap;
    }
    y += btnH + gap + 8;

    // Section: custom seconds
    MoveWindow(g_hCustomLabel, pad, y, W - 2 * pad, 20, TRUE);
    y += 26;
    MoveWindow(g_hEditSeconds, pad, y, 120, 30, TRUE);
    MoveWindow(g_hBtnCustom, pad + 120 + gap, y, 180, 30, TRUE);
    y += 38 + 8;

    // Section: default
    MoveWindow(g_hDefaultLabel, pad, y, W - 2 * pad, 20, TRUE);
    y += 26;
    MoveWindow(g_hBtnDefault, pad, y, 180, 30, TRUE);
    y += 38 + 8;

    // Response label
    MoveWindow(g_hResponseLabel, pad, y, W - 2 * pad, 20, TRUE);
    y += 24;

    // Response edit fills the rest
    int respH = H - y - pad;
    if (respH < 60) respH = 60;
    MoveWindow(g_hResponse, pad, y, W - 2 * pad, respH, TRUE);
}

// ---------- Pipe query (runs on background thread, posts result back) ----------
static const UINT WM_QUERY_RESULT = WM_USER + 50;

void SendApiQuery(HWND hWnd, const std::string& jsonRequest)
{
    // Show "Querying..." immediately
    SetWindowTextW(g_hResponse, L"Querying...");

    // Copy request for the thread
    std::string* req = new std::string(jsonRequest);

    std::thread([hWnd, req]() {
        std::string response;

        HANDLE hPipe = CreateFileW(
            L"\\\\.\\pipe\\WarpFileActivityAPI",
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);

        if (hPipe == INVALID_HANDLE_VALUE)
        {
            response = "Error: Could not connect to pipe (GetLastError=" +
                       std::to_string(GetLastError()) + ")";
        }
        else
        {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

            DWORD written = 0;
            WriteFile(hPipe, req->c_str(), (DWORD)req->size(), &written, nullptr);

            // Read the full message (loop on ERROR_MORE_DATA)
            char buf[4096];
            DWORD bytesRead = 0;
            BOOL readOk;
            for (;;)
            {
                readOk = ReadFile(hPipe, buf, sizeof(buf), &bytesRead, nullptr);
                if (readOk)
                {
                    response.append(buf, bytesRead);
                    break;
                }
                if (GetLastError() == ERROR_MORE_DATA)
                {
                    response.append(buf, bytesRead);
                    continue;
                }
                break; // real error
            }

            if (!response.empty())
            {

                // Pretty-print the JSON (simple indentation)
                std::string pretty;
                pretty.reserve(response.size() * 2);
                int indent = 0;
                bool inString = false;
                for (size_t i = 0; i < response.size(); ++i)
                {
                    char c = response[i];
                    if (c == '"' && (i == 0 || response[i - 1] != '\\'))
                        inString = !inString;

                    if (!inString)
                    {
                        if (c == '{' || c == '[')
                        {
                            pretty += c;
                            pretty += "\r\n";
                            indent += 2;
                            pretty.append(indent, ' ');
                            continue;
                        }
                        if (c == '}' || c == ']')
                        {
                            pretty += "\r\n";
                            indent -= 2;
                            if (indent < 0) indent = 0;
                            pretty.append(indent, ' ');
                            pretty += c;
                            continue;
                        }
                        if (c == ',')
                        {
                            pretty += c;
                            pretty += "\r\n";
                            pretty.append(indent, ' ');
                            continue;
                        }
                        if (c == ':')
                        {
                            pretty += ": ";
                            continue;
                        }
                    }
                    pretty += c;
                }
                response = pretty;
            }
            else
            {
                response = "Error: ReadFile failed (GetLastError=" +
                           std::to_string(GetLastError()) + ")";
            }
            CloseHandle(hPipe);
        }

        delete req;

        // Marshal result back to UI thread
        wchar_t* wResult = nullptr;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, response.c_str(), -1, nullptr, 0);
        wResult = new wchar_t[wlen];
        MultiByteToWideChar(CP_UTF8, 0, response.c_str(), -1, wResult, wlen);
        PostMessageW(hWnd, WM_QUERY_RESULT, 0, (LPARAM)wResult);
    }).detach();
}

void OnQueryButton(HWND hWnd, int id)
{
    std::string request;
    switch (id)
    {
    case IDB_QUERY_15M:     request = R"({"window":"15m"})"; break;
    case IDB_QUERY_30M:     request = R"({"window":"30m"})"; break;
    case IDB_QUERY_1H:      request = R"({"window":"1h"})";  break;
    case IDB_QUERY_2H:      request = R"({"window":"2h"})";  break;
    case IDB_QUERY_6H:      request = R"({"window":"6h"})";  break;
    case IDB_QUERY_24H:     request = R"({"window":"24h"})"; break;
    case IDB_QUERY_7D:      request = R"({"window":"7d"})";  break;
    case IDB_QUERY_15D:     request = R"({"window":"15d"})"; break;
    case IDB_QUERY_30D:     request = R"({"window":"30d"})"; break;
    case IDB_QUERY_DEFAULT: request = R"({})";               break;
    case IDB_QUERY_CUSTOM:
    {
        wchar_t buf[64] = {};
        GetWindowTextW(g_hEditSeconds, buf, 64);
        int secs = _wtoi(buf);
        if (secs <= 0) secs = 300;
        request = "{\"seconds\":" + std::to_string(secs) + "}";
        break;
    }
    default: return;
    }

    SendApiQuery(hWnd, request);
}

// =================================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP)
            ShowTrayMenu(hWnd);
        else if (lParam == WM_LBUTTONDBLCLK)
            RestoreFromTray(hWnd);
        break;

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        if (wmId == IDM_TRAY_OPEN)
            RestoreFromTray(hWnd);
        else if (wmId == IDM_TRAY_EXIT)
        {
            RemoveTrayIcon();
            DestroyWindow(hWnd);
        }
        else if (wmId >= IDB_QUERY_15M && wmId <= IDB_QUERY_CUSTOM)
            OnQueryButton(hWnd, wmId);
        else if (wmId == IDB_TOGGLE_THEME)
            ToggleTheme(hWnd);
        else if (wmId == IDB_CLEAR_HISTORY)
            ClearHistory(hWnd);
        else
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    break;

    case WM_QUERY_RESULT:
    {
        wchar_t* text = (wchar_t*)lParam;
        if (text)
        {
            SetWindowTextW(g_hResponse, text);
            delete[] text;
        }
    }
    break;

    case WM_DRAWITEM:
    {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
        if (dis->CtlType == ODT_BUTTON)
        {
            DrawModernButton(dis);
            return TRUE;
        }
    }
    break;

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, g_theme.text);
        SetBkColor(hdc, g_theme.bg);
        return (LRESULT)g_hBrBg;
    }

    case WM_CTLCOLOREDIT:
    {
        HDC hdc = (HDC)wParam;
        HWND hCtl = (HWND)lParam;
        if (hCtl == g_hResponse)
        {
            SetTextColor(hdc, g_theme.responseText);
            SetBkColor(hdc, g_theme.response);
            return (LRESULT)g_hBrResponse;
        }
        // Custom seconds edit
        SetTextColor(hdc, g_theme.text);
        SetBkColor(hdc, g_theme.panel);
        return (LRESULT)g_hBrPanel;
    }

    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, g_hBrBg);

        // Draw accent separator line below status text
        HPEN hPen = CreatePen(PS_SOLID, 2, g_theme.separator);
        HPEN hOld = (HPEN)SelectObject(hdc, hPen);
        int y = 52;
        MoveToEx(hdc, 16, y, nullptr);
        LineTo(hdc, rc.right - 16, y);
        SelectObject(hdc, hOld);
        DeleteObject(hPen);
        return 1;
    }

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            LayoutControls(hWnd);
        break;

    case WM_GETMINMAXINFO:
    {
        LPMINMAXINFO mmi = (LPMINMAXINFO)lParam;
        mmi->ptMinTrackSize.x = 800;
        mmi->ptMinTrackSize.y = 500;
    }
    break;

    case WM_SYSCOMMAND:
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
            g_db.EvictOlderThan30Days();
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
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

// =================================================================
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

void ToggleTheme(HWND hWnd)
{
    g_isDarkMode = !g_isDarkMode;
    g_theme = g_isDarkMode ? THEME_DARK : THEME_LIGHT;
    RecreateThemeBrushes();

    // Update toggle button label
    SetWindowTextW(g_hBtnTheme, g_isDarkMode ? L"Light Mode" : L"Dark Mode");

    // Force full repaint of main window and all children
    RedrawWindow(hWnd, nullptr, nullptr,
                 RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void ClearHistory(HWND hWnd)
{
    int result = MessageBoxW(hWnd,
        L"Are you sure you want to clear all activity history?\n\n"
        L"This will permanently delete all recorded file activity.\n"
        L"Only new events after clearing will be stored.",
        L"WARP - Clear History",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);

    if (result == IDYES)
    {
        g_db.ClearAll();
        SetWindowTextW(g_hResponse, L"Activity history cleared.");
    }
}

void RestoreFromTray(HWND hWnd)
{
    ShowWindow(hWnd, SW_SHOW);
    ShowWindow(hWnd, SW_SHOWMAXIMIZED);
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
    std::wstring dbPath = GetDatabasePath();
    g_db.Open(dbPath);
    g_db.EvictOlderThan30Days();

    g_fileMonitor.SetCallback([](const std::wstring& action,
                                 const std::wstring& path,
                                 const std::wstring& oldPath) {
        g_db.InsertActivity(action, path, oldPath);
    });
    g_fileMonitor.Start();

    g_idleDetector.SetCallbacks(
        []() { g_fileMonitor.Pause(); },
        []() { g_fileMonitor.Resume(); }
    );
    g_idleDetector.Start(120000);

    g_queryApi.Start(&g_db);
}

void StopSubsystems()
{
    g_queryApi.Stop();
    g_idleDetector.Stop();
    g_fileMonitor.Stop();
    g_db.Close();
}
