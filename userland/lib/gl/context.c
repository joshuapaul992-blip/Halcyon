/* Halcyon's 3D library: contexts and their state. Creation (on caller's
 * pixels or on memory of its own), the frame buffer and the depth buffer,
 * defaults as OpenGL 1.3 gives them, errors, strings, glEnable / glDisable,
 * the glGet queries, hints, pixel storage and the attribute stacks. */
#include "glint.h"
#ifdef GL_HOST
#include <time.h>
#endif

gl_context *gl_cur;

uint64_t gl_time_ms(void) {
#ifdef GL_HOST
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
#else
    return time_ms();
#endif
}

void gl_error(gl_context *c, GLenum e) {
    if (c->error == GL_NO_ERROR) c->error = e;      /* the first one is kept until glGetError */
}

GLenum glGetError(void) {
    GET_CTX_R(c, GL_NO_ERROR);
    GLenum e = c->error;
    c->error = GL_NO_ERROR;
    return e;
}

const GLubyte *glGetString(GLenum name) {
    GET_CTX_R(c, NULL);
    switch (name) {
    case GL_VENDOR: return (const GLubyte *)"Halcyon";
    case GL_RENDERER: return (const GLubyte *)"Halcyon software renderer (SSE2, 64x64 tiles)";
    case GL_VERSION: return (const GLubyte *)"1.3 Halcyon";
    case GL_EXTENSIONS:
        return (const GLubyte *)"GL_ARB_multitexture GL_ARB_texture_env_add GL_EXT_texture_env_add GL_EXT_bgra "
                                "GL_EXT_texture_edge_clamp GL_SGIS_texture_edge_clamp";
    }
    gl_error(c, GL_INVALID_ENUM);
    return NULL;
}

/* ---------------- defaults ---------------- */
static void set4(float *d, float a, float b, float cc, float e) { d[0] = a; d[1] = b; d[2] = cc; d[3] = e; }

static void defaults(gl_context *c) {
    memset(&c->en, 0, sizeof c->en);
    c->en.dither = GL_TRUE;
    set4(c->cur.color, 1, 1, 1, 1);
    c->cur.normal[0] = 0; c->cur.normal[1] = 0; c->cur.normal[2] = 1;
    for (int u = 0; u < MAX_UNITS; u++) set4(c->cur.tex[u], 0, 0, 0, 1);
    c->cur.edge = GL_TRUE;

    gl_lighting *lt = &c->lt;
    memset(lt, 0, sizeof *lt);
    for (int i = 0; i < MAX_LIGHTS; i++) {
        gl_light *L = &lt->light[i];
        set4(L->ambient, 0, 0, 0, 1);
        if (i == 0) { set4(L->diffuse, 1, 1, 1, 1); set4(L->specular, 1, 1, 1, 1); }
        else { set4(L->diffuse, 0, 0, 0, 1); set4(L->specular, 0, 0, 0, 1); }
        set4(L->pos, 0, 0, 1, 0);
        L->spot_dir[0] = 0; L->spot_dir[1] = 0; L->spot_dir[2] = -1;
        L->spot_exp = 0;
        L->spot_cutoff = 180;
        L->spot_cos = -1;
        L->att[0] = 1; L->att[1] = 0; L->att[2] = 0;
    }
    for (int f = 0; f < 2; f++) {
        gl_material *m = &lt->mat[f];
        set4(m->ambient, 0.2f, 0.2f, 0.2f, 1);
        set4(m->diffuse, 0.8f, 0.8f, 0.8f, 1);
        set4(m->specular, 0, 0, 0, 1);
        set4(m->emission, 0, 0, 0, 1);
        m->shininess = 0;
    }
    set4(lt->model_ambient, 0.2f, 0.2f, 0.2f, 1);
    lt->color_control = GL_SINGLE_COLOR;
    lt->cm_face = GL_FRONT_AND_BACK;
    lt->cm_mode = GL_AMBIENT_AND_DIFFUSE;
    lt->shade_model = GL_SMOOTH;

    c->fog.mode = GL_EXP;
    c->fog.density = 1;
    c->fog.start = 0;
    c->fog.end = 1;
    set4(c->fog.color, 0, 0, 0, 0);

    memset(&c->tx, 0, sizeof c->tx);
    for (int u = 0; u < MAX_UNITS; u++) {
        gl_texunit *tu = &c->tx.unit[u];
        tu->env_mode = GL_MODULATE;
        for (int k = 0; k < 4; k++) tu->gen_mode[k] = GL_EYE_LINEAR;
        tu->obj_plane[0][0] = tu->eye_plane[0][0] = 1;
        tu->obj_plane[1][1] = tu->eye_plane[1][1] = 1;
    }

    c->cb.alpha_func = GL_ALWAYS;
    c->cb.alpha_ref = 0;
    c->cb.blend_src = GL_ONE;
    c->cb.blend_dst = GL_ZERO;
    set4(c->cb.clear, 0, 0, 0, 0);
    for (int k = 0; k < 4; k++) c->cb.mask[k] = GL_TRUE;
    c->db.func = GL_LESS;
    c->db.mask = GL_TRUE;
    c->db.clear = 1;
    c->poly.cull = GL_BACK;
    c->poly.front = GL_CCW;
    c->poly.mode[0] = c->poly.mode[1] = GL_FILL;
    c->poly.offset_factor = c->poly.offset_units = 0;
    c->vp.x = c->vp.y = 0;
    c->vp.w = c->w; c->vp.h = c->h;
    c->vp.znear = 0; c->vp.zfar = 1;
    c->sc.x = c->sc.y = 0;
    c->sc.w = c->w; c->sc.h = c->h;
    memset(&c->xf, 0, sizeof c->xf);
    c->xf.matrix_mode = GL_MODELVIEW;
    c->hints.persp = c->hints.point = c->hints.line = c->hints.polygon = c->hints.fog = GL_DONT_CARE;
    c->line_width = 1;
    c->point_size = 1;

    memset(&c->cl, 0, sizeof c->cl);
    c->cl.unpack_align = c->cl.pack_align = 4;
    for (int i = 0; i < MODELVIEW_DEPTH; i++) mat_identity(&c->mv[i]);
    for (int i = 0; i < PROJECTION_DEPTH; i++) mat_identity(&c->proj[i]);
    for (int u = 0; u < MAX_UNITS; u++)
        for (int i = 0; i < TEXTURE_DEPTH; i++) mat_identity(&c->texm[u][i]);
    c->mv_top = c->proj_top = 0;
    for (int u = 0; u < MAX_UNITS; u++) c->tex_top[u] = 0;
    mat_identity(&c->mvp);
    c->mvp_dirty = c->nmat_dirty = 1;
    for (int u = 0; u < MAX_UNITS; u++) {
        memset(&c->tex0[u], 0, sizeof c->tex0[u]);
        c->tex0[u].min_filter = GL_NEAREST_MIPMAP_LINEAR;
        c->tex0[u].mag_filter = GL_LINEAR;
        c->tex0[u].wrap_s = c->tex0[u].wrap_t = GL_REPEAT;
        c->tex0[u].priority = 1;
        c->tex0[u].max_level = 1000;
    }
    c->next_texture = 1;
    c->list_mode = 0;
    c->list_base = 0;
}

