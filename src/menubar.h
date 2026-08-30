#ifndef TIVI_MENUBAR_H
#define TIVI_MENUBAR_H

#include <stdbool.h>
#include <stddef.h>

// menubar.c — Hisashi OS Window Layer (hoswl) bridge.
//
// tivi has no menu bar of its own; when Hisashi is running it gets one:
// File / Playback / Audio / Subtitles / Video / Help are published to Hisashi's
// macOS-style menubar over the named pipe \\.\pipe\hoswl and the ids the user
// clicks come back to main.c. Never blocks: while Hisashi isn't running it
// retries a connect every 2 s and everything else is a no-op.
// Protocol: Hisashi/docs/hoswl-protocol.md · client: src/hoswl.h (vendored copy
// of Hisashi/sdk/hoswl/hoswl.h — never edit it here, fix upstream).
// Same contract as pidi's src/menubar.h.

#define MENUBAR_MAX_TRACKS 64

typedef struct {
    int  kind;             // TRACK_AUDIO / TRACK_SUBTITLE (player.h)
    int  stream_index;
    char title[224];       // player's UI label, e.g. "English (AAC)"
} MenubarTrack;

// Everything that changes what the bar shows. main.c fills one per frame; the
// menu is republished only when it differs from the last one sent.
typedef struct {
    bool open, playing, has_next, has_prev;
    bool muted;
    int  repeat;           // 0 off · 1 one · 2 all
    bool shuffle;
    int  speed_idx;        // index into the 0.5/0.75/1/1.25/1.5/2 presets, -1 = none
    bool pitch_correct;
    bool subs_on;          // an embedded track is showing (sub_source == SUB_EMBEDDED)
    int  cur_audio, cur_sub;   // stream indices, -1 = none
    int  ntracks;
    MenubarTrack tracks[MENUBAR_MAX_TRACKS];
    bool fullscreen, aot, hw_decode, gpu_convert;
    int  panel;            // PANEL_* from main.c
} MenubarState;

void menubar_init(const char *app_version);   // once, before the first menubar_frame
void menubar_set_enabled(bool on);            // the "Hisashi menubar" setting; off disconnects
bool menubar_connected(void);

// Once per frame from the main loop: connect / republish when needed, and call
// on_click(id) for every item Hisashi reports clicked. Ids are the ones listed
// in menubar.c ("file.open", "pb.toggle", "audio.track.<stream>", ...).
// Callbacks run right here on the main thread, never from inside the pipe code.
void menubar_frame(const MenubarState *st, void (*on_click)(const char *id));
void menubar_shutdown(void);

// The menu text (hoswl's line DSL) for a state — exposed for test_menubar.c.
// Returns 0, or -1 when it does not fit in cap.
int  menubar_build_text(const MenubarState *st, char *out, size_t cap);

#endif
