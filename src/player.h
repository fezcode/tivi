#ifndef TIVI_PLAYER_H
#define TIVI_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

// The decode engine: opens any container/codec FFmpeg supports, decodes video +
// audio on a background thread, keeps an A/V sync clock (audio is master), and
// hands the UI the frame that should be on screen right now.

typedef struct Player Player;

enum { TRACK_AUDIO = 0, TRACK_SUBTITLE = 1 };

// Pixel layout of a decoded display frame. RGBA is the CPU-converted compat path;
// NV12 (8-bit) and P010 (10-bit) are packed planar YUV for GPU shader conversion.
typedef enum { TIVI_PIX_RGBA = 0, TIVI_PIX_NV12, TIVI_PIX_P010 } TiviPixFmt;

typedef struct {
    TiviPixFmt fmt;
    int w, h;                 // luma dimensions
    const uint8_t *plane[2];  // [0] = Y (or RGBA); [1] = interleaved UV (NULL for RGBA)
    int stride[2];            // bytes per row, per plane
    int colorspace;           // 0 = BT.601, 1 = BT.709
    int full_range;           // 0 = limited (16-235), 1 = full (0-255)
    int bitdepth;             // 8 or 10
} TiviFrame;

typedef struct {
    int  stream_index;     // index into the container
    int  kind;             // TRACK_AUDIO | TRACK_SUBTITLE
    char title[224];       // "English (AAC)" style label for the UI
    char lang[8];          // ISO language tag if present
    char codec[40];
    bool is_bitmap;        // subtitle only: image-based (PGS/VOBSUB) vs text/ASS
} TrackInfo;

Player *player_create(void);
void    player_destroy(Player *p);

bool    player_open(Player *p, const char *path);   // opens + starts decoding (paused)
void    player_close(Player *p);
bool    player_is_open(const Player *p);
const char *player_error(const Player *p);          // last open error, if any

void    player_play(Player *p);
void    player_pause(Player *p);
void    player_toggle(Player *p);
bool    player_is_playing(const Player *p);
bool    player_eof(Player *p);                      // playback reached the end

// Advance internal clock + frame selection. Call once per UI frame with dt (s).
void    player_update(Player *p, double dt);

double  player_position(Player *p);                 // current time (s)
double  player_duration(const Player *p);           // total (s), 0 if unknown
void    player_seek(Player *p, double seconds);     // absolute
void    player_seek_relative(Player *p, double delta);

void    player_set_volume(Player *p, float v);      // 0..1
void    player_set_speed(Player *p, double speed);  // 0.25..4 (1 = normal)
void    player_set_pitch_correct(Player *p, bool on);  // speed via time-stretch (keeps pitch) vs resample
double  player_speed(const Player *p);

// Video. player_frame() fills the current display frame; *changed is set true
// when it differs from the previous call (so the UI re-uploads the texture).
bool    player_has_video(const Player *p);
int     player_video_width(const Player *p);
int     player_video_height(const Player *p);
double  player_fps(const Player *p);
bool    player_frame(Player *p, TiviFrame *out, bool *changed);
// Snapshot: always RGBA, converting the current display frame on demand. The
// returned buffer is owned by the player and valid until the next call.
bool    player_snapshot_rgba(Player *p, uint8_t **rgba, int *w, int *h);

// GPU acceleration controls + live status (for the settings UI).
void        player_set_hw_decode(Player *p, bool on);    // takes effect on next player_open
void        player_set_gpu_convert(Player *p, bool on);  // affects subsequently decoded frames
bool        player_hw_active(const Player *p);           // GPU decode currently producing frames
const char *player_decode_desc(const Player *p);         // "D3D11VA" | "software"
const char *player_convert_desc(const Player *p);        // "GPU shader (nv12)" | "CPU (sws)"

// Tracks
int     player_track_count(const Player *p);
const TrackInfo *player_track(const Player *p, int i);
int     player_current_audio(const Player *p);
int     player_current_sub(const Player *p);
void    player_set_audio_track(Player *p, int stream_index);
void    player_set_sub_track(Player *p, int stream_index);   // -1 = disable embedded subs

// Embedded subtitle plumbing for the libass renderer (see subs.c).
// Returns the ASS header (codec extradata) for the current text sub track.
const uint8_t *player_sub_header(Player *p, int *size);
bool    player_sub_is_bitmap(Player *p);
// Drain one decoded text subtitle event. Caller must free *ass_out. Returns
// false when the queue is empty. start/dur are in milliseconds (file timeline).
bool    player_pop_sub_text(Player *p, long long *start_ms, long long *dur_ms, char **ass_out);
// Bitmap subtitles: returns the overlay (video-sized RGBA) active at the current
// clock, or false if none. Buffer owned by the player.
bool    player_active_sub_bitmap(Player *p, uint8_t **rgba, int *w, int *h);
// Bumped whenever the selected subtitle track changes (UI resets its ASS track).
unsigned player_sub_generation(Player *p);

const char *player_title(const Player *p);
const char *player_path(const Player *p);

// One-shot decode self-test (headless): open, decode a few frames, print info.
// Returns 0 on success. Used by `tivi --probe FILE`.
int     player_probe(const char *path);

#endif
