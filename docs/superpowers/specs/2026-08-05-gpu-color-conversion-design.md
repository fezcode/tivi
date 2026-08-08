# GPU color-conversion for tivi

**Date:** 2026-08-05
**Status:** Approved, implementing

## Problem

After moving HEVC/H.264 decode to the GPU (D3D11VA), the remaining per-frame CPU
cost is the `sws_scale` YUV→RGBA color conversion in `decode_video` (`player.c`),
run on the decode thread for every frame. For 1080p 10-bit content this is a
meaningful secondary cost, plus it uploads 4 bytes/px of RGBA to the GPU each
frame. Move the conversion into the fragment shader that already runs every frame.

## Approach — two paths (fast + compat)

The fragment shader in `main.c` (`FS_ADJUST`) already samples the video texture
and applies brightness/contrast/saturation/hue/gamma. Extend it to do YUV→RGB
first, for the common hardware-decoded formats. Keep the CPU path as a fallback.

| Path | Trigger | Behavior |
|------|---------|----------|
| Fast (GPU) | frame is `NV12` (8-bit) or `P010` (10-bit) | upload Y + UV planes, shader converts |
| Compat (CPU) | any other format (yuv420p, 12-bit, 4:2:2/4:4:4, …) | existing `sws_scale`→RGBA, unchanged |

Hardware-decoded HEVC/H.264 always reads back as NV12 (8-bit) or P010 (10-bit),
so real target content uses the fast path. The compat path guarantees no
regression for software-only or exotic formats.

## Player side (`player.c` / `player.h`)

- Frame-pool machinery (free list, queue, `disp_buf` hold-for-display) unchanged;
  only slot *contents* change from RGBA to packed planar YUV. Slot size becomes
  `w*h*4` (covers RGBA=4, P010=3, NV12=1.5 bytes/px).
- `decode_video`: per frame, if the (readback or software) frame is NV12/P010,
  `memcpy` Y and UV planes tightly-packed into the slot and tag it; otherwise run
  the existing `sws_scale`→RGBA and tag `RGBA`.
- New public type + accessor:
  ```c
  typedef enum { TIVI_PIX_RGBA, TIVI_PIX_NV12, TIVI_PIX_P010 } TiviPixFmt;
  typedef struct {
      TiviPixFmt fmt;
      int w, h;                 // luma dimensions
      const uint8_t *plane[2];  // [0]=Y (or RGBA), [1]=UV
      int stride[2];
      int colorspace;           // 0=BT601, 1=BT709
      int full_range;           // 0=limited(16-235), 1=full
      int bitdepth;             // 8 or 10
  } TiviFrame;
  bool player_frame2(Player *p, TiviFrame *out, bool *changed);
  ```
  colorspace/full_range/bitdepth read once from the decoder, constant per stream;
  `fmt` stored per queued frame (handles rare mixed fast/compat).
- Snapshot: `player_snapshot_rgba()` lazily `sws`-converts the current display
  frame on demand (rare op → CPU convert is fine, keeps behavior identical).

## Render side (`main.c` + new `yuvtex.c`/`yuvtex.h`)

- New focused module `yuvtex` manages Y and UV plane textures via **raw OpenGL**
  (`glGenTextures`/`glTexImage2D`/`glTexSubImage2D`) — raylib's `PixelFormat`
  enum has no 2-channel 16-bit format for P010. GL internal formats: `R8`/`RG8`
  (8-bit), `R16`/`RG16` (10-bit). Textures wrapped in raylib `Texture2D` structs
  so `DrawTexturePro` / `SetShaderValueTexture` still apply. All `gl*` calls used
  are GL 1.x, already exported by the linked `opengl32`. `glPixelStorei(GL_UNPACK_ALIGNMENT,1)`.
- Fast path: `glTexSubImage2D` upload Y + UV on frame change, draw with shader.
  Compat path: existing `UpdateTexture(vtex, rgba)`, untouched.

## Shader (extend `FS_ADJUST`)