/* ---------------- frame buffer ---------------- */
void gl_update_clip_rect(gl_context *c) {
    int x0 = 0, y0 = 0, x1 = c->w, y1 = c->h;
    if (c->en.scissor_test) {
        /* the scissor box is in GL coordinates (rows from the bottom) */
        int sx0 = c->sc.x, sx1 = c->sc.x + c->sc.w;
        int sy0 = c->h - (c->sc.y + c->sc.h), sy1 = c->h - c->sc.y;
        x0 = GL_MAX(x0, sx0); x1 = GL_MIN(x1, sx1);
        y0 = GL_MAX(y0, sy0); y1 = GL_MIN(y1, sy1);
    }
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    c->clip_x0 = x0; c->clip_y0 = y0; c->clip_x1 = x1; c->clip_y1 = y1;
}

/* the depth buffer: rows of a multiple of 4 floats, 16-byte aligned (the
 * renderer loads 4 at a time); the heap gives 8-byte alignment only */
static int alloc_depth(gl_context *c, int w, int h) {
    if (c->no_depth) return 1;
    int ds = (w + 3) & ~3;
    if (c->depth && ds == c->dstride && h <= c->depth_h && ds * h <= c->dstride * c->depth_h) {
        c->depth_w = w;
        return 1;
    }
    free(c->depth_mem);
    c->depth = NULL;
    c->depth_mem = malloc(sizeof(float) * (size_t)ds * (size_t)(h ? h : 1) + 16);
    if (!c->depth_mem) { c->dstride = c->depth_w = c->depth_h = 0; return 0; }
    c->depth = (float *)(((uintptr_t)c->depth_mem + 15) & ~(uintptr_t)15);
    c->dstride = ds;
    c->depth_w = w;
    c->depth_h = h;
    for (size_t i = 0; i < (size_t)ds * (size_t)h; i++) c->depth[i] = 1;
    return 1;
}

int gl_set_framebuffer(gl_context *c, unsigned *color, int w, int h, int stride) {
    if (w < 0 || h < 0 || w > MAX_FB || h > MAX_FB || (w && !color)) return 0;
    gl_render(c);                          /* what was queued goes to the old pixels */
    c->color = color;
    c->w = w;
    c->h = h;
    c->stride = stride;
    if (!alloc_depth(c, w, h)) return 0;
    if (!gl_raster_resize(c)) return 0;
    gl_update_clip_rect(c);
    gl_state_changed(c);
    return 1;
}

