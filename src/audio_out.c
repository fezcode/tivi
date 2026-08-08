#include "audio_out.h"

#include "raylib.h"

#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

// Single-producer (decode thread) / single-consumer (raylib audio thread) ring.
// Lock-free via atomic indices, so this translation unit needs neither windows.h
// (which clashes with raylib's Rectangle/CloseWindow/ShowCursor) nor a mutex.
//
// ~2.7 s of stereo float at 48 kHz. Power of two so we can mask instead of modulo.
#define CAP_FRAMES (1u << 17)
#define CAP_MASK   (CAP_FRAMES - 1u)

static struct {
    AudioStream stream;
    bool        inited;
    bool        ready;
    bool        paused;

    float      *ring;            // interleaved stereo, CAP_FRAMES * 2 floats
    atomic_uint wr, rd;          // frame indices (monotonic, masked on access)

    volatile double end_pts;     // pts (s) at the end of all data written
    atomic_bool have_pts;
    volatile float volume;
    volatile double speed;       // media-seconds per output-second (1 = normal)

    atomic_uint cb_count, cb_max;   // perf probe: callback invocations / max chunk
} A;

static unsigned ao_fill_frames(void) {
    return atomic_load(&A.wr) - atomic_load(&A.rd);
}

// raylib audio-thread callback: pull frames out of the ring, apply volume.
static void ao_callback(void *buffer, unsigned int frames) {
    float *out = (float *)buffer;
    unsigned rd = atomic_load_explicit(&A.rd, memory_order_relaxed);
    unsigned wr = atomic_load_explicit(&A.wr, memory_order_acquire);
    unsigned avail = wr - rd;
    unsigned n = frames < avail ? frames : avail;
    float v = A.volume;
    for (unsigned i = 0; i < n; i++) {
        unsigned idx = (rd & CAP_MASK) * 2u;
        float l = A.ring[idx + 0] * v, r = A.ring[idx + 1] * v;
        out[i * 2 + 0] = (l > 1.0f) ? 1.0f : (l < -1.0f) ? -1.0f : l;   // boosted gain can clip
        out[i * 2 + 1] = (r > 1.0f) ? 1.0f : (r < -1.0f) ? -1.0f : r;
        rd++;
    }
    atomic_store_explicit(&A.rd, rd, memory_order_release);
    for (unsigned i = n; i < frames; i++) { out[i * 2] = 0.0f; out[i * 2 + 1] = 0.0f; }  // underrun → silence
    atomic_fetch_add(&A.cb_count, 1);
    unsigned prev = atomic_load(&A.cb_max);
    while (frames > prev && !atomic_compare_exchange_weak(&A.cb_max, &prev, frames)) {}
}

void audio_out_perf(unsigned *callbacks, unsigned *max_chunk) {
    if (callbacks) *callbacks = atomic_exchange(&A.cb_count, 0);
    if (max_chunk) *max_chunk = atomic_exchange(&A.cb_max, 0);
}

bool audio_out_init(void) {
    if (A.inited) return true;
    memset(&A, 0, sizeof(A));
    A.volume = 1.0f;
    A.speed = 1.0;
    atomic_init(&A.wr, 0); atomic_init(&A.rd, 0); atomic_init(&A.have_pts, false);
    A.ring = (float *)malloc(sizeof(float) * CAP_FRAMES * 2);
    if (!A.ring) return false;

    SetAudioStreamBufferSizeDefault(1024);
    A.stream = LoadAudioStream(AO_RATE, 32, AO_CHANNELS);   // 32-bit float, stereo
    if (!IsAudioStreamValid(A.stream)) { free(A.ring); A.ring = NULL; return false; }
    SetAudioStreamCallback(A.stream, ao_callback);
    PlayAudioStream(A.stream);
    A.inited = true;
    A.ready = true;
    return true;
}

void audio_out_shutdown(void) {
    if (!A.inited) return;
    StopAudioStream(A.stream);
    UnloadAudioStream(A.stream);
    free(A.ring);
    A.ring = NULL;
    A.inited = false;
    A.ready = false;
}

int audio_out_writable(void) {
    return (int)(CAP_FRAMES - ao_fill_frames() - 1u);
}

int audio_out_push(const float *interleaved, int frames) {
    if (frames <= 0) return 0;
    unsigned wr = atomic_load_explicit(&A.wr, memory_order_relaxed);
    unsigned rd = atomic_load_explicit(&A.rd, memory_order_acquire);
    unsigned free = CAP_FRAMES - (wr - rd) - 1u;
    unsigned n = (unsigned)frames < free ? (unsigned)frames : free;
    for (unsigned i = 0; i < n; i++) {
        unsigned idx = (wr & CAP_MASK) * 2u;
        A.ring[idx + 0] = interleaved[i * 2 + 0];
        A.ring[idx + 1] = interleaved[i * 2 + 1];
        wr++;
    }
    atomic_store_explicit(&A.wr, wr, memory_order_release);
    return (int)n;
}

void audio_out_set_end_pts(double seconds) {
    A.end_pts = seconds;
    atomic_store(&A.have_pts, true);
}

int audio_out_fill(void) { return (int)ao_fill_frames(); }

void audio_out_clear(void) {
    atomic_store(&A.have_pts, false);
    atomic_store(&A.rd, atomic_load(&A.wr));   // consumer-visible empty
}

double audio_out_clock(void) {
    if (!atomic_load(&A.have_pts)) return -1.0;
    double sp = A.speed; if (sp <= 0) sp = 1.0;
    return A.end_pts - (double)ao_fill_frames() / (double)AO_RATE * sp;
}

void audio_out_set_speed(double speed) {
    A.speed = (speed > 0) ? speed : 1.0;
}

void audio_out_set_volume(float v) {
    if (v < 0) v = 0;
    if (v > 2) v = 2;   // VLC-style software boost up to 200%
    A.volume = v;
}

void audio_out_set_paused(bool paused) {
    if (!A.inited || paused == A.paused) return;
    A.paused = paused;
    if (paused) PauseAudioStream(A.stream);
    else        ResumeAudioStream(A.stream);
}

bool audio_out_ready(void) { return A.ready; }
