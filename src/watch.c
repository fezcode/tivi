#include "watch.h"
#include "viconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Line format:  position|duration|sub_delay_ms|last_seen_unix|path
// The path comes last so it can hold anything but a newline — separators,
// spaces, '=' and '|' all survive the round trip.

#define WATCH_MAX  400     // files remembered; the least recently seen are dropped
#define LINE_MAX  1400

typedef struct {
    char      *path;
    WatchEntry e;
    long long  seen;       // unix time of the last update — the prune key
} Rec;

static Rec  *g_rec;
static int   g_n, g_cap;
static bool  g_dirty, g_loaded;

#ifdef _WIN32
#define PATH_SEP '\\'
#else
#define PATH_SEP '/'
#endif

static void hist_path(char *out, int cap) {
    char dir[600]; viconfig_dir(dir, sizeof dir);
    snprintf(out, cap, "%s%c%s", dir, PATH_SEP, "history.ini");
}

// Windows paths differ in case and separator without naming a different file.
static bool path_eq(const char *a, const char *b) {
#ifdef _WIN32
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca != cb) return false;
        if (!ca) return true;
    }
#else
    return strcmp(a, b) == 0;
#endif
}

static Rec *find(const char *path) {
    for (int i = 0; i < g_n; i++) if (path_eq(g_rec[i].path, path)) return &g_rec[i];
    return NULL;
}

static Rec *add(const char *path) {
    if (g_n == g_cap) {
        int cap = g_cap ? g_cap * 2 : 64;
        Rec *r = (Rec *)realloc(g_rec, (size_t)cap * sizeof(Rec));
        if (!r) return NULL;
        g_rec = r; g_cap = cap;
    }
    size_t len = strlen(path) + 1;
    char *d = (char *)malloc(len);
    if (!d) return NULL;
    memcpy(d, path, len);
    Rec *r = &g_rec[g_n++];
    r->path = d; r->seen = 0;
    r->e.position = 0; r->e.duration = 0; r->e.sub_delay_ms = 0;
    return r;
}

void watch_load(void) {
    if (g_loaded) return;
    g_loaded = true;
    char path[700]; hist_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[LINE_MAX];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        double pos = 0, dur = 0; int dly = 0; long long seen = 0; int off = 0;
        if (sscanf(line, "%lf|%lf|%d|%lld|%n", &pos, &dur, &dly, &seen, &off) != 4 || off <= 0) continue;
        char *p = line + off;
        for (char *q = p; *q; q++) if (*q == '\n' || *q == '\r') { *q = 0; break; }
        if (!*p || find(p)) continue;
        Rec *r = add(p);
        if (!r) break;
        r->e.position = pos; r->e.duration = dur; r->e.sub_delay_ms = dly; r->seen = seen;
    }
    fclose(f);
}

bool watch_get(const char *path, WatchEntry *out) {
    if (!path || !*path) return false;
    watch_load();
    Rec *r = find(path);
    if (!r) return false;
    if (out) *out = r->e;
    return true;
}

void watch_put(const char *path, const WatchEntry *e) {
    if (!path || !*path || !e) return;
    watch_load();
    Rec *r = find(path);
    if (!r) r = add(path);
    if (!r) return;
    // Writing the file every few seconds is pointless while nothing moves.
    if (r->seen != 0 && r->e.position == e->position && r->e.duration == e->duration &&
        r->e.sub_delay_ms == e->sub_delay_ms) return;
    r->e = *e;
    r->seen = (long long)time(NULL);
    g_dirty = true;
}

void watch_forget(const char *path) {
    if (!path || !*path) return;
    watch_load();
    Rec *r = find(path);
    if (!r) return;
    free(r->path);
    *r = g_rec[--g_n];
    g_dirty = true;
}

static int by_seen_desc(const void *a, const void *b) {
    long long x = ((const Rec *)a)->seen, y = ((const Rec *)b)->seen;
    return (x < y) - (x > y);
}

void watch_save(void) {
    if (!g_dirty) return;
    char path[700]; hist_path(path, sizeof path);
    char tmp[720]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# tivi watch history - position|duration|sub_delay_ms|last_seen|path\n");
    qsort(g_rec, (size_t)g_n, sizeof(Rec), by_seen_desc);      // newest first, oldest pruned
    int n = (g_n < WATCH_MAX) ? g_n : WATCH_MAX;
    for (int i = 0; i < n; i++)
        fprintf(f, "%.3f|%.3f|%d|%lld|%s\n", g_rec[i].e.position, g_rec[i].e.duration,
                g_rec[i].e.sub_delay_ms, g_rec[i].seen, g_rec[i].path);
    fclose(f);
    remove(path);                  // Win32 rename() will not overwrite
    if (rename(tmp, path) != 0) remove(tmp);
    g_dirty = false;
}

void watch_free(void) {
    for (int i = 0; i < g_n; i++) free(g_rec[i].path);
    free(g_rec);
    g_rec = NULL; g_n = g_cap = 0; g_dirty = false; g_loaded = false;
}