gl_context *gl_context_new(unsigned *color, int w, int h, int stride, unsigned flags) {
    if (w < 0 || h < 0 || w > MAX_FB || h > MAX_FB) return NULL;
    gl_context *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->win = -1;
    c->no_depth = (flags & HGL_NO_DEPTH) != 0;
    c->w = w;
    c->h = h;
    defaults(c);
    gl_raster_init(c);
    c->attrib = calloc(ATTRIB_DEPTH, sizeof *c->attrib);
    c->client_attrib = calloc(ATTRIB_DEPTH, sizeof *c->client_attrib);
    if (!c->tris || !c->rstates || !c->attrib || !c->client_attrib || !gl_set_framebuffer(c, color, w, h, stride)) {
        gl_context_free(c);
        return NULL;
    }
    c->fps_t0 = gl_time_ms();
    return c;
}

void gl_context_free(gl_context *c) {
    if (!c) return;
    if (gl_cur == c) gl_cur = NULL;
    gl_raster_free(c);
    gl_textures_free(c);
    gl_lists_free(c);
    free(c->depth_mem);
    if (c->own_color) free(c->color);
    free(c->raw);
    free(c->vtx);
    free(c->attrib);
    free(c->client_attrib);
    free(c);
}

/* a frame was finished: the counters */
void gl_frame_done(gl_context *c) {
    gl_render(c);
    c->frames++;
    c->fps_frames++;
    c->tris_frame = 0;
    uint64_t now = gl_time_ms();
    uint64_t dt = now - c->fps_t0;
    if (dt >= 1000) {
        c->fps = (float)c->fps_frames * 1000.0f / (float)dt;
        c->fps_frames = 0;
        c->fps_t0 = now;
    }
    c->frame_dirty = 0;
    c->dirty_x0 = c->dirty_y0 = c->dirty_x1 = c->dirty_y1 = 0;
}

/* ---------------- the binding's common part (memory contexts) ---------------- */
HGLRC hglCreateMemoryContext(int w, int h, unsigned flags) {
    if (w <= 0 || h <= 0 || w > MAX_FB || h > MAX_FB) return NULL;
    unsigned *px = calloc((size_t)w * (size_t)h, sizeof *px);
    if (!px) return NULL;
    gl_context *c = gl_context_new(px, w, h, w, flags);
    if (!c) { free(px); return NULL; }
    c->own_color = 1;
    return c;
}

HGLRC hglCreateBufferContext(unsigned *pixels, int w, int h, int stride, unsigned flags) {
    if (!pixels || stride < w) return NULL;
    return gl_context_new(pixels, w, h, stride, flags);
}

int hglSetBuffer(HGLRC c, unsigned *pixels, int w, int h, int stride) {
    if (!c || c->own_color || !pixels || stride < w) return 0;
    return gl_set_framebuffer(c, pixels, w, h, stride);
}

unsigned *hglGetPixels(HGLRC c, int *w, int *h, int *stride) {
    if (!c) return NULL;
    gl_render(c);
    if (w) *w = c->w;
    if (h) *h = c->h;
    if (stride) *stride = c->stride;
    return c->color;
}

int hglMakeCurrent(HGLRC c) {
    if (gl_cur && gl_cur != c) gl_render(gl_cur);
    gl_cur = c;
    return 1;
}

HGLRC hglGetCurrentContext(void) { return gl_cur; }

void hglDeleteContext(HGLRC c) {
    if (!c) return;
    gl_render(c);
    gl_context_free(c);
}

unsigned hglFrameCount(HGLRC c) { return c ? c->frames : 0; }
float hglFramesPerSecond(HGLRC c) { return c ? c->fps : 0; }

void hglFinishFrame(HGLRC c) {
    if (!c) return;
    gl_frame_done(c);
}

