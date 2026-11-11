// Installer.cpp : "always on" WARP via Task Scheduler (interim design until a
// dedicated service split exists).  Installs into %ProgramFiles%\WARP and
// registers a highest-privilege logon task so WARP starts at every logon
// without a per-logon UAC prompt.
#include "framework.h"
#include "Installer.h"

#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>
#include <sddl.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <string>
#include <vector>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
    // --- COM RAII helpers ---------------------------------------------------
    struct ComInit
    {
        HRESULT hr;
        bool    uninit;
        ComInit()
        {
            hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            uninit = (hr == S_OK || hr == S_FALSE);
        }
        ~ComInit()
        {
            if (uninit)
                CoUninitialize();
        }
        ComInit(const ComInit&) = delete;
        ComInit& operator=(const ComInit&) = delete;
    };

    template <class T>
    struct ComPtr
    {
        T* p = nullptr;
        ComPtr() = default;
        ~ComPtr() { if (p) p->Release(); }
        ComPtr(const ComPtr&) = delete;
        ComPtr& operator=(const ComPtr&) = delete;
        T** put()      { return &p; }
        T*  get() const { return p; }
        T*  operator->() const { return p; }
        void reset() { if (p) { p->Release(); p = nullptr; } }
    };

    struct BStr
    {
        BSTR b;
        explicit BStr(const wchar_t* s) : b(SysAllocString(s)) {}
        ~BStr() { if (b) SysFreeString(b); }
        BStr(const BStr&) = delete;
        BStr& operator=(const BStr&) = delete;
        operator BSTR() const { return b; }
    };

    struct Var
    {
        VARIANT v;
        Var() { VariantInit(&v); }
        ~Var() { VariantClear(&v); }
        Var(const Var&) = delete;
        Var& operator=(const Var&) = delete;
    };

    std::wstring HrHex(HRESULT hr)
    {
        wchar_t b[32];
        swprintf_s(b, L"0x%08X", (unsigned)hr);
        return std::wstring(b);
    }

    Installer::Result Ok(const std::wstring& msg)   { return { true,  msg }; }
    Installer::Result Fail(const std::wstring& msg) { return { false, msg }; }

    // --- Path / identity helpers -------------------------------------------
    std::wstring CurrentExePath()
    {
        DWORD cap = MAX_PATH;
        for (;;)
        {
            std::wstring buf;
            buf.resize(cap);
            DWORD n = GetModuleFileNameW(nullptr, &buf[0], cap);
            if (n == 0)
                return std::wstring();
            if (n < cap)
            {
                buf.resize(n);
                return buf;
            }
            if (cap >= 32768)
            {
                buf.resize(n);
                return buf;
            }
            cap *= 2;
        }
    }

    std::wstring DirOf(const std::wstring& path)
    {
        size_t s = path.find_last_of(L"\\/");
        return (s == std::wstring::npos) ? std::wstring() : path.substr(0, s);
    }

    std::wstring ExeName(const std::wstring& path)
    {
        size_t s = path.find_last_of(L"\\/");
        return (s == std::wstring::npos) ? path : path.substr(s + 1);
    }

    bool PathExists(const std::wstring& p)
    {
        return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    std::wstring KnownFolder(REFKNOWNFOLDERID id)
    {
        PWSTR p = nullptr;
        std::wstring r;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p)
            r = p;
        if (p)
            CoTaskMemFree(p);
        return r;
    }

    bool GetProcessUserSid(std::wstring& out)
    {
        out.clear();
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
            return false;

        DWORD len = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
        if (len == 0)
        {
            CloseHandle(tok);
            return false;
        }

        std::vector<BYTE> buf(len);
        bool ok = false;
        if (GetTokenInformation(tok, TokenUser, buf.data(), len, &len))
        {
            TOKEN_USER* tu = reinterpret_cast<TOKEN_USER*>(buf.data());
            LPWSTR s = nullptr;
            if (ConvertSidToStringSidW(tu->User.Sid, &s) && s)
            {
                out = s;
                ok = true;
                LocalFree(s);
            }
        }
        CloseHandle(tok);
        return ok;
    }

    std::wstring ShortTaskName()
    {
        std::wstring sid;
        GetProcessUserSid(sid);
        return L"WARP Agent - " + sid;
    }

    // --- Recursive file ops (Win32, non-throwing) --------------------------
    // Name-surrogate reparse points (junctions, symbolic links) are skipped: the installer runs elevated
    // and must never be redirected into copying files from (or to) elsewhere. Other reparse points -
    // OneDrive / cloud-files placeholders, dedup - are ordinary data and are copied.
    bool IsLinkReparsePoint(const std::wstring& path, DWORD attrs)
    {
        if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(path.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return true;            // tag unknown: treat it as a link
        FindClose(h);
        return IsReparseTagNameSurrogate(fd.dwReserved0) != 0;
    }

    bool CopyDirRecursive(const std::wstring& src, const std::wstring& dst, std::wstring& err)
    {
        if (!CreateDirectoryW(dst.c_str(), nullptr))
        {
            DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS)
            {
                if (err.empty())
                    err = L"CreateDirectory failed for " + dst;
                return false;
            }
        }

        std::wstring pattern = src + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            return true;   // nothing to copy

        bool okAll = true;
        do
        {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(fd.dwReserved0))
                continue;

            std::wstring s = src + L"\\" + fd.cFileName;
            std::wstring d = dst + L"\\" + fd.cFileName;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!CopyDirRecursive(s, d, err))
                    okAll = false;
            }
            else
            {
                if (!CopyFileW(s.c_str(), d.c_str(), FALSE))
                {
                    SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
                    if (!CopyFileW(s.c_str(), d.c_str(), FALSE))
                    {
                        if (err.empty())
                            err = L"CopyFile failed for " + d;
                        okAll = false;
                    }
                }
            }
        }
        while (FindNextFileW(h, &fd));
        FindClose(h);
        return okAll;
    }

    // Copies exactly the WARP package (the running exe, the ONNX Runtime DLLs, the shipped
    // documents and the models\ and licenses\ folders) - never whatever else shares the folder the
    // exe was started from (e.g. Downloads).
    bool CopyPayload(const std::wstring& srcDir, const std::wstring& dstDir, const std::wstring& exeName, std::wstring& err)
    {
        if (!CreateDirectoryW(dstDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            err = L"CreateDirectory failed for " + dstDir;
            return false;
        }
        const wchar_t* const kFiles[] = {
            L"onnxruntime.dll", L"onnxruntime-genai.dll", L"NOTICE.md", L"API.md", L"SCHEMA.md", L"WARP - Readme.txt", L"SHA256SUMS.txt",
        };
        const wchar_t* const kDirs[] = { L"models", L"licenses" };

        bool ok = true;
        auto copyFile = [&](const std::wstring& name, bool required)
        {
            const std::wstring s = srcDir + L"\\" + name;
            const DWORD attrs = GetFileAttributesW(s.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) || IsLinkReparsePoint(s, attrs))
            {
                if (required) { ok = false; if (err.empty()) err = L"missing " + s; }
                return;
            }
            const std::wstring d = dstDir + L"\\" + name;
            if (!CopyFileW(s.c_str(), d.c_str(), FALSE))
            {
                SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!CopyFileW(s.c_str(), d.c_str(), FALSE))
                {
                    ok = false;
                    if (err.empty()) err = L"CopyFile failed for " + d;
                }
            }
        };
        copyFile(exeName, true);
        for (const wchar_t* f : kFiles) copyFile(f, false);
        for (const wchar_t* dname : kDirs)
        {
            const std::wstring s = srcDir + L"\\" + dname;
            const DWORD attrs = GetFileAttributesW(s.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || IsLinkReparsePoint(s, attrs))
                continue;
            if (!CopyDirRecursive(s, dstDir + L"\\" + dname, err))
                ok = false;
        }
        return ok;
    }

    bool DeleteDirRecursive(const std::wstring& dir, bool scheduleOnFailure)
    {
        // Runs elevated over a user-writable tree: a junction / directory symlink is removed
        // itself and never followed, so a planted link cannot redirect the delete elsewhere.
        const DWORD dirAttrs = GetFileAttributesW(dir.c_str());
        if (dirAttrs != INVALID_FILE_ATTRIBUTES && (dirAttrs & FILE_ATTRIBUTE_REPARSE_POINT))
            return RemoveDirectoryW(dir.c_str()) != FALSE;

        std::wstring pattern = dir + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                    continue;

                std::wstring p = dir + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    DeleteDirRecursive(p, scheduleOnFailure);
                }
                else
                {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                        SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                    if (!DeleteFileW(p.c_str()) && scheduleOnFailure)
                        MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
                }
            }
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }

        if (!RemoveDirectoryW(dir.c_str()))
        {
            if (scheduleOnFailure)
                MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            return false;
        }
        return true;
    }

    // --- Task Scheduler ----------------------------------------------------
    // Creates/updates the highest-privilege logon task for the current user.
    // COM must already be initialised on the calling thread.
    Installer::Result RegisterTask(bool startNow)
    {
        std::wstring sid;
        if (!GetProcessUserSid(sid) || sid.empty())
            return Fail(L"Could not obtain current user SID.");

        const std::wstring exePath   = Installer::InstalledExePath();
        const std::wstring workDir   = Installer::InstallDir();
        const std::wstring shortName = L"WARP Agent - " + sid;

        ComPtr<ITaskService> svc;
        HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_ITaskService, reinterpret_cast<void**>(svc.put()));
        if (FAILED(hr))
            return Fail(L"CoCreateInstance(TaskScheduler) failed (" + HrHex(hr) + L").");

        {
            Var s, u, d, p;   // local machine, current credentials
            hr = svc->Connect(s.v, u.v, d.v, p.v);
            if (FAILED(hr))
                return Fail(L"ITaskService::Connect failed (" + HrHex(hr) + L").");
        }

        ComPtr<ITaskFolder> root;
        {
            BStr b(L"\\");
            hr = svc->GetFolder(b, root.put());
        }
        if (FAILED(hr))
            return Fail(L"GetFolder(root) failed (" + HrHex(hr) + L").");

        ComPtr<ITaskFolder> folder;
        {
            BStr b(L"\\WARP");
            hr = root->GetFolder(b, folder.put());
        }
        if (FAILED(hr))
        {
            folder.reset();
            BStr b(L"\\WARP");
            Var sddl;
            hr = root->CreateFolder(b, sddl.v, folder.put());
            if (FAILED(hr))
                return Fail(L"CreateFolder(\\WARP) failed (" + HrHex(hr) + L").");
        }

        ComPtr<ITaskDefinition> task;
        hr = svc->NewTask(0, task.put());
        if (FAILED(hr))
            return Fail(L"NewTask failed (" + HrHex(hr) + L").");

        {
            ComPtr<IRegistrationInfo> reg;
            if (SUCCEEDED(task->get_RegistrationInfo(reg.put())) && reg.get())
            {
                BStr author(L"WARP");
                reg->put_Author(author);
                BStr desc(L"WARP always-on activity-capture agent.");
                reg->put_Description(desc);
            }
        }

        {
            ComPtr<IPrincipal> prin;
            hr = task->get_Principal(prin.put());
            if (FAILED(hr) || !prin.get())
                return Fail(L"get_Principal failed (" + HrHex(hr) + L").");
            BStr id(L"WARPPrincipal");
            prin->put_Id(id);
            BStr user(sid.c_str());
            prin->put_UserId(user);
            prin->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
            prin->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
        }

        {
            ComPtr<ITaskSettings> set;
            hr = task->get_Settings(set.put());
            if (FAILED(hr) || !set.get())
                return Fail(L"get_Settings failed (" + HrHex(hr) + L").");
            set->put_StartWhenAvailable(VARIANT_TRUE);
            set->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
            set->put_StopIfGoingOnBatteries(VARIANT_FALSE);
            {
                BStr lim(L"PT0S");
                set->put_ExecutionTimeLimit(lim);
            }
            set->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
            set->put_AllowHardTerminate(VARIANT_TRUE);
            set->put_Hidden(VARIANT_FALSE);
            set->put_Priority(6);
            set->put_Enabled(VARIANT_TRUE);
            set->put_AllowDemandStart(VARIANT_TRUE);
            set->put_RestartCount(3);
            {
                BStr ri(L"PT1M");
                set->put_RestartInterval(ri);
            }
            ComPtr<IIdleSettings> idle;
            if (SUCCEEDED(set->get_IdleSettings(idle.put())) && idle.get())
            {
                idle->put_StopOnIdleEnd(VARIANT_FALSE);
                idle->put_RestartOnIdle(VARIANT_FALSE);
            }
        }

        {
            ComPtr<ITriggerCollection> triggers;
            hr = task->get_Triggers(triggers.put());
            if (FAILED(hr) || !triggers.get())
                return Fail(L"get_Triggers failed (" + HrHex(hr) + L").");
            ComPtr<ITrigger> trigger;
            hr = triggers->Create(TASK_TRIGGER_LOGON, trigger.put());
            if (FAILED(hr) || !trigger.get())
                return Fail(L"Create(logon trigger) failed (" + HrHex(hr) + L").");
            ComPtr<ILogonTrigger> logon;
            hr = trigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(logon.put()));
            if (FAILED(hr) || !logon.get())
                return Fail(L"QueryInterface(ILogonTrigger) failed (" + HrHex(hr) + L").");
            {
                BStr id(L"WARPLogonTrigger");
                logon->put_Id(id);
            }
            {
                BStr user(sid.c_str());
                logon->put_UserId(user);
            }
            {
                BStr delay(L"PT15S");
                logon->put_Delay(delay);
            }
            logon->put_Enabled(VARIANT_TRUE);
        }

        {
            ComPtr<IActionCollection> actions;
            hr = task->get_Actions(actions.put());
            if (FAILED(hr) || !actions.get())
                return Fail(L"get_Actions failed (" + HrHex(hr) + L").");
            ComPtr<IAction> action;
            hr = actions->Create(TASK_ACTION_EXEC, action.put());
            if (FAILED(hr) || !action.get())
                return Fail(L"Create(exec action) failed (" + HrHex(hr) + L").");
            ComPtr<IExecAction> exec;
            hr = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.put()));
            if (FAILED(hr) || !exec.get())
                return Fail(L"QueryInterface(IExecAction) failed (" + HrHex(hr) + L").");
            {
                BStr pth(exePath.c_str());
                exec->put_Path(pth);
            }
            {
                BStr args(L"--headless");
                exec->put_Arguments(args);
            }
            {
                BStr wd(workDir.c_str());
                exec->put_WorkingDirectory(wd);
            }
        }

        ComPtr<IRegisteredTask> registered;
        {
            BStr name(shortName.c_str());
            Var userIdV, pwdV, sddlV;   // VT_EMPTY -> use principal for interactive token
            hr = folder->RegisterTaskDefinition(
                name, task.get(), TASK_CREATE_OR_UPDATE,
                userIdV.v, pwdV.v, TASK_LOGON_INTERACTIVE_TOKEN, sddlV.v,
                registered.put());
        }
        if (FAILED(hr))
            return Fail(L"RegisterTaskDefinition failed (" + HrHex(hr) + L").");

        if (startNow && registered.get())
        {
            Var params;
            ComPtr<IRunningTask> running;
            HRESULT hrRun = registered->Run(params.v, running.put());
            if (FAILED(hrRun))
                return Ok(L"Task registered; immediate start failed (" + HrHex(hrRun) + L").");
        }

        return Ok(L"WARP installed and scheduled (" + Installer::TaskName() + L").");
    }

    HRESULT DeleteTaskImpl()
    {
        const std::wstring shortName = ShortTaskName();

        ComPtr<ITaskService> svc;
        HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_ITaskService, reinterpret_cast<void**>(svc.put()));
        if (FAILED(hr))
            return hr;
        {
            Var a, b, c, d;
            hr = svc->Connect(a.v, b.v, c.v, d.v);
            if (FAILED(hr))
                return hr;
        }

        ComPtr<ITaskFolder> folder;
        {
            BStr p(L"\\WARP");
            hr = svc->GetFolder(p, folder.put());
        }
        if (FAILED(hr))
            return hr;   // folder missing -> nothing to delete

        {
            BStr n(shortName.c_str());
            hr = folder->DeleteTask(n, 0);
        }

        // Best-effort: drop the (now hopefully empty) \WARP folder.
        ComPtr<ITaskFolder> root;
        {
            BStr r(L"\\");
            if (SUCCEEDED(svc->GetFolder(r, root.put())) && root.get())
            {
                BStr w(L"WARP");
                root->DeleteFolder(w, 0);
            }
        }
        return hr;
    }

    void SignalAndWaitForShutdown(DWORD totalMs)
    {
        HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, Installer::ShutdownEventName());
        if (!h)
            return;   // no running instance in this session
        SetEvent(h);
        CloseHandle(h);

        const DWORD step = 200;
        DWORD waited = 0;
        while (waited < totalMs)
        {
            Sleep(step);
            waited += step;
            HANDLE h2 = OpenEventW(SYNCHRONIZE, FALSE, Installer::ShutdownEventName());
            if (!h2)
                break;   // last instance closed its handle -> all exited
            CloseHandle(h2);
        }
    }
}

