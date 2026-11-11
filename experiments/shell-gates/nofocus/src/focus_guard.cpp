// focus_guard.dll - an in-process focus guard for a 32-bit game window (Civ 5).
//
// Loaded into the game (injected by bg_helper32.exe, or the same code compiled into the game DLL with
// FOCUS_GUARD_NO_DLLMAIN and a call to FocusGuardInstall()), it patches the process' own copy of
// user32 so that the game cannot take the keyboard focus or cover other windows while it is in the
// background, and so that an outside driver can make it see keys as held:
//
//   SetForegroundWindow (any window: Civ 5's only call is its second-instance check, which raises
//   the FIRST instance's "WinClass_FXS" window) / BringWindowToTop / SwitchToThisWindow (own)
//                                                                   -> no-op, TRUE
//   SetWindowsHookExA/W(WH_KEYBOARD_LL)                             -> not installed (Civ 5 installs a
//                                                                      global one that swallows Alt+Enter
//                                                                      in EVERY app, and a stalled game
//                                                                      thread would lag Art's keyboard)
//   ShowWindow(own top-level, activating cmd) while not foreground -> the non-activating twin
//   SetWindowPos(own top-level) while not foreground                -> + SWP_NOACTIVATE (+ SWP_NOZORDER
//                                                                      when it would raise: HWND_TOP,
//                                                                      HWND_TOPMOST or HWND_NOTOPMOST;
//                                                                      Civ 5 re-centres itself with
//                                                                      HWND_NOTOPMOST at each mode set)
//   ClipCursor(rect) while not foreground                           -> no-op, TRUE
//   activation of an own window (WH_CBT HCBT_ACTIVATE) while not foreground -> vetoed, unless the
//                                                                      user just clicked the window or
//                                                                      the taskbar (fresh input + cursor)
//   GetAsyncKeyState / GetKeyState                                  -> real state OR the driver's
//                                                                      virtual "held" keys
//                                                                      (physical keys hidden while in
//                                                                      the background: "isolate")
//   GetActiveWindow / GetForegroundWindow                           -> the main window, only while the
//                                                                      driver sets pretend_active
//   GetCursorPos                                                    -> the driver's virtual cursor,
//                                                                      only while it sets vcursor_on
//   DWMWA_CLOAK on the main window                                  -> while the driver sets cloak
//                                                                      (or CIVBG_CLOAK=1 at launch)
// Nothing changes while the game itself is the foreground window (a human playing it normally), and
// the driver can switch the whole guard off at run time (shared memory "enabled").
//
// Patching is inline, in user32's x86 prologues (both shapes seen on Windows 11 26100 WOW64:
// "mov edi,edi; push ebp; mov ebp,esp" and "jmp dword ptr [win32u thunk]"), so it also covers calls
// made through GetProcAddress and from any module, and it never writes to the game's own EXE image
// (which is Steam CEG-protected).
//
// Shared memory: "Local\CivBgGuard_<pid>", layout GuardShm below (civ_bg.py mirrors it).
// Log: %TEMP%\civbg_guard_<pid>.log
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>

enum {
    H_SETFOREGROUND, H_BRINGTOTOP, H_SWITCHTO, H_SHOWWINDOW, H_SETWINDOWPOS, H_CLIPCURSOR,
    H_GETASYNCKEY, H_GETKEY, H_GETACTIVE, H_GETFOREGROUND, H_SETACTIVE, H_SETFOCUS, H_ACTIVATE_VETO,
    H_GETCURSORPOS, H_LLHOOK, H_COUNT
};
// Counter slots (calls[] / altered[] below), in this order: SetForegroundWindow BringWindowToTop
// SwitchToThisWindow ShowWindow SetWindowPos ClipCursor GetAsyncKeyState GetKeyState GetActiveWindow
// GetForegroundWindow SetActiveWindow SetFocus ActivateVeto GetCursorPos SetWindowsHookEx(LL)
// (bgwin.GUARD_HOOKS mirrors it).

