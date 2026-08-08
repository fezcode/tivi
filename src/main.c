// tivi — a resizable, native video player in the visual style of Timp.
// raylib for the window/UI/GPU/audio, FFmpeg (player.c) for all-codec decode,
// libass (subs.c) for subtitles. Borderless + resizable, letterboxed video,
// auto-hiding controls, video adjustments, and VLC-style track/subtitle/playlist
// handling.
#include "raylib.h"
#include "rlgl.h"

#include "player.h"
#include "subs.h"
#include "yuvtex.h"
#include "audio_out.h"
#include "playlist.h"
#include "osvideo.h"
#include "viconfig.h"
#include "mediakeys.h"
#include "singleinst.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TIVI_VERSION "0.2.0"
#define SS 2                 // supersample factor — the whole UI is rendered at
                             // SS× and downscaled with bilinear, for smooth AA on
                             // every shape, icon, and glyph (as in Timp).

// ---------- palette (Timp-style color themes, selectable in Settings) ----------
static Color BG0, BG1, TXT, MUT, TRK, ACCENT;

typedef struct { const char *name; Color bg0, bg1, txt, mut, trk, accent; } Theme;
static const Theme THEMES[] = {
    { "Gold",   { 18,16,13,255 }, { 8,7,5,255 }, { 236,227,207,255 }, { 150,139,114,255 }, { 60,54,42,255 }, { 201,164, 90,255 } },
    { "Ocean",  { 13,16,20,255 }, { 5,7,9,255 }, { 211,224,236,255 }, { 117,136,152,255 }, { 42,52,62,255 }, {  96,160,210,255 } },
    { "Forest", { 13,18,14,255 }, { 5,8,6,255 }, { 215,232,216,255 }, { 122,146,124,255 }, { 44,58,46,255 }, { 110,185,125,255 } },
    { "Rose",   { 20,14,15,255 }, { 9,5,6,255 }, { 238,215,218,255 }, { 156,122,127,255 }, { 64,44,47,255 }, { 214,110,120,255 } },
    { "Slate",  { 16,16,18,255 }, { 6,6,8,255 }, { 224,224,228,255 }, { 134,134,142,255 }, { 52,52,58,255 }, { 168,170,182,255 } },
};
#define NTHEMES ((int)(sizeof(THEMES) / sizeof(THEMES[0])))
static int g_theme = 0;
static bool g_icon_dirty = false;   // re-tint the app icon at the top of the next
                                    // frame — NEVER inside the render (set_app_icon
                                    // uses a texture mode, which cannot nest)

// "Adaptive" pseudo-theme (index NTHEMES): the palette drifts toward tints derived
// from the average color of the playing video. Targets are recomputed from a sparse
// pixel grid ~2×/s (negligible cost) and the live palette eases toward them.
static Color g_ad_bg0, g_ad_bg1, g_ad_txt, g_ad_mut, g_ad_trk, g_ad_acc;
static bool  g_ad_have = false;
static double g_ad_last = 0;
static float clampf(float v, float lo, float hi);   // defined below

static Color clerp(Color a, Color b, float t) {
    return (Color){ (unsigned char)(a.r + (b.r - a.r) * t), (unsigned char)(a.g + (b.g - a.g) * t),
                    (unsigned char)(a.b + (b.b - a.b) * t), 255 };
}

static void apply_theme(int idx) {
    if (idx < 0 || idx > NTHEMES) idx = 0;
    g_theme = idx;
    const Theme *t = &THEMES[(idx < NTHEMES) ? idx : 0];   // adaptive seeds from Gold
    BG0 = t->bg0; BG1 = t->bg1; TXT = t->txt; MUT = t->mut; TRK = t->trk; ACCENT = t->accent;
    g_ad_have = false;
    g_icon_dirty = true;
}

// Derive the adaptive palette targets from the frame's average color.
static void adaptive_targets(float r, float g, float b) {
    Vector3 hsv = ColorToHSV((Color){ (unsigned char)(r * 255), (unsigned char)(g * 255), (unsigned char)(b * 255), 255 });
    float h = hsv.x, s = clampf(hsv.y * 1.5f, 0.30f, 0.72f);
    g_ad_acc = ColorFromHSV(h, s, 0.78f);
    g_ad_bg0 = ColorFromHSV(h, 0.32f, 0.085f);
    g_ad_bg1 = ColorFromHSV(h, 0.34f, 0.045f);
    g_ad_trk = ColorFromHSV(h, 0.28f, 0.22f);
    g_ad_mut = ColorFromHSV(h, 0.16f, 0.56f);
    g_ad_txt = ColorFromHSV(h, 0.06f, 0.93f);
    g_ad_have = true;
}

// app/taskbar icon — the retro-TV mark from assets/tivi-icon.svg, tinted with the
// theme accent so the window icon follows the palette (re-tinted on theme change)
static void set_app_icon(void) {
    RenderTexture2D it = LoadRenderTexture(64, 64);
    BeginTextureMode(it); ClearBackground(BLANK);
    Color dark = { 16, 13, 9, 255 };
    DrawRectangleRounded((Rectangle){ 2, 2, 60, 60 }, 0.34f, 16, dark);
    DrawLineEx((Vector2){ 27, 26 }, (Vector2){ 19, 12 }, 3.0f, ACCENT);   // antennae
    DrawLineEx((Vector2){ 37, 26 }, (Vector2){ 45, 12 }, 3.0f, ACCENT);
    DrawCircleV((Vector2){ 19, 12 }, 2.8f, ACCENT);
    DrawCircleV((Vector2){ 45, 12 }, 2.8f, ACCENT);
    DrawRectangleRounded((Rectangle){ 10, 25, 44, 32 }, 0.35f, 12, ACCENT);   // body
    DrawRectangleRounded((Rectangle){ 15, 30, 34, 22 }, 0.40f, 12, dark);     // screen
    DrawTriangle((Vector2){ 28, 35 }, (Vector2){ 28, 47 }, (Vector2){ 38, 41 }, ACCENT);
    EndTextureMode();
    Image ico = LoadImageFromTexture(it.texture); ImageFlipVertical(&ico);
    SetWindowIcon(ico); UnloadImage(ico); UnloadRenderTexture(it);
}

// ---------- fonts ----------
static Font fBig, fUI, fSmall;

