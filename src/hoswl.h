/* hoswl.h — Hisashi OS Window Layer client. Single header, C99 / C++.
 *
 *   #define HOSWL_IMPLEMENTATION      (in exactly ONE .c / .cpp file)
 *   #include "hoswl.h"
 *
 * Publishes your app's menus to Hisashi's macOS-style menubar over the named
 * pipe \\.\pipe\hoswl and hands back the ids the user clicks. No threads, no
 * allocation, never blocks: call hoswl_poll() once per frame / tick.
 *
 *   static hoswl_t h;
 *   hoswl_init(&h, "com.example.app", "Example", "1.0");
 *   hoswl_set_menus(&h,
 *       "File\n"
 *       " file.new|New|Ctrl+N\n"
 *       " -\n"
 *       " file.quit|Quit|Ctrl+Q\n"
 *       "View\n"
 *       " view.wrap|Word Wrap||x\n");
 *   for (;;) {
 *       const char* id;
 *       while ((id = hoswl_poll(&h)) != NULL) on_menu(id);
 *       ...
 *   }
 *   hoswl_shutdown(&h);
 *
 * Menu text (one line per row):
 *   no indent      a top-level menu: "Label" or "id|Label" (id defaults to a
 *                  lowercase slug of the label)
 *   one space      an item: "id|Label|Key|flags" — Key and flags optional.
 *                  flags: d = disabled, c = checkable (unchecked), x = checked
 *   " -"           a separator
 *   " id|Label|>"  a submenu header; its children are indented one more space
 *
 * Win32 only. On other platforms every function compiles to a harmless no-op
 * (menus are cached, poll returns NULL) so callers need no #ifdef.
 *
 * Protocol reference: Hisashi/docs/hoswl-protocol.md
 * License: MIT (same as Hisashi)
 */
#ifndef HOSWL_H
#define HOSWL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOSWL_VERSION    "1.0.0"
#define HOSWL_PROTOCOL   1
#ifndef HOSWL_PIPE
#define HOSWL_PIPE       "\\\\.\\pipe\\hoswl"   /* override before including to talk to a test server */
#endif
#define HOSWL_MENU_MAX   61440   /* bytes of compiled menu JSON */
#define HOSWL_RECV_MAX   4096    /* bytes buffered from Hisashi */
#define HOSWL_ID_MAX     128     /* longest item id, incl. NUL */
#define HOSWL_RETRY_MS   2000    /* reconnect cadence while Hisashi is absent */

/* Treat as opaque; fields are public only so the struct can live in static
 * storage without a malloc. */
typedef struct hoswl {
    char   app_id[128];
    char   name[128];
    char   version[64];
    int    enabled;               /* the protocol's "enable" flag (default 1) */
    int    inited;
    int    connected;
    void*  pipe;                  /* HANDLE on Win32 */
    unsigned long next_retry;     /* GetTickCount() deadline for the next connect attempt */
    char   menu_line[HOSWL_MENU_MAX + 64];   /* cached {"t":"menu",...}\n line */
    size_t menu_len;
    char   rbuf[HOSWL_RECV_MAX];
    size_t rlen;
    char   click_id[HOSWL_ID_MAX];
    char   last_error[160];
} hoswl_t;

/* Record identity. Does not connect — hoswl_poll() does, lazily and non-blocking.
 * Returns 0, or -1 when app_id / name is NULL or empty. */
int hoswl_init(hoswl_t* h, const char* app_id, const char* name, const char* version);

/* Compile the menu text described above, cache it, and send it if connected.
 * Returns 0, or -1 with the reason in h->last_error (the cached menu is dropped). */
int hoswl_set_menus(hoswl_t* h, const char* menu_text);

/* Same, but you supply the protocol's "menus" JSON array yourself. */
int hoswl_set_menus_json(hoswl_t* h, const char* menus_json_array);

/* Patch one item's state without resending the tree. Pass -1 to leave a field
 * unchanged. Returns 0 (also when not connected), -1 if the write failed. */
