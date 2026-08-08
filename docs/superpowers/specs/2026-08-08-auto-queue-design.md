# Auto-queue next episodes

**Date:** 2026-08-08
**Status:** Approved (queue scope: "only episodes after the opened file")

## Problem

Opening one episode from a season folder plays that file and stops. The user
wants the rest of the season queued automatically so auto-advance / `N` walks
forward without rebuilding a playlist by hand.

## Behavior

- **Trigger** — only when an open action starts playback *fresh* with exactly
  **one** media file:
  - CLI launch with a single file (double-click in Explorer),
  - single-instance forward while nothing is open,
  - drag-drop of a single file when nothing is open,
  - Open dialog with a single selection when nothing is open.
  Multiple files at once, or adding while something is already open/queued,
  never auto-queues (a curated queue is left untouched).
- **Action** — scan the opened file's directory, keep media files (video +
  audio extension set mirroring the Open dialog filter; subtitle files never
  qualify), natural-sort case-insensitively (`E2 < E10`), and append only the
  files strictly **after** the opened one by that order. The opened file plays
  immediately at index 0.
- **Feedback** — top-right note: `Queued N next from folder`.
- **Toggle** — Settings row "Auto-queue folder" with the other toggles;
  persisted as `auto_queue` in `config.ini`; default **on**.

## Implementation

- `osvideo.c/h`: `os_scan_dir_files(utf8dir, cb, ud)` — `FindFirstFileW`
  enumeration with UTF-8 conversion (matches the existing wide-char handling;
  raylib's `LoadDirectoryFiles` goes through ANSI `opendir` and would break on
  non-ASCII names).
- `main.c`: `is_media_ext()` (video + audio list), `natcmp()` (numeric-aware,
  case-insensitive), `autoqueue_siblings(opened)` — collect, sort, compare by
  file name (`natcmp(name, opened_name) > 0` → queue). Called from the four
  open sites only; never from `load_file`/`next_track`, so advancing to the
  next episode cannot re-trigger a scan.
- `viconfig.c/h`: `bool auto_queue`, default true.
- Settings panel: extend the toggle block (5th row).
- Edge cases: unreadable directory or no later siblings → silent no-op;
  shuffle/repeat apply to the queued list exactly as to a hand-built one.

## Verification

Scratch folder with dummy episode files including `E2`/`E10`-style names to
prove natural ordering; open a middle episode from the CLI; confirm queue
content/order in the playlist panel (screenshot hook extended:
`TIVI_SHOT_PANEL=playlist`), and that earlier episodes stay out.
