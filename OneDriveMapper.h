#pragma once

// OneDriveMapper
//
// Maps between OneDrive / SharePoint URLs and local paths using the sync roots registered
// under HKCU\Software\SyncEngines\Providers\OneDrive\<id> (MountPoint <-> UrlNamespace).
//
//   * Longest-prefix match, case-insensitive, on path-segment boundaries.
//   * URLs are percent-decoded before matching; query strings and fragments are ignored and
//     SharePoint "/:x:/r/" resource links are unwrapped. '/' <-> '\' are converted.
//   * Results containing "." / ".." segments are rejected (no escaping a sync root).
//   * Thread-safe. Loads lazily on first use; call Refresh() to pick up new sync roots.

#include <mutex>
#include <string>
#include <vector>

struct SyncRootMapping
{
    std::wstring localRoot;      // e.g. C:\Users\me\OneDrive - Contoso
    std::wstring urlNamespace;   // e.g. https://contoso-my.sharepoint.com/personal/me_contoso_com/Documents
};

class OneDriveMapper
{
public:
    static OneDriveMapper& Instance();

    void Refresh();
    std::vector<SyncRootMapping> Mappings() const;
    bool UrlToLocalPath(const std::wstring& url, std::wstring& localOut) const;
    // urlOut is percent-encoded where required (e.g. spaces become %20).
    bool LocalPathToUrl(const std::wstring& path, std::wstring& urlOut) const;

private:
    OneDriveMapper() = default;
    OneDriveMapper(const OneDriveMapper&) = delete;
    OneDriveMapper& operator=(const OneDriveMapper&) = delete;

    void EnsureLoaded() const;

    mutable std::mutex                   m_mutex;
    mutable std::vector<SyncRootMapping> m_mappings;
    mutable std::vector<std::wstring>    m_matchUrls;   // decoded urlNamespace, parallel to m_mappings
    mutable bool                         m_loaded = false;
};