int hoswl_set_item(hoswl_t* h, const char* id, int enabled, int check);

/* Your app's own integration switch. Off keeps the connection but Hisashi shows
 * nothing for you. Remembered across reconnects. */
int hoswl_set_enabled(hoswl_t* h, int on);

int hoswl_connected(const hoswl_t* h);

/* Connect / reconnect when due, drain incoming lines, and return the next clicked
 * item id (valid until the next call) or NULL. Never blocks. */
const char* hoswl_poll(hoswl_t* h);

/* Send bye and close. The struct can be hoswl_init()ed again afterwards. */
void hoswl_shutdown(hoswl_t* h);

/* --- Pure helpers, exposed for tests and for apps that build JSON themselves --- */

/* JSON-escape s into out (always NUL-terminated, truncates). Returns chars written. */
size_t hoswl_json_escape(char* out, size_t cap, const char* s);

/* Menu text → JSON array. Returns 0, or -1 with a message in err. */
int hoswl_compile_menu_text(const char* text, char* out, size_t cap, char* err, size_t errcap);

/* 1 and fills id_out when line is a {"t":"click","id":...} message, else 0. */
int hoswl_parse_click(const char* line, char* id_out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* HOSWL_H */

/* ========================================================================== */
#ifdef HOSWL_IMPLEMENTATION

#include <stdio.h>
#include <string.h>

/* ------------------------------ JSON escape ------------------------------ */

size_t hoswl_json_escape(char* out, size_t cap, const char* s)
{
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    if (!out || cap == 0) return 0;
    if (!s) s = "";
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        const char* rep = NULL;
        char ubuf[7];
        switch (c) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n";  break;
            case '\r': rep = "\\r";  break;
            case '\t': rep = "\\t";  break;
            case '\b': rep = "\\b";  break;
            case '\f': rep = "\\f";  break;
            default:
                if (c < 0x20) {
                    ubuf[0] = '\\'; ubuf[1] = 'u'; ubuf[2] = '0'; ubuf[3] = '0';
                    ubuf[4] = hex[c >> 4]; ubuf[5] = hex[c & 15]; ubuf[6] = 0;
                    rep = ubuf;
                }
                break;
        }
        if (rep) {
            size_t n = strlen(rep);
            if (o + n >= cap) break;
            memcpy(out + o, rep, n);
            o += n;
        } else {
            if (o + 1 >= cap) break;
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
    return o;
}

/* ------------------------------ menu text -------------------------------- */

typedef struct {
    char*  out;
    size_t cap;
    size_t len;
    int    overflow;
} hoswl__writer;

static void hoswl__put(hoswl__writer* w, const char* s)
{
    size_t n = strlen(s);
    if (w->overflow) return;
    if (w->len + n >= w->cap) { w->overflow = 1; return; }
    memcpy(w->out + w->len, s, n);
    w->len += n;
    w->out[w->len] = 0;
}

static void hoswl__put_escaped(hoswl__writer* w, const char* s)
{
    if (w->overflow) return;
    size_t room = w->cap - w->len;
    /* Worst case is 6x expansion; check with a dry run rather than guess. */
    size_t n = hoswl_json_escape(w->out + w->len, room, s);
    /* Detect truncation: re-escape length must equal what fits. */
    {
        size_t need = 0;
        const char* p;
        for (p = s; *p; ++p) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' || c == '\b' || c == '\f') need += 2;
            else if (c < 0x20) need += 6;
            else need += 1;
        }
        if (need != n) { w->overflow = 1; w->out[w->len] = 0; return; }
    }
    w->len += n;
}

