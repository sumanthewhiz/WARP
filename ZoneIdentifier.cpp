#include "framework.h"
#include "ZoneIdentifier.h"

#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    const DWORD   kMaxStreamBytes = 64 * 1024;
    const wchar_t kStreamSuffix[] = L":Zone.Identifier";

    class ScopedHandle
    {
    public:
        explicit ScopedHandle(HANDLE handle) : m_handle(handle)
        {
        }

        ~ScopedHandle()
        {
            if (m_handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(m_handle);
            }
        }

        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;

        HANDLE Get() const
        {
            return m_handle;
        }

    private:
        HANDLE m_handle;
    };

    bool IsBlank(wchar_t c)
    {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\0' || c == 0xFEFF;
    }

    std::wstring Trimmed(const std::wstring& text, size_t begin, size_t end)
    {
        while (begin < end && IsBlank(text[begin]))
        {
            ++begin;
        }
        while (end > begin && IsBlank(text[end - 1]))
        {
            --end;
        }
        return text.substr(begin, end - begin);
    }

    bool EqualsNoCase(const std::wstring& a, const wchar_t* b)
    {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b, -1, TRUE) == CSTR_EQUAL;
    }

    std::wstring BuildStreamPath(const std::wstring& filePath)
    {
        std::wstring path = filePath;
        const size_t suffixLength = _countof(kStreamSuffix) - 1;
        // Paths beyond MAX_PATH need the \\?\ form unless the process is long-path aware.
        if (path.size() + suffixLength >= static_cast<size_t>(MAX_PATH) && path.compare(0, 4, L"\\\\?\\") != 0)
        {
            for (wchar_t& c : path)
            {
                if (c == L'/')
                {
                    c = L'\\';
                }
            }
            if (path.size() > 2 && path[0] == L'\\' && path[1] == L'\\' && path[2] != L'.' && path[2] != L'?')
            {
                path = L"\\\\?\\UNC\\" + path.substr(2);
            }
            else if (path.size() > 2 && path[1] == L':' && path[2] == L'\\')
            {
                path = L"\\\\?\\" + path;
            }
        }
        path += kStreamSuffix;
        return path;
    }

    // Returns false when the stream does not exist or cannot be opened.
    bool ReadStream(const std::wstring& streamPath, std::vector<BYTE>& bytes)
    {
        bytes.clear();
        ScopedHandle stream(CreateFileW(streamPath.c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (stream.Get() == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD capacity = kMaxStreamBytes;
        LARGE_INTEGER size = {};
        if (GetFileSizeEx(stream.Get(), &size) && size.QuadPart >= 0 && size.QuadPart < kMaxStreamBytes)
        {
            capacity = static_cast<DWORD>(size.QuadPart);
        }
        bytes.resize(capacity);
        DWORD total = 0;
        while (total < capacity)
        {
            DWORD read = 0;
            if (!ReadFile(stream.Get(), bytes.data() + total, capacity - total, &read, nullptr) || read == 0)
            {
                break;
            }
            total += read;
        }
        bytes.resize(total);
        return true;
    }

    std::wstring MultiByteToWide(UINT codePage, DWORD flags, const char* data, int length)
    {
        const int chars = MultiByteToWideChar(codePage, flags, data, length, nullptr, 0);
        if (chars <= 0)
        {
            return std::wstring();
        }
        std::wstring text(static_cast<size_t>(chars), L'\0');
        if (MultiByteToWideChar(codePage, flags, data, length, &text[0], chars) != chars)
        {
            return std::wstring();
        }
        return text;
    }

    std::wstring Utf16ToWide(const std::vector<BYTE>& bytes, size_t offset, bool bigEndian)
    {
        const size_t count = (bytes.size() - offset) / 2;
        std::wstring text(count, L'\0');
        for (size_t i = 0; i < count; ++i)
        {
            const unsigned first = bytes[offset + 2 * i];
            const unsigned second = bytes[offset + 2 * i + 1];
            text[i] = static_cast<wchar_t>(bigEndian ? ((first << 8) | second) : ((second << 8) | first));
        }
        return text;
    }

    std::wstring Decode(const std::vector<BYTE>& bytes)
    {
        const size_t size = bytes.size();
        if (size >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
        {
            return Utf16ToWide(bytes, 2, false);
        }
        if (size >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF)
        {
            return Utf16ToWide(bytes, 2, true);
        }
        // BOM-less UTF-16LE ("[\0Z\0...") as written by some scripts.
        if (size >= 4 && bytes[0] != 0 && bytes[1] == 0 && bytes[2] != 0 && bytes[3] == 0)
        {
            return Utf16ToWide(bytes, 0, false);
        }
        size_t offset = 0;
        if (size >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        {
            offset = 3;
        }
        if (size <= offset)
        {
            return std::wstring();
        }
        const char* data = reinterpret_cast<const char*>(bytes.data()) + offset;
        const int length = static_cast<int>(size - offset);
        std::wstring text = MultiByteToWide(CP_UTF8, MB_ERR_INVALID_CHARS, data, length);
        if (text.empty())
        {
            text = MultiByteToWide(CP_ACP, 0, data, length);   // not valid UTF-8: legacy ANSI
        }
        return text;
    }

    bool ParseInt(const std::wstring& text, int& value)
    {
        if (text.empty())
        {
            return false;
        }
        const wchar_t* begin = text.c_str();
        wchar_t* end = nullptr;
        errno = 0;
        const long parsed = wcstol(begin, &end, 10);
        if (end == begin || errno == ERANGE)
        {
            return false;
        }
        while (*end != L'\0' && IsBlank(*end))
        {
            ++end;
        }
        if (*end != L'\0')
        {
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    }

    void ParseZoneTransfer(const std::wstring& text, ZoneIdentifier::Info& info)
    {
        bool inZoneTransfer = true;   // keys before any section header are accepted as well
        bool haveZone = false;
        bool haveHost = false;
        bool haveReferrer = false;
        const size_t size = text.size();
        size_t start = 0;
        while (start < size)
        {
            size_t end = text.find_first_of(L"\r\n", start);
            if (end == std::wstring::npos)
            {
                end = size;
            }
            const std::wstring line = Trimmed(text, start, end);
            start = end + 1;
            if (line.empty() || line[0] == L';' || line[0] == L'#')
            {
                continue;
            }
            if (line[0] == L'[')
            {
                const size_t close = line.find(L']');
                const std::wstring section = close == std::wstring::npos ? std::wstring() : Trimmed(line, 1, close);
                inZoneTransfer = EqualsNoCase(section, L"ZoneTransfer");
                continue;
            }
            if (!inZoneTransfer)
            {
                continue;
            }
            const size_t equals = line.find(L'=');
            if (equals == std::wstring::npos)
            {
                continue;
            }
            const std::wstring key = Trimmed(line, 0, equals);
            const std::wstring value = Trimmed(line, equals + 1, line.size());
            // First occurrence wins, as with GetPrivateProfileString.
            if (!haveZone && EqualsNoCase(key, L"ZoneId"))
            {
                int zone = -1;
                if (ParseInt(value, zone))
                {
                    info.zoneId = zone;
                    haveZone = true;
                }
            }
            else if (!haveHost && EqualsNoCase(key, L"HostUrl"))
            {
                info.hostUrl = value;
                haveHost = true;
            }
            else if (!haveReferrer && EqualsNoCase(key, L"ReferrerUrl"))
            {
                info.referrerUrl = value;
                haveReferrer = true;
            }
        }
    }
}

namespace ZoneIdentifier
{
    Info Read(const std::wstring& filePath)
    {
        Info info;
        if (filePath.empty() || filePath.find(L'\0') != std::wstring::npos)
        {
            return info;
        }
        std::vector<BYTE> bytes;
        if (!ReadStream(BuildStreamPath(filePath), bytes))
        {
            return info;
        }
        info.present = true;
        ParseZoneTransfer(Decode(bytes), info);
        return info;
    }
}
