#include "osvideo.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>

static char *w_to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = (char *)malloc(n);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

int os_open_media_files(void (*on_file)(const char *, void *), void *ud) {
    static wchar_t buf[32768];
    buf[0] = 0;
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter =
        L"Media Files\0"
        L"*.mp4;*.mkv;*.webm;*.avi;*.mov;*.m4v;*.wmv;*.flv;*.mpg;*.mpeg;*.ts;*.m2ts;*.mts;*.vob;*.ogv;*.3gp;*.divx;"
        L"*.mp3;*.flac;*.wav;*.ogg;*.opus;*.m4a;*.aac;*.wma;*.ac3\0"
        L"Video Files\0"
        L"*.mp4;*.mkv;*.webm;*.avi;*.mov;*.m4v;*.wmv;*.flv;*.mpg;*.mpeg;*.ts;*.m2ts;*.mts;*.vob;*.ogv;*.3gp;*.divx\0"
        L"Audio Files\0"
        L"*.mp3;*.flac;*.wav;*.ogg;*.opus;*.m4a;*.aac;*.wma;*.ac3\0"
        L"All Files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrTitle = L"Open media in tivi";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
                OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return 0;

    int count = 0;
    wchar_t *dir = buf;
    wchar_t *first = buf + wcslen(buf) + 1;
    if (*first == 0) {
        char *u = w_to_utf8(buf);
        if (u) { on_file(u, ud); free(u); count++; }
    } else {
        for (wchar_t *f = first; *f; f += wcslen(f) + 1) {
            wchar_t full[4096];
            swprintf(full, 4096, L"%ls\\%ls", dir, f);
            full[4095] = 0;
            char *u = w_to_utf8(full);
            if (u) { on_file(u, ud); free(u); count++; }
        }
    }
    return count;
}

bool os_open_subtitle_file(char *out, int cap) {
    wchar_t buf[4096]; buf[0] = 0;
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Subtitles\0*.srt;*.ass;*.ssa;*.vtt;*.sub;*.sbv;*.lrc\0All Files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrTitle = L"Load subtitle file";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    char *u = w_to_utf8(buf);
    if (!u) return false;
    snprintf(out, cap, "%s", u);
    free(u);
    return true;
}

void os_round_window(void *hwnd, bool rounded) {
    // 33 = DWMWA_WINDOW_CORNER_PREFERENCE; 2 = DWMWCP_ROUND, 1 = DWMWCP_DONOTROUND.
    DWORD pref = rounded ? 2 : 1;
    DwmSetWindowAttribute((HWND)hwnd, 33, &pref, sizeof(pref));
}

void os_window_shadow(void *hwnd) {
    // Extend the frame by 1px so DWM paints its drop shadow even though the window
    // is borderless. Cheap way to make the floating window read as a real window.
    MARGINS m = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea((HWND)hwnd, &m);
}

char **os_args_utf8(int argc, char **argv, int *out_count) {
    (void)argc; (void)argv;
    int wc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wc);
    if (!wargv) { *out_count = 0; return NULL; }
    char **out = (char **)calloc((size_t)wc, sizeof(char *));
    if (!out) { LocalFree(wargv); *out_count = 0; return NULL; }
    for (int i = 0; i < wc; i++) out[i] = w_to_utf8(wargv[i]);
    LocalFree(wargv);
    *out_count = wc;
    return out;
}

void os_focus_window(void *hwnd) {
    HWND h = (HWND)hwnd;
    if (!h) return;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
    BringWindowToTop(h);
}

bool os_work_area(void *hwnd, int *x, int *y, int *w, int *h) {
    HMONITOR mon = MonitorFromWindow((HWND)hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfo(mon, &mi)) return false;
    if (x) *x = mi.rcWork.left;
    if (y) *y = mi.rcWork.top;
    if (w) *w = mi.rcWork.right - mi.rcWork.left;
    if (h) *h = mi.rcWork.bottom - mi.rcWork.top;
    return true;
}

void os_console_attach(void) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
}

// ---- aero snap for the borderless window ----
// Recipe: keep the window visually borderless but give it the standard frame
// styles (WS_THICKFRAME etc.) and eat the frame in WM_NCCALCSIZE, so Windows
// treats it as a normal window: drag-to-edge snap, Win+Arrow, snap layouts,
// native resize/move loops and the minimize/maximize animations all work.
static WNDPROC g_snap_prev;
static int g_snap_on = 1;
static int g_cap_left, g_cap_right, g_cap_h, g_border;

