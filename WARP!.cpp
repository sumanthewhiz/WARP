// WARP!.cpp : Defines the entry point for the application.
//
// Process model (interim always-on runtime, gap-analysis RUN-1..RUN-7):
//   * Started at logon by a per-user scheduled task ("--headless"); restarted by the task and
//     by Windows Error Reporting after a crash (CrashHandler); one instance per user (mutex).
//   * The message pump starts first; storage, models and capture start on a worker thread;
//     only the WinEvent hooks live on the UI thread. All DB writes go through the
//     ActivityStore writer thread.
//   * Session/power lifecycle: WM_ENDSESSION flushes and stops; presence (lock, sleep, away,
//     passive) comes from PresenceMonitor and never pauses capture.

#include "framework.h"
#include "WARP!.h"
#include "ActivityStore.h"
#include "EntityStore.h"
#include "HistoryEngine.h"
#include "ApiServer.h"
#include "FileCapture.h"
#include "LaunchCapture.h"
#include "WebCapture.h"
#include "FocusTracker.h"
#include "DocResolver.h"
#include "PresenceMonitor.h"
#include "ClipboardMonitor.h"
#include "PrintMonitor.h"
#include "ContextInference.h"
#include "LlmSummarizer.h"
#include "ForegroundChangeBroker.h"
#include "EventContext.h"
#include "EntityKey.h"
#include "Settings.h"
#include "Logger.h"
#include "CrashHandler.h"
#include "Installer.h"
#include "Json.h"

#include <string>
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <atomic>
#include <thread>
#include <vector>
#include <ctime>
#include <algorithm>
#include <cstdio>
#include <map>

#pragma comment(lib, "advapi32.lib")

#define MAX_LOADSTRING 100
#define WARP_PRODUCT_VERSION "6.0.0"

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
ActivityStore       g_store;
EntityStore         g_entities(&g_store);
HistoryEngine       g_history(&g_store);
ApiServer           g_api;
FileCapture         g_files;
LaunchCapture       g_launch;
WebCapture          g_web;
FocusTracker        g_focus;
ClipboardMonitor    g_clipboard;
PrintMonitor        g_print;
ContextInference    g_contextInference;
LlmSummarizer       g_llmSummarizer;

// Runtime state
static std::wstring       g_dataDir;
static bool               g_headless = false;
static HANDLE             g_instanceMutex = nullptr;
static HANDLE             g_shutdownEvent = nullptr;
static std::thread        g_shutdownWatcher;
static std::thread        g_startThread;
static std::thread        g_supervisorThread;             // capture heartbeats, off the UI thread
static HANDLE             g_supervisorStop = nullptr;     // manual-reset
static std::atomic<bool>  g_subsystemsUp{ false };
static std::atomic<bool>  g_stopping{ false };
static std::atomic<int>   g_presenceState{ 0 };        // PresenceState as int
static std::atomic<int64_t> g_presenceSinceMs{ 0 };
static std::mutex          g_presenceEvidenceMutex;
static std::string         g_presenceEpisode;
static int                 g_presenceRevision = 0;
static bool                g_presenceEvidencePaused = false;
static bool               g_capturePausedApplied = false;
static UINT               g_wmTaskbarCreated = 0;
static int64_t            g_startedUtc = 0;

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

// Recent context buttons + category / window selectors
static HWND g_hBtnRecentContext  = nullptr;
static HWND g_hBtnContextHistory = nullptr;
static HWND g_hCtxCategoryCombo  = nullptr;
static HWND g_hCtxWindowCombo    = nullptr;

// Forward declarations
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
void                AddTrayIcon(HWND hWnd);
void                UpdateTrayTip();
void                RemoveTrayIcon();
void                ShowTrayMenu(HWND hWnd);
void                MinimizeToTray(HWND hWnd);
void                RestoreFromTray(HWND hWnd);
std::wstring        GetDatabasePath();
void                StartSubsystems();          // worker thread: storage, models, API, capture
void                StartUiSubsystems();        // UI thread: hooks
void                StopSubsystems();
void                CreateUIControls(HWND hWnd, HINSTANCE hInstance);
void                LayoutControls(HWND hWnd);
void                SendApiQuery(HWND hWnd, const std::string& jsonRequest);
void                OnQueryButton(HWND hWnd, int id);
void                ToggleTheme(HWND hWnd);
void                ClearHistory(HWND hWnd);
void                OnInferenceButton(HWND hWnd, int id);
void                ApplyPauseState();
bool                SetCapturePaused(int64_t untilUtc);
void                PurgeRecent(HWND hWnd, int64_t seconds, const wchar_t* label);
static int          RunCli(int argc, wchar_t** argv, bool& handled);

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
namespace
{
    std::wstring UserSidString()
    {
        std::wstring out;
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return out;
        DWORD len = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
        std::vector<BYTE> buf(len ? len : 1);
        if (len && GetTokenInformation(tok, TokenUser, buf.data(), len, &len))
        {
            LPWSTR s = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &s))
            {
                out = s;
                LocalFree(s);
            }
        }
        CloseHandle(tok);
        return out;
    }

    bool IsElevated()
    {
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
        TOKEN_ELEVATION te = {};
        DWORD len = 0;
        bool e = GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &len) && te.TokenIsElevated;
        CloseHandle(tok);
        return e;
    }

    // PRIV-2: the data directory (DB, logs, dumps) is readable only by the user, SYSTEM and
    // Administrators (protected DACL, no inherited "Users" access) and EFS-encrypted when the
    // edition supports it.
    bool PrepareSecureDirectory(const std::wstring& dir, bool encrypt)
    {
        std::wstring sid = UserSidString();
        std::wstring sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
        if (!sid.empty()) sddl += L"(A;OICI;FA;;;" + sid + L")";
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
            return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
        SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
        bool ok = CreateDirectoryW(dir.c_str(), &sa) != FALSE;
        if (!ok && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            BOOL present = FALSE, defaulted = FALSE;
            PACL dacl = nullptr;
            if (GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present)
                SetNamedSecurityInfoW(const_cast<LPWSTR>(dir.c_str()), SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                      nullptr, nullptr, dacl, nullptr);
            ok = true;
        }
        LocalFree(sd);
        if (ok && encrypt)
        {
            DWORD attrs = GetFileAttributesW(dir.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_ENCRYPTED))
                EncryptFileW(dir.c_str());                  // best effort (EFS needs Pro/Enterprise)
        }
        return ok;
    }

    std::wstring ExeDir()
    {
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
        std::wstring p(buf, n);
        size_t s = p.find_last_of(L"\\/");
        return s == std::wstring::npos ? p : p.substr(0, s);
    }

    std::wstring ArgValue(int argc, wchar_t** argv, const wchar_t* name)
    {
        for (int i = 1; i + 1 < argc; ++i)
            if (_wcsicmp(argv[i], name) == 0) return argv[i + 1];
        return std::wstring();
    }

    bool HasArg(int argc, wchar_t** argv, const wchar_t* name)
    {
        for (int i = 1; i < argc; ++i)
            if (_wcsicmp(argv[i], name) == 0) return true;
        return false;
    }

    // Writes CLI output to redirected stdout (scripts, CI), else the parent console, else a message box.
    void CliPrint(const std::wstring& text)
    {
        const std::wstring line = text + L"\r\n";
        auto writeTo = [&line](HANDLE out) -> bool
        {
            if (out == nullptr || out == INVALID_HANDLE_VALUE) return false;
            DWORD w = 0, mode = 0;
            if (GetConsoleMode(out, &mode))       // a console: UTF-16 keeps non-ASCII paths and titles readable
                return WriteConsoleW(out, line.c_str(), (DWORD)line.size(), &w, nullptr) != FALSE;
            if (GetFileType(out) == FILE_TYPE_UNKNOWN) return false;
            const std::string u = WarpJson::WideToUtf8(line);   // file, pipe or NUL: UTF-8
            return WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr) != FALSE;
        };
        if (writeTo(GetStdHandle(STD_OUTPUT_HANDLE))) return;
        if (AttachConsole(ATTACH_PARENT_PROCESS) || GetConsoleWindow())
        {
            if (writeTo(GetStdHandle(STD_OUTPUT_HANDLE))) return;
            HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            const bool ok = writeTo(con);
            if (con != INVALID_HANDLE_VALUE) CloseHandle(con);
            if (ok) return;
        }
        MessageBoxW(nullptr, text.c_str(), L"WARP", MB_OK | MB_ICONINFORMATION);
    }

    // Sends one request to the running instance in this session (same exe => control scope).
    std::string PipeRequest(const std::string& req)
    {
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        std::wstring name = ApiServer::SessionPipeName(session);
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return std::string();
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
        DWORD n = 0;
        WriteFile(h, req.data(), (DWORD)req.size(), &n, nullptr);
        std::string resp;
        std::vector<char> buf(65536);
        for (;;)
        {
            if (ReadFile(h, buf.data(), (DWORD)buf.size(), &n, nullptr)) { resp.append(buf.data(), n); break; }
            if (GetLastError() != ERROR_MORE_DATA) break;
            resp.append(buf.data(), n);
        }
        CloseHandle(h);
        return resp;
    }
}