#pragma pack(push, 4)
struct GuardShm {
    DWORD magic;                  // 'CBG1'
    DWORD version;                // 1
    volatile LONG enabled;        // 1 = guard active (default 1)
    volatile LONG pretend_active; // 1 = GetActiveWindow/GetForegroundWindow report the main window
    volatile LONG calls[16];      // per hook: calls seen
    volatile LONG altered[16];    // per hook: calls blocked or rewritten
    volatile DWORD main_hwnd;     // last own top-level window shown (driver may overwrite)
    DWORD pid;
    volatile BYTE vk_down[256];   // driver-held virtual keys (non-zero = held)
    char status[512];             // which hooks are installed
    volatile LONG cloak;          // driver's wish: 1 = main window cloaked (DWMWA_CLOAK), 0 = not
    volatile LONG cloak_applied;  // what the watcher last applied (-1 = never), and its HRESULT:
    volatile LONG cloak_hr;
    volatile LONG isolate;        // 1 = while not foreground, the game sees no PHYSICAL keys through
                                  //     GetAsyncKeyState/GetKeyState (Art's typing elsewhere), only
                                  //     the driver's vk_down (default 1)
    volatile LONG vcursor_on;     // 1 = GetCursorPos returns (vcursor_x, vcursor_y), screen coords
    volatile LONG vcursor_x;
    volatile LONG vcursor_y;
};
#pragma pack(pop)

static GuardShm* g_shm;
static GuardShm g_fallbackShm;
static FILE* g_log;
static CRITICAL_SECTION g_logLock;

