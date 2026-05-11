#include "framework.h"
#include "UrlExtractor.h"

#include <UIAutomation.h>
#include <comdef.h>
#include <algorithm>

UrlExtractor& UrlExtractor::Instance()
{
    static UrlExtractor inst;
    return inst;
}

void UrlExtractor::Start()
{
    if (m_running.exchange(true)) return;
    m_worker = std::thread(&UrlExtractor::WorkerLoop, this);
}

void UrlExtractor::Stop()
{
    if (!m_running.exchange(false)) return;
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void UrlExtractor::Submit(HWND hwnd,
                          const std::wstring& browser,
                          const std::wstring& title,
                          const EventContext& ctx)
{
    if (!m_running) return;

    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_queue.size() >= kMaxQueue) return;   // drop on overflow
        m_queue.push_back({ hwnd, browser, title, ctx });
    }
    m_cv.notify_one();
}

void UrlExtractor::WorkerLoop()
{
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IUIAutomation* pAutomation = nullptr;
    if (SUCCEEDED(hrInit))
    {
        CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IUIAutomation, reinterpret_cast<void**>(&pAutomation));
    }

    while (m_running)
    {
        Request r;
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cv.wait(lk, [this] { return !m_running || !m_queue.empty(); });
            if (!m_running) break;
            r = std::move(m_queue.front());
            m_queue.pop_front();
        }

        std::wstring url;
        if (pAutomation)
        {
            // Re-bind ExtractUrl to use the live IUIAutomation pointer
            // captured in the worker thread. ExtractUrl below uses a
            // function-local accessor.
            url = ExtractUrl(r.hwnd, r.browser);
        }

        if (m_emitCb)
            m_emitCb(r.browser, r.title, url, r.ctx);
    }

    if (pAutomation) pAutomation->Release();
    if (SUCCEEDED(hrInit)) CoUninitialize();
}

namespace
{
    // RAII helper for COM raw pointers used in the extractor.
    template <typename T>
    struct ComPtr
    {
        T* p = nullptr;
        ComPtr() = default;
        ~ComPtr() { if (p) p->Release(); }
        T** operator&() { return &p; }
        T*  operator->() const { return p; }
        operator bool() const { return p != nullptr; }
        void Reset() { if (p) { p->Release(); p = nullptr; } }
    };

    bool BstrEquals(BSTR b, const wchar_t* s)
    {
        if (!b || !s) return false;
        return wcscmp(b, s) == 0;
    }

    bool BstrIequals(BSTR b, const wchar_t* s)
    {
        if (!b || !s) return false;
        return _wcsicmp(b, s) == 0;
    }

    // Read the current value out of an element via ValuePattern.
    std::wstring ReadValue(IUIAutomationElement* el)
    {
        std::wstring out;
        if (!el) return out;
        IUnknown* pUnk = nullptr;
        if (FAILED(el->GetCurrentPattern(UIA_ValuePatternId, &pUnk)) || !pUnk)
            return out;
        IUIAutomationValuePattern* vp = nullptr;
        if (SUCCEEDED(pUnk->QueryInterface(IID_PPV_ARGS(&vp))) && vp)
        {
            BSTR b = nullptr;
            if (SUCCEEDED(vp->get_CurrentValue(&b)) && b)
            {
                out.assign(b, SysStringLen(b));
                SysFreeString(b);
            }
            vp->Release();
        }
        pUnk->Release();
        return out;
    }
} // namespace

std::wstring UrlExtractor::ExtractUrl(HWND hwnd, const std::wstring& browser)
{
    // Re-create the automation object on demand. The Worker thread holds
    // a long-lived one; ExtractUrl is called from there. We re-acquire
    // here to keep this function self-contained for testing.
    IUIAutomation* pAutomation = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IUIAutomation, reinterpret_cast<void**>(&pAutomation))))
        return L"";

    std::wstring url;

    ComPtr<IUIAutomationElement> root;
    if (FAILED(pAutomation->ElementFromHandle(hwnd, &root)) || !root)
    {
        pAutomation->Release();
        return L"";
    }

    // Build a condition that finds Edit controls. We then walk the matches
    // and pick the one that looks like an address bar.
    VARIANT vEdit = {};
    vEdit.vt = VT_I4;
    vEdit.lVal = UIA_EditControlTypeId;

    ComPtr<IUIAutomationCondition> cEdit;
    if (FAILED(pAutomation->CreatePropertyCondition(
                   UIA_ControlTypePropertyId, vEdit, &cEdit)))
    {
        pAutomation->Release();
        return L"";
    }

    ComPtr<IUIAutomationElementArray> matches;
    if (FAILED(root->FindAll(TreeScope_Descendants, cEdit.p, &matches)) || !matches)
    {
        pAutomation->Release();
        return L"";
    }

    int count = 0;
    matches->get_Length(&count);

    // Browser-specific AutomationId / Name patterns for the address bar.
    // Order: prefer AutomationId (most reliable); fall back to Name match.
    auto isAddressEdit = [&](IUIAutomationElement* el) -> bool {
        BSTR aid = nullptr, name = nullptr;
        bool match = false;
        if (SUCCEEDED(el->get_CurrentAutomationId(&aid)) && aid)
        {
            // Chromium omnibox: AutomationId == "url" or
            //   contains "Address" in some builds.
            // Firefox: "urlbar-input"
            // Edge legacy: "addressEditBox"
            if (BstrIequals(aid, L"url") ||
                BstrIequals(aid, L"urlbar-input") ||
                BstrIequals(aid, L"addressEditBox"))
                match = true;
            SysFreeString(aid);
        }
        if (!match && SUCCEEDED(el->get_CurrentName(&name)) && name)
        {
            // Localized names vary; English includes "Address and search bar"
            // (Chromium) or "Search or enter address" (Firefox). Substring
            // match catches common patterns without locale lock-in.
            std::wstring n(name, SysStringLen(name));
            std::wstring nl = n;
            std::transform(nl.begin(), nl.end(), nl.begin(), ::towlower);
            if (nl.find(L"address") != std::wstring::npos ||
                nl.find(L"search or enter") != std::wstring::npos ||
                nl.find(L"omnibox") != std::wstring::npos)
                match = true;
            SysFreeString(name);
        }
        return match;
    };

    for (int i = 0; i < count; ++i)
    {
        ComPtr<IUIAutomationElement> el;
        if (FAILED(matches->GetElement(i, &el)) || !el) continue;
        if (!isAddressEdit(el.p)) continue;

        url = ReadValue(el.p);
        if (!url.empty())
        {
            // The omnibox often displays a stripped form ("example.com/foo")
            // rather than the canonical scheme; prepend https:// when the
            // value lacks a scheme so downstream URL handling works.
            std::wstring lower = url;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            if (lower.compare(0, 7, L"http://") != 0 &&
                lower.compare(0, 8, L"https://") != 0 &&
                lower.compare(0, 7, L"file://") != 0 &&
                lower.compare(0, 9, L"chrome://") != 0 &&
                lower.compare(0, 7, L"edge://") != 0 &&
                lower.compare(0, 7, L"about:") != 0)
            {
                url = L"https://" + url;
            }
            break;
        }
    }

    pAutomation->Release();
    (void)browser;   // currently unused; kept for future browser-specific logic
    return url;
}