// CLI verbs: --install, --uninstall [--remove-data], --start, --status, --pause <minutes>, --resume,
// --purge-hours <n>, --purge-domain <host>, --purge-path <folder>.
static int RunCli(int argc, wchar_t** argv, bool& handled)
{
    handled = true;
    const bool install = HasArg(argc, argv, L"--install");
    const bool uninstall = HasArg(argc, argv, L"--uninstall");
    if ((install || uninstall) && !IsElevated())
    {
        // Installing a highest-privilege logon task and writing Program Files needs elevation.
        wchar_t exe[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
        std::wstring params = install ? L"--install" : (HasArg(argc, argv, L"--remove-data") ? L"--uninstall --remove-data" : L"--uninstall");
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = exe;
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei)) { CliPrint(L"Administrator approval is required."); return 1; }
        DWORD code = 1;
        if (sei.hProcess)
        {
            WaitForSingleObject(sei.hProcess, INFINITE);
            GetExitCodeProcess(sei.hProcess, &code);
            CloseHandle(sei.hProcess);
        }
        return (int)code;
    }
    if (install)
    {
        Installer::Result r = Installer::Install(true);
        CliPrint(r.ok ? L"WARP installed and started: " + r.message : L"Install failed: " + r.message);
        return r.ok ? 0 : 1;
    }
    if (uninstall)
    {
        Installer::Result r = Installer::Uninstall(true, HasArg(argc, argv, L"--remove-data"));
        CliPrint(r.ok ? L"WARP uninstalled. " + r.message : L"Uninstall failed: " + r.message);
        return r.ok ? 0 : 1;
    }
    if (HasArg(argc, argv, L"--start"))
    {
        // Starts the installed logon task now (elevated agent, no UAC prompt).
        Installer::Result r = Installer::StartTask();
        CliPrint(r.ok ? L"WARP started." : L"Start failed: " + r.message);
        return r.ok ? 0 : 1;
    }
    std::string req;
    if (HasArg(argc, argv, L"--status")) req = "{\"op\":\"GetStatus\"}";
    else if (HasArg(argc, argv, L"--resume")) req = "{\"op\":\"Resume\"}";
    else if (HasArg(argc, argv, L"--pause"))
        req = "{\"op\":\"Pause\",\"minutes\":" + std::to_string(_wtoi(ArgValue(argc, argv, L"--pause").c_str()) > 0 ? _wtoi(ArgValue(argc, argv, L"--pause").c_str()) : 60) + "}";
    else if (HasArg(argc, argv, L"--purge-hours"))
    {
        int64_t now = (int64_t)time(nullptr);
        req = "{\"op\":\"Purge\",\"start\":" + std::to_string(now - (int64_t)_wtoi(ArgValue(argc, argv, L"--purge-hours").c_str()) * 3600) +
              ",\"end\":" + std::to_string(now + 60) + "}";
    }
    else if (HasArg(argc, argv, L"--purge-domain") || HasArg(argc, argv, L"--purge-path"))
    {
        std::string out;
        WarpJson::Writer w(out);
        w.BeginObject().KV("op", "Purge");
        if (HasArg(argc, argv, L"--purge-domain")) w.KV("domain", ArgValue(argc, argv, L"--purge-domain"));
        if (HasArg(argc, argv, L"--purge-path")) w.KV("path_prefix", ArgValue(argc, argv, L"--purge-path"));
        w.EndObject();
        req = out;
    }
    else
    {
        handled = false;
        return 0;
    }
    std::string resp = PipeRequest(req);
    CliPrint(resp.empty() ? L"WARP is not running in this session." : WarpJson::Utf8ToWide(resp));
    return resp.empty() ? 2 : 0;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    // SEC-1: never resolve DLLs from the current directory or PATH.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    {
        bool handled = false;
        int rc = argv ? RunCli(argc, argv, handled) : 0;
        if (handled)
        {
            LocalFree(argv);
            return rc;
        }
    }
    g_headless = argv && HasArg(argc, argv, L"--headless") && !HasArg(argc, argv, L"--show");
    std::wstring dataDirArg = argv ? ArgValue(argc, argv, L"--data-dir") : std::wstring();
    bool restarted = argv && HasArg(argc, argv, L"--restarted");
    if (argv) LocalFree(argv);

    // RUN-3: one instance per user. A second launch just surfaces the first one's window.
    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_WARP, szWindowClass, MAX_LOADSTRING);
    std::wstring mutexName = L"Local\\WARP-" + UserSidString();
    g_instanceMutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    const DWORD mutexErr = GetLastError();
    // ERROR_ACCESS_DENIED: the running instance is elevated (logon task) and this one is not.
    if ((g_instanceMutex && mutexErr == ERROR_ALREADY_EXISTS) || (!g_instanceMutex && mutexErr == ERROR_ACCESS_DENIED))
    {
        HWND existing = FindWindowW(szWindowClass, nullptr);
        if (existing && !g_headless) PostMessageW(existing, WM_APP_SHOW, 0, 0);
        if (g_instanceMutex) CloseHandle(g_instanceMutex);
        return 0;
    }

    // Manual, unelevated launch: kernel ETW capture needs the elevated logon task. Start the
    // installed task (no UAC prompt), or offer the one-time install; otherwise run degraded.
    if (!g_headless && !IsElevated())
    {
        if (Installer::IsTaskInstalled())
        {
            if (g_instanceMutex) { CloseHandle(g_instanceMutex); g_instanceMutex = nullptr; }
            Installer::Result r = Installer::StartTask();
            if (r.ok) return 0;
            g_instanceMutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
        }
        else if (MessageBoxW(nullptr,
                     L"WARP needs a one-time installation (administrator approval) to run at every sign-in and "
                     L"to capture file and app activity.\n\nInstall now? Choose No to run without file/app capture.",
                     L"WARP", MB_YESNO | MB_ICONQUESTION) == IDYES)
        {
            wchar_t exe[MAX_PATH * 2] = {};
            GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.lpVerb = L"runas";
            sei.lpFile = exe;
            sei.lpParameters = L"--install";
            sei.nShow = SW_HIDE;
            if (g_instanceMutex) { CloseHandle(g_instanceMutex); g_instanceMutex = nullptr; }
            if (ShellExecuteExW(&sei)) return 0;
            g_instanceMutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
        }
    }

    Settings::Instance().Load();
    WarpSettings cfg = Settings::Instance().Get();

    if (!dataDirArg.empty()) g_dataDir = dataDirArg;
    else
    {
        wchar_t appData[MAX_PATH] = {};
        SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, appData);
        g_dataDir = std::wstring(appData) + L"\\WARP";
    }
    PrepareSecureDirectory(g_dataDir, cfg.encryptDataDir);
    PrepareSecureDirectory(g_dataDir + L"\\logs", false);
    PrepareSecureDirectory(g_dataDir + L"\\dumps", false);

    Log::Init(g_dataDir + L"\\logs", (Log::Level)(std::max)(0, (std::min)(3, cfg.logLevel)));
    CrashHandler::Install(g_dataDir + L"\\dumps", L"--headless --restarted");
    g_startedUtc = (int64_t)time(nullptr);
    WARP_LOG_INFO("main", "WARP %s starting (pid=%lu, session=%lu, headless=%d, restarted=%d, elevated=%d)",
                  WARP_PRODUCT_VERSION, GetCurrentProcessId(), EventContextUtil::OwnSessionId(),
                  g_headless ? 1 : 0, restarted ? 1 : 0, IsElevated() ? 1 : 0);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    MyRegisterClass(hInstance);

    if (!InitInstance(hInstance, SW_HIDE))
    {
        WARP_LOG_ERROR("main", "main window could not be created");
        return FALSE;
    }

    // PRIV-3: first-run disclosure / consent. Declining starts WARP paused (resume from the tray).
    if (!cfg.consentAcknowledged)
    {
        int r = MessageBoxW(g_hWnd,
            L"WARP records which files, apps and web pages you use on this PC (names, titles, paths and "
            L"URLs - never file contents, keystrokes or screenshots) and keeps this history locally to help "
            L"apps such as Search prioritize what matters to you.\n\n"
            L"Private browsing windows, sign-in pages and excluded apps/sites are never recorded. You can pause "
            L"or delete history at any time from the tray icon.\n\nStart recording now?",
            L"WARP - Activity history", MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND);
        Settings::Instance().SetConsentAcknowledged(true);
        if (r != IDYES) Settings::Instance().SetPausedUntil(INT64_MAX);
    }

    // Installer / uninstaller asks a running agent to exit through this event.
    g_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, Installer::ShutdownEventName());
    if (g_shutdownEvent)
    {
        g_shutdownWatcher = std::thread([]() {
            HANDLE hs[1] = { g_shutdownEvent };
            if (WaitForMultipleObjects(1, hs, FALSE, INFINITE) == WAIT_OBJECT_0 && !g_stopping.load() && g_hWnd)
                PostMessageW(g_hWnd, WM_APP_SHUTDOWN, 0, 0);
        });
    }

    // RUN-7: pump first; storage, models and capture start on a worker.
    g_startThread = std::thread([]() {
        SetThreadDescription(GetCurrentThread(), L"WARP startup");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        StartSubsystems();
        CoUninitialize();
    });
    SetTimer(g_hWnd, IDT_EVICTION_TIMER, 6 * 60 * 60 * 1000, nullptr);
    SetTimer(g_hWnd, IDT_SUPERVISOR_TIMER, 30 * 1000, nullptr);
    if (!g_headless) RestoreFromTray(g_hWnd);

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

    if (g_shutdownEvent)
    {
        SetEvent(g_shutdownEvent);
        if (g_shutdownWatcher.joinable()) g_shutdownWatcher.join();
        CloseHandle(g_shutdownEvent);
    }
    WARP_LOG_INFO("main", "WARP stopped");
    Log::Shutdown();
    CoUninitialize();
    if (g_instanceMutex) CloseHandle(g_instanceMutex);
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

    // RUN-4: be told about logoff/shutdown ordering and Explorer restarts.
    SetProcessShutdownParameters(0x3FF, SHUTDOWN_NORETRY);
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_hWnd, g_wmTaskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_hWnd, WM_APP_SHOW, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(g_hWnd, WM_TRAYICON, MSGFLT_ALLOW, nullptr);   // Explorer (medium IL) -> elevated tray callback

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
        L"Query with Default (empty body � returns last 1 hour)",
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

    // Recent context buttons (latest snapshot + last-N history)
    g_hBtnRecentContext = CreateWindowW(L"BUTTON", L"Show Context Summary",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDB_RECENT_CONTEXT, hInstance, nullptr);
    SendMessageW(g_hBtnRecentContext, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    g_hBtnContextHistory = CreateWindowW(L"BUTTON", L"Show Summary History (last 10)",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
        0, 0, 0, 0, hWnd, (HMENU)IDB_CONTEXT_HISTORY, hInstance, nullptr);
    SendMessageW(g_hBtnContextHistory, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);

    // Category drop-down: narrows the context one-liner returned by the
    // two buttons above to a single facet (Files / Websites / Apps)
    // or shows the combined view (All).  Default = All.
    g_hCtxCategoryCombo = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
        CBS_DROPDOWNLIST | CBS_HASSTRINGS,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CTX_CATEGORY, hInstance, nullptr);
    SendMessageW(g_hCtxCategoryCombo, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hCtxCategoryCombo, CB_ADDSTRING, 0, (LPARAM)L"All");
    SendMessageW(g_hCtxCategoryCombo, CB_ADDSTRING, 0, (LPARAM)L"Files");
    SendMessageW(g_hCtxCategoryCombo, CB_ADDSTRING, 0, (LPARAM)L"Websites");
    SendMessageW(g_hCtxCategoryCombo, CB_ADDSTRING, 0, (LPARAM)L"Apps");
    SendMessageW(g_hCtxCategoryCombo, CB_SETCURSEL, 0, 0); // default "All"

    // Window-length drop-down: controls how far back the snapshot
    // looks for activity.  Default = Last 15 minutes (which matches
    // the rolling-history compute cadence).
    g_hCtxWindowCombo = CreateWindowExW(0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
        CBS_DROPDOWNLIST | CBS_HASSTRINGS,
        0, 0, 0, 0, hWnd, (HMENU)IDC_CTX_WINDOW, hInstance, nullptr);
    SendMessageW(g_hCtxWindowCombo, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hCtxWindowCombo, CB_ADDSTRING, 0, (LPARAM)L"Last 5 minutes");
    SendMessageW(g_hCtxWindowCombo, CB_ADDSTRING, 0, (LPARAM)L"Last 15 minutes");
    SendMessageW(g_hCtxWindowCombo, CB_ADDSTRING, 0, (LPARAM)L"Last 30 minutes");
    SendMessageW(g_hCtxWindowCombo, CB_ADDSTRING, 0, (LPARAM)L"Last 1 hour");
    SendMessageW(g_hCtxWindowCombo, CB_ADDSTRING, 0, (LPARAM)L"Last 2 hours");
    SendMessageW(g_hCtxWindowCombo, CB_ADDSTRING, 0, (LPARAM)L"Last 6 hours");
    SendMessageW(g_hCtxWindowCombo, CB_ADDSTRING, 0, (LPARAM)L"Last 24 hours");
    SendMessageW(g_hCtxWindowCombo, CB_ADDSTRING, 0, (LPARAM)L"Last 7 days");
    SendMessageW(g_hCtxWindowCombo, CB_ADDSTRING, 0, (LPARAM)L"Last 15 days");
    SendMessageW(g_hCtxWindowCombo, CB_ADDSTRING, 0, (LPARAM)L"Last 30 days");
    SendMessageW(g_hCtxWindowCombo, CB_SETCURSEL, 1, 0); // default "Last 15 minutes"

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

    // Recent context buttons + category selector + window selector
    int ctxBtnW       = 200;
    int ctxBtnW2      = 250;
    int ctxCatComboW  = 110;
    int ctxWinComboW  = 160;
    int xCursor = pad;
    MoveWindow(g_hBtnRecentContext,  xCursor, y, ctxBtnW,   30, TRUE);
    xCursor += ctxBtnW + gap;
    MoveWindow(g_hBtnContextHistory, xCursor, y, ctxBtnW2,  30, TRUE);
    xCursor += ctxBtnW2 + gap;
    // ComboBox visible height = field + drop-list; CB_DROPDOWNLIST uses
    // the height parameter as the *combined* drop-list height.  Give
    // the category combo ~6 lines (4 items + room) and the window
    // combo ~12 lines (10 items + room).
    MoveWindow(g_hCtxCategoryCombo,  xCursor, y, ctxCatComboW, 30 + 6  * 20, TRUE);
    xCursor += ctxCatComboW + gap;
    MoveWindow(g_hCtxWindowCombo,    xCursor, y, ctxWinComboW, 30 + 12 * 20, TRUE);
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

