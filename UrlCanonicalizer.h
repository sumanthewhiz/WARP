// ---------------------------------------------------------------------------
// UrlCanonicalizer
//
// Turns whatever a browser address bar shows into a privacy-safe canonical
// URL plus, when the site is recognized, a stable document identity
// (Google Docs id, SharePoint item GUID, GitHub PR, ...).
//
// Privacy rules: fragments and userinfo are always removed; query strings
// are dropped except for a small per-host allowlist (sensitive keys are
// never kept); token-like path segments become ":token"; sign-in,
// password-manager, banking and patient-portal hosts are flagged excluded.
// ---------------------------------------------------------------------------

#pragma once

#include <string>
#include <vector>

namespace UrlCanon
{
struct Result
{
    std::wstring canonical;     // privacy-safe canonical URL; empty if input unusable (or excluded)
    std::wstring scheme;        // lowercase, e.g. L"https"
    std::wstring host;          // lowercase, punycode/IDN left as-is, no port, no userinfo
    std::wstring docId;         // stable document identity when recognized, else empty
    std::wstring docKind;       // e.g. L"google-docs", L"sharepoint", L"github-pr", L"youtube"; empty if none
    std::wstring title;         // unused by Canonicalize (always empty); reserved
    bool excluded   = false;    // host matched an exclusion -> caller MUST drop the event
    bool isInternal = false;    // about:, chrome://, edge://, file://, data:, blob:, javascript:, extensions, ...
    bool hadQuery   = false;    // original had a non-empty query string (even if dropped)
};

// rawUrl: what a browser address bar shows; may lack a scheme ("example.com/a"), may be "about:blank",
// may contain user typing (spaces) -- then it is NOT a URL and canonical is empty.
// excludedDomains: domains to drop (equal or subdomain match); a built-in denylist always applies too.
// extraAllowedQueryKeys: additional lowercase query keys to keep for ALL hosts (sensitive keys still dropped).
Result Canonicalize(const std::wstring& rawUrl,
                    const std::vector<std::wstring>& excludedDomains = {},
                    const std::vector<std::wstring>& extraAllowedQueryKeys = {});

// True if s plausibly is a URL or bare domain (scheme://..., internal URL, or host[:port][/...]), no spaces.
bool LooksLikeUrl(const std::wstring& s);

// Lowercased host of a URL-ish string (empty on failure).
std::wstring HostOf(const std::wstring& url);

// Percent-decode (UTF-8 aware) for display/matching. Invalid escapes, invalid UTF-8, control characters
// and bidi controls are kept verbatim (still escaped).
std::wstring PercentDecode(const std::wstring& s);
}
