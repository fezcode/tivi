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

#else
int  os_open_media_files(void (*on_file)(const char *, void *), void *ud) { (void)on_file; (void)ud; return 0; }
bool os_open_subtitle_file(char *out, int cap) { (void)out; (void)cap; return false; }
void os_round_window(void *hwnd, bool rounded) { (void)hwnd; (void)rounded; }
void os_window_shadow(void *hwnd) { (void)hwnd; }
char **os_args_utf8(int argc, char **argv, int *out_count) { *out_count = argc; return argv; }
void os_focus_window(void *hwnd) { (void)hwnd; }
bool os_work_area(void *hwnd, int *x, int *y, int *w, int *h) { (void)hwnd;(void)x;(void)y;(void)w;(void)h; return false; }
void os_console_attach(void) {}
#endif