// Walks a pretty-printed JSON string line by line and, on any line that
// declares a known epoch-seconds timestamp field, appends a JS-style
// "// YYYY-MM-DD HH:MM:SS" annotation. The output is intended for the
// API Response edit control only -- it is not strictly valid JSON
// after annotation, but it is human-readable and round-trips through
// any JSON-tolerant viewer that ignores // comments.
//
// Annotation rules:
//   * Only lines containing one of TS_KEYS get annotated.
//   * The first decimal-integer value after the ":" is interpreted as
//     a Unix epoch in seconds.
//   * Values outside [1e9, 1e11] are skipped (filters out 0, recency
//     scores, version numbers, and millisecond timestamps that would
//     blow past the year 5138 if treated as seconds).
//   * Conversion uses localtime_s so the displayed time matches the
//     user's wall clock.
static std::string AnnotateTimestamps(const std::string& s)
{
    static const char* const TS_KEYS[] = {
        "\"timestamp\"",
        "\"last_event_ts\"",
        "\"last_open_ts\"",
        "\"last_edit_ts\"",
        "\"updated_at\"",
        "\"first_seen_ts\"",
        "\"window_start\"",
        "\"window_end\"",
        "\"created_window_ms\"", // also epoch-like in some payloads
    };

    std::string out;
    out.reserve(s.size() + 256);

    size_t lineStart = 0;
    for (size_t i = 0; i <= s.size(); ++i)
    {
        bool atEnd = (i == s.size());
        bool atNewline = !atEnd && s[i] == '\n';
        if (!atEnd && !atNewline) continue;

        std::string line = s.substr(lineStart, i - lineStart);
        while (!line.empty() && line.back() == '\r') line.pop_back();

        for (const char* k : TS_KEYS)
        {
            size_t kp = line.find(k);
            if (kp == std::string::npos) continue;
            size_t cp = line.find(':', kp);
            if (cp == std::string::npos) continue;

            size_t vp = cp + 1;
            while (vp < line.size() && (line[vp] == ' ' || line[vp] == '\t'))
                ++vp;

            size_t ve = vp;
            while (ve < line.size() && line[ve] >= '0' && line[ve] <= '9')
                ++ve;
            if (ve == vp) break; // no integer to annotate

            int64_t epoch = _strtoi64(line.c_str() + vp, nullptr, 10);
            if (epoch < 1000000000LL || epoch > 99999999999LL)
                break; // implausible as Unix-seconds; leave alone

            time_t t = static_cast<time_t>(epoch);
            struct tm tmBuf;
            if (localtime_s(&tmBuf, &t) != 0) break;

            char ts[64];
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmBuf);

            // Append the annotation. A trailing comma may already be
            // present after the integer (",\r\n") -- preserve it by
            // splicing in just before the first non-numeric char that
            // is also non-comma.
            size_t insertAt = ve;
            if (insertAt < line.size() && line[insertAt] == ',')
                ++insertAt;

            std::string annotation = "  // ";
            annotation += ts;
            line.insert(insertAt, annotation);
            break; // one annotation per line is enough
        }

        out += line;
        if (atNewline)
        {
            out += "\r\n";
            lineStart = i + 1;
        }
    }
    return out;
}

