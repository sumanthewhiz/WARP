#include "framework.h"
#include "IdleDetector.h"
#include "EventContext.h"

HWND IdleDetector::s_msgWnd = nullptr;
IdleDetector* IdleDetector::s_instance = nullptr;

IdleDetector::IdleDetector()
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_instance = this;
}

IdleDetector::~IdleDetector()
{
    Stop();
    if (m_stopEvent) CloseHandle(m_stopEvent);
    s_instance = nullptr;
}

void IdleDetector::SetCallbacks(Callback onIdle, Callback onActive)
{
    m_onIdle = std::move(onIdle);
    m_onActive = std::move(onActive);
}

void IdleDetector::SetAttributionCallbacks(Callback onAttribLost, Callback onAttribRegained)
{
    m_onAttribLost     = std::move(onAttribLost);
    m_onAttribRegained = std::move(onAttribRegained);
}

void IdleDetector::Start(DWORD pauseThresholdMs, DWORD attribThresholdMs, DWORD wakeBoundaryMs)
{
    if (m_running) return;
    m_pauseThreshold = pauseThresholdMs;
    m_attribThreshold = attribThresholdMs;
    m_wakeBoundaryMs = wakeBoundaryMs;
    m_running = true;
    ResetEvent(m_stopEvent);
    m_thread = std::thread(&IdleDetector::Run, this);
}

void IdleDetector::Stop()
{
    if (!m_running) return;
    m_running = false;
    SetEvent(m_stopEvent);
    if (s_msgWnd)
        PostMessageW(s_msgWnd, WM_QUIT, 0, 0);
    if (m_thread.joinable()) m_thread.join();
}

void IdleDetector::TriggerWakeBoundary()
{
    EventContextUtil::SetWakeBoundary(GetTickCount64() + m_wakeBoundaryMs);
}

LRESULT CALLBACK IdleDetector::MsgWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_POWERBROADCAST && s_instance)
    {
        if (wParam == PBT_APMSUSPEND)
        {
            // Going to sleep
            if (!s_instance->m_isIdle && s_instance->m_onIdle)
            {
                s_instance->m_isIdle = true;
                s_instance->m_onIdle();
            }
        }
        else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND)
        {
            // Waking up. Set the wake-boundary BEFORE re-enabling the
            // monitors so any events that fire during the burst are
            // already attenuated by EventContext::CaptureContext.
            s_instance->TriggerWakeBoundary();
            if (s_instance->m_isIdle && s_instance->m_onActive)
            {
                s_instance->m_isIdle = false;
                s_instance->m_onActive();
            }
        }
        return TRUE;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void IdleDetector::Run()
{
    // Create a message-only window for power notifications
    const wchar_t* className = L"WarpIdleDetectorWnd";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className;
    RegisterClassExW(&wc);

    s_msgWnd = CreateWindowExW(0, className, L"", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, nullptr, wc.hInstance, nullptr);

    HPOWERNOTIFY hPower = nullptr;
    if (s_msgWnd)
        hPower = RegisterPowerSettingNotification(s_msgWnd, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

    while (m_running)
    {
        // Check user idle time. We track two thresholds:
        //   * attribThreshold: drop "user is driving" attribution
        //     (downgrades confidence on subsequent events without
        //     pausing capture).
        //   * pauseThreshold: stop capture entirely.
        LASTINPUTINFO lii = {};
        lii.cbSize = sizeof(lii);
        if (GetLastInputInfo(&lii))
        {
            DWORD idleTime = GetTickCount() - lii.dwTime;

            // Pause / Resume
            if (!m_isIdle && idleTime >= m_pauseThreshold)
            {
                m_isIdle = true;
                if (m_onIdle) m_onIdle();
            }
            else if (m_isIdle && idleTime < m_pauseThreshold)
            {
                // Coming back from a long idle counts as a wake -- the
                // OS replays a similar burst of catch-up activity.
                TriggerWakeBoundary();
                m_isIdle = false;
                if (m_onActive) m_onActive();
            }

            // Attribution-lost / regained (only meaningful when not paused)
            if (!m_isIdle)
            {
                if (!m_attribLost && idleTime >= m_attribThreshold)
                {
                    m_attribLost = true;
                    if (m_onAttribLost) m_onAttribLost();
                }
                else if (m_attribLost && idleTime < m_attribThreshold)
                {
                    m_attribLost = false;
                    if (m_onAttribRegained) m_onAttribRegained();
                }
            }
        }

        // Process messages (for power notifications) and wait
        DWORD waitResult = MsgWaitForMultipleObjects(1, &m_stopEvent, FALSE, 5000, QS_ALLINPUT);
        if (waitResult == WAIT_OBJECT_0) // stop
            break;

        MSG msg;
        while (PeekMessageW(&msg, s_msgWnd, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (hPower)
        UnregisterPowerSettingNotification(hPower);

    if (s_msgWnd)
    {
        DestroyWindow(s_msgWnd);
        s_msgWnd = nullptr;
    }
    UnregisterClassW(className, GetModuleHandleW(nullptr));
}
