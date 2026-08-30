// Test for src/menubar.c — the Hisashi menubar tree built from a MenubarState.
// Pure: no window, no pipe, no player (hoswl's offline compile step only).
//
// build + run (MSYS2 MinGW gcc):
//   gcc -O2 -std=c11 -Isrc src/menubar.c test_menubar.c -o build/test_menubar.exe && build/test_menubar.exe
#include "menubar.h"
#include "hoswl.h"   // hoswl_compile_menu_text (implementation lives in menubar.c)

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int fails = 0;
static void check(bool cond, const char *what) { printf("%s %s\n", cond ? "PASS" : "FAIL", what); if (!cond) fails++; }

int main(void) {
    static char text[HOSWL_MENU_MAX], json[HOSWL_MENU_MAX], err[160];
    MenubarState s = { 0 };

    // Nothing open: file-dependent rows are disabled, no track rows.
    check(menubar_build_text(&s, text, sizeof text) == 0, "build with nothing open");
    check(strstr(text, " pb.toggle|Play|Space|d\n") != NULL, "Play row disabled while nothing is open");
    check(strstr(text, " file.open|Open…|O\n") != NULL, "Open row always enabled");
    check(strstr(text, "audio.track.") == NULL, "no audio track rows without a file");
    check(strstr(text, " sub.off|Off||x\n") != NULL, "Subtitles Off is checked by default");
    check(hoswl_compile_menu_text(text, json, sizeof json, err, sizeof err) == 0, "DSL compiles to JSON");

    // A file with two audio tracks and one subtitle track, playing at 1.5x, repeat all.
    s.open = true; s.playing = true; s.has_next = true; s.speed_idx = 4; s.repeat = 2;
    s.cur_audio = 1; s.cur_sub = 3; s.subs_on = true; s.hw_decode = true; s.panel = 5;
    s.ntracks = 3;
    s.tracks[0] = (MenubarTrack){ 0, 1, "English (AAC)" };
    s.tracks[1] = (MenubarTrack){ 0, 2, "Commentary | director" };   // '|' must not split the row
    s.tracks[2] = (MenubarTrack){ 1, 3, "English (SRT)" };
    check(menubar_build_text(&s, text, sizeof text) == 0, "build with a file open");
    check(strstr(text, " pb.toggle|Pause|Space|\n") != NULL, "Play row reads Pause while playing");
    check(strstr(text, "  speed.4|1.5×||x\n") != NULL, "1.5x preset checked");
    check(strstr(text, "  speed.2|1× (Normal)||c\n") != NULL, "other presets checkable-off");
    check(strstr(text, "  repeat.2|All||x\n") != NULL, "repeat all checked");
    check(strstr(text, " audio.track.1|English (AAC)||x\n") != NULL, "current audio track checked");
    check(strstr(text, " audio.track.2|Commentary   director||c\n") != NULL, "pipe in a title is neutralised");
    check(strstr(text, " sub.track.3|English (SRT)||x\n") != NULL, "current subtitle track checked");
    check(strstr(text, " sub.off|Off||c\n") != NULL, "Subtitles Off unchecked while a track shows");
    check(strstr(text, " video.hw|Hardware Decoding||x\n") != NULL, "hardware decoding checked");
    check(strstr(text, " video.settings|Settings|G|x\n") != NULL, "settings panel checked");
    check(hoswl_compile_menu_text(text, json, sizeof json, err, sizeof err) == 0, "DSL with tracks compiles to JSON");
    check(strstr(json, "\"id\":\"audio.track.2\",\"label\":\"Commentary   director\",\"check\":false") != NULL, "JSON carries the track row");

    // Too small a buffer is reported, not overflowed.
    char tiny[64];
    check(menubar_build_text(&s, tiny, sizeof tiny) == -1, "tiny buffer rejected");

    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
