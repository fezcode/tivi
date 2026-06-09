// Verifies the embedded-subtitle full-track preload: after preloading, libass
// can render at ANY timestamp (i.e. seeking can never miss a line). Uses
// test_clip_subs.mkv (subrip stream 2: "line one" 0.5–3.0s, "line two" 3.0–5.5s).
//
// Build: gcc -O2 -std=c11 -Isrc src/subs.c test_subs_preload.c -o build/test_subs.exe `pkg-config --cflags --libs libass libavformat libavcodec libavutil`
#include "subs.h"
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleep_ms(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
#endif

static int fails = 0;
static void check(bool cond, const char *what) { printf("%s %s\n", cond ? "PASS" : "FAIL", what); if (!cond) fails++; }

int main(void) {
    Subs *s = subs_create();
    if (!s) { printf("FAIL subs_create\n"); return 1; }
    subs_set_size(s, 640, 360);

    // Simulate the player state right after a seek: a live track exists but the
    // events around the seek point were never fed.
    subs_begin_embedded(s, NULL, 0);
    uint8_t *rgba; int w, h; bool ch;
    check(!subs_render(s, 1500, &rgba, &w, &h, &ch), "live track w/o fed events shows nothing (the bug)");

    subs_preload_start(s, "test_clip_subs.mkv", 2);
    int waited = 0;
    while (!subs_is_preloaded(s) && waited < 15000) { subs_preload_update(s); sleep_ms(10); waited += 10; }
    check(subs_is_preloaded(s), "background preload completed");

    check( subs_render(s, 1500, &rgba, &w, &h, &ch), "subtitle visible at 1.5s (seek into line 1)");
    check( subs_render(s, 4000, &rgba, &w, &h, &ch), "subtitle visible at 4.0s (seek into line 2)");
    check(!subs_render(s, 5900, &rgba, &w, &h, &ch), "no subtitle at 5.9s (past last line)");

    // cancelling mid-flight must not crash or leak the track
    subs_preload_start(s, "test_clip_subs.mkv", 2);
    subs_clear(s);
    check(!subs_is_preloaded(s), "subs_clear cancels a pending preload");

    subs_destroy(s);
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
