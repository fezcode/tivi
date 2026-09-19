#ifndef TIVI_WATCH_H
#define TIVI_WATCH_H

#include <stdbool.h>

// Per-file playback memory, kept in %APPDATA%\fezcode\tivi\history.ini (one
// line per file). Two things belong to the file rather than the session: where
// you stopped watching, and how far its subtitles had to be nudged — a bad sync
// is a property of that release, not of tivi. Both live here so opening the
// episode again picks up exactly where it was left.
//
// The store is small and fully in memory; the oldest entries are dropped once
// it passes a few hundred files.

typedef struct {
    double position;      // seconds into the file (0 = start / finished)
    double duration;      // seconds, 0 when unknown
    int    sub_delay_ms;  // subtitle nudge, + = subtitles appear later
} WatchEntry;

void watch_load(void);                                  // read the store (once, at startup)
bool watch_get(const char *path, WatchEntry *out);      // false when the file is unknown
void watch_put(const char *path, const WatchEntry *e);  // record / update in memory
void watch_forget(const char *path);                    // drop one file (nothing worth resuming)
void watch_save(void);                                  // flush to disk, if anything changed
void watch_free(void);

#endif