/* ---------------- enable / disable ---------------- */
static GLboolean *cap_flag(gl_context *c, GLenum cap) {
    gl_enables *e = &c->en;
    int u = c->tx.active;
    switch (cap) {
    case GL_LIGHTING: return &e->lighting;
    case GL_COLOR_MATERIAL: return &e->color_material;
    case GL_NORMALIZE: return &e->normalize;
    case GL_RESCALE_NORMAL: return &e->rescale_normal;
    case GL_FOG: return &e->fog;
    case GL_DEPTH_TEST: return &e->depth_test;
    case GL_ALPHA_TEST: return &e->alpha_test;
    case GL_BLEND: return &e->blend;
    case GL_CULL_FACE: return &e->cull_face;
    case GL_SCISSOR_TEST: return &e->scissor_test;
    case GL_DITHER: return &e->dither;
    case GL_POLYGON_OFFSET_FILL: return &e->offset_fill;
    case GL_POLYGON_OFFSET_LINE: return &e->offset_line;
    case GL_POLYGON_OFFSET_POINT: return &e->offset_point;
    case GL_TEXTURE_2D: return &e->tex2d[u];
    case GL_TEXTURE_1D: return &e->tex1d[u];
    case GL_TEXTURE_GEN_S: return &e->texgen[u][0];
    case GL_TEXTURE_GEN_T: return &e->texgen[u][1];
    case GL_TEXTURE_GEN_R: return &e->texgen[u][2];
    case GL_TEXTURE_GEN_Q: return &e->texgen[u][3];
    case GL_LINE_SMOOTH: return &e->line_smooth;
    case GL_POINT_SMOOTH: return &e->point_smooth;
    case GL_POLYGON_SMOOTH: return &e->polygon_smooth;
    case GL_LINE_STIPPLE: return &e->line_stipple;
    case GL_POLYGON_STIPPLE: return &e->polygon_stipple;
    case GL_STENCIL_TEST: return &e->stencil_test;
    case GL_COLOR_LOGIC_OP: case GL_INDEX_LOGIC_OP: return &e->logic_op;
    }
    if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + MAX_LIGHTS) return &e->light[cap - GL_LIGHT0];
    if (cap >= GL_CLIP_PLANE0 && cap < GL_CLIP_PLANE0 + MAX_CLIP_PLANES) return &e->clip[cap - GL_CLIP_PLANE0];
    return NULL;
}

static void client_cap(gl_context *c, GLenum cap, int on) {
    if (on) glEnableClientState(cap); else glDisableClientState(cap);
}

static void set_cap(GLenum cap, int on) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { gl_dlarg a = { .u = cap }; if (gl_dl_record(c, on ? OP_ENABLE : OP_DISABLE, 1, &a)) return; }
    NOT_IN_BEGIN(c);
    if (cap == GL_VERTEX_ARRAY || cap == GL_NORMAL_ARRAY || cap == GL_COLOR_ARRAY || cap == GL_TEXTURE_COORD_ARRAY) {
        client_cap(c, cap, on);           /* (accepted here as some old code does) */
        return;
    }
    GLboolean *f = cap_flag(c, cap);
    if (!f) { gl_error(c, GL_INVALID_ENUM); return; }
    GLboolean v = on ? GL_TRUE : GL_FALSE;
    if (*f == v) return;
    *f = v;
    if (cap == GL_SCISSOR_TEST) gl_update_clip_rect(c);
    gl_state_changed(c);
}
void glEnable(GLenum cap) { set_cap(cap, 1); }
void glDisable(GLenum cap) { set_cap(cap, 0); }

GLboolean glIsEnabled(GLenum cap) {
    GET_CTX_R(c, GL_FALSE);
    switch (cap) {
    case GL_VERTEX_ARRAY: return c->cl.vertex.enabled;
    case GL_NORMAL_ARRAY: return c->cl.normal.enabled;
    case GL_COLOR_ARRAY: return c->cl.color.enabled;
    case GL_TEXTURE_COORD_ARRAY: return c->cl.tex[c->cl.active].enabled;
    case GL_EDGE_FLAG_ARRAY: return c->cl.edge.enabled;
    }
    GLboolean *f = cap_flag(c, cap);
    if (!f) { gl_error(c, GL_INVALID_ENUM); return GL_FALSE; }
    return *f;
}

/* ---------------- hints, pixel storage ---------------- */
void glHint(GLenum target, GLenum mode) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { gl_dlarg a[2] = { { .u = target }, { .u = mode } }; if (gl_dl_record(c, OP_HINT, 2, a)) return; }
    NOT_IN_BEGIN(c);
    if (mode != GL_DONT_CARE && mode != GL_FASTEST && mode != GL_NICEST) { gl_error(c, GL_INVALID_ENUM); return; }
    switch (target) {
    case GL_PERSPECTIVE_CORRECTION_HINT: c->hints.persp = mode; break;   /* (always perspective-correct) */
    case GL_POINT_SMOOTH_HINT: c->hints.point = mode; break;
    case GL_LINE_SMOOTH_HINT: c->hints.line = mode; break;
    case GL_POLYGON_SMOOTH_HINT: c->hints.polygon = mode; break;
    case GL_FOG_HINT: c->hints.fog = mode; break;
    default: gl_error(c, GL_INVALID_ENUM); break;
    }
}