std::wstring Installer::TaskName()
{
    std::wstring sid;
    GetProcessUserSid(sid);
    return L"\\WARP\\WARP Agent - " + sid;
}

const wchar_t* Installer::ShutdownEventName()
{
    return L"Local\\WARP-ShutdownRequest";
}

std::wstring Installer::InstallDir()
{
    std::wstring pf = KnownFolder(FOLDERID_ProgramFiles);
    if (pf.empty())
        return std::wstring();
    return pf + L"\\WARP";
}

std::wstring Installer::InstalledExePath()
{
    std::wstring dir = InstallDir();
    if (dir.empty())
        return std::wstring();
    return dir + L"\\" + ExeName(CurrentExePath());
}

Installer::Result Installer::Install(bool startNow)
{
    std::wstring exePath = CurrentExePath();
    if (exePath.empty())
        return Fail(L"Could not determine the running executable path.");

    std::wstring srcDir = DirOf(exePath);
    std::wstring dstDir = InstallDir();
    if (dstDir.empty())
        return Fail(L"Could not resolve %ProgramFiles%.");

    // 1) Stage files into Program Files unless we already run from there.
    if (_wcsicmp(srcDir.c_str(), dstDir.c_str()) != 0)
    {
        // An upgrade replaces files the running agent has open: stop it first (the task restarts it).
        SignalAndWaitForShutdown(10000);
        std::wstring err;
        const bool copied = CopyPayload(srcDir, dstDir, ExeName(exePath), err);
        std::wstring destExe = dstDir + L"\\" + ExeName(exePath);
        if (!copied || !PathExists(destExe))
        {
            // Never register a task over a half-replaced (mixed-version) installation.
            std::wstring m = L"Failed to copy application into " + dstDir;
            if (!err.empty())
                m += L" (" + err + L")";
            return Fail(m);
        }
    }

    // 2) Register the scheduled task (COM).
    ComInit co;
    if (co.hr != RPC_E_CHANGED_MODE && FAILED(co.hr))
        return Fail(L"CoInitializeEx failed (" + HrHex(co.hr) + L").");

    return RegisterTask(startNow);
}

