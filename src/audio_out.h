#ifndef TIVI_AUDIO_OUT_H
#define TIVI_AUDIO_OUT_H

#include <stdbool.h>

// Audio output: a raylib AudioStream (48 kHz / stereo / float) fed by a
// single-producer / single-consumer ring buffer. The player's decode thread
// pushes resampled PCM; raylib's audio thread pulls it in the stream callback.
//
// The audio clock (samples actually played) is the master clock the UI uses to
// schedule video frames, so A/V stays in sync.

#define AO_RATE     48000
#define AO_CHANNELS 2

bool   audio_out_init(void);     // call after raylib InitAudioDevice()
void   audio_out_shutdown(void);

// Producer side (decode thread):
int    audio_out_writable(void);                 // free space, in frames
int    audio_out_push(const float *interleaved, int frames);  // returns frames written
void   audio_out_set_end_pts(double seconds);    // pts at the end of all pushed data
int    audio_out_fill(void);                     // buffered frames not yet played
void   audio_out_clear(void);                    // flush (on seek / stop)

// Clock + transport:
double audio_out_clock(void);                    // current playback time (s), or -1 if unknown
void   audio_out_set_volume(float v);            // 0..1
void   audio_out_set_paused(bool paused);
bool   audio_out_ready(void);

#endif