void glPixelStorei(GLenum pname, GLint v) {
    GET_CTX(c);
    NOT_IN_BEGIN(c);
    gl_client *cl = &c->cl;
    switch (pname) {
    case GL_UNPACK_ALIGNMENT: case GL_PACK_ALIGNMENT:
        if (v != 1 && v != 2 && v != 4 && v != 8) { gl_error(c, GL_INVALID_VALUE); return; }
        if (pname == GL_UNPACK_ALIGNMENT) cl->unpack_align = v; else cl->pack_align = v;
        return;
    case GL_UNPACK_SWAP_BYTES: case GL_UNPACK_LSB_FIRST: case GL_PACK_SWAP_BYTES: case GL_PACK_LSB_FIRST:
        return;                            /* (8-bit and float data only: nothing to swap) */
    }
    if (v < 0) { gl_error(c, GL_INVALID_VALUE); return; }
    switch (pname) {
    case GL_UNPACK_ROW_LENGTH: cl->unpack_row = v; break;
    case GL_UNPACK_SKIP_ROWS: cl->unpack_skip_rows = v; break;
    case GL_UNPACK_SKIP_PIXELS: cl->unpack_skip_px = v; break;
    case GL_PACK_ROW_LENGTH: cl->pack_row = v; break;
    case GL_PACK_SKIP_ROWS: cl->pack_skip_rows = v; break;
    case GL_PACK_SKIP_PIXELS: cl->pack_skip_px = v; break;
    default: gl_error(c, GL_INVALID_ENUM); break;
    }
}
void glPixelStoref(GLenum pname, GLfloat v) { glPixelStorei(pname, (GLint)v); }

/* one color buffer: front, back and the rest name the same pixels */
void glDrawBuffer(GLenum mode) { (void)mode; }
void glReadBuffer(GLenum mode) { (void)mode; }

/* ---------------- glGet ---------------- */
enum { GV_FLOAT, GV_INT, GV_BOOL, GV_COLOR };   /* GV_COLOR: floats that glGetIntegerv maps to the full range */

static int mat_out(const glmat *m, double *v, int transpose) {
    for (int i = 0; i < 16; i++) v[i] = transpose ? m->m[(i & 3) * 4 + (i >> 2)] : m->m[i];
    return 16;
}

