#include "framework.h"
#include "AddressBarReader.h"

#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <climits>
#include <cwchar>
#include <deque>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uiautomationcore.lib")

using Microsoft::WRL::ComPtr;

#ifndef UIA_E_ELEMENTNOTAVAILABLE
#define UIA_E_ELEMENTNOTAVAILABLE ((HRESULT)0x80040201L)
#endif
#ifndef UIA_E_TIMEOUT
#define UIA_E_TIMEOUT ((HRESULT)0x80131505L)
#endif

namespace
{
    constexpr int       kMaxNodes      = 600;    // elements examined per walk
    constexpr int       kMaxDepth      = 16;
    constexpr int       kMaxChildren   = 100;    // children enumerated per element
    constexpr ULONGLONG kWalkBudgetMs  = 2500;   // wall-clock budget per walk
    constexpr size_t    kMaxCachedBars = 16;     // per-window address-bar elements kept

    // Web content and heavy frame subtrees that never contain the address bar or a private marker.
    const wchar_t* const kSkipClasses[] =
    {
        L"Chrome_RenderWidgetHostHWND", L"Intermediate D3D Window", L"ContentsWebView",
        L"ContentsContainerView", L"BookmarkBarView", L"SidePanel", L"InfoBarContainerView",
        L"DownloadShelfView",
    };
    const wchar_t* const kSkipAutomationIds[] =
    {
        L"PersonalToolbar", L"sidebar-box", L"tabbrowser-tabs", L"tabbrowser-arrowscrollbox",
    };

    struct NameToken { const wchar_t* text; bool prefix; };
    // Lowercase names browsers give their private-window indicator. Brand terms are rarely translated;
    // a few common translations of "Incognito" are included (best effort).
    const NameToken kPrivateNames[] =
    {
        { L"incognito", false }, { L"inkognito", false }, { L"inc\u00F3gnito", false },
        { L"inprivate", true }, { L"private browsing", false }, { L"private window", false },
        { L"navigation priv\u00E9e", false }, { L"privater modus", false }, { L"navegaci\u00F3n privada", false },
        { L"\u0438\u043D\u043A\u043E\u0433\u043D\u0438\u0442\u043E", false },          // ru
        { L"\u65E0\u75D5", true },                                                    // zh-CN
        { L"\u30B7\u30FC\u30AF\u30EC\u30C3\u30C8", true },                            // ja
        { L"\uC2DC\uD06C\uB9BF", true },                                              // ko
    };

    // Window-name suffixes (after the browser brand) that mark a private window.
    const wchar_t* const kPrivateRootSuffixes[] =
    {
        L"(incognito)", L"(inkognito)", L"(inc\u00F3gnito)", L"(private)",
        L"(\u0438\u043D\u043A\u043E\u0433\u043D\u0438\u0442\u043E)", L"private browsing",
    };

    bool IsInvisible(wchar_t c)
    {
        return c == 0x00AD || c == 0x061C || c == 0x180E || (c >= 0x200B && c <= 0x200F) ||
               (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x2064) || (c >= 0x2066 && c <= 0x2069) ||
               c == 0xFEFF;
    }

    bool IsBlank(wchar_t c)
    {
        return c <= 0x20 || c == 0x7F || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
               c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
    }

    // Invisible characters removed, blanks collapsed to one ' ', trimmed, lowercased.
    std::wstring Clean(const std::wstring& s)
    {
        std::wstring out;
        out.reserve(s.size());
        bool pendingSpace = false;
        for (wchar_t c : s)
        {
            if (IsInvisible(c))
                continue;
            if (IsBlank(c))
            {
                pendingSpace = !out.empty();
                continue;
            }
            if (pendingSpace)
            {
                out.push_back(L' ');
                pendingSpace = false;
            }
            out.push_back(c);
        }
        if (!out.empty())
            CharLowerBuffW(&out[0], static_cast<DWORD>(out.size()));
        return out;
    }

