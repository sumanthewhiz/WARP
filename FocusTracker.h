#pragma once
// FocusTracker — per-document focus sessions (replaces ForegroundMonitor).
//
//   APP-5  Sessions split when the (normalized, debounced) window title changes, so switching
//          documents inside one window produces one row per document with its own dwell.
//   APP-6  Rows carry start/end (UTC ms), active vs idle seconds, and context captured at
//          session START (not at emit time, when the broker already names the next app).
//   APP-7  Active seconds flow into the entity store (app + resolved document).
//   APP-8  File Explorer folder windows and Windows Search are tracked (shell chrome is not).
//   SIG-2  Away closes at last input; lock / sleep close at the transition; while away there is
//          no "current session" (no fictional dwell). Passive presence is recorded separately;
//          a background audio signal is not proof of interaction with the foreground document.
//   WEB-1 / PRIV-3  A redaction hook drops titles/documents of private or excluded windows.

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ActivityStore.h"

class FocusTracker
{
public:
    using Callback = std::function<void(FocusEventRecord&& rec)>;

    struct DocInfo
    {
        std::string  key;          // entity key ("" = unresolved)
        std::wstring display;      // full path / canonical URL
        std::string  type;         // file | url
        std::string  resolution = "unresolved";
    };
    enum class Redaction { None, DropContent, DropAll };

    using DocResolverFn = std::function<bool(HWND root, DWORD pid, const std::wstring& exePath,
                                             const std::wstring& normalizedTitle, DocInfo& out)>;
    using RedactFn = std::function<Redaction(HWND root, DWORD pid, const std::wstring& exePath,
                                             const std::wstring& title)>;

    struct ActiveSession
    {
        std::wstring exeName;
        std::wstring exePath;
        std::wstring windowTitle;
        int          durationSoFarSecs = 0;
        int          activeSecsSoFar = 0;
        int          passiveSecsSoFar = 0;
        int          idleSecsSoFar = 0;
        int          unknownSecsSoFar = 0;
        int64_t      startedAtUtcSecs = 0;
        std::string  docKey;
        std::wstring docDisplay;
        std::string  docType;
        std::wstring aumid;
        std::string  docResolution;
    };

    FocusTracker();
    ~FocusTracker();

    void SetCallback(Callback cb);
    void SetDocResolver(DocResolverFn fn);
    void SetRedaction(RedactFn fn);
    void Start();                         // UI thread: subscribes to ForegroundChangeBroker, seeds the current window
    void Stop();                          // emits the open segment
    void Flush();                         // emit the open segment and continue it (logoff / shutdown / maintenance)
    void SetPaused(bool paused);          // privacy pause: close the session; ignore focus until resumed
    // Presence changes from PresenceMonitor: "active", "passive", "away", "locked", "asleep".
    void OnPresenceChanged(const std::string& state, int64_t lastInputUtcMs);
    bool GetCurrentSession(ActiveSession& out) const;

    static bool IsShellChrome(const std::wstring& exeNameLower, const std::wstring& windowClass);
    static std::string AttentionState(const std::string& presence, DWORD msSinceInput);

private:
    struct Segment
    {
        HWND         hwnd = nullptr;
        DWORD        pid = 0;
        std::wstring exeName, exePath, windowClass, aumid, desktopId;
        std::wstring title;                 // normalized
        Redaction    redaction = Redaction::None;
        int64_t      startMs = 0;
        ULONGLONG    lastSampleTick = 0;
        double       activeSecs = 0.0;
        double       idleSecs = 0.0;
        double       passiveSecs = 0.0;
        double       unknownSecs = 0.0;
        EventContext ctx;                   // captured at START (APP-6)
        DocInfo      doc;
        bool         docResolved = false;
        // aumid / desktopId still to be fetched by the sampler thread (COM into Explorer: never
        // on the UI thread, never under m_mtx)
        bool         shellInfoPending = false;
        // pending title change (debounce)
        std::wstring pendingTitle;
        ULONGLONG    pendingSinceTick = 0;
    };

    void OnForegroundChanged(HWND hwnd, DWORD pid);
    void BeginLocked(HWND hwnd, DWORD pid, int64_t startMs, std::vector<FocusEventRecord>& out);
    void EndLocked(int64_t endMs, std::vector<FocusEventRecord>& out);
    void AccountLocked(ULONGLONG nowTick);
    void ResolveDocLocked(Segment& s);
    void Emit(std::vector<FocusEventRecord>& out);
    void SamplerThread();
    static int64_t NowMs();

    Callback      m_callback;
    DocResolverFn m_docResolver;
    RedactFn      m_redact;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_paused{ false };
    size_t        m_brokerToken = 0;
    HANDLE        m_stopEvent = nullptr;
    HANDLE        m_wakeEvent = nullptr;    // auto-reset: a new segment needs its shell info fetched
    std::thread   m_sampler;

    mutable std::mutex m_mtx;
    Segment       m_cur;
    bool          m_haveCur = false;
    bool          m_away = false;
    std::string   m_presence = "active";
};