void SendApiQuery(HWND hWnd, const std::string& jsonRequest)
{
    // Show "Querying..." immediately
    SetWindowTextW(g_hResponse, L"Querying...");

    // Copy request for the thread
    std::string* req = new std::string(jsonRequest);

    std::thread([hWnd, req]() {
        std::string response = g_subsystemsUp.load()
            ? g_api.Handle(*req, ApiClient::InProcess())
            : std::string("Error: WARP is still starting up - try again in a moment.");

        {
            if (!response.empty() && response[0] == '{')
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
                response = AnnotateTimestamps(pretty);
            }
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
    if (g_wmTaskbarCreated != 0 && message == g_wmTaskbarCreated)
    {
        AddTrayIcon(hWnd);                 // Explorer restarted: the icon must be re-added (RUN-8)
        UpdateTrayTip();
        return 0;
    }
    switch (message)
    {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP)
            ShowTrayMenu(hWnd);
        else if (lParam == WM_LBUTTONDBLCLK)
            RestoreFromTray(hWnd);
        break;

    case WM_APP_START_UI:
        StartUiSubsystems();
        break;

    case WM_APP_READY:
        UpdateTrayTip();
        break;

    case WM_APP_SHOW:
        RestoreFromTray(hWnd);
        break;

    case WM_APP_SHUTDOWN:
        RemoveTrayIcon();
        DestroyWindow(hWnd);
        break;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        // RUN-4: logoff / shutdown - flush the open focus session and in-flight writes now;
        // the process may be terminated right after this message returns.
        if (wParam)
        {
            WARP_LOG_INFO("main", "session ending (flags=0x%llx): flushing", (unsigned long long)lParam);
            StopSubsystems();
        }
        return 0;

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
        else if (wmId == IDM_TRAY_STATUS)
        {
            RestoreFromTray(hWnd);
            SendApiQuery(hWnd, "{\"op\":\"GetStatus\"}");
        }
        else if (wmId == IDM_TRAY_PAUSE_1H)
            SetCapturePaused((int64_t)time(nullptr) + 3600);
        else if (wmId == IDM_TRAY_PAUSE_TOMORROW)
        {
            time_t now = time(nullptr);
            struct tm lt = {};
            localtime_s(&lt, &now);
            lt.tm_mday += 1;
            lt.tm_hour = 6;
            lt.tm_min = 0;
            lt.tm_sec = 0;
            SetCapturePaused((int64_t)mktime(&lt));
        }
        else if (wmId == IDM_TRAY_RESUME)
            SetCapturePaused(0);
        else if (wmId == IDM_TRAY_PURGE_15M)
            PurgeRecent(hWnd, 15 * 60, L"the last 15 minutes");
        else if (wmId == IDM_TRAY_PURGE_1H)
            PurgeRecent(hWnd, 3600, L"the last hour");
        else if (wmId == IDM_TRAY_PURGE_TODAY)
        {
            time_t now = time(nullptr);
            struct tm lt = {};
            localtime_s(&lt, &now);
            lt.tm_hour = 0;
            lt.tm_min = 0;
            lt.tm_sec = 0;
            PurgeRecent(hWnd, (int64_t)now - (int64_t)mktime(&lt), L"today");
        }
        else if (wmId == IDM_TRAY_PURGE_ALL)
            ClearHistory(hWnd);
        else if (wmId >= IDB_QUERY_15M && wmId <= IDB_QUERY_CUSTOM)
            OnQueryButton(hWnd, wmId);
        else if (wmId == IDB_TOGGLE_THEME)
            ToggleTheme(hWnd);
        else if (wmId == IDB_CLEAR_HISTORY)
            ClearHistory(hWnd);
        else if (wmId == IDB_INFER_TOP || wmId == IDB_INFER_LOOKUP)
            OnInferenceButton(hWnd, wmId);
        else if (wmId == IDB_RECENT_CONTEXT)
        {
            // Read current category + window from the dropdowns and
            // forward them to ContextInference; defaults are "all" /
            // 15 minutes.
            int catSel = (int)SendMessageW(g_hCtxCategoryCombo, CB_GETCURSEL, 0, 0);
            const char* cat = "all";
            if      (catSel == 1) cat = "files";
            else if (catSel == 2) cat = "websites";
            else if (catSel == 3) cat = "apps";
            int winSel = (int)SendMessageW(g_hCtxWindowCombo, CB_GETCURSEL, 0, 0);
            // Index mapping must match CreateUIControls dropdown order.
            static const long long kWinSecs[] = {
                300, 900, 1800, 3600, 7200, 21600, 86400,
                604800, 1296000, 2592000
            };
            long long winSecs = 900; // default 15 min
            if (winSel >= 0 && winSel < (int)(sizeof(kWinSecs)/sizeof(kWinSecs[0])))
                winSecs = kWinSecs[winSel];
            std::string req = std::string("{\"op\":\"GetRecentContext\",\"category\":\"")
                            + cat
                            + "\",\"window_seconds\":"
                            + std::to_string(winSecs)
                            + "}";
            SendApiQuery(hWnd, req);
        }
        else if (wmId == IDB_CONTEXT_HISTORY)
        {
            int catSel = (int)SendMessageW(g_hCtxCategoryCombo, CB_GETCURSEL, 0, 0);
            const char* cat = "all";
            if      (catSel == 1) cat = "files";
            else if (catSel == 2) cat = "websites";
            else if (catSel == 3) cat = "apps";
            int winSel = (int)SendMessageW(g_hCtxWindowCombo, CB_GETCURSEL, 0, 0);
            static const long long kWinSecs[] = {
                300, 900, 1800, 3600, 7200, 21600, 86400,
                604800, 1296000, 2592000
            };
            long long winSecs = 900;
            if (winSel >= 0 && winSel < (int)(sizeof(kWinSecs)/sizeof(kWinSecs[0])))
                winSecs = kWinSecs[winSel];
            std::string req = std::string("{\"op\":\"GetRecentContexts\",\"count\":10,\"category\":\"")
                            + cat
                            + "\",\"window_seconds\":"
                            + std::to_string(winSecs)
                            + "}";
            SendApiQuery(hWnd, req);
        }
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
            // Retention, tombstones, rollup refresh, incremental vacuum - all on the store's
            // writer thread (RUN-7, DATA-4); the UI thread never blocks on the database.
            if (g_subsystemsUp.load()) g_store.ScheduleMaintenance();
        }
        else if (wParam == IDT_SUPERVISOR_TIMER)
        {
            // RUN-5 supervisor (UI part): apply pause expiry and refresh the tray health. Capture
            // heartbeats run on the supervisor thread so ETW restarts never stall the hooks.
            if (g_subsystemsUp.load()) ApplyPauseState();
            UpdateTrayTip();
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
        KillTimer(hWnd, IDT_SUPERVISOR_TIMER);
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
    wcscpy_s(nid.szTip, L"WARP - starting");
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (Shell_NotifyIconW(NIM_ADD, &nid)) break;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        Sleep(200);
    }
}

// Tray tooltip reflects health (RUN-5): recording / paused / degraded.
void UpdateTrayTip()
{
    if (!nid.hWnd) return;
    std::wstring tip;
    const int64_t now = (int64_t)time(nullptr);
    WarpSettings cfg = Settings::Instance().Get();
    if (!g_subsystemsUp.load())
        tip = L"WARP - starting";
    else if (Settings::Instance().IsPaused(now))
    {
        if (cfg.pausedUntilUtc == INT64_MAX) tip = L"WARP - paused (resume from this menu)";
        else
        {
            time_t t = (time_t)cfg.pausedUntilUtc;
            struct tm lt = {};
            localtime_s(&lt, &t);
            wchar_t buf[64] = {};
            wcsftime(buf, 64, L"%a %H:%M", &lt);
            tip = std::wstring(L"WARP - paused until ") + buf;
        }
    }
    else
    {
        std::wstring problems;
        FileCapture::Stats fs = g_files.GetStats();
        LaunchCapture::Stats ls = g_launch.GetStats();
        ActivityStore::Stats ss = g_store.GetStats();
        if (cfg.captureFiles && !fs.etwRunning && !fs.usnRunning) problems += L" files";
        if (cfg.captureAppLaunch && !ls.etwRunning) problems += L" launches";
        if (!g_store.IsOpen()) problems += L" storage";
        if (ss.dropped > 0 && ss.queueDepth > 10000) problems += L" backlog";
        tip = problems.empty() ? L"WARP - recording your activity" : L"WARP - degraded:" + problems;
    }
    if (tip.size() >= 127) tip.resize(127);
    wcscpy_s(nid.szTip, tip.c_str());
    nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void ShowTrayMenu(HWND hWnd)
{
    POINT pt;
    GetCursorPos(&pt);
    const bool paused = Settings::Instance().IsPaused((int64_t)time(nullptr));

    HMENU hPurge = CreatePopupMenu();
    AppendMenuW(hPurge, MF_STRING, IDM_TRAY_PURGE_15M, L"Last 15 minutes");
    AppendMenuW(hPurge, MF_STRING, IDM_TRAY_PURGE_1H, L"Last hour");
    AppendMenuW(hPurge, MF_STRING, IDM_TRAY_PURGE_TODAY, L"Today");
    AppendMenuW(hPurge, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hPurge, MF_STRING, IDM_TRAY_PURGE_ALL, L"Everything...");

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_OPEN, L"Open");
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_STATUS, L"Status");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    if (paused)
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_RESUME, L"Resume recording");
    else
    {
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_PAUSE_1H, L"Pause for 1 hour");
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_PAUSE_TOMORROW, L"Pause until tomorrow");
    }
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hPurge, L"Delete activity");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, hWnd, nullptr);
    PostMessage(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);        // also destroys the submenu
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
        L"This permanently and securely deletes everything WARP has recorded "
        L"(raw events, per-item history, work threads and summaries).",
        L"WARP - Clear History",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);

    if (result == IDYES)
    {
        SetWindowTextW(g_hResponse, L"Clearing...");
        std::thread([hWnd]() {
            // PRIV-4: context history first (epoch-safe), then a secure wipe on the writer thread
            // (secure_delete + WAL truncate + VACUUM) so no in-flight event can resurrect anything.
            g_contextInference.ClearHistory();
            bool ok = g_store.ClearAll();
            g_contextInference.ClearHistory();
            const wchar_t* msg = ok ? L"Activity history cleared." : L"Clearing failed - see the log for details.";
            size_t n = wcslen(msg) + 1;
            wchar_t* copy = new wchar_t[n];
            wcscpy_s(copy, n, msg);
            PostMessageW(hWnd, WM_QUERY_RESULT, 0, (LPARAM)copy);
        }).detach();
    }
}

