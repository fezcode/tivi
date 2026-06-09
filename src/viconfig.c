#include "viconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

static void cfg_path(char *out, int cap) {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata && *appdata) {
        char dir[512]; snprintf(dir, sizeof(dir), "%s\\tivi", appdata);
        CreateDirectoryA(dir, NULL);
        snprintf(out, cap, "%s\\config.ini", dir);
    } else snprintf(out, cap, "config.ini");
#else
    const char *home = getenv("HOME");
    if (home && *home) snprintf(out, cap, "%s/.tivi-config.ini", home);
    else snprintf(out, cap, "config.ini");
#endif
}

void viconfig_defaults(ViConfig *c) {
    memset(c, 0, sizeof(*c));
    c->volume = 0.85f;
    c->always_on_top = false;
    c->subtitles_enabled = true;
    c->letterbox_black = true;   // black bars look right for video by default
    c->click_pause = true;
    c->theme = 0;
    c->brightness = 0.0f;
    c->contrast   = 1.0f;
    c->saturation = 1.0f;
    c->hue        = 0.0f;
    c->gamma      = 1.0f;
    c->has_win = false;
}

bool viconfig_load(ViConfig *c) {
    viconfig_defaults(c);
    char path[600]; cfg_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64]; float fv; int iv;
        if (sscanf(line, " %63[^= ] = %f", key, &fv) == 2) {
            if      (!strcmp(key, "volume"))     c->volume = fv;
            else if (!strcmp(key, "brightness")) c->brightness = fv;
            else if (!strcmp(key, "contrast"))   c->contrast = fv;
            else if (!strcmp(key, "saturation")) c->saturation = fv;
            else if (!strcmp(key, "hue"))        c->hue = fv;
            else if (!strcmp(key, "gamma"))      c->gamma = fv;
        }
        if (sscanf(line, " %63[^= ] = %d", key, &iv) == 2) {
            if      (!strcmp(key, "always_on_top"))     c->always_on_top = iv != 0;
            else if (!strcmp(key, "subtitles_enabled")) c->subtitles_enabled = iv != 0;
            else if (!strcmp(key, "letterbox_black"))   c->letterbox_black = iv != 0;
            else if (!strcmp(key, "click_pause"))       c->click_pause = iv != 0;
            else if (!strcmp(key, "theme"))             c->theme = iv;
            else if (!strcmp(key, "win_x")) { c->win_x = iv; c->has_win = true; }
            else if (!strcmp(key, "win_y"))  c->win_y = iv;
            else if (!strcmp(key, "win_w"))  c->win_w = iv;
            else if (!strcmp(key, "win_h"))  c->win_h = iv;
        }
    }
    fclose(f);
    return true;
}

bool viconfig_save(const ViConfig *c) {
    char path[600]; cfg_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "# tivi settings\n");
    fprintf(f, "volume=%.3f\n", c->volume);
    fprintf(f, "always_on_top=%d\n", c->always_on_top ? 1 : 0);
    fprintf(f, "subtitles_enabled=%d\n", c->subtitles_enabled ? 1 : 0);
    fprintf(f, "letterbox_black=%d\n", c->letterbox_black ? 1 : 0);
    fprintf(f, "click_pause=%d\n", c->click_pause ? 1 : 0);
    fprintf(f, "theme=%d\n", c->theme);
    fprintf(f, "brightness=%.3f\n", c->brightness);
    fprintf(f, "contrast=%.3f\n",   c->contrast);
    fprintf(f, "saturation=%.3f\n", c->saturation);
    fprintf(f, "hue=%.3f\n",        c->hue);
    fprintf(f, "gamma=%.3f\n",      c->gamma);
    if (c->has_win) {
        fprintf(f, "win_x=%d\n", c->win_x);
        fprintf(f, "win_y=%d\n", c->win_y);
        fprintf(f, "win_w=%d\n", c->win_w);
        fprintf(f, "win_h=%d\n", c->win_h);
    }
    fclose(f);
    return true;
}
