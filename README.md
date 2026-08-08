<p align="center">
  <img src="docs/assets/banner.svg" alt="tivi — a small, native video player" width="100%">
</p>

<p align="center">
  <b>A small, native video player that plays <i>everything</i> — GPU-fast decode, crisp subtitles, a warm Hi-Fi shell.</b>
</p>

<p align="center">
  <a href="https://github.com/fezcode/tivi/releases/latest"><img alt="Release" src="https://img.shields.io/github/v/release/fezcode/tivi?style=flat&color=c9a45a"></a>
  <a href="https://github.com/fezcode/tivi/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/fezcode/tivi/total?style=flat&color=c9a45a"></a>
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows-0078D6?style=flat">
  <img alt="C11" src="https://img.shields.io/badge/C-C11-A8B9CC?style=flat&logo=c&logoColor=white">
  <img alt="raylib" src="https://img.shields.io/badge/raylib-f5f5f5?style=flat&logo=raylib&logoColor=black">
  <img alt="FFmpeg" src="https://img.shields.io/badge/FFmpeg-007808?style=flat&logo=ffmpeg&logoColor=white">
  <img alt="libass" src="https://img.shields.io/badge/libass-5b4a86?style=flat">
  <img alt="License" src="https://img.shields.io/badge/license-public%20domain-22c55e?style=flat">
</p>

---

