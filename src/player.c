#include "player.h"
#include "audio_out.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- tiny cross-platform thread / mutex / condvar layer ----
#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION    pl_mtx;
typedef CONDITION_VARIABLE  pl_cnd;
typedef HANDLE              pl_thr;
static void M_init(pl_mtx *m)   { InitializeCriticalSection(m); }
static void M_free(pl_mtx *m)   { DeleteCriticalSection(m); }
static void M_lock(pl_mtx *m)   { EnterCriticalSection(m); }
static void M_unlock(pl_mtx *m) { LeaveCriticalSection(m); }
static void C_init(pl_cnd *c)   { InitializeConditionVariable(c); }
static void C_wait_ms(pl_cnd *c, pl_mtx *m, int ms) { SleepConditionVariableCS(c, m, (DWORD)ms); }
static void C_wakeall(pl_cnd *c){ WakeAllConditionVariable(c); }
static void msleep(int ms)      { Sleep((DWORD)ms); }
#else
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t pl_mtx;
typedef pthread_cond_t  pl_cnd;
typedef pthread_t       pl_thr;
static void M_init(pl_mtx *m)   { pthread_mutex_init(m, NULL); }
static void M_free(pl_mtx *m)   { pthread_mutex_destroy(m); }
static void M_lock(pl_mtx *m)   { pthread_mutex_lock(m); }
static void M_unlock(pl_mtx *m) { pthread_mutex_unlock(m); }
static void C_init(pl_cnd *c)   { pthread_cond_init(c, NULL); }
static void C_wait_ms(pl_cnd *c, pl_mtx *m, int ms) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L; ts.tv_sec += ts.tv_nsec / 1000000000L; ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(c, m, &ts);
}
static void C_wakeall(pl_cnd *c){ pthread_cond_broadcast(c); }
static void msleep(int ms)      { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
#endif

#define VPOOL 8       // video frame buffers (pool == queue ring size)
#define SUBQ  512     // decoded text-subtitle event ring
#define SBQ   32      // decoded bitmap-subtitle ring
#define NO_REQ (-2)

typedef struct { long long start_ms, dur_ms; char *ass; } SubText;
typedef struct { long long start_ms, end_ms; uint8_t *rgba; int w, h; } SubBmp;

struct Player {
    AVFormatContext *fmt;
    AVCodecContext  *vdec, *adec, *sdec;
    struct SwsContext *sws;
    struct SwrContext *swr;

    // GPU (D3D11VA) video decode. hw_pix_fmt == AV_PIX_FMT_NONE means the video
    // stream is decoded in software (no GPU, or the GPU can't handle this codec).
    AVBufferRef       *hw_device_ctx;   // D3D11 device (our ref; the decoder holds its own)
    enum AVPixelFormat hw_pix_fmt;      // format the decoder emits for GPU frames
    AVFrame           *hwsw;            // scratch frame for GPU->CPU readback
    bool               hw_logged;       // one-shot "which path is active" log

    // acceleration controls + planar-output state
    bool  want_hw;              // attempt GPU decode (settings; read at open time)
    bool  want_gpu_convert;     // emit planar YUV for shader color conversion
    bool  v_live;               // real video stream (not cover art) — drives A/V pacing
    double preroll_to;          // precise seek: drop decoded output before this time
    bool  hw_in_use;            // a GPU-format frame was actually received
    int   v_colorspace;         // 0 = BT.601, 1 = BT.709   (per stream)
    int   v_range;              // 0 = limited, 1 = full
    int   v_bitdepth;           // 8 or 10
    TiviPixFmt disp_fmt;        // pixel layout currently in disp_buf
    struct SwsContext *snap_sws; uint8_t *snap_rgba;   // lazy RGBA scratch for snapshots

    int v_stream, a_stream, s_stream;
    AVRational v_tb, a_tb, s_tb;
    int vw, vh;
    double fps, duration;
    bool has_video, has_audio;

    char path[1024], title[256], errbuf[256];
    bool opened;

    TrackInfo tracks[64];
    int ntracks;

    // video frame pool + queue (indices into vbuf)
    uint8_t *vbuf[VPOOL];
    size_t   vbuf_sz;
    int      vfree[VPOOL], nfree;
    struct { int buf; double pts; TiviPixFmt fmt; } vq[VPOOL];
    int      vqh, vqn;
    int      disp_buf;            // index held for display, -1 = none
    double   disp_pts;
    bool     display_dirty;

    // subtitle event queues
    SubText subq[SUBQ]; int subqh, subqn;
    SubBmp  sbq[SBQ];   int sbqh, sbqn;
    SubBmp  cur_bmp;    bool cur_bmp_valid;       // UI-owned active bitmap sub
    uint8_t *sub_header; int sub_header_size; bool sub_is_bitmap; unsigned sub_gen;

    // pitch-preserving speed: atempo filter graph at the decoder's native format
    // (time-stretch, VLC-style). Active when pitch_correct is on and speed != 1.
    AVFilterGraph   *agraph;
    AVFilterContext *asrc, *asink;
    AVFrame         *afr;            // filtered-frame scratch
    double           chain_speed;    // speed the audio chain was built for
    int              chain_pitch;    // 1 = atempo chain, 0 = resample (pitch shifts)
    int              pitch_correct;  // desired mode (UI)

    // resample scratch
    uint8_t *swr_buf; int swr_buf_frames;
    double   audio_pts_running;
    double   swr_speed;          // playback speed the resampler was built for
    int      swr_eff_rate;       // declared input rate (real rate × speed)

    // clock / state
    double v_clock;
    bool   playing;
    float  volume;
    double speed;

    // thread control (guarded by lock)
    pl_thr thread; bool thread_running;
    pl_mtx lock; pl_cnd cond;
    bool   stop;
    int    seek_req; double seek_target;
    int    req_audio, req_sub;
    bool   demux_eof, reached_eof;
};

// ---------- helpers ----------
static bool codec_is_bitmap_sub(enum AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_DVD_SUBTITLE:
        case AV_CODEC_ID_DVB_SUBTITLE:
        case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
        case AV_CODEC_ID_XSUB:
        case AV_CODEC_ID_DVB_TELETEXT:
            return true;
        default: return false;
    }
}

static void str_copy(char *dst, int cap, const char *src) {
    if (!src) { dst[0] = 0; return; }
    snprintf(dst, cap, "%s", src);
}

// Decoder callback: FFmpeg offers the pixel formats it can output for this frame.
// Pick the GPU (D3D11) format when it's on offer; otherwise let FFmpeg choose a
// software format — that's the per-stream fallback when the GPU can't decode this
// profile/bit-depth (e.g. an old card facing 10-bit HEVC).
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts) {
    Player *p = (Player *)ctx->opaque;
    for (const enum AVPixelFormat *f = fmts; *f != AV_PIX_FMT_NONE; f++)
        if (*f == p->hw_pix_fmt) return *f;
    return avcodec_default_get_format(ctx, fmts);   // GPU declined -> software
}

// Try to attach a D3D11VA GPU device to this video decoder. On any failure
// (decoder has no D3D11VA config, no compatible GPU, driver refuses) it leaves
// the context untouched, so the caller silently gets a normal software decoder.
static void try_enable_hw(Player *p, AVCodecContext *ctx, const AVCodec *dec) {
    if (!p->want_hw) return;                 // hardware decode disabled in settings
    const enum AVHWDeviceType type = AV_HWDEVICE_TYPE_D3D11VA;
    enum AVPixelFormat hwfmt = AV_PIX_FMT_NONE;
    for (int i = 0;; i++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(dec, i);
        if (!cfg) return;   // decoder can't do D3D11VA (e.g. AV1 on a build without it)
        if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == type) {
            hwfmt = cfg->pix_fmt; break;
        }
    }
    AVBufferRef *dev = NULL;
    if (av_hwdevice_ctx_create(&dev, type, NULL, NULL, 0) < 0) return;   // no usable GPU
    ctx->hw_device_ctx = av_buffer_ref(dev);
    ctx->opaque        = p;
    ctx->get_format    = get_hw_format;
    p->hw_device_ctx   = dev;      // our own ref, released in player_close
    p->hw_pix_fmt      = hwfmt;
}

