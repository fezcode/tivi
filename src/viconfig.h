#ifndef TIVI_VICONFIG_H
#define TIVI_VICONFIG_H

#include <stdbool.h>

// Persisted per-user settings, stored in %APPDATA%\tivi\config.ini.
typedef struct {
    float volume;            // 0..1
    bool  always_on_top;
    bool  subtitles_enabled;
    bool  letterbox_black;   // pure-black letterbox bars instead of the warm tint
    bool  click_pause;       // single click on the video toggles play/pause
    bool  auto_queue;        // opening one file queues the later episodes in its folder
    int   theme;             // UI palette index (see THEMES in main.c)

    // Video adjustments (VLC "Video Effects" essentials).
    float brightness;        // -1..1   (0 = neutral)
    float contrast;          // 0..2    (1 = neutral)
    float saturation;        // 0..2    (1 = neutral)
    float hue;               // -180..180 degrees (0 = neutral)
    float gamma;             // 0.2..3  (1 = neutral)

    // Performance / GPU acceleration.
    bool  hw_decode;         // true = auto D3D11VA GPU decode (+fallback); false = force software
    bool  gpu_convert;       // true = shader YUV->RGB; false = force CPU sws_scale

    // Subtitle font styling (applied to plain-text subs only; ASS/SSA keep their own).
    char     sub_font[64];       // family name, default "sans-serif"
    float    sub_font_scale;     // 0.25..2.5 (1 = default)
    unsigned sub_color;          // 0xRRGGBB fill
    unsigned sub_outline_color;  // 0xRRGGBB outline
    float    sub_outline;        // 0..4 px
    float    sub_shadow;         // 0..4 px

    int   win_x, win_y, win_w, win_h;
    bool  has_win;
} ViConfig;

void viconfig_defaults(ViConfig *c);
bool viconfig_load(ViConfig *c);
bool viconfig_save(const ViConfig *c);

#endif
