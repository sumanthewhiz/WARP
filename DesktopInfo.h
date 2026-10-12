#pragma once

// DesktopInfo
// -----------------------------------------------------------------------------
// Stateless window / desktop helpers. Safe to call from any thread: helpers
// that need COM use the caller's apartment if COM is initialized, otherwise
// initialize (MTA) and uninitialize it locally around the call. All return an
// empty string / false on failure and never throw (except std::bad_alloc).
// -----------------------------------------------------------------------------

#include <windows.h>
#include <string>

namespace DesktopInfo
{
    std::wstring GetWindowDesktopId(HWND hwnd);   // IVirtualDesktopManager::GetWindowDesktopId -> "{GUID}" string; empty on failure
    bool IsWindowCloaked(HWND hwnd);              // DwmGetWindowAttribute(DWMWA_CLOAKED)
    bool IsFullscreenOrPresenting();              // SHQueryUserNotificationState: busy / D3D full screen / presentation
    std::wstring GetMonitorDeviceName(HWND hwnd); // MonitorFromWindow + GetMonitorInfoW(MONITORINFOEXW).szDevice
    std::wstring GetAumidForWindow(HWND hwnd);    // SHGetPropertyStoreForWindow -> PKEY_AppUserModel_ID; else GetAumidForProcess(window pid)
    std::wstring GetAumidForProcess(DWORD pid);   // OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) + GetApplicationUserModelId; empty if none
    // A window a user would consider "an app window": IsWindowVisible, not cloaked, no owner (GetWindow GW_OWNER == null) or has WS_EX_APPWINDOW,
    // not WS_EX_TOOLWINDOW (unless WS_EX_APPWINDOW), not a child (GetAncestor(hwnd, GA_ROOT)==hwnd), non-zero size.
    bool IsUserFacingAppWindow(HWND hwnd);
}
