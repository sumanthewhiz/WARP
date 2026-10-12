// ---------------------------------------------------------------------------
// AddressBarReader -- worker-thread-only UI Automation helper for WebCapture
// (gap-analysis WEB-1 d, WEB-4, WEB-6).
//
// * Finds a browser's address bar STRUCTURALLY and locale-independently: a
//   bounded breadth-first walk of the ControlView (TreeWalker + CacheRequest:
//   ControlType, ClassName, AutomationId, Name, ValueValue, HasKeyboardFocus)
//   that never descends into web content (ControlType Document,
//   "Chrome_RenderWidgetHostHWND") nor into the tab list or bookmarks bar.
//     Chromium omnibox : Edit with ClassName "OmniboxViewViews"
//     Firefox / Gecko  : Edit with AutomationId "urlbar-input"
//     fallback         : an Edit whose ancestor is a ToolBar
// * While walking the browser frame it looks for private-window markers.
// * The address-bar element is cached per window: steady-state reads are one
//   cross-process call (BuildUpdatedCache).
// * One long-lived IUIAutomation (CUIAutomation8 -> IUIAutomation2 with
//   connection / transaction timeouts). Every COM object is created, used and
//   released on the thread that called Init(): never share an instance.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <atomic>
#include <memory>
#include <string>

class AddressBarReader
{
public:
    enum class Status { Ok, Timeout, Failed, NoWindow, Unavailable, Cancelled };

    struct Result
    {
        Status       status = Status::Failed;
        bool         foundAddressBar = false;   // Read: any accepted candidate; Probe: omnibox / urlbar only
        std::wstring value;                     // raw address-bar text (NOT canonicalized)
        bool         valueHasFocus = false;     // the user may be typing: value may not be the page URL
        int          privateVerdict = -1;       // -1 unknown / not checked, 0 no marker, 1 private marker found
        std::wstring rootName;                  // UIA Name of the window element (walks only)
        int          nodesVisited = 0;
        bool         fromCache = false;
    };

    AddressBarReader();
    ~AddressBarReader();
    AddressBarReader(const AddressBarReader&) = delete;
    AddressBarReader& operator=(const AddressBarReader&) = delete;

    // Call on the worker thread after CoInitializeEx(COINIT_MULTITHREADED).
    bool Init(DWORD timeoutMs = 500);
    void Shutdown();                            // releases every COM object; call before CoUninitialize
    bool Ready() const;

    // Reads the address bar of top-level window `root`. checkPrivate: walk the whole browser frame
    // (never web content) and report private-window markers. `cancel` is polled between UIA calls.
    Result Read(HWND root, bool checkPrivate, const std::atomic<bool>* cancel);
    // Browser detection for an unknown Chromium / Gecko-class window: foundAddressBar is true only
    // for an omnibox ("OmniboxViewViews") or a Gecko urlbar ("urlbar-input").
    Result Probe(HWND root, const std::atomic<bool>* cancel);

    void Forget(HWND root);
    void ForgetDeadWindows();

    // Pure helpers (public for unit tests).
    static bool IsPrivateMarkerName(const std::wstring& name);              // "Incognito", "Incognito (2)", "InPrivate", ...
    static bool IsPrivateMarkerToken(const std::wstring& automationIdOrClass);
    static bool RootNameMarksPrivate(const std::wstring& windowName);       // "... (Incognito)", "[InPrivate]", "... Private Browsing"

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
