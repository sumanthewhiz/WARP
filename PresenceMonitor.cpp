#include "framework.h"
#include "PresenceMonitor.h"
#include "EventContext.h"

#include <objbase.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <wtsapi32.h>
#include <wrl/client.h>

#include <cstring>
#include <cwchar>
#include <string>
#include <system_error>
#include <utility>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    // Local copies of the winnt.h power-setting GUIDs: their DEFINE_GUID declarations are extern
    // without INITGUID, and not every SDK import library defines them (avoids LNK2001).
    const GUID kGuidConsoleDisplayState = { 0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 } };
    const GUID kGuidAcDcPowerSource     = { 0x5d3e9a59, 0xe9d5, 0x4b00, { 0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48 } };

    const wchar_t   kWindowClass[]        = L"WarpPresenceMonitorWnd";
    const DWORD     kDefaultAwayAfterMs   = 300000;
    const DWORD     kPollIntervalMs       = 2000;    // input poll / wait timeout
    const ULONGLONG kPassiveRefreshMs     = 5000;    // media / mic / camera / fullscreen / power
    const ULONGLONG kCandidateFreshMs     = 1000;    // passive data younger than this is trusted for a transition
    const ULONGLONG kWakeBoundaryMs       = 5000;
    const ULONGLONG kMissedResumeGraceMs  = 2000;    // input this long after PBT_APMSUSPEND => we are awake
    const ULONGLONG kWtsRetryIntervalMs   = 10000;
    const DWORD     kStartupWaitMs        = 5000;
    const float     kAudiblePeak          = 0.001f;
    const DWORD     kMaxConsentKeys       = 4096;

    int64_t NowUnixMs()
    {
        FILETIME ft = {};
        GetSystemTimeAsFileTime(&ft);
        const ULONGLONG ticks = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        // 100-ns ticks between 1601-01-01 and 1970-01-01.
        return static_cast<int64_t>((ticks - 116444736000000000ULL) / 10000ULL);
    }

    // ConsentStore app key: in use while LastUsedTimeStart is set and LastUsedTimeStop is 0.
    bool ConsentAppKeyInUse(HKEY parent, const wchar_t* subKey)
    {
        ULONGLONG start = 0;
        ULONGLONG stop = 0;
        DWORD cb = static_cast<DWORD>(sizeof(start));
        if (RegGetValueW(parent, subKey, L"LastUsedTimeStart", RRF_RT_REG_QWORD, nullptr, &start, &cb) != ERROR_SUCCESS)
            return false;
        cb = static_cast<DWORD>(sizeof(stop));
        if (RegGetValueW(parent, subKey, L"LastUsedTimeStop", RRF_RT_REG_QWORD, nullptr, &stop, &cb) != ERROR_SUCCESS)
            return false;
        return start != 0 && stop == 0;
    }

    bool ConsentKeysInUse(HKEY key, bool descendIntoNonPackaged)
    {
        wchar_t name[256];
        for (DWORD i = 0; i < kMaxConsentKeys; ++i)
        {
            DWORD len = static_cast<DWORD>(ARRAYSIZE(name));
            const LSTATUS rc = RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr);
            if (rc == ERROR_MORE_DATA)
                continue;
            if (rc != ERROR_SUCCESS)
                break;      // ERROR_NO_MORE_ITEMS, or a hard failure (never spin on it)

            if (descendIntoNonPackaged && _wcsicmp(name, L"NonPackaged") == 0)
            {
                HKEY nonPackaged = nullptr;
                if (RegOpenKeyExW(key, name, 0, KEY_READ, &nonPackaged) == ERROR_SUCCESS)
                {
                    const bool inUse = ConsentKeysInUse(nonPackaged, false);
                    RegCloseKey(nonPackaged);
                    if (inUse)
                        return true;
                }
                continue;
            }
            if (ConsentAppKeyInUse(key, name))
                return true;
        }
        return false;
    }

    // capability: L"microphone" or L"webcam".
    bool ConsentStoreInUse(const wchar_t* capability)
    {
        std::wstring path = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";
        path += capability;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
            return false;
        const bool inUse = ConsentKeysInUse(key, true);
        RegCloseKey(key);
        return inUse;
    }

    bool QueryFullscreen()
    {
        QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
        if (FAILED(SHQueryUserNotificationState(&state)))
            return false;
        return state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE;
    }

    void QueryPowerStatus(bool& onBattery, bool& batterySaver)
    {
        SYSTEM_POWER_STATUS sps = {};
        if (GetSystemPowerStatus(&sps))
        {
            onBattery    = (sps.ACLineStatus == 0);
            batterySaver = (sps.SystemStatusFlag == 1);
        }
    }

    // Reads the current session's lock / connect state. Returns false if unavailable.
    bool QuerySessionState(bool& locked, bool& disconnected)
    {
        LPWSTR buffer = nullptr;
        DWORD bytes = 0;
        if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx,
                                         &buffer, &bytes) || !buffer)
            return false;

        bool ok = false;
        if (bytes >= sizeof(WTSINFOEXW))
        {
            const WTSINFOEXW* info = reinterpret_cast<const WTSINFOEXW*>(buffer);
            if (info->Level == 1)
            {
                locked       = (info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK);
                disconnected = (info->Data.WTSInfoExLevel1.SessionState == WTSDisconnected);
                ok = true;
            }
        }
        WTSFreeMemory(buffer);
        return ok;
    }

    // True if the default (flow, role) endpoint has an active session (render: also audible).
    // Sets serviceLost when the enumerator should be recreated (e.g. AudioSrv restarted).
    bool EndpointHasActiveSession(IMMDeviceEnumerator* enumerator, EDataFlow flow, ERole role,
                                  bool requireAudible, bool& serviceLost)
    {
        ComPtr<IMMDevice> device;
        HRESULT hr = enumerator->GetDefaultAudioEndpoint(flow, role, &device);
        if (FAILED(hr) || !device)
        {
            if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))   // not found == no such device
                serviceLost = true;
            return false;
        }

        ComPtr<IAudioSessionManager2> manager;
        hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(manager.GetAddressOf()));
        if (FAILED(hr) || !manager)
            return false;

        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(&sessions)) || !sessions)
            return false;

        int count = 0;
        if (FAILED(sessions->GetCount(&count)))
            return false;

        for (int i = 0; i < count; ++i)
        {
            ComPtr<IAudioSessionControl> control;
            if (FAILED(sessions->GetSession(i, &control)) || !control)
                continue;

            AudioSessionState state = AudioSessionStateInactive;
            if (FAILED(control->GetState(&state)) || state != AudioSessionStateActive)
                continue;

            // Notification dings are not media.
            ComPtr<IAudioSessionControl2> control2;
            if (SUCCEEDED(control.As(&control2)) && control2->IsSystemSoundsSession() == S_OK)
                continue;

            if (!requireAudible)
                return true;

            ComPtr<IAudioMeterInformation> meter;
            float peak = 0.0f;
            if (SUCCEEDED(control.As(&meter)) && SUCCEEDED(meter->GetPeakValue(&peak)) && peak > kAudiblePeak)
                return true;
        }
        return false;
    }
}

