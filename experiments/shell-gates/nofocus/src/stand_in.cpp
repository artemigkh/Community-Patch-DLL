// stand_in.exe - a 32-bit D3D11 window that stands in for Civ 5 in the no-focus experiments.
//
// It renders like a game (PeekMessage loop, Present with vsync), behaves like a badly-mannered one
// (optionally calls SetForegroundWindow / SetActiveWindow / ShowWindow(SW_RESTORE) on itself every N
// seconds, optionally ClipCursor), and logs everything an outside driver could deliver:
//   - window messages: keys, chars, mouse, activation, focus, show/size
//   - for each key message: what GetKeyState and GetAsyncKeyState report for that key
//   - for each mouse message: lParam position vs the real cursor position
//   - raw input (WM_INPUT) arrivals, registered without RIDEV_INPUTSINK (= only while foreground)
//   - polled GetAsyncKeyState transitions for F6..F9 (what a game polling camera keys would see)
//   - once per second: frames presented, Present results (S_OK / DXGI_STATUS_OCCLUDED / other),
//     iconic, foreground, active, cloaked, the desktop it runs on
//
// Screen content: the background colour cycles every frame; a 7-segment frame counter top left;
// the last key's virtual-key code under it; a white box at the last click position.
//
// Usage: stand_in.exe [--log path] [--x X --y Y --w W --h H] [--show N] [--steal SEC]
//                     [--restore SEC] [--clip] [--flip] [--title T] [--sleep-occluded] [--start-steal 0|1]
//                     [--cloak SEC] [--llhook]
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static FILE* g_log;
static HWND g_hwnd;
static DWORD g_t0;
static int g_lastVk = 0;
static int g_clickX = -1, g_clickY = -1;
static long g_rawKeys = 0, g_rawMouse = 0;

static void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    if (g_log) { fprintf(g_log, "%8.3f %s\n", (GetTickCount() - g_t0) / 1000.0, buf); fflush(g_log); }
}

static const char* msgName(UINT m) {
    switch (m) {
    case WM_KEYDOWN: return "WM_KEYDOWN"; case WM_KEYUP: return "WM_KEYUP";
    case WM_SYSKEYDOWN: return "WM_SYSKEYDOWN"; case WM_SYSKEYUP: return "WM_SYSKEYUP";
    case WM_CHAR: return "WM_CHAR"; case WM_SYSCHAR: return "WM_SYSCHAR";
    case WM_MOUSEMOVE: return "WM_MOUSEMOVE"; case WM_LBUTTONDOWN: return "WM_LBUTTONDOWN";
    case WM_LBUTTONUP: return "WM_LBUTTONUP"; case WM_RBUTTONDOWN: return "WM_RBUTTONDOWN";
    case WM_RBUTTONUP: return "WM_RBUTTONUP"; case WM_MOUSEWHEEL: return "WM_MOUSEWHEEL";
    }
    return "?";
}