    std::wstring LowerCopy(std::wstring s)
    {
        if (!s.empty())
            CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
        return s;
    }

    bool StartsWith(const std::wstring& s, const std::wstring& prefix)
    {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

    bool EndsWith(const std::wstring& s, const wchar_t* suffix)
    {
        const size_t n = wcslen(suffix);
        return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
    }

    template <size_t N>
    bool InList(const std::wstring& s, const wchar_t* const (&list)[N])
    {
        if (s.empty())
            return false;
        for (const wchar_t* item : list)
        {
            if (s == item)
                return true;
        }
        return false;
    }

    std::wstring TakeBstr(BSTR b)
    {
        std::wstring s;
        if (b)
        {
            s.assign(b, SysStringLen(b));
            SysFreeString(b);
        }
        return s;
    }

    struct Props
    {
        CONTROLTYPEID type = 0;
        std::wstring  cls, aid, name;
    };

    Props ReadCached(IUIAutomationElement* el)
    {
        Props p;
        if (!el)
            return p;
        if (FAILED(el->get_CachedControlType(&p.type)))
            p.type = 0;
        BSTR b = nullptr;
        if (SUCCEEDED(el->get_CachedClassName(&b)))
            p.cls = TakeBstr(b);
        b = nullptr;
        if (SUCCEEDED(el->get_CachedAutomationId(&b)))
            p.aid = TakeBstr(b);
        b = nullptr;
        if (SUCCEEDED(el->get_CachedName(&b)))
            p.name = TakeBstr(b);
        return p;
    }

    std::wstring CachedValue(IUIAutomationElement* el)
    {
        std::wstring out;
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(el->GetCachedPropertyValue(UIA_ValueValuePropertyId, &v)) && v.vt == VT_BSTR && v.bstrVal)
            out.assign(v.bstrVal, SysStringLen(v.bstrVal));
        VariantClear(&v);
        return out;
    }

    bool CachedFocus(IUIAutomationElement* el)
    {
        BOOL focused = FALSE;
        return SUCCEEDED(el->get_CachedHasKeyboardFocus(&focused)) && focused != FALSE;
    }

    // Only structural containers are expanded; leaves (buttons, text, lists, documents, tab lists, ...)
    // are examined but never entered.
    bool IsContainer(CONTROLTYPEID t)
    {
        switch (t)
        {
        case 0:
        case UIA_WindowControlTypeId:
        case UIA_PaneControlTypeId:
        case UIA_GroupControlTypeId:
        case UIA_ToolBarControlTypeId:
        case UIA_CustomControlTypeId:
        case UIA_ComboBoxControlTypeId:
        case UIA_HeaderControlTypeId:
            return true;
        default:
            return false;
        }
    }

    AddressBarReader::Status StatusFromHr(HRESULT hr, HWND root)
    {
        if (hr == UIA_E_TIMEOUT || hr == RPC_E_TIMEOUT || hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT))
            return AddressBarReader::Status::Timeout;
        if (!IsWindow(root))
            return AddressBarReader::Status::NoWindow;
        return AddressBarReader::Status::Failed;
    }

    bool Cancelled(const std::atomic<bool>* cancel)
    {
        return cancel != nullptr && cancel->load();
    }
}

struct AddressBarReader::Impl
{
    ComPtr<IUIAutomation>             automation;
    ComPtr<IUIAutomationCacheRequest> cache;
    ComPtr<IUIAutomationTreeWalker>   walker;

    struct Bar
    {
        HWND                          root = nullptr;
        ComPtr<IUIAutomationElement>  el;
        ULONGLONG                     lastUse = 0;
    };
    std::vector<Bar> bars;

    Bar* FindBar(HWND root)
    {
        for (Bar& b : bars)
        {
            if (b.root == root)
                return &b;
        }
        return nullptr;
    }