const char* PresenceStateName(PresenceState s)
{
    switch (s)
    {
    case PresenceState::Active:  return "active";
    case PresenceState::Passive: return "passive";
    case PresenceState::Away:    return "away";
    case PresenceState::Locked:  return "locked";
    case PresenceState::Asleep:  return "asleep";
    }
    return "unknown";
}

// -----------------------------------------------------------------------------
// Worker: everything owned by the monitor thread (window, registrations, COM
// objects, raw signals). Only the monitor thread touches it.
// -----------------------------------------------------------------------------
struct PresenceMonitor::Worker
{
    Worker(PresenceMonitor& owner, bool comReady) : m_owner(owner), m_comReady(comReady) {}
    ~Worker() { Shutdown(); }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool CreateMessageWindow();
    void Initialize();
    void Run();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void OnSessionChange(WPARAM code);
    void OnPowerBroadcast(WPARAM code, LPARAM lParam);
    void OnResume();
    void TryRegisterSessionNotifications();
    void Shutdown();
    bool PumpMessages();
    void Poll();
    void Evaluate(bool forcePassiveRefresh);
    void EvaluateOnce(bool forcePassiveRefresh);
    void ReadInput();
    void RefreshPassive();
    PresenceState Classify(ULONGLONG nowTick, DWORD awayAfterMs) const;
    void Publish(PresenceState next);
    void Dispatch(PresenceState oldState, PresenceState newState, const PresenceSnapshot& snap);