static LRESULT CALLBACK snap_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) {
            // client area = whole window. When natively maximized, Windows hangs
            // the (hidden) resize frame off-screen — inset by it so content fits.
            if (IsZoomed(h)) {
                NCCALCSIZE_PARAMS *pr = (NCCALCSIZE_PARAMS *)lp;
                int pad = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                pr->rgrc[0].left += pad; pr->rgrc[0].top += pad;
                pr->rgrc[0].right -= pad; pr->rgrc[0].bottom -= pad;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        if (!g_snap_on) break;
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(h, &pt);
        RECT rc; GetClientRect(h, &rc);
        if (!IsZoomed(h)) {
            int e = 0;
            if (pt.y < g_border)             e |= 1;
            if (pt.y >= rc.bottom - g_border) e |= 2;
            if (pt.x < g_border)             e |= 4;
            if (pt.x >= rc.right - g_border)  e |= 8;
            switch (e) {
                case 1 | 4: return HTTOPLEFT;    case 1 | 8: return HTTOPRIGHT;
                case 2 | 4: return HTBOTTOMLEFT; case 2 | 8: return HTBOTTOMRIGHT;
                case 1: return HTTOP;  case 2: return HTBOTTOM;
                case 4: return HTLEFT; case 8: return HTRIGHT;
            }
        }
        if (pt.y < g_cap_h && pt.x > g_cap_left && pt.x < rc.right - g_cap_right)
            return HTCAPTION;
        return HTCLIENT;
    }
    }
    return CallWindowProcW(g_snap_prev, h, msg, wp, lp);
}

void os_enable_snap(void *hwnd, int cap_left, int cap_right, int cap_h, int border) {
    HWND h = (HWND)hwnd;
    g_cap_left = cap_left; g_cap_right = cap_right; g_cap_h = cap_h; g_border = border;
    LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    st |= WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MAXIMIZEBOX | WS_MINIMIZEBOX;
    SetWindowLongPtrW(h, GWL_STYLE, st);
    g_snap_prev = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)snap_wndproc);
    SetWindowPos(h, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

bool os_snap_active(void) { return g_snap_prev != NULL; }
void os_snap_set_enabled(bool on) { g_snap_on = on ? 1 : 0; }
bool os_is_zoomed(void *hwnd) { return hwnd && IsZoomed((HWND)hwnd); }
void os_native_maximize_toggle(void *hwnd) {
    HWND h = (HWND)hwnd;
    ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
}

bool os_desktop_dir(char *out, int cap) {
    wchar_t w[MAX_PATH] = { 0 };
    if (SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, w) != S_OK) return false;
    char *u = w_to_utf8(w);
    if (!u) return false;
    snprintf(out, cap, "%s", u);
    free(u);
    return true;
}

int os_scan_dir_files(const char *utf8dir, void (*on_file)(const char *utf8_path, void *ud), void *ud) {
    wchar_t wdir[4096], pat[4096];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8dir, -1, wdir, 4096)) return 0;
    swprintf(pat, 4096, L"%ls\\*", wdir); pat[4095] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t full[4096];
        swprintf(full, 4096, L"%ls\\%ls", wdir, fd.cFileName); full[4095] = 0;
        char *u = w_to_utf8(full);
        if (u) { on_file(u, ud); free(u); count++; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return count;
}

#elif !defined(__APPLE__)   /* macOS lives in osvideo_mac.m */
int  os_open_media_files(void (*on_file)(const char *, void *), void *ud) { (void)on_file; (void)ud; return 0; }
bool os_open_subtitle_file(char *out, int cap) { (void)out; (void)cap; return false; }
void os_round_window(void *hwnd, bool rounded) { (void)hwnd; (void)rounded; }
void os_window_shadow(void *hwnd) { (void)hwnd; }
char **os_args_utf8(int argc, char **argv, int *out_count) { *out_count = argc; return argv; }
void os_focus_window(void *hwnd) { (void)hwnd; }
bool os_work_area(void *hwnd, int *x, int *y, int *w, int *h) { (void)hwnd;(void)x;(void)y;(void)w;(void)h; return false; }
void os_console_attach(void) {}
void os_enable_snap(void *hwnd, int cap_left, int cap_right, int cap_h, int border) { (void)hwnd;(void)cap_left;(void)cap_right;(void)cap_h;(void)border; }
bool os_snap_active(void) { return false; }
void os_snap_set_enabled(bool on) { (void)on; }
bool os_is_zoomed(void *hwnd) { (void)hwnd; return false; }
void os_native_maximize_toggle(void *hwnd) { (void)hwnd; }
int  os_scan_dir_files(const char *utf8dir, void (*on_file)(const char *utf8_path, void *ud), void *ud) { (void)utf8dir;(void)on_file;(void)ud; return 0; }
bool os_desktop_dir(char *out, int cap) { (void)out;(void)cap; return false; }
#endif

// Finder open-events only exist on macOS (implemented in osvideo_mac.m).
#ifndef __APPLE__
void os_open_files_handler_install(void) {}
#endif
