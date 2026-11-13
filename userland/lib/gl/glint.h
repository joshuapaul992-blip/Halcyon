/* Halcyon's 3D library: internal declarations (see docs/DESIGN-M11-3D.md).
 *
 * The pipeline: glVertex / vertex arrays collect raw vertices; vertex.c
 * transforms and lights them (gl_vertex: clip, eye and window coordinates,
 * colors, texture coordinates, fog); prim.c assembles primitives, clips
 * them against the frustum and the user planes, culls, and sets triangles
 * up (fixed-point edge functions, attribute planes); each set-up triangle is
 * queued with a snapshot of the per-pixel state and binned into the 64 x 64
 * tiles it touches; raster.c draws the queue tile by tile with SSE2 when the
 * frame is finished (or the queue is full, or pixels are read).
 *
 * Built for Halcyon (GL_HOST undefined: hal.h's C library) and for the host
 * tests (GL_HOST: the C library's headers), from the same files. */
#ifndef GLINT_H
#define GLINT_H
#ifdef GL_HOST
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "halmath.h"
#else
#include "hal.h"
#endif
/* the SSE2 intrinsics; xmmintrin.h would include the C library's stdlib.h
 * for _mm_malloc, which Halcyon programs do not have */
#define _MM_MALLOC_H_INCLUDED
#include <emmintrin.h>
#include "GL/gl.h"
#include "GL/hgl.h"

#define MAX_LIGHTS 8
#define MAX_UNITS 2
#define MAX_CLIP_PLANES 6
#define MODELVIEW_DEPTH 32
#define PROJECTION_DEPTH 4
#define TEXTURE_DEPTH 4
#define ATTRIB_DEPTH 16
#define MAX_TEX_LOG 11                     /* textures up to 2048 x 2048 */
#define MAX_LEVELS (MAX_TEX_LOG + 1)
#define MAX_FB 4096                        /* frame buffers and viewports up to 4096 x 4096 */
#define TILE_LOG 6
#define TILE (1 << TILE_LOG)               /* 64 x 64 pixel tiles */
#define MAX_LIST_NESTING 64

#define GL_MIN(a, b) ((a) < (b) ? (a) : (b))
#define GL_MAX(a, b) ((a) > (b) ? (a) : (b))
#define GL_CLAMPF(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* ---------------- matrices (column-major, as GL) ---------------- */
typedef struct glmat { float m[16]; int identity; } glmat;
void mat_identity(glmat *a);
void mat_mul(glmat *r, const glmat *a, const glmat *b);           /* r = a * b (r may be a or b) */
int  mat_invert(float out[16], const float m[16]);                 /* 0: singular */
void mat_xform4(float out[4], const glmat *a, const float v[4]);

/* ---------------- state groups (the glPushAttrib groups) ---------------- */
typedef struct gl_enables {
    GLboolean lighting, light[MAX_LIGHTS], color_material, normalize, rescale_normal, fog, depth_test,
              alpha_test, blend, cull_face, scissor_test, dither, offset_fill, offset_line, offset_point,
              tex2d[MAX_UNITS], tex1d[MAX_UNITS], texgen[MAX_UNITS][4], clip[MAX_CLIP_PLANES],
              line_smooth, point_smooth, polygon_smooth, line_stipple, polygon_stipple, stencil_test, logic_op;
} gl_enables;

typedef struct gl_current {
    float color[4], normal[3], tex[MAX_UNITS][4];
    GLboolean edge;
} gl_current;

typedef struct gl_light {
    float ambient[4], diffuse[4], specular[4];
    float pos[4];                          /* eye coordinates (transformed when set) */
    float spot_dir[3];                     /* eye coordinates, normalized */
    float spot_exp, spot_cutoff, spot_cos;
    float att[3];                          /* constant, linear, quadratic */
} gl_light;

typedef struct gl_material { float ambient[4], diffuse[4], specular[4], emission[4], shininess; } gl_material;

typedef struct gl_lighting {
    gl_light light[MAX_LIGHTS];
    gl_material mat[2];                    /* front, back */
    float model_ambient[4];
    GLboolean local_viewer, two_side;
    GLenum color_control;                  /* GL_SINGLE_COLOR / GL_SEPARATE_SPECULAR_COLOR */
    GLenum cm_face, cm_mode;               /* glColorMaterial */
    GLenum shade_model;
} gl_lighting;

typedef struct gl_fog { GLenum mode; float density, start, end, color[4]; } gl_fog;

typedef struct gl_texunit {
    GLuint bound;                          /* the texture object's name (0: the default one) */
    GLenum env_mode;
    float env_color[4];
    GLenum gen_mode[4];                    /* S T R Q */
    float obj_plane[4][4], eye_plane[4][4];
} gl_texunit;

