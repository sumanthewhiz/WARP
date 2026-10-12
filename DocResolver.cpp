#include "framework.h"
#include "DocResolver.h"

#include <algorithm>
#include <cwctype>

namespace
{
    std::wstring Lower(std::wstring s)
    {
        for (wchar_t& c : s) c = (wchar_t)std::towlower(c);
        return s;
    }

    std::wstring Trim(const std::wstring& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && std::iswspace(s[a])) ++a;
        while (b > a && std::iswspace(s[b - 1])) --b;
        return s.substr(a, b - a);
    }

    // Splits on " - ", " — ", " – " and " | " (title separators used by apps).
    std::vector<std::wstring> SplitSegments(const std::wstring& t)
    {
        std::vector<std::wstring> out;
        size_t start = 0;
        for (size_t i = 0; i + 2 < t.size() + 1; ++i)
        {
            if (i + 2 < t.size() && t[i] == L' ' && t[i + 2] == L' ' &&
                (t[i + 1] == L'-' || t[i + 1] == 0x2014 || t[i + 1] == 0x2013 || t[i + 1] == L'|'))
            {
                out.push_back(Trim(t.substr(start, i - start)));
                start = i + 3;
                i += 2;
            }
        }
        out.push_back(Trim(t.substr(start)));
        out.erase(std::remove_if(out.begin(), out.end(), [](const std::wstring& s) { return s.empty(); }), out.end());
        return out;
    }

    bool HasExtension(const std::wstring& s)
    {
        size_t dot = s.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0 || dot + 1 >= s.size() || s.size() - dot > 6) return false;
        for (size_t i = dot + 1; i < s.size(); ++i) if (!std::iswalnum(s[i])) return false;
        return true;
    }

    std::wstring StemOf(const std::wstring& name)
    {
        size_t dot = name.find_last_of(L'.');
        return dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
    }

    std::wstring FileNameOf(const std::wstring& path)
    {
        size_t s = path.find_last_of(L"\\/");
        return s == std::wstring::npos ? path : path.substr(s + 1);
    }

    // Apps whose title is "<document> - <app>" (document first).
    const wchar_t* const kDocFirstApps[] = {
        L"winword.exe", L"excel.exe", L"powerpnt.exe", L"visio.exe", L"winproj.exe", L"mspub.exe", L"onenote.exe",
        L"acrord32.exe", L"acrobat.exe", L"foxitpdfeditor.exe", L"foxitphantompdf.exe", L"sumatrapdf.exe",
        L"notepad.exe", L"notepad++.exe", L"sublime_text.exe", L"code.exe", L"code - insiders.exe", L"cursor.exe",
        L"devenv.exe", L"rider64.exe", L"idea64.exe", L"pycharm64.exe", L"webstorm64.exe", L"clion64.exe", L"goland64.exe",
        L"wordpad.exe", L"mspaint.exe", L"photoshop.exe", L"illustrator.exe", L"indesign.exe", L"afterfx.exe",
        L"soffice.bin", L"swriter.exe", L"scalc.exe", L"simpress.exe", L"obsidian.exe", L"typora.exe", L"marktext.exe",
        L"vim.exe", L"gvim.exe", L"emacs.exe", L"texstudio.exe", L"xmind.exe", L"drawio.exe", L"figma.exe",
        L"photos.exe", L"microsoft.photos.exe", L"irfanview.exe", L"i_view64.exe", L"xnview.exe", L"vlc.exe", L"mpc-hc64.exe",
    };
}

namespace DocResolver
{
    std::wstring NormalizeTitle(const std::wstring& title)
    {
        std::wstring t;
        t.reserve(title.size());
        for (wchar_t c : title)
        {
            if (c == 0x200B || c == 0x200C || c == 0x200D || c == 0x2060 || c == 0xFEFF ||
                (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069))
                continue;                                     // zero-width / bidi controls
            if (c == 0x25CF || c == 0x2022) continue;         // "●" / "•" unsaved markers
            t.push_back(std::iswspace(c) ? L' ' : c);
        }
        t = Trim(t);
        // Leading unread counters: "(3) Inbox", "[2] Chat", "(99+) ..."
        for (int pass = 0; pass < 2 && !t.empty(); ++pass)
        {
            if (t[0] == L'(' || t[0] == L'[')
            {
                wchar_t close = t[0] == L'(' ? L')' : L']';
                size_t e = t.find(close);
                if (e != std::wstring::npos && e > 1 && e <= 6)
                {
                    bool counter = true;
                    for (size_t i = 1; i < e; ++i) if (!std::iswdigit(t[i]) && t[i] != L'+') counter = false;
                    if (counter) t = Trim(t.substr(e + 1));
                }
            }
        }
        // Unsaved "*" markers (Notepad "*notes.txt", Word/Excel "Report *").
        while (!t.empty() && t.front() == L'*') t = Trim(t.substr(1));
        size_t star = t.find(L" *");
        if (star != std::wstring::npos && (star + 2 == t.size() || t[star + 2] == L' ')) t.erase(star, 2);
        // Collapse runs of spaces.
        std::wstring out;
        out.reserve(t.size());
        for (wchar_t c : t) if (!(c == L' ' && !out.empty() && out.back() == L' ')) out.push_back(c);
        return Trim(out);
    }

    bool IsDocumentApp(const std::wstring& exeNameLower)
    {
        for (const wchar_t* a : kDocFirstApps) if (exeNameLower == a) return true;
        return false;
    }

    std::wstring DocNameFromTitle(const std::wstring& exeNameLower, const std::wstring& normalizedTitle)
    {
        if (normalizedTitle.empty()) return std::wstring();
        std::vector<std::wstring> seg = SplitSegments(normalizedTitle);
        if (seg.empty()) return std::wstring();
        if (IsDocumentApp(exeNameLower))
        {
            // Visual Studio: "Solution - Microsoft Visual Studio" -> solution; VS Code / JetBrains: file first.
            std::wstring first = seg.front();
            // "Compatibility Mode", "Read-Only", "AutoSaved" suffixes in Office.
            static const wchar_t* const kSuffixes[] = { L" [Compatibility Mode]", L" [Read-Only]", L" - Saved", L" - AutoSaved" };
            for (const wchar_t* s : kSuffixes)
            {
                size_t p = first.find(s);
                if (p != std::wstring::npos) first = Trim(first.substr(0, p));
            }
            if (seg.size() == 1 && !HasExtension(first)) return std::wstring();   // just the app name
            return first;
        }
        // Unknown apps: only a segment that looks like a file name.
        for (const std::wstring& s : seg)
            if (HasExtension(s) && s.find(L' ') == std::wstring::npos) return s;
        return std::wstring();
    }

    bool MatchPath(const std::wstring& docName, const std::vector<std::wstring>& candidates, std::wstring& out)
    {
        if (docName.empty()) return false;
        const std::wstring want = Lower(docName);
        const bool wantHasExt = HasExtension(docName);
        for (const std::wstring& c : candidates)
        {
            std::wstring name = Lower(FileNameOf(c));
            if (name == want || (!wantHasExt && StemOf(name) == want))
            {
                out = c;
                return true;
            }
        }
        return false;
    }
}
