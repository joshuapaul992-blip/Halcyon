/* Halcyon's 3D library: textures. 2D texture objects (power-of-two sizes
 * up to 2048, every mipmap level), images converted to 32-bit texels when
 * they are given (any of the usual formats and sizes), parameters, the
 * texture environment, texture coordinate generation, and the per-pixel
 * description of a unit that the tile renderer uses. */
#include "glint.h"

static int is_pow2(int v) { return v > 0 && !(v & (v - 1)); }
static int ilog2(int v) { int l = 0; while ((1 << l) < v) l++; return l; }

static void tex_init(gl_texture *t, GLuint name) {
    memset(t, 0, sizeof *t);
    t->name = name;
    t->min_filter = GL_NEAREST_MIPMAP_LINEAR;
    t->mag_filter = GL_LINEAR;
    t->wrap_s = t->wrap_t = GL_REPEAT;
    t->priority = 1;
    t->max_level = 1000;
}

static void tex_free_levels(gl_texture *t) {
    for (int i = 0; i < MAX_LEVELS; i++) { free(t->level[i]); t->level[i] = NULL; t->lw[i] = t->lh[i] = 0; }
}

static gl_texture *lookup(gl_context *c, GLuint name) {
    if (name == 0 || name >= (GLuint)c->ntextures) return NULL;
    return c->textures[name];
}

gl_texture *gl_bound_texture(gl_context *c, int unit) {
    GLuint n = c->tx.unit[unit].bound;
    gl_texture *t = lookup(c, n);
    return t ? t : &c->tex0[unit];
}

void gl_textures_free(gl_context *c) {
    for (int i = 0; i < c->ntextures; i++)
        if (c->textures[i]) { tex_free_levels(c->textures[i]); free(c->textures[i]); }
    free(c->textures);
    c->textures = NULL;
    c->ntextures = 0;
    for (int u = 0; u < MAX_UNITS; u++) tex_free_levels(&c->tex0[u]);
}

/* textures may be queued for drawing: draw the queue before changing them */
static void before_change(gl_context *c) {
    gl_render(c);
    gl_state_changed(c);
}

void glGenTextures(GLsizei n, GLuint *names) {
    GET_CTX(c);
    NOT_IN_BEGIN(c);
    if (n < 0) { gl_error(c, GL_INVALID_VALUE); return; }
    for (int i = 0; i < n; i++) {
        GLuint name = c->next_texture ? c->next_texture : 1;
        while (lookup(c, name)) name++;
        /* reserve it: an object without images */
        if (name >= (GLuint)c->ntextures) {
            int cap = c->ntextures ? c->ntextures : 64;
            while ((GLuint)cap <= name) cap *= 2;
            gl_texture **p = realloc(c->textures, sizeof *p * (size_t)cap);
            if (!p) { gl_error(c, GL_OUT_OF_MEMORY); return; }
            memset(p + c->ntextures, 0, sizeof *p * (size_t)(cap - c->ntextures));
            c->textures = p;
            c->ntextures = cap;
        }
        gl_texture *t = malloc(sizeof *t);
        if (!t) { gl_error(c, GL_OUT_OF_MEMORY); return; }
        tex_init(t, name);
        c->textures[name] = t;
        names[i] = name;
        c->next_texture = name + 1;
    }
}

void glDeleteTextures(GLsizei n, const GLuint *names) {
    GET_CTX(c);
    NOT_IN_BEGIN(c);
    if (n < 0) { gl_error(c, GL_INVALID_VALUE); return; }
    before_change(c);
    for (int i = 0; i < n; i++) {
        gl_texture *t = lookup(c, names[i]);
        if (!t) continue;
        for (int u = 0; u < MAX_UNITS; u++) if (c->tx.unit[u].bound == names[i]) c->tx.unit[u].bound = 0;
        tex_free_levels(t);
        free(t);
        c->textures[names[i]] = NULL;
        if (names[i] < c->next_texture) c->next_texture = names[i];
    }
}

GLboolean glIsTexture(GLuint name) { GET_CTX_R(c, GL_FALSE); return lookup(c, name) ? GL_TRUE : GL_FALSE; }

