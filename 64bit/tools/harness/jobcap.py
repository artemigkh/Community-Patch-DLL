"""jobcap: run a headless process inside a Windows job object with a per-process memory limit (ticket 0488).

    python -I tools/harness/jobcap.py [--cap-gb N | --cap-mb N] [--grace S] -- COMMAND [ARG ...]

    import jobcap                                    # tools/harness on sys.path
    p = jobcap.popen(cmd, cap_bytes=None, **popen_kwargs)   # a subprocess.Popen; p.jobcap is its Cap
    cp = jobcap.run(cmd, cap_bytes=None, timeout=None, **popen_kwargs)   # as subprocess.run (no capture options)

Why: on 2026-11-17 a headless server.exe whose load read a map size from the wrong bytes asked for 115 GB and reached
131 GB private on the workstation (ticket 0488; research/03-dll-engine-boundary/server-host.md section 18). A job object
with JOB_OBJECT_LIMIT_PROCESS_MEMORY makes every commit of a process in the job past the cap fail, so the machine's commit
is never pressed; the process sees an allocation failure (C++ bad_alloc, NULL from malloc). Windows then posts
JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT; jobcap gives the process `grace` seconds (default 10) to end by itself, which lets
server.exe write its report and exit 2, and ends it after that with exit code CAP_EXIT_CODE (0xC0000017, STATUS_NO_MEMORY).
The cap is per process and covers every process the command starts (children join the job). A game EXE
(CivilizationV*.exe) is never ended by jobcap (AGENTS.md section 4: no kill while it may autosave): its allocations still
fail at the cap and the event is logged. Use jobcap for headless processes: server.exe, test hosts, fake programs.

The child starts suspended, joins the job, then runs: no instruction of it runs outside the cap. If the job cannot be
created or the child cannot join it, the child is ended before it ran and OSError is raised: never an uncapped run.

The cap: `cap_bytes` (or --cap-gb / --cap-mb), else the environment variable C5_JOBCAP_GB (a number of GB), else 16 GB.
A cap of 0 runs without a job (a line says so); use it only to reproduce a runaway on a machine where that is safe.
Exit code of the command line: the command's. Python 3.8, standard library only (the testbed hosts run it too).
"""
import ctypes
import os
import subprocess
import sys
import threading
import time
from ctypes import wintypes

DEFAULT_CAP_GB = 16.0
CAP_EXIT_CODE = 0xC0000017  # STATUS_NO_MEMORY: what a process ended by jobcap exits with
DEFAULT_GRACE_S = 10.0
NEVER_END = ("civilizationv",)  # image name prefixes jobcap never ends (the game EXE)

_CREATE_SUSPENDED = 0x00000004
_JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x00000100
_JobObjectAssociateCompletionPortInformation = 7
_JobObjectExtendedLimitInformation = 9
_MSG_PROCESS_MEMORY_LIMIT = 9  # JOB_OBJECT_MSG_PROCESS_MEMORY_LIMIT; lpOverlapped is the process id
_PROCESS_TERMINATE, _SYNCHRONIZE, _PROCESS_QUERY_LIMITED_INFORMATION = 0x0001, 0x00100000, 0x1000
_WAIT_OBJECT_0, _INFINITE = 0, 0xFFFFFFFF
_INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class _IO_COUNTERS(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ulonglong) for n in ("ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                                                    "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]


class _BASIC_LIMIT(ctypes.Structure):
    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong),
                ("LimitFlags", wintypes.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t), ("PriorityClass", wintypes.DWORD), ("SchedulingClass", wintypes.DWORD)]


class _EXTENDED_LIMIT(ctypes.Structure):
    _fields_ = [("BasicLimitInformation", _BASIC_LIMIT), ("IoInfo", _IO_COUNTERS),
                ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]


class _ASSOC_PORT(ctypes.Structure):
    _fields_ = [("CompletionKey", ctypes.c_void_p), ("CompletionPort", wintypes.HANDLE)]