    void Remember(HWND root, const ComPtr<IUIAutomationElement>& el)
    {
        if (Bar* existing = FindBar(root))
        {
            existing->el = el;
            existing->lastUse = GetTickCount64();
            return;
        }
        if (bars.size() >= kMaxCachedBars)
        {
            size_t oldest = 0;
            for (size_t i = 1; i < bars.size(); ++i)
            {
                if (bars[i].lastUse < bars[oldest].lastUse)
                    oldest = i;
            }
            bars.erase(bars.begin() + static_cast<std::ptrdiff_t>(oldest));
        }
        Bar b;
        b.root = root;
        b.el = el;
        b.lastUse = GetTickCount64();
        bars.push_back(std::move(b));
    }

    void Walk(HWND root, bool checkPrivate, bool probe, const std::atomic<bool>* cancel, Result& r);
};

void AddressBarReader::Impl::Walk(HWND root, bool checkPrivate, bool probe, const std::atomic<bool>* cancel, Result& r)
{
    struct Node
    {
        ComPtr<IUIAutomationElement> el;
        int                          depth = 0;
        bool                         underToolbar = false;
    };

    ComPtr<IUIAutomationElement> rootEl;
    HRESULT hr = automation->ElementFromHandleBuildCache(reinterpret_cast<UIA_HWND>(root), cache.Get(),
                                                         rootEl.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !rootEl)
    {
        r.status = FAILED(hr) ? StatusFromHr(hr, root) : Status::Failed;
        return;
    }
    const Props rootProps = ReadCached(rootEl.Get());
    r.rootName = rootProps.name;
    bool privateFound = checkPrivate && RootNameMarksPrivate(rootProps.name);

    std::deque<Node> queue;
    Node first;
    first.el = rootEl;
    queue.push_back(std::move(first));

    ComPtr<IUIAutomationElement> best;
    int bestScore = 0;
    int bestDepth = INT_MAX;
    bool complete = false, timedOut = false, partialFailure = false;
    const ULONGLONG deadline = GetTickCount64() + kWalkBudgetMs;

    while (!queue.empty())
    {
        if (Cancelled(cancel))
        {
            r.status = Status::Cancelled;
            return;
        }
        if (r.nodesVisited >= kMaxNodes || GetTickCount64() > deadline)
            break;                                                    // budget exhausted: incomplete
        if (bestScore >= 3 && (probe || !checkPrivate || privateFound))
        {
            complete = true;                                          // nothing left to learn
            break;
        }
        if (bestScore >= 3 && queue.front().depth > bestDepth)
        {
            complete = true;                                          // every frame element up to the omnibox level was seen
            break;
        }

        Node n = std::move(queue.front());
        queue.pop_front();
        ++r.nodesVisited;
        const Props p = n.depth == 0 ? rootProps : ReadCached(n.el.Get());

        if (n.depth > 0)
        {
            if (p.type == UIA_EditControlTypeId)
            {
                int score = 0;
                if (p.cls == L"OmniboxViewViews" || p.aid == L"urlbar-input")
                    score = 3;
                else if (n.underToolbar && !probe)
                    score = 1;
                if (score > bestScore)
                {
                    best = n.el;
                    bestScore = score;
                    bestDepth = n.depth;
                }
            }
            if (checkPrivate && !privateFound &&
                (IsPrivateMarkerToken(p.aid) || IsPrivateMarkerToken(p.cls) || IsPrivateMarkerName(p.name)))
            {
                privateFound = true;
            }
        }

        // Never enter web content (Document, render widget host) or heavy non-chrome subtrees.
        if (!IsContainer(p.type) || n.depth >= kMaxDepth)
            continue;
        if (InList(p.cls, kSkipClasses) || InList(p.aid, kSkipAutomationIds))
            continue;
        if (bestScore >= 3 && n.depth >= bestDepth)
            continue;

        const bool underToolbar = n.underToolbar || p.type == UIA_ToolBarControlTypeId;
        ComPtr<IUIAutomationElement> child;
        hr = walker->GetFirstChildElementBuildCache(n.el.Get(), cache.Get(), child.ReleaseAndGetAddressOf());
        int count = 0;
        while (SUCCEEDED(hr) && child && count < kMaxChildren)
        {
            Node c;
            c.el = child;
            c.depth = n.depth + 1;
            c.underToolbar = underToolbar;
            queue.push_back(std::move(c));
            ++count;
            if (Cancelled(cancel))
                break;
            ComPtr<IUIAutomationElement> next;
            hr = walker->GetNextSiblingElementBuildCache(child.Get(), cache.Get(), next.ReleaseAndGetAddressOf());
            child = next;
        }
        if (FAILED(hr) && hr != UIA_E_ELEMENTNOTAVAILABLE)
        {
            const Status st = StatusFromHr(hr, root);
            if (st == Status::Timeout)
            {
                timedOut = true;
                break;
            }
            if (st == Status::NoWindow)
            {
                r.status = st;
                return;
            }
            partialFailure = true;                                    // keep walking other branches
        }
    }
    if (queue.empty() && !timedOut)
        complete = true;

    if (best && (!probe || bestScore >= 3))
    {
        r.foundAddressBar = true;
        r.value = CachedValue(best.Get());
        r.valueHasFocus = CachedFocus(best.Get());
        Remember(root, best);
    }
    if (checkPrivate)
        r.privateVerdict = privateFound ? 1 : ((complete && !partialFailure) ? 0 : -1);
    if (timedOut)
        r.status = Status::Timeout;
    else if (partialFailure && !r.foundAddressBar)
        r.status = Status::Failed;
    else
        r.status = Status::Ok;
}

