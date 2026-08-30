# Hisashi menubar (hoswl) — Design

**Date:** 2026-08-30
**Status:** Implemented and verified (v0.4.0)

## Problem

tivi is borderless and has no menu bar; everything is keys, the control strip
and panels. Hisashi (the Fezcode top bar) renders a macOS-style global menu
bar for the foreground app over a tiny named-pipe protocol, **hoswl**. When
Hisashi is running, tivi should get a proper File / Playback / Audio /
Subtitles / Video / Help menu in the bar — the same actions the keys and
panels run, with live checkmarks and the file's audio/subtitle tracks as
submenus.

## Behavior

- `src/menubar.c` (contract `src/menubar.h`, same shape as pidi's) rebuilds the
  whole tree as hoswl's line DSL from a `MenubarState` snapshot that `main.c`
  fills once per frame, and republishes only when an FNV-1a fingerprint of that
  snapshot changes. Clicks come back as ids through `hoswl_poll()` and are
  dispatched to `menu_click()` on the main thread — the pipe never runs code.
- `src/hoswl.h` is a verbatim copy of `Hisashi/sdk/hoswl/hoswl.h`; never edited
  here.
- Menus: **File** (Open… `O`, Load Subtitle File…, Save Snapshot `S`, Quit),
  **Playback** (Play/Pause `Space` — the label follows the state, Stop,
  Previous `B`, Next `N`, Back/Forward 10 s `J`/`L`, Speed › 0.5–2× presets +
  Keep Pitch, Repeat › Off/One/All, Shuffle), **Audio** (Volume Up/Down `↑`/`↓`,
  Mute `M`, one radio row per audio track), **Subtitles** (Off, one radio row per
  embedded track, Load Subtitle File…), **Video** (Full Screen `F`, Always on
  Top `T`, Hardware Decoding, GPU Color Conversion, Adjustments `A`, Reset
  Adjustments, Playlist `Q`, Settings `G`), **Help** (About tivi… → Settings).
  Rows that need an open file are disabled until one is.
- Per-frame hook sits with the other drained event sources (after
  `mediakeys_poll()`); `menubar_shutdown()` sends `bye` before the FFmpeg
  teardown so the bar drops tivi at once.
- **Toggle** — Settings row "Hisashi menubar" in a new HISASHI section (with a
  `connected / waiting for Hisashi / off` status line); persisted as
  `hisashi_menubar` in config.ini; default **on** (harmless without Hisashi:
  a failed connect every 2 s).
- Hardware-decoding from the bar mirrors the panel: it reopens the file
  (`reload_current()`), so expect a dropped frame.

## Tests

`test_menubar.c` (repo root, hand-rolled PASS/FAIL harness like
`test_subs_preload.c`) builds the tree for an empty and a populated state and
checks disabled rows, checkmarks, track rows (including a `|` in a title) and
that the DSL compiles through `hoswl_compile_menu_text`. Build line is in the
file header.