static void glog(const char* fmt, ...) {
    if (!g_log) return;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    SYSTEMTIME st; GetLocalTime(&st);
    EnterCriticalSection(&g_logLock);
    fprintf(g_log, "%02d:%02d:%02d.%03d tid=%lu %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentThreadId(), buf);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

// --- originals --------------------------------------------------------------------------------------
typedef BOOL(WINAPI* PFN_HWND)(HWND);
typedef VOID(WINAPI* PFN_SWITCH)(HWND, BOOL);
typedef BOOL(WINAPI* PFN_SHOW)(HWND, int);
typedef BOOL(WINAPI* PFN_SWP)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL(WINAPI* PFN_CLIP)(const RECT*);
typedef SHORT(WINAPI* PFN_KEY)(int);
typedef HWND(WINAPI* PFN_GETWND)(void);
typedef HWND(WINAPI* PFN_SETWND)(HWND);
typedef BOOL(WINAPI* PFN_GETCUR)(LPPOINT);
typedef HHOOK(WINAPI* PFN_SWHE)(int, HOOKPROC, HINSTANCE, DWORD);
static PFN_SWHE o_SetWindowsHookExA, o_SetWindowsHookExW;
static PFN_GETCUR o_GetCursorPos;

static PFN_HWND o_SetForegroundWindow, o_BringWindowToTop;
static PFN_SWITCH o_SwitchToThisWindow;
static PFN_SHOW o_ShowWindow;
static PFN_SWP o_SetWindowPos;
static PFN_CLIP o_ClipCursor;
static PFN_KEY o_GetAsyncKeyState, o_GetKeyState;
static PFN_GETWND o_GetActiveWindow, o_GetForegroundWindow;
static PFN_SETWND o_SetActiveWindow, o_SetFocus;
typedef HWND(WINAPI* PFN_CWA)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef HWND(WINAPI* PFN_CWW)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static PFN_CWA o_CreateWindowExA;
static PFN_CWW o_CreateWindowExW;
static void ensureThreadHooked(DWORD tid);
static void applyCloak();

// --- helpers ----------------------------------------------------------------------------------------
static bool ownWindow(HWND h) {
    DWORD pid = 0;
    if (!h || !GetWindowThreadProcessId(h, &pid)) return false;
    return pid == GetCurrentProcessId();
}
static bool ownTopLevel(HWND h) {
    return ownWindow(h) && !(GetWindowLongW(h, GWL_STYLE) & WS_CHILD);
}
static bool weAreForeground() {
    HWND fg = o_GetForegroundWindow ? o_GetForegroundWindow() : GetForegroundWindow();
    return ownWindow(fg);
}
static bool on() { return g_shm->enabled != 0; }
static void count(int h, bool altered) {
    InterlockedIncrement(&g_shm->calls[h]);
    if (altered) InterlockedIncrement(&g_shm->altered[h]);
}
static SHORT virtualKey(int vk) {
    if (vk < 0 || vk > 255) return 0;
    bool down = g_shm->vk_down[vk] != 0;
    // Generic modifiers are held when either side is; sided ones follow the generic one.
    if (!down) {
        switch (vk) {
        case VK_SHIFT: down = g_shm->vk_down[VK_LSHIFT] || g_shm->vk_down[VK_RSHIFT]; break;
        case VK_CONTROL: down = g_shm->vk_down[VK_LCONTROL] || g_shm->vk_down[VK_RCONTROL]; break;
        case VK_MENU: down = g_shm->vk_down[VK_LMENU] || g_shm->vk_down[VK_RMENU]; break;
        case VK_LSHIFT: down = g_shm->vk_down[VK_SHIFT] != 0; break;
        case VK_LCONTROL: down = g_shm->vk_down[VK_CONTROL] != 0; break;
        case VK_LMENU: down = g_shm->vk_down[VK_MENU] != 0; break;
        }
    }
    return down ? (SHORT)0x8000 : 0;
}

// --- detours ----------------------------------------------------------------------------------------
static BOOL WINAPI d_SetForegroundWindow(HWND h) {
    if (on() && !weAreForeground()) {
        count(H_SETFOREGROUND, true);
        glog("blocked SetForegroundWindow(%p)", (void*)h);
        return TRUE;
    }
    count(H_SETFOREGROUND, false);
    return o_SetForegroundWindow(h);
}
static BOOL WINAPI d_BringWindowToTop(HWND h) {
    if (on() && ownWindow(h) && !weAreForeground()) {
        count(H_BRINGTOTOP, true);
        glog("blocked BringWindowToTop(%p)", (void*)h);
        return TRUE;
    }
    count(H_BRINGTOTOP, false);
    return o_BringWindowToTop(h);
}
static VOID WINAPI d_SwitchToThisWindow(HWND h, BOOL alt) {
    if (on() && ownWindow(h) && !weAreForeground()) {
        count(H_SWITCHTO, true);
        glog("blocked SwitchToThisWindow(%p)", (void*)h);
        return;
    }
    count(H_SWITCHTO, false);
    o_SwitchToThisWindow(h, alt);
}
static BOOL WINAPI d_ShowWindow(HWND h, int cmd) {
    ensureThreadHooked(GetCurrentThreadId());
    int newCmd = cmd;
    if (on() && ownTopLevel(h) && !weAreForeground()) {
        switch (cmd) {
        case SW_SHOWNORMAL: case SW_RESTORE: case SW_SHOWDEFAULT: newCmd = SW_SHOWNOACTIVATE; break;
        case SW_SHOW: newCmd = SW_SHOWNA; break;
        case SW_SHOWMINIMIZED: newCmd = SW_SHOWMINNOACTIVE; break;
        }
    }
    count(H_SHOWWINDOW, newCmd != cmd);
    if (newCmd != cmd) glog("ShowWindow(%p, %d) -> %d", (void*)h, cmd, newCmd);
    if (ownTopLevel(h) && cmd != SW_HIDE) {
        RECT r; GetWindowRect(h, &r);
        if (r.right - r.left >= 200 && r.bottom - r.top >= 150 && (HWND)(UINT_PTR)g_shm->main_hwnd != h) {
            g_shm->main_hwnd = (DWORD)(UINT_PTR)h;
            g_shm->cloak_applied = -1;
            applyCloak();   // cloak before the first frame is ever on screen
        }
    }
    return o_ShowWindow(h, newCmd);
}
static BOOL WINAPI d_SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy, UINT flags) {
    UINT nf = flags;
    if (on() && ownTopLevel(h) && !weAreForeground()) {
        nf |= SWP_NOACTIVATE;
        if (!(flags & SWP_NOZORDER) && (after == HWND_TOP || after == HWND_TOPMOST || after == HWND_NOTOPMOST))
            nf |= SWP_NOZORDER;
    }
    count(H_SETWINDOWPOS, nf != flags);
    if (nf != flags) glog("SetWindowPos(%p, after=%p, flags=0x%x) -> flags 0x%x", (void*)h, (void*)after, flags, nf);
    return o_SetWindowPos(h, after, x, y, cx, cy, nf);
}
static BOOL WINAPI d_ClipCursor(const RECT* r) {
    if (on() && r && !weAreForeground()) {
        count(H_CLIPCURSOR, true);
        glog("blocked ClipCursor(%ld,%ld,%ld,%ld)", r->left, r->top, r->right, r->bottom);
        return TRUE;
    }
    count(H_CLIPCURSOR, false);
    return o_ClipCursor(r);
}
static bool isolated() { return on() && g_shm->isolate && !weAreForeground(); }
static SHORT WINAPI d_GetAsyncKeyState(int vk) {
    SHORT v = isolated() ? 0 : o_GetAsyncKeyState(vk);
    SHORT o = virtualKey(vk);
    if (o) { count(H_GETASYNCKEY, true); v |= o; }
    return v;
}
static SHORT WINAPI d_GetKeyState(int vk) {
    SHORT v = o_GetKeyState(vk);
    if (isolated()) v &= 1;   // keep the toggle bit (Caps/Num Lock), drop "down"
    SHORT o = virtualKey(vk);
    if (o) { count(H_GETKEY, true); v |= o; }
    return v;
}
static HWND WINAPI d_GetActiveWindow(void) {
    if (g_shm->pretend_active && g_shm->main_hwnd) {
        count(H_GETACTIVE, true);
        return (HWND)(UINT_PTR)g_shm->main_hwnd;
    }
    return o_GetActiveWindow();
}
static HWND WINAPI d_GetForegroundWindow(void) {
    if (g_shm->pretend_active && g_shm->main_hwnd) {
        count(H_GETFOREGROUND, true);
        return (HWND)(UINT_PTR)g_shm->main_hwnd;
    }
    return o_GetForegroundWindow();
}
static BOOL WINAPI d_GetCursorPos(LPPOINT p) {
    if (g_shm->vcursor_on && p) {
        count(H_GETCURSORPOS, true);
        p->x = g_shm->vcursor_x;
        p->y = g_shm->vcursor_y;
        return TRUE;
    }
    return o_GetCursorPos(p);
}
// A dummy handle for a refused low-level hook; UnhookWindowsHookEx on it just fails.
static HHOOK refuseLL(int id, const char* which) {
    count(H_LLHOOK, true);
    glog("refused %s(WH_%s_LL): global low-level hook while in the background", which,
         id == WH_KEYBOARD_LL ? "KEYBOARD" : "MOUSE");
    return (HHOOK)(UINT_PTR)0x0C1BB00C;
}
static HHOOK WINAPI d_SetWindowsHookExA(int id, HOOKPROC fn, HINSTANCE mod, DWORD tid) {
    if (on() && (id == WH_KEYBOARD_LL || id == WH_MOUSE_LL) && !weAreForeground()) return refuseLL(id, "SetWindowsHookExA");
    if (id == WH_KEYBOARD_LL || id == WH_MOUSE_LL) count(H_LLHOOK, false);
    return o_SetWindowsHookExA(id, fn, mod, tid);
}
static HHOOK WINAPI d_SetWindowsHookExW(int id, HOOKPROC fn, HINSTANCE mod, DWORD tid) {
    if (on() && (id == WH_KEYBOARD_LL || id == WH_MOUSE_LL) && !weAreForeground()) return refuseLL(id, "SetWindowsHookExW");
    if (id == WH_KEYBOARD_LL || id == WH_MOUSE_LL) count(H_LLHOOK, false);
    return o_SetWindowsHookExW(id, fn, mod, tid);
}
static HWND WINAPI d_SetActiveWindow(HWND h) {
    // Not harmless: on the stand-in (2026-11-09) a background SetActiveWindow on its own window made
    // it the foreground window. The CBT veto would catch it too; this keeps it from even trying.
    if (on() && ownWindow(h) && !weAreForeground()) {
        count(H_SETACTIVE, true);
        glog("blocked SetActiveWindow(%p)", (void*)h);
        return o_GetActiveWindow();
    }
    count(H_SETACTIVE, false);
    return o_SetActiveWindow(h);
}
static HWND WINAPI d_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD st, int x, int y, int w, int h,
                                     HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    ensureThreadHooked(GetCurrentThreadId());   // before the window exists: WS_VISIBLE activates inside
    return o_CreateWindowExA(ex, cls, name, st, x, y, w, h, parent, menu, inst, param);
}
static HWND WINAPI d_CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD st, int x, int y, int w, int h,
                                     HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    ensureThreadHooked(GetCurrentThreadId());
    return o_CreateWindowExW(ex, cls, name, st, x, y, w, h, parent, menu, inst, param);
}
static HWND WINAPI d_SetFocus(HWND h) {
    count(H_SETFOCUS, false);
    return o_SetFocus(h);
}