void glBindTexture(GLenum target, GLuint name) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { gl_dlarg a[2] = { { .u = target }, { .u = name } }; if (gl_dl_record(c, OP_BIND_TEXTURE, 2, a)) return; }
    NOT_IN_BEGIN(c);
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_1D) { gl_error(c, GL_INVALID_ENUM); return; }
    if (name && !lookup(c, name)) {         /* a name never generated: made now */
        GLuint save = c->next_texture;
        c->next_texture = name;
        GLuint got;
        glGenTextures(1, &got);
        c->next_texture = save > name ? save : name + 1;
        if (got != name) return;
    }
    if (c->tx.unit[c->tx.active].bound != name) {
        c->tx.unit[c->tx.active].bound = name;
        gl_state_changed(c);
    }
}

void glPrioritizeTextures(GLsizei n, const GLuint *names, const GLclampf *prio) {
    GET_CTX(c);
    for (int i = 0; i < n; i++) { gl_texture *t = lookup(c, names[i]); if (t) t->priority = prio[i]; }
}

/* ---------------- images ---------------- */
static int base_format(GLint internal) {
    switch (internal) {
    case 1: case GL_LUMINANCE: case GL_LUMINANCE4: case GL_LUMINANCE8: return BASE_LUMINANCE;
    case 2: case GL_LUMINANCE_ALPHA: case GL_LUMINANCE4_ALPHA4: case GL_LUMINANCE8_ALPHA8: return BASE_LUMINANCE_ALPHA;
    case 3: case GL_RGB: case GL_R3_G3_B2: case GL_RGB4: case GL_RGB5: case GL_RGB8: case GL_BGR: return BASE_RGB;
    case 4: case GL_RGBA: case GL_RGBA2: case GL_RGBA4: case GL_RGB5_A1: case GL_RGBA8: case GL_BGRA: return BASE_RGBA;
    case GL_ALPHA: case GL_ALPHA4: case GL_ALPHA8: return BASE_ALPHA;
    case GL_INTENSITY: case GL_INTENSITY4: case GL_INTENSITY8: return BASE_INTENSITY;
    }
    return 0;
}

static int format_comps(GLenum f) {
    switch (f) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE: return 1;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_RGB: case GL_BGR: return 3;
    case GL_RGBA: case GL_BGRA: return 4;
    }
    return 0;
}

/* one source pixel to R G B A (0..255) */
static void src_rgba(GLenum format, GLenum type, const unsigned char *p, int *o) {
    int n = format_comps(format), v[4];
    for (int k = 0; k < n; k++) {
        if (type == GL_FLOAT) { float f; memcpy(&f, p + 4 * k, 4); f = GL_CLAMPF(f, 0.0f, 1.0f); v[k] = (int)(f * 255 + 0.5f); }
        else v[k] = p[k];
    }
    o[0] = o[1] = o[2] = 0; o[3] = 255;
    switch (format) {
    case GL_RED: o[0] = v[0]; break;
    case GL_GREEN: o[1] = v[0]; break;
    case GL_BLUE: o[2] = v[0]; break;
    case GL_ALPHA: o[3] = v[0]; break;
    case GL_LUMINANCE: o[0] = o[1] = o[2] = v[0]; break;
    case GL_LUMINANCE_ALPHA: o[0] = o[1] = o[2] = v[0]; o[3] = v[1]; break;
    case GL_RGB: o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; break;
    case GL_BGR: o[0] = v[2]; o[1] = v[1]; o[2] = v[0]; break;
    case GL_RGBA: o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; o[3] = v[3]; break;
    case GL_BGRA: o[0] = v[2]; o[1] = v[1]; o[2] = v[0]; o[3] = v[3]; break;
    }
}

/* R G B A to a texel of the base format; what the format lacks is filled
 * so that GL_MODULATE needs no special case (see raster.c env16) */
