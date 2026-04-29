// WARP!.cpp : Defines the entry point for the application.
// WARP!.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "WARP!.h"
#include "ActivityDatabase.h"
#include "FileMonitor.h"
#include "AppLaunchMonitor.h"
#include "BrowsingMonitor.h"
#include "ForegroundMonitor.h"
#include "IdleDetector.h"
#include "QueryApi.h"
#include "InferenceEngine.h"
#include "TopicInference.h"

#include <string>
#include <shlobj.h>
#include <thread>
#include <vector>
#include <ctime>
#include <algorithm>

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
ActivityDatabase   g_db;
FileMonitor         g_fileMonitor;
AppLaunchMonitor    g_appLaunchMonitor;
BrowsingMonitor     g_browsingMonitor;
ForegroundMonitor   g_foregroundMonitor;
IdleDetector        g_idleDetector;
QueryApi            g_queryApi;
InferenceEngine     g_inference;
TopicInference      g_topicInference;

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
static HWND g_hChkFile     = nullptr;
static HWND g_hChkAppLaunch= nullptr;
static HWND g_hChkAppFocus = nullptr;
static HWND g_hChkBrowsing = nullptr;
static HWND g_hFilterLabel = nullptr;
static std::vector<HWND> g_hWindowBtns;

// Inference exploration controls
static HWND g_hInferLabel     = nullptr;
static HWND g_hChkInferFile   = nullptr;
static HWND g_hChkInferApp    = nullptr;
static HWND g_hChkInferUrl    = nullptr;
static HWND g_hInferFilterLbl = nullptr;
static HWND g_hBtnInferTop    = nullptr;
static HWND g_hEditInferPath  = nullptr;
static HWND g_hBtnInferLookup = nullptr;
static HWND g_hEditInferTopN  = nullptr;
static HWND g_hInferTopNLabel = nullptr;