// Open a decoder context for a stream. Returns NULL on failure.
static AVCodecContext *open_stream_decoder(Player *p, int stream_index) {
    AVStream *st = p->fmt->streams[stream_index];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) return NULL;
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    if (!ctx) return NULL;
    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) { avcodec_free_context(&ctx); return NULL; }
    ctx->pkt_timebase = st->time_base;
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        ctx->thread_count = 0;                 // auto: use all cores (software fallback)
        ctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
        try_enable_hw(p, ctx, dec);            // offload to GPU when possible
    }
    if (avcodec_open2(ctx, dec, NULL) < 0) { avcodec_free_context(&ctx); return NULL; }
    return ctx;
}

static void free_video_pool(Player *p) {
    for (int i = 0; i < VPOOL; i++) { free(p->vbuf[i]); p->vbuf[i] = NULL; }
    p->nfree = 0; p->vqh = p->vqn = 0; p->disp_buf = -1;
}

static bool alloc_video_pool(Player *p) {
    p->vbuf_sz = (size_t)p->vw * p->vh * 4;
    for (int i = 0; i < VPOOL; i++) {
        p->vbuf[i] = (uint8_t *)malloc(p->vbuf_sz);
        if (!p->vbuf[i]) return false;
        p->vfree[i] = i;
    }
    p->nfree = VPOOL; p->vqh = p->vqn = 0; p->disp_buf = -1; p->display_dirty = false;
    return true;
}

static void clear_sub_queues(Player *p) {
    while (p->subqn > 0) { free(p->subq[p->subqh].ass); p->subqh = (p->subqh + 1) % SUBQ; p->subqn--; }
    while (p->sbqn  > 0) { free(p->sbq[p->sbqh].rgba);  p->sbqh  = (p->sbqh  + 1) % SBQ;  p->sbqn--; }
}

// ---------- producer (decode thread) ----------
static int acquire_buf(Player *p) {
    M_lock(&p->lock);
    while (p->nfree == 0 && !p->stop && !p->seek_req && p->req_audio == NO_REQ && p->req_sub == NO_REQ)
        C_wait_ms(&p->cond, &p->lock, 15);
    int idx = (p->stop || p->seek_req || p->req_audio != NO_REQ || p->req_sub != NO_REQ) ? -1 : p->vfree[--p->nfree];
    M_unlock(&p->lock);
    return idx;
}

static void enqueue_buf(Player *p, int idx, double pts, TiviPixFmt fmt) {
    {   // perf probe: dump the first queued pts values to check spacing
        static int perf = -1; if (perf < 0) perf = getenv("TIVI_PERF") ? 1 : 0;
        static int dumped = 0;
        if (perf && dumped < 16) { printf("[perf] enq pts %.4f\n", pts); fflush(stdout); dumped++; }
    }
    M_lock(&p->lock);
    if (p->stop || p->seek_req) { p->vfree[p->nfree++] = idx; M_unlock(&p->lock); return; }
    int t = (p->vqh + p->vqn) % VPOOL;
    p->vq[t].buf = idx; p->vq[t].pts = pts; p->vq[t].fmt = fmt; p->vqn++;
    C_wakeall(&p->cond);
    M_unlock(&p->lock);
}

