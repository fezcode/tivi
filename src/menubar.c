// menubar.c — Hisashi OS Window Layer (hoswl) bridge. Contract in menubar.h.
//
// The whole menu tree is rebuilt as hoswl's line DSL from a MenubarState
// snapshot and pushed with one hoswl_set_menus() whenever the fingerprint of
// that snapshot moves (a few KB; cheaper than tracking per-item patches).
// Clicks come back as item ids through hoswl_poll() and are dispatched to
// main.c's callback from inside menubar_frame — the main thread, never a
// pipe thread.
//
// hoswl.h is a verbatim copy of Hisashi/sdk/hoswl/hoswl.h. Never edit the
// copy; fix upstream and re-vendor.
#define HOSWL_IMPLEMENTATION
#include "hoswl.h"
#include "menubar.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static hoswl_t  g_h;
static char     g_version[64];
static bool     g_enabled = true;
static uint32_t g_fp;          // fingerprint of the last published menu
static bool     g_published;   // g_fp is meaningful

void menubar_init(const char *app_version) {
    snprintf(g_version, sizeof g_version, "%s", app_version ? app_version : "");
}

void menubar_set_enabled(bool on) {
    g_enabled = on;
    if (!on && g_h.inited) { hoswl_shutdown(&g_h); g_published = false; }   // sends "bye" if connected
}

bool menubar_connected(void) { return hoswl_connected(&g_h) != 0; }

// FNV-1a over every field that shows up in the menu text.
static uint32_t fingerprint(const MenubarState *s) {
    uint32_t h = 2166136261u;
    #define FNV(b) (h = (h ^ (uint32_t)(unsigned char)(b)) * 16777619u)
    const int fields[] = { s->open, s->playing, s->has_next, s->has_prev, s->muted, s->repeat,
                           s->shuffle, s->speed_idx, s->pitch_correct, s->subs_on, s->cur_audio,
                           s->cur_sub, s->ntracks, s->fullscreen, s->aot, s->hw_decode,
                           s->gpu_convert, s->panel };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) { FNV(fields[i]); FNV(fields[i] >> 8); }
    for (int i = 0; i < s->ntracks && i < MENUBAR_MAX_TRACKS; i++) {
        FNV(s->tracks[i].kind); FNV(s->tracks[i].stream_index); FNV(s->tracks[i].stream_index >> 8);
        for (const char *p = s->tracks[i].title; *p; p++) FNV(*p);
        FNV('\n');
    }
    #undef FNV
    return h;
}

// Labels travel through the DSL, whose only separators are '|' and newlines;
// everything else (UTF-8 included) passes through and is JSON-escaped by hoswl.
static void dsl_label(char *out, size_t cap, const char *s) {
    size_t o = 0;
    for (; s && *s && o + 1 < cap; s++) out[o++] = (*s == '|' || *s == '\n' || *s == '\r') ? ' ' : *s;
    out[o] = 0;
}

