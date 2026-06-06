#ifndef TIVI_SUBS_H
#define TIVI_SUBS_H

#include <stdbool.h>
#include <stdint.h>

// libass-backed subtitle renderer. Text/ASS/SSA/SRT/VTT subtitles (embedded or
// external) are rendered to an RGBA overlay sized to the video; the UI draws it
// stretched over the picture, so subs scale with the video. Image-based subs
// (PGS/VOBSUB) are handled by the player directly, not here.

typedef struct Subs Subs;

Subs *subs_create(void);
void  subs_destroy(Subs *s);

void  subs_set_size(Subs *s, int w, int h);   // ass frame size = video native res
// Bottom margin (in native px) for default bottom-centered subs, so they can be
// lifted above the control bar while it is visible. Cheap to call every frame.
void  subs_set_bottom_margin(Subs *s, int px);

// Start a fresh embedded track. header/size = decoder ASS header (may be NULL).
void  subs_begin_embedded(Subs *s, const uint8_t *header, int size);
// Feed one decoded ASS event line (FFmpeg rect->ass) timed at start_ms for dur_ms.
void  subs_feed(Subs *s, const char *ass_line, long long start_ms, long long dur_ms);

// Load an external subtitle file (.ass/.ssa direct; .srt/.vtt/.sub via FFmpeg).
bool  subs_load_file(Subs *s, const char *path);

// Render the overlay for now_ms. Returns true with *rgba/*w/*h when something is
// visible; false when nothing is shown. *changed is set when the image differs
// from the previous render (so the UI can skip re-uploading the texture).
bool  subs_render(Subs *s, long long now_ms, uint8_t **rgba, int *w, int *h, bool *changed);

void  subs_clear(Subs *s);     // drop the current track
bool  subs_has_track(const Subs *s);

#endif