    PresenceMonitor&            m_owner;
    const bool                  m_comReady;
    HWND                        m_hwnd = nullptr;
    HPOWERNOTIFY                m_suspendNotify = nullptr;
    HPOWERNOTIFY                m_displayNotify = nullptr;
    HPOWERNOTIFY                m_powerSourceNotify = nullptr;
    bool                        m_wtsRegistered = false;
    ULONGLONG                   m_nextWtsRetryTick = 0;
    ComPtr<IMMDeviceEnumerator> m_audio;

    // Discrete signals (from notifications).
    bool      m_asleep = false;
    ULONGLONG m_suspendTick = 0;
    bool      m_locked = false;
    bool      m_disconnected = false;
    bool      m_displayOff = false;
    ULONGLONG m_displayOffSinceTick = 0;

    // Polled signals.
    bool      m_haveInput = false;
    DWORD     m_lastInputTick = 0;
    DWORD     m_msSinceInput = 0;
    bool      m_havePassive = false;
    ULONGLONG m_lastPassiveTick = 0;
    bool      m_mediaPlaying = false;
    bool      m_micInUse = false;
    bool      m_cameraInUse = false;
    bool      m_fullscreen = false;
    bool      m_onBattery = false;
    bool      m_batterySaver = false;

    // Published state.
    PresenceState m_state = PresenceState::Active;
    bool          m_published = false;
    bool          m_dispatching = false;
    bool          m_reevaluate = false;
};

bool PresenceMonitor::Worker::CreateMessageWindow()
{
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize        = static_cast<UINT>(sizeof(wc));
    wc.lpfnWndProc   = &Worker::WndProc;
    wc.hInstance     = instance;
    wc.lpszClassName = kWindowClass;
    // Left registered for the process lifetime so a later Start() can reuse it.
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    m_hwnd = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    return m_hwnd != nullptr;
}