void PurgeRecent(HWND hWnd, int64_t seconds, const wchar_t* label)
{
    std::wstring q = std::wstring(L"Delete everything WARP recorded during ") + label + L"?";
    if (MessageBoxW(hWnd, q.c_str(), L"WARP - Delete activity", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    const int64_t now = (int64_t)time(nullptr);
    std::string req = "{\"op\":\"Purge\",\"start\":" + std::to_string(now - (std::max<int64_t>)(1, seconds)) +
                      ",\"end\":" + std::to_string(now + 60) + "}";
    RestoreFromTray(hWnd);
    SendApiQuery(hWnd, req);
}

// Applies the effective pause state (user pause / policy) to every capture source.
namespace { std::string PresenceName(); }

static void CheckpointPresence(const char* reason, bool close, const std::string& state, bool paused)
{
    std::lock_guard<std::mutex> lock(g_presenceEvidenceMutex);
    const int64_t now = StoreUtil::NowMs();
    PresenceRecord r;
    r.startMs = g_presenceSinceMs.exchange(now); r.endMs = now; r.state = state;
    if (g_presenceEpisode.empty()) g_presenceEpisode = Evidence::NewId("presence");
    r.evidence.episodeId = g_presenceEpisode; r.evidence.revision = ++g_presenceRevision;
    r.evidence.id = g_presenceEpisode + ":" + std::to_string(g_presenceRevision);
    r.evidence.timestampKind = "presence_sample"; r.evidence.boundaryReason = reason; r.evidence.open = !close;
    r.evidence.observedMs = now; r.evidence.actorMethod = "device_presence";
    if (r.startMs>0 && r.endMs>r.startMs && !g_presenceEvidencePaused &&
        Settings::Instance().Get().capturePresence) g_store.RecordPresence(std::move(r));
    g_presenceEvidencePaused = paused;
    if (close) { g_presenceEpisode.clear(); g_presenceRevision = 0; }
}

static void CheckpointCoverage()
{
    static std::mutex mutex;
    static int64_t since = 0;
    static std::map<std::string,std::pair<std::string,uint64_t>> previous;
    static uint64_t previousWebWithheld = 0;
    std::lock_guard<std::mutex> lock(mutex);
    const int64_t now = StoreUtil::NowMs();
    const auto cfg = Settings::Instance().Get();
    const bool paused = Settings::Instance().IsPaused(now/1000);
    const auto files = g_files.GetStats();
    const auto launch = g_launch.GetStats();
    const auto web = g_web.GetStats();
    const auto store = g_store.GetStats();
    auto state = [&](bool enabled, bool running) -> std::string {
        return !enabled ? "disabled" : paused ? "paused" : running ? "healthy" : "unavailable";
    };
    std::map<std::string,std::pair<std::string,uint64_t>> current = {
        {"files",{state(cfg.captureFiles,files.etwRunning && (files.usnRunning || files.rdcwRoots>0)),files.queueDropped+files.etwEventsLost+files.etwBuffersLost+files.rdcwOverflows}},
        {"launch",{state(cfg.captureAppLaunch,launch.etwRunning),launch.eventsLost}},
        {"browser",{state(cfg.captureWeb,web.running && web.uiaAvailable),web.queueDropped+web.uiaTimeouts+web.uiaFailures}},
        {"focus",{state(cfg.captureFocus,true),0}},
        {"presence",{state(cfg.capturePresence,true),0}},
        {"storage",{g_store.IsOpen() ? "healthy" : "unavailable",store.dropped+store.writeErrors}}
    };
    if (files.etwUnconfirmed && current["files"].first=="healthy") current["files"].first="degraded";
    for (const auto& pair:current)
    {
        if (!since) break;
        const auto old=previous.find(pair.first);
        SourceCoverage c; c.source=pair.first; c.startMs=since; c.endMs=now;
        c.state=old==previous.end() ? "unknown" : old->second.first;
        c.dropped=old==previous.end() || pair.second.second<old->second.second ? 0 : pair.second.second-old->second.second;
        if (c.dropped) { c.state="degraded"; c.reason="lost_or_failed_observations"; }
        else if (pair.second.first!=c.state) { c.state="degraded"; c.reason="state_transition_within_checkpoint"; }
        else c.reason=c.state=="healthy" ? "collector_available_not_complete_recall" : c.state;
        if (c.source=="browser" && web.privateDropped+web.excludedDropped>previousWebWithheld)
        { c.state="privacy_withheld"; c.reason="private_or_excluded_content"; }
        g_store.RecordCoverage(std::move(c));
    }
    since=now; previous=std::move(current);
    previousWebWithheld=web.privateDropped+web.excludedDropped;
}

void ApplyPauseState()
{
    const bool paused = Settings::Instance().IsPaused((int64_t)time(nullptr));
    if (paused == g_capturePausedApplied) return;
    CheckpointPresence(paused ? "pause" : "resume",true,PresenceName(),paused);
    CheckpointCoverage();
    g_capturePausedApplied = paused;
    g_files.SetPaused(paused);
    g_launch.SetPaused(paused);
    g_web.SetPaused(paused);
    g_focus.SetPaused(paused);
    WARP_LOG_INFO("main", "capture %s", paused ? "paused" : "resumed");
}

bool SetCapturePaused(int64_t untilUtc)
{
    bool ok = Settings::Instance().SetPausedUntil(untilUtc);
    if (!ok) WARP_LOG_WARN("main", "pause state is locked by policy");
    if (g_subsystemsUp.load()) ApplyPauseState();
    UpdateTrayTip();
    return ok;
}

void RestoreFromTray(HWND hWnd)
{
    ShowWindow(hWnd, SW_SHOW);
    ShowWindow(hWnd, SW_SHOWMAXIMIZED);
    SetForegroundWindow(hWnd);
}

std::wstring GetDatabasePath()
{
    return g_dataDir + L"\\activity.db";
}

// ---------------------------------------------------------------------------
// UI-thread WinEvent hook for launch correlation (APP-2/APP-3): EVENT_OBJECT_SHOW of a
// top-level window completes a parked process start. OUTOFCONTEXT events are delivered to
// this (pumping) thread; the handler only does a few cheap window checks.
// ---------------------------------------------------------------------------
static HWINEVENTHOOK g_hWinEventHook = nullptr;

static void CALLBACK LaunchWinEventProc(HWINEVENTHOOK /*hHook*/,
                                        DWORD          event,
                                        HWND           hwnd,
                                        LONG           idObject,
                                        LONG           idChild,
                                        DWORD          /*idEventThread*/,
                                        DWORD          /*dwmsEventTime*/)
{
    if (event != EVENT_OBJECT_SHOW)          return;
    if (idObject != OBJID_WINDOW)            return;
    if (idChild  != CHILDID_SELF)            return;
    if (!hwnd)                               return;
    g_launch.OnWindowShown(hwnd);
}

namespace
{
    int64_t NowUtcMs()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        return (int64_t)((t - 116444736000000000ULL) / 10000ULL);
    }

    std::string PresenceName()
    {
        return PresenceStateName((PresenceState)g_presenceState.load());
    }

    bool g_taskInstalled = false;

    // SEC-1: models load from the install directory. The per-user copy is accepted only when
    // WARP runs unelevated - an elevated process must never parse model files a standard user
    // can replace.
    std::wstring ModelsDir()
    {
        std::wstring exeModels = ExeDir() + L"\\models";
        DWORD attr = GetFileAttributesW(exeModels.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return exeModels;
        if (!IsElevated())
        {
            std::wstring user = g_dataDir + L"\\models";
            attr = GetFileAttributesW(user.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return user;
        }
        return std::wstring();
    }

    // Which document is a focus session about (gap analysis §5.3, APP-5, WEB-8)?
    //   browser   -> the page WebCapture last read for that window (canonical URL / doc id),
    //                accepted only if its title is the one the window shows;
    //   documents -> the file the same process recently opened whose name the title carries,
    //                else an explicit recent open (Recent items / Office MRU) with that name.
    bool ResolveDoc(HWND root, DWORD pid, const std::wstring& exePath, const std::wstring& title, FocusTracker::DocInfo& out)
    {
        WebCapture::PageInfo page;
        if (g_web.LookupPage(root, page))
        {
            if (page.isPrivate || page.excluded || !page.privacyKnown) return false;
            if (NowUtcMs()-page.observedMs>30000) { out.resolution="stale"; return false; }
            if (page.url.empty() && page.docId.empty()) return false;
            std::wstring pt = DocResolver::NormalizeTitle(page.title);
            if (!pt.empty() && title.find(pt) == std::wstring::npos && pt.find(title) == std::wstring::npos) return false;
            out.key = StoreUtil::WebEntityKey(page.url, page.docId, page.title);
            out.display = page.url;
            out.type = "url";
            out.resolution = "browser_window";
            out.instance = page.pageInstance;
            out.observedMs = page.observedMs; out.validUntilMs = page.observedMs+30000; out.confidence = 0.9;
            return !out.key.empty();
        }
        std::wstring exeLower = EntityKey::FromUtf8(EntityKey::Key(EntityKey::BaseName(exePath)));
        std::wstring docName = DocResolver::DocNameFromTitle(exeLower, title);
        if (docName.empty()) return false;
        std::wstring path;
        bool ambiguous = false;
        if (!DocResolver::MatchPath(docName, g_files.RecentPathsForPid(pid, 6 * 3600 * 1000, 16), path, &ambiguous))
        {
            if (ambiguous) { out.resolution = "ambiguous"; return false; }
            if (!DocResolver::MatchPath(docName, g_files.RecentExplicitOpens(24 * 3600 * 1000, 64), path, &ambiguous))
            {
                if (ambiguous) out.resolution = "ambiguous";
                return false;
            }
        }
        if (Settings::Instance().IsExcludedPath(path)) return false;
        out.key = EntityKey::FileKey(path);
        out.display = path;
        out.type = "file";
        out.resolution = "filename_candidate";
        out.observedMs = NowUtcMs(); out.validUntilMs = out.observedMs+3000; out.confidence = 0.5;
        return !out.key.empty();
    }

    // WEB-1 / PRIV-3: private windows, excluded apps / sites / titles never reach the store.
    FocusTracker::Redaction Redact(HWND root, DWORD /*pid*/, const std::wstring& exePath, const std::wstring& title)
    {
        if (Settings::Instance().IsExcludedApp(exePath)) return FocusTracker::Redaction::DropAll;
        if (g_web.IsPrivateWindow(root)) return FocusTracker::Redaction::DropContent;
        WebCapture::PageInfo page;
        const bool knownPage = g_web.LookupPage(root, page);
        if (knownPage && (page.excluded || !page.privacyKnown)) return FocusTracker::Redaction::DropContent;
        if (!knownPage && WebCapture::IsBrowserProcess(exePath)) return FocusTracker::Redaction::DropContent;
        if (Settings::Instance().IsExcludedTitle(title)) return FocusTracker::Redaction::DropContent;
        return FocusTracker::Redaction::None;
    }

    // Extra GetStatus members (RUN-5): per-source health, presence, models, runtime. Paths, error
    // texts and microphone / camera / media signals only for WARP's own CLI and tray (`full`).
    void WriteStatus(WarpJson::Writer& w, bool full)
    {
        WarpSettings cfg = Settings::Instance().Get();
        FileCapture::Stats f = g_files.GetStats();
        LaunchCapture::Stats l = g_launch.GetStats();
        WebCapture::Stats wb = g_web.GetStats();
        w.Key("capture").BeginObject();
        w.Key("files").BeginObject().KV("enabled", cfg.captureFiles).KV("etw_running", f.etwRunning).KV("usn_running", f.usnRunning)
         .KV("recent_running", f.recentRunning).KV("mru_running", f.mruRunning).KV("emitted", f.emitted).KV("filtered", f.filtered)
         .KV("queue_dropped", f.queueDropped).KV("etw_events_lost", f.etwEventsLost).KV("etw_buffers_lost", f.etwBuffersLost)
         .KV("last_event_ms", f.lastEventMs).KV("session", f.etwSessionName);
        if (full) w.KV("last_error", f.lastError);
        w.EndObject();
        w.Key("launches").BeginObject().KV("enabled", cfg.captureAppLaunch).KV("etw_running", l.etwRunning).KV("starts", l.starts)
         .KV("recorded", l.recorded).KV("with_window", l.withWindow).KV("no_window", l.noWindow).KV("helpers", l.helpers)
         .KV("system", l.system).KV("other_session", l.otherSession).KV("exits", l.exits).KV("events_lost", l.eventsLost)
         .KV("last_event_ms", l.lastEventMs);
        if (full) w.KV("last_error", l.lastError);
        w.EndObject();
        w.Key("web").BeginObject().KV("enabled", cfg.captureWeb).KV("running",wb.running).KV("uia_available",wb.uiaAvailable)
         .KV("pages", wb.pages).KV("private_dropped", wb.privateDropped)
         .KV("excluded_dropped", wb.excludedDropped).KV("uia_requests", wb.uiaRequests).KV("uia_timeouts", wb.uiaTimeouts)
         .KV("uia_failures", wb.uiaFailures).KV("last_event_ms", wb.lastEventMs).EndObject();
        w.KV("focus_enabled", cfg.captureFocus);
        w.EndObject();
        PresenceSnapshot ps = PresenceMonitor::Instance().GetSnapshot();
        w.Key("presence").BeginObject().KV("state", PresenceStateName(ps.state)).KV("since", ps.sinceUtc)
         .KV("ms_since_input", (int64_t)ps.msSinceInput).KV("fullscreen", ps.fullscreen).KV("locked", ps.sessionLocked)
         .KV("on_battery", ps.onBattery);
        if (full) w.KV("media_playing", ps.mediaPlaying).KV("mic_in_use", ps.micInUse).KV("camera_in_use", ps.cameraInUse);
        w.EndObject();
        w.Key("models").BeginObject().KV("embedding_loaded", g_contextInference.IsModelLoaded())
         .KV("llm_enabled", cfg.llmEnabled).KV("llm_available", g_llmSummarizer.IsAvailable())
         .KV("llm_resident", g_llmSummarizer.IsLoaded()).EndObject();
        if (full) w.KV("data_dir", g_dataDir);
        w.KV("elevated", IsElevated()).KV("headless", g_headless).KV("logon_task_installed", g_taskInstalled);
    }
}

// Worker thread: everything that does not need the UI thread (RUN-7).
void StartSubsystems()
{
    WarpSettings cfg = Settings::Instance().Get();
    g_taskInstalled = Installer::IsTaskInstalled();

    // ---- storage (single writer, entity + history sinks) ----
    g_store.AddSink(&g_entities);
    g_store.AddSink(&g_history);
    g_store.SetRetention(cfg.rawRetentionDays, cfg.entityRetentionDays);
    const std::wstring dbPath = GetDatabasePath();
    if (cfg.encryptDataDir)
    {
        DWORD attrs = GetFileAttributesW(dbPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_ENCRYPTED)) EncryptFileW(dbPath.c_str());
    }
    if (!g_store.Open(dbPath))
        WARP_LOG_ERROR("main", "activity store could not be opened; capture continues but nothing is persisted");
    else
        g_store.ScheduleMaintenance();          // retention + leftover deletes from a previous run

    // ---- presence (SIG-1/SIG-2: never pauses capture; tags it and closes focus sessions) ----
    PresenceMonitor& pm = PresenceMonitor::Instance();
    pm.Configure((DWORD)cfg.awayAfterSeconds * 1000);
    g_presenceSinceMs = NowUtcMs();
    g_presenceEvidencePaused = Settings::Instance().IsPaused(StoreUtil::NowUtc());
    pm.AddListener([](PresenceState oldState, PresenceState newState, const PresenceSnapshot& snap) {
        const int64_t now = NowUtcMs();
        CheckpointPresence(PresenceStateName(newState),true,PresenceStateName(oldState),Settings::Instance().IsPaused(now/1000));
        g_presenceState = (int)newState;
        g_focus.OnPresenceChanged(PresenceStateName(newState), snap.lastInputUtc * 1000);
    });
    if (cfg.capturePresence && pm.Start())
        g_presenceState = (int)pm.GetSnapshot().state;

    // ---- capture sources that run on their own threads ----
    g_files.SetCallback([](FileEventRecord&& r) {
        if (r.presence.empty()) r.presence = PresenceName();
        g_store.RecordFile(std::move(r));
    });
    g_files.SetCursorStore(
        [](const std::wstring& volume, std::string& blob) { return g_store.GetMeta("usn:" + EntityKey::Key(volume), blob); },
        [](const std::wstring& volume, const std::string& blob) { g_store.SetMeta("usn:" + EntityKey::Key(volume), blob); });
    g_files.SetCloudCallback([](BrowseEventRecord&& r) { g_store.RecordBrowse(std::move(r)); });
    if (cfg.captureFiles && !g_files.Start())
        WARP_LOG_ERROR("main", "file capture could not start (see earlier errors); the supervisor will keep retrying");

    g_launch.SetCallbacks(
        [](LaunchEventRecord&& r) { g_store.RecordLaunch(std::move(r)); },
        [](ProcessExitRecord&& r) { g_store.RecordProcessExit(std::move(r)); });
    if (cfg.captureAppLaunch || cfg.captureFiles || cfg.captureFocus || cfg.captureWeb) g_launch.Start();

    if (cfg.captureClipboardMetadata)
        g_clipboard.Start([](const ClipboardEvent& e) {
            if (Settings::Instance().IsPaused((int64_t)time(nullptr))) return;   // these monitors have no pause of their own
            if (Settings::Instance().IsExcludedApp(e.ownerExePath)) return;
            ClipboardRecord r;
            r.eventMs = e.utcMs;
            r.ownerExe = e.ownerExePath;
            for (const std::wstring& f : e.formats) r.formats += (r.formats.empty() ? L"" : L",") + f;
            g_store.RecordClipboard(std::move(r));
        });
    if (cfg.capturePrint &&
        !g_print.Start([](const PrintEvent& e) {
            if (Settings::Instance().IsPaused((int64_t)time(nullptr))) return;
            PrintRecord r;
            r.eventMs = e.utcMs;
            r.document = e.documentName;
            r.printer = e.printer;
            r.pages = e.pages;
            g_store.RecordPrint(std::move(r));
        }, true))
        WARP_LOG_WARN("main", "print capture unavailable: %S", g_print.Status().c_str());

    // ---- intelligence ----
    const std::wstring modelsDir = ModelsDir();
    g_contextInference.Init(modelsDir);                 // empty => deterministic only
    g_contextInference.SetEntityStore(&g_entities);
    g_contextInference.SetHistoryEngine(&g_history);
    g_contextInference.SetPresenceProvider([]() { return PresenceName(); });
    // PERF-1 / INTEL-6: the LLM refiner is opt-in (LlmEnabled); nothing is loaded otherwise.
    if (cfg.llmEnabled && g_llmSummarizer.Init(modelsDir))
        g_contextInference.SetLlmSummarizer(&g_llmSummarizer);
    WARP_LOG_INFO("main", "models: embedding=%d llm=%d (dir=%S)", g_contextInference.IsModelLoaded() ? 1 : 0,
                  g_llmSummarizer.IsAvailable() ? 1 : 0, modelsDir.c_str());

    // ---- consumer API ----
    ApiServer::Deps deps;
    deps.store = &g_store;
    deps.entities = &g_entities;
    deps.history = &g_history;
    deps.context = &g_contextInference;
    deps.statusProvider = [](WarpJson::Writer& w, bool full) { WriteStatus(w, full); };
    deps.setPaused = [](int64_t untilUtc) {
        bool ok = Settings::Instance().SetPausedUntil(untilUtc);
        if (g_hWnd) PostMessageW(g_hWnd, WM_TIMER, IDT_SUPERVISOR_TIMER, 0);     // apply on the UI thread
        return ok;
    };
    deps.productVersion = WARP_PRODUCT_VERSION;
    g_api.Start(deps);

    // ---- hooks must live on the UI thread ----
    if (g_hWnd) PostMessageW(g_hWnd, WM_APP_START_UI, 0, 0);
}

// UI thread: WinEvent hooks and the capture that depends on them.
void StartUiSubsystems()
{
    if (g_stopping.load() || g_subsystemsUp.load()) return;
    WarpSettings cfg = Settings::Instance().Get();
    ForegroundChangeBroker::Instance().Start();
    // A window switch is proof of a present user: let Away -> Active happen now, not at the next poll.
    ForegroundChangeBroker::Instance().Subscribe([](HWND, DWORD) { PresenceMonitor::Instance().NotifyUserActivity(); });
    g_hWinEventHook = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, LaunchWinEventProc,
                                      0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    g_web.SetCallback([](BrowseEventRecord&& r) {
        if (r.presence.empty()) r.presence = PresenceName();
        g_store.RecordBrowse(std::move(r));
    });
    g_web.SetEpisodeCallback([](EpisodeRecord&& r) { g_store.RecordPageEpisode(std::move(r)); });
    if (cfg.captureWeb) g_web.Start();

    g_focus.SetDocResolver(&ResolveDoc);
    g_focus.SetRedaction(&Redact);
    g_focus.SetCallback([](FocusEventRecord&& r) {
        if (r.presence.empty()) r.presence = PresenceName();
        g_store.RecordFocus(std::move(r));
    });
    if (cfg.captureFocus) g_focus.Start();

    g_contextInference.Start(&g_store, &g_focus);
    g_subsystemsUp = true;
    g_capturePausedApplied = false;
    ApplyPauseState();
    CheckpointCoverage();

    // RUN-5 supervisor thread: every 30 s restart dead ETW consumers with backoff, poll lost events,
    // reconcile sources with Settings, and retry a file capture that could not start at all.
    g_supervisorStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_supervisorStop)
    {
        g_supervisorThread = std::thread([]() {
            SetThreadDescription(GetCurrentThread(), L"WARP supervisor");
            int fileRetryTicks = 0;
            while (WaitForSingleObject(g_supervisorStop, 30 * 1000) == WAIT_TIMEOUT)
            {
                if (!g_subsystemsUp.load()) continue;
                // Registry / policy changes apply without a restart: exclusions and pause are read per
                // event, file sources are reconciled by Heartbeat, retention and log level here.
                Settings::Instance().Load();
                const WarpSettings live = Settings::Instance().Get();
                g_store.SetRetention(live.rawRetentionDays, live.entityRetentionDays);
                Log::SetMinLevel((Log::Level)(std::max)(0, (std::min)(3, live.logLevel)));
                g_files.Heartbeat();
                g_launch.Heartbeat();
                g_web.Heartbeat();
                CheckpointPresence("checkpoint",false,PresenceName(),Settings::Instance().IsPaused(StoreUtil::NowUtc()));
                CheckpointCoverage();
                // A running FileCapture restarts its own dead sources in Heartbeat; Start() is only retried
                // when it never started (or stopped) - e.g. capture enabled live, or every source failed.
                if (live.captureFiles && !g_files.IsRunning() && ++fileRetryTicks >= 10)
                {
                    fileRetryTicks = 0;                       // every ~5 minutes
                    if (g_files.Start()) WARP_LOG_INFO("main", "file capture started by the supervisor");
                }
            }
        });
    }
    WARP_LOG_INFO("main", "all subsystems started (startup took %lld s)", (long long)((int64_t)time(nullptr) - g_startedUtc));
    PostMessageW(g_hWnd, WM_APP_READY, 0, 0);
}

void OnInferenceButton(HWND hWnd, int id)
{
    if (id == IDB_INFER_TOP)
    {
        wchar_t buf[32] = {};
        GetWindowTextW(g_hEditInferTopN, buf, 32);
        int topN = _wtoi(buf);
        if (topN <= 0) topN = 50;
        if (topN > 1000) topN = 1000;

        bool wantFile = (SendMessageW(g_hChkInferFile, BM_GETCHECK, 0, 0) == BST_CHECKED);
        bool wantApp  = (SendMessageW(g_hChkInferApp,  BM_GETCHECK, 0, 0) == BST_CHECKED);
        bool wantUrl  = (SendMessageW(g_hChkInferUrl,  BM_GETCHECK, 0, 0) == BST_CHECKED);
        int checked = (wantFile ? 1 : 0) + (wantApp ? 1 : 0) + (wantUrl ? 1 : 0);
        std::string type;
        if (checked == 1) type = wantFile ? "file" : (wantApp ? "app" : "url");

        // RANK-4: ranked by decayed engagement (TopEntities) instead of scanning the change feed.
        std::string request = "{\"op\":\"TopEntities\",\"limit\":" + std::to_string(checked == 2 ? topN * 2 : topN) +
                              (type.empty() ? std::string() : ",\"type\":\"" + type + "\"") + "}";
        SetWindowTextW(g_hResponse, L"Querying inferences...");

        struct InferFilter { bool file; bool app; bool url; int topN; };
        InferFilter* pf = new InferFilter{ wantFile || checked == 0, wantApp || checked == 0, wantUrl || checked == 0, topN };
        std::string* req = new std::string(request);

        std::thread([hWnd, req, pf]() {
            std::string response = g_subsystemsUp.load()
                ? g_api.Handle(*req, ApiClient::InProcess())
                : std::string();
            delete req;

            std::string output;
            WarpJson::ParseResult pr = WarpJson::Parse(response);
            if (!pr.ok || !pr.value.get("ok").asBool(false))
            {
                output = response.empty() ? "WARP is still starting up - try again in a moment.\r\n" : response;
            }
            else
            {
                std::vector<const WarpJson::Value*> rows;
                for (const WarpJson::Value& v : pr.value.get("results").items())
                {
                    const std::string& t = v.get("entity_type").asString();
                    if ((t == "file" && pf->file) || (t == "app" && pf->app) || (t == "url" && pf->url))
                        rows.push_back(&v);
                    if ((int)rows.size() >= pf->topN) break;
                }
                output += "Top " + std::to_string(rows.size()) + " entities by engagement "
                          "(score = percentile within type, decayed at query time)\r\n";
                output += std::string(78, '-') + "\r\n";
                char line[512];
                auto stamp = [](int64_t t) -> std::string {
                    if (t <= 0) return std::string();
                    time_t tt = (time_t)t;
                    struct tm tmBuf;
                    localtime_s(&tmBuf, &tt);
                    char ts[64];
                    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmBuf);
                    return ts;
                };
                for (size_t i = 0; i < rows.size(); ++i)
                {
                    const WarpJson::Value& r = *rows[i];
                    sprintf_s(line, sizeof(line), "%3d. [%s] score=%.2f  opens 7d=%lld  active 7d=%lld min  days=%lld\r\n",
                              (int)(i + 1), r.get("entity_type").asString().c_str(), r.get("score").asDouble(),
                              (long long)r.get("open_count_7d").asInt64(), (long long)(r.get("active_secs_7d").asInt64() / 60),
                              (long long)r.get("distinct_days").asInt64());
                    output += line;
                    std::string display = r.get("display_name").asString();
                    output += "     " + (display.empty() ? r.get("entity_key").asString() : display) + "\r\n";
                    std::string reasons;
                    for (const std::string& s : r.get("reasons").asStringArray()) reasons += (reasons.empty() ? "" : " | ") + s;
                    if (!reasons.empty()) output += "     " + reasons + "\r\n";
                    std::string lo = stamp(r.get("last_open_ts").asInt64()), le = stamp(r.get("last_edit_ts").asInt64());
                    if (!lo.empty() || !le.empty())
                        output += "     " + (lo.empty() ? std::string() : "last_open=" + lo + "  ") + (le.empty() ? std::string() : "last_edit=" + le) + "\r\n";
                    output += "\r\n";
                }
                if (rows.empty()) output += "(No entities match the selected types yet.)\r\n";
            }
            delete pf;

            int wlen = MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, nullptr, 0);
            wchar_t* wResult = new wchar_t[wlen];
            MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, wResult, wlen);
            PostMessageW(hWnd, WM_QUERY_RESULT, 0, (LPARAM)wResult);
        }).detach();
    }
    else if (id == IDB_INFER_LOOKUP)
    {
        wchar_t pathBuf[1024] = {};
        GetWindowTextW(g_hEditInferPath, pathBuf, 1024);
        if (pathBuf[0] == L'\0')
        {
            SetWindowTextW(g_hResponse, L"Enter a file path, exe path, or URL to look up.");
            return;
        }
        // Scored lookup with reasons and the current-thread boost (API v2), built with the JSON writer.
        std::string request;
        WarpJson::Writer w(request);
        w.BeginObject().KV("op", "ScoreEntities").Key("keys").BeginArray().String(std::wstring(pathBuf)).EndArray()
         .KV("context", "current").EndObject();
        SendApiQuery(hWnd, request);
    }
}

void StopSubsystems()
{
    if (g_stopping.exchange(true)) return;
    if (g_startThread.joinable()) g_startThread.join();     // startup must finish before teardown
    g_subsystemsUp = false;
    WARP_LOG_INFO("main", "stopping subsystems");
    if (g_supervisorStop) SetEvent(g_supervisorStop);
    if (g_supervisorThread.joinable()) g_supervisorThread.join();   // no heartbeat races the shutdown
    if (g_supervisorStop) { CloseHandle(g_supervisorStop); g_supervisorStop = nullptr; }

    g_contextInference.Stop();
    g_llmSummarizer.Stop();
    g_api.Stop();
    g_focus.Stop();                                          // emits the open focus session
    g_web.Stop();
    if (g_hWinEventHook)
    {
        UnhookWinEvent(g_hWinEventHook);
        g_hWinEventHook = nullptr;
    }
    ForegroundChangeBroker::Instance().Stop();
    g_launch.Stop();
    g_files.Stop();
    g_clipboard.Stop();
    g_print.Stop();
    PresenceMonitor::Instance().Stop();
    CheckpointPresence("shutdown",true,PresenceName(),true);
    CheckpointCoverage();
    g_store.Close();                                         // drains the writer queue (bounded)
}