// ---------- helpers ----------
static float approach(float c, float t, float dt) { return c + (t - c) * (1.0f - expf(-dt * 14.0f)); }
static Color alpha(Color c, unsigned char a) { c.a = a; return c; }
static Color afade(Color c, float f) { c.a = (unsigned char)(c.a * (f < 0 ? 0 : f > 1 ? 1 : f)); return c; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// Smooth filled circle. raylib's DrawCircle uses ~36 segments, so big circles look
// faceted (polygonal) even under supersampling. Scale the segment count with the
// radius so the curve stays smooth.
static void scircle(float cx, float cy, float r, Color c) {
    int segs = (int)(r * 4.0f);
    if (segs < 64) segs = 64;
    if (segs > 360) segs = 360;
    DrawCircleSector((Vector2){ cx, cy }, r, 0, 360, segs, c);
}

static void fmt_time(double s, char *buf, int cap) {
    if (s < 0 || s != s) s = 0;
    int t = (int)s, h = t / 3600, m = (t % 3600) / 60, sec = t % 60;
    if (h > 0) snprintf(buf, cap, "%d:%02d:%02d", h, m, sec);
    else       snprintf(buf, cap, "%d:%02d", m, sec);
}

static void draw_fit(Font f, const char *txt, Vector2 pos, float size, float sp, Color c, float maxw) {
    char buf[1024]; snprintf(buf, sizeof(buf), "%s", txt);
    if (MeasureTextEx(f, buf, size, sp).x <= maxw) { DrawTextEx(f, buf, pos, size, sp, c); return; }
    for (int n = (int)strlen(buf); n > 1; n--) {
        buf[n - 1] = 0; char tmp[1032]; snprintf(tmp, sizeof(tmp), "%s…", buf);
        if (MeasureTextEx(f, tmp, size, sp).x <= maxw) { DrawTextEx(f, tmp, pos, size, sp, c); return; }
    }
    DrawTextEx(f, buf, pos, size, sp, c);
}

// ---------- vector transport icons (crisp, drawn with primitives) ----------
static void ic_play(float cx, float cy, float r, Color c) {
    DrawTriangle((Vector2){ cx - r * 0.7f, cy - r }, (Vector2){ cx - r * 0.7f, cy + r }, (Vector2){ cx + r, cy }, c);
}
static void ic_pause(float cx, float cy, float r, Color c) {
    float w = r * 0.46f;
    DrawRectangleRounded((Rectangle){ cx - r * 0.62f, cy - r, w, 2 * r }, 0.5f, 6, c);
    DrawRectangleRounded((Rectangle){ cx + r * 0.16f, cy - r, w, 2 * r }, 0.5f, 6, c);
}
static void ic_prev(float cx, float cy, float r, Color c) {
    DrawRectangleRounded((Rectangle){ cx - r, cy - r, r * 0.28f, 2 * r }, 0.8f, 4, c);
    DrawTriangle((Vector2){ cx + r, cy - r }, (Vector2){ cx - r * 0.35f, cy }, (Vector2){ cx + r, cy + r }, c);
}
static void ic_next(float cx, float cy, float r, Color c) {
    DrawRectangleRounded((Rectangle){ cx + r - r * 0.28f, cy - r, r * 0.28f, 2 * r }, 0.8f, 4, c);
    DrawTriangle((Vector2){ cx - r, cy - r }, (Vector2){ cx - r, cy + r }, (Vector2){ cx + r * 0.35f, cy }, c);
}
static float IT = 2.2f;   // icon stroke thickness (1x coords; supersampled ×SS)
static void ic_gear(float cx, float cy, float r, Color c) {
    for (int k = 0; k < 8; k++) {
        float a = k * (PI / 4.0f);
        DrawLineEx((Vector2){ cx + cosf(a) * r * 0.52f, cy + sinf(a) * r * 0.52f },
                   (Vector2){ cx + cosf(a) * r * 1.05f, cy + sinf(a) * r * 1.05f }, r * 0.42f, c);
    }
    DrawRing((Vector2){ cx, cy }, r * 0.34f, r * 0.76f, 0, 360, 40, c);   // body + hollow hub
}
static void ic_list(float cx, float cy, float r, Color c) {   // playlist: bulleted lines
    for (int i = 0; i < 3; i++) {
        float yy = cy - r * 0.7f + i * r * 0.7f;
        DrawCircleV((Vector2){ cx - r * 0.86f, yy }, r * 0.15f, c);
        DrawLineEx((Vector2){ cx - r * 0.5f, yy }, (Vector2){ cx + r, yy }, IT, c);
    }
}
static void ic_cc(float cx, float cy, float r, Color c) {     // subtitles: framed text lines
    Rectangle b = { cx - r, cy - r * 0.7f, 2 * r, r * 1.4f };
    DrawRectangleLinesEx(b, IT, c);
    DrawLineEx((Vector2){ b.x + r * 0.34f, cy - r * 0.16f }, (Vector2){ b.x + b.width - r * 0.34f, cy - r * 0.16f }, IT, c);
    DrawLineEx((Vector2){ b.x + r * 0.34f, cy + r * 0.30f }, (Vector2){ b.x + b.width - r * 0.85f, cy + r * 0.30f }, IT, c);
}
static void ic_speaker(float cx, float cy, float r, Color c) {
    DrawRectangleRec((Rectangle){ cx - r, cy - r * 0.34f, r * 0.5f, r * 0.68f }, c);       // box
    DrawTriangle((Vector2){ cx - r * 0.5f, cy - r * 0.34f }, (Vector2){ cx - r * 0.5f, cy + r * 0.34f }, (Vector2){ cx + r * 0.18f, cy + r }, c);
    DrawTriangle((Vector2){ cx - r * 0.5f, cy - r * 0.34f }, (Vector2){ cx + r * 0.18f, cy + r }, (Vector2){ cx + r * 0.18f, cy - r }, c);
}
static void ic_audio(float cx, float cy, float r, Color c) {  // speaker + sound waves
    ic_speaker(cx - r * 0.4f, cy, r * 0.86f, c);
    DrawRing((Vector2){ cx - r * 0.22f, cy }, r * 0.95f, r * 0.95f + IT, -48, 48, 24, c);
    DrawRing((Vector2){ cx - r * 0.22f, cy }, r * 1.32f, r * 1.32f + IT, -48, 48, 24, c);
}
static void ic_speaker_mute(float cx, float cy, float r, Color c) {
    ic_speaker(cx - r * 0.18f, cy, r * 0.86f, c);
    float wx = cx + r * 0.78f, k = r * 0.42f;
    DrawLineEx((Vector2){ wx - k, cy - k }, (Vector2){ wx + k, cy + k }, IT, c);
    DrawLineEx((Vector2){ wx + k, cy - k }, (Vector2){ wx - k, cy + k }, IT, c);
}
static void ic_notes(float cx, float cy, float r, Color c) {  // audio tracks: beamed eighth notes
    float x1 = cx - r * 0.52f, x2 = cx + r * 0.62f;
    float y1 = cy + r * 0.62f, y2 = cy + r * 0.42f;         // note heads (right one higher)
    float t1 = cy - r * 0.62f, t2 = cy - r * 0.82f;         // stem tops
    DrawCircleV((Vector2){ x1 - r * 0.14f, y1 }, r * 0.32f, c);
    DrawCircleV((Vector2){ x2 - r * 0.14f, y2 }, r * 0.32f, c);
    DrawLineEx((Vector2){ x1 + r * 0.12f, y1 }, (Vector2){ x1 + r * 0.12f, t1 }, IT, c);
    DrawLineEx((Vector2){ x2 + r * 0.12f, y2 }, (Vector2){ x2 + r * 0.12f, t2 }, IT, c);
    DrawLineEx((Vector2){ x1 + r * 0.12f, t1 }, (Vector2){ x2 + r * 0.12f, t2 }, r * 0.30f, c);   // beam
}
static void ic_adjust(float cx, float cy, float r, Color c) {  // three sliders (knob per row)
    float kf[3] = { 0.66f, 0.34f, 0.74f };
    for (int i = 0; i < 3; i++) {
        float yy = cy - r * 0.72f + i * r * 0.72f;
        DrawLineEx((Vector2){ cx - r, yy }, (Vector2){ cx + r, yy }, IT, alpha(c, 170));
        DrawCircleV((Vector2){ cx - r + 2 * r * kf[i], yy }, r * 0.26f, c);
    }
}
static void ic_full(float cx, float cy, float r, Color c) {   // four corner brackets
    float k = r * 0.62f;
    DrawLineEx((Vector2){ cx - r, cy - r }, (Vector2){ cx - r + k, cy - r }, IT, c);
    DrawLineEx((Vector2){ cx - r, cy - r }, (Vector2){ cx - r, cy - r + k }, IT, c);
    DrawLineEx((Vector2){ cx + r, cy - r }, (Vector2){ cx + r - k, cy - r }, IT, c);
    DrawLineEx((Vector2){ cx + r, cy - r }, (Vector2){ cx + r, cy - r + k }, IT, c);
    DrawLineEx((Vector2){ cx - r, cy + r }, (Vector2){ cx - r + k, cy + r }, IT, c);
    DrawLineEx((Vector2){ cx - r, cy + r }, (Vector2){ cx - r, cy + r - k }, IT, c);
    DrawLineEx((Vector2){ cx + r, cy + r }, (Vector2){ cx + r - k, cy + r }, IT, c);
    DrawLineEx((Vector2){ cx + r, cy + r }, (Vector2){ cx + r, cy + r - k }, IT, c);
}

// ---------- adjustment shader ----------
static const char *FS_ADJUST =
"#version 330\n"
"in vec2 fragTexCoord; in vec4 fragColor;\n"
"uniform sampler2D texture0; uniform vec4 colDiffuse;\n"
"uniform sampler2D u_texUV;\n"                       // chroma plane (planar path)
"uniform int u_convert;\n"                           // 1 = YUV->RGB, 0 = texture0 is RGB
"uniform vec3 u_c0; uniform vec3 u_c1; uniform vec3 u_c2; uniform vec3 u_yoff;\n"
"uniform float u_bright; uniform float u_contrast; uniform float u_sat; uniform float u_hue; uniform float u_gamma;\n"
"out vec4 finalColor;\n"
"vec3 rgb2hsv(vec3 c){vec4 K=vec4(0.,-1./3.,2./3.,-1.);"
" vec4 p=mix(vec4(c.bg,K.wz),vec4(c.gb,K.xy),step(c.b,c.g));"
" vec4 q=mix(vec4(p.xyw,c.r),vec4(c.r,p.yzx),step(p.x,c.r));"
" float d=q.x-min(q.w,q.y); float e=1.0e-10;"
" return vec3(abs(q.z+(q.w-q.y)/(6.*d+e)), d/(q.x+e), q.x);}\n"
"vec3 hsv2rgb(vec3 c){vec4 K=vec4(1.,2./3.,1./3.,3.);"
" vec3 p=abs(fract(c.xxx+K.xyz)*6.-K.www);"
" return c.z*mix(K.xxx,clamp(p-K.xxx,0.,1.),c.y);}\n"
"void main(){\n"
" vec3 c;\n"
" if (u_convert == 1) {\n"                            // GPU color conversion (NV12/P010)
"   float y = texture(texture0, fragTexCoord).r;\n"
"   vec2 uv = texture(u_texUV, fragTexCoord).rg;\n"
"   c = clamp(u_c0*y + u_c1*uv.x + u_c2*uv.y + u_yoff, 0.0, 1.0);\n"
" } else {\n"                                          // compat: texture0 already RGB
"   c = texture(texture0, fragTexCoord).rgb;\n"
" }\n"
" c = pow(max(c,0.0), vec3(1.0/max(u_gamma,0.01)));\n"
" vec3 hsv = rgb2hsv(c); hsv.x = fract(hsv.x + u_hue/360.0); c = hsv2rgb(hsv);\n"
" c = (c - 0.5) * u_contrast + 0.5;\n"
" c += u_bright;\n"
" float l = dot(c, vec3(0.299,0.587,0.114)); c = mix(vec3(l), c, u_sat);\n"
" finalColor = vec4(clamp(c,0.0,1.0),1.0) * colDiffuse * fragColor;\n"
"}\n";

// ---------- global state ----------
static Player  *P;
static Subs    *SUB;
static Playlist PL;
static ViConfig CFG;

static Texture2D vtex;  static bool vtex_valid; static int vtex_w, vtex_h;
static Texture2D stex;  static bool stex_valid; static int stex_w, stex_h;
static Shader    adjShader; static int locBright, locContrast, locSat, locHue, locGamma;
static int locConvert, locC0, locC1, locC2, locYoff, locTexUV;   // GPU YUV->RGB conversion
static YuvTex    g_yuv;         // Y/UV plane textures for the shader path
static YuvXfm    g_yuv_xfm;     // current YUV->RGB transform
static int       g_perf_vidupd; // perf probe: video texture updates this window

static float g_volume = 0.85f; static bool g_muted = false;
static int   g_repeat = 0;      // 0 off · 1 one · 2 all
static bool  g_aot = false;
static bool  g_letterbox_black = true;   // black letterbox bars vs the warm tint

// subtitle source
enum { SUB_NONE = 0, SUB_EMBEDDED, SUB_EXTERNAL };
static int      g_sub_source = SUB_NONE;
static unsigned g_sub_gen_seen = 0;
static bool     g_subs_on = true;
static char     g_ext_sub_name[256] = "";   // filename of the loaded external subtitle

static bool g_click_pause = true;   // single click on the video toggles play/pause

// panels
enum { PANEL_NONE = 0, PANEL_PLAYLIST, PANEL_AUDIO, PANEL_SUBS, PANEL_ADJUST, PANEL_SETTINGS };
static int g_panel = PANEL_NONE;

// playlist panel interaction (reorder / remove / scroll)
static int    g_pl_scroll = 0;
static float  g_set_scroll = 0;     // settings panel scroll (px)
static int    g_q_press = -1, g_q_drag = -1, g_q_target = -1;
static float  g_q_press_y = 0;
static double g_q_press_t = 0;

// controls visibility + osd
static float g_ctrl = 1.0f;          // 0..1 alpha
static double g_last_activity = 0;
static char   g_osd[128]; static double g_osd_until = 0;
static char   g_note[192]; static double g_note_until = 0;   // top-right "added" toast
static bool   g_menu_open = false; static Vector2 g_menu_pos; // right-click context menu
static bool   g_quit = false;                                 // menu "Exit" → leave main loop
static double g_center_flash = 0;    // play/pause flash timer
static bool   g_center_play = false;

// window / resize
static bool g_fullscreen = false;
static int  g_fs_x, g_fs_y, g_fs_w, g_fs_h;   // geometry to restore when leaving fullscreen
static bool g_maximized = false;
static int  g_restore_x, g_restore_y, g_restore_w, g_restore_h;
static bool g_resizing = false; static int g_resize_edge = 0;  // bitmask L1 R2 T4 B8
static float g_fixed_right, g_fixed_bottom;
static bool g_moving = false; static Vector2 g_move_grab;

static void osd(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(g_osd, sizeof(g_osd), fmt, ap); va_end(ap);
    g_osd_until = GetTime() + 1.6;
}
static void note(const char *fmt, ...) {   // top-right notification (file added, etc.)
    va_list ap; va_start(ap, fmt); vsnprintf(g_note, sizeof(g_note), fmt, ap); va_end(ap);
    g_note_until = GetTime() + 3.2;
}
static bool is_sub_ext(const char *path) {
    const char *e = GetFileExtension(path);   // includes the dot
    if (!e) return false;
    return !strcmp(e, ".srt") || !strcmp(e, ".ass") || !strcmp(e, ".ssa") ||
           !strcmp(e, ".vtt") || !strcmp(e, ".sub") || !strcmp(e, ".sbv");
}

// ---------- video / subtitle textures ----------
static void destroy_textures(void) {
    if (vtex_valid) { UnloadTexture(vtex); vtex_valid = false; }
    if (stex_valid) { UnloadTexture(stex); stex_valid = false; }
}
static void ensure_vtex(int w, int h) {
    if (vtex_valid && vtex_w == w && vtex_h == h) return;
    if (vtex_valid) UnloadTexture(vtex);
    Image im = GenImageColor(w, h, BLACK);
    vtex = LoadTextureFromImage(im); UnloadImage(im);
    SetTextureFilter(vtex, TEXTURE_FILTER_BILINEAR);
    vtex_valid = true; vtex_w = w; vtex_h = h;
}
static void ensure_stex(int w, int h) {
    if (stex_valid && stex_w == w && stex_h == h) return;
    if (stex_valid) UnloadTexture(stex);
    Image im = GenImageColor(w, h, BLANK);
    stex = LoadTextureFromImage(im); UnloadImage(im);
    SetTextureFilter(stex, TEXTURE_FILTER_BILINEAR);
    stex_valid = true; stex_w = w; stex_h = h;
}

// ---------- subtitle source management ----------
static void apply_subtitles_for_new_file(void) {
    g_sub_source = SUB_NONE;
    subs_clear(SUB);
    int cur = player_current_sub(P);
    if (g_subs_on && cur >= 0) {
        if (player_video_width(P) > 0) subs_set_size(SUB, player_video_width(P), player_video_height(P));
        g_sub_source = SUB_EMBEDDED;
        g_sub_gen_seen = player_sub_generation(P) - 1;   // force rebuild next frame
    } else {
        player_set_sub_track(P, -1);
    }
}

// Push the user's font styling (from CFG) into the subtitle renderer.
static void apply_sub_style(void) {
    SubStyle st = { CFG.sub_font, CFG.sub_font_scale, CFG.sub_color, CFG.sub_outline_color, CFG.sub_outline, CFG.sub_shadow };
    subs_set_style(SUB, &st);
}
// Is the current embedded subtitle track authored ASS/SSA (keep its styling) or
// plain text (apply the user font)?
static bool cur_sub_is_ass(void) {
    int cur = player_current_sub(P);
    if (cur < 0) return false;
    for (int i = 0; i < player_track_count(P); i++) {
        const TrackInfo *t = player_track(P, i);
        if (t->kind == TRACK_SUBTITLE && t->stream_index == cur)
            return strstr(t->codec, "ass") != NULL || strstr(t->codec, "ssa") != NULL;
    }
    return false;
}

static void load_external_subs(const char *path) {
    if (player_video_width(P) > 0) subs_set_size(SUB, player_video_width(P), player_video_height(P));
    player_set_sub_track(P, -1);
    if (subs_load_file(SUB, path)) {
        g_sub_source = SUB_EXTERNAL; g_subs_on = true;
        const char *e = GetFileExtension(path);
        bool styled = e && (!strcmp(e, ".ass") || !strcmp(e, ".ssa"));
        subs_set_plaintext(SUB, !styled);
        snprintf(g_ext_sub_name, sizeof g_ext_sub_name, "%s", GetFileName(path)); osd("Subtitles: %s", GetFileName(path));
    }
    else osd("Could not load subtitles");
}

static void apply_adjustments_uniforms(void) {
    SetShaderValue(adjShader, locBright,   &CFG.brightness, SHADER_UNIFORM_FLOAT);
    SetShaderValue(adjShader, locContrast, &CFG.contrast,   SHADER_UNIFORM_FLOAT);
    SetShaderValue(adjShader, locSat,      &CFG.saturation, SHADER_UNIFORM_FLOAT);
    SetShaderValue(adjShader, locHue,      &CFG.hue,        SHADER_UNIFORM_FLOAT);
    SetShaderValue(adjShader, locGamma,    &CFG.gamma,      SHADER_UNIFORM_FLOAT);
}

// ---------- open a file ----------
static void load_file(const char *path) {
    if (!path) return;
    if (!player_open(P, path)) { osd("Can't open: %s", player_error(P)); return; }
    if (player_has_video(P)) ensure_vtex(player_video_width(P), player_video_height(P));
    player_set_volume(P, g_muted ? 0.0f : g_volume);
    player_set_speed(P, 1.0);
    apply_subtitles_for_new_file();
    player_play(P);
    g_last_activity = GetTime();
}

// Reopen the current file in place (used when a setting like hardware decoding
// only takes effect at decoder-open time), preserving position, speed and state.
static void reload_current(void) {
    if (!player_is_open(P)) return;
    char path[1024]; snprintf(path, sizeof path, "%s", player_path(P));
    double pos = player_position(P), spd = player_speed(P);
    bool was_playing = player_is_playing(P);
    load_file(path);
    player_set_speed(P, spd);
    if (pos > 0) player_seek(P, pos);
    if (!was_playing) player_pause(P);
}

// ---------- auto-queue: opening one episode queues the rest of its folder ----------
static bool is_media_ext(const char *path) {   // mirrors the Open dialog filter
    static const char *exts[] = {
        ".mp4", ".mkv", ".webm", ".avi", ".mov", ".m4v", ".wmv", ".flv", ".mpg", ".mpeg",
        ".ts", ".m2ts", ".mts", ".vob", ".ogv", ".3gp", ".divx",
        ".mp3", ".flac", ".wav", ".ogg", ".opus", ".m4a", ".aac", ".wma", ".ac3", NULL };
    const char *e = GetFileExtension(path);
    if (!e) return false;
    for (int i = 0; exts[i]; i++) {
        const char *a = e, *b = exts[i];
        while (*a && *b && tolower((unsigned char)*a) == (unsigned char)*b) { a++; b++; }
        if (!*a && !*b) return true;
    }
    return false;
}

// Natural order, case-insensitive: "E2" < "E10". Digit runs compare as numbers.
static int natcmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            const char *pa = a; while (*pa == '0') pa++;
            const char *pb = b; while (*pb == '0') pb++;
            const char *ea = pa; while (isdigit((unsigned char)*ea)) ea++;
            const char *eb = pb; while (isdigit((unsigned char)*eb)) eb++;
            if (ea - pa != eb - pb) return (ea - pa < eb - pb) ? -1 : 1;
            for (; pa < ea; pa++, pb++) if (*pa != *pb) return (*pa < *pb) ? -1 : 1;
            a = ea; b = eb;
        } else {
            int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
            if (ca != cb) return ca - cb;
            a++; b++;
        }
    }
    return (*a ? 1 : 0) - (*b ? 1 : 0);
}

typedef struct { char **v; int n, cap; } StrVec;
static void aq_collect(const char *path, void *ud) {
    StrVec *sv = (StrVec *)ud;
    if (!is_media_ext(path)) return;
    if (sv->n == sv->cap) { sv->cap = sv->cap ? sv->cap * 2 : 32; sv->v = (char **)realloc(sv->v, sv->cap * sizeof(char *)); }
    size_t len = strlen(path) + 1;
    char *d = (char *)malloc(len); memcpy(d, path, len);
    sv->v[sv->n++] = d;
}
static int aq_cmp(const void *x, const void *y) {
    return natcmp(GetFileName(*(const char *const *)x), GetFileName(*(const char *const *)y));
}

// Queue the media files that sort (naturally) after `opened` in its directory.
// Callers gate this to fresh single-file opens so a curated queue is never touched.
static void autoqueue_siblings(const char *opened) {
    if (!CFG.auto_queue || !opened) return;
    char dir[1024]; snprintf(dir, sizeof dir, "%s", opened);
    char *slash = NULL;
    for (char *p = dir; *p; p++) if (*p == '/' || *p == '\\') slash = p;
    if (!slash || slash == dir) return;
    *slash = 0;
    StrVec sv = { 0 };
    os_scan_dir_files(dir, aq_collect, &sv);
    qsort(sv.v, sv.n, sizeof(char *), aq_cmp);
    const char *oname = GetFileName(opened);
    int queued = 0;
    for (int i = 0; i < sv.n; i++) {
        if (natcmp(GetFileName(sv.v[i]), oname) > 0) { playlist_add(&PL, sv.v[i]); queued++; }
        free(sv.v[i]);
    }
    free(sv.v);
    if (queued > 0) note("Queued %d next from folder", queued);
}

static void add_cb(const char *path, void *ud) { (void)ud; playlist_add(&PL, path); }

static void open_dialog(void) {
    bool was = player_is_open(P);
    int before = playlist_count(&PL);
    os_open_media_files(add_cb, NULL);
    int added = playlist_count(&PL) - before;
    if (added == 1)      note("Added  %s", GetFileName(PL.paths[playlist_count(&PL) - 1]));
    else if (added > 1)  note("Added %d files to playlist", added);
    if (!was && added > 0) load_file(playlist_current(&PL));
    if (!was && added == 1 && playlist_count(&PL) == 1 && player_is_open(P)) autoqueue_siblings(playlist_current(&PL));
}