void PresenceMonitor::Worker::Initialize()
{
    // Delivered straight to this (message-only) window; broadcasts would not reach it.
    m_suspendNotify     = RegisterSuspendResumeNotification(m_hwnd, DEVICE_NOTIFY_WINDOW_HANDLE);
    m_displayNotify     = RegisterPowerSettingNotification(m_hwnd, &kGuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    m_powerSourceNotify = RegisterPowerSettingNotification(m_hwnd, &kGuidAcDcPowerSource, DEVICE_NOTIFY_WINDOW_HANDLE);
    TryRegisterSessionNotifications();

    bool locked = false;
    bool disconnected = false;
    if (QuerySessionState(locked, disconnected))
    {
        m_locked = locked;
        m_disconnected = disconnected;
    }
    Evaluate(true);     // initial state is published without notifying listeners
}

void PresenceMonitor::Worker::TryRegisterSessionNotifications()
{
    if (m_wtsRegistered || !m_hwnd)
        return;
    // Fails with RPC_S_INVALID_BINDING while Terminal Services is still starting; Poll() retries.
    m_wtsRegistered = WTSRegisterSessionNotification(m_hwnd, NOTIFY_FOR_THIS_SESSION) != FALSE;
    m_nextWtsRetryTick = GetTickCount64() + kWtsRetryIntervalMs;
}

void PresenceMonitor::Worker::Shutdown()
{
    if (m_wtsRegistered && m_hwnd)
        WTSUnRegisterSessionNotification(m_hwnd);
    m_wtsRegistered = false;
    if (m_suspendNotify)
    {
        UnregisterSuspendResumeNotification(m_suspendNotify);
        m_suspendNotify = nullptr;
    }
    if (m_displayNotify)
    {
        UnregisterPowerSettingNotification(m_displayNotify);
        m_displayNotify = nullptr;
    }
    if (m_powerSourceNotify)
    {
        UnregisterPowerSettingNotification(m_powerSourceNotify);
        m_powerSourceNotify = nullptr;
    }
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_audio.Reset();    // must precede the thread's CoUninitialize
}

LRESULT CALLBACK PresenceMonitor::Worker::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    Worker* self = reinterpret_cast<Worker*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCDESTROY)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    if (self)
    {
        try
        {
            return self->HandleMessage(hwnd, msg, wParam, lParam);
        }
        catch (...)
        {
            // Never let a C++ exception unwind through user32.
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT PresenceMonitor::Worker::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_WTSSESSION_CHANGE:
        OnSessionChange(wParam);
        return 0;
    case WM_POWERBROADCAST:
        OnPowerBroadcast(wParam, lParam);
        return TRUE;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void PresenceMonitor::Worker::OnSessionChange(WPARAM code)
{
    switch (code)
    {
    case WTS_SESSION_LOCK:
        m_locked = true;
        break;
    case WTS_CONSOLE_DISCONNECT:
    case WTS_REMOTE_DISCONNECT:
        m_disconnected = true;
        break;
    case WTS_SESSION_UNLOCK:
        m_locked = false;
        m_disconnected = false;
        break;
    case WTS_CONSOLE_CONNECT:
    case WTS_REMOTE_CONNECT:
    {
        // Reconnecting does not imply unlocked (fast user switching lands on the lock screen).
        m_disconnected = false;
        bool locked = m_locked;
        bool disconnected = false;
        if (QuerySessionState(locked, disconnected))
            m_locked = locked;
        break;
    }
    default:
        return;
    }
    Evaluate(false);
}

void PresenceMonitor::Worker::OnPowerBroadcast(WPARAM code, LPARAM lParam)
{
    switch (code)
    {
    case PBT_APMSUSPEND:
        // Evaluated synchronously so listeners see Asleep before the machine actually sleeps.
        m_asleep = true;
        m_suspendTick = GetTickCount64();
        Evaluate(false);
        break;
    case PBT_APMRESUMEAUTOMATIC:
    case PBT_APMRESUMESUSPEND:
        OnResume();
        break;
    case PBT_APMPOWERSTATUSCHANGE:
        QueryPowerStatus(m_onBattery, m_batterySaver);
        Evaluate(false);
        break;
    case PBT_POWERSETTINGCHANGE:
    {
        const POWERBROADCAST_SETTING* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
        if (!setting || setting->DataLength < sizeof(DWORD))
            break;
        DWORD value = 0;
        memcpy(&value, setting->Data, sizeof(value));
        if (IsEqualGUID(setting->PowerSetting, kGuidConsoleDisplayState))
        {
            const bool off = (value == 0);      // 0 = off, 1 = on, 2 = dimmed
            if (off && !m_displayOff)
                m_displayOffSinceTick = GetTickCount64();
            m_displayOff = off;
            Evaluate(false);
        }
        else if (IsEqualGUID(setting->PowerSetting, kGuidAcDcPowerSource))
        {
            QueryPowerStatus(m_onBattery, m_batterySaver);
            Evaluate(false);
        }
        break;
    }
    default:
        break;
    }
}

void PresenceMonitor::Worker::OnResume()
{
    // Publish the boundary first so the post-wake housekeeping burst is attenuated.
    EventContextUtil::SetWakeBoundary(GetTickCount64() + kWakeBoundaryMs);
    m_asleep = false;
    Evaluate(true);
}

bool PresenceMonitor::Worker::PumpMessages()
{
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
            return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

void PresenceMonitor::Worker::Run()
{
    HANDLE handles[2] = { m_owner.m_stopEvent, m_owner.m_wakeEvent };
    ULONGLONG nextPoll = GetTickCount64() + kPollIntervalMs;
    for (;;)
    {
        ULONGLONG now = GetTickCount64();
        const DWORD timeout = (now >= nextPoll) ? 0 : static_cast<DWORD>(nextPoll - now);
        const DWORD rc = MsgWaitForMultipleObjectsEx(2, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (rc == WAIT_OBJECT_0)
            return;                                     // Stop()
        if (rc == WAIT_OBJECT_0 + 2)
        {
            if (!PumpMessages())
                return;                                 // WM_QUIT posted to this thread
        }
        else if (rc == WAIT_FAILED)
        {
            // Not expected; back off instead of spinning while still honouring Stop().
            if (WaitForSingleObject(m_owner.m_stopEvent, kPollIntervalMs) == WAIT_OBJECT_0)
                return;
        }
        if (WaitForSingleObject(m_owner.m_stopEvent, 0) == WAIT_OBJECT_0)
            return;                                     // Stop() wins over pending work

        now = GetTickCount64();
        if (now >= nextPoll)
        {
            nextPoll = now + kPollIntervalMs;
            Poll();
        }
        else if (rc == WAIT_OBJECT_0 + 1)
        {
            Evaluate(false);                            // NotifyUserActivity() / Configure()
        }
    }
}

void PresenceMonitor::Worker::Poll()
{
    if (!m_wtsRegistered)
    {
        // No lock/unlock notifications yet: poll the session state instead.
        bool locked = m_locked;
        bool disconnected = m_disconnected;
        if (QuerySessionState(locked, disconnected))
        {
            m_locked = locked;
            m_disconnected = disconnected;
        }
        if (GetTickCount64() >= m_nextWtsRetryTick)
            TryRegisterSessionNotifications();
    }
    Evaluate(false);
}

void PresenceMonitor::Worker::Evaluate(bool forcePassiveRefresh)
{
    if (m_dispatching)
    {
        // Re-entered from a listener that pumped messages: finish that dispatch first.
        m_reevaluate = true;
        return;
    }
    do
    {
        m_reevaluate = false;
        EvaluateOnce(forcePassiveRefresh);
        forcePassiveRefresh = false;
    } while (m_reevaluate);
}

void PresenceMonitor::Worker::ReadInput()
{
    LASTINPUTINFO lii = {};
    lii.cbSize = static_cast<UINT>(sizeof(lii));
    if (GetLastInputInfo(&lii))
    {
        m_lastInputTick = lii.dwTime;
        m_haveInput = true;
    }
    if (!m_haveInput)
    {
        m_msSinceInput = 0;     // unknown: assume present rather than away
        return;
    }
    // Clock read after the input timestamp; unsigned 32-bit arithmetic handles the 49.7-day wrap.
    const DWORD elapsed = GetTickCount() - m_lastInputTick;
    m_msSinceInput = (elapsed > 0x7FFFFFFFu) ? 0 : elapsed;
}

void PresenceMonitor::Worker::RefreshPassive()
{
    m_cameraInUse = ConsentStoreInUse(L"webcam");
    bool mic      = ConsentStoreInUse(L"microphone");
    m_fullscreen  = QueryFullscreen();
    QueryPowerStatus(m_onBattery, m_batterySaver);

    bool media = false;
    if (m_comReady)
    {
        if (!m_audio)
        {
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&m_audio))))
                m_audio.Reset();
        }
        if (m_audio)
        {
            // Default endpoints are re-acquired on every refresh, so device switches are picked up.
            bool serviceLost = false;
            media = EndpointHasActiveSession(m_audio.Get(), eRender, eConsole, true, serviceLost);
            if (!mic)
            {
                mic = EndpointHasActiveSession(m_audio.Get(), eCapture, eConsole, false, serviceLost) ||
                      EndpointHasActiveSession(m_audio.Get(), eCapture, eCommunications, false, serviceLost);
            }
            if (serviceLost)
                m_audio.Reset();    // recreated on the next refresh
        }
    }
    m_mediaPlaying    = media;
    m_micInUse        = mic;
    m_havePassive     = true;
    m_lastPassiveTick = GetTickCount64();
}