typedef struct gl_texstate { gl_texunit unit[MAX_UNITS]; int active; } gl_texstate;

typedef struct gl_colorbuf {
    GLenum alpha_func; float alpha_ref;
    GLenum blend_src, blend_dst;
    float clear[4];
    GLboolean mask[4];
} gl_colorbuf;

typedef struct gl_depthbuf { GLenum func; GLboolean mask; float clear; } gl_depthbuf;
typedef struct gl_polygon { GLenum cull, front, mode[2]; float offset_factor, offset_units; } gl_polygon;
typedef struct gl_viewport { int x, y, w, h; float znear, zfar; } gl_viewport;
typedef struct gl_scissor { int x, y, w, h; } gl_scissor;
typedef struct gl_transform { GLenum matrix_mode; float clip_plane[MAX_CLIP_PLANES][4]; } gl_transform;
typedef struct gl_hints { GLenum persp, point, line, polygon, fog; } gl_hints;

typedef struct gl_attrib_save {
    GLbitfield mask;
    gl_enables en;
    gl_current cur;
    gl_lighting lt;
    gl_fog fog;
    gl_texstate tx;
    gl_colorbuf cb;
    gl_depthbuf db;
    gl_polygon poly;
    gl_viewport vp;
    gl_scissor sc;
    gl_transform xf;
    gl_hints hints;
    float line_width, point_size;
    GLuint list_base;
} gl_attrib_save;

/* ---------------- texture objects ---------------- */
enum { BASE_ALPHA = 1, BASE_LUMINANCE, BASE_LUMINANCE_ALPHA, BASE_INTENSITY, BASE_RGB, BASE_RGBA };
typedef struct gl_texture {
    GLuint name;
    int base;                              /* BASE_* of level 0 */
    GLenum internal;                       /* as given */
    uint32_t *level[MAX_LEVELS];           /* 0xAARRGGBB, rows top to bottom as given (t = 0 first) */
    int lw[MAX_LEVELS], lh[MAX_LEVELS], lbase[MAX_LEVELS];
    GLenum min_filter, mag_filter, wrap_s, wrap_t;
    float border[4], priority;
    int max_level;
} gl_texture;

/* ---------------- vertices ---------------- */
typedef struct gl_rawvert {                /* as given (glVertex, arrays) */
    float obj[4], col[4], nrm[3], tex[MAX_UNITS][4];
    GLboolean edge;
} gl_rawvert;

/* a processed vertex; the floats up to GLV_LERP are interpolated by clipping */
typedef struct gl_vertex {
    float clip[4];
    float eye[4];
    float col[2][4];                       /* front, back (0..1, clamped) */
    float spec[2][4];
    float tex[MAX_UNITS][4];
    float fog;                             /* fog factor: 1 no fog .. 0 all fog */
    float win[4];                          /* x, y (pixels, y down), z (0..1), 1/w */
    unsigned clipmask;
    unsigned flags;                        /* GLV_WIN: win[] computed */
    GLboolean edge;
} gl_vertex;
#define GLV_LERP (offsetof(gl_vertex, win) / sizeof(float))
#define GLV_WIN 1

/* ---------------- the tile renderer's queue ---------------- */
/* per-pixel state, copied when it changes: queued triangles point to one */
typedef struct gl_rs_unit {
    const uint32_t *level[MAX_LEVELS];
    int wlog[MAX_LEVELS], hlog[MAX_LEVELS];
    int nlevels;                           /* usable levels (1: no mipmaps) */
    int min_linear, mag_linear, mip;       /* mip: 0 none, 1 nearest level, 2 between levels */
    int clamp_s, clamp_t;
    int env;                               /* GL_MODULATE ... */
    int keep_rgb, keep_a, intensity;       /* the texture has no color / no alpha / is intensity */
    float w0, h0;                          /* level 0 size */
    uint16_t env_color[8];                 /* B G R A, twice */
} gl_rs_unit;

enum {
    RS_DEPTH_TEST = 0x01, RS_DEPTH_WRITE = 0x02, RS_ALPHA_TEST = 0x04, RS_BLEND = 0x08, RS_FOG = 0x10,
    RS_TEX0 = 0x20, RS_TEX1 = 0x40, RS_SPECULAR = 0x80, RS_COLOR_MASK = 0x100, RS_NO_COLOR = 0x200,
};
typedef struct gl_rstate {
    unsigned flags;
    GLenum depth_func, alpha_func, blend_src, blend_dst;
    float alpha_ref255;
    uint32_t color_mask;                   /* bytes of a pixel that are written */
    uint16_t fog_color[8];
    gl_rs_unit unit[MAX_UNITS];
} gl_rstate;

