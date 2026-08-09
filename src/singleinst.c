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

#else  // ---- POSIX (macOS / Linux): Unix domain socket in the config dir ----

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>

#define SI_MAXPATH 4096
#define SI_QDEPTH  32

static int             g_listen_fd = -1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char            g_queue[SI_QDEPTH][SI_MAXPATH];
static int             g_qhead, g_qtail;       // ring buffer (head==tail → empty)
static int             g_focus_pending;
static char            g_sock_path[512];

static const char *sock_path(void) {
    if (!g_sock_path[0]) {
        const char *home = getenv("HOME");
        if (!home || !*home) home = "/tmp";
        snprintf(g_sock_path, sizeof(g_sock_path), "%s/.tivi", home);
        mkdir(g_sock_path, 0755);
        size_t n = strlen(g_sock_path);
        snprintf(g_sock_path + n, sizeof(g_sock_path) - n, "/tivi.sock");
    }
    return g_sock_path;
}

static int sock_connect(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a; memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path());
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return -1; }
    return fd;
}

bool singleinst_acquire(void) {
    // A live socket means a running tivi; a dead file is a stale crash leftover.
    int probe = sock_connect();
    if (probe >= 0) { close(probe); return false; }
    unlink(sock_path());

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return true;   // can't own the socket — behave standalone
    struct sockaddr_un a; memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path());
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return true;
    }
    g_listen_fd = fd;
    return true;
}

static bool send_line(char tag, const char *payload) {
    int fd = sock_connect();
    if (fd < 0) return false;
    char buf[SI_MAXPATH + 8];
    int n = snprintf(buf, sizeof(buf), "%c%s\n", tag, payload ? payload : "");
    bool ok = write(fd, buf, (size_t)n) == n;
    close(fd);
    return ok;
}

bool singleinst_send_file(const char *utf8_path) { return send_line('F', utf8_path); }
bool singleinst_send_focus(void)                 { return send_line('R', NULL); }

static void queue_push(const char *path) {
    pthread_mutex_lock(&g_lock);
    int next = (g_qtail + 1) % SI_QDEPTH;
    if (next != g_qhead) {
        snprintf(g_queue[g_qtail], SI_MAXPATH, "%s", path);
        g_qtail = next;
    }
    pthread_mutex_unlock(&g_lock);
}

static void *listen_thread(void *p) {
    (void)p;
    for (;;) {
        int c = accept(g_listen_fd, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; break; }
        char buf[SI_MAXPATH + 8]; int total = 0;
        for (;;) {
            ssize_t r = read(c, buf + total, sizeof(buf) - 1 - total);
            if (r <= 0) break;
            total += (int)r;
            if (total >= (int)sizeof(buf) - 1) break;
        }
        close(c);
        buf[total] = 0;
        // one message per line: F<path> = play this file, R = raise the window
        char *line = buf;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (line[0] == 'F' && line[1]) queue_push(line + 1);
            else if (line[0] == 'R') {
                pthread_mutex_lock(&g_lock);
                g_focus_pending = 1;
                pthread_mutex_unlock(&g_lock);
            }
            line = nl ? nl + 1 : NULL;
        }
        // any handed-off song should also raise the window, matching Windows
        pthread_mutex_lock(&g_lock);
        g_focus_pending = 1;
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

void singleinst_listen_start(void) {
    if (g_listen_fd < 0) return;
    pthread_t t;
    if (pthread_create(&t, NULL, listen_thread, NULL) == 0) pthread_detach(t);
}

bool singleinst_poll_file(char *out, int cap) {
    bool got = false;
    pthread_mutex_lock(&g_lock);
    if (g_qhead != g_qtail) {
        int n = (int)strlen(g_queue[g_qhead]);
        if (n >= cap) n = cap - 1;
        if (n > 0) memcpy(out, g_queue[g_qhead], (size_t)n);
        out[n] = 0;
        g_qhead = (g_qhead + 1) % SI_QDEPTH;
        got = true;
    }
    pthread_mutex_unlock(&g_lock);
    return got;
}

bool singleinst_poll_focus(void) {
    pthread_mutex_lock(&g_lock);
    int f = g_focus_pending;
    g_focus_pending = 0;
    pthread_mutex_unlock(&g_lock);
    return f != 0;
}

#endif