Installer::Result Installer::Uninstall(bool removeFiles, bool removeData)
{
    // 1) Ask running instances in this session to exit, then wait briefly.
    SignalAndWaitForShutdown(10000);

    // 2) Remove the scheduled task.
    std::wstring note;
    {
        ComInit co;
        if (co.hr != RPC_E_CHANGED_MODE && FAILED(co.hr))
            return Fail(L"CoInitializeEx failed (" + HrHex(co.hr) + L").");
        HRESULT hr = DeleteTaskImpl();
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            note = L" (task delete returned " + HrHex(hr) + L")";
    }

    // 3) Remove installed binaries (scheduling reboot-deletion for locked ones).
    if (removeFiles)
    {
        std::wstring d = InstallDir();
        if (!d.empty())
            DeleteDirRecursive(d, true);
    }

    // 4) Remove per-user data (%LOCALAPPDATA%\WARP).
    if (removeData)
    {
        std::wstring la = KnownFolder(FOLDERID_LocalAppData);
        if (!la.empty())
            DeleteDirRecursive(la + L"\\WARP", true);
    }

    return Ok(L"WARP uninstalled." + note);
}

bool Installer::IsTaskInstalled()
{
    ComInit co;
    if (co.hr != RPC_E_CHANGED_MODE && FAILED(co.hr))
        return false;

    std::wstring sid;
    if (!GetProcessUserSid(sid))
        return false;
    std::wstring shortName = L"WARP Agent - " + sid;

    ComPtr<ITaskService> svc;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITaskService, reinterpret_cast<void**>(svc.put()))))
        return false;
    {
        Var a, b, c, d;
        if (FAILED(svc->Connect(a.v, b.v, c.v, d.v)))
            return false;
    }
    ComPtr<ITaskFolder> folder;
    {
        BStr p(L"\\WARP");
        if (FAILED(svc->GetFolder(p, folder.put())))
            return false;
    }
    ComPtr<IRegisteredTask> task;
    {
        BStr n(shortName.c_str());
        if (FAILED(folder->GetTask(n, task.put())))
            return false;
    }
    return task.get() != nullptr;
}

