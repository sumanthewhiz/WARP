#include "framework.h"
#include "SystemProcessClassifier.h"

#include <wintrust.h>
#include <Softpub.h>
#include <wincrypt.h>
#include <mscat.h>
#include <tlhelp32.h>
#include <wtsapi32.h>
#include <algorithm>
#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace WARP
{
    namespace
    {
        // -------------------------------------------------------------------
        // Per-EXE cache for the *static* signals. Path, signature and name
        // never change at runtime; PID-bound signals do.
        // -------------------------------------------------------------------
        struct StaticSignals
        {
            bool nameBlocklist = false;
            bool windowsTree   = false;
            bool msSigned      = false;
        };

        constexpr size_t kCacheCap = 4096;

        std::mutex                                 g_cacheMtx;
        std::unordered_map<std::wstring, StaticSignals> g_cache;
        std::list<std::wstring>                    g_lru;

        // -------------------------------------------------------------------
        // Hard-coded blocklist used by both Classify() (as one vote) and
        // IsHardBlocked() (as a pre-filter for the caller).
        // -------------------------------------------------------------------
        const std::unordered_set<std::wstring>& HardBlockSet()
        {
            static const std::unordered_set<std::wstring> s = {
                // Definite kernel / SCM tier -- never user-launched
                L"svchost.exe", L"csrss.exe", L"smss.exe", L"lsass.exe",
                L"services.exe", L"wininit.exe", L"winlogon.exe",
                L"wmiprvse.exe", L"wmiapsrv.exe", L"dwm.exe",
                L"audiodg.exe", L"fontdrvhost.exe", L"conhost.exe",
                L"sihost.exe", L"ctfmon.exe", L"settingsynchost.exe",
                L"runtimebroker.exe", L"backgroundtaskhost.exe",
                L"applicationframehost.exe", L"textinputhost.exe",
                // Windows Defender real-time engine -- famously high-volume
                L"msmpeng.exe", L"mpcmdrun.exe", L"nissrv.exe",
                L"securityhealthservice.exe", L"securityhealthsystray.exe",
                L"smartscreen.exe", L"sgrmbroker.exe",
                // Windows Update / Servicing
                L"trustedinstaller.exe", L"tiworker.exe", L"musnotifybroker.exe",
                L"usoclient.exe", L"usocoreworker.exe", L"wuauclt.exe",
                // Telemetry / compat
                L"compattelrunner.exe", L"devicecensus.exe", L"diagtrack.exe",
                // Search indexer
                L"searchprotocolhost.exe", L"searchindexer.exe",
                L"searchfilterhost.exe", L"searchhost.exe",
                // Pseudo
                L"system", L"registry", L"idle",
                // Self
                L"warp!.exe",
            };
            return s;
        }

        bool IsInWindowsTree(const std::wstring& exePathLower)
        {
            static const wchar_t* kDirs[] = {
                L"\\windows\\system32\\",
                L"\\windows\\syswow64\\",
                L"\\windows\\systemapps\\",
                L"\\windows\\immersivecontrolpanel\\",
                L"\\windows\\winsxs\\",
                L"\\windows\\servicing\\",
                L"\\windows\\security\\",
                L"\\windows\\temp\\",
                L"\\windows\\softwaredistribution\\",
                L"\\windows\\windowsupdate\\",
                L"\\programdata\\microsoft\\windows defender\\",
            };
            for (const auto* d : kDirs)
                if (exePathLower.find(d) != std::wstring::npos) return true;
            return false;
        }

        // -------------------------------------------------------------------
        // Authenticode + cert-chain "signed by Microsoft" check.
        //
        // We don't just trust WinVerifyTrust's WTD_REVOKE_NONE pass, because
        // any signed binary would qualify -- including signed third-party
        // installers. We additionally inspect the leaf certificate's
        // subject name and require it to be issued to a Microsoft entity.
        //
        // This is intentionally strict; false-negatives for actually-MS code
        // are rare, and false-positives (treating a signed-by-Acme installer
        // as "system") would defeat the whole point of the signal.
        // -------------------------------------------------------------------
        bool IsSubjectMicrosoft(const std::wstring& subject)
        {
            // Canonical Microsoft subject patterns. CN= prefixes vary.
            static const wchar_t* kPats[] = {
                L"Microsoft Corporation",
                L"Microsoft Windows",
                L"Microsoft Windows Publisher",
                L"Microsoft Windows Hardware Compatibility Publisher",
                L"Microsoft Windows Software Compatibility Publisher",
            };
            for (const auto* p : kPats)
                if (subject.find(p) != std::wstring::npos) return true;
            return false;
        }

        bool IsSignedByMicrosoft(const std::wstring& exePath)
        {
            if (exePath.empty()) return false;

            // Step 1: WinVerifyTrust succeeds for *any* valid Authenticode signature.
            WINTRUST_FILE_INFO fileData = {};
            fileData.cbStruct       = sizeof(fileData);
            fileData.pcwszFilePath  = exePath.c_str();

            GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;

            WINTRUST_DATA wd = {};
            wd.cbStruct            = sizeof(wd);
            wd.dwUIChoice          = WTD_UI_NONE;
            wd.fdwRevocationChecks = WTD_REVOKE_NONE;
            wd.dwUnionChoice       = WTD_CHOICE_FILE;
            wd.pFile               = &fileData;
            wd.dwStateAction       = WTD_STATEACTION_VERIFY;
            wd.dwProvFlags         = WTD_CACHE_ONLY_URL_RETRIEVAL |
                                     WTD_REVOCATION_CHECK_NONE |
                                     WTD_DISABLE_MD2_MD4;

            LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE),
                                         &policy, &wd);

            // Always close the trust state (per MSDN), regardless of result.
            wd.dwStateAction = WTD_STATEACTION_CLOSE;
            WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &policy, &wd);

            if (status != ERROR_SUCCESS) return false;

            // Step 2: Look at the actual signer subject name.
            HCERTSTORE  hStore   = nullptr;
            HCRYPTMSG   hMsg     = nullptr;
            DWORD       dwEncoding = 0, dwContentType = 0, dwFormatType = 0;

            if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, exePath.c_str(),
                                  CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                                  CERT_QUERY_FORMAT_FLAG_BINARY,
                                  0, &dwEncoding, &dwContentType, &dwFormatType,
                                  &hStore, &hMsg, nullptr))
                return false;

            DWORD signerInfoSize = 0;
            CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerInfoSize);
            std::vector<BYTE> buf(signerInfoSize);
            CMSG_SIGNER_INFO* signerInfo = reinterpret_cast<CMSG_SIGNER_INFO*>(buf.data());
            bool ok = false;

            if (CryptMsgGetParam(hMsg, CMSG_SIGNER_INFO_PARAM, 0, signerInfo, &signerInfoSize))
            {
                CERT_INFO certInfo = {};
                certInfo.Issuer       = signerInfo->Issuer;
                certInfo.SerialNumber = signerInfo->SerialNumber;

                PCCERT_CONTEXT ctx = CertFindCertificateInStore(
                    hStore, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                    CERT_FIND_SUBJECT_CERT, &certInfo, nullptr);

                if (ctx)
                {
                    DWORD nameSize = CertGetNameStringW(
                        ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
                    if (nameSize > 1)
                    {
                        std::vector<wchar_t> nameBuf(nameSize);
                        CertGetNameStringW(ctx, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                                           nullptr, nameBuf.data(), nameSize);
                        std::wstring subj(nameBuf.data());
                        ok = IsSubjectMicrosoft(subj);
                    }
                    CertFreeCertificateContext(ctx);
                }
            }

            if (hMsg)   CryptMsgClose(hMsg);
            if (hStore) CertCloseStore(hStore, 0);
            return ok;
        }

        StaticSignals ComputeStaticSignals(const std::wstring& exePath,
                                           const std::wstring& exeNameLower,
                                           const std::wstring& exePathLower)
        {
            StaticSignals s;
            s.nameBlocklist = HardBlockSet().count(exeNameLower) > 0;
            s.windowsTree   = IsInWindowsTree(exePathLower);
            s.msSigned      = IsSignedByMicrosoft(exePath);
            return s;
        }

        StaticSignals GetStaticSignalsCached(const std::wstring& exePath)
        {
            std::wstring lower = exePath;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

            std::wstring nameLower = lower;
            auto bs = nameLower.find_last_of(L"\\/");
            if (bs != std::wstring::npos) nameLower = nameLower.substr(bs + 1);

            {
                std::lock_guard<std::mutex> lk(g_cacheMtx);
                auto it = g_cache.find(lower);
                if (it != g_cache.end()) return it->second;
            }

            // Compute outside the lock; signature verification can take many ms.
            StaticSignals s = ComputeStaticSignals(exePath, nameLower, lower);

            {
                std::lock_guard<std::mutex> lk(g_cacheMtx);
                if (g_cache.size() >= kCacheCap && !g_lru.empty())
                {
                    g_cache.erase(g_lru.front());
                    g_lru.pop_front();
                }
                g_cache[lower] = s;
                g_lru.push_back(lower);
            }
            return s;
        }

        // -------------------------------------------------------------------
        // Per-PID signals
        // -------------------------------------------------------------------
        bool IsParentSystem(DWORD pid)
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE) return false;

            DWORD parentPid = 0;
            PROCESSENTRY32W pe = {}; pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe))
            {
                do {
                    if (pe.th32ProcessID == pid) { parentPid = pe.th32ParentProcessID; break; }
                } while (Process32NextW(snap, &pe));
            }

            if (parentPid == 0 || parentPid == 4) { CloseHandle(snap); return true; }

            std::wstring parentExe;
            pe = {}; pe.dwSize = sizeof(pe);
            if (Process32FirstW(snap, &pe))
            {
                do {
                    if (pe.th32ProcessID == parentPid) { parentExe = pe.szExeFile; break; }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);

            std::transform(parentExe.begin(), parentExe.end(), parentExe.begin(), ::towlower);
            return parentExe == L"services.exe" ||
                   parentExe == L"svchost.exe"  ||
                   parentExe == L"smss.exe"     ||
                   parentExe == L"wininit.exe"  ||
                   parentExe == L"csrss.exe"    ||
                   parentExe == L"winlogon.exe";
        }

        bool IsSessionZero(DWORD pid)
        {
            DWORD sessionId = (DWORD)-1;
            if (!ProcessIdToSessionId(pid, &sessionId)) return false;
            return sessionId == 0;
        }

        // High or System integrity is unusual for genuinely user-launched apps
        // (which usually run at Medium). Installers running elevated will be
        // High, but those tend to also satisfy other system signals.
        bool IsHighIntegrity(DWORD pid)
        {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, pid);
            if (!hProc) return false;

            HANDLE hTok = nullptr;
            if (!OpenProcessToken(hProc, TOKEN_QUERY, &hTok))
            {
                CloseHandle(hProc);
                return false;
            }

            DWORD needed = 0;
            GetTokenInformation(hTok, TokenIntegrityLevel, nullptr, 0, &needed);
            std::vector<BYTE> buf(needed);
            bool result = false;
            if (GetTokenInformation(hTok, TokenIntegrityLevel,
                                    buf.data(), needed, &needed))
            {
                TOKEN_MANDATORY_LABEL* tml = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
                if (tml->Label.Sid && IsValidSid(tml->Label.Sid))
                {
                    UCHAR count = *GetSidSubAuthorityCount(tml->Label.Sid);
                    DWORD level = *GetSidSubAuthority(tml->Label.Sid, count - 1);
                    // SECURITY_MANDATORY_HIGH_RID = 0x3000
                    result = level >= 0x3000;
                }
            }

            CloseHandle(hTok);
            CloseHandle(hProc);
            return result;
        }
    } // namespace

    bool SystemProcessClassifier::IsHardBlocked(const std::wstring& exeNameLower)
    {
        return HardBlockSet().count(exeNameLower) > 0;
    }

    SystemProcessClassifier& SystemProcessClassifier::Instance()
    {
        static SystemProcessClassifier inst;
        return inst;
    }

    ClassificationResult SystemProcessClassifier::Classify(
        const std::wstring& exePath, DWORD pid)
    {
        ClassificationResult r;
        if (exePath.empty() && pid == 0) return r;

        StaticSignals s = exePath.empty()
            ? StaticSignals{}
            : GetStaticSignalsCached(exePath);

        if (s.nameBlocklist) { r.signals |= SIG_NAME_BLOCKLIST;   r.voteCount++; }
        if (s.windowsTree)   { r.signals |= SIG_WINDOWS_TREE;     r.voteCount++; }
        if (s.msSigned)      { r.signals |= SIG_MS_SIGNED;        r.voteCount++; }

        if (pid != 0 && pid != 4)
        {
            if (IsParentSystem(pid))   { r.signals |= SIG_PARENT_IS_SYSTEM; r.voteCount++; }
            if (IsSessionZero(pid))    { r.signals |= SIG_SESSION_ZERO;     r.voteCount++; }
            if (IsHighIntegrity(pid))  { r.signals |= SIG_INTEGRITY_HIGH;   r.voteCount++; }
        }

        // SIG_NO_USER_WINDOW is set externally by the LaunchCorrelator on timeout
        // and OR-ed into a subsequent classification. We don't compute it here
        // because we don't have the correlation deadline at this point.

        r.isSystem   = (r.voteCount >= kSystemThreshold);
        r.confidence = static_cast<double>(r.voteCount) / kMaxVotes;
        return r;
    }
}
