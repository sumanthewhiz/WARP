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

    // Resolves only one distinct canonical path. Duplicate observations are harmless;
    // different paths with the same name/stem produce ambiguity, not a recency guess.
    bool MatchPath(const std::wstring& docName, const std::vector<std::wstring>& candidates,
                   std::wstring& out, bool* ambiguous = nullptr);

    // True for apps whose windows are documents (editors, Office, PDF readers, viewers).
    bool IsDocumentApp(const std::wstring& exeNameLower);
}
