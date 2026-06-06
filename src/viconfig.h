#ifndef TIVI_VICONFIG_H
#define TIVI_VICONFIG_H

#include <stdbool.h>

// Persisted per-user settings, stored in %APPDATA%\tivi\config.ini.
typedef struct {
    float volume;            // 0..1
    bool  always_on_top;
    bool  subtitles_enabled;
    bool  letterbox_black;   // pure-black letterbox bars instead of the warm tint

    // Video adjustments (VLC "Video Effects" essentials).
    float brightness;        // -1..1   (0 = neutral)
    float contrast;          // 0..2    (1 = neutral)
    float saturation;        // 0..2    (1 = neutral)
    float hue;               // -180..180 degrees (0 = neutral)
    float gamma;             // 0.2..3  (1 = neutral)

    int   win_x, win_y, win_w, win_h;
    bool  has_win;
} ViConfig;

void viconfig_defaults(ViConfig *c);
bool viconfig_load(ViConfig *c);
bool viconfig_save(const ViConfig *c);

#endif