AddressBarReader::AddressBarReader() : m(new Impl())
{
}

AddressBarReader::~AddressBarReader()
{
    Shutdown();
}

bool AddressBarReader::Init(DWORD timeoutMs)
{
    Shutdown();

    // CUIAutomation8 (Windows 8+) exposes IUIAutomation2 with connection / transaction timeouts, so a
    // hung browser costs at most ~timeoutMs per call instead of the 20 s default.
    ComPtr<IUIAutomation2> automation2;
    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation2));
    if (SUCCEEDED(hr) && automation2)
    {
        automation2->put_ConnectionTimeout(timeoutMs);
        automation2->put_TransactionTimeout(timeoutMs);
        m->automation = automation2;
    }
    else
    {
        hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m->automation));
        if (FAILED(hr) || !m->automation)
        {
            Shutdown();
            return false;
        }
    }

    if (FAILED(m->automation->CreateCacheRequest(m->cache.ReleaseAndGetAddressOf())) || !m->cache)
    {
        Shutdown();
        return false;
    }
    const PROPERTYID props[] =
    {
        UIA_ControlTypePropertyId, UIA_ClassNamePropertyId, UIA_AutomationIdPropertyId,
        UIA_NamePropertyId, UIA_ValueValuePropertyId, UIA_HasKeyboardFocusPropertyId,
    };
    for (PROPERTYID id : props)
    {
        if (FAILED(m->cache->AddProperty(id)))
        {
            Shutdown();
            return false;
        }
    }
    if (FAILED(m->automation->get_ControlViewWalker(m->walker.ReleaseAndGetAddressOf())) || !m->walker)
    {
        Shutdown();
        return false;
    }
    return true;
}

void AddressBarReader::Shutdown()
{
    if (!m)
        return;
    m->bars.clear();
    m->walker.Reset();
    m->cache.Reset();
    m->automation.Reset();
}

bool AddressBarReader::Ready() const
{
    return m && m->automation && m->cache && m->walker;
}

