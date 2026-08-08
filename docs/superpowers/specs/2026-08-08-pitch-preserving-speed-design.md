# Pitch-preserving playback speed

**Date:** 2026-08-08
**Status:** Implemented and verified

## Problem

Speed was done by declaring the resampler's input rate as `real_rate × speed`,
which shifts pitch with speed (chipmunk at 1.5×). The user wants VLC-style
speed-up that keeps pitch.

## Design

- New option **"Keep pitch (time-stretch)"** under the Playback speed slider,
  persisted as `pitch_correct` (default **on**).
- When on and speed ≠ 1: decoded audio runs through an avfilter graph
  `abuffer → atempo=<speed> → aformat(flt, stereo, AO_RATE) → abuffersink`
  (two chained `atempo=sqrt(speed)` below 0.5×). Sink frames go straight to
  the PCM ring — no second resampler. `swr` stays at ratio 1.0 for probing.
- When off (or speed = 1): the classic resample path, unchanged.
- Either way one output second covers `speed` media seconds, so the audio
  clock (`audio_out_set_speed`) is identical. pts tracked on the input side;
  atempo's small internal window (~tens of ms) is an accepted constant offset.
- Chain rebuilds on the decode thread when speed or the toggle changes, and
  after seeks (`chain_speed = -1` forces it, dropping stale filter state).
  Graph failure falls back to the resample path silently.

## Bugs found on the way (fixed)

1. **Heap corruption via format negotiation**: without the trailing `aformat`,
   the graph negotiates atempo's format freely, so the sink can emit a
   different layout than the decoder's (e.g. packed flt vs planar fltp).
   Feeding that to a converter configured for the decoder's layout reads wild
   plane pointers → heap corruption → SIGSEGV in unrelated threads, seconds of
   UI stalls before death. Rule: **always pin an avfilter sink's output format**.
2. **Clock freeze starvation**: the audio-master clock froze whenever the PCM
   ring drained (atempo's bootstrap window right after a rebuild). Frozen clock
   → pacer stops → video queue fills → the single decode thread blocks on a
   video slot before it can refill audio: permanent stall. Fixed in
   `player_update`: the audio clock is master only while the ring has samples;
   when starved the clock freewheels at play speed and snaps back on refill.
   (Also protects against audio-device underruns generally.)

## Verification (TIVI_SPEEDTEST=<x> dev hook sets speed at frame 120)

- 2× pitch-on: `atempo fed 1127424 out 562176` (exactly ½ — stretch active),
  UI 60 fps, video ~47 fps, pacing 1.02 fr/ev — identical health to pitch-off.
- 2× pitch-off (classic path through the refactor): unchanged, healthy.
- TIVI_SEEKTEST at 1.5× with pitch on: ±5 s seeks land precisely, chain
  rebuilds after each seek, pacing stays ~1.1 fr/ev.
