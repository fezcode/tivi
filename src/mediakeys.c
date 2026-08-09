#include "mediakeys.h"

#ifdef _WIN32
#include <windows.h>

static volatile LONG g_action = MK_NONE;
static HHOOK g_hook;

// Low-level keyboard hook: catches the transport keys system-wide regardless of
// focus, and — unlike RegisterHotKey — coexists with other media apps instead of
// failing silently when they already own the keys. We never consume the key
// (always CallNextHookEx), so the OS keeps its normal routing.
static LRESULT CALLBACK ll_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lparam;
        LONG act = MK_NONE;
        switch (k->vkCode) {
            case VK_MEDIA_PLAY_PAUSE: act = MK_PLAYPAUSE; break;
            case VK_MEDIA_STOP:       act = MK_STOP;      break;
            case VK_MEDIA_PREV_TRACK: act = MK_PREV;      break;
            case VK_MEDIA_NEXT_TRACK: act = MK_NEXT;      break;
            default: break;
        }
        if (act != MK_NONE) InterlockedExchange(&g_action, act);
    }
    return CallNextHookEx(g_hook, code, wparam, lparam);
}

static DWORD WINAPI mk_thread(LPVOID p) {
    (void)p;
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, ll_proc, GetModuleHandleW(NULL), 0);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) { /* pump so the hook keeps firing */ }
    if (g_hook) UnhookWindowsHookEx(g_hook);
    return 0;
}

void mediakeys_start(void) {
    HANDLE h = CreateThread(NULL, 0, mk_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
int mediakeys_poll(void) { return (int)InterlockedExchange(&g_action, MK_NONE); }

#elif !defined(__APPLE__)   /* macOS lives in mediakeys_mac.m */
void mediakeys_start(void) {}
int  mediakeys_poll(void) { return 0; }
#endif
