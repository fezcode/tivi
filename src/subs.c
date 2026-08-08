#include "subs.h"

#include <ass/ass.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE sub_thr;
#else
#include <pthread.h>
typedef pthread_t sub_thr;
#endif

typedef struct { long long start_ms, dur_ms; char *ass; } SubEvent;

struct Subs {
    ASS_Library  *lib;
    ASS_Renderer *renderer;
    ASS_Track    *track;
    int w, h;
    int bottom_margin;
    uint8_t *overlay;     // w*h*4 RGBA
    size_t   overlay_sz;
    bool     have_content;
    int      last_change;

    // background full-track preload of an embedded subtitle stream
    sub_thr  pre_thread;
    bool     pre_running;
    volatile int pre_cancel;       // UI → worker
    volatile int pre_done;         // worker → UI (events list valid after join)
    bool     pre_ok;
    char     pre_path[1024];
    int      pre_stream;
    SubEvent *pre_ev; int pre_n;
    uint8_t  *pre_hdr; int pre_hdr_sz;
    bool     preloaded;

    // user font styling (plain-text tracks only)
    bool      have_style, plaintext;
    char      font_buf[64];
    float     st_scale, st_outline, st_shadow;
    unsigned  st_color, st_outline_color;
    ASS_Style ov;
};

static void ass_quiet(int level, const char *fmt, va_list args, void *data) {
    (void)level; (void)fmt; (void)args; (void)data;   // swallow libass chatter
}

Subs *subs_create(void) {
    Subs *s = (Subs *)calloc(1, sizeof(Subs));
    if (!s) return NULL;
    s->lib = ass_library_init();
    if (!s->lib) { free(s); return NULL; }
    ass_set_message_cb(s->lib, ass_quiet, NULL);
    s->renderer = ass_renderer_init(s->lib);
    if (!s->renderer) { ass_library_done(s->lib); free(s); return NULL; }
    ass_set_fonts(s->renderer, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
    s->w = 1280; s->h = 720;
    return s;
}

static void preload_cancel(Subs *s);
static void apply_style_override(Subs *s);

void subs_destroy(Subs *s) {
    if (!s) return;
    preload_cancel(s);
    if (s->track)    ass_free_track(s->track);
    if (s->renderer) ass_renderer_done(s->renderer);
    if (s->lib)      ass_library_done(s->lib);
    free(s->overlay);
    free(s);
}

void subs_set_size(Subs *s, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w == s->w && h == s->h && s->overlay) return;   // cheap to call every frame
    s->w = w; s->h = h;
    ass_set_frame_size(s->renderer, w, h);
    ass_set_storage_size(s->renderer, w, h);
    // NOTE: do NOT enable use_margins + a bottom margin — libass treats that as
    // EXTRA area added *below* the frame, pushing bottom-anchored subs outside the
    // overlay texture so they get clipped. Lifting above the control bar is done
    // by the UI shrinking the draw rect instead.
    ass_set_use_margins(s->renderer, 0);
    ass_set_margins(s->renderer, 0, 0, 0, 0);
    size_t need = (size_t)w * h * 4;
    if (need != s->overlay_sz) {
        free(s->overlay);
        s->overlay = (uint8_t *)malloc(need);
        s->overlay_sz = s->overlay ? need : 0;
    }
    s->have_content = false;
    apply_style_override(s);   // FontSize is proportional to video height
}

// 0xRRGGBB -> libass colour (0xRRGGBBAA, AA = transparency; 0 = opaque).
static uint32_t ass_rgb(unsigned rgb) {
    return ((uint32_t)(rgb & 0xFFFFFF) << 8);
}