Add chroma sampler + `u_convert` flag + CPU-computed affine color transform:

```glsl
if (u_convert == 1) {
    float y  = texture(texture0, uv).r;
    vec2  cc = texture(u_texUV,  uv).rg;   // Cb, Cr
    c = u_yuv2rgb * vec3(y, cc) + u_yuvoff;
} else {
    c = texture(texture0, uv).rgb;
}
// existing gamma/hue/contrast/brightness/saturation block runs unchanged on c
```

`u_yuv2rgb` (mat3) + `u_yuvoff` (vec3) computed on CPU from
`{colorspace, full_range, bitdepth}` — bakes in BT.601 vs BT.709, limited vs full
range, and P010 10-bit-in-16-bit normalization (sample = code10<<6 / 65535).
Shader stays bit-depth-agnostic. BT.709 for HD, BT.601 for SD (from frame color
metadata; height ≥ 720 → 709 as fallback).

## Error handling / fallback

- GL plane-texture creation failure → frame routed to compat (CPU) path.
- Unknown/unsupported pixel format → compat path. No hard failures.

## Testing / verification

- **Color regression (key gate):** snapshot the same frame old-CPU vs new-GPU;
  pixel diff within rounding (±1–2/255).
- 10-bit HEVC (Pantheon) + 8-bit H.264 (`test_clip.mp4`): correct colors; confirm
  `sws_scale` no longer on the hot path.
- Subtitles (text + bitmap), snapshot, adjustments panel, seek, resize,
  track-switch: still correct.

## Out of scope (YAGNI)

- GPU conversion of 3-plane software formats (yuv420p) — compat CPU path instead.
- Zero-copy D3D11↔GL interop (skip readback) — bigger/riskier, not justified yet.
- SAR/anamorphic and crop handling — matches current behavior (ignored).

---

# Addendum A — GPU / hardware-acceleration options

User-facing controls for the acceleration features, in the Settings panel and
`config.ini`.

## Config (`viconfig`)
```c
bool hw_decode;     // true = auto D3D11VA GPU decode (+fallback); false = force software.  default true
bool gpu_convert;   // true = shader YUV->RGB; false = force CPU sws_scale.                  default true
```

## Player API
```c
void        player_set_hw_decode(Player*, bool);    // read by try_enable_hw at open time
void        player_set_gpu_convert(Player*, bool);  // routes frames to compat path when false
bool        player_hw_decode_active(const Player*); // GPU decode currently live?
const char *player_decode_desc(const Player*);      // "D3D11VA" | "software"
const char *player_convert_desc(const Player*);     // "shader (p010le)" | "CPU (sws)"
```
- `hw_decode` gates `try_enable_hw`; changing it takes effect on next file open (re-open to apply).
- `gpu_convert=false` forces every frame down the compat (RGBA) path.

## Settings panel (`main.c`, `PANEL_SETTINGS`)
New "Performance" section: Hardware decoding [Auto｜Off], GPU color conversion
[On｜Off], and a read-only "Active:" line via the `*_desc` accessors.

# Addendum B — Subtitle font options (plain-text subs only)

Style SRT/VTT and other unstyled subs via libass; **respect authored ASS/SSA**.

## Config (`viconfig`)
```c
char     sub_font[64];       // family, default "sans-serif"
float    sub_font_scale;     // 0.25..2.5, default 1.0
unsigned sub_color;          // 0xRRGGBB fill,    default 0xFFFFFF
unsigned sub_outline_color;  // 0xRRGGBB outline, default 0x000000
float    sub_outline;        // 0..4 px, default 2.0
float    sub_shadow;         // 0..4 px, default 0.0
```