def _k32():
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateJobObjectW.restype = wintypes.HANDLE
    k.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    k.SetInformationJobObject.restype = wintypes.BOOL
    k.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    k.QueryInformationJobObject.restype = wintypes.BOOL
    k.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD,
                                            ctypes.c_void_p]
    k.AssignProcessToJobObject.restype = wintypes.BOOL
    k.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    k.CreateIoCompletionPort.restype = wintypes.HANDLE
    k.CreateIoCompletionPort.argtypes = [wintypes.HANDLE, wintypes.HANDLE, ctypes.c_size_t, wintypes.DWORD]
    k.GetQueuedCompletionStatus.restype = wintypes.BOOL
    k.GetQueuedCompletionStatus.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD),
                                            ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_void_p),
                                            wintypes.DWORD]
    k.PostQueuedCompletionStatus.restype = wintypes.BOOL
    k.PostQueuedCompletionStatus.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_size_t, ctypes.c_void_p]
    k.OpenProcess.restype = wintypes.HANDLE
    k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k.TerminateProcess.restype = wintypes.BOOL
    k.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    k.WaitForSingleObject.restype = wintypes.DWORD
    k.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    k.QueryFullProcessImageNameW.restype = wintypes.BOOL
    k.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                             ctypes.POINTER(wintypes.DWORD)]
    k.CloseHandle.restype = wintypes.BOOL
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    return k


def default_cap_bytes():
    """C5_JOBCAP_GB (GB, a number; 0 = no cap) or 16 GB."""
    v = os.environ.get("C5_JOBCAP_GB", "").strip()
    gb = float(v) if v else DEFAULT_CAP_GB
    if gb < 0:
        raise ValueError("C5_JOBCAP_GB must be 0 or more, not %r" % v)
    return int(gb * (1 << 30))


def describe(cap_bytes):
    return "no cap" if not cap_bytes else "%.0f MB per process" % (cap_bytes / 1048576.0)