// Row syntax: " id|Label|Key|flags" — flags: d disabled, x checked, c checkable-off.
int menubar_build_text(const MenubarState *s, char *text, size_t cap) {
    size_t o = 0;
    #define PUT(...) do { \
        int w_ = snprintf(text + o, cap - o, __VA_ARGS__); \
        if (w_ < 0 || (size_t)w_ >= cap - o) return -1; \
        o += (size_t)w_; \
    } while (0)
    #define RADIO(cond) ((cond) ? "x" : "c")
    const char *need = s->open ? "" : "d";   // rows that need an open file

    PUT("File\n");
    PUT(" file.open|Open…|O\n");
    PUT(" file.subs|Load Subtitle File…||%s\n", need);
    PUT(" -\n");
    PUT(" file.snapshot|Save Snapshot|S|%s\n", need);
    PUT(" -\n");
    PUT(" file.quit|Quit tivi\n");

    PUT("Playback\n");
    PUT(" pb.toggle|%s|Space|%s\n", s->playing ? "Pause" : "Play", need);
    PUT(" pb.stop|Stop||%s\n", need);
    PUT(" -\n");
    PUT(" pb.prev|Previous|B|%s\n", s->open ? "" : "d");
    PUT(" pb.next|Next|N|%s\n", s->has_next ? "" : "d");
    PUT(" -\n");
    PUT(" pb.back10|Back 10 s|J|%s\n", need);
    PUT(" pb.fwd10|Forward 10 s|L|%s\n", need);
    PUT(" -\n");
    PUT(" pb.speed|Speed|>\n");
    {
        static const char *names[6] = { "0.5×", "0.75×", "1× (Normal)", "1.25×", "1.5×", "2×" };
        for (int k = 0; k < 6; k++) PUT("  speed.%d|%s||%s\n", k, names[k], RADIO(s->speed_idx == k));
        PUT("  -\n");
        PUT("  speed.pitch|Keep Pitch (time-stretch)||%s\n", RADIO(s->pitch_correct));
    }
    PUT(" pb.repeat|Repeat|>\n");
    PUT("  repeat.0|Off||%s\n", RADIO(s->repeat == 0));
    PUT("  repeat.1|One||%s\n", RADIO(s->repeat == 1));
    PUT("  repeat.2|All||%s\n", RADIO(s->repeat == 2));
    PUT(" pb.shuffle|Shuffle||%s\n", RADIO(s->shuffle));

    PUT("Audio\n");
    PUT(" audio.up|Volume Up|Up\n");
    PUT(" audio.down|Volume Down|Down\n");
    PUT(" audio.mute|Mute|M|%s\n", RADIO(s->muted));
    {
        int n = 0;
        for (int i = 0; i < s->ntracks && i < MENUBAR_MAX_TRACKS; i++) if (s->tracks[i].kind == 0) n++;
        if (n > 0) {
            PUT(" -\n");
            for (int i = 0; i < s->ntracks && i < MENUBAR_MAX_TRACKS; i++) {
                if (s->tracks[i].kind != 0) continue;
                char label[240]; dsl_label(label, sizeof label, s->tracks[i].title);
                PUT(" audio.track.%d|%s||%s\n", s->tracks[i].stream_index, label,
                    RADIO(s->tracks[i].stream_index == s->cur_audio));
            }
        }
    }

    PUT("Subtitles\n");
    PUT(" sub.off|Off||%s\n", RADIO(!s->subs_on));
    for (int i = 0; i < s->ntracks && i < MENUBAR_MAX_TRACKS; i++) {
        if (s->tracks[i].kind != 1) continue;
        char label[240]; dsl_label(label, sizeof label, s->tracks[i].title);
        PUT(" sub.track.%d|%s||%s\n", s->tracks[i].stream_index, label,
            RADIO(s->subs_on && s->tracks[i].stream_index == s->cur_sub));
    }
    PUT(" -\n");
    PUT(" sub.load|Load Subtitle File…||%s\n", need);

    PUT("Video\n");
    PUT(" video.fullscreen|Full Screen|F|%s\n", RADIO(s->fullscreen));
    PUT(" video.aot|Always on Top|T|%s\n", RADIO(s->aot));
    PUT(" -\n");
    PUT(" video.hw|Hardware Decoding||%s\n", RADIO(s->hw_decode));
    PUT(" video.gpu|GPU Color Conversion||%s\n", RADIO(s->gpu_convert));
    PUT(" -\n");
    PUT(" video.adjust|Adjustments|A|%s\n", RADIO(s->panel == 4));      // PANEL_ADJUST
    PUT(" video.reset|Reset Adjustments\n");
    PUT(" -\n");
    PUT(" video.playlist|Playlist|Q|%s\n", RADIO(s->panel == 1));      // PANEL_PLAYLIST
    PUT(" video.settings|Settings|G|%s\n", RADIO(s->panel == 5));      // PANEL_SETTINGS

    PUT("Help\n");
    PUT(" help.about|About tivi…\n");
    #undef RADIO
    #undef PUT
    return 0;
}

static void publish(const MenubarState *s) {
    static char text[HOSWL_MENU_MAX];
    if (menubar_build_text(s, text, sizeof text) != 0) { fprintf(stderr, "hoswl: menu text too large, not published\n"); return; }
    if (hoswl_set_menus(&g_h, text) != 0) fprintf(stderr, "hoswl: menu rejected: %s\n", g_h.last_error);
}

void menubar_frame(const MenubarState *s, void (*on_click)(const char *id)) {
    if (!g_enabled) {
        if (g_h.inited) hoswl_shutdown(&g_h);
        g_published = false;
        return;
    }
    if (!g_h.inited) {
        if (hoswl_init(&g_h, "com.fezcode.tivi", "tivi", g_version) != 0) return;
        g_published = false;
    }
    // hoswl_poll connects lazily (retrying every 2 s while Hisashi is absent)
    // and re-sends the cached menu on every reconnect by itself.
    const char *id;
    while ((id = hoswl_poll(&g_h)) != NULL) {
        char copy[HOSWL_ID_MAX];
        snprintf(copy, sizeof copy, "%s", id);   // the click may republish, which reuses the buffer
        on_click(copy);
        if (!g_h.inited) return;                 // the click switched the integration off
    }
    uint32_t fp = fingerprint(s);
    if (!g_published || fp != g_fp) {
        g_fp = fp; g_published = true;
        publish(s);
    }
}

void menubar_shutdown(void) {
    if (g_h.inited) hoswl_shutdown(&g_h);
    g_published = false;
}