static LRESULT CALLBACK llProc(int code, WPARAM w, LPARAM l) {
    if (code >= 0 && w == WM_SYSKEYDOWN && ((KBDLLHOOKSTRUCT*)l)->vkCode == VK_RETURN) return 1;
    return CallNextHookEx(0, code, w, l);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        int vk = (int)w;
        if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) g_lastVk = vk;
        logf("MSG %s vk=0x%02x lParam=0x%08lx GetKeyState=0x%04x GetAsyncKeyState=0x%04x shiftKS=0x%04x shiftAKS=0x%04x",
             msgName(m), vk, (unsigned long)l, (unsigned short)GetKeyState(vk),
             (unsigned short)GetAsyncKeyState(vk), (unsigned short)GetKeyState(VK_SHIFT),
             (unsigned short)GetAsyncKeyState(VK_SHIFT));
        break;
    }
    case WM_CHAR: case WM_SYSCHAR:
        logf("MSG %s ch=0x%04x", msgName(m), (unsigned)w);
        break;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
        if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN) { g_clickX = x; g_clickY = y; }
        logf("MSG %s x=%d y=%d wParam=0x%x cursor_client=(%ld,%ld) lbuttonAKS=0x%04x",
             msgName(m), x, y, (unsigned)w, p.x, p.y, (unsigned short)GetAsyncKeyState(VK_LBUTTON));
        break;
    }
    case WM_MOUSEWHEEL:
        logf("MSG WM_MOUSEWHEEL delta=%d", (short)HIWORD(w));
        break;
    case WM_INPUT: {
        RAWINPUTHEADER hdr; UINT sz = sizeof(hdr);
        if (GetRawInputData((HRAWINPUT)l, RID_HEADER, &hdr, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            if (hdr.dwType == RIM_TYPEKEYBOARD) g_rawKeys++; else if (hdr.dwType == RIM_TYPEMOUSE) g_rawMouse++;
        }
        break;
    }
    case WM_ACTIVATE:
        logf("MSG WM_ACTIVATE state=%u minimized=%u other=%p", LOWORD(w), HIWORD(w), (void*)l);
        break;
    case WM_ACTIVATEAPP:
        logf("MSG WM_ACTIVATEAPP active=%u", (unsigned)w);
        break;
    case WM_SETFOCUS: logf("MSG WM_SETFOCUS"); break;
    case WM_KILLFOCUS: logf("MSG WM_KILLFOCUS"); break;
    case WM_SHOWWINDOW: logf("MSG WM_SHOWWINDOW show=%u", (unsigned)w); break;
    case WM_SIZE: logf("MSG WM_SIZE type=%u %ux%u", (unsigned)w, LOWORD(l), HIWORD(l)); break;
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// 7-segment digit via ClearView rectangles.
static void drawDigit(ID3D11DeviceContext1* ctx, ID3D11RenderTargetView* rtv, int d, int x, int y, int s, const float* col) {
    static const unsigned char seg[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    int t = s / 5 + 1, L = s;
    D3D11_RECT r[7] = {
        {x, y, x + L, y + t},                       // a top
        {x + L - t, y, x + L, y + L},               // b top right
        {x + L - t, y + L, x + L, y + 2 * L},       // c bottom right
        {x, y + 2 * L - t, x + L, y + 2 * L},       // d bottom
        {x, y + L, x + t, y + 2 * L},               // e bottom left
        {x, y, x + t, y + L},                       // f top left
        {x, y + L - t / 2, x + L, y + L + t - t / 2} // g middle
    };
    D3D11_RECT on[7]; int n = 0;
    for (int i = 0; i < 7; i++) if (seg[d] & (1 << i)) on[n++] = r[i];
    if (n) ctx->ClearView(rtv, col, on, n);
}

static void drawNumber(ID3D11DeviceContext1* ctx, ID3D11RenderTargetView* rtv, unsigned v, int x, int y, int s, const float* col) {
    char buf[16]; sprintf(buf, "%u", v);
    for (int i = 0; buf[i]; i++) drawDigit(ctx, rtv, buf[i] - '0', x + i * (s + s / 2), y, s, col);
}

static const char* argStr(int argc, char** argv, const char* name, const char* def) {
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], name)) return argv[i + 1];
    return def;
}
static bool argFlag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], name)) return true;
    return false;
}