// Map an FFmpeg frame's colour metadata onto our per-stream ints (constant per stream).
static void capture_color_info(Player *p, const AVFrame *f) {
    switch (f->colorspace) {
        case AVCOL_SPC_BT709:                             p->v_colorspace = 1; break;
        case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: p->v_colorspace = 0; break;
        default:                p->v_colorspace = (p->vh >= 720) ? 1 : 0;      break;  // unspecified
    }
    p->v_range = (f->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
}

static void copy_plane(uint8_t *dst, int dst_stride, const uint8_t *src, int src_stride, int row_bytes, int rows) {
    for (int y = 0; y < rows; y++)
        memcpy(dst + (size_t)y * dst_stride, src + (size_t)y * src_stride, row_bytes);
}

// Pack a semi-planar (NV12/P010) frame tightly into a pool slot: Y then interleaved UV.
static void pack_planar(Player *p, int idx, const AVFrame *src, int bytes) {
    int cw = (p->vw + 1) / 2, ch = (p->vh + 1) / 2;
    uint8_t *y  = p->vbuf[idx];
    uint8_t *uv = y + (size_t)p->vw * bytes * p->vh;
    copy_plane(y,  p->vw * bytes,  src->data[0], src->linesize[0], p->vw * bytes,  p->vh);
    copy_plane(uv, cw * 2 * bytes, src->data[1], src->linesize[1], cw * 2 * bytes, ch);
}

static void decode_video(Player *p, AVPacket *pkt, AVFrame *frame) {
    // perf probe (TIVI_PERF=1): per-stage decode timings, reported every 120 frames
    static int perf = -1; if (perf < 0) perf = getenv("TIVI_PERF") ? 1 : 0;
    static double s_recv, s_xfer, s_wait, s_pack; static int s_n; static int64_t s_w0;
    if (avcodec_send_packet(p->vdec, pkt) < 0) return;
    for (;;) {
        int64_t ptA = av_gettime_relative();
        if (avcodec_receive_frame(p->vdec, frame) != 0) break;
        int64_t ptB = av_gettime_relative();
        int64_t ts = frame->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) ts = frame->pts;
        double pts = (ts == AV_NOPTS_VALUE) ? p->v_clock : ts * av_q2d(p->v_tb);

        // Seek preroll: this frame is before the seek target — it had to be decoded
        // (reference chain) but must not be shown. Drop it before the expensive
        // GPU readback / conversion. Keep the one frame straddling the target so
        // there's a picture the moment we arrive.
        double fdur = (p->fps > 1.0) ? 1.0 / p->fps : 0.04;
        if (pts < p->preroll_to - fdur) { av_frame_unref(frame); continue; }

        // A GPU-decoded frame lives in VRAM (format == hw_pix_fmt); copy it down to
        // a CPU frame (NV12 / P010) before color conversion. A software-decoded
        // frame (GPU fallback for this stream) is already usable as-is.
        AVFrame *src = frame;
        if (p->hw_pix_fmt != AV_PIX_FMT_NONE && frame->format == p->hw_pix_fmt) {
            if (!p->hwsw) p->hwsw = av_frame_alloc();
            if (!p->hwsw || av_hwframe_transfer_data(p->hwsw, frame, 0) < 0) {
                av_frame_unref(frame); continue;      // readback failed — drop this frame
            }
            src = p->hwsw;
        }
        p->hw_in_use = (src == p->hwsw);
        int64_t ptC = av_gettime_relative();

        // Fast path: hand the shader packed planar YUV. Only when GPU conversion is
        // enabled, the layout is NV12/P010, and dims match the pool/texture sizing.
        bool fast = p->want_gpu_convert && src->width == p->vw && src->height == p->vh &&
                    (src->format == AV_PIX_FMT_NV12 || src->format == AV_PIX_FMT_P010LE);
        TiviPixFmt tf = !fast ? TIVI_PIX_RGBA
                       : (src->format == AV_PIX_FMT_P010LE ? TIVI_PIX_P010 : TIVI_PIX_NV12);

        if (!p->hw_logged) {
            p->hw_logged = true;
            printf("video: %s decode, %s -> %s\n", p->hw_in_use ? "D3D11VA GPU" : "software (CPU)",
                   av_get_pix_fmt_name(src->format) ? av_get_pix_fmt_name(src->format) : "?",
                   fast ? "GPU shader convert" : "CPU sws convert");
            fflush(stdout);
        }

        int idx = acquire_buf(p);
        int64_t ptD = av_gettime_relative();
        if (idx < 0) {                                // aborted (stop/seek/track switch)
            av_frame_unref(frame);
            if (src == p->hwsw) av_frame_unref(p->hwsw);
            return;
        }

        if (fast) {
            capture_color_info(p, src);
            p->v_bitdepth = (tf == TIVI_PIX_P010) ? 10 : 8;
            pack_planar(p, idx, src, tf == TIVI_PIX_P010 ? 2 : 1);
            enqueue_buf(p, idx, pts, tf);
        } else {
            p->sws = sws_getCachedContext(p->sws, src->width, src->height, src->format,
                                          p->vw, p->vh, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
            if (p->sws) {
                uint8_t *dst[4] = { p->vbuf[idx], NULL, NULL, NULL };
                int dstst[4]    = { p->vw * 4, 0, 0, 0 };
                sws_scale(p->sws, (const uint8_t * const *)src->data, src->linesize, 0, src->height, dst, dstst);
                enqueue_buf(p, idx, pts, TIVI_PIX_RGBA);
            } else {
                M_lock(&p->lock); p->vfree[p->nfree++] = idx; M_unlock(&p->lock);
            }
        }
        if (perf) {
            int64_t ptE = av_gettime_relative();
            if (s_n == 0) s_w0 = ptA;
            s_recv += (ptB - ptA) / 1000.0; s_xfer += (ptC - ptB) / 1000.0;
            s_wait += (ptD - ptC) / 1000.0; s_pack += (ptE - ptD) / 1000.0;
            if (++s_n == 120) {
                printf("[perf] dec: recv %.1f  xfer %.1f  wait %.1f  pack %.1f ms/f  (%.1f fps out)\n",
                       s_recv / s_n, s_xfer / s_n, s_wait / s_n, s_pack / s_n, s_n / ((ptE - s_w0) / 1e6));
                fflush(stdout);
                s_recv = s_xfer = s_wait = s_pack = 0; s_n = 0;
            }
        }
        av_frame_unref(frame);
        if (src == p->hwsw) av_frame_unref(p->hwsw);
    }
}

static void push_audio(Player *p, const float *buf, int frames) {
    int off = 0;
    int64_t t0 = av_gettime_relative(); int warned = 0;
    while (off < frames) {
        M_lock(&p->lock);
        bool abort = p->stop || p->seek_req || p->req_audio != NO_REQ || p->req_sub != NO_REQ;
        int vq = p->vqn;
        M_unlock(&p->lock);
        if (abort) return;
        {   // perf probe: a push blocked >2 s means the ring is not draining
            static int perf = -1; if (perf < 0) perf = getenv("TIVI_PERF") ? 1 : 0;
            if (perf && av_gettime_relative() - t0 > (warned + 1) * 2000000LL) {
                warned++;
                printf("[perf] push_audio stuck %ds: fill %u vq %d frames %d off %d\n",
                       warned * 2, audio_out_fill(), vq, frames, off);
                fflush(stdout);
            }
        }
        // Keep ~0.5 s of audio buffered so speed changes apply promptly — but never
        // at the cost of starving video: many files (e.g. WEB-DL MKVs) interleave
        // audio packets up to ~0.5 s AHEAD of the matching video packets, so pausing
        // the demuxer on the audio cap would stall video decode until frames are
        // already late (they then arrive in bursts and get dropped — slideshow).
        // While the video queue is low, relax the cap to 2 s so the demuxer can
        // reach the video packets; the ring (2.7 s) still bounds it.
        int cap = (p->v_live && vq < VPOOL - 2) ? AO_RATE * 2 : AO_RATE / 2;
        if (audio_out_fill() > cap) { msleep(4); continue; }
        int wr = audio_out_push(buf + off * 2, frames - off);
        off += wr;
        if (wr == 0) msleep(4);     // ring full — let the device drain
    }
}

// Native-format frame → swr (format/rate, and speed in the classic mode) → ring.
// Returns output frames pushed.
static int convert_push(Player *p, AVFrame *frame) {
    int out_count = (int)av_rescale_rnd(swr_get_delay(p->swr, p->swr_eff_rate) + frame->nb_samples,
                                        AO_RATE, p->swr_eff_rate, AV_ROUND_UP);
    if (out_count > p->swr_buf_frames) {
        free(p->swr_buf);
        p->swr_buf = (uint8_t *)malloc((size_t)out_count * AO_CHANNELS * sizeof(float));
        p->swr_buf_frames = p->swr_buf ? out_count : 0;
        if (!p->swr_buf) return 0;
    }
    uint8_t *outp[1] = { p->swr_buf };
    int got = swr_convert(p->swr, outp, out_count, (const uint8_t **)frame->extended_data, frame->nb_samples);
    if (got > 0) push_audio(p, (const float *)p->swr_buf, got);
    return got > 0 ? got : 0;
}

static void decode_audio(Player *p, AVPacket *pkt, AVFrame *frame) {
    if (avcodec_send_packet(p->adec, pkt) < 0) return;
    while (avcodec_receive_frame(p->adec, frame) == 0) {
        double sec = (frame->pts != AV_NOPTS_VALUE) ? frame->pts * av_q2d(p->a_tb) : p->audio_pts_running;
        double dur = (frame->sample_rate > 0) ? (double)frame->nb_samples / frame->sample_rate : 0;
        // Seek preroll: skip audio that ends before the seek target. Pushing it
        // would rewind the master clock to the keyframe and replay old audio.
        if (frame->pts != AV_NOPTS_VALUE && frame->sample_rate > 0 && sec + dur < p->preroll_to) {
            p->audio_pts_running = sec + dur;
            av_frame_unref(frame);
            continue;
        }
        if (p->agraph) {
            // Pitch-preserving path: atempo eats the frame at native pitch, then swr
            // converts. pts tracked on the input side — the filter's small internal
            // buffer (~tens of ms) is an accepted constant clock offset.
            static int perf = -1; if (perf < 0) perf = getenv("TIVI_PERF") ? 1 : 0;
            static int64_t fed = 0, out = 0; static int64_t plast = 0;
            int rc = av_buffersrc_add_frame_flags(p->asrc, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
            if (rc >= 0) {
                fed += frame->nb_samples;
                if (!p->afr) p->afr = av_frame_alloc();
                while (p->afr && av_buffersink_get_frame(p->asink, p->afr) >= 0) {
                    out += p->afr->nb_samples;
                    // sink is pinned to interleaved flt stereo @ AO_RATE → straight to the ring
                    if (p->afr->data[0] && p->afr->nb_samples > 0)
                        push_audio(p, (const float *)p->afr->data[0], p->afr->nb_samples);
                    av_frame_unref(p->afr);
                }
                p->audio_pts_running = sec + dur;
                audio_out_set_end_pts(p->audio_pts_running);
            } else if (perf) {
                printf("[perf] atempo: buffersrc add failed (%d)\n", rc); fflush(stdout);
            }
            if (perf) {
                int64_t nowt = av_gettime_relative();
                if (nowt - plast > 2000000LL) { plast = nowt; printf("[perf] atempo: fed %lld out %lld samples\n", (long long)fed, (long long)out); fflush(stdout); }
            }
        } else {
            int got = convert_push(p, frame);
            if (got > 0) {
                // each output second covers swr_speed media seconds
                p->audio_pts_running = sec + (double)got / (double)AO_RATE * p->swr_speed;
                audio_out_set_end_pts(p->audio_pts_running);
            }
        }
        av_frame_unref(frame);
    }
}

static void blit_bitmap_sub(Player *p, AVSubtitle *sub, long long start_ms, long long end_ms) {
    uint8_t *rgba = (uint8_t *)calloc((size_t)p->vw * p->vh * 4, 1);
    if (!rgba) return;
    for (unsigned r = 0; r < sub->num_rects; r++) {
        AVSubtitleRect *rc = sub->rects[r];
        if (rc->type != SUBTITLE_BITMAP || !rc->data[0] || !rc->data[1]) continue;
        const uint32_t *pal = (const uint32_t *)rc->data[1];
        for (int y = 0; y < rc->h; y++) {
            int py = rc->y + y; if (py < 0 || py >= p->vh) continue;
            const uint8_t *srow = rc->data[0] + (size_t)y * rc->linesize[0];
            for (int x = 0; x < rc->w; x++) {
                int px = rc->x + x; if (px < 0 || px >= p->vw) continue;
                uint32_t c = pal[srow[x]];
                uint8_t a = (c >> 24) & 0xff;
                if (!a) continue;
                uint8_t *d = rgba + ((size_t)py * p->vw + px) * 4;
                d[0] = (c >> 16) & 0xff; d[1] = (c >> 8) & 0xff; d[2] = c & 0xff; d[3] = a;
            }
        }
    }
    M_lock(&p->lock);
    if (p->sbqn == SBQ) { free(p->sbq[p->sbqh].rgba); p->sbqh = (p->sbqh + 1) % SBQ; p->sbqn--; }
    int t = (p->sbqh + p->sbqn) % SBQ;
    p->sbq[t].start_ms = start_ms; p->sbq[t].end_ms = end_ms; p->sbq[t].rgba = rgba;
    p->sbq[t].w = p->vw; p->sbq[t].h = p->vh; p->sbqn++;
    M_unlock(&p->lock);
}

static void decode_sub(Player *p, AVPacket *pkt) {
    AVSubtitle sub; int got = 0;
    if (avcodec_decode_subtitle2(p->sdec, &sub, &got, pkt) < 0 || !got) return;

    double pts_sec = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts * av_q2d(p->s_tb) : p->v_clock;
    long long start_ms = (long long)(pts_sec * 1000.0) + sub.start_display_time;
    long long dur_ms;
    if (sub.end_display_time > sub.start_display_time) dur_ms = sub.end_display_time - sub.start_display_time;
    else if (pkt->duration > 0)                        dur_ms = (long long)(pkt->duration * av_q2d(p->s_tb) * 1000.0);
    else                                               dur_ms = 4000;

    bool any_bitmap = false;
    for (unsigned r = 0; r < sub.num_rects; r++) {
        AVSubtitleRect *rc = sub.rects[r];
        if (rc->type == SUBTITLE_BITMAP) { any_bitmap = true; continue; }
        const char *ass = rc->ass;
        if (!ass || !*ass) continue;
        M_lock(&p->lock);
        if (p->subqn == SUBQ) { free(p->subq[p->subqh].ass); p->subqh = (p->subqh + 1) % SUBQ; p->subqn--; }
        int t = (p->subqh + p->subqn) % SUBQ;
        p->subq[t].start_ms = start_ms; p->subq[t].dur_ms = dur_ms; p->subq[t].ass = strdup(ass);
        p->subqn++;
        M_unlock(&p->lock);
    }
    if (any_bitmap) blit_bitmap_sub(p, &sub, start_ms, start_ms + dur_ms);
    avsubtitle_free(&sub);
}

static void do_seek(Player *p, double target) {
    if (target < 0) target = 0;
    {   // perf probe: log seek requests to diagnose landing position
        static int perf = -1; if (perf < 0) perf = getenv("TIVI_PERF") ? 1 : 0;
        if (perf) { printf("[perf] seek -> %.2f (from %.2f)\n", target, p->v_clock); fflush(stdout); }
    }
    int64_t ts = (int64_t)(target * AV_TIME_BASE);
    // Precise seek: av_seek_frame can only land on the keyframe AT/BEFORE the
    // target (x265 WEB-DLs space keyframes 10 s+ apart, so a "+5 s" seek would
    // otherwise snap BACK to the current GOP's keyframe). Decode from the
    // keyframe but discard all output before `preroll_to`, so playback resumes
    // exactly at the requested time. decode_video/decode_audio do the dropping.
    p->preroll_to = target;
    av_seek_frame(p->fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
    if (p->vdec) avcodec_flush_buffers(p->vdec);
    if (p->adec) avcodec_flush_buffers(p->adec);
    if (p->sdec) avcodec_flush_buffers(p->sdec);
    p->chain_speed = -1;   // force an audio-chain rebuild → drops stale atempo/swr state
    audio_out_clear();
    M_lock(&p->lock);
    while (p->vqn > 0) { p->vfree[p->nfree++] = p->vq[p->vqh].buf; p->vqh = (p->vqh + 1) % VPOOL; p->vqn--; }
    clear_sub_queues(p);
    p->v_clock = target; p->audio_pts_running = target;
    p->demux_eof = false; p->reached_eof = false;
    C_wakeall(&p->cond);
    M_unlock(&p->lock);
}

// (Re)build the resampler for the given decoder + speed ratio. Ratio != 1 does
// speed by declaring the input rate as real_rate × ratio: the resampler then
// emits proportionally fewer/more output samples, so audio plays faster/slower
// WITH the matching pitch shift. In pitch-preserving mode the ratio is 1.0 and
// atempo (below) does the time-stretch instead.
static bool init_swr(Player *p, AVCodecContext *ctx, double speed) {
    if (p->swr) swr_free(&p->swr);
    if (speed < 0.25) speed = 0.25;
    if (speed > 4.0)  speed = 4.0;
    AVChannelLayout out_ch; av_channel_layout_default(&out_ch, AO_CHANNELS);
    AVChannelLayout in_ch  = ctx->ch_layout;
    if (in_ch.nb_channels <= 0) av_channel_layout_default(&in_ch, 2);
    int eff = (int)(ctx->sample_rate * speed + 0.5);
    if (eff < 4000) eff = 4000;
    if (swr_alloc_set_opts2(&p->swr, &out_ch, AV_SAMPLE_FMT_FLT, AO_RATE,
                            &in_ch, ctx->sample_fmt, eff, 0, NULL) < 0 || swr_init(p->swr) < 0) {
        if (p->swr) swr_free(&p->swr);
        return false;
    }
    p->swr_speed = speed; p->swr_eff_rate = eff;
    return true;
}

static void free_afilter(Player *p) {
    if (p->agraph) avfilter_graph_free(&p->agraph);
    p->asrc = p->asink = NULL;
}

// atempo graph at the decoder's native format/rate — changes duration, not pitch.
// atempo covers [0.5, 100]; below 0.5 two instances of sqrt(speed) are chained.
static bool init_afilter(Player *p, AVCodecContext *ctx, double speed) {
    free_afilter(p);
    const AVFilter *fsrc = avfilter_get_by_name("abuffer");
    const AVFilter *fsink = avfilter_get_by_name("abuffersink");
    if (!fsrc || !fsink || ctx->sample_rate <= 0) return false;
    p->agraph = avfilter_graph_alloc();
    if (!p->agraph) return false;
    AVChannelLayout in_ch = ctx->ch_layout;
    if (in_ch.nb_channels <= 0) av_channel_layout_default(&in_ch, 2);
    char chdesc[128];
    if (av_channel_layout_describe(&in_ch, chdesc, sizeof chdesc) < 0) snprintf(chdesc, sizeof chdesc, "stereo");
    char args[256];
    snprintf(args, sizeof args, "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             ctx->sample_rate, ctx->sample_rate, av_get_sample_fmt_name(ctx->sample_fmt), chdesc);
    if (avfilter_graph_create_filter(&p->asrc, fsrc, "in", args, NULL, p->agraph) < 0 ||
        avfilter_graph_create_filter(&p->asink, fsink, "out", NULL, NULL, p->agraph) < 0) {
        free_afilter(p); return false;
    }
    // The sink MUST be pinned to the ring's format (interleaved flt, stereo,
    // AO_RATE): the graph is free to negotiate atempo's internal format, so
    // without the trailing aformat the output format is whatever the negotiator
    // picked — feeding that to a converter expecting the decoder's layout reads
    // wild plane pointers (heap corruption). aformat also makes swr unnecessary:
    // sink frames go straight to the ring.
    char chain[256];
    int n = 0;
    if (speed < 0.5) { double r = sqrt(speed); n = snprintf(chain, sizeof chain, "atempo=%.6f,atempo=%.6f", r, r); }
    else             n = snprintf(chain, sizeof chain, "atempo=%.6f", speed);
    snprintf(chain + n, sizeof chain - n, ",aformat=sample_fmts=flt:sample_rates=%d:channel_layouts=stereo", AO_RATE);
    AVFilterInOut *outs = avfilter_inout_alloc(), *ins = avfilter_inout_alloc();
    int rc = (outs && ins) ? 0 : -1;
    if (rc == 0) {
        outs->name = av_strdup("in");  outs->filter_ctx = p->asrc;  outs->pad_idx = 0; outs->next = NULL;
        ins->name  = av_strdup("out"); ins->filter_ctx  = p->asink; ins->pad_idx = 0; ins->next  = NULL;
        rc = avfilter_graph_parse_ptr(p->agraph, chain, &ins, &outs, NULL);
        if (rc >= 0) rc = avfilter_graph_config(p->agraph, NULL);
    }
    avfilter_inout_free(&ins); avfilter_inout_free(&outs);
    if (rc < 0) { free_afilter(p); return false; }
    return true;
}

// Build the full audio chain for the current speed + pitch mode. Either way one
// output second covers `speed` media seconds, so the clock math is identical:
// - pitch-correct: atempo stretches time at native pitch, swr only converts
//   format/rate (ratio 1.0); falls back to the resample path if the graph fails
// - classic: swr does the speed via the rate lie (pitch shifts with speed)
static void rebuild_audio_chain(Player *p, AVCodecContext *ctx) {
    double spd = p->speed;
    if (spd < 0.25) spd = 0.25;
    if (spd > 4.0)  spd = 4.0;
    int pc = (p->pitch_correct && spd != 1.0) ? 1 : 0;
    if (pc && !init_afilter(p, ctx, spd)) pc = 0;
    if (!pc) free_afilter(p);
    init_swr(p, ctx, pc ? 1.0 : spd);
    audio_out_set_speed(spd);
    p->chain_speed = spd; p->chain_pitch = pc;
}

static void switch_audio(Player *p, int idx) {
    if (p->adec) avcodec_free_context(&p->adec);
    if (p->swr)  swr_free(&p->swr);
    free_afilter(p);
    p->a_stream = -1; p->has_audio = false;
    if (idx < 0) return;
    AVCodecContext *ctx = open_stream_decoder(p, idx);
    if (!ctx) return;
    if (!init_swr(p, ctx, 1.0)) { avcodec_free_context(&ctx); return; }   // probe formats
    p->adec = ctx; p->a_stream = idx; p->a_tb = p->fmt->streams[idx]->time_base; p->has_audio = true;
    rebuild_audio_chain(p, ctx);
}

static void switch_sub(Player *p, int idx) {
    if (p->sdec) avcodec_free_context(&p->sdec);
    free(p->sub_header); p->sub_header = NULL; p->sub_header_size = 0;
    p->s_stream = -1; p->sub_is_bitmap = false;
    M_lock(&p->lock); p->sub_gen++; M_unlock(&p->lock);
    if (idx < 0) return;
    AVCodecContext *ctx = open_stream_decoder(p, idx);
    if (!ctx) return;
    p->sdec = ctx; p->s_stream = idx; p->s_tb = p->fmt->streams[idx]->time_base;
    p->sub_is_bitmap = codec_is_bitmap_sub(ctx->codec_id);
    if (ctx->subtitle_header && ctx->subtitle_header_size > 0) {
        p->sub_header = (uint8_t *)malloc(ctx->subtitle_header_size + 1);
        if (p->sub_header) {
            memcpy(p->sub_header, ctx->subtitle_header, ctx->subtitle_header_size);
            p->sub_header[ctx->subtitle_header_size] = 0;
            p->sub_header_size = ctx->subtitle_header_size;
        }
    }
}

static void decode_loop(Player *p) {
    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frame = av_frame_alloc();
    while (1) {
        M_lock(&p->lock);
        bool stop = p->stop;
        bool seek = p->seek_req; double tgt = p->seek_target; if (seek) p->seek_req = 0;
        int ra = p->req_audio, rs = p->req_sub; p->req_audio = p->req_sub = NO_REQ;
        double cur = p->v_clock;
        M_unlock(&p->lock);
        if (stop) break;

        bool resync = false;
        if (ra != NO_REQ) { switch_audio(p, ra); resync = true; }
        if (rs != NO_REQ) { switch_sub(p, rs);   resync = true; }
        if (seek)   { do_seek(p, tgt); continue; }
        if (resync) { do_seek(p, cur); continue; }

        // playback speed or pitch mode changed → rebuild the audio chain
        double spd = p->speed;
        if (spd < 0.25) spd = 0.25;
        if (spd > 4.0)  spd = 4.0;
        int wantpc = (p->pitch_correct && spd != 1.0) ? 1 : 0;
        if (p->adec && p->swr && (spd != p->chain_speed || wantpc != p->chain_pitch))
            rebuild_audio_chain(p, p->adec);

        static int perf = -1; if (perf < 0) perf = getenv("TIVI_PERF") ? 1 : 0;
        static double lp_read, lp_aud, lp_vid, lp_sub; static int64_t lp_w0;
        int64_t lt0 = perf ? av_gettime_relative() : 0;
        int r = av_read_frame(p->fmt, pkt);
        int64_t lt1 = perf ? av_gettime_relative() : 0;
        if (r < 0) {
            if (p->vdec) decode_video(p, NULL, frame);     // flush
            if (p->adec) decode_audio(p, NULL, frame);
            M_lock(&p->lock); p->demux_eof = true;
            while (!p->stop && !p->seek_req && p->req_audio == NO_REQ && p->req_sub == NO_REQ)
                C_wait_ms(&p->cond, &p->lock, 50);
            M_unlock(&p->lock);
            continue;
        }
        if      (pkt->stream_index == p->v_stream && p->vdec) decode_video(p, pkt, frame);
        else if (pkt->stream_index == p->a_stream && p->adec) decode_audio(p, pkt, frame);
        else if (pkt->stream_index == p->s_stream && p->sdec) decode_sub(p, pkt);
        if (perf) {
            int64_t lt2 = av_gettime_relative();
            double ms = (lt2 - lt1) / 1000.0;
            lp_read += (lt1 - lt0) / 1000.0;
            if      (pkt->stream_index == p->v_stream) lp_vid += ms;
            else if (pkt->stream_index == p->a_stream) lp_aud += ms;
            else                                       lp_sub += ms;
            if (!lp_w0) lp_w0 = lt0;
            if (lt2 - lp_w0 >= 2000000) {
                printf("[perf] loop2s: read %.0f  audio %.0f  video %.0f  sub %.0f ms  afill %d\n",
                       lp_read, lp_aud, lp_vid, lp_sub, audio_out_fill());
                fflush(stdout);
                lp_read = lp_aud = lp_vid = lp_sub = 0; lp_w0 = lt2;
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
}

#ifdef _WIN32
static DWORD WINAPI decode_thread(LPVOID arg) { decode_loop((Player *)arg); return 0; }
#else
static void *decode_thread(void *arg) { decode_loop((Player *)arg); return NULL; }
#endif

// ---------- public API ----------
Player *player_create(void) {
    av_log_set_level(AV_LOG_ERROR);
    Player *p = (Player *)calloc(1, sizeof(Player));
    if (!p) return NULL;
    M_init(&p->lock); C_init(&p->cond);
    p->v_stream = p->a_stream = p->s_stream = -1;
    p->req_audio = p->req_sub = NO_REQ;
    p->disp_buf = -1;
    p->volume = 1.0f; p->speed = 1.0;
    p->want_hw = true; p->want_gpu_convert = true;   // settings override before player_open
    p->disp_fmt = TIVI_PIX_RGBA;
    return p;
}

static void enumerate_tracks(Player *p) {
    p->ntracks = 0;
    for (unsigned i = 0; i < p->fmt->nb_streams && p->ntracks < 64; i++) {
        AVStream *st = p->fmt->streams[i];
        enum AVMediaType mt = st->codecpar->codec_type;
        if (mt != AVMEDIA_TYPE_AUDIO && mt != AVMEDIA_TYPE_SUBTITLE) continue;
        TrackInfo *t = &p->tracks[p->ntracks++];
        t->stream_index = (int)i;
        t->kind = (mt == AVMEDIA_TYPE_AUDIO) ? TRACK_AUDIO : TRACK_SUBTITLE;
        str_copy(t->codec, sizeof(t->codec), avcodec_get_name(st->codecpar->codec_id));
        AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
        AVDictionaryEntry *ttl  = av_dict_get(st->metadata, "title", NULL, 0);
        str_copy(t->lang, sizeof(t->lang), lang ? lang->value : "");
        t->is_bitmap = (mt == AVMEDIA_TYPE_SUBTITLE) && codec_is_bitmap_sub(st->codecpar->codec_id);
        // Build a friendly label: "<title|lang|#> (<codec>[, NCH])"
        char base[120];
        if (ttl && ttl->value)        snprintf(base, sizeof(base), "%s", ttl->value);
        else if (lang && lang->value) snprintf(base, sizeof(base), "%s", lang->value);
        else                          snprintf(base, sizeof(base), "%s %d",
                                               mt == AVMEDIA_TYPE_AUDIO ? "Audio" : "Subtitle", p->ntracks);
        char cod[40]; snprintf(cod, sizeof(cod), "%s", t->codec);
        char label[200];
        if (mt == AVMEDIA_TYPE_AUDIO)
            snprintf(label, sizeof(label), "%s (%s, %dch)", base, cod, st->codecpar->ch_layout.nb_channels);
        else
            snprintf(label, sizeof(label), "%s (%s)", base, cod);
        str_copy(t->title, sizeof(t->title), label);
    }
}

bool player_open(Player *p, const char *path) {
    player_close(p);
    p->errbuf[0] = 0;
    str_copy(p->path, sizeof(p->path), path);

    if (avformat_open_input(&p->fmt, path, NULL, NULL) < 0) {
        str_copy(p->errbuf, sizeof(p->errbuf), "Could not open file"); return false;
    }
    if (avformat_find_stream_info(p->fmt, NULL) < 0) {
        str_copy(p->errbuf, sizeof(p->errbuf), "Could not read stream info");
        avformat_close_input(&p->fmt); return false;
    }

    p->duration = (p->fmt->duration != AV_NOPTS_VALUE) ? (double)p->fmt->duration / AV_TIME_BASE : 0.0;
    AVDictionaryEntry *mt = av_dict_get(p->fmt->metadata, "title", NULL, 0);
    if (mt && mt->value) str_copy(p->title, sizeof(p->title), mt->value);
    else {
        const char *b = strrchr(path, '/'); const char *b2 = strrchr(path, '\\');
        if (b2 > b) b = b2;
        str_copy(p->title, sizeof(p->title), b ? b + 1 : path);
    }
    enumerate_tracks(p);

    int vs = av_find_best_stream(p->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int as = av_find_best_stream(p->fmt, AVMEDIA_TYPE_AUDIO, -1, vs, NULL, 0);
    int ss = av_find_best_stream(p->fmt, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);

    if (vs >= 0) {
        p->vdec = open_stream_decoder(p, vs);
        if (p->vdec) {
            p->v_stream = vs; p->v_tb = p->fmt->streams[vs]->time_base;
            p->v_live = !(p->fmt->streams[vs]->disposition & AV_DISPOSITION_ATTACHED_PIC);
            p->vw = p->vdec->width; p->vh = p->vdec->height;
            if (p->vw <= 0 || p->vh <= 0) { p->vw = 1280; p->vh = 720; }
            AVRational fr = p->fmt->streams[vs]->avg_frame_rate;
            if (fr.num == 0 || fr.den == 0) fr = p->fmt->streams[vs]->r_frame_rate;
            p->fps = (fr.den > 0) ? av_q2d(fr) : 25.0;
            p->has_video = true;
            if (!alloc_video_pool(p)) { str_copy(p->errbuf, sizeof(p->errbuf), "Out of memory (video pool)"); player_close(p); return false; }
        }
    }
    if (as >= 0) switch_audio(p, as);
    if (ss >= 0) switch_sub(p, ss);

    if (!p->has_video && !p->has_audio) {
        str_copy(p->errbuf, sizeof(p->errbuf), "No playable video or audio stream");
        player_close(p); return false;
    }

    p->opened = true;
    p->v_clock = 0; p->audio_pts_running = 0;
    p->stop = false; p->demux_eof = false; p->reached_eof = false;
    audio_out_clear();
    audio_out_set_volume(p->volume);
    audio_out_set_paused(true);

#ifdef _WIN32
    p->thread = CreateThread(NULL, 0, decode_thread, p, 0, NULL);
    p->thread_running = (p->thread != NULL);
#else
    p->thread_running = (pthread_create(&p->thread, NULL, decode_thread, p) == 0);
#endif
    return true;
}

void player_close(Player *p) {
    if (!p) return;
    if (p->thread_running) {
        M_lock(&p->lock); p->stop = true; C_wakeall(&p->cond); M_unlock(&p->lock);
#ifdef _WIN32
        WaitForSingleObject(p->thread, INFINITE); CloseHandle(p->thread);
#else
        pthread_join(p->thread, NULL);
#endif
        p->thread_running = false;
    }
    if (p->vdec) avcodec_free_context(&p->vdec);   // releases the decoder's device ref
    if (p->adec) avcodec_free_context(&p->adec);
    if (p->sdec) avcodec_free_context(&p->sdec);
    if (p->hwsw) av_frame_free(&p->hwsw);
    if (p->hw_device_ctx) av_buffer_unref(&p->hw_device_ctx);
    p->hw_pix_fmt = AV_PIX_FMT_NONE; p->hw_logged = false; p->hw_in_use = false;
    p->disp_fmt = TIVI_PIX_RGBA; p->v_bitdepth = 8; p->v_colorspace = 1; p->v_range = 0;
    if (p->snap_sws) { sws_freeContext(p->snap_sws); p->snap_sws = NULL; }
    free(p->snap_rgba); p->snap_rgba = NULL;
    if (p->sws)  { sws_freeContext(p->sws); p->sws = NULL; }
    if (p->swr)  swr_free(&p->swr);
    free_afilter(p);
    if (p->afr)  av_frame_free(&p->afr);
    if (p->fmt)  avformat_close_input(&p->fmt);
    free_video_pool(p);
    clear_sub_queues(p);
    free(p->cur_bmp.rgba); p->cur_bmp.rgba = NULL; p->cur_bmp_valid = false;
    free(p->sub_header); p->sub_header = NULL; p->sub_header_size = 0;
    free(p->swr_buf); p->swr_buf = NULL; p->swr_buf_frames = 0;
    p->v_stream = p->a_stream = p->s_stream = -1;
    p->has_video = p->has_audio = false; p->v_live = false; p->preroll_to = 0;
    p->ntracks = 0; p->opened = false; p->stop = false;
    p->title[0] = 0;
}

void player_destroy(Player *p) {
    if (!p) return;
    player_close(p);
    M_free(&p->lock);
    free(p);
}

bool player_is_open(const Player *p) { return p && p->opened; }
const char *player_error(const Player *p) { return p ? p->errbuf : ""; }

void player_play(Player *p)   { if (!p->opened) return; p->playing = true;  audio_out_set_paused(false); }
void player_pause(Player *p)  { if (!p->opened) return; p->playing = false; audio_out_set_paused(true); }
void player_toggle(Player *p) { if (p->playing) player_pause(p); else player_play(p); }
bool player_is_playing(const Player *p) { return p && p->playing; }

bool player_eof(Player *p) {
    M_lock(&p->lock); bool e = p->reached_eof; M_unlock(&p->lock);
    return e;
}

void player_update(Player *p, double dt) {
    if (!p->opened) return;
    M_lock(&p->lock);
    // Audio is the master clock — but only while the ring actually has samples.
    // When it starves (device underrun, or the atempo chain's bootstrap window
    // right after a speed change), freewheel at play speed instead of freezing:
    // a frozen clock stops the pacer, the video queue fills, and the single
    // decode thread blocks on a video slot before it can refill the audio.
    double aclk = (p->has_audio && audio_out_fill() > 0) ? audio_out_clock() : -1.0;
    if (aclk >= 0)            p->v_clock = aclk;
    else if (p->playing)      { p->v_clock += dt * p->speed; if (p->v_clock < 0) p->v_clock = 0; }
    double master = p->v_clock;

    int adv = 0;
    while (p->vqn > 0 && p->vq[p->vqh].pts <= master) {
        if (p->disp_buf >= 0) p->vfree[p->nfree++] = p->disp_buf;
        p->disp_buf = p->vq[p->vqh].buf; p->disp_pts = p->vq[p->vqh].pts; p->disp_fmt = p->vq[p->vqh].fmt;
        p->vqh = (p->vqh + 1) % VPOOL; p->vqn--; p->display_dirty = true;
        adv++;
    }
    {   // perf probe (TIVI_PERF=1): pacing behavior — how frames leave the queue
        static int perf = -1; if (perf < 0) perf = getenv("TIVI_PERF") ? 1 : 0;
        if (perf) {
            static int ev, fr, multi, ticks, qsum; static int64_t w0;
            int64_t t = av_gettime_relative();
            if (!w0) w0 = t;
            ticks++; qsum += p->vqn;
            if (adv) { ev++; fr += adv; if (adv > 1) multi++; }
            if (t - w0 >= 2000000) {
                printf("[perf] pace: %d events, %d frames (%.2f fr/ev), %d multi, avg queue %.1f  master %.2f\n",
                       ev, fr, ev ? (double)fr / ev : 0.0, multi, ticks ? (double)qsum / ticks : 0.0, master);
                fflush(stdout);
                w0 = t; ev = fr = multi = ticks = qsum = 0;
            }
        }
    }
    bool audio_drained = !p->has_audio || audio_out_fill() == 0;
    p->reached_eof = p->demux_eof && p->vqn == 0 && audio_drained;
    C_wakeall(&p->cond);
    M_unlock(&p->lock);

    // advance the active bitmap subtitle (UI-owned)
    long long now_ms = (long long)(master * 1000.0);
    M_lock(&p->lock);
    if (p->cur_bmp_valid && now_ms > p->cur_bmp.end_ms) { free(p->cur_bmp.rgba); p->cur_bmp.rgba = NULL; p->cur_bmp_valid = false; }
    while (p->sbqn > 0) {
        SubBmp *s = &p->sbq[p->sbqh];
        if (s->end_ms < now_ms) { free(s->rgba); p->sbqh = (p->sbqh + 1) % SBQ; p->sbqn--; continue; }
        if (s->start_ms <= now_ms) {
            if (p->cur_bmp_valid) free(p->cur_bmp.rgba);
            p->cur_bmp = *s; p->cur_bmp_valid = true;
            p->sbqh = (p->sbqh + 1) % SBQ; p->sbqn--;
        } else break;
    }
    M_unlock(&p->lock);
}

double player_position(Player *p) { M_lock(&p->lock); double v = p->v_clock; M_unlock(&p->lock); return v; }
double player_duration(const Player *p) { return p->duration; }

void player_seek(Player *p, double seconds) {
    if (!p->opened) return;
    if (p->duration > 0 && seconds > p->duration) seconds = p->duration;
    if (seconds < 0) seconds = 0;
    M_lock(&p->lock); p->seek_target = seconds; p->seek_req = 1; C_wakeall(&p->cond); M_unlock(&p->lock);
}
void player_seek_relative(Player *p, double delta) { player_seek(p, player_position(p) + delta); }

void player_set_volume(Player *p, float v) { p->volume = v; audio_out_set_volume(v); }
void player_set_speed(Player *p, double s)  {
    if (s < 0.25) s = 0.25;
    if (s > 4)    s = 4;
    M_lock(&p->lock); p->speed = s; C_wakeall(&p->cond); M_unlock(&p->lock);
}
double player_speed(const Player *p) { return p->speed; }

void player_set_pitch_correct(Player *p, bool on) {
    M_lock(&p->lock); p->pitch_correct = on ? 1 : 0; C_wakeall(&p->cond); M_unlock(&p->lock);
}

bool player_has_video(const Player *p) { return p->has_video; }
int  player_video_width(const Player *p)  { return p->vw; }
int  player_video_height(const Player *p) { return p->vh; }
double player_fps(const Player *p) { return p->fps; }

bool player_frame(Player *p, TiviFrame *out, bool *changed) {
    M_lock(&p->lock);
    if (p->disp_buf < 0) { if (changed) *changed = false; M_unlock(&p->lock); return false; }
    uint8_t *buf = p->vbuf[p->disp_buf];
    TiviPixFmt f = p->disp_fmt;
    out->fmt = f; out->w = p->vw; out->h = p->vh;
    out->colorspace = p->v_colorspace; out->full_range = p->v_range;
    if (f == TIVI_PIX_RGBA) {
        out->plane[0] = buf; out->stride[0] = p->vw * 4;
        out->plane[1] = NULL; out->stride[1] = 0;
        out->bitdepth = 8;
    } else {
        int bytes = (f == TIVI_PIX_P010) ? 2 : 1;
        int cw = (p->vw + 1) / 2;
        out->plane[0] = buf;                                 out->stride[0] = p->vw * bytes;
        out->plane[1] = buf + (size_t)p->vw * bytes * p->vh; out->stride[1] = cw * 2 * bytes;
        out->bitdepth = (f == TIVI_PIX_P010) ? 10 : 8;
    }
    if (changed) *changed = p->display_dirty;
    p->display_dirty = false;
    M_unlock(&p->lock);
    return true;
}

bool player_snapshot_rgba(Player *p, uint8_t **rgba, int *w, int *h) {
    M_lock(&p->lock);
    int idx = p->disp_buf; TiviPixFmt f = p->disp_fmt; int vw = p->vw, vh = p->vh;
    M_unlock(&p->lock);
    if (idx < 0) return false;
    uint8_t *buf = p->vbuf[idx];
    if (f == TIVI_PIX_RGBA) { *rgba = buf; *w = vw; *h = vh; return true; }
    // Planar display frame — convert to RGBA on demand into the snapshot scratch.
    if (!p->snap_rgba) { p->snap_rgba = (uint8_t *)malloc((size_t)vw * vh * 4); if (!p->snap_rgba) return false; }
    int bytes = (f == TIVI_PIX_P010) ? 2 : 1, cw = (vw + 1) / 2;
    enum AVPixelFormat inf = (f == TIVI_PIX_P010) ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    const uint8_t *sd[4] = { buf, buf + (size_t)vw * bytes * vh, NULL, NULL };
    int sst[4] = { vw * bytes, cw * 2 * bytes, 0, 0 };
    p->snap_sws = sws_getCachedContext(p->snap_sws, vw, vh, inf, vw, vh, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
    if (!p->snap_sws) return false;
    uint8_t *dst[4] = { p->snap_rgba, NULL, NULL, NULL };
    int dstst[4] = { vw * 4, 0, 0, 0 };
    sws_scale(p->snap_sws, sd, sst, 0, vh, dst, dstst);
    *rgba = p->snap_rgba; *w = vw; *h = vh;
    return true;
}

void player_set_hw_decode(Player *p, bool on)   { p->want_hw = on; }
void player_set_gpu_convert(Player *p, bool on) { M_lock(&p->lock); p->want_gpu_convert = on; M_unlock(&p->lock); }
bool player_hw_active(const Player *p)          { return p->hw_in_use; }
const char *player_decode_desc(const Player *p) { return p->hw_in_use ? "D3D11VA" : "software"; }
const char *player_convert_desc(const Player *p) {
    switch (p->disp_fmt) {
        case TIVI_PIX_P010: return "GPU shader (p010)";
        case TIVI_PIX_NV12: return "GPU shader (nv12)";
        default:            return "CPU (sws)";
    }
}

int player_track_count(const Player *p) { return p->ntracks; }
const TrackInfo *player_track(const Player *p, int i) { return (i >= 0 && i < p->ntracks) ? &p->tracks[i] : NULL; }
int player_current_audio(const Player *p) { return p->a_stream; }
int player_current_sub(const Player *p) { return p->s_stream; }
void player_set_audio_track(Player *p, int stream_index) { M_lock(&p->lock); p->req_audio = stream_index; C_wakeall(&p->cond); M_unlock(&p->lock); }
void player_set_sub_track(Player *p, int stream_index)   { M_lock(&p->lock); p->req_sub   = stream_index; C_wakeall(&p->cond); M_unlock(&p->lock); }

const uint8_t *player_sub_header(Player *p, int *size) { if (size) *size = p->sub_header_size; return p->sub_header; }
bool player_sub_is_bitmap(Player *p) { return p->sub_is_bitmap; }
unsigned player_sub_generation(Player *p) { M_lock(&p->lock); unsigned g = p->sub_gen; M_unlock(&p->lock); return g; }

bool player_pop_sub_text(Player *p, long long *start_ms, long long *dur_ms, char **ass_out) {
    M_lock(&p->lock);
    if (p->subqn == 0) { M_unlock(&p->lock); return false; }
    *start_ms = p->subq[p->subqh].start_ms; *dur_ms = p->subq[p->subqh].dur_ms; *ass_out = p->subq[p->subqh].ass;
    p->subqh = (p->subqh + 1) % SUBQ; p->subqn--;
    M_unlock(&p->lock);
    return true;
}

bool player_active_sub_bitmap(Player *p, uint8_t **rgba, int *w, int *h) {
    M_lock(&p->lock);
    bool ok = p->cur_bmp_valid && p->cur_bmp.rgba;
    if (ok) { *rgba = p->cur_bmp.rgba; *w = p->cur_bmp.w; *h = p->cur_bmp.h; }
    M_unlock(&p->lock);
    return ok;
}

const char *player_title(const Player *p) { return p->title; }
const char *player_path(const Player *p)  { return p->path; }

// ---------- headless probe ----------
int player_probe(const char *path) {
    av_log_set_level(AV_LOG_INFO);
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) { fprintf(stderr, "probe: cannot open %s\n", path); return 1; }
    if (avformat_find_stream_info(fmt, NULL) < 0) { fprintf(stderr, "probe: no stream info\n"); avformat_close_input(&fmt); return 1; }
    printf("file: %s\n", path);
    printf("format: %s\n", fmt->iformat ? fmt->iformat->long_name : "?");
    printf("duration: %.2fs\n", fmt->duration != AV_NOPTS_VALUE ? (double)fmt->duration / AV_TIME_BASE : 0.0);
    int vs = -1, as = -1;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVCodecParameters *par = fmt->streams[i]->codecpar;
        const char *kind = av_get_media_type_string(par->codec_type);
        printf("  stream %u: %s %s", i, kind ? kind : "?", avcodec_get_name(par->codec_id));
        if (par->codec_type == AVMEDIA_TYPE_VIDEO) { printf(" %dx%d", par->width, par->height); if (vs < 0) vs = (int)i; }
        if (par->codec_type == AVMEDIA_TYPE_AUDIO) { printf(" %dHz %dch", par->sample_rate, par->ch_layout.nb_channels); if (as < 0) as = (int)i; }
        AVDictionaryEntry *lang = av_dict_get(fmt->streams[i]->metadata, "language", NULL, 0);
        if (lang) printf(" [%s]", lang->value);
        printf("\n");
    }
    int decoded_v = 0, decoded_a = 0;
    AVCodecContext *vd = NULL, *ad = NULL;
    if (vs >= 0) { const AVCodec *c = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id); if (c) { vd = avcodec_alloc_context3(c); avcodec_parameters_to_context(vd, fmt->streams[vs]->codecpar); avcodec_open2(vd, c, NULL); } }
    if (as >= 0) { const AVCodec *c = avcodec_find_decoder(fmt->streams[as]->codecpar->codec_id); if (c) { ad = avcodec_alloc_context3(c); avcodec_parameters_to_context(ad, fmt->streams[as]->codecpar); avcodec_open2(ad, c, NULL); } }
    AVPacket *pkt = av_packet_alloc(); AVFrame *fr = av_frame_alloc();
    while ((decoded_v < 30 || decoded_a < 30) && av_read_frame(fmt, pkt) >= 0) {
        if (vd && pkt->stream_index == vs) { if (avcodec_send_packet(vd, pkt) == 0) while (avcodec_receive_frame(vd, fr) == 0) decoded_v++; }
        if (ad && pkt->stream_index == as) { if (avcodec_send_packet(ad, pkt) == 0) while (avcodec_receive_frame(ad, fr) == 0) decoded_a++; }
        av_packet_unref(pkt);
    }
    printf("decoded: %d video frames, %d audio frames\n", decoded_v, decoded_a);
    av_frame_free(&fr); av_packet_free(&pkt);
    if (vd) avcodec_free_context(&vd);
    if (ad) avcodec_free_context(&ad);
    avformat_close_input(&fmt);
    printf("probe OK\n");
    return 0;
}