static uint32_t texel(int base, const int *c) {
    int r = c[0], g = c[1], b = c[2], a = c[3];
    switch (base) {
    case BASE_ALPHA: r = g = b = 255; break;
    case BASE_LUMINANCE: g = b = r; a = 255; break;
    case BASE_LUMINANCE_ALPHA: g = b = r; break;
    case BASE_INTENSITY: g = b = a = r; break;
    case BASE_RGB: a = 255; break;
    }
    return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static int convert(gl_context *c, uint32_t *dst, int dstride, int w, int h, int base, GLenum format, GLenum type, const void *pixels) {
    int comps = format_comps(format);
    int bsize = type == GL_FLOAT ? 4 : 1;
    int rowlen = (c->cl.unpack_row > 0 ? c->cl.unpack_row : w) * comps * bsize;
    int al = c->cl.unpack_align;
    rowlen = (rowlen + al - 1) / al * al;
    const unsigned char *src = (const unsigned char *)pixels + (size_t)c->cl.unpack_skip_rows * (size_t)rowlen +
                               (size_t)c->cl.unpack_skip_px * (size_t)(comps * bsize);
    for (int y = 0; y < h; y++) {
        const unsigned char *s = src + (size_t)y * (size_t)rowlen;
        uint32_t *d = dst + (size_t)y * (size_t)dstride;
        for (int x = 0; x < w; x++) {
            int v[4];
            src_rgba(format, type, s + (size_t)x * (size_t)(comps * bsize), v);
            d[x] = texel(base, v);
        }
    }
    return 1;
}

static int check_target_type(gl_context *c, GLenum target, GLenum format, GLenum type) {
    if (target != GL_TEXTURE_2D) { gl_error(c, GL_INVALID_ENUM); return 0; }
    if (!format_comps(format) || (type != GL_UNSIGNED_BYTE && type != GL_FLOAT)) { gl_error(c, GL_INVALID_ENUM); return 0; }
    return 1;
}

void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels) {
    GET_CTX(c);
    NOT_IN_BEGIN(c);
    if (!check_target_type(c, target, format, type)) return;
    int base = base_format(internal);
    if (!base) { gl_error(c, GL_INVALID_VALUE); return; }
    if (level < 0 || level > MAX_TEX_LOG || border != 0 || !is_pow2(w) || !is_pow2(h) ||
        w > (1 << MAX_TEX_LOG) || h > (1 << MAX_TEX_LOG)) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    before_change(c);
    gl_texture *t = gl_bound_texture(c, c->tx.active);
    uint32_t *img = malloc(sizeof *img * (size_t)w * (size_t)h);
    if (!img) { gl_error(c, GL_OUT_OF_MEMORY); return; }
    if (pixels) convert(c, img, w, w, h, base, format, type, pixels);
    else memset(img, 0, sizeof *img * (size_t)w * (size_t)h);
    free(t->level[level]);
    t->level[level] = img;
    t->lw[level] = w;
    t->lh[level] = h;
    t->lbase[level] = base;
    if (level == 0) { t->base = base; t->internal = (GLenum)internal; }
}

void glTexSubImage2D(GLenum target, GLint level, GLint xo, GLint yo, GLsizei w, GLsizei h,
                     GLenum format, GLenum type, const GLvoid *pixels) {
    GET_CTX(c);
    NOT_IN_BEGIN(c);
    if (!check_target_type(c, target, format, type)) return;
    gl_texture *t = gl_bound_texture(c, c->tx.active);
    if (level < 0 || level > MAX_TEX_LOG || !t->level[level]) { gl_error(c, GL_INVALID_OPERATION); return; }
    if (w < 0 || h < 0 || xo < 0 || yo < 0 || xo + w > t->lw[level] || yo + h > t->lh[level]) { gl_error(c, GL_INVALID_VALUE); return; }
    if (!w || !h || !pixels) return;
    before_change(c);
    convert(c, t->level[level] + (size_t)yo * (size_t)t->lw[level] + xo, t->lw[level], w, h, t->lbase[level], format, type, pixels);
}