Built on [raylib](https://www.raylib.com/) for the window and GPU, [FFmpeg](https://ffmpeg.org/)
for demux/decode, and [libass](https://github.com/libass/libass) for subtitles — styled after
**Timp**. Every codec FFmpeg supports, hardware-accelerated decode, styled subtitles, video
adjustments, and track switching, wrapped in a borderless, fluid interface.

<p align="center">
  <img src="examples/hero.png" alt="tivi playing the Sintel trailer" width="860"/>
</p>

## Features

- **Plays everything** — demux + decode through FFmpeg's `libav*`: H.264 / HEVC / AV1 / VP9 /
  MPEG-2/4, VC-1, ProRes, Theora, … with AAC / AC-3 / E-AC-3 / DTS / Opus / Vorbis / MP3 /
  FLAC / TrueHD audio.
- **GPU-accelerated** — D3D11VA hardware decode with automatic software fallback, plus
  shader-side YUV→RGB color conversion (8-bit NV12 and 10-bit P010). Smooth 1080p x265
  10-bit on modest CPUs, with toggles and a live decode-path readout in Settings.
- **Subtitles, done right** — embedded and external `.srt` / `.ass` / `.ssa` / `.vtt` / `.sub`,
  image subs (PGS / VOBSUB / DVB), full ASS/SSA styling. Rasterized at display resolution for
  crisp anti-aliasing, with font, size, color, outline and shadow options for plain-text subs —
  authored ASS styling is respected.
- **Pitch-preserving speed** — 0.25–2× playback with VLC-style time-stretch, so voices stay
  natural at 1.5×; the classic pitch-shifting mode is a toggle away.
- **Precise seeking** — arrow-key seeks land exactly on target even on WEB-DLs with 10–20 s
  keyframe gaps.
- **Auto-queue** — open one episode and the rest of the folder queues behind it in natural
  order (`E2` before `E10`); toggleable in Settings.
- **Adjustments and themes** — brightness, contrast, saturation, hue and gamma applied live on
  the GPU; five color themes plus an Adaptive theme that tints the UI from the playing video.
- **Volume boost** — software amplification up to 200% for quiet media, with a 100% notch on
  the slider.
- **Native feel** — borderless resizable window, aero snap, Win11 rounded corners and shadow,
  fullscreen, always-on-top, right-click context menu, pinnable control bar.
- **A/V sync that holds** — audio-master clock with starvation freewheel, background decode
  thread, late-frame scheduling.
- **System integration** — system-wide media keys, single-instance file handoff, snapshots to
  the Desktop, settings that autosave as you change them.

<p align="center">
  <img src="examples/settings.png" alt="tivi settings — performance and subtitle options" width="860"/>
</p>

## Install

Grab **`tivi-Setup-<version>.exe`** from the [latest release](https://github.com/fezcode/tivi/releases/latest)
and run it — fully self-contained (FFmpeg, libass and raylib bundled), registers in
Add/Remove Programs, ships its own uninstaller. Silent install: `Setup.exe /S`.

Settings live in `%APPDATA%\tivi` and survive uninstall.

## Usage

- **Open**: drag files or folders onto the window, click the **`+`** button, or press `O`.
- **Subtitles**: drag a subtitle file onto the window, or CC panel › *Load subtitle file…*
- **Resize**: drag any window edge or corner. **Move**: drag the top bar.
- **Click** the video to play/pause, **double-click** for fullscreen, **right-click** for the
  context menu.

| Key | Action | Key | Action |
| --- | --- | --- | --- |
| Space | Play / pause | `F` / dbl-click | Fullscreen |
| ← / → | Seek −5 s / +5 s | `J` / `L` | Seek −10 s / +10 s |
| ↑ / ↓ | Volume up / down | `M` | Mute |
| `N` / `B` | Next / previous | `S` | Snapshot |
| `C` | Cycle subtitle track | `X` | Cycle audio track |
| `[` / `]` or `-` / `+` | Slower / faster | `\` | Reset speed |
| `Q` | Playlist panel | `A` | Adjustments panel |
| `G` | Settings panel | `T` | Always on top |
| `R` | Cycle repeat | Esc | Back / exit fullscreen |
| Media keys | Play/Pause, Stop, Prev, Next (system-wide) | | |

## Building

Dependencies (MSYS2 / MinGW-w64):

```powershell
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf `
          mingw-w64-x86_64-raylib mingw-w64-x86_64-ffmpeg mingw-w64-x86_64-libass
```

```powershell
.\build.ps1
.\build\tivi.exe "C:\path\to\movie.mkv"
```

| Script | Does |
| --- | --- |
| `build.ps1`     | Compiles + links `build\tivi.exe`. |
| `bundle.ps1`    | Stages a standalone `dist\win-x64\` (exe + full DLL closure — copy anywhere). |
| `installer.ps1` | Packages `dist\win-x64\` into a Windows `Setup.exe` via **Forge**. |

## Configuration

`config.ini` lives in `%APPDATA%\tivi\`. Settings autosave as you change them — a crash or
kill can't lose them. Persisted: volume, always-on-top, theme, subtitle settings, hardware
decode / GPU conversion toggles, auto-queue, pinned controls, keep-pitch, the five video
adjustments, and window geometry.

## Command line

```
tivi [files...]      Open and play (multiple files queue up)
tivi -h | --help     Show help
tivi -v | --version  Show version
tivi --probe FILE    Decode self-test: print stream info (diagnostic)
```

## Project layout

```
src/
  main.c        window, borderless chrome, UI, panels, input, drawing
  player.c      FFmpeg engine — demux/decode thread, D3D11VA, A/V sync, precise seek
  yuvtex.c      raw-GL Y/UV plane textures for shader color conversion (NV12/P010)
  audio_out.c   raylib AudioStream + lock-free PCM ring (audio clock = master)
  subs.c        libass subtitle rendering (embedded + external, style overrides)
  playlist.c    queue / index management
  osvideo.c     native dialogs, DWM corners/shadow, UTF-8 args, dir scan, console
  viconfig.c    persistent %APPDATA% settings
  mediakeys.c   system-wide media-key hotkeys
  singleinst.c  single-instance (forward files to the running window)
assets/tivi.ico embedded Windows executable icon (assets/tivi-icon.svg is the source)
```

## Credits

- **[raylib](https://www.raylib.com/)** — window, input, GPU rendering, audio output (zlib)
- **[FFmpeg](https://ffmpeg.org/)** (`libav*`) — demux / decode / filter / resample / scale (LGPL/GPL)
- **[libass](https://github.com/libass/libass)** — ASS/SSA/SRT subtitle rendering (ISC)
- Visual design after **Timp** by Şamil Bülbül.
- Screenshots show the [Sintel](https://durian.blender.org/) trailer — © Blender Foundation,
  [durian.blender.org](https://durian.blender.org/), CC-BY 3.0.

All code written for this project is released into the public domain.
