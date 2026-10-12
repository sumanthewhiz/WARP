#include "framework.h"
#include "OneDriveMapper.h"

#include <algorithm>
#include <cstring>
#include <utility>

#pragma comment(lib, "advapi32.lib")

namespace
{
    const wchar_t kProvidersKey[] = L"Software\\SyncEngines\\Providers\\OneDrive";
    const DWORD   kMaxProviders   = 1024;

    class RegKey
    {
    public:
        RegKey() = default;

        ~RegKey()
        {
            if (m_key != nullptr)
            {
                RegCloseKey(m_key);
            }
        }

        RegKey(const RegKey&) = delete;
        RegKey& operator=(const RegKey&) = delete;

        bool Open(HKEY parent, const wchar_t* subKey)
        {
            HKEY key = nullptr;
            if (RegOpenKeyExW(parent, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
            {
                return false;
            }
            m_key = key;
            return true;
        }

        HKEY Get() const
        {
            return m_key;
        }

    private:
        HKEY m_key = nullptr;
    };

    bool ReadRegString(HKEY parent, const wchar_t* subKey, const wchar_t* valueName, std::wstring& value)
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            DWORD bytes = 0;
            LSTATUS status = RegGetValueW(parent, subKey, valueName, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
            if (status != ERROR_SUCCESS || bytes == 0)
            {
                return false;
            }
            std::vector<wchar_t> buffer(static_cast<size_t>(bytes) / sizeof(wchar_t) + 2);
            DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
            status = RegGetValueW(parent, subKey, valueName, RRF_RT_REG_SZ, nullptr, buffer.data(), &size);
            if (status == ERROR_MORE_DATA)
            {
                continue;   // the value grew between the two calls
            }
            if (status != ERROR_SUCCESS)
            {
                return false;
            }
            buffer.back() = L'\0';
            value.assign(buffer.data());
            return true;
        }
        return false;
    }

    bool IsBlank(wchar_t c)
    {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\0';
    }

    void TrimBlanks(std::wstring& text)
    {
        size_t end = text.size();
        while (end > 0 && IsBlank(text[end - 1]))
        {
            --end;
        }
        size_t begin = 0;
        while (begin < end && IsBlank(text[begin]))
        {
            ++begin;
        }
        text = text.substr(begin, end - begin);
    }

