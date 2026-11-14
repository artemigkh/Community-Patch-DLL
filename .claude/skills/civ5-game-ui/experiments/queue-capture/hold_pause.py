"""Hold the DLL's external-pause mutex ("TurnByTurn") until <stopfile> appears, then release it properly."""
import ctypes, os, sys, time
K = ctypes.windll.kernel32
h = K.CreateMutexW(None, True, "TurnByTurn")
print("holding TurnByTurn, handle", h, "last error", K.GetLastError(), flush=True)
stop = sys.argv[1]
deadline = time.time() + float(sys.argv[2]) if len(sys.argv) > 2 else None
while not os.path.exists(stop) and (deadline is None or time.time() < deadline):
    time.sleep(0.5)
K.ReleaseMutex(h); K.CloseHandle(h)
print("released", flush=True)
