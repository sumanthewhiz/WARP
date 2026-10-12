// CrashHandler.cpp : minidumps on fault/invalid-parameter/purecall/terminate,
// plus WER auto-restart registration for an always-on WARP agent.
#include "framework.h"
#include "CrashHandler.h"

#include <dbghelp.h>
#include <strsafe.h>
#include <intrin.h>
#include <csignal>
#include <exception>
#include <cstdlib>
#include <cstdint>

#pragma comment(lib, "dbghelp.lib")

#pragma intrinsic(_ReturnAddress)

namespace
{
    wchar_t g_dumpDir[MAX_PATH] = { 0 };
    int     g_keepDumps = 5;
    LONG    g_inHandler = 0;

    const MINIDUMP_TYPE kDumpType = (MINIDUMP_TYPE)(
        MiniDumpNormal |
        MiniDumpWithThreadInfo |
        MiniDumpWithUnloadedModules |
        MiniDumpWithHandleData);

    // Allocation-free: repeatedly find and delete the single oldest dump
    // until at most `keep` remain.  Uses only stack WIN32_FIND_DATAW so it is
    // safe to invoke from a crashing thread.
    void PruneOldDumps(int keep)
    {
        if (keep < 0) keep = 0;
        if (g_dumpDir[0] == L'\0') return;

        for (int guard = 0; guard < 4096; ++guard)
        {
            wchar_t pattern[MAX_PATH];
            if (FAILED(StringCchPrintfW(pattern, MAX_PATH, L"%s\\warp-*.dmp", g_dumpDir)))
                return;

            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(pattern, &fd);
            if (h == INVALID_HANDLE_VALUE)
                return;

            int       count = 0;
            bool      haveOldest = false;
            ULONGLONG oldestTime = 0;
            wchar_t   oldestName[MAX_PATH];
            oldestName[0] = L'\0';

            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;
                ++count;
                ULONGLONG t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32)
                            | fd.ftLastWriteTime.dwLowDateTime;
                if (!haveOldest || t < oldestTime)
                {
                    oldestTime = t;
                    haveOldest = true;
                    StringCchCopyW(oldestName, MAX_PATH, fd.cFileName);
                }
            }
            while (FindNextFileW(h, &fd));
            FindClose(h);

            if (count <= keep || !haveOldest)
                return;

            wchar_t full[MAX_PATH];
            if (SUCCEEDED(StringCchPrintfW(full, MAX_PATH, L"%s\\%s", g_dumpDir, oldestName)))
                DeleteFileW(full);
        }
    }

    bool WriteDumpCore(EXCEPTION_POINTERS* ep, const char* reason,
                       wchar_t* outPath, size_t outCap)
    {
        if (g_dumpDir[0] == L'\0')
            return false;
        if (reason && *reason)
            OutputDebugStringA(reason);

        SYSTEMTIME st;
        GetSystemTime(&st);

        wchar_t path[MAX_PATH];
        if (FAILED(StringCchPrintfW(path, MAX_PATH,
            L"%s\\warp-%04u%02u%02u-%02u%02u%02u-%lu.dmp",
            g_dumpDir,
            (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
            (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
            (unsigned long)GetCurrentProcessId())))
        {
            return false;
        }

        HANDLE hFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
            return false;

        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;

        BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                    hFile, kDumpType,
                                    ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(hFile);

        if (!ok)
        {
            DeleteFileW(path);
            return false;
        }

        if (outPath && outCap)
            StringCchCopyW(outPath, outCap, path);

        PruneOldDumps(g_keepDumps);
        return true;
    }

    // Builds a synthetic EXCEPTION_POINTERS for the current thread so dumps
    // from the non-SEH handlers still capture a usable thread context.
    bool WriteDumpFromHere(const char* reason, wchar_t* outPath, size_t outCap)
    {
        CONTEXT ctx;
        RtlCaptureContext(&ctx);

        EXCEPTION_RECORD er = { 0 };
        er.ExceptionCode = 0xE0000001;                 // app-defined
        er.ExceptionAddress = _ReturnAddress();

        EXCEPTION_POINTERS ep;
        ep.ExceptionRecord = &er;
        ep.ContextRecord = &ctx;

        return WriteDumpCore(&ep, reason, outPath, outCap);
    }

    LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep)
    {
        if (InterlockedCompareExchange(&g_inHandler, 1, 0) != 0)
            return EXCEPTION_CONTINUE_SEARCH;

        wchar_t path[MAX_PATH];
        WriteDumpCore(ep, "unhandled-exception", path, MAX_PATH);

        // Let the default handler (WER) run so RegisterApplicationRestart can
        // relaunch the process.
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // After our own dump, hand the failure to Windows Error Reporting (bypassing every handler):
    // unlike TerminateProcess, this lets RegisterApplicationRestart relaunch the agent.
    [[noreturn]] void FailFast()
    {
        RaiseFailFastException(nullptr, nullptr, 0);
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);                // not reached
    }

    void __cdecl InvalidParamHandler(const wchar_t*, const wchar_t*,
                                     const wchar_t*, unsigned int, uintptr_t)
    {
        wchar_t path[MAX_PATH];
        if (InterlockedCompareExchange(&g_inHandler, 1, 0) == 0)
            WriteDumpFromHere("invalid-parameter", path, MAX_PATH);
        FailFast();
    }

    void __cdecl PureCallHandler()
    {
        wchar_t path[MAX_PATH];
        if (InterlockedCompareExchange(&g_inHandler, 1, 0) == 0)
            WriteDumpFromHere("pure-virtual-call", path, MAX_PATH);
        FailFast();
    }

    void TerminateHandler()
    {
        wchar_t path[MAX_PATH];
        if (InterlockedCompareExchange(&g_inHandler, 1, 0) == 0)
            WriteDumpFromHere("std-terminate", path, MAX_PATH);
        FailFast();
    }

    // std::set_terminate is per thread in the MSVC runtime; abort() (the default terminate on
    // every other thread) raises SIGABRT process-wide.
    void __cdecl AbortSignalHandler(int)
    {
        wchar_t path[MAX_PATH];
        if (InterlockedCompareExchange(&g_inHandler, 1, 0) == 0)
            WriteDumpFromHere("abort", path, MAX_PATH);
        FailFast();
    }
}

void CrashHandler::Install(const std::wstring& dumpDir, const std::wstring& restartArgs, int keepDumps)
{
    StringCchCopyW(g_dumpDir, MAX_PATH, dumpDir.c_str());
    g_keepDumps = (keepDumps < 0) ? 0 : keepDumps;

    if (g_dumpDir[0] != L'\0')
        SHCreateDirectoryExW(nullptr, g_dumpDir, nullptr);

    SetUnhandledExceptionFilter(UnhandledFilter);
    _set_invalid_parameter_handler(InvalidParamHandler);
    _set_purecall_handler(PureCallHandler);
    std::set_terminate(TerminateHandler);
    signal(SIGABRT, AbortSignalHandler);
    _set_abort_behavior(0, _WRITE_ABORT_MSG);       // no "abort() has been called" dialog

    RegisterApplicationRestart(
        restartArgs.empty() ? nullptr : restartArgs.c_str(),
        RESTART_NO_REBOOT | RESTART_NO_PATCH);

    PruneOldDumps(g_keepDumps);
}

std::wstring CrashHandler::WriteDumpNow(const char* reason)
{
    wchar_t path[MAX_PATH] = { 0 };
    if (WriteDumpFromHere(reason ? reason : "manual", path, MAX_PATH))
        return std::wstring(path);
    return std::wstring();
}