    // text starts with prefix (case-insensitive) and the match ends on a separator boundary.
    bool HasPrefixAtBoundary(const std::wstring& text, const std::wstring& prefix, wchar_t separator)
    {
        if (prefix.empty() || text.size() < prefix.size())
        {
            return false;
        }
        if (CompareStringOrdinal(text.c_str(), static_cast<int>(prefix.size()),
                                 prefix.c_str(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL)
        {
            return false;
        }
        return text.size() == prefix.size() || prefix.back() == separator || text[prefix.size()] == separator;
    }

    bool EqualsNoCase(const std::wstring& a, const std::wstring& b)
    {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                    b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    }

    int HexValue(wchar_t c)
    {
        if (c >= L'0' && c <= L'9')
        {
            return c - L'0';
        }
        if (c >= L'a' && c <= L'f')
        {
            return c - L'a' + 10;
        }
        if (c >= L'A' && c <= L'F')
        {
            return c - L'A' + 10;
        }
        return -1;
    }

    std::wstring Utf8ToWide(const std::string& bytes)
    {
        if (bytes.empty())
        {
            return std::wstring();
        }
        const int length = static_cast<int>(bytes.size());
        const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), length, nullptr, 0);
        if (chars <= 0)
        {
            return std::wstring();
        }
        std::wstring text(static_cast<size_t>(chars), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), length, &text[0], chars) != chars)
        {
            return std::wstring();
        }
        return text;
    }

    std::string WideToUtf8(const std::wstring& text)
    {
        if (text.empty())
        {
            return std::string();
        }
        const int length = static_cast<int>(text.size());
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), length, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0)
        {
            return std::string();
        }
        std::string out(static_cast<size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, text.c_str(), length, &out[0], bytes, nullptr, nullptr) != bytes)
        {
            return std::string();
        }
        return out;
    }

    void FlushEscapedBytes(std::string& run, std::wstring& out)
    {
        if (run.empty())
        {
            return;
        }
        const std::wstring decoded = Utf8ToWide(run);
        if (!decoded.empty())
        {
            out += decoded;
        }
        else
        {
            for (char c : run)
            {
                out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));   // not UTF-8: Latin-1
            }
        }
        run.clear();
    }

    // %XX escapes are UTF-8 bytes; consecutive escapes are decoded together. %00 is kept literal.
    std::wstring PercentDecode(const std::wstring& text)
    {
        std::wstring out;
        out.reserve(text.size());
        std::string run;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == L'%' && i + 2 < text.size())
            {
                const int high = HexValue(text[i + 1]);
                const int low = HexValue(text[i + 2]);
                if (high >= 0 && low >= 0 && (high | low) != 0)
                {
                    run.push_back(static_cast<char>((high << 4) | low));
                    i += 2;
                    continue;
                }
            }
            FlushEscapedBytes(run, out);
            out.push_back(text[i]);
        }
        FlushEscapedBytes(run, out);
        return out;
    }

    // Encodes only what cannot appear literally in a URL; reserved delimiters are kept.
    std::wstring PercentEncodeUrl(const std::wstring& text)
    {
        static const char kHex[] = "0123456789ABCDEF";
        static const char kKeep[] = "-._~:/@!$&'()*+,;=";
        const std::string utf8 = WideToUtf8(text);
        std::wstring out;
        out.reserve(utf8.size());
        for (char ch : utf8)
        {
            const unsigned char c = static_cast<unsigned char>(ch);
            const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                              (c != 0 && strchr(kKeep, c) != nullptr);
            if (keep)
            {
                out.push_back(static_cast<wchar_t>(c));
            }
            else
            {
                out.push_back(L'%');
                out.push_back(static_cast<wchar_t>(kHex[c >> 4]));
                out.push_back(static_cast<wchar_t>(kHex[c & 0x0F]));
            }
        }
        return out;
    }

    // https://host/:w:/r/sites/Team/Doc.docx -> https://host/sites/Team/Doc.docx
    void StripResourceLinkSegment(std::wstring& url)
    {
        const size_t scheme = url.find(L"://");
        if (scheme == std::wstring::npos)
        {
            return;
        }
        const size_t pathStart = url.find(L'/', scheme + 3);
        if (pathStart == std::wstring::npos || url.compare(pathStart, 2, L"/:") != 0)
        {
            return;
        }
        const size_t typeEnd = url.find(L':', pathStart + 2);
        if (typeEnd == std::wstring::npos || typeEnd == pathStart + 2 || typeEnd > pathStart + 4)
        {
            return;
        }
        if (url.compare(typeEnd, 4, L":/r/") != 0)
        {
            return;
        }
        url.erase(pathStart, typeEnd + 3 - pathStart);
    }

    std::wstring NormalizeUrlForMatch(const std::wstring& url)
    {
        std::wstring s = url;
        TrimBlanks(s);
        const size_t cut = s.find_first_of(L"?#");
        if (cut != std::wstring::npos)
        {
            s.erase(cut);
        }
        s = PercentDecode(s);
        StripResourceLinkSegment(s);
        while (!s.empty() && s.back() == L'/')
        {
            s.pop_back();
        }
        return s;
    }

    std::wstring NormalizeLocalPath(const std::wstring& path)
    {
        std::wstring s = path;
        TrimBlanks(s);
        std::replace(s.begin(), s.end(), L'/', L'\\');
        if (s.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        {
            s.replace(0, 8, L"\\\\");
        }
        else if (s.compare(0, 4, L"\\\\?\\") == 0)
        {
            s.erase(0, 4);
        }
        while (s.size() > 3 && s.back() == L'\\')
        {
            s.pop_back();
        }
        return s;
    }

    bool HasDotSegment(const std::wstring& relative, wchar_t separator)
    {
        size_t start = 0;
        for (;;)
        {
            size_t end = relative.find(separator, start);
            if (end == std::wstring::npos)
            {
                end = relative.size();
            }
            const size_t length = end - start;
            if ((length == 1 && relative[start] == L'.') ||
                (length == 2 && relative[start] == L'.' && relative[start + 1] == L'.'))
            {
                return true;
            }
            if (end >= relative.size())
            {
                return false;
            }
            start = end + 1;
        }
    }

    void TrimLeadingSeparators(std::wstring& text, wchar_t separator)
    {
        size_t skip = 0;
        while (skip < text.size() && text[skip] == separator)
        {
            ++skip;
        }
        text.erase(0, skip);
    }

    void ReadSyncRoots(std::vector<SyncRootMapping>& mappings, std::vector<std::wstring>& matchUrls)
    {
        RegKey providers;
        if (!providers.Open(HKEY_CURRENT_USER, kProvidersKey))
        {
            return;
        }
        wchar_t name[256] = {};   // registry key names are at most 255 characters
        for (DWORD index = 0; index < kMaxProviders; ++index)
        {
            DWORD length = static_cast<DWORD>(_countof(name));
            const LSTATUS status = RegEnumKeyExW(providers.Get(), index, name, &length,
                                                 nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            if (status != ERROR_SUCCESS)
            {
                continue;
            }
            SyncRootMapping mapping;
            if (!ReadRegString(providers.Get(), name, L"MountPoint", mapping.localRoot) ||
                !ReadRegString(providers.Get(), name, L"UrlNamespace", mapping.urlNamespace))
            {
                continue;
            }
            mapping.localRoot = NormalizeLocalPath(mapping.localRoot);
            TrimBlanks(mapping.urlNamespace);
            while (!mapping.urlNamespace.empty() && mapping.urlNamespace.back() == L'/')
            {
                mapping.urlNamespace.pop_back();
            }
            std::wstring matchUrl = NormalizeUrlForMatch(mapping.urlNamespace);
            if (mapping.localRoot.empty() || matchUrl.find(L"://") == std::wstring::npos)
            {
                continue;
            }
            bool duplicate = false;
            for (size_t i = 0; i < mappings.size(); ++i)
            {
                if (EqualsNoCase(mappings[i].localRoot, mapping.localRoot) && EqualsNoCase(matchUrls[i], matchUrl))
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
            {
                mappings.push_back(std::move(mapping));
                matchUrls.push_back(std::move(matchUrl));
            }
        }
    }
}

OneDriveMapper& OneDriveMapper::Instance()
{
    static OneDriveMapper instance;
    return instance;
}

void OneDriveMapper::Refresh()
{
    std::vector<SyncRootMapping> mappings;
    std::vector<std::wstring> matchUrls;
    ReadSyncRoots(mappings, matchUrls);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_mappings.swap(mappings);
    m_matchUrls.swap(matchUrls);
    m_loaded = true;
}

void OneDriveMapper::EnsureLoaded() const
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_loaded)
        {
            return;
        }
    }
    // Registry I/O happens outside the lock; the first completed load wins.
    std::vector<SyncRootMapping> mappings;
    std::vector<std::wstring> matchUrls;
    ReadSyncRoots(mappings, matchUrls);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_loaded)
    {
        m_mappings.swap(mappings);
        m_matchUrls.swap(matchUrls);
        m_loaded = true;
    }
}

