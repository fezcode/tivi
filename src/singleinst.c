#include "singleinst.h"

#ifdef _WIN32
#include <windows.h>
#include <string.h>

#define SI_MUTEX_NAME  L"Local\\TiviSingleInstance_io.tivi.tivi"
#define SI_CLASS_NAME  L"TiviSingleInstanceWnd"
#define SI_MAGIC       0x54495649u   // 'TIVI' — WM_COPYDATA dwData tag

#define SI_MAXPATH 4096
#define SI_QDEPTH  32

static HANDLE          g_mutex;
static CRITICAL_SECTION g_lock;
static int             g_lock_inited;
static char            g_queue[SI_QDEPTH][SI_MAXPATH];
static int             g_qhead, g_qtail;       // ring buffer (head==tail → empty)
static volatile LONG   g_focus_pending;

static void si_push_path(const char *utf8, int len) {
    if (len <= 0 || len >= SI_MAXPATH) return;
    EnterCriticalSection(&g_lock);
    int next = (g_qtail + 1) % SI_QDEPTH;
    if (next != g_qhead) {                      // drop silently if the ring is full
        memcpy(g_queue[g_qtail], utf8, (size_t)len);
        g_queue[g_qtail][len] = 0;
        g_qtail = next;
    }
    LeaveCriticalSection(&g_lock);
}

static LRESULT CALLBACK si_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COPYDATA) {
        COPYDATASTRUCT *cds = (COPYDATASTRUCT *)lp;
        if (cds && cds->dwData == SI_MAGIC) {
            InterlockedExchange(&g_focus_pending, 1);   // any message also raises the window
            if (cds->cbData > 0 && cds->lpData)
                si_push_path((const char *)cds->lpData, (int)cds->cbData - 1); // cbData includes NUL
        }
        return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static DWORD WINAPI si_listen_thread(LPVOID p) {
    (void)p;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = si_wndproc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = SI_CLASS_NAME;
    RegisterClassW(&wc);
    HWND win = CreateWindowExW(0, SI_CLASS_NAME, L"tivi", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    if (!win) return 0;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static HWND si_find_listener(void) {
    for (int i = 0; i < 50; i++) {
        HWND w = FindWindowExW(HWND_MESSAGE, NULL, SI_CLASS_NAME, NULL);
        if (w) return w;
        Sleep(100);
    }
    return NULL;
}

static bool si_send(const char *utf8_path) {
    HWND w = si_find_listener();
    if (!w) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid) AllowSetForegroundWindow(pid);

    COPYDATASTRUCT cds;
    cds.dwData = SI_MAGIC;
    if (utf8_path && utf8_path[0]) {
        cds.cbData = (DWORD)strlen(utf8_path) + 1;   // include the NUL
        cds.lpData = (PVOID)utf8_path;
    } else {
        cds.cbData = 0;
        cds.lpData = NULL;
    }
    LRESULT r = SendMessageW(w, WM_COPYDATA, 0, (LPARAM)&cds);
    return r == TRUE;
}

bool singleinst_acquire(void) {
    if (!g_lock_inited) { InitializeCriticalSection(&g_lock); g_lock_inited = 1; }
    g_mutex = CreateMutexW(NULL, FALSE, SI_MUTEX_NAME);
    return GetLastError() != ERROR_ALREADY_EXISTS;
}

bool singleinst_send_file(const char *utf8_path) { return si_send(utf8_path); }
bool singleinst_send_focus(void)                 { return si_send(NULL); }

void singleinst_listen_start(void) {
    HANDLE h = CreateThread(NULL, 0, si_listen_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}

bool singleinst_poll_file(char *out, int cap) {
    bool got = false;
    EnterCriticalSection(&g_lock);
    if (g_qhead != g_qtail) {
        int n = (int)strlen(g_queue[g_qhead]);
        if (n >= cap) n = cap - 1;
        if (n > 0) memcpy(out, g_queue[g_qhead], (size_t)n);
        out[n] = 0;
        g_qhead = (g_qhead + 1) % SI_QDEPTH;
        got = true;
    }
    LeaveCriticalSection(&g_lock);
    return got;
}

bool singleinst_poll_focus(void) {
    return InterlockedExchange(&g_focus_pending, 0) != 0;
}

#else  // ---- non-Windows: every launch is its own instance ----

bool singleinst_acquire(void)                    { return true; }
bool singleinst_send_file(const char *utf8_path) { (void)utf8_path; return false; }
bool singleinst_send_focus(void)                 { return false; }
void singleinst_listen_start(void)               {}
bool singleinst_poll_file(char *out, int cap)    { (void)out; (void)cap; return false; }
bool singleinst_poll_focus(void)                 { return false; }

#endif
