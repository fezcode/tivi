// osvideo_mac.m — macOS backend for osvideo.h (Cocoa).
// NSOpenPanel dialogs, layer corner rounding + shadow for the borderless
// NSWindow, NSScreen work area, native zoom, readdir folder scan. Aero-snap
// hit-testing is Windows-only — the manual work-area maximize path is used
// instead (os_snap_active() returns false). Compile with ARC (-fobjc-arc).
#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include "osvideo.h"

// Same list as the Windows lpstrFilter, minus the glob syntax.
static NSArray<NSString *> *media_exts(void) {
    return @[ @"mp4", @"mkv", @"webm", @"avi", @"mov", @"m4v", @"wmv", @"flv",
              @"mpg", @"mpeg", @"ts", @"m2ts", @"mts", @"vob", @"ogv", @"3gp", @"divx",
              @"mp3", @"flac", @"wav", @"ogg", @"opus", @"m4a", @"aac", @"wma", @"ac3" ];
}

static void apply_types(NSOpenPanel *panel, NSArray<NSString *> *exts) {
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (NSString *ext in exts) {
        UTType *t = [UTType typeWithFilenameExtension:ext];
        if (t) [types addObject:t];
    }
    if (types.count) panel.allowedContentTypes = types;
}

int os_open_media_files(void (*on_file)(const char *, void *), void *ud) {
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = YES;
        panel.title = @"Open Media Files";
        apply_types(panel, media_exts());
        int count = 0;
        if ([panel runModal] == NSModalResponseOK) {
            for (NSURL *url in panel.URLs) {
                const char *p = url.fileSystemRepresentation;
                if (p) { on_file(p, ud); count++; }
            }
        }
        return count;
    }
}

bool os_open_subtitle_file(char *out, int cap) {
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.title = @"Open Subtitle File";
        apply_types(panel, @[ @"srt", @"ass", @"ssa", @"vtt", @"sub", @"sbv", @"lrc" ]);
        if ([panel runModal] != NSModalResponseOK) return false;
        const char *p = panel.URL.fileSystemRepresentation;
        if (!p) return false;
        snprintf(out, cap, "%s", p);
        return true;
    }
}

// hwnd is raylib's GetWindowHandle() → the borderless GLFW NSWindow. Rounded
// corners are clipped on the content view's layer (≈ the Win11 DWM radius);
// maximize/fullscreen squares them off so the picture reaches the edges.
void os_round_window(void *hwnd, bool rounded) {
    NSWindow *win = (__bridge NSWindow *)hwnd;
    if (!win) return;
    win.opaque = NO;
    win.backgroundColor = [NSColor clearColor];
    NSView *v = win.contentView;
    v.wantsLayer = YES;
    v.layer.cornerRadius = rounded ? 10.0 : 0.0;
    v.layer.masksToBounds = YES;
    [win invalidateShadow];
}

void os_window_shadow(void *hwnd) {
    NSWindow *win = (__bridge NSWindow *)hwnd;
    if (!win) return;
    win.hasShadow = YES;
    [win invalidateShadow];
}

// argv is already UTF-8 on macOS.
char **os_args_utf8(int argc, char **argv, int *out_count) {
    *out_count = argc;
    return argv;
}

void os_focus_window(void *hwnd) {
    NSWindow *win = (__bridge NSWindow *)hwnd;
    [NSApp activateIgnoringOtherApps:YES];
    if (win) {
        if (win.miniaturized) [win deminiaturize:nil];
        [win makeKeyAndOrderFront:nil];
    }
}

// visibleFrame (excludes menu bar + Dock) of the window's screen, translated
// into GLFW's screen space: origin at the top-left of the PRIMARY screen,
// y growing downward (Cocoa's origin is the primary's bottom-left, y upward).
bool os_work_area(void *hwnd, int *x, int *y, int *w, int *h) {
    NSWindow *win = (__bridge NSWindow *)hwnd;
    if (!win) return false;
    NSScreen *scr = win.screen ?: NSScreen.mainScreen;
    if (!scr) return false;
    NSRect v = scr.visibleFrame;
    CGFloat primaryTop = NSMaxY(NSScreen.screens.firstObject.frame);
    *x = (int)v.origin.x;
    *y = (int)(primaryTop - NSMaxY(v));
    *w = (int)v.size.width;
    *h = (int)v.size.height;
    return true;
}

void os_console_attach(void) {}   // stdout already reaches the terminal on macOS

// Aero-snap subclassing is Windows-only; report inactive so main.c uses the
// manual work-area maximize. macOS edge-drag tiling needs a titled window,
// which the borderless shell deliberately isn't.
void os_enable_snap(void *hwnd, int cap_left, int cap_right, int cap_h, int border) {
    (void)hwnd; (void)cap_left; (void)cap_right; (void)cap_h; (void)border;
}
bool os_snap_active(void) { return false; }
void os_snap_set_enabled(bool on) { (void)on; }

bool os_is_zoomed(void *hwnd) {
    NSWindow *win = (__bridge NSWindow *)hwnd;
    return win ? win.zoomed : false;
}

void os_native_maximize_toggle(void *hwnd) {
    NSWindow *win = (__bridge NSWindow *)hwnd;
    if (win) [win zoom:nil];
}

int os_scan_dir_files(const char *utf8dir, void (*on_file)(const char *, void *), void *ud) {
    DIR *d = opendir(utf8dir);
    if (!d) return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", utf8dir, e->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) { on_file(full, ud); count++; }
    }
    closedir(d);
    return count;
}

bool os_desktop_dir(char *out, int cap) {
    @autoreleasepool {
        NSArray<NSString *> *dirs =
            NSSearchPathForDirectoriesInDomains(NSDesktopDirectory, NSUserDomainMask, YES);
        if (!dirs.count) return false;
        snprintf(out, cap, "%s", dirs.firstObject.fileSystemRepresentation);
        return true;
    }
}

// ---- Finder "Open With" / double-click ----
// macOS delivers opened documents to the app delegate (AppKit converts the
// kAEOpenDocuments Apple Event into -application:openFiles: during its own
// event routing — a raw NSAppleEventManager handler gets overwritten when
// NSApplication finishes launching). GLFW's delegate doesn't implement the
// method, so graft it on with the runtime BEFORE InitWindow registers the
// delegate. Received paths go through our own single-instance socket: the
// listen thread queues them and the main loop plays them like a CLI handoff.
#include "singleinst.h"
#import <objc/runtime.h>

static void open_files_imp(id self, SEL _cmd, NSApplication *app, NSArray<NSString *> *files) {
    (void)self; (void)_cmd;
    for (NSString *f in files) singleinst_send_file(f.fileSystemRepresentation);
    [app replyToOpenOrPrint:NSApplicationDelegateReplySuccess];
}

void os_open_files_handler_install(void) {
    Class cls = NSClassFromString(@"GLFWApplicationDelegate");
    if (cls) class_addMethod(cls, @selector(application:openFiles:), (IMP)open_files_imp, "v@:@@");
}

#endif // __APPLE__
