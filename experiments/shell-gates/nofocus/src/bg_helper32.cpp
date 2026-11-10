// bg_helper32.exe - the 32-bit half of civ_bg.py (Python here is 64-bit, the game is 32-bit).
//
//   bg_helper32.exe inject <pid> <dll>
//       LoadLibraryW(<dll>) inside a running 32-bit process (CreateRemoteThread). Prints "OK <hmodule>".
//   bg_helper32.exe launch [--show N] [--desktop NAME] [--cwd DIR] [--inject DLL] [--suspended]
//                          [--detached] -- <exe> [args...]
//       CreateProcessW with STARTUPINFO.wShowWindow = N (STARTF_USESHOWWINDOW) and lpDesktop = NAME.
//       --inject: load DLL into the child. With --suspended the child is created suspended, the DLL is
//       loaded by a remote thread (which also runs the loader's process initialisation), and only then
//       is the main thread resumed - so the guard is in place before the first window exists.
//       Without --suspended the DLL is injected right after the child starts running.
//       Prints "PID <pid>".
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static int inject(HANDLE proc, const wchar_t* dll) {
    wchar_t full[MAX_PATH];
    GetFullPathNameW(dll, MAX_PATH, full, 0);
    SIZE_T bytes = (wcslen(full) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, 0, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { fprintf(stderr, "VirtualAllocEx failed %lu\n", GetLastError()); return 0; }
    WriteProcessMemory(proc, remote, full, bytes, 0);
    // kernel32 sits at the same address in every 32-bit process of this boot.
    LPTHREAD_START_ROUTINE ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = CreateRemoteThread(proc, 0, 0, ll, remote, 0, 0);
    if (!t) { fprintf(stderr, "CreateRemoteThread failed %lu\n", GetLastError()); return 0; }
    DWORD w = WaitForSingleObject(t, 15000);
    DWORD code = 0; GetExitCodeThread(t, &code);
    CloseHandle(t);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    if (w != WAIT_OBJECT_0) { fprintf(stderr, "remote LoadLibraryW timed out\n"); return 0; }
    if (!code) { fprintf(stderr, "remote LoadLibraryW returned NULL\n"); return 0; }
    printf("OK hmodule=0x%08lx\n", code);
    return 1;
}

static void quoteArg(wchar_t* cmd, size_t cap, const wchar_t* a) {
    bool q = !*a || wcspbrk(a, L" \t\"");
    if (q) wcsncat(cmd, L"\"", cap - wcslen(cmd) - 1);
    wcsncat(cmd, a, cap - wcslen(cmd) - 1);
    if (q) wcsncat(cmd, L"\"", cap - wcslen(cmd) - 1);
    wcsncat(cmd, L" ", cap - wcslen(cmd) - 1);
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && !wcscmp(argv[1], L"inject")) {
        DWORD pid = wcstoul(argv[2], 0, 10);
        HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                   PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
        if (!p) { fprintf(stderr, "OpenProcess(%lu) failed %lu\n", pid, GetLastError()); return 2; }
        int ok = inject(p, argv[3]);
        CloseHandle(p);
        return ok ? 0 : 3;
    }
    if (argc >= 3 && !wcscmp(argv[1], L"launch")) {
        int show = -1; const wchar_t* desktop = 0; const wchar_t* cwd = 0; const wchar_t* dll = 0;
        bool suspended = false, detached = false;
        int i = 2;
        for (; i < argc; i++) {
            if (!wcscmp(argv[i], L"--")) { i++; break; }
            if (!wcscmp(argv[i], L"--show") && i + 1 < argc) show = _wtoi(argv[++i]);
            else if (!wcscmp(argv[i], L"--desktop") && i + 1 < argc) desktop = argv[++i];
            else if (!wcscmp(argv[i], L"--cwd") && i + 1 < argc) cwd = argv[++i];
            else if (!wcscmp(argv[i], L"--inject") && i + 1 < argc) dll = argv[++i];
            else if (!wcscmp(argv[i], L"--suspended")) suspended = true;
            else if (!wcscmp(argv[i], L"--detached")) detached = true;
            else { fprintf(stderr, "unknown option %ls\n", argv[i]); return 1; }
        }
        if (i >= argc) { fprintf(stderr, "missing -- <exe>\n"); return 1; }
        static wchar_t cmd[32768]; cmd[0] = 0;
        for (int k = i; k < argc; k++) quoteArg(cmd, 32768, argv[k]);
        STARTUPINFOW si = {sizeof(si)};
        if (show >= 0) { si.dwFlags |= STARTF_USESHOWWINDOW; si.wShowWindow = (WORD)show; }
        if (desktop) si.lpDesktop = (LPWSTR)desktop;
        PROCESS_INFORMATION pi = {};
        DWORD flags = CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS; (void)detached;   // never a console window
        if (dll && suspended) flags |= CREATE_SUSPENDED;
        if (!CreateProcessW(argv[i], cmd, 0, 0, FALSE, flags, 0, cwd, &si, &pi)) {
            fprintf(stderr, "CreateProcessW failed %lu\n", GetLastError());
            return 2;
        }
        printf("PID %lu\n", pi.dwProcessId);
        fflush(stdout);
        int rc = 0;
        if (dll && !inject(pi.hProcess, dll)) rc = 3;
        if (dll && suspended) ResumeThread(pi.hThread);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return rc;
    }
    fprintf(stderr, "usage: bg_helper32 inject <pid> <dll> | launch [opts] -- <exe> [args]\n");
    return 1;
}
