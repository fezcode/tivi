#ifndef TIVI_YUVTEX_H
#define TIVI_YUVTEX_H

#include <stdbool.h>
#include "raylib.h"
#include "player.h"   // TiviFrame / TiviPixFmt

// GPU-side YUV plane textures for the shader color-conversion path. Holds a luma
// (Y) and interleaved chroma (UV) texture, uploaded from a packed planar frame.
// Managed with raw OpenGL because raylib's PixelFormat enum has no 2-channel
// 16-bit format for P010; the GL texture ids are wrapped in raylib Texture2D so
// DrawTexturePro / SetShaderValueTexture still apply.

typedef struct {
    Texture2D  y, uv;     // plane textures (raw-GL ids wrapped for raylib)
    int        w, h;      // luma dimensions currently allocated
    TiviPixFmt fmt;       // layout currently allocated (NV12 / P010)
    bool       valid;
} YuvTex;

// (Re)create the plane textures if geometry/format changed, then upload the
// frame's Y + UV planes. Returns false for a non-planar (RGBA) frame or on GL
// failure — the caller should use its RGBA/CPU path instead.
bool yuvtex_update(YuvTex *t, const TiviFrame *f);
void yuvtex_destroy(YuvTex *t);

// Affine YUV->RGB transform for the shader: rgb = c0*y + c1*u + c2*v + off,
// where y/u/v are the raw texture samples. Encodes BT.601/709, limited/full
// range, and the P010 10-bit-in-16-bit normalization.
typedef struct { Vector3 c0, c1, c2, off; } YuvXfm;
YuvXfm yuvtex_transform(const TiviFrame *f);

#endif