PresenceState PresenceMonitor::Worker::Classify(ULONGLONG nowTick, DWORD awayAfterMs) const
{
    if (m_asleep)
        return PresenceState::Asleep;
    if (m_locked || m_disconnected)
        return PresenceState::Locked;
    if (m_msSinceInput < awayAfterMs)
        return PresenceState::Active;
    // A display that has been off for longer than the away threshold means nobody is watching.
    const bool displayLongOff = m_displayOff && (nowTick - m_displayOffSinceTick >= awayAfterMs);
    if (!displayLongOff && (m_mediaPlaying || m_micInUse || m_cameraInUse || m_fullscreen))
        return PresenceState::Passive;
    return PresenceState::Away;
}

void PresenceMonitor::Worker::EvaluateOnce(bool forcePassiveRefresh)
{
    ReadInput();
    const ULONGLONG now = GetTickCount64();

    // Safety net: input well after PBT_APMSUSPEND means we resumed without being told.
    if (m_asleep && m_haveInput && now > m_msSinceInput)
    {
        const ULONGLONG inputTick = now - m_msSinceInput;
        if (inputTick > m_suspendTick + kMissedResumeGraceMs)
        {
            EventContextUtil::SetWakeBoundary(now + kWakeBoundaryMs);
            m_asleep = false;
            forcePassiveRefresh = true;
        }
    }

    if (!m_asleep && (forcePassiveRefresh || !m_havePassive || now - m_lastPassiveTick >= kPassiveRefreshMs))
        RefreshPassive();

    DWORD awayAfter = m_owner.m_awayAfterMs.load();
    if (awayAfter == 0)
        awayAfter = kDefaultAwayAfterMs;

    PresenceState next = Classify(GetTickCount64(), awayAfter);

    // Passive vs Away hinges on the passive signals: make sure they are fresh before committing.
    if (m_published && next != m_state &&
        (next == PresenceState::Passive || next == PresenceState::Away) &&
        GetTickCount64() - m_lastPassiveTick >= kCandidateFreshMs)
    {
        RefreshPassive();
        next = Classify(GetTickCount64(), awayAfter);
    }
    Publish(next);
}