static void hoswl__slug(const char* label, char* out, size_t cap)
{
    size_t o = 0;
    int pending_dash = 0;
    for (; *label && o + 1 < cap; ++label) {
        unsigned char c = (unsigned char)*label;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (pending_dash && o) out[o++] = '-';
            pending_dash = 0;
            if (o + 1 < cap) out[o++] = (char)c;
        } else if (c >= 'A' && c <= 'Z') {
            if (pending_dash && o) out[o++] = '-';
            pending_dash = 0;
            if (o + 1 < cap) out[o++] = (char)(c - 'A' + 'a');
        } else {
            pending_dash = 1;
        }
    }
    out[o] = 0;
    if (o == 0) { out[0] = 'm'; out[1] = 0; }
}

static void hoswl__err(char* err, size_t errcap, int line, const char* msg)
{
    if (err && errcap) snprintf(err, errcap, "line %d: %s", line, msg);
}

/* Bounded copy that truncates silently (the DSL caps ids/labels by design). */
static void hoswl__ncopy(char* dst, size_t cap, const char* src)
{
    size_t o = 0;
    if (!cap) return;
    while (src[o] && o + 1 < cap) { dst[o] = src[o]; ++o; }
    dst[o] = 0;
}

int hoswl_compile_menu_text(const char* text, char* out, size_t cap, char* err, size_t errcap)
{
    hoswl__writer w;
    int   stack_depth[16];      /* container depths; children sit one deeper */
    int   stack_first[16];      /* 1 until the first child has been emitted */
    int   sp = 0;               /* number of open containers */
    int   menus = 0;
    int   lineno = 0;
    const char* p = text ? text : "";

    if (!out || cap < 3) { hoswl__err(err, errcap, 0, "buffer too small"); return -1; }
    w.out = out; w.cap = cap; w.len = 0; w.overflow = 0;
    out[0] = 0;
    hoswl__put(&w, "[");

    while (*p) {
        const char* eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        char* s;
        char* end;
        int depth = 0;
        ++lineno;
        if (len >= sizeof line) { hoswl__err(err, errcap, lineno, "line too long"); return -1; }
        memcpy(line, p, len);
        line[len] = 0;
        p = eol ? eol + 1 : p + len;

        /* trim trailing \r and spaces, count leading spaces */
        end = line + len;
        while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        s = line;
        while (*s == ' ') { ++s; ++depth; }
        if (!*s) continue;

        if (depth == 0) {
            /* ---- top-level menu ---- */
            char id[128], label[512];
            char* bar = strchr(s, '|');
            while (sp > 0) { hoswl__put(&w, "]}"); --sp; }
            if (bar) {
                *bar = 0;
                hoswl__ncopy(id, sizeof id, s);
                hoswl__ncopy(label, sizeof label, bar + 1);
            } else {
                hoswl__ncopy(label, sizeof label, s);
                hoswl__slug(s, id, sizeof id);
            }
            if (!id[0])    { hoswl__err(err, errcap, lineno, "empty menu id"); return -1; }
            if (!label[0]) { hoswl__err(err, errcap, lineno, "empty menu label"); return -1; }
            if (menus++) hoswl__put(&w, ",");
            hoswl__put(&w, "{\"id\":\"");  hoswl__put_escaped(&w, id);
            hoswl__put(&w, "\",\"label\":\""); hoswl__put_escaped(&w, label);
            hoswl__put(&w, "\",\"items\":[");
            stack_depth[0] = 0; stack_first[0] = 1; sp = 1;
        } else {
            /* ---- item / separator / submenu header ---- */
            char* f[4] = { NULL, NULL, NULL, NULL };
            int nf = 0;
            if (sp == 0) { hoswl__err(err, errcap, lineno, "item before any menu"); return -1; }
            while (sp > 0 && stack_depth[sp - 1] >= depth) { hoswl__put(&w, "]}"); --sp; }
            if (sp == 0 || stack_depth[sp - 1] != depth - 1) {
                hoswl__err(err, errcap, lineno, "indent jumps more than one level"); return -1;
            }
            if (!stack_first[sp - 1]) hoswl__put(&w, ",");
            stack_first[sp - 1] = 0;

            if (strcmp(s, "-") == 0) { hoswl__put(&w, "{\"sep\":true}"); continue; }

            /* split on '|' into at most 4 fields */
            f[nf++] = s;
            {
                char* q = s;
                while (nf < 4 && (q = strchr(q, '|')) != NULL) { *q++ = 0; f[nf++] = q; }
            }
            if (!f[0][0])            { hoswl__err(err, errcap, lineno, "empty item id"); return -1; }
            if (!f[1] || !f[1][0])   { hoswl__err(err, errcap, lineno, "empty item label"); return -1; }

            hoswl__put(&w, "{\"id\":\"");  hoswl__put_escaped(&w, f[0]);
            hoswl__put(&w, "\",\"label\":\""); hoswl__put_escaped(&w, f[1]);
            hoswl__put(&w, "\"");

            if (f[2] && strcmp(f[2], ">") == 0) {
                /* submenu header: children follow, one level deeper */
                if (sp >= 16) { hoswl__err(err, errcap, lineno, "menus nested too deep"); return -1; }
                hoswl__put(&w, ",\"items\":[");
                stack_depth[sp] = depth; stack_first[sp] = 1; ++sp;
                continue;
            }
            if (f[2] && f[2][0]) {
                hoswl__put(&w, ",\"key\":\""); hoswl__put_escaped(&w, f[2]); hoswl__put(&w, "\"");
            }
            if (f[3]) {
                if (strchr(f[3], 'd')) hoswl__put(&w, ",\"enabled\":false");
                if (strchr(f[3], 'x')) hoswl__put(&w, ",\"check\":true");
                else if (strchr(f[3], 'c')) hoswl__put(&w, ",\"check\":false");
            }
            hoswl__put(&w, "}");
        }
        if (w.overflow) { hoswl__err(err, errcap, lineno, "buffer too small"); return -1; }
    }
    while (sp > 0) { hoswl__put(&w, "]}"); --sp; }
    hoswl__put(&w, "]");
    if (w.overflow) { hoswl__err(err, errcap, lineno, "buffer too small"); return -1; }
    return 0;
}