/* ---------------- parameters ---------------- */
static void tex_param(gl_context *c, GLenum target, GLenum pname, const float *v) {
    if (DL_ACTIVE(c)) {
        gl_dlarg a[6] = { { .u = target }, { .u = pname } };
        for (int i = 0; i < 4; i++) a[2 + i].f = v[i];
        if (gl_dl_record(c, OP_TEX_PARAMETER, 6, a)) return;
    }
    NOT_IN_BEGIN(c);
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_1D) { gl_error(c, GL_INVALID_ENUM); return; }
    gl_texture *t = gl_bound_texture(c, c->tx.active);
    GLenum e = (GLenum)v[0];
    before_change(c);
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
        if (e != GL_NEAREST && e != GL_LINEAR && e != GL_NEAREST_MIPMAP_NEAREST && e != GL_LINEAR_MIPMAP_NEAREST &&
            e != GL_NEAREST_MIPMAP_LINEAR && e != GL_LINEAR_MIPMAP_LINEAR) { gl_error(c, GL_INVALID_ENUM); return; }
        t->min_filter = e;
        break;
    case GL_TEXTURE_MAG_FILTER:
        if (e != GL_NEAREST && e != GL_LINEAR) { gl_error(c, GL_INVALID_ENUM); return; }
        t->mag_filter = e;
        break;
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
        if (e != GL_REPEAT && e != GL_CLAMP && e != GL_CLAMP_TO_EDGE) { gl_error(c, GL_INVALID_ENUM); return; }
        if (pname == GL_TEXTURE_WRAP_S) t->wrap_s = e; else t->wrap_t = e;
        break;
    case GL_TEXTURE_BORDER_COLOR: memcpy(t->border, v, sizeof t->border); break;
    case GL_TEXTURE_PRIORITY: t->priority = v[0]; break;
    case GL_TEXTURE_MAX_LEVEL: t->max_level = (int)v[0]; break;
    case GL_TEXTURE_BASE_LEVEL: case GL_TEXTURE_MIN_LOD: case GL_TEXTURE_MAX_LOD: break;   /* (accepted, not used) */
    default: gl_error(c, GL_INVALID_ENUM); return;
    }
}
void glTexParameteri(GLenum target, GLenum p, GLint v) { GET_CTX(c); float f[4] = { (float)v, 0, 0, 0 }; tex_param(c, target, p, f); }
void glTexParameterf(GLenum target, GLenum p, GLfloat v) { GET_CTX(c); float f[4] = { v, 0, 0, 0 }; tex_param(c, target, p, f); }
void glTexParameterfv(GLenum target, GLenum p, const GLfloat *v) {
    GET_CTX(c);
    float f[4] = { v[0], 0, 0, 0 };
    if (p == GL_TEXTURE_BORDER_COLOR) memcpy(f, v, sizeof f);
    tex_param(c, target, p, f);
}
void glTexParameteriv(GLenum target, GLenum p, const GLint *v) {
    GET_CTX(c);
    float f[4] = { (float)v[0], 0, 0, 0 };
    if (p == GL_TEXTURE_BORDER_COLOR) for (int i = 0; i < 4; i++) f[i] = (float)(v[i] / 2147483647.0);
    tex_param(c, target, p, f);
}

void glGetTexParameteriv(GLenum target, GLenum pname, GLint *v) {
    GET_CTX(c);
    gl_texture *t = gl_bound_texture(c, c->tx.active);
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: *v = (GLint)t->min_filter; break;
    case GL_TEXTURE_MAG_FILTER: *v = (GLint)t->mag_filter; break;
    case GL_TEXTURE_WRAP_S: *v = (GLint)t->wrap_s; break;
    case GL_TEXTURE_WRAP_T: *v = (GLint)t->wrap_t; break;
    case GL_TEXTURE_RESIDENT: *v = 1; break;
    default: gl_error(c, GL_INVALID_ENUM); break;
    }
    (void)target;
}

void glGetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *v) {
    GET_CTX(c);
    gl_texture *t = gl_bound_texture(c, c->tx.active);
    if (level < 0 || level > MAX_TEX_LOG) { gl_error(c, GL_INVALID_VALUE); return; }
    switch (pname) {
    case GL_TEXTURE_WIDTH: *v = t->lw[level]; break;
    case GL_TEXTURE_HEIGHT: *v = t->lh[level]; break;
    case GL_TEXTURE_INTERNAL_FORMAT: *v = t->level[level] ? (GLint)t->internal : 1; break;
    case GL_TEXTURE_BORDER: *v = 0; break;
    default: gl_error(c, GL_INVALID_ENUM); break;
    }
    (void)target;
}