void PresenceMonitor::Worker::Publish(PresenceState next)
{
    const int64_t nowMs = NowUnixMs();
    const PresenceState oldState = m_state;
    const bool changed = m_published && next != oldState;
    PresenceSnapshot snap;
    {
        std::lock_guard<std::mutex> lock(m_owner.m_snapMutex);
        PresenceSnapshot& s = m_owner.m_snap;
        if (!m_published || changed)
            s.sinceUtc = nowMs / 1000;
        s.state         = next;
        s.msSinceInput  = m_msSinceInput;
        s.lastInputUtc  = (nowMs - static_cast<int64_t>(m_msSinceInput)) / 1000;
        s.mediaPlaying  = m_mediaPlaying;
        s.micInUse      = m_micInUse;
        s.cameraInUse   = m_cameraInUse;
        s.fullscreen    = m_fullscreen;
        s.sessionLocked = m_locked || m_disconnected;
        s.onBattery     = m_onBattery;
        s.batterySaver  = m_batterySaver;
        snap = s;
    }
    m_state = next;
    m_published = true;
    if (changed)
        Dispatch(oldState, next, snap);
}

void PresenceMonitor::Worker::Dispatch(PresenceState oldState, PresenceState newState, const PresenceSnapshot& snap)
{
    std::vector<Callback> listeners;
    try
    {
        std::lock_guard<std::mutex> lock(m_owner.m_listenerMutex);
        listeners = m_owner.m_listeners;    // invoke outside the lock
    }
    catch (...)
    {
        return;     // out of memory: drop this notification rather than the monitor
    }

    m_dispatching = true;
    for (const Callback& cb : listeners)
    {
        try
        {
            cb(oldState, newState, snap);
        }
        catch (...)
        {
            // A misbehaving listener must not take down the monitor thread.
        }
    }
    m_dispatching = false;
}