// Push the user font style into the renderer as a selective style override,
// enabled only for plain-text tracks so authored ASS/SSA styling is preserved.
static void apply_style_override(Subs *s) {
    if (!s->renderer) return;
    memset(&s->ov, 0, sizeof(s->ov));
    static char empty[] = "";
    static char deflt[] = "sans-serif";
    s->ov.Name        = empty;
    s->ov.FontName    = s->font_buf[0] ? s->font_buf : deflt;
    // ASS style values are in script units (PlayResY = 288 for FFmpeg-converted
    // subs), NOT frame pixels — libass scales them to the render frame itself.
    // 16 @288 is FFmpeg's own default, ≈5.5% of the picture height (VLC-like).
    s->ov.FontSize    = 16.0 * (s->have_style ? s->st_scale : 1.0f);
    s->ov.ScaleX = s->ov.ScaleY = 1.0; s->ov.Spacing = 0.0;
    s->ov.PrimaryColour = s->ov.SecondaryColour = ass_rgb(s->have_style ? s->st_color : 0xFFFFFF);
    s->ov.OutlineColour = ass_rgb(s->have_style ? s->st_outline_color : 0x000000);
    s->ov.BackColour    = ass_rgb(0x000000);
    s->ov.BorderStyle   = 1;                                // outline + drop shadow
    s->ov.Outline       = s->have_style ? s->st_outline : 2.0;
    s->ov.Shadow        = s->have_style ? s->st_shadow  : 0.0;
    s->ov.Bold          = 1;                                // VLC-style weight for plain subs
    ass_set_selective_style_override(s->renderer, &s->ov);
    int bits = (s->have_style && s->plaintext)
             ? (ASS_OVERRIDE_BIT_FONT_NAME | ASS_OVERRIDE_BIT_FONT_SIZE_FIELDS |
                ASS_OVERRIDE_BIT_COLORS | ASS_OVERRIDE_BIT_BORDER | ASS_OVERRIDE_BIT_ATTRIBUTES)
             : 0;
    ass_set_selective_style_override_enabled(s->renderer, bits);
    s->have_content = false;   // force a re-render with the new style
}

void subs_set_style(Subs *s, const SubStyle *st) {
    if (!s || !st) return;
    snprintf(s->font_buf, sizeof(s->font_buf), "%s", (st->font && st->font[0]) ? st->font : "sans-serif");
    s->st_scale         = st->scale > 0 ? st->scale : 1.0f;
    s->st_color         = st->color;
    s->st_outline_color = st->outline_color;
    s->st_outline       = st->outline;
    s->st_shadow        = st->shadow;
    s->have_style       = true;
    apply_style_override(s);
}

void subs_set_plaintext(Subs *s, bool plaintext) {
    if (!s) return;
    s->plaintext = plaintext;
    apply_style_override(s);
}

void subs_set_bottom_margin(Subs *s, int px) {
    // Deprecated: lifting subs above the control bar is handled by the UI shrinking
    // the draw rect. libass margins push subs the wrong way and clip them.
    (void)s; (void)px;
}

void subs_clear(Subs *s) {
    preload_cancel(s);
    s->preloaded = false;
    if (s->track) { ass_free_track(s->track); s->track = NULL; }
    s->have_content = false;
}

void subs_begin_embedded(Subs *s, const uint8_t *header, int size) {
    subs_clear(s);
    s->track = ass_new_track(s->lib);
    if (!s->track) return;
    if (header && size > 0) ass_process_codec_private(s->track, (char *)header, size);
}

void subs_feed(Subs *s, const char *ass_line, long long start_ms, long long dur_ms) {
    if (!s->track || !ass_line) return;
    ass_process_chunk(s->track, (char *)ass_line, (int)strlen(ass_line), start_ms, dur_ms);
}

bool subs_has_track(const Subs *s) { return s && s->track != NULL; }

