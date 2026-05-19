// ---------------------------------------------------------------------------
// SystemProcessClassifier
//
// Replaces the previous OR-of-(name blocklist | path heuristic | service-host
// parent) decision with a multi-signal voting model, grounded in what is
// actually knowable about a Windows process at launch time.
//
// Each signal contributes one vote toward "this is a system / non-user
// process". A configurable threshold (default 3) decides the verdict.
//
// Results are cached by exe path (case-folded) because Authenticode
// verification is expensive (disk read + cryptographic operations + cert
// chain walk) and the answer for a given EXE doesn't change at runtime.
//
// The cheap per-name blocklist is retained as a fast-path that can short-
// circuit the expensive checks on known noisy names (e.g. svchost.exe).
// ---------------------------------------------------------------------------

#pragma once

#include <string>
#include <windows.h>

namespace WARP
{
    struct ClassificationResult
    {
        bool   isSystem  = false;   // verdict
        int    voteCount = 0;       // signals that fired
        double confidence = 0.0;    // 0..1, voteCount / kMaxVotes
        // Bit field of which signals fired, for diagnostics
        DWORD  signals   = 0;
    };

    // Bits in ClassificationResult::signals
    enum SystemSignal : DWORD
    {
        SIG_NAME_BLOCKLIST     = 1u << 0, // exe name is in the well-known list
        SIG_WINDOWS_TREE       = 1u << 1, // path under \Windows\, WinSxS, SystemApps, etc.
        SIG_PARENT_IS_SYSTEM   = 1u << 2, // parented by services.exe / svchost.exe / smss.exe / wininit.exe / csrss.exe / System
        SIG_SESSION_ZERO       = 1u << 3, // running in session 0 (non-interactive)
        SIG_NO_USER_WINDOW     = 1u << 4, // never owned a top-level window after launch (set externally)
        SIG_INTEGRITY_HIGH     = 1u << 5, // token integrity level >= High (suggests system / elevated installer)
        SIG_MS_SIGNED          = 1u << 6, // Authenticode signed by Microsoft
    };

    class SystemProcessClassifier
    {
    public:
        // Three-signals-out-of-seven is the threshold used by Classify().
        // Tuned during analysis: any single signal can false-positive on
        // legit user apps (Office is MS-signed; CMake is in PATH and parented
        // by explorer; etc.), but three independently failing signals is a
        // strong indicator.
        static constexpr int kSystemThreshold = 3;
        static constexpr int kMaxVotes        = 7;

        // Run the full multi-signal evaluation. `pid` is required for the
        // parent / session / integrity checks; `exePath` for the path /
        // signature checks. The cheap name blocklist is computed inside.
        //
        // Cached by lowercase(exePath). The cache holds the static signals
        // (path, signature, name); per-PID signals (parent, session,
        // integrity, no-window) are evaluated every call because they
        // depend on the process instance.
        ClassificationResult Classify(const std::wstring& exePath, DWORD pid);

        // Cheap pre-filter: returns true for hard-coded definite system
        // names (svchost, csrss, smss, lsass, wininit, MsMpEng, etc).
        // Use to short-circuit before the expensive Classify call.
        static bool IsHardBlocked(const std::wstring& exeNameLower);

        // Singleton accessor (we want a single shared cache across monitors).
        static SystemProcessClassifier& Instance();

    private:
        SystemProcessClassifier() = default;
        SystemProcessClassifier(const SystemProcessClassifier&) = delete;
        SystemProcessClassifier& operator=(const SystemProcessClassifier&) = delete;
    };
}
