#include "subs.h"

#include <ass/ass.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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

void subs_destroy(Subs *s) {
    if (!s) return;
    if (s->track)    ass_free_track(s->track);
    if (s->renderer) ass_renderer_done(s->renderer);
    if (s->lib)      ass_library_done(s->lib);
    free(s->overlay);
    free(s);
}

void subs_set_size(Subs *s, int w, int h) {
    if (w <= 0 || h <= 0) return;
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
}

void subs_set_bottom_margin(Subs *s, int px) {
    // Deprecated: lifting subs above the control bar is handled by the UI shrinking
    // the draw rect. libass margins push subs the wrong way and clip them.
    (void)s; (void)px;
}

void subs_clear(Subs *s) {
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

// Load .srt/.vtt/.sub/.sbv etc. by demuxing + decoding the whole file with FFmpeg
// into ASS events fed to a fresh libass track.
static bool load_via_ffmpeg(Subs *s, const char *path) {
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) return false;
    if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); return false; }
    int si = av_find_best_stream(fmt, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    if (si < 0) { avformat_close_input(&fmt); return false; }
    AVStream *st = fmt->streams[si];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) { avformat_close_input(&fmt); return false; }
    AVCodecContext *ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    if (avcodec_open2(ctx, dec, NULL) < 0) { avcodec_free_context(&ctx); avformat_close_input(&fmt); return false; }

    subs_begin_embedded(s, ctx->subtitle_header, ctx->subtitle_header_size);
    AVRational tb = st->time_base;
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(fmt, pkt) >= 0) {
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
                    if (a && *a) subs_feed(s, a, start_ms, dur_ms);
                }
                avsubtitle_free(&sub);
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return s->track != NULL;
}

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