std::vector<SyncRootMapping> OneDriveMapper::Mappings() const
{
    EnsureLoaded();
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_mappings;
}

bool OneDriveMapper::UrlToLocalPath(const std::wstring& url, std::wstring& localOut) const
{
    EnsureLoaded();
    const std::wstring target = NormalizeUrlForMatch(url);
    if (target.empty())
    {
        return false;
    }

    std::wstring local;
    size_t prefixLength = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t best = m_matchUrls.size();
        for (size_t i = 0; i < m_matchUrls.size(); ++i)
        {
            const std::wstring& candidate = m_matchUrls[i];
            if ((best == m_matchUrls.size() || candidate.size() > prefixLength) &&
                HasPrefixAtBoundary(target, candidate, L'/'))
            {
                best = i;
                prefixLength = candidate.size();
            }
        }
        if (best == m_matchUrls.size())
        {
            return false;
        }
        local = m_mappings[best].localRoot;
    }

    std::wstring relative = target.substr(prefixLength);
    std::replace(relative.begin(), relative.end(), L'/', L'\\');
    TrimLeadingSeparators(relative, L'\\');
    if (HasDotSegment(relative, L'\\') || relative.find(L':') != std::wstring::npos)
    {
        return false;   // would escape the sync root or address a stream / drive
    }
    if (!relative.empty())
    {
        if (local.back() != L'\\')
        {
            local.push_back(L'\\');
        }
        local += relative;
    }
    localOut = std::move(local);
    return true;
}

bool OneDriveMapper::LocalPathToUrl(const std::wstring& path, std::wstring& urlOut) const
{
    EnsureLoaded();
    const std::wstring local = NormalizeLocalPath(path);
    if (local.empty())
    {
        return false;
    }

    std::wstring url;
    size_t prefixLength = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t best = m_mappings.size();
        for (size_t i = 0; i < m_mappings.size(); ++i)
        {
            const std::wstring& root = m_mappings[i].localRoot;
            if ((best == m_mappings.size() || root.size() > prefixLength) &&
                HasPrefixAtBoundary(local, root, L'\\'))
            {
                best = i;
                prefixLength = root.size();
            }
        }
        if (best == m_mappings.size())
        {
            return false;
        }
        url = m_matchUrls[best];
    }

    std::wstring relative = local.substr(prefixLength);
    TrimLeadingSeparators(relative, L'\\');
    if (HasDotSegment(relative, L'\\'))
    {
        return false;
    }
    std::replace(relative.begin(), relative.end(), L'\\', L'/');
    if (!relative.empty())
    {
        url.push_back(L'/');
        url += relative;
    }
    std::wstring encoded = PercentEncodeUrl(url);
    if (encoded.empty())
    {
        return false;
    }
    urlOut = std::move(encoded);
    return true;
}