class Cap:
    """One job object: the limit, the watcher thread, and what happened (events: dicts with pid, image, action)."""

    def __init__(self, cap_bytes, grace_s=DEFAULT_GRACE_S, log=None):
        self.cap_bytes = int(cap_bytes)
        self.grace_s = float(grace_s)
        self.log = log or (lambda m: print(m, file=sys.stderr, flush=True))
        self.events = []
        self._seen = set()
        self._lock = threading.Lock()
        self._k = _k32()
        self.job = self._k.CreateJobObjectW(None, None)
        if not self.job:
            raise OSError(ctypes.get_last_error(), "jobcap: CreateJobObjectW failed")
        info = _EXTENDED_LIMIT()
        info.BasicLimitInformation.LimitFlags = _JOB_OBJECT_LIMIT_PROCESS_MEMORY
        info.ProcessMemoryLimit = self.cap_bytes
        if not self._k.SetInformationJobObject(self.job, _JobObjectExtendedLimitInformation, ctypes.byref(info),
                                               ctypes.sizeof(info)):
            err = ctypes.get_last_error()
            self._k.CloseHandle(self.job)
            raise OSError(err, "jobcap: SetInformationJobObject (memory limit) failed")
        self.port = self._k.CreateIoCompletionPort(_INVALID_HANDLE_VALUE, None, 0, 1)
        assoc = _ASSOC_PORT(ctypes.c_void_p(1), self.port)
        if not self.port or not self._k.SetInformationJobObject(
                self.job, _JobObjectAssociateCompletionPortInformation, ctypes.byref(assoc), ctypes.sizeof(assoc)):
            err = ctypes.get_last_error()
            self._k.CloseHandle(self.job)
            raise OSError(err, "jobcap: the job's completion port could not be set")
        self._thread = threading.Thread(target=self._watch, name="jobcap-watch", daemon=True)
        self._thread.start()

    def assign(self, handle):
        if not self._k.AssignProcessToJobObject(self.job, int(handle)):
            raise OSError(ctypes.get_last_error(), "jobcap: AssignProcessToJobObject failed")

    def peak_process_bytes(self):
        info = _EXTENDED_LIMIT()
        if self._k.QueryInformationJobObject(self.job, _JobObjectExtendedLimitInformation, ctypes.byref(info),
                                             ctypes.sizeof(info), None):
            return int(info.PeakProcessMemoryUsed)
        return -1

    def _image(self, h):
        buf = ctypes.create_unicode_buffer(1024)
        n = wintypes.DWORD(len(buf))
        if h and self._k.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)):
            return os.path.basename(buf.value)
        return "?"

    def _end_after_grace(self, pid, h, image):
        r = self._k.WaitForSingleObject(h, int(self.grace_s * 1000))
        if r == _WAIT_OBJECT_0:
            action = "ended by itself within the grace of %.0f s" % self.grace_s
        elif self._k.TerminateProcess(h, CAP_EXIT_CODE):
            action = "ended by jobcap after the grace of %.0f s (exit 0x%08X)" % (self.grace_s, CAP_EXIT_CODE)
        else:
            action = "could NOT be ended (TerminateProcess error %d)" % ctypes.get_last_error()
        self._k.CloseHandle(h)
        self._note(pid, image, action)

    def _note(self, pid, image, action):
        with self._lock:
            self.events.append({"pid": pid, "image": image, "action": action, "capMB": self.cap_bytes >> 20,
                                "time": time.strftime("%Y-%m-%d %H:%M:%S")})
        self.log("jobcap: pid %d (%s) reached the cap of %s: %s" % (pid, image, describe(self.cap_bytes), action))

    def _watch(self):
        msg, key, ov = wintypes.DWORD(), ctypes.c_size_t(), ctypes.c_void_p()
        while True:
            if not self._k.GetQueuedCompletionStatus(self.port, ctypes.byref(msg), ctypes.byref(key),
                                                     ctypes.byref(ov), _INFINITE):
                return
            if key.value == 0:  # close()
                return
            if msg.value == _MSG_PROCESS_MEMORY_LIMIT:
                pid = int(ov.value or 0)
                if pid in self._seen:  # Windows may post the message again for each refused commit
                    continue
                self._seen.add(pid)
                h = self._k.OpenProcess(_PROCESS_TERMINATE | _SYNCHRONIZE | _PROCESS_QUERY_LIMITED_INFORMATION,
                                        False, pid)
                image = self._image(h)
                if not h:
                    self._note(pid, image, "already gone")
                elif image.lower().startswith(NEVER_END):
                    self._k.CloseHandle(h)
                    self._note(pid, image, "a game EXE: not ended (its allocations past the cap fail)")
                else:
                    threading.Thread(target=self._end_after_grace, args=(pid, h, image), daemon=True).start()
            # other messages (new process, exit, active process zero): nothing to do; the watcher stays until close()

    def close(self):
        if self.port:
            self._k.PostQueuedCompletionStatus(self.port, 0, 0, None)
            self._thread.join(5)
            self._k.CloseHandle(self.port)
            self.port = None
        if self.job:
            self._k.CloseHandle(self.job)
            self.job = None


def _resume(handle):
    ntdll = ctypes.WinDLL("ntdll")
    ntdll.NtResumeProcess.restype = ctypes.c_long
    ntdll.NtResumeProcess.argtypes = [wintypes.HANDLE]
    st = ntdll.NtResumeProcess(int(handle))
    if st != 0:
        raise OSError(st, "jobcap: NtResumeProcess failed (NTSTATUS 0x%08X)" % (st & 0xFFFFFFFF))