Installer::Result Installer::StartTask()
{
    ComInit co;
    if (co.hr != RPC_E_CHANGED_MODE && FAILED(co.hr))
        return Fail(L"CoInitializeEx failed (" + HrHex(co.hr) + L").");

    std::wstring sid;
    if (!GetProcessUserSid(sid))
        return Fail(L"Could not obtain current user SID.");
    std::wstring shortName = L"WARP Agent - " + sid;

    ComPtr<ITaskService> svc;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(svc.put()));
    if (FAILED(hr))
        return Fail(L"CoCreateInstance(TaskScheduler) failed (" + HrHex(hr) + L").");
    {
        Var a, b, c, d;
        hr = svc->Connect(a.v, b.v, c.v, d.v);
        if (FAILED(hr))
            return Fail(L"ITaskService::Connect failed (" + HrHex(hr) + L").");
    }
    ComPtr<ITaskFolder> folder;
    {
        BStr p(L"\\WARP");
        hr = svc->GetFolder(p, folder.put());
    }
    if (FAILED(hr))
        return Fail(L"WARP task is not installed.");
    ComPtr<IRegisteredTask> task;
    {
        BStr n(shortName.c_str());
        hr = folder->GetTask(n, task.put());
    }
    if (FAILED(hr) || !task.get())
        return Fail(L"WARP task is not installed.");

    Var params;
    ComPtr<IRunningTask> running;
    hr = task->Run(params.v, running.put());
    if (FAILED(hr))
        return Fail(L"IRegisteredTask::Run failed (" + HrHex(hr) + L").");
    return Ok(L"WARP task started.");
}