static void play_index(int i) { playlist_set_index(&PL, i); load_file(playlist_current(&PL)); }
static void next_track(void) { if (playlist_has_next(&PL)) load_file(playlist_next(&PL)); }
static void prev_track(void) {
    if (player_position(P) > 3.0) { player_seek(P, 0); return; }
    if (playlist_has_prev(&PL)) load_file(playlist_prev(&PL)); else player_seek(P, 0);
}

static void set_volume(float v) { g_volume = clampf(v, 0, 2); g_muted = false; player_set_volume(P, g_volume); osd("Volume %d%%", (int)(g_volume * 100 + 0.5f)); }
static void toggle_mute(void)   { g_muted = !g_muted; player_set_volume(P, g_muted ? 0.0f : g_volume); osd(g_muted ? "Muted" : "Volume %d%%", (int)(g_volume * 100 + 0.5f)); }

static void snapshot(void) {
    uint8_t *rgba; int w, h;
    if (!player_snapshot_rgba(P, &rgba, &w, &h)) { osd("No frame to snapshot"); return; }
    Image im = { rgba, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    char name[256]; snprintf(name, sizeof(name), "tivi_snapshot_%ld.png", (long)time(NULL));
    if (ExportImage(im, name)) osd("Saved %s", name); else osd("Snapshot failed");
}

// Manual borderless fullscreen: just resize the (already borderless) window to
// cover the monitor + make it topmost. We deliberately avoid raylib's
// ToggleBorderlessWindowed(), which uses glfwSetWindowMonitor() — that switches
// the window into a GLFW "fullscreen" monitor mode (so NVIDIA/Windows treat it
// like a game and pop the overlay) and restores window decorations on exit
// (bringing back the native title bar).
static void toggle_fullscreen(void) {
    void *hwnd = GetWindowHandle();
    if (!g_fullscreen) {
        Vector2 wp = GetWindowPosition();
        g_fs_x = (int)wp.x; g_fs_y = (int)wp.y; g_fs_w = GetScreenWidth(); g_fs_h = GetScreenHeight();
        int m = GetCurrentMonitor();
        Vector2 mp = GetMonitorPosition(m);
        os_round_window(hwnd, false);
        os_snap_set_enabled(false);   // no caption-drag / edge-resize while fullscreen
        SetWindowState(FLAG_WINDOW_TOPMOST);                 // cover the taskbar
        // Cover the monitor but 1px larger on every side, so the client area does
        // NOT exactly match the monitor. That stops Windows from engaging
        // "fullscreen optimizations" (flip-model / exclusive-style presentation),
        // which is what made it behave like a game and pop the NVIDIA overlay.
        SetWindowPosition((int)mp.x - 1, (int)mp.y - 1);
        SetWindowSize(GetMonitorWidth(m) + 2, GetMonitorHeight(m) + 2);
        g_fullscreen = true;
    } else {
        if (!g_aot) ClearWindowState(FLAG_WINDOW_TOPMOST);
        SetWindowSize(g_fs_w, g_fs_h);
        SetWindowPosition(g_fs_x, g_fs_y);
        os_round_window(hwnd, !g_maximized && !os_is_zoomed(hwnd));   // was maximized before fullscreen
        os_snap_set_enabled(true);
        g_fullscreen = false;
    }
}

static void toggle_maximize(void) {
    void *hwnd = GetWindowHandle();
    // In fullscreen the window is an oversized TOPMOST cover with snap disabled;
    // maximizing on top of that corrupts both state machines (the "restore" rect
    // becomes the fullscreen rect and resize dies). Treat maximize as "leave
    // fullscreen" instead.
    if (g_fullscreen) { toggle_fullscreen(); return; }
    if (os_snap_active()) { os_native_maximize_toggle(hwnd); return; }   // snap-aware
    if (!g_maximized) {
        Vector2 wp = GetWindowPosition();
        g_restore_x = (int)wp.x; g_restore_y = (int)wp.y; g_restore_w = GetScreenWidth(); g_restore_h = GetScreenHeight();
        int x, y, w, h;
        if (os_work_area(hwnd, &x, &y, &w, &h)) { SetWindowPosition(x, y); SetWindowSize(w, h); }
        g_maximized = true; os_round_window(hwnd, false);
    } else {
        SetWindowSize(g_restore_w, g_restore_h); SetWindowPosition(g_restore_x, g_restore_y);
        g_maximized = false; os_round_window(hwnd, true);
    }
}

// ---------- CLI ----------
static void print_help(void) {
    printf(
"tivi %s — a resizable, native video player (Timp-style, FFmpeg-powered)\n\n"
"Usage: tivi [options] [files...]\n\n"
"Options:\n"
"  -h, --help       Show this help and exit\n"
"  -v, --version    Show version and exit\n"
"  --probe FILE     Decode-test a file and print stream info (diagnostic)\n\n"
"Playback keys:\n"
"  Space            Play / pause                 F / dbl-click  Fullscreen\n"
"  Left / Right     Seek -5s / +5s               J / L          Seek -10s / +10s\n"
"  Up / Down        Volume up / down             M              Mute\n"
"  N / B            Next / previous in playlist   S              Snapshot (PNG)\n"
"  C                Cycle subtitle track          X              Cycle audio track\n"
"  [ / ] or - / +   Slower / faster   \\           Reset speed\n"
"  Right-click      Context menu\n"
"  Q  Playlist   A  Adjustments   G  Settings     T  Always on top   Esc  Back\n\n"
"Drag & drop files onto the window to enqueue them. Media keys are supported.\n",
        TIVI_VERSION);
}
// ---------- main ----------
int main(int argc, char **argv) {
    int argn = 0;
    char **args = os_args_utf8(argc, argv, &argn);

    // CLI modes (attach to the parent console so output is visible)
    for (int i = 1; i < argn; i++) {
        if (!strcmp(args[i], "-h") || !strcmp(args[i], "--help"))    { os_console_attach(); print_help(); return 0; }
        if (!strcmp(args[i], "-v") || !strcmp(args[i], "--version")) { os_console_attach(); printf("tivi v%s\n", TIVI_VERSION); return 0; }
        if (!strcmp(args[i], "--probe")) {
            // write to a file so it is verifiable regardless of subsystem
            freopen("tivi_probe.txt", "w", stdout); freopen("tivi_probe.txt", "a", stderr);
            int rc = (i + 1 < argn) ? player_probe(args[i + 1]) : (fprintf(stderr, "--probe needs a FILE\n"), 1);
            fflush(stdout); return rc;
        }
    }

    // single instance: forward files to a running tivi and bow out
    if (!singleinst_acquire()) {
        bool sent = false;
        for (int i = 1; i < argn; i++) if (args[i][0] != '-' && singleinst_send_file(args[i])) sent = true;
        if (!sent) singleinst_send_focus();
        return 0;
    }
    singleinst_listen_start();

    viconfig_load(&CFG);
    g_volume = CFG.volume; g_aot = CFG.always_on_top; g_subs_on = CFG.subtitles_enabled; g_letterbox_black = CFG.letterbox_black;
    g_click_pause = CFG.click_pause;
    apply_theme(CFG.theme);

    playlist_init(&PL);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_UNDECORATED | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    int initW = (CFG.has_win && CFG.win_w > 200) ? CFG.win_w : 1024;
    int initH = (CFG.has_win && CFG.win_h > 150) ? CFG.win_h : 600;
    InitWindow(initW, initH, "tivi");
    SetWindowMinSize(480, 320);
    SetTargetFPS(60);
    SetExitKey(0);
    InitAudioDevice();
    audio_out_init();

    void *hwnd = GetWindowHandle();
    os_round_window(hwnd, true);   // NOTE: do NOT DwmExtendFrameIntoClientArea here —
                                   // it re-introduces the native caption on a borderless window.
    // Aero snap: the top bar acts as a native caption (drag-to-edge snap, Win+Arrow,
    // snap layouts) and the 6px edges as native resize borders. The reserved zones
    // mirror the UI layout: 84px = "+" button area, 154px = pin/min/max/close.
    os_enable_snap(hwnd, 84, 154, 48, 6);
    if (CFG.has_win) SetWindowPosition(CFG.win_x, CFG.win_y);
    if (g_aot) SetWindowState(FLAG_WINDOW_TOPMOST);

    set_app_icon(); g_icon_dirty = false;

    // fonts (loaded at 2x for crisp downscaled text) with Latin + Turkish coverage
    static int cps[640]; int cpc = 0;
    for (int c = 0x20; c <= 0x24F; c++) cps[cpc++] = c;
    static const int extra[] = { 0x2026, 0x2022, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D };
    for (unsigned i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) cps[cpc++] = extra[i];
    fBig   = LoadFontEx("C:/Windows/Fonts/seguisb.ttf", 64, cps, cpc);
    fUI    = LoadFontEx("C:/Windows/Fonts/segoeui.ttf", 40, cps, cpc);
    fSmall = LoadFontEx("C:/Windows/Fonts/segoeui.ttf", 30, cps, cpc);
    if (fBig.texture.id == 0) fBig = GetFontDefault();
    if (fUI.texture.id == 0)  fUI = GetFontDefault();
    if (fSmall.texture.id == 0) fSmall = GetFontDefault();
    SetTextureFilter(fBig.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(fUI.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(fSmall.texture, TEXTURE_FILTER_BILINEAR);

    adjShader = LoadShaderFromMemory(NULL, FS_ADJUST);
    locBright = GetShaderLocation(adjShader, "u_bright");
    locContrast = GetShaderLocation(adjShader, "u_contrast");
    locSat = GetShaderLocation(adjShader, "u_sat");
    locHue = GetShaderLocation(adjShader, "u_hue");
    locGamma = GetShaderLocation(adjShader, "u_gamma");
    locConvert = GetShaderLocation(adjShader, "u_convert");
    locC0 = GetShaderLocation(adjShader, "u_c0");
    locC1 = GetShaderLocation(adjShader, "u_c1");
    locC2 = GetShaderLocation(adjShader, "u_c2");
    locYoff = GetShaderLocation(adjShader, "u_yoff");
    locTexUV = GetShaderLocation(adjShader, "u_texUV");

    P = player_create();
    player_set_hw_decode(P, CFG.hw_decode);
    player_set_gpu_convert(P, CFG.gpu_convert);
    SUB = subs_create();
    apply_sub_style();

    mediakeys_start();

    // queue files from argv (a subtitle file is attached to the first video)
    const char *argSub = NULL;
    for (int i = 1; i < argn; i++) if (args[i][0] != '-') { if (is_sub_ext(args[i])) argSub = args[i]; else playlist_add(&PL, args[i]); }
    if (playlist_count(&PL) > 0) load_file(playlist_current(&PL));
    if (argSub && player_is_open(P)) load_external_subs(argSub);
    if (playlist_count(&PL) == 1 && player_is_open(P)) autoqueue_siblings(playlist_current(&PL));

    g_last_activity = GetTime();
    Vector2 lastMouse = GetMousePosition();
    double last_click_t = -1; int shot_frame = -1, frame = 0;
    if (getenv("TIVI_SHOT")) {                        // dev hook: screenshot the UI
        int sf = atoi(getenv("TIVI_SHOT"));           // TIVI_SHOT=<frame> picks the moment; else ~2s in
        shot_frame = (sf > 1) ? sf : 120;
        if (getenv("TIVI_SHOT_PLAYLIST"))    g_panel = PANEL_PLAYLIST;       // panel choice for the shot
        else if (!getenv("TIVI_SHOT_PLAIN")) g_panel = PANEL_SETTINGS;       // PLAIN=1 → playback view, no panel
        if (getenv("TIVI_SHOT_SCROLL")) g_set_scroll = (float)atoi(getenv("TIVI_SHOT_SCROLL")); }

    // Supersampled render target: the whole UI is drawn at SS× then downscaled with
    // bilinear filtering, giving smooth anti-aliasing on every shape, icon and glyph.
    RenderTexture2D target = LoadRenderTexture(GetScreenWidth() * SS, GetScreenHeight() * SS);
    SetTextureFilter(target.texture, TEXTURE_FILTER_BILINEAR);

    while (!WindowShouldClose() && !g_quit) {
        double dt = GetFrameTime();
        double now = GetTime();
        int W = GetScreenWidth(), H = GetScreenHeight();
        Vector2 mp = GetMousePosition();
        bool open = player_is_open(P);
        bool playing = open && player_is_playing(P);

        if (g_icon_dirty) { set_app_icon(); g_icon_dirty = false; }   // theme changed last frame

        // adaptive theme: ease the live palette toward the sampled targets
        if (g_theme == NTHEMES && g_ad_have) {
            float k = 1.0f - expf(-(float)dt * 2.2f);
            BG0 = clerp(BG0, g_ad_bg0, k); BG1 = clerp(BG1, g_ad_bg1, k);
            TXT = clerp(TXT, g_ad_txt, k); MUT = clerp(MUT, g_ad_mut, k);
            TRK = clerp(TRK, g_ad_trk, k); ACCENT = clerp(ACCENT, g_ad_acc, k);
        }

        player_update(P, dt);

        // ---- auto-advance ----
        if (open && player_eof(P)) {
            if (g_repeat == 1) player_seek(P, 0), player_play(P);
            else if (playlist_has_next(&PL)) next_track();
            else player_pause(P);
        }

        // ---- media keys ----
        switch (mediakeys_poll()) {
            case MK_PLAYPAUSE: if (open) player_toggle(P); break;
            case MK_STOP:      if (open) { player_pause(P); player_seek(P, 0); } break;
            case MK_PREV:      prev_track(); break;
            case MK_NEXT:      next_track(); break;
            default: break;
        }

        // ---- forwarded files from a 2nd launch ----
        { char fwd[4096]; int firstNew = -1, fwdAdded = 0;
          while (singleinst_poll_file(fwd, sizeof fwd)) { playlist_add(&PL, fwd); if (firstNew < 0) firstNew = playlist_count(&PL) - 1; fwdAdded++; }
          if (firstNew >= 0) play_index(firstNew);
          if (fwdAdded == 1)      note("Added  %s", GetFileName(PL.paths[playlist_count(&PL) - 1]));
          else if (fwdAdded > 1)  note("Added %d files", fwdAdded);
          if (fwdAdded == 1 && playlist_count(&PL) == 1 && player_is_open(P)) autoqueue_siblings(playlist_current(&PL));
          if (singleinst_poll_focus()) os_focus_window(hwnd); }

        // ---- drag & drop ----
        if (IsFileDropped()) {
            FilePathList d = LoadDroppedFiles();
            bool was = open; int before = playlist_count(&PL);
            char subpath[1024] = "";
            for (unsigned i = 0; i < d.count; i++) {
                if (is_sub_ext(d.paths[i])) snprintf(subpath, sizeof subpath, "%s", d.paths[i]);  // load after video
                else playlist_add(&PL, d.paths[i]);
            }
            int added = playlist_count(&PL) - before;
            UnloadDroppedFiles(d);
            if (!was && added > 0) load_file(playlist_current(&PL));
            if (subpath[0] && player_is_open(P)) load_external_subs(subpath);
            if (added == 1)      note("Added  %s", GetFileName(PL.paths[playlist_count(&PL) - 1]));
            else if (added > 1)  note("Added %d files to playlist", added);
            if (!was && added == 1 && playlist_count(&PL) == 1 && player_is_open(P)) autoqueue_siblings(playlist_current(&PL));
            g_last_activity = now;
        }

        // ---- layout ----
        float TBH = 48, CBH = 130;
        Rectangle ctrlbar = { 0, (float)H - CBH, (float)W, CBH };
        float pad = 24;
        Rectangle seekR = { pad, ctrlbar.y + 30, W - 2 * pad, 6 };
        int midx = W / 2;
        float ty = ctrlbar.y + 90;
        Rectangle playR = { (float)midx - 27, ty - 27, 54, 54 };
        Rectangle prevR = { (float)midx - 96, ty - 20, 40, 40 };
        Rectangle nextR = { (float)midx + 56, ty - 20, 40, 40 };
        Rectangle muteR = { pad + 4, ty - 16, 32, 32 };
        Rectangle volR  = { pad + 48, ty - 3, 140, 7 };
        // right-side buttons
        float BS = 36, BGAP = 52, bx = W - pad - BS;
        Rectangle fullR = { bx, ty - BS / 2, BS, BS };   bx -= BGAP;
        Rectangle setR  = { bx, ty - BS / 2, BS, BS };   bx -= BGAP;
        Rectangle plR   = { bx, ty - BS / 2, BS, BS };   bx -= BGAP;
        Rectangle adjR  = { bx, ty - BS / 2, BS, BS };   bx -= BGAP;
        Rectangle audR  = { bx, ty - BS / 2, BS, BS };   bx -= BGAP;
        Rectangle ccR   = { bx, ty - BS / 2, BS, BS };
        // top bar buttons
        Rectangle openR  = { pad, 12, 28, 26 };
        Rectangle closeR = { W - 38, 12, 26, 26 };
        Rectangle maxR   = { W - 74, 12, 26, 26 };
        Rectangle minR   = { W - 110, 12, 26, 26 };
        Rectangle aotR   = { W - 146, 12, 26, 26 };

        // ---- activity / controls visibility ----
        bool moved = (fabsf(mp.x - lastMouse.x) + fabsf(mp.y - lastMouse.y)) > 1.5f;
        if (moved || GetMouseWheelMove() != 0 || GetKeyPressed() != 0 || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_last_activity = now;
        lastMouse = mp;
        bool force_show = !playing || g_panel != PANEL_NONE || !open || !player_has_video(P);
        bool show = force_show || (now - g_last_activity) < 2.6;
        g_ctrl = approach(g_ctrl, show ? 1.0f : 0.0f, (float)dt);
        bool ctl = g_ctrl > 0.35f;                  // controls accept input
        if (player_has_video(P) && playing && g_ctrl < 0.05f) HideCursor(); else ShowCursor();

        // ---- window resize (borderless edges) ----
        // With aero snap active the OS handles edge-resize + caption-drag natively
        // (the hit-tested regions never deliver clicks here), so skip the fallback.
        if (!os_snap_active() && !g_maximized && !g_fullscreen && !g_resizing && !g_moving) {
            const float G = 6;
            int edge = 0;
            if (mp.x <= G) edge |= 1;
            if (mp.x >= W - G) edge |= 2;
            if (mp.y <= G) edge |= 4;
            if (mp.y >= H - G) edge |= 8;
            if (edge) {
                int cur = MOUSE_CURSOR_DEFAULT;
                if ((edge & 5) == 5 || (edge & 10) == 10) cur = MOUSE_CURSOR_RESIZE_NWSE;
                else if ((edge & 6) == 6 || (edge & 9) == 9) cur = MOUSE_CURSOR_RESIZE_NESW;
                else if (edge & 3) cur = MOUSE_CURSOR_RESIZE_EW;
                else cur = MOUSE_CURSOR_RESIZE_NS;
                SetMouseCursor(cur);
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_menu_open) {
                    g_resizing = true; g_resize_edge = edge;
                    Vector2 wp = GetWindowPosition();
                    g_fixed_right = wp.x + W; g_fixed_bottom = wp.y + H;
                }
            } else SetMouseCursor(MOUSE_CURSOR_DEFAULT);
        }
        if (g_resizing) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                Vector2 wp = GetWindowPosition();
                float gx = wp.x + mp.x, gy = wp.y + mp.y;
                int nx = (int)wp.x, ny = (int)wp.y, nw = W, nh = H;
                if (g_resize_edge & 2) nw = (int)(mp.x);
                if (g_resize_edge & 8) nh = (int)(mp.y);
                if (g_resize_edge & 1) { nx = (int)gx; nw = (int)(g_fixed_right - gx); }
                if (g_resize_edge & 4) { ny = (int)gy; nh = (int)(g_fixed_bottom - gy); }
                if (nw < 480) { if (g_resize_edge & 1) nx -= (480 - nw); nw = 480; }
                if (nh < 320) { if (g_resize_edge & 4) ny -= (320 - nh); nh = 320; }
                SetWindowSize(nw, nh);
                if (g_resize_edge & 5) SetWindowPosition(nx, ny);
            } else { g_resizing = false; SetMouseCursor(MOUSE_CURSOR_DEFAULT); }
        }

        // ---- window move (drag empty top bar; fallback when no native snap) ----
        if (!os_snap_active() && !g_resizing && !g_maximized && !g_fullscreen && !g_menu_open && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)
            && mp.y < TBH && mp.x > openR.x + 60 && mp.x < aotR.x - 8) { g_moving = true; g_move_grab = mp; }
        if (g_moving) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) { Vector2 wp = GetWindowPosition(); SetWindowPosition((int)(wp.x + mp.x - g_move_grab.x), (int)(wp.y + mp.y - g_move_grab.y)); }
            else g_moving = false;
        }

        // ---- right-click context menu ----
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) { g_menu_open = true; g_menu_pos = mp; g_last_activity = now; }
        { static int shot_menu = -1;   // dev hook: force the menu open for a screenshot
          if (shot_menu < 0) shot_menu = getenv("TIVI_SHOT_MENU") ? 1 : 0;
          if (shot_menu && shot_frame > 0) { g_menu_open = true; g_menu_pos = (Vector2){ W * 0.36f, H * 0.34f }; } }
        // layout (shared by input below and drawing later this frame)
        const char *menuLbl[8]; bool menuDot[8]; int menuN = 0;
        Rectangle menuR = { 0 };
        float menuRowH = 36, menuPad = 8;
        if (g_menu_open) {
            menuLbl[menuN] = (open && playing) ? "Pause" : "Play"; menuDot[menuN++] = false;
            menuLbl[menuN] = "Open files\xE2\x80\xA6";             menuDot[menuN++] = false;
            menuLbl[menuN] = "Snapshot";                           menuDot[menuN++] = false;
            menuLbl[menuN] = g_fullscreen ? "Exit fullscreen" : "Fullscreen"; menuDot[menuN++] = g_fullscreen;
            menuLbl[menuN] = "Always on top";                      menuDot[menuN++] = g_aot;
            menuLbl[menuN] = "Playlist";                           menuDot[menuN++] = (g_panel == PANEL_PLAYLIST);
            menuLbl[menuN] = "Settings";                           menuDot[menuN++] = (g_panel == PANEL_SETTINGS);
            menuLbl[menuN] = "Exit tivi";                          menuDot[menuN++] = false;
            float mw = 0;
            for (int i = 0; i < menuN; i++) { float w2 = MeasureTextEx(fSmall, menuLbl[i], 19, 0.3f).x; if (w2 > mw) mw = w2; }
            mw += 64;
            float mh = menuN * menuRowH + menuPad * 2;
            menuR = (Rectangle){ fminf(g_menu_pos.x, W - mw - 8), fminf(g_menu_pos.y, H - mh - 8), mw, mh };
            if (menuR.x < 8) menuR.x = 8;
            if (menuR.y < 8) menuR.y = 8;
        }
        bool menuAte = false;   // a click that operated (or closed) the menu eats the event
        if (g_menu_open && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            menuAte = true; g_menu_open = false;
            if (CheckCollisionPointRec(mp, menuR)) {
                int mi = (int)((mp.y - menuR.y - menuPad) / menuRowH);
                if (mi >= 0 && mi < menuN) switch (mi) {
                    case 0: if (open) { player_toggle(P); g_center_flash = now; g_center_play = player_is_playing(P); } break;
                    case 1: open_dialog(); break;
                    case 2: snapshot(); break;
                    case 3: toggle_fullscreen(); break;
                    case 4: g_aot = !g_aot; if (g_aot) SetWindowState(FLAG_WINDOW_TOPMOST); else ClearWindowState(FLAG_WINDOW_TOPMOST); osd(g_aot ? "Always on top" : "Always on top off"); break;
                    case 5: g_panel = (g_panel == PANEL_PLAYLIST) ? PANEL_NONE : PANEL_PLAYLIST; break;
                    case 6: g_panel = (g_panel == PANEL_SETTINGS) ? PANEL_NONE : PANEL_SETTINGS; break;
                    case 7: g_quit = true; break;
                }
            }
        }

        // ---- click handling (controls) ----
        bool consumed = menuAte;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !consumed && !g_resizing && !g_moving) {
            // top bar
            if (CheckCollisionPointRec(mp, closeR)) break;
            else if (CheckCollisionPointRec(mp, minR)) { MinimizeWindow(); consumed = true; }
            else if (CheckCollisionPointRec(mp, maxR)) { toggle_maximize(); consumed = true; }
            else if (CheckCollisionPointRec(mp, aotR)) { g_aot = !g_aot; if (g_aot) SetWindowState(FLAG_WINDOW_TOPMOST); else ClearWindowState(FLAG_WINDOW_TOPMOST); osd(g_aot ? "Always on top" : "Always on top off"); consumed = true; }
            else if (CheckCollisionPointRec(mp, openR)) { open_dialog(); consumed = true; }
            else if (ctl) {
                if (CheckCollisionPointRec(mp, playR)) { if (open) player_toggle(P); consumed = true; }
                else if (CheckCollisionPointRec(mp, prevR)) { prev_track(); consumed = true; }
                else if (CheckCollisionPointRec(mp, nextR)) { next_track(); consumed = true; }
                else if (CheckCollisionPointRec(mp, muteR)) { toggle_mute(); consumed = true; }
                else if (CheckCollisionPointRec(mp, ccR))  { g_panel = (g_panel == PANEL_SUBS) ? PANEL_NONE : PANEL_SUBS; consumed = true; }
                else if (CheckCollisionPointRec(mp, audR)) { g_panel = (g_panel == PANEL_AUDIO) ? PANEL_NONE : PANEL_AUDIO; consumed = true; }
                else if (CheckCollisionPointRec(mp, adjR)) { g_panel = (g_panel == PANEL_ADJUST) ? PANEL_NONE : PANEL_ADJUST; consumed = true; }
                else if (CheckCollisionPointRec(mp, plR))  { g_panel = (g_panel == PANEL_PLAYLIST) ? PANEL_NONE : PANEL_PLAYLIST; consumed = true; }
                else if (CheckCollisionPointRec(mp, setR)) { g_panel = (g_panel == PANEL_SETTINGS) ? PANEL_NONE : PANEL_SETTINGS; consumed = true; }
                else if (CheckCollisionPointRec(mp, fullR)) { toggle_fullscreen(); consumed = true; }
                else if (open && CheckCollisionPointRec(mp, (Rectangle){ seekR.x - 4, seekR.y - 10, seekR.width + 8, 24 })) {
                    double dur = player_duration(P);
                    if (dur > 0) { double t = (mp.x - seekR.x) / seekR.width; player_seek(P, clampf((float)t, 0, 1) * dur); }
                    consumed = true;
                }
                else if (CheckCollisionPointRec(mp, (Rectangle){ volR.x - 4, volR.y - 10, volR.width + 8, 24 })) {
                    set_volume(2.0f * (mp.x - volR.x) / volR.width); consumed = true;   // slider spans 0..200%
                }
            }
        }

        // seek bar live drag. Re-seek only when the target actually moves — a
        // per-frame re-seek at one spot replays the same instant in a loop. While
        // the drag is moving, scrub silently (paused) and resume on release.
        static bool seek_drag = false, vol_drag = false;
        static bool seek_resume = false;        // was playing when the drag began
        static double seek_last = -1;
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !menuAte && ctl && open && CheckCollisionPointRec(mp, (Rectangle){ seekR.x - 4, seekR.y - 10, seekR.width + 8, 24 })) {
            seek_drag = true; seek_resume = player_is_playing(P);
            double dur = player_duration(P);
            seek_last = (dur > 0) ? clampf((mp.x - seekR.x) / seekR.width, 0, 1) * dur : -1;
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !menuAte && ctl && CheckCollisionPointRec(mp, (Rectangle){ volR.x - 4, volR.y - 10, volR.width + 8, 24 })) vol_drag = true;
        if (seek_drag) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                double dur = player_duration(P);
                double t = (dur > 0) ? clampf((mp.x - seekR.x) / seekR.width, 0, 1) * dur : -1;
                if (t >= 0 && t != seek_last) {
                    if (player_is_playing(P)) player_pause(P);
                    player_seek(P, t); seek_last = t;
                }
            } else {
                seek_drag = false;
                if (seek_resume && !player_is_playing(P)) player_play(P);
            }
        }
        if (vol_drag) { if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) g_volume = 2.0f * clampf((mp.x - volR.x) / volR.width, 0, 1), g_muted = false, player_set_volume(P, g_volume); else vol_drag = false; }

        // panel interactions (handled in draw section via helper) — compute panel rect
        Rectangle panelR = { 0 };
        if (g_panel != PANEL_NONE) {
            float pw = (float)(W < 860 ? W - 40 : 430);
            panelR = (Rectangle){ W - pw - 12, TBH + 10, pw, H - TBH - CBH - 24 };
        }

        // ---- click on video → play/pause; double-click → fullscreen ----
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !consumed && !g_resizing && !g_moving && !seek_drag && !vol_drag
            && mp.y > TBH && mp.y < H - (ctl ? CBH : 0)
            && !(g_panel != PANEL_NONE && CheckCollisionPointRec(mp, panelR))) {
            if (now - last_click_t < 0.32) { toggle_fullscreen(); last_click_t = -1; }
            else { last_click_t = now; if (open && g_click_pause) { player_toggle(P); g_center_flash = now; g_center_play = player_is_playing(P); } }
        }

        // wheel = volume
        float wheel = GetMouseWheelMove();
        if (wheel != 0) {
            if (g_panel == PANEL_PLAYLIST && CheckCollisionPointRec(mp, panelR)) g_pl_scroll -= (int)wheel;
            else if (g_panel == PANEL_SETTINGS && CheckCollisionPointRec(mp, panelR)) g_set_scroll -= wheel * 36;
            else set_volume(g_volume + wheel * 0.05f);
        }

        // ---- keyboard ----
        if (IsKeyPressed(KEY_SPACE) && open) { player_toggle(P); g_center_flash = now; g_center_play = player_is_playing(P); }
        if (open && IsKeyPressed(KEY_RIGHT)) { player_seek_relative(P, 5);  osd("» +5s"); }
        if (open && IsKeyPressed(KEY_LEFT))  { player_seek_relative(P, -5); osd("« -5s"); }
        if (open && IsKeyPressed(KEY_L))     { player_seek_relative(P, 10);  osd("» +10s"); }
        if (open && IsKeyPressed(KEY_J))     { player_seek_relative(P, -10); osd("« -10s"); }
        if (IsKeyPressed(KEY_UP))   set_volume(g_volume + 0.05f);
        if (IsKeyPressed(KEY_DOWN)) set_volume(g_volume - 0.05f);
        if (IsKeyPressed(KEY_M)) toggle_mute();
        if (IsKeyPressed(KEY_F)) toggle_fullscreen();
        if (IsKeyPressed(KEY_N)) next_track();
        if (IsKeyPressed(KEY_B)) prev_track();
        if (IsKeyPressed(KEY_S)) snapshot();
        if (IsKeyPressed(KEY_T)) { g_aot = !g_aot; if (g_aot) SetWindowState(FLAG_WINDOW_TOPMOST); else ClearWindowState(FLAG_WINDOW_TOPMOST); osd(g_aot ? "Always on top" : "Always on top off"); }
        if (IsKeyPressed(KEY_Q)) g_panel = (g_panel == PANEL_PLAYLIST) ? PANEL_NONE : PANEL_PLAYLIST;
        if (IsKeyPressed(KEY_A)) g_panel = (g_panel == PANEL_ADJUST) ? PANEL_NONE : PANEL_ADJUST;
        if (IsKeyPressed(KEY_G)) g_panel = (g_panel == PANEL_SETTINGS) ? PANEL_NONE : PANEL_SETTINGS;
        if (IsKeyPressed(KEY_LEFT_BRACKET)  || IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) { player_set_speed(P, player_speed(P) - 0.1); osd("Speed %.2fx", player_speed(P)); }
        if (IsKeyPressed(KEY_RIGHT_BRACKET) || IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD))      { player_set_speed(P, player_speed(P) + 0.1); osd("Speed %.2fx", player_speed(P)); }
        if (IsKeyPressed(KEY_BACKSLASH))     { player_set_speed(P, 1.0); osd("Speed 1.00x"); }
        if (IsKeyPressed(KEY_R)) { g_repeat = (g_repeat + 1) % 3; playlist_set_loop(&PL, g_repeat == 2); osd(g_repeat==0?"Repeat off":g_repeat==1?"Repeat one":"Repeat all"); }
        if (IsKeyPressed(KEY_C)) {  // cycle subtitle track
            int n = player_track_count(P), cur = player_current_sub(P), found = -1, first = -1;
            for (int i = 0; i < n; i++) { const TrackInfo *t = player_track(P, i); if (t->kind != TRACK_SUBTITLE) continue; if (first < 0) first = t->stream_index; if (t->stream_index == cur) { /* next */ for (int j = i + 1; j < n; j++) { if (player_track(P, j)->kind == TRACK_SUBTITLE) { found = player_track(P, j)->stream_index; break; } } break; } }
            if (cur < 0) { if (first >= 0) { g_subs_on = true; g_sub_source = SUB_EMBEDDED; g_sub_gen_seen = player_sub_generation(P) - 1; player_set_sub_track(P, first); osd("Subtitles on"); } else osd("No subtitles"); }
            else if (found >= 0) { g_sub_source = SUB_EMBEDDED; g_sub_gen_seen = player_sub_generation(P) - 1; player_set_sub_track(P, found); osd("Subtitle track changed"); }
            else { g_subs_on = false; g_sub_source = SUB_NONE; subs_clear(SUB); player_set_sub_track(P, -1); osd("Subtitles off"); }
        }
        if (IsKeyPressed(KEY_X)) {  // cycle audio track
            int n = player_track_count(P), cur = player_current_audio(P), found = -1, first = -1;
            for (int i = 0; i < n; i++) { const TrackInfo *t = player_track(P, i); if (t->kind != TRACK_AUDIO) continue; if (first < 0) first = t->stream_index; if (t->stream_index == cur) { for (int j = i + 1; j < n; j++) { if (player_track(P, j)->kind == TRACK_AUDIO) { found = player_track(P, j)->stream_index; break; } } break; } }
            int pick = (found >= 0) ? found : first;
            if (pick >= 0 && pick != cur) { player_set_audio_track(P, pick); osd("Audio track changed"); }
        }
        if (IsKeyPressed(KEY_O)) open_dialog();
        if (IsKeyPressed(KEY_ESCAPE)) { if (g_menu_open) g_menu_open = false; else if (g_panel != PANEL_NONE) g_panel = PANEL_NONE; else if (g_fullscreen) toggle_fullscreen(); }

        // ---- subtitle pipeline ----
        if (open && g_sub_source == SUB_EMBEDDED) {
            unsigned gen = player_sub_generation(P);
            if (gen != g_sub_gen_seen) {
                g_sub_gen_seen = gen;
                if (!player_sub_is_bitmap(P) && player_current_sub(P) >= 0) {
                    int hs; const uint8_t *hd = player_sub_header(P, &hs);
                    subs_set_size(SUB, player_video_width(P), player_video_height(P));
                    subs_begin_embedded(SUB, hd, hs);
                    subs_set_plaintext(SUB, !cur_sub_is_ass());
                    // demux the whole track in the background — once it swaps in,
                    // seeking can never land on a missing line
                    subs_preload_start(SUB, player_path(P), player_current_sub(P));
                } else subs_clear(SUB);
            }
            if (!player_sub_is_bitmap(P)) {
                subs_preload_update(SUB);
                long long s, d; char *a;
                while (player_pop_sub_text(P, &s, &d, &a)) {
                    if (!subs_is_preloaded(SUB)) subs_feed(SUB, a, s, d);  // live feed until preload lands
                    free(a);
                }
            }
        }

        // ====== DRAW (into a supersampled target → smooth AA on everything) ======
        int tw = W * SS, th = H * SS;
        if (!g_resizing && (target.texture.width != tw || target.texture.height != th)) {
            UnloadRenderTexture(target);
            target = LoadRenderTexture(tw, th);
            SetTextureFilter(target.texture, TEXTURE_FILTER_BILINEAR);
        }
        BeginTextureMode(target);
        ClearBackground((open && player_has_video(P) && g_letterbox_black) ? BLACK : BG1);
        rlPushMatrix();
        rlScalef((float)SS, (float)SS, 1.0f);

        // video rect (letterboxed)
        Rectangle vr = { 0, 0, 0, 0 };
        if (open && player_has_video(P)) {
            TiviFrame frm; bool changed;
            bool planar = false;
            if (player_frame(P, &frm, &changed)) {
                if (changed) g_perf_vidupd++;
                // adaptive theme: average a sparse grid (~200 samples, 2×/s — negligible)
                if (g_theme == NTHEMES && now - g_ad_last > 0.5 && frm.w > 32 && frm.h > 32) {
                    g_ad_last = now;
                    int sx = frm.w / 16, sy = frm.h / 12;
                    float ar = 0, ag = 0, ab = 0; int n = 0;
                    if (frm.fmt == TIVI_PIX_RGBA) {
                        for (int yy = sy / 2; yy < frm.h; yy += sy) {
                            const uint8_t *row = frm.plane[0] + (size_t)yy * frm.stride[0];
                            for (int xx = sx / 2; xx < frm.w; xx += sx) { const uint8_t *p = row + (size_t)xx * 4; ar += p[0]; ag += p[1]; ab += p[2]; n++; }
                        }
                        if (n) adaptive_targets(ar / n / 255.0f, ag / n / 255.0f, ab / n / 255.0f);
                    } else {
                        float ay = 0, au = 0, av = 0;
                        bool p10 = (frm.fmt == TIVI_PIX_P010);
                        for (int yy = sy / 2; yy < frm.h; yy += sy) {
                            const uint8_t *yrow = frm.plane[0] + (size_t)yy * frm.stride[0];
                            const uint8_t *crow = frm.plane[1] + (size_t)(yy / 2) * frm.stride[1];
                            for (int xx = sx / 2; xx < frm.w; xx += sx) {
                                if (p10) { ay += ((const uint16_t *)yrow)[xx] / 65535.0f;
                                           au += ((const uint16_t *)crow)[(xx / 2) * 2] / 65535.0f;
                                           av += ((const uint16_t *)crow)[(xx / 2) * 2 + 1] / 65535.0f; }
                                else     { ay += yrow[xx] / 255.0f;
                                           au += crow[(xx / 2) * 2] / 255.0f;
                                           av += crow[(xx / 2) * 2 + 1] / 255.0f; }
                                n++;
                            }
                        }
                        if (n) {   // same affine transform the shader uses
                            YuvXfm x = yuvtex_transform(&frm);
                            float y = ay / n, u = au / n, v = av / n;
                            adaptive_targets(clampf(x.c0.x * y + x.c1.x * u + x.c2.x * v + x.off.x, 0, 1),
                                             clampf(x.c0.y * y + x.c1.y * u + x.c2.y * v + x.off.y, 0, 1),
                                             clampf(x.c0.z * y + x.c1.z * u + x.c2.z * v + x.off.z, 0, 1));
                        }
                    }
                }
                if (frm.fmt == TIVI_PIX_RGBA) {
                    ensure_vtex(frm.w, frm.h);
                    if (changed) UpdateTexture(vtex, frm.plane[0]);
                } else {
                    // GPU color-conversion path: (re)upload the Y/UV planes on a new
                    // frame or geometry change, and refresh the conversion transform.
                    if (changed || !g_yuv.valid || g_yuv.w != frm.w || g_yuv.h != frm.h || g_yuv.fmt != frm.fmt) {
                        if (yuvtex_update(&g_yuv, &frm)) g_yuv_xfm = yuvtex_transform(&frm);
                    }
                    planar = g_yuv.valid;
                }
            }
            float vw = (float)player_video_width(P), vh = (float)player_video_height(P);
            float scale = fminf(W / vw, H / vh);
            float dw = vw * scale, dh = vh * scale;
            vr = (Rectangle){ (W - dw) / 2, (H - dh) / 2, dw, dh };
            if (planar) {
                apply_adjustments_uniforms();
                int conv = 1;
                SetShaderValue(adjShader, locConvert, &conv, SHADER_UNIFORM_INT);
                SetShaderValue(adjShader, locC0, &g_yuv_xfm.c0, SHADER_UNIFORM_VEC3);
                SetShaderValue(adjShader, locC1, &g_yuv_xfm.c1, SHADER_UNIFORM_VEC3);
                SetShaderValue(adjShader, locC2, &g_yuv_xfm.c2, SHADER_UNIFORM_VEC3);
                SetShaderValue(adjShader, locYoff, &g_yuv_xfm.off, SHADER_UNIFORM_VEC3);
                BeginShaderMode(adjShader);
                // Must be inside Begin/EndShaderMode: the sampler registers in the
                // render batch's texture slots, and any batch flush (e.g. the shader
                // switch in BeginShaderMode) resets them — binding before the switch
                // leaves the UV unit empty and the picture green.
                SetShaderValueTexture(adjShader, locTexUV, g_yuv.uv);
                DrawTexturePro(g_yuv.y, (Rectangle){ 0, 0, (float)g_yuv.w, (float)g_yuv.h }, vr, (Vector2){ 0, 0 }, 0, WHITE);
                EndShaderMode();
            } else if (vtex_valid) {
                apply_adjustments_uniforms();
                int conv = 0;
                SetShaderValue(adjShader, locConvert, &conv, SHADER_UNIFORM_INT);
                BeginShaderMode(adjShader);
                DrawTexturePro(vtex, (Rectangle){ 0, 0, (float)vtex_w, (float)vtex_h }, vr, (Vector2){ 0, 0 }, 0, WHITE);
                EndShaderMode();
            }
            // subtitles over the video rect. When the control bar is up, draw the
            // overlay into a rect lifted above it so subs clear the buttons (VLC-style).
            if (g_subs_on) {
                Rectangle svr = vr;
                if (ctl) {
                    float overlap = (vr.y + vr.height) - (H - CBH);
                    if (overlap > 0) svr.height = vr.height - (overlap + 14);
                    if (svr.height < vr.height * 0.5f) svr.height = vr.height * 0.5f;
                }
                if (g_sub_source == SUB_EMBEDDED && player_sub_is_bitmap(P)) {
                    uint8_t *sb; int sw, sh;
                    if (player_active_sub_bitmap(P, &sb, &sw, &sh)) { ensure_stex(sw, sh); UpdateTexture(stex, sb);
                        DrawTexturePro(stex, (Rectangle){ 0, 0, (float)sw, (float)sh }, svr, (Vector2){ 0, 0 }, 0, WHITE); }
                } else if (g_sub_source != SUB_NONE) {
                    // Render the libass overlay at the on-screen video size (not the
                    // video's native size) so glyphs rasterize at display resolution —
                    // crisp AA like VLC instead of a stretched low-res bitmap.
                    subs_set_size(SUB, (int)vr.width, (int)vr.height);
                    uint8_t *sr; int sw, sh; bool sch;
                    if (subs_render(SUB, (long long)(player_position(P) * 1000.0), &sr, &sw, &sh, &sch)) {
                        ensure_stex(sw, sh); if (sch) UpdateTexture(stex, sr);
                        DrawTexturePro(stex, (Rectangle){ 0, 0, (float)sw, (float)sh }, svr, (Vector2){ 0, 0 }, 0, WHITE);
                    }
                }
            }
        } else {
            // placeholder
            DrawRectangleGradientV(0, 0, W, H, BG0, BG1);
            DrawRectangleRounded((Rectangle){ W / 2.0f - 56, H / 2.0f - 96, 112, 112 }, 0.3f, 48, ACCENT);
            DrawTriangle((Vector2){ W / 2.0f - 16, H / 2.0f - 66 }, (Vector2){ W / 2.0f - 16, H / 2.0f - 8 }, (Vector2){ W / 2.0f + 30, H / 2.0f - 37 }, BG1);
            const char *t1 = (open && !player_has_video(P)) ? player_title(P)
                           : (open ? "Loading…" : "Drop a video to begin");
            Vector2 m1 = MeasureTextEx(fBig, t1, 48, 0.3f);
            draw_fit(fBig, t1, (Vector2){ (W - fminf(m1.x, W - 80)) / 2, H / 2.0f + 48 }, 48, 0.3f, TXT, W - 80);
            const char *t2 = (open && !player_has_video(P)) ? "Audio — no video stream" : "or press O / the + button to open files";
            Vector2 m2 = MeasureTextEx(fSmall, t2, 21, 0.3f);
            DrawTextEx(fSmall, t2, (Vector2){ (W - m2.x) / 2, H / 2.0f + 112 }, 21, 0.3f, MUT);
        }

        // center play/pause flash
        if (now - g_center_flash < 0.5) {
            float k = 1.0f - (float)((now - g_center_flash) / 0.5);
            Color cc = alpha(BG1, (unsigned char)(150 * k));
            scircle(W / 2.0f, H / 2.0f, 52, cc);
            if (g_center_play) ic_play(W / 2.0f + 4, H / 2.0f, 22, afade(TXT, k)); else ic_pause(W / 2.0f, H / 2.0f, 20, afade(TXT, k));
        }

        // ---- top bar overlay ----
        float ca = g_ctrl;
        if (ca > 0.01f) {
            DrawRectangleGradientV(0, 0, W, (int)TBH + 30, afade(BG1, 0.80f * ca), afade(BG1, 0.0f));
            // wordmark + filename
            DrawTextEx(fUI, "tivi", (Vector2){ openR.x + 40, 11 }, 26, 2.0f, afade(ACCENT, ca));
            if (open) draw_fit(fSmall, player_title(P), (Vector2){ openR.x + 112, 15 }, 20, 0.2f, afade(MUT, ca), W * 0.5f);
            // open +
            Color co = afade(CheckCollisionPointRec(mp, openR) ? TXT : MUT, ca);
            float ox = openR.x + 13, oy = openR.y + 13;
            DrawRing((Vector2){ ox, oy }, 9.5f, 11.5f, 0, 360, 32, co);
            DrawLineEx((Vector2){ ox, oy - 5.5f }, (Vector2){ ox, oy + 5.5f }, IT, co);
            DrawLineEx((Vector2){ ox - 5.5f, oy }, (Vector2){ ox + 5.5f, oy }, IT, co);
            // always-on-top pin
            Color ca1 = afade(g_aot ? ACCENT : (CheckCollisionPointRec(mp, aotR) ? TXT : MUT), ca);
            float ax = aotR.x + 13, ay = aotR.y + 13;
            DrawLineEx((Vector2){ ax - 8, ay + 4 }, (Vector2){ ax + 8, ay + 4 }, IT, ca1);
            DrawLineEx((Vector2){ ax, ay - 6 }, (Vector2){ ax, ay + 4 }, IT, ca1);
            DrawCircleV((Vector2){ ax, ay - 6 }, 3.0f, ca1);
            // minimize
            Color cm = afade(CheckCollisionPointRec(mp, minR) ? TXT : MUT, ca);
            DrawLineEx((Vector2){ minR.x + 6, minR.y + 15 }, (Vector2){ minR.x + 20, minR.y + 15 }, IT, cm);
            // maximize
            Color cx = afade(CheckCollisionPointRec(mp, maxR) ? TXT : MUT, ca);
            DrawRectangleLinesEx((Rectangle){ maxR.x + 6, maxR.y + 6, 14, 14 }, 1.6f, cx);
            // close
            Color cc2 = afade(CheckCollisionPointRec(mp, closeR) ? (Color){ 235, 110, 90, 255 } : MUT, ca);
            DrawLineEx((Vector2){ closeR.x + 6, closeR.y + 6 }, (Vector2){ closeR.x + 20, closeR.y + 20 }, 1.9f, cc2);
            DrawLineEx((Vector2){ closeR.x + 20, closeR.y + 6 }, (Vector2){ closeR.x + 6, closeR.y + 20 }, 1.9f, cc2);
        }

        // ---- bottom control bar overlay ----
        if (ca > 0.01f) {
            DrawRectangleGradientV(0, (int)ctrlbar.y - 24, W, (int)CBH + 24, afade(BG1, 0.0f), afade(BG1, 0.86f * ca));
            // seek bar
            double dur = player_duration(P), pos = player_position(P);
            float t = (dur > 0) ? (float)(pos / dur) : 0; t = clampf(t, 0, 1);
            DrawRectangleRounded(seekR, 1, 6, afade(TRK, ca));
            DrawRectangleRounded((Rectangle){ seekR.x, seekR.y, seekR.width * t, seekR.height }, 1, 6, afade(ACCENT, ca));
            bool seekHover = CheckCollisionPointRec(mp, (Rectangle){ seekR.x - 4, seekR.y - 12, seekR.width + 8, 28 });
            scircle(seekR.x + seekR.width * t, seekR.y + 3, (seekHover || seek_drag) ? 9 : 6, afade(TXT, ca));
            char tl[24], tr[24]; fmt_time(pos, tl, sizeof tl); fmt_time(dur, tr, sizeof tr);
            char tb[52]; snprintf(tb, sizeof tb, "%s / %s", tl, tr);
            DrawTextEx(fSmall, tb, (Vector2){ pad, seekR.y + 11 }, 19, 0.3f, afade(MUT, ca));
            if (player_speed(P) != 1.0) { char sp[16]; snprintf(sp, sizeof sp, "%.2fx", player_speed(P)); Vector2 mw = MeasureTextEx(fSmall, sp, 19, 0.3f); DrawTextEx(fSmall, sp, (Vector2){ (W - mw.x) / 2, seekR.y + 11 }, 19, 0.3f, afade(ACCENT, ca)); }

            // transport
            scircle((float)midx, ty, 27, afade(ACCENT, ca));
            if (playing) ic_pause(midx, ty, 12, afade(BG1, ca)); else ic_play(midx + 2.0f, ty, 14, afade(BG1, ca));
            ic_prev(prevR.x + 20, prevR.y + 20, 12, afade(CheckCollisionPointRec(mp, prevR) ? TXT : alpha(TXT, 220), ca));
            ic_next(nextR.x + 20, nextR.y + 20, 12, afade(CheckCollisionPointRec(mp, nextR) ? TXT : alpha(TXT, 220), ca));
            // volume
            float vfrac = clampf((g_muted ? 0 : g_volume) / 2.0f, 0, 1);   // slider spans 0..200%
            if (g_muted) ic_speaker_mute(muteR.x + 16, muteR.y + 16, 11, afade(MUT, ca)); else ic_audio(muteR.x + 16, muteR.y + 16, 11, afade(CheckCollisionPointRec(mp, muteR) ? TXT : MUT, ca));
            DrawRectangleRounded(volR, 1, 6, afade(TRK, ca));
            DrawRectangleRec((Rectangle){ volR.x + volR.width * 0.5f - 1, volR.y - 3, 2, 13 }, afade(alpha(MUT, 130), ca));   // 100% notch
            DrawRectangleRounded((Rectangle){ volR.x, volR.y, volR.width * fminf(vfrac, 0.5f), volR.height }, 1, 6, afade((Color){ 190, 178, 150, 255 }, ca));
            if (vfrac > 0.5f)   // boosted range past 100% runs hot
                DrawRectangleRounded((Rectangle){ volR.x + volR.width * 0.5f, volR.y, volR.width * (vfrac - 0.5f), volR.height }, 1, 6, afade((Color){ 226, 138, 88, 255 }, ca));
            scircle(volR.x + volR.width * vfrac, volR.y + 3.5f, 7, afade(TXT, ca));
            // right buttons
            ic_cc(ccR.x + 18, ccR.y + 18, 13, afade((g_subs_on && g_sub_source != SUB_NONE) ? ACCENT : (CheckCollisionPointRec(mp, ccR) ? TXT : MUT), ca));
            ic_notes(audR.x + 18, audR.y + 18, 12, afade(g_panel == PANEL_AUDIO ? ACCENT : (CheckCollisionPointRec(mp, audR) ? TXT : MUT), ca));
            ic_adjust(adjR.x + 18, adjR.y + 18, 12, afade(g_panel == PANEL_ADJUST ? ACCENT : (CheckCollisionPointRec(mp, adjR) ? TXT : MUT), ca));
            ic_list(plR.x + 18, plR.y + 18, 12, afade(g_panel == PANEL_PLAYLIST ? ACCENT : (CheckCollisionPointRec(mp, plR) ? TXT : MUT), ca));
            ic_gear(setR.x + 18, setR.y + 18, 12, afade(g_panel == PANEL_SETTINGS ? ACCENT : (CheckCollisionPointRec(mp, setR) ? TXT : MUT), ca));
            ic_full(fullR.x + 18, fullR.y + 18, 11, afade(CheckCollisionPointRec(mp, fullR) ? TXT : MUT, ca));
        }

        // ---- panel ----
        if (g_panel != PANEL_NONE) {
            DrawRectangleRounded(panelR, 0.04f, 12, alpha(BG0, 244));
            DrawRectangleRoundedLines(panelR, 0.04f, 12, alpha(WHITE, 18));
            float px = panelR.x + 18, py = panelR.y + 16, iw = panelR.width - 36;
            const char *titles[] = { "", "PLAYLIST", "AUDIO TRACKS", "SUBTITLES", "ADJUSTMENTS", "SETTINGS" };
            DrawTextEx(fSmall, titles[g_panel], (Vector2){ px, py }, 17, 3.0f, ACCENT);
            py += 42;
            float rowH = 48;

            if (g_panel == PANEL_PLAYLIST) {
                int n = playlist_count(&PL), cur = playlist_index(&PL);
                if (n == 0) DrawTextEx(fSmall, "Queue is empty — drop files in", (Vector2){ px, py + 8 }, 20, 0.2f, MUT);
                float listTop = py;
                int maxRows = (int)((panelR.y + panelR.height - 12 - listTop) / rowH);
                if (maxRows < 1) maxRows = 1;
                int maxScroll = n - maxRows; if (maxScroll < 0) maxScroll = 0;
                if (g_pl_scroll > maxScroll) g_pl_scroll = maxScroll;
                if (g_pl_scroll < 0) g_pl_scroll = 0;
                for (int r = 0; r < maxRows; r++) {
                    int i = g_pl_scroll + r;
                    if (i >= n) break;
                    float ry = listTop + r * rowH;
                    Rectangle row = { px - 6, ry, iw + 12, rowH - 4 };
                    bool hov = CheckCollisionPointRec(mp, row);
                    bool isDrag = (i == g_q_drag);
                    if (i == cur)     DrawRectangleRounded(row, 0.4f, 6, alpha(ACCENT, 34));
                    else if (isDrag)  DrawRectangleRounded(row, 0.4f, 6, alpha(WHITE, 28));
                    else if (hov)     DrawRectangleRounded(row, 0.4f, 6, alpha(WHITE, 12));
                    char num[16]; snprintf(num, sizeof num, "%d", i + 1);
                    DrawTextEx(fSmall, num, (Vector2){ px, ry + 14 }, 18, 0.5f, i == cur ? ACCENT : alpha(MUT, 150));
                    const char *bn = GetFileName(PL.paths[i]);
                    draw_fit(fSmall, bn, (Vector2){ px + 34, ry + 13 }, 20, 0.2f, i == cur ? TXT : alpha(TXT, 205), iw - 76);
                    // grip dots + remove × on hover
                    Rectangle xR = { px + iw - 26, ry + (rowH - 4) / 2 - 12, 24, 24 };
                    if (hov || isDrag) {
                        for (int a = 0; a < 2; a++) for (int b = 0; b < 3; b++)
                            DrawCircleV((Vector2){ px + iw - 44 + a * 5, ry + (rowH - 4) / 2 - 6 + b * 6 }, 1.5f, alpha(MUT, 150));
                        bool xh = CheckCollisionPointRec(mp, xR);
                        Color xc = xh ? (Color){ 235, 110, 90, 255 } : alpha(TXT, 200);
                        Vector2 xc0 = { xR.x + 12, xR.y + 12 };
                        DrawLineEx((Vector2){ xc0.x - 6, xc0.y - 6 }, (Vector2){ xc0.x + 6, xc0.y + 6 }, 2.0f, xc);
                        DrawLineEx((Vector2){ xc0.x + 6, xc0.y - 6 }, (Vector2){ xc0.x - 6, xc0.y + 6 }, 2.0f, xc);
                    }
                    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        if (CheckCollisionPointRec(mp, xR)) {
                            bool removedCur = playlist_remove(&PL, i);
                            if (removedCur) { const char *p2 = playlist_current(&PL); if (p2) load_file(p2); else player_close(P); }
                        } else { g_q_press = i; g_q_press_y = mp.y; g_q_press_t = now; }
                        consumed = true;
                    }
                }
                // drag-to-reorder
                if (g_q_press >= 0 && g_q_drag < 0 && fabsf(mp.y - g_q_press_y) > 6) g_q_drag = g_q_press;
                if (g_q_drag >= 0) {
                    int t = g_pl_scroll + (int)((mp.y - listTop) / rowH);
                    if (t < 0) t = 0;
                    if (t >= n) t = n - 1;
                    g_q_target = t;
                    if (t >= g_pl_scroll && t < g_pl_scroll + maxRows) {
                        float ly = listTop + (t - g_pl_scroll) * rowH;
                        DrawRectangleRounded((Rectangle){ px - 6, ly - 2, iw + 12, 3 }, 1, 4, ACCENT);
                    }
                }
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    if (g_q_drag >= 0 && g_q_target >= 0 && g_q_target != g_q_drag) playlist_move(&PL, g_q_drag, g_q_target);
                    else if (g_q_press >= 0 && g_q_drag < 0 && (now - g_q_press_t) < 0.4) play_index(g_q_press);
                    g_q_press = g_q_drag = g_q_target = -1;
                }
                // scrollbar
                if (n > maxRows) {
                    float th = maxRows * rowH;
                    DrawRectangleRounded((Rectangle){ panelR.x + panelR.width - 5, listTop + th * g_pl_scroll / n, 3, th * maxRows / n }, 1, 4, alpha(ACCENT, 130));
                }
            } else if (g_panel == PANEL_AUDIO || g_panel == PANEL_SUBS) {
                int kind = (g_panel == PANEL_AUDIO) ? TRACK_AUDIO : TRACK_SUBTITLE;
                if (g_panel == PANEL_SUBS) {
                    bool offsel = (!g_subs_on || g_sub_source == SUB_NONE);
                    Rectangle row = { px - 6, py, iw + 12, rowH - 4 };
                    bool hov = CheckCollisionPointRec(mp, row);
                    if (offsel) DrawRectangleRounded(row, 0.4f, 6, alpha(ACCENT, 34)); else if (hov) DrawRectangleRounded(row, 0.4f, 6, alpha(WHITE, 12));
                    DrawTextEx(fSmall, "Disable subtitles", (Vector2){ px, py + 13 }, 20, 0.2f, offsel ? ACCENT : TXT);
                    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { g_subs_on = false; g_sub_source = SUB_NONE; subs_clear(SUB); player_set_sub_track(P, -1); consumed = true; }
                    py += rowH;
                    Rectangle row2 = { px - 6, py, iw + 12, rowH - 4 };
                    bool hov2 = CheckCollisionPointRec(mp, row2);
                    if (hov2) DrawRectangleRounded(row2, 0.4f, 6, alpha(WHITE, 12));
                    DrawTextEx(fSmall, "Load subtitle file…", (Vector2){ px, py + 13 }, 20, 0.2f, TXT);
                    if (hov2 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { char sp[1024]; if (os_open_subtitle_file(sp, sizeof sp)) load_external_subs(sp); consumed = true; }
                    py += rowH;
                    // active external subtitle (if any)
                    if (g_sub_source == SUB_EXTERNAL) {
                        bool xsel = g_subs_on;
                        Rectangle row3 = { px - 6, py, iw + 12, rowH - 4 };
                        bool hov3 = CheckCollisionPointRec(mp, row3);
                        if (xsel) DrawRectangleRounded(row3, 0.4f, 6, alpha(ACCENT, 34)); else if (hov3) DrawRectangleRounded(row3, 0.4f, 6, alpha(WHITE, 12));
                        char lab[300]; snprintf(lab, sizeof lab, "External:  %s", g_ext_sub_name);
                        draw_fit(fSmall, lab, (Vector2){ px, py + 13 }, 20, 0.2f, xsel ? ACCENT : alpha(TXT, 220), iw);
                        if (hov3 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { g_subs_on = true; consumed = true; }
                        py += rowH;
                    }
                    py += 6;
                }
                int n = player_track_count(P);
                int cur = (kind == TRACK_AUDIO) ? player_current_audio(P) : player_current_sub(P);
                bool any = false;
                for (int i = 0; i < n && py < panelR.y + panelR.height - rowH; i++) {
                    const TrackInfo *tr = player_track(P, i);
                    if (tr->kind != kind) continue;
                    any = true;
                    Rectangle row = { px - 6, py, iw + 12, rowH - 4 };
                    bool hov = CheckCollisionPointRec(mp, row);
                    bool sel = (tr->stream_index == cur) && (kind == TRACK_AUDIO || (g_subs_on && g_sub_source == SUB_EMBEDDED));
                    if (sel) DrawRectangleRounded(row, 0.4f, 6, alpha(ACCENT, 34)); else if (hov) DrawRectangleRounded(row, 0.4f, 6, alpha(WHITE, 12));
                    draw_fit(fSmall, tr->title, (Vector2){ px, py + 13 }, 20, 0.2f, sel ? ACCENT : alpha(TXT, 220), iw);
                    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        if (kind == TRACK_AUDIO) { player_set_audio_track(P, tr->stream_index); }
                        else { g_subs_on = true; g_sub_source = SUB_EMBEDDED; g_sub_gen_seen = player_sub_generation(P) - 1; player_set_sub_track(P, tr->stream_index); }
                        consumed = true;
                    }
                    py += rowH;
                }
                if (!any) DrawTextEx(fSmall, kind == TRACK_AUDIO ? "No audio tracks" : "No embedded subtitles", (Vector2){ px, py + 8 }, 20, 0.2f, MUT);
            } else if (g_panel == PANEL_ADJUST) {
                struct { const char *name; float *v; float lo, hi, def; } sl[] = {
                    { "Brightness", &CFG.brightness, -1, 1, 0 },
                    { "Contrast",   &CFG.contrast,    0, 2, 1 },
                    { "Saturation", &CFG.saturation,  0, 2, 1 },
                    { "Hue",        &CFG.hue,      -180, 180, 0 },
                    { "Gamma",      &CFG.gamma,     0.2f, 3, 1 },
                };
                for (int i = 0; i < 5; i++) {
                    float yy = py + i * 60;
                    DrawTextEx(fSmall, sl[i].name, (Vector2){ px, yy }, 19, 0.3f, TXT);
                    char val[16]; snprintf(val, sizeof val, "%.2f", *sl[i].v);
                    Vector2 mw = MeasureTextEx(fSmall, val, 19, 0.3f);
                    DrawTextEx(fSmall, val, (Vector2){ panelR.x + panelR.width - 18 - mw.x, yy }, 19, 0.3f, MUT);
                    Rectangle tr = { px, yy + 28, iw, 6 };
                    DrawRectangleRounded(tr, 1, 6, TRK);
                    float frac = (*sl[i].v - sl[i].lo) / (sl[i].hi - sl[i].lo);
                    DrawRectangleRounded((Rectangle){ tr.x, tr.y, tr.width * frac, tr.height }, 1, 6, ACCENT);
                    float hx = tr.x + tr.width * frac;
                    scircle(hx, tr.y + 3, 8, TXT);
                    Rectangle hit = { tr.x - 4, tr.y - 11, tr.width + 8, 28 };
                    if (CheckCollisionPointRec(mp, hit) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) { *sl[i].v = sl[i].lo + clampf((mp.x - tr.x) / tr.width, 0, 1) * (sl[i].hi - sl[i].lo); consumed = true; }
                }
                float ry = py + 5 * 60 + 8;
                Rectangle resetR = { px, ry, 104, 34 };
                bool rh = CheckCollisionPointRec(mp, resetR);
                DrawRectangleRoundedLines(resetR, 0.4f, 8, rh ? ACCENT : alpha(MUT, 150));
                DrawTextEx(fSmall, "Reset", (Vector2){ resetR.x + 28, resetR.y + 8 }, 18, 0.3f, rh ? ACCENT : MUT);
                if (rh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { CFG.brightness = 0; CFG.contrast = 1; CFG.saturation = 1; CFG.hue = 0; CFG.gamma = 1; consumed = true; }
            } else if (g_panel == PANEL_SETTINGS) {
                // Scrollable: small windows can't fit all settings + about.
                float viewTop = py - 6;
                float viewH = panelR.y + panelR.height - 10 - viewTop;
                Rectangle viewR = { panelR.x, viewTop, panelR.width, viewH };
                bool inview = CheckCollisionPointRec(mp, viewR);
                static float set_content_h = 999;   // measured last frame
                float maxScroll = set_content_h - viewH; if (maxScroll < 0) maxScroll = 0;
                if (g_set_scroll > maxScroll) g_set_scroll = maxScroll;
                if (g_set_scroll < 0) g_set_scroll = 0;
                BeginScissorMode((int)(panelR.x * SS), (int)(viewTop * SS), (int)(panelR.width * SS), (int)(viewH * SS));
                float yy = viewTop + 6 - g_set_scroll;

                // toggles
                const char *labels[5] = { "Always on top", "Black letterbox", "Shuffle", "Click video to pause", "Auto-queue folder" };
                bool st[5] = { g_aot, g_letterbox_black, playlist_shuffle(&PL), g_click_pause, CFG.auto_queue };
                for (int i = 0; i < 5; i++) {
                    Rectangle row = { px - 6, yy - 6, iw + 12, 46 };
                    bool hov = inview && CheckCollisionPointRec(mp, row);
                    if (hov) DrawRectangleRounded(row, 0.3f, 6, alpha(WHITE, 10));
                    DrawTextEx(fSmall, labels[i], (Vector2){ px, yy + 6 }, 20, 0.3f, TXT);
                    float tx = panelR.x + panelR.width - 76;
                    DrawRectangleRounded((Rectangle){ tx, yy, 54, 28 }, 1, 8, st[i] ? ACCENT : TRK);
                    scircle(st[i] ? tx + 39 : tx + 15, yy + 14, 11, st[i] ? BG1 : alpha(TXT, 210));
                    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        if (i == 0) { g_aot = !g_aot; if (g_aot) SetWindowState(FLAG_WINDOW_TOPMOST); else ClearWindowState(FLAG_WINDOW_TOPMOST); }
                        else if (i == 1) g_letterbox_black = !g_letterbox_black;
                        else if (i == 2) playlist_set_shuffle(&PL, !playlist_shuffle(&PL));
                        else if (i == 3) g_click_pause = !g_click_pause;
                        else { CFG.auto_queue = !CFG.auto_queue; osd("Auto-queue folder %s", CFG.auto_queue ? "on" : "off"); }
                        consumed = true;
                    }
                    yy += 50;
                }

                // repeat — segmented Off / One / All
                DrawTextEx(fSmall, "Repeat", (Vector2){ px, yy + 6 }, 20, 0.3f, TXT);
                {
                    const char *rm[3] = { "Off", "One", "All" };
                    float segW = 46, segH = 28;
                    float tx = panelR.x + panelR.width - 22 - (segW * 3 + 8);
                    for (int k = 0; k < 3; k++) {
                        Rectangle seg = { tx + k * (segW + 4), yy, segW, segH };
                        bool selSeg = (g_repeat == k);
                        bool hs = inview && CheckCollisionPointRec(mp, seg);
                        DrawRectangleRounded(seg, 0.5f, 8, selSeg ? alpha(ACCENT, 70) : (hs ? alpha(WHITE, 18) : TRK));
                        Vector2 mw = MeasureTextEx(fSmall, rm[k], 16, 0.3f);
                        DrawTextEx(fSmall, rm[k], (Vector2){ seg.x + (segW - mw.x) / 2, seg.y + 5 }, 16, 0.3f, selSeg ? ACCENT : MUT);
                        if (hs && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { g_repeat = k; playlist_set_loop(&PL, g_repeat == 2); consumed = true; }
                    }
                    yy += 50;
                }

                // theme swatches
                DrawTextEx(fSmall, "Theme", (Vector2){ px, yy + 6 }, 20, 0.3f, TXT);
                {
                    float sw = 28, gap = 10;
                    float tx = panelR.x + panelR.width - 22 - (NTHEMES + 1) * (sw + gap) + gap;
                    for (int k = 0; k <= NTHEMES; k++) {
                        Rectangle cell = { tx + k * (sw + gap), yy + 1, sw, sw };
                        bool hs = inview && CheckCollisionPointRec(mp, cell);
                        Vector2 cc = { cell.x + sw / 2, cell.y + sw / 2 };
                        if (k < NTHEMES) {
                            DrawRectangleRounded(cell, 0.5f, 8, THEMES[k].bg0);
                            DrawCircleV(cc, 8, THEMES[k].accent);
                        } else {   // Adaptive: two-tone dot — palette follows the video
                            DrawRectangleRounded(cell, 0.5f, 8, (Color){ 13, 13, 14, 255 });
                            DrawCircleSector(cc, 8, 90, 270, 16, (Color){ 201, 130, 74, 255 });
                            DrawCircleSector(cc, 8, 270, 450, 16, (Color){ 92, 148, 210, 255 });
                        }
                        if (k == g_theme)  DrawRectangleRoundedLines(cell, 0.5f, 8, TXT);
                        else if (hs)       DrawRectangleRoundedLines(cell, 0.5f, 8, alpha(TXT, 110));
                        else               DrawRectangleRoundedLines(cell, 0.5f, 8, alpha(WHITE, 24));
                        if (hs && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { apply_theme(k); osd("Theme: %s", k < NTHEMES ? THEMES[k].name : "Adaptive"); consumed = true; }
                    }
                    yy += 56;
                }

                DrawLineEx((Vector2){ px, yy }, (Vector2){ px + iw, yy }, 1, alpha(WHITE, 16));
                yy += 16;

                // playback speed — slider (0.25–2.00x, 0.05 steps, snaps to 1.00) + presets
                DrawTextEx(fSmall, "Playback speed", (Vector2){ px, yy }, 19, 0.3f, TXT);
                {
                    char spv[16]; snprintf(spv, sizeof spv, "%.2fx", player_speed(P));
                    Vector2 vm = MeasureTextEx(fSmall, spv, 19, 0.3f);
                    DrawTextEx(fSmall, spv, (Vector2){ panelR.x + panelR.width - 18 - vm.x, yy }, 19, 0.3f, ACCENT);
                    const float SLO = 0.25f, SHI = 2.0f;
                    Rectangle str = { px, yy + 30, iw, 6 };
                    float sfrac = clampf(((float)player_speed(P) - SLO) / (SHI - SLO), 0, 1);
                    DrawRectangleRounded(str, 1, 6, TRK);
                    float n1 = str.x + str.width * ((1.0f - SLO) / (SHI - SLO));
                    DrawRectangleRec((Rectangle){ n1 - 1, str.y - 3, 2, 12 }, alpha(MUT, 120));   // 1.00x notch
                    DrawRectangleRounded((Rectangle){ str.x, str.y, str.width * sfrac, str.height }, 1, 6, ACCENT);
                    scircle(str.x + str.width * sfrac, str.y + 3, 8, TXT);
                    Rectangle hit = { str.x - 4, str.y - 11, str.width + 8, 28 };
                    if (inview && CheckCollisionPointRec(mp, hit) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                        float v = SLO + clampf((mp.x - str.x) / str.width, 0, 1) * (SHI - SLO);
                        v = roundf(v * 20.0f) / 20.0f;
                        if (fabsf(v - 1.0f) < 0.03f) v = 1.0f;
                        player_set_speed(P, v);
                        consumed = true;
                    }
                    yy += 52;
                    const float pv[6] = { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f };
                    const char *pn[6] = { "0.5x", "0.75x", "1x", "1.25x", "1.5x", "2x" };
                    float chW = (iw - 5 * 8) / 6.0f;
                    for (int k = 0; k < 6; k++) {
                        Rectangle chip = { px + k * (chW + 8), yy, chW, 30 };
                        bool selc = fabs(player_speed(P) - pv[k]) < 0.011;
                        bool hc = inview && CheckCollisionPointRec(mp, chip);
                        DrawRectangleRounded(chip, 0.4f, 8, selc ? alpha(ACCENT, 70) : (hc ? alpha(WHITE, 18) : TRK));
                        Vector2 cw = MeasureTextEx(fSmall, pn[k], 16, 0.2f);
                        DrawTextEx(fSmall, pn[k], (Vector2){ chip.x + (chW - cw.x) / 2, chip.y + 6 }, 16, 0.2f, selc ? ACCENT : MUT);
                        if (hc && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { player_set_speed(P, pv[k]); osd("Speed %.2fx", player_speed(P)); consumed = true; }
                    }
                    yy += 48;
                }

                // ---- Performance (GPU acceleration) ----
                DrawLineEx((Vector2){ px, yy }, (Vector2){ px + iw, yy }, 1, alpha(WHITE, 16)); yy += 14;
                DrawTextEx(fSmall, "PERFORMANCE", (Vector2){ px, yy }, 15, 3.0f, alpha(ACCENT, 200)); yy += 26;
                {
                    const char *pl[2] = { "Hardware decoding", "GPU color conversion" };
                    bool pvv[2] = { CFG.hw_decode, CFG.gpu_convert };
                    for (int i = 0; i < 2; i++) {
                        Rectangle row = { px - 6, yy - 6, iw + 12, 46 };
                        bool hov = inview && CheckCollisionPointRec(mp, row);
                        if (hov) DrawRectangleRounded(row, 0.3f, 6, alpha(WHITE, 10));
                        DrawTextEx(fSmall, pl[i], (Vector2){ px, yy + 6 }, 20, 0.3f, TXT);
                        float tx = panelR.x + panelR.width - 76;
                        DrawRectangleRounded((Rectangle){ tx, yy, 54, 28 }, 1, 8, pvv[i] ? ACCENT : TRK);
                        scircle(pvv[i] ? tx + 39 : tx + 15, yy + 14, 11, pvv[i] ? BG1 : alpha(TXT, 210));
                        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                            if (i == 0) { CFG.hw_decode = !CFG.hw_decode; player_set_hw_decode(P, CFG.hw_decode);
                                          reload_current(); osd("Hardware decoding %s", CFG.hw_decode ? "On" : "Off"); }
                            else        { CFG.gpu_convert = !CFG.gpu_convert; player_set_gpu_convert(P, CFG.gpu_convert);
                                          osd("GPU color conversion %s", CFG.gpu_convert ? "On" : "Off"); }
                            consumed = true;
                        }
                        yy += 50;
                    }
                    char stat[112];
                    if (open && player_has_video(P))
                        snprintf(stat, sizeof stat, "Active:  %s  ·  %s", player_decode_desc(P), player_convert_desc(P));
                    else snprintf(stat, sizeof stat, "Active:  (no video loaded)");
                    DrawTextEx(fSmall, stat, (Vector2){ px, yy }, 16, 0.2f, alpha(MUT, 200)); yy += 30;
                }

                // ---- Subtitles (font styling for plain-text subs) ----
                DrawLineEx((Vector2){ px, yy }, (Vector2){ px + iw, yy }, 1, alpha(WHITE, 16)); yy += 14;
                DrawTextEx(fSmall, "SUBTITLES", (Vector2){ px, yy }, 15, 3.0f, alpha(ACCENT, 200)); yy += 26;
                {
                    // font family stepper: < Name >
                    static const char *FONTS[] = { "sans-serif", "Segoe UI", "Arial", "Verdana", "Tahoma",
                                                   "Calibri", "Georgia", "Times New Roman", "Consolas", "Comic Sans MS" };
                    const int NF = (int)(sizeof(FONTS) / sizeof(FONTS[0]));
                    DrawTextEx(fSmall, "Font", (Vector2){ px, yy + 4 }, 20, 0.3f, TXT);
                    int ci = 0; for (int k = 0; k < NF; k++) if (!strcmp(FONTS[k], CFG.sub_font)) { ci = k; break; }
                    float bw = 168, bx = panelR.x + panelR.width - 22 - bw;
                    Rectangle lb = { bx, yy, 26, 28 }, rb = { bx + bw - 26, yy, 26, 28 };
                    bool hl = inview && CheckCollisionPointRec(mp, lb), hr = inview && CheckCollisionPointRec(mp, rb);
                    DrawRectangleRounded(lb, 0.4f, 6, hl ? alpha(WHITE, 24) : TRK);
                    DrawRectangleRounded(rb, 0.4f, 6, hr ? alpha(WHITE, 24) : TRK);
                    DrawTextEx(fSmall, "<", (Vector2){ lb.x + 9, yy + 4 }, 20, 0, TXT);
                    DrawTextEx(fSmall, ">", (Vector2){ rb.x + 9, yy + 4 }, 20, 0, TXT);
                    Vector2 nm = MeasureTextEx(fSmall, FONTS[ci], 16, 0.2f);
                    DrawTextEx(fSmall, FONTS[ci], (Vector2){ bx + 26 + (bw - 52 - nm.x) / 2, yy + 6 }, 16, 0.2f, ACCENT);
                    if ((hl || hr) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                        ci = (ci + (hr ? 1 : NF - 1)) % NF;
                        snprintf(CFG.sub_font, sizeof CFG.sub_font, "%s", FONTS[ci]);
                        apply_sub_style(); consumed = true;
                    }
                    yy += 44;

                    // size scale slider
                    DrawTextEx(fSmall, "Size", (Vector2){ px, yy }, 19, 0.3f, TXT);
                    char sv[12]; snprintf(sv, sizeof sv, "%.2fx", CFG.sub_font_scale);
                    Vector2 svm = MeasureTextEx(fSmall, sv, 19, 0.3f);
                    DrawTextEx(fSmall, sv, (Vector2){ panelR.x + panelR.width - 18 - svm.x, yy }, 19, 0.3f, ACCENT);
                    {
                        const float LO = 0.25f, HI = 2.5f;
                        Rectangle str = { px, yy + 30, iw, 6 };
                        float fr = clampf((CFG.sub_font_scale - LO) / (HI - LO), 0, 1);
                        DrawRectangleRounded(str, 1, 6, TRK);
                        DrawRectangleRounded((Rectangle){ str.x, str.y, str.width * fr, str.height }, 1, 6, ACCENT);
                        scircle(str.x + str.width * fr, str.y + 3, 8, TXT);
                        Rectangle hit = { str.x - 4, str.y - 11, str.width + 8, 28 };
                        if (inview && CheckCollisionPointRec(mp, hit) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                            float nv = LO + clampf((mp.x - str.x) / str.width, 0, 1) * (HI - LO);
                            nv = roundf(nv * 20.0f) / 20.0f; if (fabsf(nv - 1.0f) < 0.03f) nv = 1.0f;
                            CFG.sub_font_scale = nv; apply_sub_style(); consumed = true;
                        }
                    }
                    yy += 50;

                    // colour swatch rows: text fill + outline
                    static const unsigned PAL[7] = { 0xFFFFFF, 0xFFEB3B, 0x00E5FF, 0x76FF03, 0xFF5252, 0xFF9800, 0x000000 };
                    for (int rowk = 0; rowk < 2; rowk++) {
                        unsigned cur = rowk ? CFG.sub_outline_color : CFG.sub_color;
                        DrawTextEx(fSmall, rowk ? "Outline color" : "Text color", (Vector2){ px, yy + 6 }, 20, 0.3f, TXT);
                        float sw = 24, gap = 8; int NP = 7;
                        float tx = panelR.x + panelR.width - 22 - NP * (sw + gap) + gap;
                        for (int k = 0; k < NP; k++) {
                            Rectangle cell = { tx + k * (sw + gap), yy, sw, sw };
                            bool hs = inview && CheckCollisionPointRec(mp, cell);
                            Color cc = { (unsigned char)(PAL[k] >> 16), (unsigned char)(PAL[k] >> 8), (unsigned char)PAL[k], 255 };
                            DrawRectangleRounded(cell, 0.4f, 6, cc);
                            if (cur == PAL[k])  DrawRectangleRoundedLines(cell, 0.4f, 6, ACCENT);
                            else if (hs)        DrawRectangleRoundedLines(cell, 0.4f, 6, alpha(TXT, 140));
                            else                DrawRectangleRoundedLines(cell, 0.4f, 6, alpha(WHITE, 30));
                            if (hs && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                                if (rowk) CFG.sub_outline_color = PAL[k]; else CFG.sub_color = PAL[k];
                                apply_sub_style(); consumed = true;
                            }
                        }
                        yy += 40;
                    }

                    // outline width + shadow depth sliders (0–4 px)
                    for (int slk = 0; slk < 2; slk++) {
                        float *pval = slk ? &CFG.sub_shadow : &CFG.sub_outline;
                        DrawTextEx(fSmall, slk ? "Shadow" : "Outline", (Vector2){ px, yy }, 19, 0.3f, TXT);
                        char vv[12]; snprintf(vv, sizeof vv, "%.1f px", *pval);
                        Vector2 vm = MeasureTextEx(fSmall, vv, 19, 0.3f);
                        DrawTextEx(fSmall, vv, (Vector2){ panelR.x + panelR.width - 18 - vm.x, yy }, 19, 0.3f, ACCENT);
                        const float LO = 0.0f, HI = 4.0f;
                        Rectangle str = { px, yy + 30, iw, 6 };
                        float fr = clampf((*pval - LO) / (HI - LO), 0, 1);
                        DrawRectangleRounded(str, 1, 6, TRK);
                        DrawRectangleRounded((Rectangle){ str.x, str.y, str.width * fr, str.height }, 1, 6, ACCENT);
                        scircle(str.x + str.width * fr, str.y + 3, 8, TXT);
                        Rectangle hit = { str.x - 4, str.y - 11, str.width + 8, 28 };
                        if (inview && CheckCollisionPointRec(mp, hit) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                            float nv = LO + clampf((mp.x - str.x) / str.width, 0, 1) * (HI - LO);
                            *pval = roundf(nv * 10.0f) / 10.0f; apply_sub_style(); consumed = true;
                        }
                        yy += 50;
                    }
                }

                // about
                DrawLineEx((Vector2){ px, yy }, (Vector2){ px + iw, yy }, 1, alpha(WHITE, 16));
                yy += 14;
                DrawTextEx(fSmall, "ABOUT", (Vector2){ px, yy }, 15, 3.0f, alpha(ACCENT, 200));
                yy += 26;
                DrawTextEx(fUI, "tivi", (Vector2){ px, yy }, 24, 1.5f, ACCENT);
                DrawTextEx(fSmall, "v" TIVI_VERSION, (Vector2){ px + 54, yy + 5 }, 17, 0.3f, TXT);
                yy += 32;
                DrawTextEx(fSmall, "A resizable, FFmpeg-powered video player.", (Vector2){ px, yy }, 17, 0.2f, MUT); yy += 24;
                DrawTextEx(fSmall, "FFmpeg · libass · raylib  —  public domain", (Vector2){ px, yy }, 17, 0.2f, MUT); yy += 30;
                DrawTextEx(fSmall, "Space play · J/L seek · F fullscreen · S snapshot", (Vector2){ px, yy }, 15, 0.2f, alpha(MUT, 170)); yy += 22;
                DrawTextEx(fSmall, "C subtitles · X audio · Q playlist · [ ] \\ speed", (Vector2){ px, yy }, 15, 0.2f, alpha(MUT, 170)); yy += 24;

                EndScissorMode();
                set_content_h = (yy + g_set_scroll) - viewTop;
                if (set_content_h > viewH) {   // scrollbar
                    float th = viewH * viewH / set_content_h;
                    float ty2 = viewTop + (maxScroll > 0 ? (viewH - th) * (g_set_scroll / maxScroll) : 0);
                    DrawRectangleRounded((Rectangle){ panelR.x + panelR.width - 5, ty2, 3, th }, 1, 4, alpha(ACCENT, 130));
                }
            }
        }

        // ---- OSD toast ----
        if (now < g_osd_until) {
            float k = clampf((float)((g_osd_until - now) / 0.4), 0, 1);
            Vector2 mw = MeasureTextEx(fUI, g_osd, 26, 0.3f);
            float ox = (W - mw.x) / 2, oy = H - CBH - 70;
            DrawRectangleRounded((Rectangle){ ox - 20, oy - 10, mw.x + 40, 48 }, 0.5f, 10, afade(BG1, 0.74f * k));
            DrawTextEx(fUI, g_osd, (Vector2){ ox, oy }, 26, 0.3f, afade(TXT, k));
        }

        // ---- right-click context menu (topmost) ----
        if (g_menu_open && menuN > 0) {
            DrawRectangleRounded((Rectangle){ menuR.x + 3, menuR.y + 4, menuR.width, menuR.height }, 0.12f, 10, alpha(BLACK, 90));   // soft shadow
            DrawRectangleRounded(menuR, 0.12f, 10, alpha(BG0, 250));
            DrawRectangleRoundedLines(menuR, 0.12f, 10, alpha(WHITE, 26));
            for (int i = 0; i < menuN; i++) {
                Rectangle row = { menuR.x + 5, menuR.y + menuPad + i * menuRowH, menuR.width - 10, menuRowH };
                bool hov = CheckCollisionPointRec(mp, row);
                if (hov) DrawRectangleRounded(row, 0.3f, 8, alpha(ACCENT, 46));
                DrawTextEx(fSmall, menuLbl[i], (Vector2){ menuR.x + 20, row.y + (menuRowH - 19) / 2 - 1 }, 19, 0.3f, hov ? TXT : alpha(TXT, 225));
                if (menuDot[i]) DrawCircleV((Vector2){ menuR.x + menuR.width - 18, row.y + menuRowH / 2 }, 3.6f, ACCENT);
            }
        }

        // ---- top-right notification (file added, etc.) ----
        if (now < g_note_until) {
            float k = clampf((float)((g_note_until - now) / 0.5), 0, 1);
            Vector2 mw = MeasureTextEx(fSmall, g_note, 19, 0.2f);
            float bw = mw.x + 40, bh = 50;
            float right = (g_panel != PANEL_NONE) ? panelR.x : (float)W;
            float nx = right - bw - 16, ny = TBH + 16;
            DrawRectangleRounded((Rectangle){ nx, ny, bw, bh }, 0.3f, 10, afade(BG0, 0.95f * k));
            DrawRectangleRoundedLines((Rectangle){ nx, ny, bw, bh }, 0.3f, 10, afade(ACCENT, 0.45f * k));
            DrawRectangleRounded((Rectangle){ nx, ny + 9, 4, bh - 18 }, 1, 4, afade(ACCENT, k));   // accent tick
            draw_fit(fSmall, g_note, (Vector2){ nx + 18, ny + (bh - mw.y) / 2 }, 19, 0.2f, afade(TXT, k), bw - 30);
        }

        rlPopMatrix();
        EndTextureMode();

        BeginDrawing();
        ClearBackground(BLACK);
        DrawTexturePro(target.texture, (Rectangle){ 0, 0, (float)target.texture.width, -(float)target.texture.height },
                       (Rectangle){ 0, 0, (float)W, (float)H }, (Vector2){ 0, 0 }, 0, WHITE);
        EndDrawing();

        frame++;
        if (shot_frame > 0) g_last_activity = GetTime();   // keep controls visible for the shot
        if (shot_frame > 0 && frame == shot_frame) TakeScreenshot("tivi_shot.png");
        if (shot_frame > 0 && frame == shot_frame + 3) break;
        // dev hook: scripted relative seeks (+5, +5, -5) to test seek precision headlessly
        {
            static int seektest = -1; if (seektest < 0) seektest = getenv("TIVI_SEEKTEST") ? 1 : 0;
            if (seektest && open && (frame == 240 || frame == 480)) player_seek_relative(P, 5);
            if (seektest && open && frame == 720)                   player_seek_relative(P, -5);
        }

        // ---- perf probe (TIVI_PERF=1): UI fps + video update rate, every ~2 s ----
        {
            static int perf = -1; if (perf < 0) perf = getenv("TIVI_PERF") ? 1 : 0;
            if (perf) {
                static double w0 = 0, worst = 0, lastpos = 0, maxstep = 0; static int n = 0, stalls = 0, lurches = 0;
                double t = GetTime(), ft = t - now;
                if (w0 == 0) w0 = t;
                if (ft > worst) worst = ft;
                double pos = open ? player_position(P) : 0;
                double dstep = pos - lastpos; lastpos = pos;
                if (playing && dstep >= 0) {
                    if (dstep < 0.0005) stalls++;
                    else if (dstep > 0.030) lurches++;
                    if (dstep > maxstep) maxstep = dstep;
                }
                n++;
                if (t - w0 >= 2.0) {
                    unsigned acb, amax; audio_out_perf(&acb, &amax);
                    printf("[perf] ui: %.1f fps (worst %.0f ms)  video: %.1f fps  clock: %d stalls %d lurches (max %.0f ms)  audio-cb: %u/win max %u fr  win %dx%d\n",
                           n / (t - w0), worst * 1000.0, g_perf_vidupd / (t - w0),
                           stalls, lurches, maxstep * 1000.0, acb, amax, W, H);
                    fflush(stdout);
                    w0 = t; n = 0; worst = 0; g_perf_vidupd = 0; stalls = lurches = 0; maxstep = 0;
                }
            }
        }
    }
    UnloadRenderTexture(target);

    // ---- persist + cleanup ----
    CFG.volume = g_volume; CFG.always_on_top = g_aot; CFG.subtitles_enabled = g_subs_on; CFG.letterbox_black = g_letterbox_black;
    CFG.click_pause = g_click_pause; CFG.theme = g_theme;
    if (g_fullscreen) { CFG.win_x = g_fs_x; CFG.win_y = g_fs_y; CFG.win_w = g_fs_w; CFG.win_h = g_fs_h; CFG.has_win = true; }   // pre-fullscreen geometry, not the monitor cover
    else if (!g_maximized && !os_is_zoomed(GetWindowHandle())) { Vector2 wp = GetWindowPosition(); CFG.win_x = (int)wp.x; CFG.win_y = (int)wp.y; CFG.win_w = GetScreenWidth(); CFG.win_h = GetScreenHeight(); CFG.has_win = true; }
    viconfig_save(&CFG);

    player_destroy(P);
    subs_destroy(SUB);
    destroy_textures();
    yuvtex_destroy(&g_yuv);
    UnloadShader(adjShader);
    audio_out_shutdown();
    CloseAudioDevice();
    CloseWindow();
    playlist_free(&PL);
    return 0;
}