/* ------------------------------ click parse ------------------------------ */

static int hoswl__hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t hoswl__put_utf8(char* out, size_t cap, size_t o, unsigned cp)
{
    unsigned char b[4];
    size_t n;
    if (cp < 0x80)         { b[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800)   { b[0] = (unsigned char)(0xC0 | (cp >> 6)); b[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { b[0] = (unsigned char)(0xE0 | (cp >> 12)); b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3; }
    else                   { b[0] = (unsigned char)(0xF0 | (cp >> 18)); b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4; }
    if (o + n >= cap) return o;
    memcpy(out + o, b, n);
    return o + n;
}

/* Find "key":"value" anywhere in line (whitespace around ':' allowed) and
 * unescape value into out. Returns 1 when found. */
static int hoswl__find_str_field(const char* line, const char* key, char* out, size_t cap)
{
    char pat[40];
    const char* p = line;
    size_t klen;
    if (!line || !out || cap == 0) return 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    klen = strlen(pat);
    while ((p = strstr(p, pat)) != NULL) {
        const char* q = p + klen;
        size_t o = 0;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q != ':') { p = q; continue; }
        ++q;
        while (*q == ' ' || *q == '\t') ++q;
        if (*q != '"') { p = q; continue; }
        ++q;
        while (*q && *q != '"') {
            if (*q == '\\') {
                ++q;
                switch (*q) {
                    case '"':  if (o + 1 < cap) out[o++] = '"';  ++q; break;
                    case '\\': if (o + 1 < cap) out[o++] = '\\'; ++q; break;
                    case '/':  if (o + 1 < cap) out[o++] = '/';  ++q; break;
                    case 'n':  if (o + 1 < cap) out[o++] = '\n'; ++q; break;
                    case 'r':  if (o + 1 < cap) out[o++] = '\r'; ++q; break;
                    case 't':  if (o + 1 < cap) out[o++] = '\t'; ++q; break;
                    case 'b':  if (o + 1 < cap) out[o++] = '\b'; ++q; break;
                    case 'f':  if (o + 1 < cap) out[o++] = '\f'; ++q; break;
                    case 'u': {
                        unsigned cp = 0;
                        int i, ok = 1;
                        for (i = 1; i <= 4; ++i) {
                            int v = hoswl__hexval(q[i]);
                            if (v < 0) { ok = 0; break; }
                            cp = (cp << 4) | (unsigned)v;
                        }
                        if (!ok) { out[0] = 0; return 0; }
                        q += 5;
                        if (cp >= 0xD800 && cp <= 0xDBFF && q[0] == '\\' && q[1] == 'u') {
                            unsigned lo = 0;
                            ok = 1;
                            for (i = 2; i <= 5; ++i) {
                                int v = hoswl__hexval(q[i]);
                                if (v < 0) { ok = 0; break; }
                                lo = (lo << 4) | (unsigned)v;
                            }
                            if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                q += 6;
                            }
                        }
                        o = hoswl__put_utf8(out, cap, o, cp);
                        break;
                    }
                    default:   out[0] = 0; return 0;
                }
            } else {
                if (o + 1 < cap) out[o++] = *q;
                ++q;
            }
        }
        if (*q != '"') { out[0] = 0; return 0; }
        out[o] = 0;
        return 1;
    }
    return 0;
}

int hoswl_parse_click(const char* line, char* id_out, size_t cap)
{
    char t[16];
    if (!hoswl__find_str_field(line, "t", t, sizeof t)) return 0;
    if (strcmp(t, "click") != 0) return 0;
    if (!hoswl__find_str_field(line, "id", id_out, cap)) return 0;
    return id_out[0] != 0;
}

/* ------------------------------ transport -------------------------------- */

#ifdef _WIN32
#ifndef _WINDOWS_
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

static unsigned long hoswl__now(void) { return (unsigned long)GetTickCount(); }

static void hoswl__disconnect(hoswl_t* h)
{
    if (h->pipe && h->pipe != (void*)INVALID_HANDLE_VALUE) CloseHandle((HANDLE)h->pipe);
    h->pipe = (void*)INVALID_HANDLE_VALUE;
    h->connected = 0;
    h->rlen = 0;
    h->next_retry = hoswl__now() + HOSWL_RETRY_MS;
}

static int hoswl__write(hoswl_t* h, const char* s, size_t n)
{
    if (!h->connected) return -1;
    while (n) {
        DWORD w = 0;
        if (!WriteFile((HANDLE)h->pipe, s, (DWORD)n, &w, NULL)) {
            snprintf(h->last_error, sizeof h->last_error, "write failed (error %lu)", (unsigned long)GetLastError());
            hoswl__disconnect(h);
            return -1;
        }
        s += w; n -= w;
    }
    return 0;
}

static int hoswl__try_connect(hoswl_t* h)
{
    unsigned long now = hoswl__now();
    HANDLE p;
    DWORD mode = PIPE_READMODE_BYTE;
    char e_app[300], e_name[300], e_ver[160], line[1024];
    int n;

    if ((long)(now - h->next_retry) < 0) return 0;
    p = CreateFileA(HOSWL_PIPE, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (p == INVALID_HANDLE_VALUE) {
        /* ERROR_FILE_NOT_FOUND: Hisashi isn't running. ERROR_PIPE_BUSY: it is
         * between accepts. Either way, try again later — never wait here. */
        h->next_retry = now + HOSWL_RETRY_MS;
        return 0;
    }
    SetNamedPipeHandleState(p, &mode, NULL, NULL);
    h->pipe = (void*)p;
    h->connected = 1;
    h->rlen = 0;
    h->last_error[0] = 0;

    hoswl_json_escape(e_app,  sizeof e_app,  h->app_id);
    hoswl_json_escape(e_name, sizeof e_name, h->name);
    hoswl_json_escape(e_ver,  sizeof e_ver,  h->version);
    n = snprintf(line, sizeof line,
        "{\"t\":\"hello\",\"v\":%d,\"app\":\"%s\",\"name\":\"%s\",\"ver\":\"%s\",\"pid\":%lu}\n",
        HOSWL_PROTOCOL, e_app, e_name, e_ver, (unsigned long)GetCurrentProcessId());
    if (n < 0 || (size_t)n >= sizeof line) { hoswl__disconnect(h); return 0; }
    if (hoswl__write(h, line, (size_t)n) != 0) return 0;
    if (h->menu_len && hoswl__write(h, h->menu_line, h->menu_len) != 0) return 0;
    if (!h->enabled) {
        const char* en = "{\"t\":\"enable\",\"on\":false}\n";
        if (hoswl__write(h, en, strlen(en)) != 0) return 0;
    }
    return 1;
}

/* 1 = read something, 0 = nothing pending, -1 = disconnected */
static int hoswl__read_more(hoswl_t* h)
{
    DWORD avail = 0, want, got = 0;
    if (!PeekNamedPipe((HANDLE)h->pipe, NULL, 0, NULL, &avail, NULL)) {
        snprintf(h->last_error, sizeof h->last_error, "pipe closed (error %lu)", (unsigned long)GetLastError());
        hoswl__disconnect(h);
        return -1;
    }
    if (avail == 0) return 0;
    if (h->rlen >= sizeof h->rbuf - 1) h->rlen = 0;   /* oversize line without '\n': drop it */
    want = (DWORD)(sizeof h->rbuf - 1 - h->rlen);
    if (want > avail) want = avail;
    if (!ReadFile((HANDLE)h->pipe, h->rbuf + h->rlen, want, &got, NULL)) {
        snprintf(h->last_error, sizeof h->last_error, "read failed (error %lu)", (unsigned long)GetLastError());
        hoswl__disconnect(h);
        return -1;
    }
    h->rlen += got;
    return got > 0 ? 1 : 0;
}
#else   /* !_WIN32: stubs */
static void hoswl__disconnect(hoswl_t* h) { h->connected = 0; h->rlen = 0; }
static int  hoswl__write(hoswl_t* h, const char* s, size_t n) { (void)h; (void)s; (void)n; return -1; }
static int  hoswl__try_connect(hoswl_t* h) { (void)h; return 0; }
static int  hoswl__read_more(hoswl_t* h) { (void)h; return 0; }
#endif

/* ------------------------------ public API ------------------------------- */

static void hoswl__copy(char* dst, size_t cap, const char* src)
{
    if (!src) src = "";
    snprintf(dst, cap, "%s", src);
}

int hoswl_init(hoswl_t* h, const char* app_id, const char* name, const char* version)
{
    if (!h) return -1;
    memset(h, 0, sizeof *h);
#ifdef _WIN32
    h->pipe = (void*)INVALID_HANDLE_VALUE;
#endif
    if (!app_id || !*app_id || !name || !*name) {
        snprintf(h->last_error, sizeof h->last_error, "app_id and name are required");
        return -1;
    }
    hoswl__copy(h->app_id,  sizeof h->app_id,  app_id);
    hoswl__copy(h->name,    sizeof h->name,    name);
    hoswl__copy(h->version, sizeof h->version, version ? version : "");
    h->enabled = 1;
    h->inited = 1;
    h->next_retry = 0;
    return 0;
}

static int hoswl__store_menu_json(hoswl_t* h, const char* json)
{
    static const char pre[] = "{\"t\":\"menu\",\"menus\":";
    size_t pl = sizeof pre - 1, jl = strlen(json);
    if (pl + jl + 3 > sizeof h->menu_line) {
        snprintf(h->last_error, sizeof h->last_error, "menu JSON exceeds %d bytes", HOSWL_MENU_MAX);
        h->menu_len = 0;
        return -1;
    }
    memcpy(h->menu_line, pre, pl);
    memcpy(h->menu_line + pl, json, jl);
    h->menu_line[pl + jl] = '}';
    h->menu_line[pl + jl + 1] = '\n';
    h->menu_line[pl + jl + 2] = 0;
    h->menu_len = pl + jl + 2;
    if (h->connected) return hoswl__write(h, h->menu_line, h->menu_len);
    return 0;
}

int hoswl_set_menus(hoswl_t* h, const char* menu_text)
{
    static const char pre[] = "{\"t\":\"menu\",\"menus\":";
    size_t pl = sizeof pre - 1;
    if (!h || !h->inited) return -1;
    /* Compile straight into the cached line after the prefix, then seal it. */
    if (hoswl_compile_menu_text(menu_text, h->menu_line + pl, HOSWL_MENU_MAX - pl,
                                h->last_error, sizeof h->last_error) != 0) {
        h->menu_len = 0;
        return -1;
    }
    {
        size_t jl = strlen(h->menu_line + pl);
        memcpy(h->menu_line, pre, pl);
        h->menu_line[pl + jl] = '}';
        h->menu_line[pl + jl + 1] = '\n';
        h->menu_line[pl + jl + 2] = 0;
        h->menu_len = pl + jl + 2;
    }
    if (h->connected) return hoswl__write(h, h->menu_line, h->menu_len);
    return 0;
}

int hoswl_set_menus_json(hoswl_t* h, const char* menus_json_array)
{
    if (!h || !h->inited || !menus_json_array) return -1;
    return hoswl__store_menu_json(h, menus_json_array);
}

int hoswl_set_item(hoswl_t* h, const char* id, int enabled, int check)
{
    char e_id[HOSWL_ID_MAX * 6], line[HOSWL_ID_MAX * 6 + 96];
    int n;
    if (!h || !h->inited || !id) return -1;
    if (!h->connected) return 0;
    hoswl_json_escape(e_id, sizeof e_id, id);
    n = snprintf(line, sizeof line, "{\"t\":\"set\",\"id\":\"%s\"%s%s}\n", e_id,
        enabled < 0 ? "" : (enabled ? ",\"enabled\":true" : ",\"enabled\":false"),
        check   < 0 ? "" : (check   ? ",\"check\":true"   : ",\"check\":false"));
    if (n < 0 || (size_t)n >= sizeof line) return -1;
    return hoswl__write(h, line, (size_t)n);
}

int hoswl_set_enabled(hoswl_t* h, int on)
{
    if (!h || !h->inited) return -1;
    h->enabled = on ? 1 : 0;
    if (!h->connected) return 0;
    {
        const char* line = on ? "{\"t\":\"enable\",\"on\":true}\n" : "{\"t\":\"enable\",\"on\":false}\n";
        return hoswl__write(h, line, strlen(line));
    }
}

int hoswl_connected(const hoswl_t* h)
{
    return h && h->inited && h->connected;
}

const char* hoswl_poll(hoswl_t* h)
{
    if (!h || !h->inited) return NULL;
    if (!h->connected && !hoswl__try_connect(h)) return NULL;
    for (;;) {
        char* nl = (char*)memchr(h->rbuf, '\n', h->rlen);
        if (nl) {
            size_t linelen = (size_t)(nl - h->rbuf);
            size_t rest = h->rlen - linelen - 1;
            int is_click;
            *nl = 0;
            is_click = hoswl_parse_click(h->rbuf, h->click_id, sizeof h->click_id);
            memmove(h->rbuf, nl + 1, rest);
            h->rlen = rest;
            if (is_click) return h->click_id;
            continue;
        }
        if (hoswl__read_more(h) <= 0) return NULL;
    }
}

void hoswl_shutdown(hoswl_t* h)
{
    if (!h || !h->inited) return;
    if (h->connected) {
        const char* bye = "{\"t\":\"bye\"}\n";
        hoswl__write(h, bye, strlen(bye));
    }
    hoswl__disconnect(h);
    h->inited = 0;
}

#endif /* HOSWL_IMPLEMENTATION */