/* ---------------- environment ---------------- */
static void tex_env(gl_context *c, GLenum target, GLenum pname, const float *v) {
    if (DL_ACTIVE(c)) {
        gl_dlarg a[6] = { { .u = target }, { .u = pname } };
        for (int i = 0; i < 4; i++) a[2 + i].f = v[i];
        if (gl_dl_record(c, OP_TEX_ENV, 6, a)) return;
    }
    NOT_IN_BEGIN(c);
    if (target != GL_TEXTURE_ENV) { gl_error(c, GL_INVALID_ENUM); return; }
    gl_texunit *u = &c->tx.unit[c->tx.active];
    if (pname == GL_TEXTURE_ENV_MODE) {
        GLenum m = (GLenum)v[0];
        if (m != GL_MODULATE && m != GL_REPLACE && m != GL_DECAL && m != GL_BLEND && m != GL_ADD) { gl_error(c, GL_INVALID_ENUM); return; }
        u->env_mode = m;
    } else if (pname == GL_TEXTURE_ENV_COLOR) {
        for (int i = 0; i < 4; i++) u->env_color[i] = GL_CLAMPF(v[i], 0.0f, 1.0f);
    } else { gl_error(c, GL_INVALID_ENUM); return; }
    gl_state_changed(c);
}
void glTexEnvi(GLenum target, GLenum p, GLint v) { GET_CTX(c); float f[4] = { (float)v, 0, 0, 0 }; tex_env(c, target, p, f); }
void glTexEnvf(GLenum target, GLenum p, GLfloat v) { GET_CTX(c); float f[4] = { v, 0, 0, 0 }; tex_env(c, target, p, f); }
void glTexEnvfv(GLenum target, GLenum p, const GLfloat *v) {
    GET_CTX(c);
    float f[4] = { v[0], 0, 0, 0 };
    if (p == GL_TEXTURE_ENV_COLOR) memcpy(f, v, sizeof f);
    tex_env(c, target, p, f);
}
void glTexEnviv(GLenum target, GLenum p, const GLint *v) {
    GET_CTX(c);
    float f[4] = { (float)v[0], 0, 0, 0 };
    if (p == GL_TEXTURE_ENV_COLOR) for (int i = 0; i < 4; i++) f[i] = (float)(v[i] / 2147483647.0);
    tex_env(c, target, p, f);
}

void glActiveTexture(GLenum unit) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { gl_dlarg a = { .u = unit }; if (gl_dl_record(c, OP_ACTIVE_TEXTURE, 1, &a)) return; }
    if (unit - GL_TEXTURE0 >= MAX_UNITS) { gl_error(c, GL_INVALID_ENUM); return; }
    c->tx.active = (int)(unit - GL_TEXTURE0);
}

/* ---------------- coordinate generation ---------------- */
static void tex_gen(gl_context *c, GLenum coord, GLenum pname, const float *v) {
    if (DL_ACTIVE(c)) {
        gl_dlarg a[6] = { { .u = coord }, { .u = pname } };
        for (int i = 0; i < 4; i++) a[2 + i].f = v[i];
        if (gl_dl_record(c, OP_TEX_GEN, 6, a)) return;
    }
    NOT_IN_BEGIN(c);
    unsigned k = coord - GL_S;
    if (k > 3) { gl_error(c, GL_INVALID_ENUM); return; }
    gl_texunit *u = &c->tx.unit[c->tx.active];
    switch (pname) {
    case GL_TEXTURE_GEN_MODE: {
        GLenum m = (GLenum)v[0];
        if (m != GL_OBJECT_LINEAR && m != GL_EYE_LINEAR && (m != GL_SPHERE_MAP || k > 1)) { gl_error(c, GL_INVALID_ENUM); return; }
        u->gen_mode[k] = m;
        break;
    }
    case GL_OBJECT_PLANE: memcpy(u->obj_plane[k], v, 16); break;
    case GL_EYE_PLANE: {
        /* kept in eye coordinates: p M^-1 */
        float inv[16];
        if (!mat_invert(inv, c->mv[c->mv_top].m)) { gl_error(c, GL_INVALID_OPERATION); return; }
        for (int j = 0; j < 4; j++)
            u->eye_plane[k][j] = v[0] * inv[j * 4 + 0] + v[1] * inv[j * 4 + 1] + v[2] * inv[j * 4 + 2] + v[3] * inv[j * 4 + 3];
        break;
    }
    default: gl_error(c, GL_INVALID_ENUM); return;
    }
}
void glTexGeni(GLenum coord, GLenum p, GLint v) { GET_CTX(c); float f[4] = { (float)v, 0, 0, 0 }; tex_gen(c, coord, p, f); }
void glTexGenf(GLenum coord, GLenum p, GLfloat v) { GET_CTX(c); float f[4] = { v, 0, 0, 0 }; tex_gen(c, coord, p, f); }
void glTexGenfv(GLenum coord, GLenum p, const GLfloat *v) {
    GET_CTX(c);
    float f[4] = { v[0], 0, 0, 0 };
    if (p != GL_TEXTURE_GEN_MODE) memcpy(f, v, sizeof f);
    tex_gen(c, coord, p, f);
}
void glTexGendv(GLenum coord, GLenum p, const GLdouble *v) {
    GET_CTX(c);
    float f[4] = { (float)v[0], 0, 0, 0 };
    if (p != GL_TEXTURE_GEN_MODE) for (int i = 0; i < 4; i++) f[i] = (float)v[i];
    tex_gen(c, coord, p, f);
}

