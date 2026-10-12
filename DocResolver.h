#pragma once
// DocResolver — "which document is this window about?" (gap-analysis §5.3, APP-5, WEB-8).
// Pure string helpers (unit-testable); the live resolution that combines browser pages and
// recent file opens is wired in WARP!.cpp.

#include <string>
#include <vector>

namespace DocResolver
{
    // Normalizes a window title for session splitting and display: strips unread counters
    // ("(3) Inbox"), unsaved markers ("●", leading/trailing "*"), zero-width / bidi characters
    // and collapses whitespace.
    std::wstring NormalizeTitle(const std::wstring& title);

    // Document name a window title refers to, using per-app title grammars
    // ("Report - Word" -> "Report", "auth.cpp - repo - Visual Studio Code" -> "auth.cpp",
    // "*notes.txt - Notepad" -> "notes.txt"). Empty if the app has no document grammar or
    // the title carries no document.
    std::wstring DocNameFromTitle(const std::wstring& exeNameLower, const std::wstring& normalizedTitle);

    // Picks the most recent candidate path whose file name (or stem, for apps that hide the
    // extension) equals docName, case-insensitively. Candidates are newest first.
    bool MatchPath(const std::wstring& docName, const std::vector<std::wstring>& candidates, std::wstring& out);

    // True for apps whose windows are documents (editors, Office, PDF readers, viewers).
    bool IsDocumentApp(const std::wstring& exeNameLower);
}
