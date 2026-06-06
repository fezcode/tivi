#include "playlist.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

void playlist_init(Playlist* p) {
    memset(p, 0, sizeof(*p));
    p->index = -1;
    static int seeded = 0;
    if (!seeded) { srand((unsigned)time(NULL)); seeded = 1; }
}

void playlist_clear(Playlist* p) {
    for (int i = 0; i < p->count; i++) free(p->paths[i]);
    p->count = 0;
    p->index = -1;
}

void playlist_free(Playlist* p) {
    playlist_clear(p);
    free(p->paths);
    p->paths = NULL;
    p->cap = 0;
}

static char* dupstr(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

void playlist_add(Playlist* p, const char* path) {
    if (!path) return;
    if (p->count == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 8;
        p->paths = (char**)realloc(p->paths, sizeof(char*) * p->cap);
    }
    p->paths[p->count++] = dupstr(path);
    if (p->index < 0) p->index = 0;
}

bool playlist_remove(Playlist* p, int idx) {
    if (idx < 0 || idx >= p->count) return false;
    bool was_current = (idx == p->index);
    free(p->paths[idx]);
    for (int i = idx; i < p->count - 1; i++) p->paths[i] = p->paths[i+1];
    p->count--;
    if (p->count == 0) {
        p->index = -1;
    } else if (p->index >= p->count) {
        p->index = p->count - 1;
    } else if (p->index > idx) {
        p->index--;
    }
    return was_current;
}

void playlist_move(Playlist* p, int from, int to) {
    if (from < 0 || from >= p->count) return;
    if (to < 0) to = 0;
    if (to >= p->count) to = p->count - 1;
    if (from == to) return;

    char* moved = p->paths[from];
    if (from < to) {
        for (int i = from; i < to; i++) p->paths[i] = p->paths[i + 1];
    } else {
        for (int i = from; i > to; i--) p->paths[i] = p->paths[i - 1];
    }
    p->paths[to] = moved;

    int cur = p->index;
    if (cur == from)            cur = to;
    else if (from < cur && cur <= to) cur--;
    else if (to <= cur && cur < from) cur++;
    p->index = cur;
}

const char* playlist_current(const Playlist* p) {
    if (p->index < 0 || p->index >= p->count) return NULL;
    return p->paths[p->index];
}

bool playlist_has_next(const Playlist* p) {
    if (p->count == 0) return false;
    if (p->shuffle && p->count > 1) return true;
    if (p->index + 1 < p->count) return true;
    return p->loop && p->count > 0;
}

bool playlist_has_prev(const Playlist* p) {
    if (p->count == 0) return false;
    if (p->shuffle && p->count > 1) return true;
    if (p->index > 0) return true;
    return p->loop && p->count > 0;
}

const char* playlist_next(Playlist* p) {
    if (p->count == 0) return NULL;
    if (p->shuffle && p->count > 1) {
        int next;
        do { next = rand() % p->count; } while (next == p->index);
        p->index = next;
    } else if (p->index + 1 < p->count) {
        p->index++;
    } else if (p->loop) {
        p->index = 0;
    } else {
        return NULL;
    }
    return p->paths[p->index];
}

const char* playlist_prev(Playlist* p) {
    if (p->count == 0) return NULL;
    if (p->shuffle && p->count > 1) {
        int next;
        do { next = rand() % p->count; } while (next == p->index);
        p->index = next;
    } else if (p->index > 0) {
        p->index--;
    } else if (p->loop) {
        p->index = p->count - 1;
    } else {
        return NULL;
    }
    return p->paths[p->index];
}

void playlist_set_index(Playlist* p, int i) {
    if (i >= 0 && i < p->count) p->index = i;
}

int playlist_count(const Playlist* p) { return p->count; }
int playlist_index(const Playlist* p) { return p->index; }

void playlist_set_shuffle(Playlist* p, bool on) { p->shuffle = on; }
void playlist_set_loop(Playlist* p, bool on) { p->loop = on; }
bool playlist_shuffle(const Playlist* p) { return p->shuffle; }
bool playlist_loop(const Playlist* p) { return p->loop; }