/* interpolated attributes: value at the reference point, d/dx, d/dy */
enum { A_Z, A_Q, A_R, A_G, A_B, A_A, A_SR, A_SG, A_SB, A_S0, A_T0, A_Q0, A_S1, A_T1, A_Q1, A_FOG, NATTR };
enum {
    TF_CLEAR = 0x01,                       /* a clear command, not a triangle */
    TF_SMOOTH = 0x02,                      /* the colors vary (else p[A_R..A_A][0] is the color) */
    TF_SPEC_SMOOTH = 0x04,
    TF_TEXQ0 = 0x08, TF_TEXQ1 = 0x10,       /* the unit's q is not 1: own perspective divide */
};
typedef struct gl_tri {
    const gl_rstate *rs;
    unsigned flags;
    int minx, miny, maxx, maxy;            /* pixels (max exclusive), inside the clip rectangle */
    int a[3], b[3];                        /* edge functions E = a X + b Y + c (1/16 pixel units) */
    int64_t c[3];                          /* (the fill rule's bias included) */
    float x0, y0;                          /* attribute reference point */
    float p[NATTR][3];
} gl_tri;
/* a clear: minx..maxy the area, a[0] the color, a[1] the GL_*_BUFFER_BITs,
 * a[2] the color mask, p[0][0] the depth */

typedef struct gl_bin { uint32_t *idx; int n, cap; } gl_bin;

/* ---------------- display lists ---------------- */
typedef union gl_dlarg { float f; int i; unsigned u; } gl_dlarg;
typedef struct gl_list { GLuint name; gl_dlarg *ops; int n, cap; } gl_list;

/* ---------------- client arrays ---------------- */
typedef struct gl_array { GLint size; GLenum type; GLsizei stride; const void *ptr; GLboolean enabled; } gl_array;
typedef struct gl_client {
    gl_array vertex, normal, color, tex[MAX_UNITS], edge;
    int active;
    int unpack_align, unpack_row, unpack_skip_rows, unpack_skip_px;
    int pack_align, pack_row, pack_skip_rows, pack_skip_px;
} gl_client;

/* ---------------- the context ---------------- */
struct gl_context {
    /* frame buffer */
    unsigned *color;
    int w, h, stride;
    int own_color;                         /* color was allocated here */
    float *depth;                          /* rows of dstride floats (a multiple of 4) */
    int dstride, depth_w, depth_h;
    int no_depth;
    int win;                               /* the Halcyon window, or -1 */

    GLenum error;
    gl_enables en;
    gl_current cur;
    gl_lighting lt;
    gl_fog fog;
    gl_texstate tx;
    gl_colorbuf cb;
    gl_depthbuf db;
    gl_polygon poly;
    gl_viewport vp;
    gl_scissor sc;
    gl_transform xf;
    gl_hints hints;
    float line_width, point_size;
    gl_client cl;

    /* matrices */
    glmat mv[MODELVIEW_DEPTH], proj[PROJECTION_DEPTH], texm[MAX_UNITS][TEXTURE_DEPTH];
    int mv_top, proj_top, tex_top[MAX_UNITS];
    glmat mvp;                             /* proj * modelview */
    float nmat[9];                         /* the normal matrix (inverse transpose of the modelview's 3 x 3) */
    float nscale;                          /* GL_RESCALE_NORMAL's factor */
    int mvp_dirty, nmat_dirty;

    gl_attrib_save *attrib;                /* glPushAttrib stack */
    int attrib_top;
    gl_client *client_attrib;
    GLbitfield client_attrib_mask[ATTRIB_DEPTH];
    int client_attrib_top;

    /* immediate mode */
    GLenum prim;
    int in_begin;
    gl_rawvert *raw;
    int nraw, raw_cap;
    gl_vertex *vtx;                        /* processed vertices of the batch */
    int vtx_cap;

    /* textures: objects by name (0: the default one) */
    gl_texture tex0[MAX_UNITS];
    gl_texture **textures;
    int ntextures;                         /* entries in textures[] */
    GLuint next_texture;

    /* display lists */
    gl_list **lists;
    int nlists;
    gl_list *compiling;
    GLenum list_mode;
    int list_depth;                        /* > 0 while lists execute */
    GLuint list_base;

    /* the tile renderer */
    gl_rstate *rs_cur;                     /* the snapshot in use (NULL: state changed) */
    gl_rstate *rstates;
    int nrs, rs_cap;
    gl_tri *tris;
    int ntris, tri_cap;
    gl_bin *bins;
    int tiles_x, tiles_y;
    int clip_x0, clip_y0, clip_x1, clip_y1; /* scissor and frame buffer (pixels, y down) */
    int frame_dirty;                       /* something was queued or drawn since the last swap */

    /* statistics */
    unsigned frames;
    uint64_t fps_t0;
    unsigned fps_frames;
    float fps;
    unsigned long long tris_drawn, tris_frame;
};
typedef struct gl_context gl_context;