def popen(cmd, cap_bytes=None, grace_s=DEFAULT_GRACE_S, log=None, **kw):
    """subprocess.Popen(cmd, **kw) inside a new job with a per-process cap (see the module text). The returned Popen
    has .jobcap (the Cap, None without a cap); call p.jobcap.close() after p has ended, or leave it to wait_close()."""
    say = log or (lambda m: print(m, file=sys.stderr, flush=True))
    cap = default_cap_bytes() if cap_bytes is None else int(cap_bytes)
    if not cap:
        say("jobcap: NO memory cap for %s (cap 0)" % os.path.basename(str(cmd[0])))
        p = subprocess.Popen(cmd, **kw)
        p.jobcap = None
        return p
    c = Cap(cap, grace_s, say)
    kw["creationflags"] = kw.get("creationflags", 0) | _CREATE_SUSPENDED
    try:
        p = subprocess.Popen(cmd, **kw)
    except Exception:
        c.close()
        raise
    try:
        c.assign(p._handle)
        _resume(p._handle)
    except OSError:
        c._k.TerminateProcess(int(p._handle), CAP_EXIT_CODE)  # suspended since its creation: it never ran
        p.wait()
        c.close()
        raise
    p.jobcap = c
    return p


def wait_close(p, timeout=None):
    """p.wait(timeout), then the job's handles closed (also when the wait times out and raises)."""
    try:
        return p.wait(timeout)
    finally:
        if p.poll() is not None and getattr(p, "jobcap", None):
            p.jobcap.close()


def run(cmd, cap_bytes=None, timeout=None, grace_s=DEFAULT_GRACE_S, log=None, **kw):
    """subprocess.run(cmd, timeout=timeout, **kw) inside a capped job (no capture_output/input: pass stdout= files).
    On a timeout the process is killed (as subprocess.run does) and TimeoutExpired is raised. Returns a
    CompletedProcess whose .jobcap_events lists what the cap did."""
    p = popen(cmd, cap_bytes, grace_s, log, **kw)
    try:
        rc = p.wait(timeout)
    except subprocess.TimeoutExpired:
        p.kill()  # a headless process (the module text): subprocess.run's own behaviour on a timeout
        p.wait()
        raise
    finally:
        if p.poll() is not None and p.jobcap:
            time.sleep(0.2)  # let the watcher record a cap event that came just before the end
            p.jobcap.close()
    cp = subprocess.CompletedProcess(cmd, rc)
    cp.jobcap_events = list(p.jobcap.events) if p.jobcap else []
    return cp


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--" not in argv:
        print(__doc__.split("\n\n")[0], file=sys.stderr)
        return 2
    i = argv.index("--")
    opts, cmd = argv[:i], argv[i + 1:]
    cap, grace = None, DEFAULT_GRACE_S
    while opts:
        o = opts.pop(0)
        if o in ("--cap-gb", "--cap-mb") and opts:
            cap = int(float(opts.pop(0)) * (1 << (30 if o == "--cap-gb" else 20)))
        elif o == "--grace" and opts:
            grace = float(opts.pop(0))
        else:
            print("jobcap: unknown option %s" % o, file=sys.stderr)
            return 2
    if not cmd:
        print("jobcap: no command after --", file=sys.stderr)
        return 2
    cap = default_cap_bytes() if cap is None else cap
    print("jobcap: %s, grace %.0f s: %s" % (describe(cap), grace, subprocess.list2cmdline(cmd)), file=sys.stderr,
          flush=True)
    cp = run(cmd, cap, None, grace)
    print("jobcap: exit %d (0x%08X); cap events: %d" % (cp.returncode, cp.returncode & 0xFFFFFFFF,
                                                      len(cp.jobcap_events)), file=sys.stderr, flush=True)
    rc = cp.returncode & 0xFFFFFFFF
    return rc - (1 << 32) if rc > 0x7FFFFFFF else rc  # sys.exit takes a C long: 0xC0000017 passes as its signed form


if __name__ == "__main__":
    sys.exit(main())