// Topic context button
static HWND g_hBtnTopicContext = nullptr;

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
void                OnInferenceButton(HWND hWnd, int id);

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

    // Event type filter checkboxes
    g_hFilterLabel = CreateWindowW(L"STATIC",
        L"Event Types to Query:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hWnd, (HMENU)IDC_FILTER_LABEL, hInstance, nullptr);
    SendMessageW(g_hFilterLabel, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    g_hChkFile = CreateWindowW(L"BUTTON", L"File Activity",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CHK_FILE, hInstance, nullptr);
    SendMessageW(g_hChkFile, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hChkFile, BM_SETCHECK, BST_CHECKED, 0);

    g_hChkAppLaunch = CreateWindowW(L"BUTTON", L"App Launches",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CHK_APP_LAUNCH, hInstance, nullptr);
    SendMessageW(g_hChkAppLaunch, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hChkAppLaunch, BM_SETCHECK, BST_CHECKED, 0);

    g_hChkAppFocus = CreateWindowW(L"BUTTON", L"App Focus",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CHK_APP_FOCUS, hInstance, nullptr);
    SendMessageW(g_hChkAppFocus, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hChkAppFocus, BM_SETCHECK, BST_CHECKED, 0);

    g_hChkBrowsing = CreateWindowW(L"BUTTON", L"Browsing Activity",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CHK_BROWSING, hInstance, nullptr);
    SendMessageW(g_hChkBrowsing, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hChkBrowsing, BM_SETCHECK, BST_CHECKED, 0);

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

    // --- Inference exploration section ---
    g_hInferLabel = CreateWindowW(L"STATIC",
        L"Explore Precomputed Inferences",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hWnd, (HMENU)IDC_INFER_LABEL, hInstance, nullptr);
    SendMessageW(g_hInferLabel, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    g_hInferFilterLbl = CreateWindowW(L"STATIC",
        L"Entity Types:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hWnd, (HMENU)IDC_INFER_FILTER_LABEL, hInstance, nullptr);
    SendMessageW(g_hInferFilterLbl, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    g_hChkInferFile = CreateWindowW(L"BUTTON", L"Files",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CHK_INFER_FILE, hInstance, nullptr);
    SendMessageW(g_hChkInferFile, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hChkInferFile, BM_SETCHECK, BST_CHECKED, 0);

    g_hChkInferApp = CreateWindowW(L"BUTTON", L"Apps",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CHK_INFER_APP, hInstance, nullptr);
    SendMessageW(g_hChkInferApp, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hChkInferApp, BM_SETCHECK, BST_CHECKED, 0);

    g_hChkInferUrl = CreateWindowW(L"BUTTON", L"URLs",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CHK_INFER_URL, hInstance, nullptr);
    SendMessageW(g_hChkInferUrl, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hChkInferUrl, BM_SETCHECK, BST_CHECKED, 0);

    g_hInferTopNLabel = CreateWindowW(L"STATIC",
        L"Top N:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hWnd, (HMENU)IDC_INFER_TOPN_LABEL, hInstance, nullptr);
    SendMessageW(g_hInferTopNLabel, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    g_hEditInferTopN = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"50",
        WS_CHILD | WS_VISIBLE | ES_NUMBER | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDC_EDIT_INFER_TOPN, hInstance, nullptr);
    SendMessageW(g_hEditInferTopN, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    g_hBtnInferTop = CreateWindowW(L"BUTTON", L"Show Top Inferences",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDB_INFER_TOP, hInstance, nullptr);
    SendMessageW(g_hBtnInferTop, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    g_hEditInferPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDC_EDIT_INFER_PATH, hInstance, nullptr);
    SendMessageW(g_hEditInferPath, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hEditInferPath, EM_SETCUEBANNER, TRUE, (LPARAM)L"Enter path or URL to look up...");

    g_hBtnInferLookup = CreateWindowW(L"BUTTON", L"Lookup",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDB_INFER_LOOKUP, hInstance, nullptr);
    SendMessageW(g_hBtnInferLookup, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    // Topic context button
    g_hBtnTopicContext = CreateWindowW(L"BUTTON", L"Show Recent Context",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDB_TOPIC_CONTEXT, hInstance, nullptr);
    SendMessageW(g_hBtnTopicContext, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

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

    // Event type filter checkboxes
    MoveWindow(g_hFilterLabel, pad, y, 180, 20, TRUE);
    int chkX = pad + 180;
    int chkW = 130;
    MoveWindow(g_hChkFile,      chkX,              y, chkW, 20, TRUE);
    MoveWindow(g_hChkAppLaunch, chkX + chkW + gap, y, chkW, 20, TRUE);
    MoveWindow(g_hChkAppFocus,  chkX + 2 * (chkW + gap), y, chkW, 20, TRUE);
    MoveWindow(g_hChkBrowsing,  chkX + 3 * (chkW + gap), y, 160, 20, TRUE);
    y += 28;

    // --- Inference exploration section ---
    MoveWindow(g_hInferLabel, pad, y, W - 2 * pad, 20, TRUE);
    y += 24;

    // Entity type filter + Top N + Show button (all on one row)
    int infLblW = 100;
    int infChkW = 70;
    MoveWindow(g_hInferFilterLbl, pad, y, infLblW, 20, TRUE);
    int ix = pad + infLblW;
    MoveWindow(g_hChkInferFile, ix, y, infChkW, 20, TRUE);  ix += infChkW + gap;
    MoveWindow(g_hChkInferApp,  ix, y, infChkW, 20, TRUE);  ix += infChkW + gap;
    MoveWindow(g_hChkInferUrl,  ix, y, infChkW, 20, TRUE);  ix += infChkW + gap + 8;
    MoveWindow(g_hInferTopNLabel, ix, y, 50, 20, TRUE);      ix += 50;
    MoveWindow(g_hEditInferTopN,  ix, y, 60, 24, TRUE);      ix += 60 + gap;
    MoveWindow(g_hBtnInferTop, ix, y, 180, 28, TRUE);
    y += 32;

    // Lookup row: path edit + Lookup button
    int lookupBtnW = 100;
    int editW = W - 2 * pad - lookupBtnW - gap;
    if (editW < 200) editW = 200;
    MoveWindow(g_hEditInferPath, pad, y, editW, 26, TRUE);
    MoveWindow(g_hBtnInferLookup, pad + editW + gap, y, lookupBtnW, 26, TRUE);
    y += 34;

    // Topic context button
    MoveWindow(g_hBtnTopicContext, pad, y, 200, 30, TRUE);
    y += 38;

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
    // Build the types array from checkbox state
    std::string types;
    {
        bool chkFile = (SendMessageW(g_hChkFile, BM_GETCHECK, 0, 0) == BST_CHECKED);
        bool chkApp  = (SendMessageW(g_hChkAppLaunch, BM_GETCHECK, 0, 0) == BST_CHECKED);
        bool chkFocus = (SendMessageW(g_hChkAppFocus, BM_GETCHECK, 0, 0) == BST_CHECKED);
        bool chkBrowse = (SendMessageW(g_hChkBrowsing, BM_GETCHECK, 0, 0) == BST_CHECKED);

        // If none checked, default to all
        if (!chkFile && !chkApp && !chkFocus && !chkBrowse)
        {
            chkFile = chkApp = chkFocus = chkBrowse = true;
        }

        types = ",\"types\":[";
        bool first = true;
        if (chkFile)   { if (!first) types += ","; types += "\"file\"";       first = false; }
        if (chkApp)    { if (!first) types += ","; types += "\"app_launch\""; first = false; }
        if (chkFocus)  { if (!first) types += ","; types += "\"app_focus\"";  first = false; }
        if (chkBrowse) { if (!first) types += ","; types += "\"browsing\"";   first = false; }
        types += "]";
    }

    std::string request;
    switch (id)
    {
    case IDB_QUERY_15M:     request = "{\"window\":\"15m\"" + types + "}"; break;
    case IDB_QUERY_30M:     request = "{\"window\":\"30m\"" + types + "}"; break;
    case IDB_QUERY_1H:      request = "{\"window\":\"1h\""  + types + "}"; break;
    case IDB_QUERY_2H:      request = "{\"window\":\"2h\""  + types + "}"; break;
    case IDB_QUERY_6H:      request = "{\"window\":\"6h\""  + types + "}"; break;
    case IDB_QUERY_24H:     request = "{\"window\":\"24h\"" + types + "}"; break;
    case IDB_QUERY_7D:      request = "{\"window\":\"7d\""  + types + "}"; break;
    case IDB_QUERY_15D:     request = "{\"window\":\"15d\"" + types + "}"; break;
    case IDB_QUERY_30D:     request = "{\"window\":\"30d\"" + types + "}"; break;
    case IDB_QUERY_DEFAULT: request = "{" + types.substr(1) + "}";              break;
    case IDB_QUERY_CUSTOM:
    {
        wchar_t buf[64] = {};
        GetWindowTextW(g_hEditSeconds, buf, 64);
        int secs = _wtoi(buf);
        if (secs <= 0) secs = 300;
        request = "{\"seconds\":" + std::to_string(secs) + types + "}";
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
        else if (wmId == IDB_INFER_TOP || wmId == IDB_INFER_LOOKUP)
            OnInferenceButton(hWnd, wmId);
        else if (wmId == IDB_TOPIC_CONTEXT)
            SendApiQuery(hWnd, "{\"op\":\"GetRecentContext\"}");
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
        // Custom seconds edit and inference edits
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
        {
            g_db.EvictOlderThan30Days();
            g_inference.RefreshRollingCounts();
        }
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
        g_inference.ClearCache();
        g_topicInference.ClearHistory();
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

    // Initialize inference engine with direct DB access
    g_inference.Init(g_db.DbHandle(), g_db.DbMutex());
    g_inference.RefreshRollingCounts();

    g_fileMonitor.SetCallback([](const std::wstring& action,
                                 const std::wstring& path,
                                 const std::wstring& oldPath) {
        g_db.InsertActivity(action, path, oldPath);
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        g_inference.OnFileEvent(action, path, now);
    });
    g_fileMonitor.Start();

    g_appLaunchMonitor.SetCallback([](const std::wstring& exeName,
                                      const std::wstring& exePath,
                                      DWORD pid) {
        g_db.InsertAppLaunch(exeName, exePath, pid);
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        g_inference.OnAppLaunchEvent(exePath, now);
    });
    g_appLaunchMonitor.Start();

    g_browsingMonitor.SetCallback([](const std::wstring& browser,
                                     const std::wstring& title,
                                     const std::wstring& url) {
        g_db.InsertBrowsingActivity(browser, title, url);
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        g_inference.OnBrowsingEvent(url.empty() ? title : url, now);
    });
    g_browsingMonitor.Start();

    g_foregroundMonitor.SetCallback([](const std::wstring& exeName,
                                       const std::wstring& exePath,
                                       const std::wstring& windowTitle,
                                       int durationSecs) {
        g_db.InsertAppFocusActivity(exeName, exePath, windowTitle, durationSecs);
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        g_inference.OnAppFocusEvent(exePath, now);
    });
    g_foregroundMonitor.Start();

    g_idleDetector.SetCallbacks(
        []() {
            g_fileMonitor.Pause();
            g_appLaunchMonitor.Pause();
            g_browsingMonitor.Pause();
            g_foregroundMonitor.Pause();
        },
        []() {
            g_fileMonitor.Resume();
            g_appLaunchMonitor.Resume();
            g_browsingMonitor.Resume();
            g_foregroundMonitor.Resume();
        }
    );
    g_idleDetector.Start(120000);

    g_queryApi.Start(&g_db, &g_inference, &g_topicInference);

    // Initialize and start topic inference (MiniLM semantic model).
    // Look for models directory next to the executable first,
    // then fall back to %LOCALAPPDATA%\WARP\models.
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring exeDir(exePath);
        auto pos = exeDir.find_last_of(L"\\/");
        if (pos != std::wstring::npos)
            exeDir = exeDir.substr(0, pos);

        std::wstring modelsDir = exeDir + L"\\models";

        // Check if model exists next to exe
        std::wstring modelFile = modelsDir + L"\\minilm.onnx";
        if (GetFileAttributesW(modelFile.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            // Fall back to LOCALAPPDATA\WARP\models
            wchar_t appData[MAX_PATH] = {};
            SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData);
            modelsDir = std::wstring(appData) + L"\\WARP\\models";
        }

        if (g_topicInference.Init(modelsDir))
            g_topicInference.Start(&g_db);
    }
}

void OnInferenceButton(HWND hWnd, int id)
{
    if (id == IDB_INFER_TOP)
    {
        // Read Top N value
        wchar_t buf[32] = {};
        GetWindowTextW(g_hEditInferTopN, buf, 32);
        int topN = _wtoi(buf);
        if (topN <= 0) topN = 50;
        if (topN > 5000) topN = 5000;

        // Build entity type filter from checkboxes
        bool wantFile = (SendMessageW(g_hChkInferFile, BM_GETCHECK, 0, 0) == BST_CHECKED);
        bool wantApp  = (SendMessageW(g_hChkInferApp,  BM_GETCHECK, 0, 0) == BST_CHECKED);
        bool wantUrl  = (SendMessageW(g_hChkInferUrl,  BM_GETCHECK, 0, 0) == BST_CHECKED);
        if (!wantFile && !wantApp && !wantUrl)
            wantFile = wantApp = wantUrl = true;

        // Use GetInferenceDeltas with since_version=0 to get all records,
        // then filter client-side and show top N by recency_score.
        // We send through the pipe so it goes through the same path.
        std::string request = "{\"op\":\"GetInferenceDeltas\",\"since_version\":0}";

        SetWindowTextW(g_hResponse, L"Querying inferences...");

        // Capture filter state for the thread
        struct InferFilter { bool file; bool app; bool url; int topN; };
        InferFilter* pf = new InferFilter{ wantFile, wantApp, wantUrl, topN };
        std::string* req = new std::string(request);

        std::thread([hWnd, req, pf]() {
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

                char buf[65536];
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
                    break;
                }

                if (response.empty())
                {
                    response = "Error: ReadFile failed (GetLastError=" +
                               std::to_string(GetLastError()) + ")";
                }
                CloseHandle(hPipe);
            }

            delete req;

            // Parse the deltas JSON minimally: extract each record and sort
            // We look for the deltas array and parse individual objects.
            struct Rec {
                std::string key;
                std::string type;
                double score;
                int count7d;
                int count30d;
                int countTotal;
                int64_t lastOpen;
                int64_t lastEdit;
            };
            std::vector<Rec> records;

            // Find "deltas":[
            auto deltasPos = response.find("\"deltas\":[");
            if (deltasPos != std::string::npos)
            {
                // Simple extraction: scan for each object { ... } inside the array
                size_t pos = response.find('[', deltasPos) + 1;
                while (pos < response.size())
                {
                    auto objStart = response.find('{', pos);
                    if (objStart == std::string::npos) break;
                    auto objEnd = response.find('}', objStart);
                    if (objEnd == std::string::npos) break;

                    std::string obj = response.substr(objStart, objEnd - objStart + 1);

                    // Extract fields with a simple lambda
                    auto getStr = [&](const std::string& o, const char* key) -> std::string {
                        std::string k = std::string("\"" ) + key + "\":\"";
                        auto p = o.find(k);
                        if (p == std::string::npos) return "";
                        p += k.size();
                        auto e = o.find('"', p);
                        return (e != std::string::npos) ? o.substr(p, e - p) : "";
                    };
                    auto getNum = [&](const std::string& o, const char* key) -> double {
                        std::string k = std::string("\"" ) + key + "\":";
                        auto p = o.find(k);
                        if (p == std::string::npos) return 0.0;
                        p += k.size();
                        return strtod(o.c_str() + p, nullptr);
                    };

                    Rec r;
                    r.key   = getStr(obj, "entity_key");
                    r.type  = getStr(obj, "entity_type");
                    r.score = getNum(obj, "recency_score");
                    r.count7d    = static_cast<int>(getNum(obj, "open_count_7d"));
                    r.count30d   = static_cast<int>(getNum(obj, "open_count_30d"));
                    r.countTotal = static_cast<int>(getNum(obj, "open_count_total"));
                    r.lastOpen   = static_cast<int64_t>(getNum(obj, "last_open_ts"));
                    r.lastEdit   = static_cast<int64_t>(getNum(obj, "last_edit_ts"));

                    // Apply entity type filter
                    bool keep = false;
                    if (r.type == "file" && pf->file) keep = true;
                    if (r.type == "app"  && pf->app)  keep = true;
                    if (r.type == "url"  && pf->url)  keep = true;
                    if (keep)
                        records.push_back(r);

                    pos = objEnd + 1;
                }
            }

            // Sort by recency_score descending
            std::sort(records.begin(), records.end(),
                      [](const Rec& a, const Rec& b) { return a.score > b.score; });

            // Truncate to top N
            if ((int)records.size() > pf->topN)
                records.resize(pf->topN);

            // Format as human-readable text
            std::string output;
            output += "Top " + std::to_string(records.size()) + " Inferences";
            output += " (sorted by recency score)\r\n";
            output += std::string(70, '-') + "\r\n";

            char line[512];
            for (size_t i = 0; i < records.size(); ++i)
            {
                const auto& r = records[i];
                sprintf_s(line, sizeof(line),
                    "%3d. [%s] score=%.1f  opens: 7d=%d 30d=%d total=%d\r\n",
                    (int)(i + 1), r.type.c_str(), r.score,
                    r.count7d, r.count30d, r.countTotal);
                output += line;

                // Unescape backslashes for display
                std::string displayKey = r.key;
                size_t p2 = 0;
                while ((p2 = displayKey.find("\\\\", p2)) != std::string::npos)
                {
                    displayKey.replace(p2, 2, "\\");
                    p2 += 1;
                }
                output += "     " + displayKey + "\r\n";

                // Show timestamps if non-zero
                if (r.lastOpen > 0 || r.lastEdit > 0)
                {
                    output += "     ";
                    if (r.lastOpen > 0)
                    {
                        time_t t = static_cast<time_t>(r.lastOpen);
                        struct tm tmBuf;
                        localtime_s(&tmBuf, &t);
                        char ts[64];
                        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmBuf);
                        output += std::string("last_open=") + ts + "  ";
                    }
                    if (r.lastEdit > 0)
                    {
                        time_t t = static_cast<time_t>(r.lastEdit);
                        struct tm tmBuf;
                        localtime_s(&tmBuf, &t);
                        char ts[64];
                        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmBuf);
                        output += std::string("last_edit=") + ts;
                    }
                    output += "\r\n";
                }
                output += "\r\n";
            }

            if (records.empty() && deltasPos != std::string::npos)
                output += "(No inference records match the selected entity types.)\r\n";
            else if (deltasPos == std::string::npos)
                output += response;

            delete pf;

            // Marshal result back to UI thread
            int wlen = MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, nullptr, 0);
            wchar_t* wResult = new wchar_t[wlen];
            MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, wResult, wlen);
            PostMessageW(hWnd, WM_QUERY_RESULT, 0, (LPARAM)wResult);
        }).detach();
    }
    else if (id == IDB_INFER_LOOKUP)
    {
        // Read the path from the edit control
        wchar_t pathBuf[1024] = {};
        GetWindowTextW(g_hEditInferPath, pathBuf, 1024);
        if (pathBuf[0] == L'\0')
        {
            SetWindowTextW(g_hResponse, L"Enter a file path, exe path, or URL to look up.");
            return;
        }

        // Convert to UTF-8 for JSON
        int len = WideCharToMultiByte(CP_UTF8, 0, pathBuf, -1, nullptr, 0, nullptr, nullptr);
        std::string pathUtf8(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, pathBuf, -1, &pathUtf8[0], len, nullptr, nullptr);

        // Escape for JSON
        std::string escaped;
        escaped.reserve(pathUtf8.size() + 32);
        for (char c : pathUtf8)
        {
            if (c == '"')       escaped += "\\\"";
            else if (c == '\\') escaped += "\\\\";
            else                 escaped += c;
        }

        std::string request = "{\"op\":\"QueryInferences\",\"paths\":[\"" + escaped + "\"]}";
        SendApiQuery(hWnd, request);
    }
}

void StopSubsystems()
{
    g_topicInference.Stop();
    g_queryApi.Stop();
    g_idleDetector.Stop();
    g_foregroundMonitor.Stop();
    g_browsingMonitor.Stop();
    g_appLaunchMonitor.Stop();
    g_fileMonitor.Stop();
    g_db.Close();
}