/* the values of pname (at most 16), their kind in *kind; 0: unknown */
static int get_values(gl_context *c, GLenum p, double *v, int *kind) {
    *kind = GV_INT;
    int u = c->tx.active;
#define F1(x) do { *kind = GV_FLOAT; v[0] = (x); return 1; } while (0)
#define I1(x) do { v[0] = (double)(x); return 1; } while (0)
#define B1(x) do { *kind = GV_BOOL; v[0] = (x) ? 1 : 0; return 1; } while (0)
#define C4(a) do { *kind = GV_COLOR; for (int i_ = 0; i_ < 4; i_++) v[i_] = (a)[i_]; return 4; } while (0)
    switch (p) {
    case GL_CURRENT_COLOR: C4(c->cur.color);
    case GL_CURRENT_NORMAL: *kind = GV_FLOAT; v[0] = c->cur.normal[0]; v[1] = c->cur.normal[1]; v[2] = c->cur.normal[2]; return 3;
    case GL_CURRENT_TEXTURE_COORDS: *kind = GV_FLOAT; for (int i = 0; i < 4; i++) v[i] = c->cur.tex[u][i]; return 4;
    case GL_POINT_SIZE: F1(c->point_size);
    case GL_POINT_SIZE_RANGE: case GL_LINE_WIDTH_RANGE: *kind = GV_FLOAT; v[0] = 1; v[1] = 64; return 2;
    case GL_LINE_WIDTH: F1(c->line_width);
    case GL_POLYGON_MODE: v[0] = c->poly.mode[0]; v[1] = c->poly.mode[1]; return 2;
    case GL_CULL_FACE_MODE: I1(c->poly.cull);
    case GL_FRONT_FACE: I1(c->poly.front);
    case GL_SHADE_MODEL: I1(c->lt.shade_model);
    case GL_DEPTH_RANGE: *kind = GV_COLOR; v[0] = c->vp.znear; v[1] = c->vp.zfar; return 2;
    case GL_DEPTH_WRITEMASK: B1(c->db.mask);
    case GL_DEPTH_CLEAR_VALUE: *kind = GV_COLOR; v[0] = c->db.clear; return 1;
    case GL_DEPTH_FUNC: I1(c->db.func);
    case GL_MATRIX_MODE: I1(c->xf.matrix_mode);
    case GL_MODELVIEW_STACK_DEPTH: I1(c->mv_top + 1);
    case GL_PROJECTION_STACK_DEPTH: I1(c->proj_top + 1);
    case GL_TEXTURE_STACK_DEPTH: I1(c->tex_top[u] + 1);
    case GL_MODELVIEW_MATRIX: *kind = GV_FLOAT; return mat_out(&c->mv[c->mv_top], v, 0);
    case GL_PROJECTION_MATRIX: *kind = GV_FLOAT; return mat_out(&c->proj[c->proj_top], v, 0);
    case GL_TEXTURE_MATRIX: *kind = GV_FLOAT; return mat_out(&c->texm[u][c->tex_top[u]], v, 0);
    case GL_TRANSPOSE_MODELVIEW_MATRIX: *kind = GV_FLOAT; return mat_out(&c->mv[c->mv_top], v, 1);
    case GL_TRANSPOSE_PROJECTION_MATRIX: *kind = GV_FLOAT; return mat_out(&c->proj[c->proj_top], v, 1);
    case GL_TRANSPOSE_TEXTURE_MATRIX: *kind = GV_FLOAT; return mat_out(&c->texm[u][c->tex_top[u]], v, 1);
    case GL_VIEWPORT: v[0] = c->vp.x; v[1] = c->vp.y; v[2] = c->vp.w; v[3] = c->vp.h; return 4;
    case GL_SCISSOR_BOX: v[0] = c->sc.x; v[1] = c->sc.y; v[2] = c->sc.w; v[3] = c->sc.h; return 4;
    case GL_ALPHA_TEST_FUNC: I1(c->cb.alpha_func);
    case GL_ALPHA_TEST_REF: *kind = GV_COLOR; v[0] = c->cb.alpha_ref; return 1;
    case GL_BLEND_SRC: I1(c->cb.blend_src);
    case GL_BLEND_DST: I1(c->cb.blend_dst);
    case GL_COLOR_CLEAR_VALUE: C4(c->cb.clear);
    case GL_COLOR_WRITEMASK: *kind = GV_BOOL; for (int i = 0; i < 4; i++) v[i] = c->cb.mask[i]; return 4;
    case GL_DOUBLEBUFFER: B1(1);
    case GL_RGBA_MODE: B1(1);
    case GL_MAX_LIGHTS: I1(MAX_LIGHTS);
    case GL_MAX_CLIP_PLANES: I1(MAX_CLIP_PLANES);
    case GL_MAX_TEXTURE_SIZE: I1(1 << MAX_TEX_LOG);
    case GL_MAX_ATTRIB_STACK_DEPTH: I1(ATTRIB_DEPTH);
    case GL_MAX_MODELVIEW_STACK_DEPTH: I1(MODELVIEW_DEPTH);
    case GL_MAX_PROJECTION_STACK_DEPTH: I1(PROJECTION_DEPTH);
    case GL_MAX_TEXTURE_STACK_DEPTH: I1(TEXTURE_DEPTH);
    case GL_MAX_VIEWPORT_DIMS: v[0] = MAX_FB; v[1] = MAX_FB; return 2;
    case GL_MAX_LIST_NESTING: I1(MAX_LIST_NESTING);
    case GL_MAX_TEXTURE_UNITS: I1(MAX_UNITS);
    case GL_MAX_ELEMENTS_VERTICES: case GL_MAX_ELEMENTS_INDICES: I1(65536);
    case GL_SUBPIXEL_BITS: I1(4);
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: I1(8);
    case GL_ALPHA_BITS: case GL_STENCIL_BITS: I1(0);
    case GL_DEPTH_BITS: I1(c->depth ? 24 : 0);
    case GL_POLYGON_OFFSET_FACTOR: F1(c->poly.offset_factor);
    case GL_POLYGON_OFFSET_UNITS: F1(c->poly.offset_units);
    case GL_LIGHT_MODEL_AMBIENT: C4(c->lt.model_ambient);
    case GL_LIGHT_MODEL_LOCAL_VIEWER: B1(c->lt.local_viewer);
    case GL_LIGHT_MODEL_TWO_SIDE: B1(c->lt.two_side);
    case GL_LIGHT_MODEL_COLOR_CONTROL: I1(c->lt.color_control);
    case GL_COLOR_MATERIAL_FACE: I1(c->lt.cm_face);
    case GL_COLOR_MATERIAL_PARAMETER: I1(c->lt.cm_mode);
    case GL_FOG_MODE: I1(c->fog.mode);
    case GL_FOG_DENSITY: F1(c->fog.density);
    case GL_FOG_START: F1(c->fog.start);
    case GL_FOG_END: F1(c->fog.end);
    case GL_FOG_COLOR: C4(c->fog.color);
    case GL_TEXTURE_BINDING_2D: I1(c->tx.unit[u].bound);
    case GL_TEXTURE_BINDING_1D: I1(0);
    case GL_ACTIVE_TEXTURE: I1(GL_TEXTURE0 + u);
    case GL_CLIENT_ACTIVE_TEXTURE: I1(GL_TEXTURE0 + c->cl.active);
    case GL_LIST_BASE: I1(c->list_base);
    case GL_LIST_INDEX: I1(c->compiling ? c->compiling->name : 0);
    case GL_LIST_MODE: I1(c->compiling ? c->list_mode : 0);
    case GL_UNPACK_ALIGNMENT: I1(c->cl.unpack_align);
    case GL_UNPACK_ROW_LENGTH: I1(c->cl.unpack_row);
    case GL_UNPACK_SKIP_ROWS: I1(c->cl.unpack_skip_rows);
    case GL_UNPACK_SKIP_PIXELS: I1(c->cl.unpack_skip_px);
    case GL_PACK_ALIGNMENT: I1(c->cl.pack_align);
    case GL_PACK_ROW_LENGTH: I1(c->cl.pack_row);
    case GL_PACK_SKIP_ROWS: I1(c->cl.pack_skip_rows);
    case GL_PACK_SKIP_PIXELS: I1(c->cl.pack_skip_px);
    case GL_UNPACK_SWAP_BYTES: case GL_UNPACK_LSB_FIRST: case GL_PACK_SWAP_BYTES: case GL_PACK_LSB_FIRST: B1(0);
    case GL_PERSPECTIVE_CORRECTION_HINT: I1(c->hints.persp);
    case GL_POINT_SMOOTH_HINT: I1(c->hints.point);
    case GL_LINE_SMOOTH_HINT: I1(c->hints.line);
    case GL_POLYGON_SMOOTH_HINT: I1(c->hints.polygon);
    case GL_FOG_HINT: I1(c->hints.fog);
    case GL_VERTEX_ARRAY: B1(c->cl.vertex.enabled);
    case GL_NORMAL_ARRAY: B1(c->cl.normal.enabled);
    case GL_COLOR_ARRAY: B1(c->cl.color.enabled);
    case GL_TEXTURE_COORD_ARRAY: B1(c->cl.tex[c->cl.active].enabled);
    case GL_EDGE_FLAG_ARRAY: B1(c->cl.edge.enabled);
    }
#undef F1
#undef I1
#undef B1
#undef C4
    GLboolean *f = cap_flag(c, p);
    if (f) { *kind = GV_BOOL; v[0] = *f; return 1; }
    return 0;
}

