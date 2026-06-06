#ifndef TIVI_PLAYLIST_H
#define TIVI_PLAYLIST_H

#include <stdbool.h>

// A simple ordered list of media paths with a "current" index, shuffle and loop.
// Reused, near-verbatim, from Timp — the queue model is identical for video.
typedef struct Playlist {
    char** paths;
    int count;
    int cap;
    int index;

    bool shuffle;
    bool loop;
} Playlist;

void playlist_init(Playlist* p);
void playlist_free(Playlist* p);
void playlist_clear(Playlist* p);

void playlist_add(Playlist* p, const char* path);
bool playlist_remove(Playlist* p, int idx);   // true if the current entry was removed
void playlist_move(Playlist* p, int from, int to);

const char* playlist_current(const Playlist* p);
bool playlist_has_next(const Playlist* p);
bool playlist_has_prev(const Playlist* p);

const char* playlist_next(Playlist* p);
const char* playlist_prev(Playlist* p);
void playlist_set_index(Playlist* p, int i);

int playlist_count(const Playlist* p);
int playlist_index(const Playlist* p);

void playlist_set_shuffle(Playlist* p, bool on);
void playlist_set_loop(Playlist* p, bool on);
bool playlist_shuffle(const Playlist* p);
bool playlist_loop(const Playlist* p);

#endif
