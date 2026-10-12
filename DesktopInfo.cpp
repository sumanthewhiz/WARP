#include "framework.h"
#include "DesktopInfo.h"

#include <objbase.h>
#include <shobjidl.h>
#include <propidl.h>
#include <propsys.h>
#include <dwmapi.h>
#include <appmodel.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    // CLSID_VirtualDesktopManager {AA509086-5CA9-4C25-8F95-589D3C07B48A}.
    const CLSID kClsidVirtualDesktopManager =
        { 0xAA509086, 0x5CA9, 0x4C25, { 0x8F, 0x95, 0x58, 0x9D, 0x3C, 0x07, 0xB4, 0x8A } };

    // PKEY_AppUserModel_ID {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, 5. Defined locally so the
    // link does not depend on which library carries the propkey.h / uuid.lib symbols.
    const PROPERTYKEY kPkeyAppUserModelId =
        { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };

    // Uses the caller's COM apartment when there is one; otherwise joins the MTA for the call.
    class ScopedCom
    {
    public:
        ScopedCom() : m_hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
        ~ScopedCom()
        {
            if (SUCCEEDED(m_hr))
                CoUninitialize();     // balances S_OK and S_FALSE
        }
        ScopedCom(const ScopedCom&) = delete;
        ScopedCom& operator=(const ScopedCom&) = delete;

        // RPC_E_CHANGED_MODE: the thread is already an STA, which is fine to use.
        bool Usable() const { return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE; }

    private:
        HRESULT m_hr;
    };

    class ProcessHandle
    {
    public:
        explicit ProcessHandle(HANDLE h) : m_h(h) {}
        ~ProcessHandle()
        {
            if (m_h)
                CloseHandle(m_h);
        }
        ProcessHandle(const ProcessHandle&) = delete;
        ProcessHandle& operator=(const ProcessHandle&) = delete;

        HANDLE Get() const { return m_h; }

    private:
        HANDLE m_h;
    };

    struct PropVariantHolder
    {
        PROPVARIANT value;
        PropVariantHolder() { PropVariantInit(&value); }
        ~PropVariantHolder() { PropVariantClear(&value); }
        PropVariantHolder(const PropVariantHolder&) = delete;
        PropVariantHolder& operator=(const PropVariantHolder&) = delete;
    };
}

namespace DesktopInfo
{
    std::wstring GetWindowDesktopId(HWND hwnd)
    {
        if (!hwnd || !IsWindow(hwnd))
            return std::wstring();
        ScopedCom com;
        if (!com.Usable())
            return std::wstring();

        ComPtr<IVirtualDesktopManager> manager;     // released before ScopedCom uninitializes
        if (FAILED(CoCreateInstance(kClsidVirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&manager))) ||
            !manager)
            return std::wstring();

        GUID desktopId = {};
        if (FAILED(manager->GetWindowDesktopId(hwnd, &desktopId)))
            return std::wstring();
        const GUID none = {};
        if (IsEqualGUID(desktopId, none))
            return std::wstring();

        wchar_t text[64] = {};
        if (StringFromGUID2(desktopId, text, static_cast<int>(ARRAYSIZE(text))) == 0)
            return std::wstring();
        return std::wstring(text);
    }

    bool IsWindowCloaked(HWND hwnd)
    {
        if (!hwnd)
            return false;
        DWORD cloaked = 0;
        return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, static_cast<DWORD>(sizeof(cloaked)))) &&
               cloaked != 0;
    }

    bool IsFullscreenOrPresenting()
    {
        ScopedCom com;      // the shell may consult COM-based state for this query
        QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
        if (FAILED(SHQueryUserNotificationState(&state)))
            return false;
        return state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE;
    }

    std::wstring GetMonitorDeviceName(HWND hwnd)
    {
        if (!hwnd)
            return std::wstring();
        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (!monitor)
            return std::wstring();
        MONITORINFOEXW info = {};
        info.cbSize = static_cast<DWORD>(sizeof(info));
        if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info)))
            return std::wstring();
        return std::wstring(info.szDevice);
    }

    std::wstring GetAumidForWindow(HWND hwnd)
    {
        if (!hwnd || !IsWindow(hwnd))
            return std::wstring();

        std::wstring aumid;
        {
            ScopedCom com;
            if (com.Usable())
            {
                ComPtr<IPropertyStore> store;
                if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) && store)
                {
                    PropVariantHolder pv;
                    if (SUCCEEDED(store->GetValue(kPkeyAppUserModelId, &pv.value)) &&
                        pv.value.vt == VT_LPWSTR && pv.value.pwszVal != nullptr)
                    {
                        aumid = pv.value.pwszVal;
                    }
                }
            }
        }
        if (!aumid.empty())
            return aumid;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        return GetAumidForProcess(pid);
    }

    std::wstring GetAumidForProcess(DWORD pid)
    {
        if (pid == 0)
            return std::wstring();
        ProcessHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!process.Get())
            return std::wstring();

        UINT32 length = 0;
        LONG rc = GetApplicationUserModelId(process.Get(), &length, nullptr);
        if (rc != ERROR_INSUFFICIENT_BUFFER || length == 0 || length > 32768)
            return std::wstring();      // e.g. APPMODEL_ERROR_NO_APPLICATION: no package identity

        std::vector<wchar_t> buffer(length, L'\0');
        rc = GetApplicationUserModelId(process.Get(), &length, buffer.data());
        if (rc != ERROR_SUCCESS)
            return std::wstring();
        buffer.back() = L'\0';
        return std::wstring(buffer.data());
    }

    bool IsUserFacingAppWindow(HWND hwnd)
    {
        if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd))
            return false;
        if (GetAncestor(hwnd, GA_ROOT) != hwnd)
            return false;                           // child window

        const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        const bool appWindow = (exStyle & WS_EX_APPWINDOW) != 0;
        if ((exStyle & WS_EX_TOOLWINDOW) != 0 && !appWindow)
            return false;
        if (GetWindow(hwnd, GW_OWNER) != nullptr && !appWindow)
            return false;
        if (IsWindowCloaked(hwnd))
            return false;                           // other virtual desktop, suspended UWP frame, ...

        RECT rc = {};
        return GetWindowRect(hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top;
    }
}
