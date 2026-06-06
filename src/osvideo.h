#ifndef TIVI_OSVIDEO_H
#define TIVI_OSVIDEO_H

#include <stdbool.h>

// Native OS "open video/audio files" dialog (multi-select). Invokes on_file once
// per chosen file with a UTF-8 path. Returns the number of files chosen (0 if
// cancelled). Windows-only; a stub elsewhere.
int os_open_media_files(void (*on_file)(const char *utf8_path, void *ud), void *ud);

// Native "open subtitle file" dialog (single). Fills out (UTF-8) and returns true,
// or false if cancelled.
bool os_open_subtitle_file(char *out, int cap);

// Toggle Win11 DWM rounded corners on the borderless window (off when maximized /
// fullscreen so the picture reaches the screen edges). hwnd = GetWindowHandle().
void os_round_window(void *hwnd, bool rounded);

// Drop an aero-style shadow on the borderless window so it reads as a real window.
void os_window_shadow(void *hwnd);

// Process arguments as UTF-8 (out[0] = program path). On Windows the real Unicode
// command line is decoded (ANSI argv mangles non-ASCII paths). Lives for the
// process lifetime — do not free.
char **os_args_utf8(int argc, char **argv, int *out_count);

// Restore + raise the given OS window to the foreground. No-op off Windows.
void os_focus_window(void *hwnd);

// Work area (excludes the taskbar) of the monitor the window is on. Returns false
// off Windows. Used to implement "maximize" for the borderless window.
bool os_work_area(void *hwnd, int *x, int *y, int *w, int *h);

// Attach a GUI-subsystem (-mwindows) process to the parent console so that
// printf from CLI modes (-h / -v / --probe) is visible in the terminal.
void os_console_attach(void);

#endif