// --- the activation veto ----------------------------------------------------------------------------
// Blocking SetForegroundWindow is not enough: when the foreground window closes, hides or minimises,
// Windows itself activates the next window in z-order, and a freshly shown game window is usually at
// the top (seen on the stand-in 2026-11-09: activated 12 ms before its own, blocked, call). A thread
// WH_CBT hook sees every activation of the thread's windows (HCBT_ACTIVATE) and returning 1 cancels
// it. Installed on every thread that creates or shows a window (from the CreateWindowEx/ShowWindow
// detours, so before the first window exists) and, for windows that existed before the guard was
// injected, by a watcher thread that scans the process' top-level windows.
static HMODULE g_module;
static CRITICAL_SECTION g_hookLock;
static DWORD g_hookedTids[128];
static int g_nHooked;

static bool userAskedForIt() {
    // A human click on the window itself or on the taskbar (button / thumbnail) is honoured.
    LASTINPUTINFO li = {sizeof(li)};
    if (!GetLastInputInfo(&li) || GetTickCount() - li.dwTime > 500) return false;
    POINT pt;
    if (!(o_GetCursorPos ? o_GetCursorPos(&pt) : GetCursorPos(&pt))) return false;
    HWND at = WindowFromPoint(pt);
    HWND root = at ? GetAncestor(at, GA_ROOT) : 0;
    if (!root) return false;
    if (ownWindow(root)) return true;
    char cls[64] = "";
    GetClassNameA(root, cls, sizeof(cls));
    return !strcmp(cls, "Shell_TrayWnd") || !strcmp(cls, "Shell_SecondaryTrayWnd") ||
           !strcmp(cls, "TaskListThumbnailWnd");
}