static int get_common(GLenum p, double *v, int *kind) {
    GET_CTX_R(c, 0);
    if (c->in_begin) { gl_error(c, GL_INVALID_OPERATION); return 0; }
    int n = get_values(c, p, v, kind);
    if (!n) gl_error(c, GL_INVALID_ENUM);
    return n;
}

void glGetDoublev(GLenum p, GLdouble *out) {
    double v[16];
    int kind, n = get_common(p, v, &kind);
    for (int i = 0; i < n; i++) out[i] = v[i];
}
void glGetFloatv(GLenum p, GLfloat *out) {
    double v[16];
    int kind, n = get_common(p, v, &kind);
    for (int i = 0; i < n; i++) out[i] = (float)v[i];
}
void glGetIntegerv(GLenum p, GLint *out) {
    double v[16];
    int kind, n = get_common(p, v, &kind);
    for (int i = 0; i < n; i++) {
        if (kind == GV_COLOR) {            /* [-1, 1] to the whole range of integers */
            double d = v[i] < -1 ? -1 : v[i] > 1 ? 1 : v[i];
            out[i] = (GLint)(d >= 0 ? d * 2147483647.0 + 0.5 : d * 2147483647.0 - 0.5);
        } else out[i] = (GLint)(v[i] >= 0 ? v[i] + 0.5 : v[i] - 0.5);
    }
}
void glGetBooleanv(GLenum p, GLboolean *out) {
    double v[16];
    int kind, n = get_common(p, v, &kind);
    for (int i = 0; i < n; i++) out[i] = v[i] != 0 ? GL_TRUE : GL_FALSE;
}

/* ---------------- attribute stacks ---------------- */
void glPushAttrib(GLbitfield mask) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { gl_dlarg a = { .u = mask }; if (gl_dl_record(c, OP_PUSH_ATTRIB, 1, &a)) return; }
    NOT_IN_BEGIN(c);
    if (c->attrib_top >= ATTRIB_DEPTH) { gl_error(c, GL_STACK_OVERFLOW); return; }
    gl_attrib_save *s = &c->attrib[c->attrib_top++];
    s->mask = mask;
    /* every group is saved (cheap); only those in mask come back */
    s->en = c->en; s->cur = c->cur; s->lt = c->lt; s->fog = c->fog; s->tx = c->tx; s->cb = c->cb; s->db = c->db;
    s->poly = c->poly; s->vp = c->vp; s->sc = c->sc; s->xf = c->xf; s->hints = c->hints;
    s->line_width = c->line_width; s->point_size = c->point_size; s->list_base = c->list_base;
    for (int u = 0; u < MAX_UNITS; u++) {
        gl_texture *t = gl_bound_texture(c, u);
        s->tex_params[u].min = t->min_filter; s->tex_params[u].mag = t->mag_filter;
        s->tex_params[u].ws = t->wrap_s; s->tex_params[u].wt = t->wrap_t;
    }
}