/* ---------------- what the renderer needs ---------------- */
void gl_build_rs_unit(gl_context *c, int unit, gl_rs_unit *u) {
    memset(u, 0, sizeof *u);
    gl_texture *t = gl_bound_texture(c, unit);
    if (!t->level[0]) return;
    int mip = t->min_filter != GL_NEAREST && t->min_filter != GL_LINEAR;
    int n = 1;
    if (mip) {
        /* complete: every level down to 1 x 1, each half the one before, one format */
        int w = t->lw[0], h = t->lh[0], levels = ilog2(GL_MAX(w, h)) + 1;
        if (levels > t->max_level + 1) levels = t->max_level + 1;
        if (levels < 1) levels = 1;
        for (int l = 1; l < levels; l++) {
            int lw = GL_MAX(1, w >> l), lh = GL_MAX(1, h >> l);
            if (!t->level[l] || t->lw[l] != lw || t->lh[l] != lh || t->lbase[l] != t->base) return;   /* incomplete: off */
        }
        n = levels;
    }
    for (int l = 0; l < n; l++) {
        u->level[l] = t->level[l];
        u->wlog[l] = ilog2(t->lw[l]);
        u->hlog[l] = ilog2(t->lh[l]);
    }
    u->nlevels = n;
    u->min_linear = t->min_filter == GL_LINEAR || t->min_filter == GL_LINEAR_MIPMAP_NEAREST || t->min_filter == GL_LINEAR_MIPMAP_LINEAR;
    u->mag_linear = t->mag_filter == GL_LINEAR;
    u->mip = !mip ? 0 : (t->min_filter == GL_NEAREST_MIPMAP_LINEAR || t->min_filter == GL_LINEAR_MIPMAP_LINEAR) ? 2 : 1;
    if (n == 1 && mip) u->mip = 0;
    u->clamp_s = t->wrap_s != GL_REPEAT;
    u->clamp_t = t->wrap_t != GL_REPEAT;
    u->w0 = (float)t->lw[0];
    u->h0 = (float)t->lh[0];
    const gl_texunit *tu = &c->tx.unit[unit];
    u->env = (int)tu->env_mode;
    u->keep_rgb = t->base == BASE_ALPHA;
    u->keep_a = t->base == BASE_RGB || t->base == BASE_LUMINANCE;
    u->intensity = t->base == BASE_INTENSITY;
    for (int k = 0; k < 2; k++) {
        u->env_color[k * 4 + 0] = (uint16_t)(tu->env_color[2] * 255 + 0.5f);
        u->env_color[k * 4 + 1] = (uint16_t)(tu->env_color[1] * 255 + 0.5f);
        u->env_color[k * 4 + 2] = (uint16_t)(tu->env_color[0] * 255 + 0.5f);
        u->env_color[k * 4 + 3] = (uint16_t)(tu->env_color[3] * 255 + 0.5f);
    }
}
