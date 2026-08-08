#include "yuvtex.h"
#include <string.h>

// --- minimal OpenGL 1.x prototypes (exported by opengl32; declared here so we
// --- don't drag in <GL/gl.h>, which clashes with raylib.h under windows.h) ---
typedef unsigned int GLenum;   typedef int GLint;   typedef int GLsizei;
typedef unsigned int GLuint;   typedef float GLfloat;
#ifdef _WIN32
#define GLCALL __stdcall
#else
#define GLCALL
#endif
extern void GLCALL glGenTextures(GLsizei, GLuint *);
extern void GLCALL glDeleteTextures(GLsizei, const GLuint *);
extern void GLCALL glBindTexture(GLenum, GLuint);
extern void GLCALL glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
extern void GLCALL glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
extern void GLCALL glTexParameteri(GLenum, GLenum, GLint);
extern void GLCALL glPixelStorei(GLenum, GLint);

#define GL_TEXTURE_2D        0x0DE1
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S    0x2802
#define GL_TEXTURE_WRAP_T    0x2803
#define GL_LINEAR            0x2601
#define GL_CLAMP_TO_EDGE     0x812F
#define GL_UNPACK_ALIGNMENT  0x0CF5
#define GL_UNSIGNED_BYTE     0x1401
#define GL_UNSIGNED_SHORT    0x1403
#define GL_RED               0x1903
#define GL_RG                0x8227
#define GL_R8                0x8229
#define GL_R16               0x822A
#define GL_RG8               0x822B
#define GL_RG16              0x822C

static Texture2D make_tex(int w, int h, GLint internal, GLenum fmt, GLenum type) {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, fmt, type, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    Texture2D t = {0};
    t.id = id; t.width = w; t.height = h; t.mipmaps = 1;
    t.format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE;   // unused (we manage the GL id ourselves)
    return t;
}

void yuvtex_destroy(YuvTex *t) {
    if (t->y.id)  { GLuint id = t->y.id;  glDeleteTextures(1, &id); }
    if (t->uv.id) { GLuint id = t->uv.id; glDeleteTextures(1, &id); }
    memset(t, 0, sizeof(*t));
}

bool yuvtex_update(YuvTex *t, const TiviFrame *f) {
    if (f->fmt == TIVI_PIX_RGBA || !f->plane[0] || !f->plane[1]) return false;
    bool p10 = (f->fmt == TIVI_PIX_P010);
    int  cw = (f->w + 1) / 2, ch = (f->h + 1) / 2;
    GLenum type = p10 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;

    if (!t->valid || t->w != f->w || t->h != f->h || t->fmt != f->fmt) {
        yuvtex_destroy(t);
        t->y  = make_tex(f->w, f->h, p10 ? GL_R16  : GL_R8,  GL_RED, type);
        t->uv = make_tex(cw,   ch,   p10 ? GL_RG16 : GL_RG8, GL_RG,  type);
        if (!t->y.id || !t->uv.id) { yuvtex_destroy(t); return false; }
        t->w = f->w; t->h = f->h; t->fmt = f->fmt; t->valid = true;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);                    // tightly packed rows
    glBindTexture(GL_TEXTURE_2D, t->y.id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->w, f->h, GL_RED, type, f->plane[0]);
    glBindTexture(GL_TEXTURE_2D, t->uv.id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cw, ch, GL_RG, type, f->plane[1]);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

YuvXfm yuvtex_transform(const TiviFrame *f) {
    // Luma/chroma sample -> normalized Y'CbCr:  Y' = ay*sy + by,  C = ac*sc + bc.
    double ay, by, ac, bc;
    if (f->bitdepth >= 10) {
        const double k = 65535.0 / 64.0;         // P010: sample = (code10<<6)/65535
        if (f->full_range) { ay = k / 1023.0; by = 0.0;            ac = k / 1023.0; bc = -512.0 / 1023.0; }
        else               { ay = k / 876.0;  by = -64.0 / 876.0;  ac = k / 896.0;  bc = -512.0 / 896.0;  }
    } else {
        if (f->full_range) { ay = 1.0;         by = 0.0;           ac = 1.0;        bc = -0.5;            }
        else               { ay = 255.0/219.0; by = -16.0/219.0;   ac = 255.0/224.0; bc = -128.0/224.0;   }
    }
    // Y'CbCr -> RGB coefficients.
    double kr, kgb, kgr, kb;
    if (f->colorspace == 1) { kr = 1.5748; kgb = -0.1873;   kgr = -0.4681;   kb = 1.8556; }  // BT.709
    else                    { kr = 1.402;  kgb = -0.344136; kgr = -0.714136; kb = 1.772;  }  // BT.601

    YuvXfm x;
    x.c0  = (Vector3){ (float)ay,       (float)ay,             (float)ay        };
    x.c1  = (Vector3){ 0.0f,            (float)(kgb * ac),     (float)(kb * ac) };
    x.c2  = (Vector3){ (float)(kr*ac),  (float)(kgr * ac),     0.0f             };
    x.off = (Vector3){ (float)(by + kr*bc), (float)(by + kgb*bc + kgr*bc), (float)(by + kb*bc) };
    return x;
}