extern gl_context *gl_cur;

/* context.c */
void gl_error(gl_context *c, GLenum e);
gl_context *gl_context_new(unsigned *color, int w, int h, int stride, unsigned flags);
void gl_context_free(gl_context *c);
int  gl_set_framebuffer(gl_context *c, unsigned *color, int w, int h, int stride);
void gl_state_changed(gl_context *c);      /* per-pixel state: a new snapshot for the next triangle */
void gl_update_clip_rect(gl_context *c);
uint64_t gl_time_ms(void);

/* matrix.c */
glmat *gl_top_matrix(gl_context *c);
void gl_update_matrices(gl_context *c);

/* vertex.c */
void gl_flush_prims(gl_context *c);        /* the vertices of glBegin so far (glEnd) */
void gl_process(gl_context *c, const gl_rawvert *in, int n, gl_vertex *out);
void gl_vertex4(gl_context *c, float x, float y, float z, float w);
void gl_color4(gl_context *c, float r, float g, float b, float a);
void gl_normal3(gl_context *c, float x, float y, float z);
void gl_texcoord4(gl_context *c, int unit, float s, float t, float r, float q);
int  gl_reserve_vtx(gl_context *c, int n);

/* prim.c */
void gl_draw(gl_context *c, GLenum mode, gl_vertex *v, const unsigned *idx, int n);
void gl_compute_win(gl_context *c, gl_vertex *v);

/* raster.c */
void gl_raster_init(gl_context *c);
void gl_raster_free(gl_context *c);
int  gl_raster_resize(gl_context *c);      /* the frame buffer's size changed */
void gl_render(gl_context *c);             /* draw everything queued */
gl_tri *gl_queue_tri(gl_context *c);       /* a new record, or NULL (after a flush, again) */
void gl_bin_tri(gl_context *c, gl_tri *t);
const gl_rstate *gl_get_rstate(gl_context *c);
void gl_queue_clear(gl_context *c, GLbitfield mask);

/* texture.c */
gl_texture *gl_bound_texture(gl_context *c, int unit);
void gl_build_rs_unit(gl_context *c, int unit, gl_rs_unit *u);   /* 0 in u->nlevels: incomplete */
void gl_textures_free(gl_context *c);

/* list.c */
enum {
    OP_BEGIN = 1, OP_END, OP_VERTEX, OP_COLOR, OP_NORMAL, OP_TEXCOORD, OP_MATERIAL, OP_LIGHT, OP_LIGHT_MODEL,
    OP_COLOR_MATERIAL, OP_MATRIX_MODE, OP_LOAD_IDENTITY, OP_LOAD_MATRIX, OP_MULT_MATRIX, OP_PUSH_MATRIX,
    OP_POP_MATRIX, OP_ROTATE, OP_TRANSLATE, OP_SCALE, OP_FRUSTUM, OP_ORTHO, OP_ENABLE, OP_DISABLE,
    OP_BIND_TEXTURE, OP_TEX_ENV, OP_TEX_PARAMETER, OP_SHADE_MODEL, OP_BLEND_FUNC, OP_ALPHA_FUNC,
    OP_DEPTH_FUNC, OP_DEPTH_MASK, OP_COLOR_MASK, OP_CULL_FACE, OP_FRONT_FACE, OP_FOG, OP_CALL_LIST,
    OP_ACTIVE_TEXTURE, OP_POLYGON_MODE, OP_POLYGON_OFFSET, OP_LINE_WIDTH, OP_POINT_SIZE, OP_TEX_GEN,
    OP_CLEAR, OP_CLEAR_COLOR, OP_CLEAR_DEPTH, OP_VIEWPORT, OP_SCISSOR, OP_HINT, OP_PUSH_ATTRIB,
    OP_POP_ATTRIB, OP_LIST_BASE, OP_EDGE_FLAG, OP_DEPTH_RANGE, OP_CLIP_PLANE,
};
/* record a command while a list is compiled; returns 1 if it must not run now (GL_COMPILE) */
int  gl_dl_record(gl_context *c, int op, int n, const gl_dlarg *args);
void gl_lists_free(gl_context *c);
#define DL_ACTIVE(c) ((c)->compiling && !(c)->list_depth)

/* hgl.c / the host's binding */
void gl_frame_done(gl_context *c);

/* the current context, or return (GL calls without one do nothing) */
#define GET_CTX(c) gl_context *c = gl_cur; if (!c) return
#define GET_CTX_R(c, r) gl_context *c = gl_cur; if (!c) return r
/* GL calls that are not allowed between glBegin and glEnd */
#define NOT_IN_BEGIN(c) do { if ((c)->in_begin) { gl_error(c, GL_INVALID_OPERATION); return; } } while (0)

#endif