static LRESULT CALLBACK cbtProc(int code, WPARAM w, LPARAM l) {
    if (code == HCBT_ACTIVATE && g_shm && g_shm->enabled) {
        HWND h = (HWND)w;
        if (ownWindow(h) && !weAreForeground()) {
            // A cloaked window cannot be seen, so a click on its taskbar button would give Art an
            // invisible focused window that eats his keys: no user exception while cloaked
            // (civ_bg.py front uncloaks and turns the guard off first).
            if (g_shm->cloak_applied == 1 || !userAskedForIt()) {
                count(H_ACTIVATE_VETO, true);
                glog("vetoed activation of %p (HCBT_ACTIVATE)", (void*)h);
                return 1;
            }
            count(H_ACTIVATE_VETO, false);
            glog("allowed activation of %p: fresh user input on the window or the taskbar", (void*)h);
        }
    }
    return CallNextHookEx(0, code, w, l);
}

static void ensureThreadHooked(DWORD tid) {
    if (!g_module) return;
    EnterCriticalSection(&g_hookLock);
    bool have = false;
    for (int i = 0; i < g_nHooked; i++) if (g_hookedTids[i] == tid) { have = true; break; }
    if (!have && g_nHooked < 128) {
        HHOOK hk = (o_SetWindowsHookExW ? o_SetWindowsHookExW : SetWindowsHookExW)(WH_CBT, cbtProc, g_module, tid);
        g_hookedTids[g_nHooked++] = tid;   // recorded even on failure: do not retry every call
        glog("activation veto (WH_CBT) on thread %lu: %s (err %lu)", tid, hk ? "ok" : "FAILED", hk ? 0 : GetLastError());
    }
    LeaveCriticalSection(&g_hookLock);
}

static HWND g_biggest;
static LONG g_biggestArea;

static BOOL CALLBACK enumOwn(HWND h, LPARAM) {
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    ensureThreadHooked(tid);
    RECT r;
    if (IsWindowVisible(h) && GetWindowRect(h, &r)) {
        LONG area = (r.right - r.left) * (r.bottom - r.top);
        if (r.right - r.left >= 200 && r.bottom - r.top >= 150 && area > g_biggestArea) {
            g_biggestArea = area;
            g_biggest = h;
        }
    }
    return TRUE;
}