// portable case-insensitive compare (avoid platform stricmp name differences)
static int ci_cmp(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static bool ext_is(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    return ci_cmp(dot + 1, ext) == 0;
}

static void free_events(SubEvent *ev, int n) {
    for (int i = 0; i < n; i++) free(ev[i].ass);
    free(ev);
}

// Demux + decode one subtitle stream of `path` completely into an event list
// (plus a copy of the decoder's ASS header). stream_index < 0 picks the best
// subtitle stream. Other streams are discarded so the scan is demux-only.
// `cancel` (may be NULL) aborts the scan. On false all outputs are zeroed.
static bool collect_events(const char *path, int stream_index, volatile int *cancel,
                           SubEvent **out_ev, int *out_n, uint8_t **out_hdr, int *out_hdr_sz) {
    *out_ev = NULL; *out_n = 0; *out_hdr = NULL; *out_hdr_sz = 0;
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return false;
    if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); return false; }
    int si = stream_index;
    if (si < 0) si = av_find_best_stream(fmt, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    if (si < 0 || si >= (int)fmt->nb_streams ||
        fmt->streams[si]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
        avformat_close_input(&fmt); return false;
    }
    AVStream *st = fmt->streams[si];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) { avformat_close_input(&fmt); return false; }
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    if (avcodec_open2(ctx, dec, NULL) < 0) { avcodec_free_context(&ctx); avformat_close_input(&fmt); return false; }
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if ((int)i != si) fmt->streams[i]->discard = AVDISCARD_ALL;

    SubEvent *ev = NULL; int n = 0, cap = 0;
    bool aborted = false;
    AVRational tb = st->time_base;
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(fmt, pkt) >= 0) {
        if (cancel && *cancel) { av_packet_unref(pkt); aborted = true; break; }
        if (pkt->stream_index == si) {
            AVSubtitle sub; int got = 0;
            if (avcodec_decode_subtitle2(ctx, &sub, &got, pkt) >= 0 && got) {
                double pts_sec = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts * av_q2d(tb) : 0.0;
                long long start_ms = (long long)(pts_sec * 1000.0) + sub.start_display_time;
                long long dur_ms = (sub.end_display_time > sub.start_display_time)
                                       ? sub.end_display_time - sub.start_display_time
                                       : (pkt->duration > 0 ? (long long)(pkt->duration * av_q2d(tb) * 1000.0) : 4000);
                for (unsigned r = 0; r < sub.num_rects; r++) {
                    const char *a = sub.rects[r]->ass;
                    if (!a || !*a) continue;
                    if (n == cap) {
                        cap = cap ? cap * 2 : 256;
                        SubEvent *nev = (SubEvent *)realloc(ev, (size_t)cap * sizeof(SubEvent));
                        if (!nev) { aborted = true; break; }
                        ev = nev;
                    }
                    ev[n].start_ms = start_ms; ev[n].dur_ms = dur_ms; ev[n].ass = strdup(a);
                    if (ev[n].ass) n++;
                }
                avsubtitle_free(&sub);
                if (aborted) { av_packet_unref(pkt); break; }
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    uint8_t *hdr = NULL; int hdr_sz = 0;
    if (!aborted && ctx->subtitle_header && ctx->subtitle_header_size > 0) {
        hdr = (uint8_t *)malloc(ctx->subtitle_header_size + 1);
        if (hdr) {
            memcpy(hdr, ctx->subtitle_header, ctx->subtitle_header_size);
            hdr[ctx->subtitle_header_size] = 0;
            hdr_sz = ctx->subtitle_header_size;
        }
    }
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    if (aborted) { free_events(ev, n); free(hdr); return false; }
    *out_ev = ev; *out_n = n; *out_hdr = hdr; *out_hdr_sz = hdr_sz;
    return true;
}

// Load .srt/.vtt/.sub/.sbv etc. by demuxing + decoding the whole file with FFmpeg
// into ASS events fed to a fresh libass track.
static bool load_via_ffmpeg(Subs *s, const char *path) {
    SubEvent *ev; int n; uint8_t *hdr; int hdr_sz;
    if (!collect_events(path, -1, NULL, &ev, &n, &hdr, &hdr_sz)) return false;
    subs_begin_embedded(s, hdr, hdr_sz);
    for (int i = 0; i < n; i++) subs_feed(s, ev[i].ass, ev[i].start_ms, ev[i].dur_ms);
    free_events(ev, n); free(hdr);
    return s->track != NULL;
}

// ---------- background preload of an embedded subtitle stream ----------
#ifdef _WIN32
static DWORD WINAPI preload_main(LPVOID arg) {
#else
static void *preload_main(void *arg) {
#endif
    Subs *s = (Subs *)arg;
    s->pre_ok = collect_events(s->pre_path, s->pre_stream, &s->pre_cancel,
                               &s->pre_ev, &s->pre_n, &s->pre_hdr, &s->pre_hdr_sz);
    s->pre_done = 1;
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void preload_join(Subs *s) {
#ifdef _WIN32
    WaitForSingleObject(s->pre_thread, INFINITE);
    CloseHandle(s->pre_thread);
#else
    pthread_join(s->pre_thread, NULL);
#endif
    s->pre_running = false;
}

static void preload_cancel(Subs *s) {
    if (s->pre_running) { s->pre_cancel = 1; preload_join(s); }
    free_events(s->pre_ev, s->pre_n); s->pre_ev = NULL; s->pre_n = 0;
    free(s->pre_hdr); s->pre_hdr = NULL; s->pre_hdr_sz = 0;
    s->pre_done = 0; s->pre_ok = false;
}

void subs_preload_start(Subs *s, const char *path, int stream_index) {
    if (!s || !path) return;
    preload_cancel(s);
    s->preloaded = false;
    snprintf(s->pre_path, sizeof(s->pre_path), "%s", path);
    s->pre_stream = stream_index;
    s->pre_cancel = 0; s->pre_done = 0; s->pre_ok = false;
#ifdef _WIN32
    s->pre_thread = CreateThread(NULL, 0, preload_main, s, 0, NULL);
    s->pre_running = (s->pre_thread != NULL);
#else
    s->pre_running = (pthread_create(&s->pre_thread, NULL, preload_main, s) == 0);
#endif
}

void subs_preload_update(Subs *s) {
    if (!s || !s->pre_running || !s->pre_done) return;
    preload_join(s);
    if (s->pre_ok && !s->pre_cancel) {
        // swap in a complete fresh track (also drops any duplicate live-fed events)
        if (s->track) ass_free_track(s->track);
        s->track = ass_new_track(s->lib);
        if (s->track) {
            if (s->pre_hdr && s->pre_hdr_sz > 0)
                ass_process_codec_private(s->track, (char *)s->pre_hdr, s->pre_hdr_sz);
            for (int i = 0; i < s->pre_n; i++)
                ass_process_chunk(s->track, s->pre_ev[i].ass, (int)strlen(s->pre_ev[i].ass),
                                  s->pre_ev[i].start_ms, s->pre_ev[i].dur_ms);
            s->preloaded = true;
            s->have_content = false;   // force a re-render off the new track
        }
    }
    free_events(s->pre_ev, s->pre_n); s->pre_ev = NULL; s->pre_n = 0;
    free(s->pre_hdr); s->pre_hdr = NULL; s->pre_hdr_sz = 0;
    s->pre_done = 0; s->pre_ok = false;
}

bool subs_is_preloaded(const Subs *s) { return s && s->preloaded; }

bool subs_load_file(Subs *s, const char *path) {
    if (ext_is(path, "ass") || ext_is(path, "ssa")) {
        subs_clear(s);
        s->track = ass_read_file(s->lib, (char *)path, NULL);
        return s->track != NULL;
    }
    return load_via_ffmpeg(s, path);
}

// src-over composite of one ASS_Image (single colour + alpha coverage) onto the
// straight-alpha RGBA overlay.
static void blend_image(Subs *s, ASS_Image *im) {
    unsigned color = im->color;
    int cr = (color >> 24) & 0xff, cg = (color >> 16) & 0xff, cb = (color >> 8) & 0xff;
    int opacity = 255 - (color & 0xff);
    if (opacity <= 0) return;
    for (int y = 0; y < im->h; y++) {
        int py = im->dst_y + y; if (py < 0 || py >= s->h) continue;
        const uint8_t *srow = im->bitmap + (size_t)y * im->stride;
        for (int x = 0; x < im->w; x++) {
            int px = im->dst_x + x; if (px < 0 || px >= s->w) continue;
            int cov = srow[x]; if (!cov) continue;
            int sa = cov * opacity / 255;            // 0..255 source alpha
            if (!sa) continue;
            uint8_t *d = s->overlay + ((size_t)py * s->w + px) * 4;
            int da = d[3];
            int outa = sa + da * (255 - sa) / 255;
            if (outa <= 0) { d[0] = d[1] = d[2] = d[3] = 0; continue; }
            d[0] = (uint8_t)((cr * sa + d[0] * da * (255 - sa) / 255) / outa);
            d[1] = (uint8_t)((cg * sa + d[1] * da * (255 - sa) / 255) / outa);
            d[2] = (uint8_t)((cb * sa + d[2] * da * (255 - sa) / 255) / outa);
            d[3] = (uint8_t)outa;
        }
    }
}

bool subs_render(Subs *s, long long now_ms, uint8_t **rgba, int *w, int *h, bool *changed) {
    if (changed) *changed = false;
    if (!s->track || !s->overlay) return false;
    int change = 0;
    ASS_Image *img = ass_render_frame(s->renderer, s->track, now_ms, &change);

    if (!img) {
        if (s->have_content) { if (changed) *changed = true; s->have_content = false; }
        return false;
    }
    // libass says the image list is unchanged → reuse the cached overlay.
    if (!change && s->have_content) {
        *rgba = s->overlay; *w = s->w; *h = s->h;
        return true;
    }
    memset(s->overlay, 0, s->overlay_sz);
    for (ASS_Image *it = img; it; it = it->next) blend_image(s, it);
    s->have_content = true;
    if (changed) *changed = true;
    *rgba = s->overlay; *w = s->w; *h = s->h;
    return true;
}
