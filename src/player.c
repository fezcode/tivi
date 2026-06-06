#include "player.h"
#include "audio_out.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

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
    struct { int buf; double pts; } vq[VPOOL];
    int      vqh, vqn;
    int      disp_buf;            // index held for display, -1 = none
    double   disp_pts;
    bool     display_dirty;

    // subtitle event queues
    SubText subq[SUBQ]; int subqh, subqn;
    SubBmp  sbq[SBQ];   int sbqh, sbqn;
    SubBmp  cur_bmp;    bool cur_bmp_valid;       // UI-owned active bitmap sub
    uint8_t *sub_header; int sub_header_size; bool sub_is_bitmap; unsigned sub_gen;

    // resample scratch
    uint8_t *swr_buf; int swr_buf_frames;
    double   audio_pts_running;

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
        ctx->thread_count = 0;                 // auto: use all cores
        ctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
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

static void enqueue_buf(Player *p, int idx, double pts) {
    M_lock(&p->lock);
    if (p->stop || p->seek_req) { p->vfree[p->nfree++] = idx; M_unlock(&p->lock); return; }
    int t = (p->vqh + p->vqn) % VPOOL;
    p->vq[t].buf = idx; p->vq[t].pts = pts; p->vqn++;
    C_wakeall(&p->cond);
    M_unlock(&p->lock);
}

static void decode_video(Player *p, AVPacket *pkt, AVFrame *frame) {
    if (avcodec_send_packet(p->vdec, pkt) < 0) return;
    while (avcodec_receive_frame(p->vdec, frame) == 0) {
        int64_t ts = frame->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) ts = frame->pts;
        double pts = (ts == AV_NOPTS_VALUE) ? p->v_clock : ts * av_q2d(p->v_tb);

        int idx = acquire_buf(p);
        if (idx < 0) { av_frame_unref(frame); return; }   // aborted (stop/seek/track switch)

        p->sws = sws_getCachedContext(p->sws, frame->width, frame->height, frame->format,
                                      p->vw, p->vh, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
        if (p->sws) {
            uint8_t *dst[4] = { p->vbuf[idx], NULL, NULL, NULL };
            int dstst[4]    = { p->vw * 4, 0, 0, 0 };
            sws_scale(p->sws, (const uint8_t * const *)frame->data, frame->linesize, 0, frame->height, dst, dstst);
            enqueue_buf(p, idx, pts);
        } else {
            M_lock(&p->lock); p->vfree[p->nfree++] = idx; M_unlock(&p->lock);
        }
        av_frame_unref(frame);
    }
}

static void push_audio(Player *p, const float *buf, int frames) {
    int off = 0;
    while (off < frames) {
        M_lock(&p->lock);
        bool abort = p->stop || p->seek_req || p->req_audio != NO_REQ || p->req_sub != NO_REQ;
        M_unlock(&p->lock);
        if (abort) return;
        int wr = audio_out_push(buf + off * 2, frames - off);
        off += wr;
        if (wr == 0) msleep(4);     // ring full — let the device drain
    }
}