// Cloaking (DWMWA_CLOAK) hides a window from the screen while DWM keeps composing it: on the stand-in
// it kept rendering at the monitor rate with fresh PrintWindow frames. Only the owning process may
// cloak its window, which is why it lives here. dwmapi is loaded on demand so this file can also be
// compiled into the game DLL without a new static import.
typedef HRESULT(WINAPI* PFN_DWMSET)(HWND, DWORD, LPCVOID, DWORD);
static void applyCloak() {
    static PFN_DWMSET dwmSet;
    if (!dwmSet) {
        HMODULE d = LoadLibraryA("dwmapi.dll");
        dwmSet = d ? (PFN_DWMSET)GetProcAddress(d, "DwmSetWindowAttribute") : 0;
        if (!dwmSet) return;
    }
    HWND h = (HWND)(UINT_PTR)g_shm->main_hwnd;
    LONG want = g_shm->cloak ? 1 : 0;
    if (!h || !IsWindow(h) || want == g_shm->cloak_applied) return;
    BOOL v = want;
    HRESULT hr = dwmSet(h, 13 /* DWMWA_CLOAK */, &v, sizeof(v));
    g_shm->cloak_hr = hr;
    g_shm->cloak_applied = want;
    glog("cloak(%p) = %ld -> hr 0x%08lx", (void*)h, want, hr);
}

static DWORD WINAPI watcher(LPVOID) {
    for (;;) {
        g_biggest = 0;
        g_biggestArea = 0;
        EnumWindows(enumOwn, 0);
        // Windows that existed before the guard arrived never went through d_ShowWindow.
        if (g_biggest && (!g_shm->main_hwnd || !IsWindow((HWND)(UINT_PTR)g_shm->main_hwnd)))
            g_shm->main_hwnd = (DWORD)(UINT_PTR)g_biggest;
        applyCloak();
        Sleep(100);
    }
}

// --- the patcher ------------------------------------------------------------------------------------
static BYTE* g_tramp;   // executable page for trampolines
static int g_trampUsed;

