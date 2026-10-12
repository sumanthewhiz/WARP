#pragma once

// ZoneIdentifier
//
// Reads the Mark-of-the-Web alternate data stream ("<file>:Zone.Identifier") that browsers,
// mail clients and archivers attach to downloaded files:
//
//   [ZoneTransfer]
//   ZoneId=3
//   ReferrerUrl=https://example.com/page
//   HostUrl=https://example.com/file.zip
//
// ZoneId: 0 local machine, 1 intranet, 2 trusted, 3 internet, 4 restricted. Reads at most
// 64 KB; accepts UTF-8 / ANSI, or UTF-16 with a BOM. Never throws (except std::bad_alloc).

#include <string>

namespace ZoneIdentifier
{
    struct Info
    {
        bool         present = false;   // the stream exists and could be opened
        int          zoneId = -1;       // -1 when missing or unparsable
        std::wstring hostUrl;
        std::wstring referrerUrl;
    };

    Info Read(const std::wstring& filePath);
}