int main(int argc, char** argv) {
    g_t0 = GetTickCount();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    char defLog[MAX_PATH]; sprintf(defLog, "stand_in_%lu.log", GetCurrentProcessId());
    g_log = fopen(argStr(argc, argv, "--log", defLog), "w");
    int X = atoi(argStr(argc, argv, "--x", "100")), Y = atoi(argStr(argc, argv, "--y", "100"));
    int W = atoi(argStr(argc, argv, "--w", "800")), H = atoi(argStr(argc, argv, "--h", "600"));
    int showCmd = atoi(argStr(argc, argv, "--show", "1"));   // SW_SHOWNORMAL, like most games
    double steal = atof(argStr(argc, argv, "--steal", "0"));  // SetForegroundWindow every N s
    double restore = atof(argStr(argc, argv, "--restore", "0")); // ShowWindow(SW_RESTORE) every N s
    bool clip = argFlag(argc, argv, "--clip");
    bool flip = argFlag(argc, argv, "--flip");
    bool sleepOcc = argFlag(argc, argv, "--sleep-occluded");
    const char* title = argStr(argc, argv, "--title", "VP StandIn");
    double cloakAt = atof(argStr(argc, argv, "--cloak", "0"));   // DWMWA_CLOAK itself after N s
    bool cloakDone = false;

    STARTUPINFOW si = {sizeof(si)}; GetStartupInfoW(&si);
    char desk[256] = "?"; DWORD need = 0;
    GetUserObjectInformationA(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desk, sizeof(desk), &need);
    logf("START pid=%lu desktop=%s startupinfo.flags=0x%lx wShowWindow=%u lpDesktop=%S fg_at_start=%p",
         GetCurrentProcessId(), desk, si.dwFlags, si.wShowWindow, si.lpDesktop ? si.lpDesktop : L"(null)",
         (void*)GetForegroundWindow());

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(0);
    wc.hCursor = LoadCursor(0, IDC_ARROW); wc.lpszClassName = L"VPStandIn";
    RegisterClassExW(&wc);
    RECT rc = {0, 0, W, H}; AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    wchar_t wtitle[256]; MultiByteToWideChar(CP_ACP, 0, title, -1, wtitle, 256);
    g_hwnd = CreateWindowExW(0, L"VPStandIn", wtitle, WS_OVERLAPPEDWINDOW, X, Y, rc.right - rc.left,
                             rc.bottom - rc.top, 0, 0, wc.hInstance, 0);
    logf("CREATED hwnd=%p", (void*)g_hwnd);
    BOOL sw = ShowWindow(g_hwnd, showCmd);
    logf("ShowWindow(%d) -> %d; fg_is_me=%d iconic=%d", showCmd, sw, GetForegroundWindow() == g_hwnd, IsIconic(g_hwnd));
    if (atoi(argStr(argc, argv, "--start-steal", "1"))) {
        BOOL sf = SetForegroundWindow(g_hwnd);
        logf("SetForegroundWindow at start -> %d; fg_is_me=%d", sf, GetForegroundWindow() == g_hwnd);
    }
    { WINDOWPLACEMENT wp = {sizeof(wp)}; GetWindowPlacement(g_hwnd, &wp);
      logf("PLACEMENT showCmd=%u visible=%d iconic=%d", wp.showCmd, IsWindowVisible(g_hwnd), IsIconic(g_hwnd)); }

    if (argFlag(argc, argv, "--llhook")) {   // like Civ 5: a global WH_KEYBOARD_LL that eats Alt+Enter
        HHOOK hk = SetWindowsHookExA(WH_KEYBOARD_LL, llProc, GetModuleHandleW(0), 0);
        logf("SetWindowsHookExA(WH_KEYBOARD_LL) -> %p", (void*)hk);
    }
    RAWINPUTDEVICE rid[2] = {{0x01, 0x06, 0, g_hwnd}, {0x01, 0x02, 0, g_hwnd}};
    logf("RegisterRawInputDevices -> %d", RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE)));

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = flip ? 2 : 1;
    sd.BufferDesc.Width = W; sd.BufferDesc.Height = H;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = flip ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
    ID3D11Device* dev = 0; ID3D11DeviceContext* ctx0 = 0; IDXGISwapChain* swp = 0; D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0, 0, 0, D3D11_SDK_VERSION, &sd,
                                               &swp, &dev, &fl, &ctx0);
    logf("D3D11CreateDeviceAndSwapChain hr=0x%08lx fl=0x%x swap=%s", hr, fl, flip ? "flip_discard" : "discard(blt)");
    if (FAILED(hr)) return 2;
    {
        IDXGIDevice* dd = 0; IDXGIAdapter* ad = 0; DXGI_ADAPTER_DESC adesc = {};
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && SUCCEEDED(dd->GetAdapter(&ad)))
            ad->GetDesc(&adesc);
        logf("ADAPTER %S", adesc.Description);
        if (ad) ad->Release(); if (dd) dd->Release();
    }
    ID3D11DeviceContext1* ctx = 0; ctx0->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx);
    ID3D11Texture2D* bb = 0; swp->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    ID3D11RenderTargetView* rtv = 0; dev->CreateRenderTargetView(bb, 0, &rtv); bb->Release();

    if (clip) { RECT cr; GetClientRect(g_hwnd, &cr); MapWindowPoints(g_hwnd, 0, (POINT*)&cr, 2); logf("ClipCursor -> %d", ClipCursor(&cr)); }

    unsigned frame = 0, fps = 0, ok = 0, occ = 0, other = 0;
    DWORD lastSec = GetTickCount(), lastSteal = GetTickCount(), lastRestore = GetTickCount();
    bool polled[4] = {};
    const int pollVk[4] = {VK_F6, VK_F7, VK_F8, VK_F9};
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        for (int i = 0; i < 4; i++) {
            bool down = (GetAsyncKeyState(pollVk[i]) & 0x8000) != 0;
            if (down != polled[i]) { polled[i] = down; logf("POLL GetAsyncKeyState(F%d) %s", 6 + i, down ? "DOWN" : "up"); }
        }
        DWORD now = GetTickCount();
        if (steal > 0 && now - lastSteal >= steal * 1000) {
            lastSteal = now;
            BOOL r1 = SetForegroundWindow(g_hwnd);
            HWND r2 = SetActiveWindow(g_hwnd);
            logf("STEAL SetForegroundWindow -> %d SetActiveWindow -> %p fg_is_me=%d", r1, (void*)r2, GetForegroundWindow() == g_hwnd);
        }
        if (restore > 0 && now - lastRestore >= restore * 1000) {
            lastRestore = now;
            BOOL r = ShowWindow(g_hwnd, SW_RESTORE);
            logf("RESTORE ShowWindow(SW_RESTORE) -> %d fg_is_me=%d iconic=%d", r, GetForegroundWindow() == g_hwnd, IsIconic(g_hwnd));
        }
        if (cloakAt > 0 && !cloakDone && now - g_t0 >= cloakAt * 1000) {
            cloakDone = true;
            BOOL c = TRUE;
            logf("CLOAK DwmSetWindowAttribute(DWMWA_CLOAK) hr=0x%08lx", DwmSetWindowAttribute(g_hwnd, 13, &c, sizeof(c)));
        }
        // Render.
        float t = frame * 0.02f;
        float bg[4] = {0.5f + 0.5f * sinf(t), 0.5f + 0.5f * sinf(t + 2.1f), 0.5f + 0.5f * sinf(t + 4.2f), 1};
        ctx->ClearRenderTargetView(rtv, bg);
        const float white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 1}, yellow[4] = {1, 1, 0, 1};
        D3D11_RECT panel = {10, 10, 520, 250};
        ctx->ClearView(rtv, black, &panel, 1);
        drawNumber(ctx, rtv, frame, 20, 20, 40, white);
        drawNumber(ctx, rtv, (unsigned)g_lastVk, 20, 130, 40, yellow);
        if (g_clickX >= 0) {
            D3D11_RECT box = {g_clickX - 12, g_clickY - 12, g_clickX + 12, g_clickY + 12};
            ctx->ClearView(rtv, white, &box, 1);
        }
        ctx->OMSetRenderTargets(1, &rtv, 0);
        hr = swp->Present(1, 0);
        frame++; fps++;
        if (hr == S_OK) ok++; else if (hr == DXGI_STATUS_OCCLUDED) occ++; else other++;
        if (hr == DXGI_STATUS_OCCLUDED && sleepOcc) Sleep(50);
        if (hr != S_OK && hr != DXGI_STATUS_OCCLUDED) { logf("PRESENT hr=0x%08lx", hr); Sleep(10); }
        if (now - lastSec >= 1000) {
            BOOL cloaked = 0; DwmGetWindowAttribute(g_hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            logf("SEC fps=%u present_ok=%u occluded=%u other=%u frame=%u iconic=%d fg_is_me=%d active_is_me=%d focus_is_me=%d cloaked=%d rawkeys=%ld rawmouse=%ld",
                 fps, ok, occ, other, frame, IsIconic(g_hwnd), GetForegroundWindow() == g_hwnd,
                 GetActiveWindow() == g_hwnd, GetFocus() == g_hwnd, cloaked, g_rawKeys, g_rawMouse);
            fps = ok = occ = other = 0; lastSec = now;
        }
    }
done:
    logf("EXIT frames=%u", frame);
    if (clip) ClipCursor(0);
    return 0;
}