static bool writeCode(BYTE* at, const BYTE* code, int n) {
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, code, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

// Returns the address to call for the original behaviour, or 0 if the prologue is not one we know.
// The original pointer (*origOut) is published before the jump is written, so a thread entering the
// detour mid-install never sees a missing original.
static void* hookAt(BYTE* p, void* detour, void** origOut, const char* name, char* status, size_t statusLen) {
    BYTE jmp[6] = {0xE9, 0, 0, 0, 0, 0x90};
    void* orig = 0;
    int len = 0;
    if (p[0] == 0x8B && p[1] == 0xFF && p[2] == 0x55 && p[3] == 0x8B && p[4] == 0xEC) {
        // mov edi,edi; push ebp; mov ebp,esp -> trampoline replays push ebp; mov ebp,esp; jmp p+5
        BYTE* t = g_tramp + g_trampUsed;
        g_trampUsed += 16;
        t[0] = 0x55; t[1] = 0x8B; t[2] = 0xEC; t[3] = 0xE9;
        *(INT32*)(t + 4) = (INT32)((p + 5) - (t + 8));
        orig = t; len = 5;
    } else if (p[0] == 0xFF && p[1] == 0x25) {
        // jmp dword ptr [abs] -> the original is whatever the thunk slot points at
        orig = **(void***)(p + 2); len = 6;
    } else {
        _snprintf(status + strlen(status), statusLen - strlen(status) - 1, "%s:unknown(%02x %02x %02x) ", name, p[0], p[1], p[2]);
        return 0;
    }
    *(INT32*)(jmp + 1) = (INT32)((BYTE*)detour - (p + 5));
    *origOut = orig;
    MemoryBarrier();
    if (!writeCode(p, jmp, len)) {
        _snprintf(status + strlen(status), statusLen - strlen(status) - 1, "%s:protect-failed ", name);
        return 0;
    }
    _snprintf(status + strlen(status), statusLen - strlen(status) - 1, "%s:ok ", name);
    return orig;
}

static bool g_installed;

extern "C" __declspec(dllexport) int FocusGuardInstall(void) {
    if (g_installed) return 1;
    g_installed = true;
    InitializeCriticalSection(&g_logLock);
    InitializeCriticalSection(&g_hookLock);
    if (!g_module)
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&FocusGuardInstall, &g_module);
    char path[MAX_PATH], tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    _snprintf(path, MAX_PATH, "%scivbg_guard_%lu.log", tmp, GetCurrentProcessId());
    g_log = fopen(path, "a");

    char name[64];
    _snprintf(name, sizeof(name), "Local\\CivBgGuard_%lu", GetCurrentProcessId());
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0, 4096, name);
    g_shm = map ? (GuardShm*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 4096) : 0;
    if (!g_shm) { g_shm = &g_fallbackShm; memset(g_shm, 0, sizeof(*g_shm)); }
    bool fresh = g_shm->magic != 0x31474243;
    if (fresh) {
        g_shm->magic = 0x31474243; g_shm->version = 1; g_shm->enabled = 1;
        g_shm->cloak_applied = -1;
        g_shm->isolate = 1;
        // Defaults from the launcher's environment (inherited by the game unless Steam relaunches it).
        char v[8];
        if (GetEnvironmentVariableA("CIVBG_GUARD", v, sizeof(v)) && v[0] == '0') g_shm->enabled = 0;
        if (GetEnvironmentVariableA("CIVBG_CLOAK", v, sizeof(v)) && v[0] == '1') g_shm->cloak = 1;
    }
    g_shm->pid = GetCurrentProcessId();
    // The driver may have created the section first to pre-set flags; keep its values then.

    g_tramp = (BYTE*)VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    HMODULE u = GetModuleHandleA("user32.dll");
    if (!u) u = LoadLibraryA("user32.dll");
    char* st = g_shm->status; st[0] = 0;
    struct { const char* n; void* d; void** o; } hooks[] = {
        {"GetForegroundWindow", (void*)d_GetForegroundWindow, (void**)&o_GetForegroundWindow},
        {"SetForegroundWindow", (void*)d_SetForegroundWindow, (void**)&o_SetForegroundWindow},
        {"BringWindowToTop", (void*)d_BringWindowToTop, (void**)&o_BringWindowToTop},
        {"SwitchToThisWindow", (void*)d_SwitchToThisWindow, (void**)&o_SwitchToThisWindow},
        {"ShowWindow", (void*)d_ShowWindow, (void**)&o_ShowWindow},
        {"SetWindowPos", (void*)d_SetWindowPos, (void**)&o_SetWindowPos},
        {"ClipCursor", (void*)d_ClipCursor, (void**)&o_ClipCursor},
        {"GetAsyncKeyState", (void*)d_GetAsyncKeyState, (void**)&o_GetAsyncKeyState},
        {"GetKeyState", (void*)d_GetKeyState, (void**)&o_GetKeyState},
        {"GetActiveWindow", (void*)d_GetActiveWindow, (void**)&o_GetActiveWindow},
        {"SetActiveWindow", (void*)d_SetActiveWindow, (void**)&o_SetActiveWindow},
        {"SetFocus", (void*)d_SetFocus, (void**)&o_SetFocus},
        {"CreateWindowExA", (void*)d_CreateWindowExA, (void**)&o_CreateWindowExA},
        {"CreateWindowExW", (void*)d_CreateWindowExW, (void**)&o_CreateWindowExW},
        {"GetCursorPos", (void*)d_GetCursorPos, (void**)&o_GetCursorPos},
        {"SetWindowsHookExA", (void*)d_SetWindowsHookExA, (void**)&o_SetWindowsHookExA},
        {"SetWindowsHookExW", (void*)d_SetWindowsHookExW, (void**)&o_SetWindowsHookExW},
    };
    int okCount = 0;
    for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
        BYTE* p = (BYTE*)GetProcAddress(u, hooks[i].n);
        if (!p) continue;
        if (hookAt(p, hooks[i].d, hooks[i].o, hooks[i].n, st, sizeof(g_shm->status))) okCount++;
        else *hooks[i].o = (void*)p;   // not patched: the export itself is the original
    }
    glog("FocusGuardInstall pid=%lu hooks_ok=%d status: %s", GetCurrentProcessId(), okCount, st);
    HANDLE th = CreateThread(0, 0, watcher, 0, 0, 0);   // starts once the loader lock is released
    if (th) CloseHandle(th);
    return okCount;
}

#ifndef FOCUS_GUARD_NO_DLLMAIN
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_module = inst;
        FocusGuardInstall();
    }
    return TRUE;
}
#endif