## Mechanism (`subs.c`)
libass **selective style override**, enabled only for plain-text tracks:
```c
ASS_Style ov = {0};
ov.FontName = cfg.sub_font;                 // non-NULL required
ov.FontSize = frame_h * 0.05f * scale;      // ~5% of video height at scale 1.0
ov.PrimaryColour = ass_abgr(sub_color);     // libass = &HAABBGGRR (00 alpha = opaque)
ov.OutlineColour = ass_abgr(sub_outline_color);
ov.Outline = sub_outline; ov.Shadow = sub_shadow; ov.BorderStyle = 1;
ass_set_selective_style_override(renderer, &ov);
ass_set_selective_style_override_enabled(renderer,
    is_plaintext ? (BIT_FONT_NAME|BIT_FONT_SIZE|BIT_COLORS|BIT_BORDER) : 0);
```
- Track type (plain-text vs ASS-styled) is tracked when the track is created:
  embedded ASS / external `.ass`/`.ssa` = styled; SRT/VTT/embedded subrip = plain.
- New API `void subs_set_style(Subs*, const SubStyle*)`; re-applied on config change
  and track change. Exact `ASS_OVERRIDE_BIT_*` names verified against installed
  libass header during implementation.

## Settings panel
New "Subtitles" section: Font (family), Size (scale ± ), Text color, Outline
(px + color), Shadow — persisted to `config.ini` and applied live.

# Addendum C — A/V pacing fix (the real "5 fps" root cause)

Profiling (TIVI_PERF=1 probes, left in the code) showed the perceived lag was
never decode: the decoder produced a perfect 24 fps but only ~11 unique frames/s
reached the screen. `push_audio` capped the audio ring at 0.5 s, freezing the
demuxer whenever audio ran ahead — and WEB-DL MKVs interleave audio packets up
to ~0.5 s AHEAD of the matching video packets, so video frames were demuxed at
their deadline, arrived late in bursts, and the display pacer dropped about
half. No video-side option could affect this.

Fix (`player.c push_audio`): while the video queue is low, relax the audio cap
from 0.5 s to 2 s so the demuxer can reach the video packets (`v_live` excludes
cover-art streams; audio-only files keep the prompt 0.5 s cap for speed
changes). Verified: 23.976 fps file → 24 fps displayed, 1.00 frames per pacer
advance, video queue steady at ~6.5/8.

# Addendum D — Precise seeking (forward arrow "did nothing")

`do_seek` used `av_seek_frame(..., AVSEEK_FLAG_BACKWARD)` alone, which lands on
the keyframe AT/BEFORE the target — and the first decoded audio then reset the
master clock to the keyframe. x265 WEB-DLs space keyframes 10–21 s apart, so
"+5 s" usually resolved to the keyframe already behind the playhead: playback
snapped back to (or before) where it was. "−5 s" always found an earlier
keyframe, so it appeared to work (landing early).

Fix: preroll-based precise seek (`preroll_to` in Player). Seek to the keyframe,
then decode-and-discard: video frames with pts < target − 1 frame are dropped
BEFORE the GPU readback/pack (preroll burns at pure decode speed); audio frames
ending before the target are skipped before the resampler (this is what stops
the clock snap-back). Verified via `TIVI_SEEKTEST=1` scripted seeks: +5 from
5.03 → landed 10.17; +5 from 13.64 → 19.02; −5 from 22.48 → 17.89 (exact,
previously would land at keyframe 10.4). Cost: a forward seek decodes up to one
GOP of frames before showing the target (~0.5–1 s with GPU decode).

# Addendum E — VLC-quality subtitle rendering

The libass overlay was rasterized at VIDEO resolution then bilinearly stretched
to the window — soft glyph edges (the "bad AA"). Now `subs_set_size` is called
per frame with the on-screen video rect (early-out when unchanged), so glyphs
rasterize at display resolution like VLC. Plain-text subs also default to Bold
(`ASS_OVERRIDE_BIT_ATTRIBUTES`), matching VLC's look. Style units fixed: ASS
FontSize/Outline/Shadow are script units (PlayResY=288 for FFmpeg-converted
subs), not frame pixels — base size 16 @288 (FFmpeg's own default, ≈5.5% of
picture height), scaled by the user's Size slider.
