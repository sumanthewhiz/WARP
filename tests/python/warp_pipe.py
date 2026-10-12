"""Minimal WARP named-pipe client (ctypes only, no pywin32).

Used by the CI integration test (tests/python/test_live_api.py) and handy for manual checks:

    python tests/python/warp_pipe.py '{"op":"GetStatus"}'

The client opens the per-session pipe ``\\\\.\\pipe\\WarpFileActivityAPI-<SessionId>`` with
GENERIC_READ | GENERIC_WRITE (what most consumers request), switches to message mode, writes one
JSON request and reads one JSON response (looping on ERROR_MORE_DATA).
``as_standard_user=True`` performs the call while impersonating a filtered, medium-integrity
token of the current user, i.e. what a normal, non-elevated app gets (gap analysis API-1).
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import os
import sys
import time

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
PIPE_READMODE_MESSAGE = 0x2
ERROR_MORE_DATA = 234
ERROR_PIPE_BUSY = 231
ERROR_FILE_NOT_FOUND = 2

kernel32.CreateFileW.restype = wt.HANDLE
kernel32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.DWORD, wt.DWORD, wt.HANDLE]
kernel32.SetNamedPipeHandleState.restype = wt.BOOL
kernel32.SetNamedPipeHandleState.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD), wt.LPVOID, wt.LPVOID]
kernel32.WriteFile.restype = wt.BOOL
kernel32.WriteFile.argtypes = [wt.HANDLE, wt.LPCVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
kernel32.ReadFile.restype = wt.BOOL
kernel32.ReadFile.argtypes = [wt.HANDLE, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD), wt.LPVOID]
kernel32.WaitNamedPipeW.restype = wt.BOOL
kernel32.WaitNamedPipeW.argtypes = [wt.LPCWSTR, wt.DWORD]
kernel32.CloseHandle.restype = wt.BOOL
kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.ProcessIdToSessionId.restype = wt.BOOL
kernel32.ProcessIdToSessionId.argtypes = [wt.DWORD, ctypes.POINTER(wt.DWORD)]
kernel32.GetNamedPipeServerProcessId.restype = wt.BOOL
kernel32.GetNamedPipeServerProcessId.argtypes = [wt.HANDLE, ctypes.POINTER(wt.ULONG)]
kernel32.LocalFree.restype = wt.HLOCAL
kernel32.LocalFree.argtypes = [wt.HLOCAL]

advapi32.SaferCreateLevel.restype = wt.BOOL
advapi32.SaferCreateLevel.argtypes = [wt.DWORD, wt.DWORD, wt.DWORD, ctypes.POINTER(wt.HANDLE), wt.LPVOID]
advapi32.SaferComputeTokenFromLevel.restype = wt.BOOL
advapi32.SaferComputeTokenFromLevel.argtypes = [wt.HANDLE, wt.HANDLE, ctypes.POINTER(wt.HANDLE), wt.DWORD, wt.LPVOID]
advapi32.SaferCloseLevel.restype = wt.BOOL
advapi32.SaferCloseLevel.argtypes = [wt.HANDLE]
advapi32.ConvertStringSidToSidW.restype = wt.BOOL
advapi32.ConvertStringSidToSidW.argtypes = [wt.LPCWSTR, ctypes.POINTER(ctypes.c_void_p)]
advapi32.GetLengthSid.restype = wt.DWORD
advapi32.GetLengthSid.argtypes = [ctypes.c_void_p]
advapi32.SetTokenInformation.restype = wt.BOOL
advapi32.SetTokenInformation.argtypes = [wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD]
advapi32.ImpersonateLoggedOnUser.restype = wt.BOOL
advapi32.ImpersonateLoggedOnUser.argtypes = [wt.HANDLE]
advapi32.RevertToSelf.restype = wt.BOOL
advapi32.RevertToSelf.argtypes = []


class PipeError(OSError):
    pass


def session_pipe_name(pid: int | None = None) -> str:
    sid = wt.DWORD(0)
    kernel32.ProcessIdToSessionId(pid or os.getpid(), ctypes.byref(sid))
    return r"\\.\pipe\WarpFileActivityAPI-%d" % sid.value


def wait_for_pipe(name: str | None = None, timeout_s: float = 60.0) -> bool:
    name = name or session_pipe_name()
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if kernel32.WaitNamedPipeW(name, 1000):
            return True
        time.sleep(0.5)
    return False


class _SID_AND_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Sid", ctypes.c_void_p), ("Attributes", wt.DWORD)]


class _TOKEN_MANDATORY_LABEL(ctypes.Structure):
    _fields_ = [("Label", _SID_AND_ATTRIBUTES)]


def _standard_user_token() -> wt.HANDLE:
    """Admin-filtered (Safer "normal user") token of the current user at medium integrity."""
    SAFER_SCOPEID_USER, SAFER_LEVELID_NORMALUSER, SAFER_LEVEL_OPEN = 2, 0x20000, 1
    level = wt.HANDLE()
    if not advapi32.SaferCreateLevel(SAFER_SCOPEID_USER, SAFER_LEVELID_NORMALUSER, SAFER_LEVEL_OPEN, ctypes.byref(level), None):
        raise ctypes.WinError(ctypes.get_last_error())
    token = wt.HANDLE()
    try:
        if not advapi32.SaferComputeTokenFromLevel(level, None, ctypes.byref(token), 0, None):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        advapi32.SaferCloseLevel(level)
    sid = ctypes.c_void_p()
    if not advapi32.ConvertStringSidToSidW("S-1-16-8192", ctypes.byref(sid)):     # medium mandatory level
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        SE_GROUP_INTEGRITY, TOKEN_INTEGRITY_LEVEL = 0x20, 25
        tml = _TOKEN_MANDATORY_LABEL(_SID_AND_ATTRIBUTES(sid, SE_GROUP_INTEGRITY))
        size = ctypes.sizeof(tml) + advapi32.GetLengthSid(sid)
        if not advapi32.SetTokenInformation(token, TOKEN_INTEGRITY_LEVEL, ctypes.byref(tml), size):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel32.LocalFree(sid)
    return token


def _open(name: str, timeout_s: float) -> wt.HANDLE:
    deadline = time.monotonic() + timeout_s
    while True:
        h = kernel32.CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None)
        if h and h != INVALID_HANDLE_VALUE:
            return h
        err = ctypes.get_last_error()
        if err not in (ERROR_PIPE_BUSY, ERROR_FILE_NOT_FOUND) or time.monotonic() > deadline:
            raise PipeError(err, "cannot open %s: %s" % (name, ctypes.FormatError(err)))
        kernel32.WaitNamedPipeW(name, 1000)


def call(request: dict | str, *, name: str | None = None, timeout_s: float = 30.0,
         as_standard_user: bool = False, server_pid_out: list | None = None) -> dict:
    """Sends one request and returns the parsed JSON response."""
    name = name or session_pipe_name()
    payload = (request if isinstance(request, str) else json.dumps(request)).encode("utf-8")
    token = _standard_user_token() if as_standard_user else None
    if token is not None and not advapi32.ImpersonateLoggedOnUser(token):
        err = ctypes.get_last_error()
        kernel32.CloseHandle(token)
        raise ctypes.WinError(err)
    try:
        h = _open(name, timeout_s)
    finally:
        if token is not None:
            advapi32.RevertToSelf()
            kernel32.CloseHandle(token)
    try:
        if server_pid_out is not None:
            pid = wt.ULONG(0)
            kernel32.GetNamedPipeServerProcessId(h, ctypes.byref(pid))
            server_pid_out.append(pid.value)
        mode = wt.DWORD(PIPE_READMODE_MESSAGE)
        if not kernel32.SetNamedPipeHandleState(h, ctypes.byref(mode), None, None):
            raise PipeError(ctypes.get_last_error(), "SetNamedPipeHandleState failed")
        written = wt.DWORD(0)
        if not kernel32.WriteFile(h, payload, len(payload), ctypes.byref(written), None):
            raise PipeError(ctypes.get_last_error(), "WriteFile failed")
        chunks = []
        buf = ctypes.create_string_buffer(65536)
        while True:
            got = wt.DWORD(0)
            ok = kernel32.ReadFile(h, buf, len(buf), ctypes.byref(got), None)
            chunks.append(buf.raw[: got.value])
            if ok:
                break
            err = ctypes.get_last_error()
            if err != ERROR_MORE_DATA:
                raise PipeError(err, "ReadFile failed: %s" % ctypes.FormatError(err))
        return json.loads(b"".join(chunks).decode("utf-8"))
    finally:
        kernel32.CloseHandle(h)


if __name__ == "__main__":
    req = sys.argv[1] if len(sys.argv) > 1 else '{"op":"Ping"}'
    print(json.dumps(call(req), indent=2, ensure_ascii=False))