// -----------------------------------------------------------------------------
// PresenceMonitor
// -----------------------------------------------------------------------------
PresenceMonitor& PresenceMonitor::Instance()
{
    static PresenceMonitor instance;
    return instance;
}

PresenceMonitor::PresenceMonitor()
{
    m_stopEvent  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_wakeEvent  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_readyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

PresenceMonitor::~PresenceMonitor()
{
    Stop();
    if (m_readyEvent)
        CloseHandle(m_readyEvent);
    if (m_wakeEvent)
        CloseHandle(m_wakeEvent);
    if (m_stopEvent)
        CloseHandle(m_stopEvent);
}

void PresenceMonitor::Configure(DWORD awayAfterMs)
{
    m_awayAfterMs.store(awayAfterMs != 0 ? awayAfterMs : kDefaultAwayAfterMs);
    if (m_wakeEvent)
        SetEvent(m_wakeEvent);      // apply the new threshold now
}

void PresenceMonitor::AddListener(Callback cb)
{
    if (!cb)
        return;
    std::lock_guard<std::mutex> lock(m_listenerMutex);
    m_listeners.push_back(std::move(cb));
}

bool PresenceMonitor::IsMonitorThread() const
{
    const DWORD id = m_threadId.load();
    return id != 0 && id == GetCurrentThreadId();
}

bool PresenceMonitor::Start()
{
    if (IsMonitorThread())
        return WaitForSingleObject(m_stopEvent, 0) != WAIT_OBJECT_0;   // called from a listener

    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    if (!m_stopEvent || !m_wakeEvent || !m_readyEvent)
        return false;
    if (m_thread.joinable())
    {
        if (WaitForSingleObject(m_stopEvent, 0) != WAIT_OBJECT_0)
            return true;                                // already running
        m_thread.join();                                // reap a thread stopped from a listener
    }

    ResetEvent(m_stopEvent);
    ResetEvent(m_wakeEvent);
    ResetEvent(m_readyEvent);
    m_startResult.store(0);
    try
    {
        m_thread = std::thread(&PresenceMonitor::ThreadMain, this);
    }
    catch (const std::system_error&)
    {
        return false;
    }

    // Wait for the window and the initial snapshot. On timeout (a slow OS call) the thread
    // keeps initializing in the background and is still stopped normally by Stop().
    if (WaitForSingleObject(m_readyEvent, kStartupWaitMs) == WAIT_OBJECT_0 && m_startResult.load() < 0)
    {
        m_thread.join();
        return false;
    }
    return true;
}

void PresenceMonitor::Stop()
{
    if (IsMonitorThread())
    {
        // Called from a listener: the loop exits as soon as the listener returns; the
        // thread is joined by the next Start()/Stop() on another thread or the destructor.
        SetEvent(m_stopEvent);
        return;
    }
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    if (!m_thread.joinable())
        return;
    SetEvent(m_stopEvent);
    m_thread.join();
}

PresenceSnapshot PresenceMonitor::GetSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_snapMutex);
    return m_snap;
}

void PresenceMonitor::NotifyUserActivity()
{
    if (m_wakeEvent)
        SetEvent(m_wakeEvent);
}

void PresenceMonitor::ThreadMain()
{
    m_threadId.store(GetCurrentThreadId());
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        Worker worker(*this, SUCCEEDED(hrCom));
        if (!worker.CreateMessageWindow())
        {
            m_startResult.store(-1);
            SetEvent(m_readyEvent);
        }
        else
        {
            try
            {
                worker.Initialize();
            }
            catch (...)
            {
                // Out of memory during the first evaluation: the next poll retries.
            }
            m_startResult.store(1);
            SetEvent(m_readyEvent);
            try
            {
                worker.Run();
            }
            catch (...)
            {
                // Out of memory inside the loop: fall through to orderly teardown.
            }
        }
    }   // ~Worker: unregister notifications, destroy the window, release COM objects
    if (SUCCEEDED(hrCom))
        CoUninitialize();
    SetEvent(m_stopEvent);      // an unexpected exit looks like a stop, so Start() can restart cleanly
    m_threadId.store(0);
}