void glPopAttrib(void) {
    GET_CTX(c);
    if (DL_ACTIVE(c) && gl_dl_record(c, OP_POP_ATTRIB, 0, 0)) return;
    NOT_IN_BEGIN(c);
    if (c->attrib_top <= 0) { gl_error(c, GL_STACK_UNDERFLOW); return; }
    gl_attrib_save *s = &c->attrib[--c->attrib_top];
    GLbitfield m = s->mask;
    gl_enables *e = &c->en;
    const gl_enables *se = &s->en;
    if (m & GL_ENABLE_BIT) *e = *se;
    if (m & GL_CURRENT_BIT) c->cur = s->cur;
    if (m & GL_LIGHTING_BIT) {
        c->lt = s->lt;
        e->lighting = se->lighting; e->color_material = se->color_material;
        memcpy(e->light, se->light, sizeof e->light);
    }
    if (m & GL_FOG_BIT) { c->fog = s->fog; e->fog = se->fog; }
    if (m & GL_TEXTURE_BIT) {
        int active = c->tx.active;
        c->tx = s->tx;
        c->tx.active = active;             /* (GL_TEXTURE_BIT does not hold the active unit) */
        memcpy(e->tex2d, se->tex2d, sizeof e->tex2d);
        memcpy(e->tex1d, se->tex1d, sizeof e->tex1d);
        memcpy(e->texgen, se->texgen, sizeof e->texgen);
        for (int u = 0; u < MAX_UNITS; u++) {
            gl_texture *t = gl_bound_texture(c, u);
            gl_render(c);
            t->min_filter = s->tex_params[u].min; t->mag_filter = s->tex_params[u].mag;
            t->wrap_s = s->tex_params[u].ws; t->wrap_t = s->tex_params[u].wt;
        }
    }
    if (m & GL_COLOR_BUFFER_BIT) {
        c->cb = s->cb;
        e->alpha_test = se->alpha_test; e->blend = se->blend; e->dither = se->dither; e->logic_op = se->logic_op;
    }
    if (m & GL_DEPTH_BUFFER_BIT) { c->db = s->db; e->depth_test = se->depth_test; }
    if (m & GL_POLYGON_BIT) {
        c->poly = s->poly;
        e->cull_face = se->cull_face; e->offset_fill = se->offset_fill; e->offset_line = se->offset_line;
        e->offset_point = se->offset_point; e->polygon_smooth = se->polygon_smooth; e->polygon_stipple = se->polygon_stipple;
    }
    if (m & GL_VIEWPORT_BIT) c->vp = s->vp;
    if (m & GL_SCISSOR_BIT) { c->sc = s->sc; e->scissor_test = se->scissor_test; }
    if (m & GL_TRANSFORM_BIT) {
        c->xf = s->xf;
        e->normalize = se->normalize; e->rescale_normal = se->rescale_normal;
        memcpy(e->clip, se->clip, sizeof e->clip);
    }
    if (m & GL_HINT_BIT) c->hints = s->hints;
    if (m & GL_LINE_BIT) { c->line_width = s->line_width; e->line_smooth = se->line_smooth; e->line_stipple = se->line_stipple; }
    if (m & GL_POINT_BIT) { c->point_size = s->point_size; e->point_smooth = se->point_smooth; }
    if (m & GL_LIST_BIT) c->list_base = s->list_base;
    gl_update_clip_rect(c);
    gl_state_changed(c);
}

void glPushClientAttrib(GLbitfield mask) {
    GET_CTX(c);
    NOT_IN_BEGIN(c);
    if (c->client_attrib_top >= ATTRIB_DEPTH) { gl_error(c, GL_STACK_OVERFLOW); return; }
    c->client_attrib[c->client_attrib_top] = c->cl;
    c->client_attrib_mask[c->client_attrib_top] = mask;
    c->client_attrib_top++;
}

void glPopClientAttrib(void) {
    GET_CTX(c);
    NOT_IN_BEGIN(c);
    if (c->client_attrib_top <= 0) { gl_error(c, GL_STACK_UNDERFLOW); return; }
    c->client_attrib_top--;
    const gl_client *s = &c->client_attrib[c->client_attrib_top];
    GLbitfield m = c->client_attrib_mask[c->client_attrib_top];
    if (m & GL_CLIENT_VERTEX_ARRAY_BIT) {
        c->cl.vertex = s->vertex; c->cl.normal = s->normal; c->cl.color = s->color; c->cl.edge = s->edge;
        memcpy(c->cl.tex, s->tex, sizeof c->cl.tex);
        c->cl.active = s->active;
    }
    if (m & GL_CLIENT_PIXEL_STORE_BIT) {
        c->cl.unpack_align = s->unpack_align; c->cl.unpack_row = s->unpack_row;
        c->cl.unpack_skip_rows = s->unpack_skip_rows; c->cl.unpack_skip_px = s->unpack_skip_px;
        c->cl.pack_align = s->pack_align; c->cl.pack_row = s->pack_row;
        c->cl.pack_skip_rows = s->pack_skip_rows; c->cl.pack_skip_px = s->pack_skip_px;
    }
}