static void decode_audio(Player *p, AVPacket *pkt, AVFrame *frame) {
    if (avcodec_send_packet(p->adec, pkt) < 0) return;
    while (avcodec_receive_frame(p->adec, frame) == 0) {
        int out_count = (int)av_rescale_rnd(swr_get_delay(p->swr, p->adec->sample_rate) + frame->nb_samples,
                                            AO_RATE, p->adec->sample_rate, AV_ROUND_UP);
        if (out_count > p->swr_buf_frames) {
            free(p->swr_buf);
            p->swr_buf = (uint8_t *)malloc((size_t)out_count * AO_CHANNELS * sizeof(float));
            p->swr_buf_frames = p->swr_buf ? out_count : 0;
            if (!p->swr_buf) { av_frame_unref(frame); return; }
        }
        uint8_t *outp[1] = { p->swr_buf };
        int got = swr_convert(p->swr, outp, out_count, (const uint8_t **)frame->extended_data, frame->nb_samples);
        if (got > 0) {
            double sec = (frame->pts != AV_NOPTS_VALUE) ? frame->pts * av_q2d(p->a_tb) : p->audio_pts_running;
            push_audio(p, (const float *)p->swr_buf, got);
            p->audio_pts_running = sec + (double)got / (double)AO_RATE;
            audio_out_set_end_pts(p->audio_pts_running);
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
    int64_t ts = (int64_t)(target * AV_TIME_BASE);
    av_seek_frame(p->fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
    if (p->vdec) avcodec_flush_buffers(p->vdec);
    if (p->adec) avcodec_flush_buffers(p->adec);
    if (p->sdec) avcodec_flush_buffers(p->sdec);
    audio_out_clear();
    M_lock(&p->lock);
    while (p->vqn > 0) { p->vfree[p->nfree++] = p->vq[p->vqh].buf; p->vqh = (p->vqh + 1) % VPOOL; p->vqn--; }
    clear_sub_queues(p);
    p->v_clock = target; p->audio_pts_running = target;
    p->demux_eof = false; p->reached_eof = false;
    C_wakeall(&p->cond);
    M_unlock(&p->lock);
}

static void switch_audio(Player *p, int idx) {
    if (p->adec) avcodec_free_context(&p->adec);
    if (p->swr)  swr_free(&p->swr);
    p->a_stream = -1; p->has_audio = false;
    if (idx < 0) return;
    AVCodecContext *ctx = open_stream_decoder(p, idx);
    if (!ctx) return;
    AVChannelLayout out_ch; av_channel_layout_default(&out_ch, AO_CHANNELS);
    AVChannelLayout in_ch  = ctx->ch_layout;
    if (in_ch.nb_channels <= 0) av_channel_layout_default(&in_ch, 2);
    if (swr_alloc_set_opts2(&p->swr, &out_ch, AV_SAMPLE_FMT_FLT, AO_RATE,
                            &in_ch, ctx->sample_fmt, ctx->sample_rate, 0, NULL) < 0 || swr_init(p->swr) < 0) {
        avcodec_free_context(&ctx); if (p->swr) swr_free(&p->swr); return;
    }
    p->adec = ctx; p->a_stream = idx; p->a_tb = p->fmt->streams[idx]->time_base; p->has_audio = true;
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

        int r = av_read_frame(p->fmt, pkt);
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
    if (p->vdec) avcodec_free_context(&p->vdec);
    if (p->adec) avcodec_free_context(&p->adec);
    if (p->sdec) avcodec_free_context(&p->sdec);
    if (p->sws)  { sws_freeContext(p->sws); p->sws = NULL; }
    if (p->swr)  swr_free(&p->swr);
    if (p->fmt)  avformat_close_input(&p->fmt);
    free_video_pool(p);
    clear_sub_queues(p);
    free(p->cur_bmp.rgba); p->cur_bmp.rgba = NULL; p->cur_bmp_valid = false;
    free(p->sub_header); p->sub_header = NULL; p->sub_header_size = 0;
    free(p->swr_buf); p->swr_buf = NULL; p->swr_buf_frames = 0;
    p->v_stream = p->a_stream = p->s_stream = -1;
    p->has_video = p->has_audio = false;
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
    double aclk = p->has_audio ? audio_out_clock() : -1.0;
    if (aclk >= 0)            p->v_clock = aclk;
    else if (p->playing)      { p->v_clock += dt * p->speed; if (p->v_clock < 0) p->v_clock = 0; }
    double master = p->v_clock;

    while (p->vqn > 0 && p->vq[p->vqh].pts <= master) {
        if (p->disp_buf >= 0) p->vfree[p->nfree++] = p->disp_buf;
        p->disp_buf = p->vq[p->vqh].buf; p->disp_pts = p->vq[p->vqh].pts;
        p->vqh = (p->vqh + 1) % VPOOL; p->vqn--; p->display_dirty = true;
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
void player_set_speed(Player *p, double s)  { if (s < 0.25) s = 0.25; if (s > 4) s = 4; p->speed = s; }
double player_speed(const Player *p) { return p->speed; }

bool player_has_video(const Player *p) { return p->has_video; }
int  player_video_width(const Player *p)  { return p->vw; }
int  player_video_height(const Player *p) { return p->vh; }
double player_fps(const Player *p) { return p->fps; }

bool player_frame(Player *p, uint8_t **rgba, int *w, int *h, bool *changed) {
    M_lock(&p->lock);
    if (p->disp_buf < 0) { if (changed) *changed = false; M_unlock(&p->lock); return false; }
    *rgba = p->vbuf[p->disp_buf]; *w = p->vw; *h = p->vh;
    if (changed) *changed = p->display_dirty;
    p->display_dirty = false;
    M_unlock(&p->lock);
    return true;
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