AddressBarReader::Result AddressBarReader::Read(HWND root, bool checkPrivate, const std::atomic<bool>* cancel)
{
    Result r;
    if (!Ready())
    {
        r.status = Status::Unavailable;
        return r;
    }
    if (!root || !IsWindow(root))
    {
        Forget(root);
        r.status = Status::NoWindow;
        return r;
    }

    if (!checkPrivate)
    {
        if (Impl::Bar* bar = m->FindBar(root))
        {
            ComPtr<IUIAutomationElement> fresh;
            const HRESULT hr = bar->el->BuildUpdatedCache(m->cache.Get(), fresh.ReleaseAndGetAddressOf());
            if (SUCCEEDED(hr) && fresh)
            {
                CONTROLTYPEID type = 0;
                if (SUCCEEDED(fresh->get_CachedControlType(&type)) && type == UIA_EditControlTypeId)
                {
                    bar->lastUse = GetTickCount64();
                    r.status = Status::Ok;
                    r.fromCache = true;
                    r.foundAddressBar = true;
                    r.value = CachedValue(fresh.Get());
                    r.valueHasFocus = CachedFocus(fresh.Get());
                    return r;
                }
            }
            else if (FAILED(hr) && StatusFromHr(hr, root) == Status::Timeout)
            {
                r.status = Status::Timeout;
                return r;
            }
            Forget(root);                                             // stale element: walk again
        }
    }

    m->Walk(root, checkPrivate, false, cancel, r);
    return r;
}

AddressBarReader::Result AddressBarReader::Probe(HWND root, const std::atomic<bool>* cancel)
{
    Result r;
    if (!Ready())
    {
        r.status = Status::Unavailable;
        return r;
    }
    if (!root || !IsWindow(root))
    {
        r.status = Status::NoWindow;
        return r;
    }
    m->Walk(root, false, true, cancel, r);
    return r;
}

void AddressBarReader::Forget(HWND root)
{
    if (!m)
        return;
    for (auto it = m->bars.begin(); it != m->bars.end(); ++it)
    {
        if (it->root == root)
        {
            m->bars.erase(it);
            return;
        }
    }
}

void AddressBarReader::ForgetDeadWindows()
{
    if (!m)
        return;
    for (auto it = m->bars.begin(); it != m->bars.end();)
    {
        if (!IsWindow(it->root))
            it = m->bars.erase(it);
        else
            ++it;
    }
}

bool AddressBarReader::IsPrivateMarkerName(const std::wstring& name)
{
    const std::wstring n = Clean(name);
    if (n.empty() || n.size() > 48)
        return false;
    for (const NameToken& t : kPrivateNames)
    {
        const std::wstring token(t.text);
        if (n == token)
            return true;
        if (!StartsWith(n, token))
            continue;
        if (t.prefix)
            return true;
        // Chromium numbers concurrent private windows: "Incognito (2)".
        const std::wstring rest = n.substr(token.size());
        if (rest.size() >= 4 && rest[0] == L' ' && rest[1] == L'(' && rest.back() == L')')
        {
            bool digits = true;
            for (size_t i = 2; i + 1 < rest.size(); ++i)
            {
                if (rest[i] < L'0' || rest[i] > L'9')
                    digits = false;
            }
            if (digits)
                return true;
        }
    }
    return false;
}

bool AddressBarReader::IsPrivateMarkerToken(const std::wstring& automationIdOrClass)
{
    if (automationIdOrClass.empty())
        return false;
    const std::wstring s = LowerCopy(automationIdOrClass);
    // "private-browsing-indicator[-with-label]" is Firefox's indicator; Firefox's customizable
    // "privatebrowsing-button" (open a private window) exists in normal windows and must not match.
    return s.find(L"incognito") != std::wstring::npos || s.find(L"inprivate") != std::wstring::npos ||
           s.find(L"private-browsing-indicator") != std::wstring::npos;
}

bool AddressBarReader::RootNameMarksPrivate(const std::wstring& windowName)
{
    const std::wstring n = Clean(windowName);
    if (n.empty())
        return false;
    if (n.find(L"[inprivate]") != std::wstring::npos)
        return true;
    for (const wchar_t* suffix : kPrivateRootSuffixes)
    {
        if (EndsWith(n, suffix))
            return true;
    }
    return false;
}
