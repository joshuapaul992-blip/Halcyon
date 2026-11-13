/* Halcyon's 3D library: the state setters. The frame buffer's operations
 * (clear values, masks, depth, alpha test, blending), rasterization
 * (culling, polygon modes and offset, shading, line and point sizes), the
 * viewport and scissor box, lights, materials and fog. Calls that change
 * what the tile renderer needs per pixel start a new state snapshot. */
#include "glint.h"

static int rec(gl_context *c, int op, int n, const unsigned *u, const float *f, int nu) {
    gl_dlarg a[8];
    for (int i = 0; i < n; i++) {
        if (i < nu) a[i].u = u[i];
        else a[i].f = f[i - nu];
    }
    return gl_dl_record(c, op, n, a);
}
#define REC_U(op, ...) do { if (DL_ACTIVE(c)) { unsigned u_[] = { __VA_ARGS__ }; \
    if (rec(c, op, (int)(sizeof u_ / sizeof u_[0]), u_, NULL, (int)(sizeof u_ / sizeof u_[0]))) return; } } while (0)
#define REC_F(op, ...) do { if (DL_ACTIVE(c)) { float f_[] = { __VA_ARGS__ }; \
    if (rec(c, op, (int)(sizeof f_ / sizeof f_[0]), NULL, f_, 0)) return; } } while (0)

static int is_func(GLenum f) { return f >= GL_NEVER && f <= GL_ALWAYS; }

/* ---------------- frame buffer operations ---------------- */
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
    GET_CTX(c);
    REC_F(OP_CLEAR_COLOR, r, g, b, a);
    NOT_IN_BEGIN(c);
    c->cb.clear[0] = GL_CLAMPF(r, 0.0f, 1.0f); c->cb.clear[1] = GL_CLAMPF(g, 0.0f, 1.0f);
    c->cb.clear[2] = GL_CLAMPF(b, 0.0f, 1.0f); c->cb.clear[3] = GL_CLAMPF(a, 0.0f, 1.0f);
}

void glClearDepth(GLclampd d) {
    GET_CTX(c);
    REC_F(OP_CLEAR_DEPTH, (float)d);
    NOT_IN_BEGIN(c);
    c->db.clear = (float)GL_CLAMPF(d, 0.0, 1.0);
}

void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
    GET_CTX(c);
    REC_U(OP_COLOR_MASK, r, g, b, a);
    NOT_IN_BEGIN(c);
    c->cb.mask[0] = r != 0; c->cb.mask[1] = g != 0; c->cb.mask[2] = b != 0; c->cb.mask[3] = a != 0;
    gl_state_changed(c);
}

void glDepthMask(GLboolean flag) {
    GET_CTX(c);
    REC_U(OP_DEPTH_MASK, flag);
    NOT_IN_BEGIN(c);
    c->db.mask = flag != 0;
    gl_state_changed(c);
}

void glDepthFunc(GLenum func) {
    GET_CTX(c);
    REC_U(OP_DEPTH_FUNC, func);
    NOT_IN_BEGIN(c);
    if (!is_func(func)) { gl_error(c, GL_INVALID_ENUM); return; }
    c->db.func = func;
    gl_state_changed(c);
}

void glDepthRange(GLclampd n, GLclampd f) {
    GET_CTX(c);
    REC_F(OP_DEPTH_RANGE, (float)n, (float)f);
    NOT_IN_BEGIN(c);
    c->vp.znear = (float)GL_CLAMPF(n, 0.0, 1.0);
    c->vp.zfar = (float)GL_CLAMPF(f, 0.0, 1.0);
}

void glAlphaFunc(GLenum func, GLclampf ref) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { unsigned u = func; float f = ref; gl_dlarg a[2]; a[0].u = u; a[1].f = f; if (gl_dl_record(c, OP_ALPHA_FUNC, 2, a)) return; }
    NOT_IN_BEGIN(c);
    if (!is_func(func)) { gl_error(c, GL_INVALID_ENUM); return; }
    c->cb.alpha_func = func;
    c->cb.alpha_ref = GL_CLAMPF(ref, 0.0f, 1.0f);
    gl_state_changed(c);
}

static int is_blend_factor(GLenum f, int src) {
    switch (f) {
    case GL_ZERO: case GL_ONE: case GL_SRC_ALPHA: case GL_ONE_MINUS_SRC_ALPHA: case GL_DST_ALPHA: case GL_ONE_MINUS_DST_ALPHA:
        return 1;
    case GL_DST_COLOR: case GL_ONE_MINUS_DST_COLOR: case GL_SRC_ALPHA_SATURATE: return src;
    case GL_SRC_COLOR: case GL_ONE_MINUS_SRC_COLOR: return !src;
    }
    return 0;
}

void glBlendFunc(GLenum s, GLenum d) {
    GET_CTX(c);
    REC_U(OP_BLEND_FUNC, s, d);
    NOT_IN_BEGIN(c);
    if (!is_blend_factor(s, 1) || !is_blend_factor(d, 0)) { gl_error(c, GL_INVALID_ENUM); return; }
    c->cb.blend_src = s;
    c->cb.blend_dst = d;
    gl_state_changed(c);
}

void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    GET_CTX(c);
    REC_U(OP_SCISSOR, (unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h);
    NOT_IN_BEGIN(c);
    if (w < 0 || h < 0) { gl_error(c, GL_INVALID_VALUE); return; }
    c->sc.x = x; c->sc.y = y; c->sc.w = w; c->sc.h = h;
    gl_update_clip_rect(c);
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    GET_CTX(c);
    REC_U(OP_VIEWPORT, (unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h);
    NOT_IN_BEGIN(c);
    if (w < 0 || h < 0) { gl_error(c, GL_INVALID_VALUE); return; }
    c->vp.x = GL_CLAMPF(x, -MAX_FB, MAX_FB);
    c->vp.y = GL_CLAMPF(y, -MAX_FB, MAX_FB);
    c->vp.w = GL_MIN(w, MAX_FB);
    c->vp.h = GL_MIN(h, MAX_FB);
}

/* ---------------- rasterization ---------------- */
void glCullFace(GLenum mode) {
    GET_CTX(c);
    REC_U(OP_CULL_FACE, mode);
    NOT_IN_BEGIN(c);
    if (mode != GL_FRONT && mode != GL_BACK && mode != GL_FRONT_AND_BACK) { gl_error(c, GL_INVALID_ENUM); return; }
    c->poly.cull = mode;
}

void glFrontFace(GLenum mode) {
    GET_CTX(c);
    REC_U(OP_FRONT_FACE, mode);
    NOT_IN_BEGIN(c);
    if (mode != GL_CW && mode != GL_CCW) { gl_error(c, GL_INVALID_ENUM); return; }
    c->poly.front = mode;
}

void glPolygonMode(GLenum face, GLenum mode) {
    GET_CTX(c);
    REC_U(OP_POLYGON_MODE, face, mode);
    NOT_IN_BEGIN(c);
    if ((face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) || (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL)) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (face != GL_BACK) c->poly.mode[0] = mode;
    if (face != GL_FRONT) c->poly.mode[1] = mode;
}

void glPolygonOffset(GLfloat factor, GLfloat units) {
    GET_CTX(c);
    REC_F(OP_POLYGON_OFFSET, factor, units);
    NOT_IN_BEGIN(c);
    c->poly.offset_factor = factor;
    c->poly.offset_units = units;
}

void glShadeModel(GLenum mode) {
    GET_CTX(c);
    REC_U(OP_SHADE_MODEL, mode);
    NOT_IN_BEGIN(c);
    if (mode != GL_FLAT && mode != GL_SMOOTH) { gl_error(c, GL_INVALID_ENUM); return; }
    c->lt.shade_model = mode;
}

void glLineWidth(GLfloat w) {
    GET_CTX(c);
    REC_F(OP_LINE_WIDTH, w);
    NOT_IN_BEGIN(c);
    if (w <= 0) { gl_error(c, GL_INVALID_VALUE); return; }
    c->line_width = GL_MIN(w, 64.0f);
}

void glPointSize(GLfloat s) {
    GET_CTX(c);
    REC_F(OP_POINT_SIZE, s);
    NOT_IN_BEGIN(c);
    if (s <= 0) { gl_error(c, GL_INVALID_VALUE); return; }
    c->point_size = GL_MIN(s, 64.0f);
}

/* ---------------- lights ---------------- */
static void xform_dir3(const float *m, const float *in, float *out) {
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8] * in[2];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9] * in[2];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2];
}

static int light_count(GLenum p) {
    switch (p) {
    case GL_AMBIENT: case GL_DIFFUSE: case GL_SPECULAR: case GL_POSITION: return 4;
    case GL_SPOT_DIRECTION: return 3;
    case GL_SPOT_EXPONENT: case GL_SPOT_CUTOFF: case GL_CONSTANT_ATTENUATION: case GL_LINEAR_ATTENUATION:
    case GL_QUADRATIC_ATTENUATION: return 1;
    }
    return 0;
}

static void light(gl_context *c, GLenum li, GLenum p, const float *v) {
    if (DL_ACTIVE(c)) {
        gl_dlarg a[6];
        a[0].u = li; a[1].u = p;
        for (int i = 0; i < 4; i++) a[2 + i].f = i < light_count(p) ? v[i] : 0;
        if (gl_dl_record(c, OP_LIGHT, 6, a)) return;
    }
    NOT_IN_BEGIN(c);
    unsigned i = li - GL_LIGHT0;
    if (i >= MAX_LIGHTS || !light_count(p)) { gl_error(c, GL_INVALID_ENUM); return; }
    gl_light *L = &c->lt.light[i];
    const float *mv = c->mv[c->mv_top].m;
    switch (p) {
    case GL_AMBIENT: memcpy(L->ambient, v, 16); break;
    case GL_DIFFUSE: memcpy(L->diffuse, v, 16); break;
    case GL_SPECULAR: memcpy(L->specular, v, 16); break;
    case GL_POSITION: {                    /* to eye coordinates now */
        glmat t; memcpy(t.m, mv, sizeof t.m); t.identity = 0;
        mat_xform4(L->pos, &t, v);
        break;
    }
    case GL_SPOT_DIRECTION: {
        float d[3];
        xform_dir3(mv, v, d);
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l > 0) { d[0] /= l; d[1] /= l; d[2] /= l; }
        memcpy(L->spot_dir, d, sizeof d);
        break;
    }
    case GL_SPOT_EXPONENT:
        if (v[0] < 0 || v[0] > 128) { gl_error(c, GL_INVALID_VALUE); return; }
        L->spot_exp = v[0];
        break;
    case GL_SPOT_CUTOFF:
        if ((v[0] < 0 || v[0] > 90) && v[0] != 180) { gl_error(c, GL_INVALID_VALUE); return; }
        L->spot_cutoff = v[0];
        L->spot_cos = v[0] == 180 ? -1 : cosf(v[0] * (float)(M_PI / 180));
        break;
    case GL_CONSTANT_ATTENUATION: case GL_LINEAR_ATTENUATION: case GL_QUADRATIC_ATTENUATION:
        if (v[0] < 0) { gl_error(c, GL_INVALID_VALUE); return; }
        L->att[p - GL_CONSTANT_ATTENUATION] = v[0];
        break;
    }
}
void glLightfv(GLenum li, GLenum p, const GLfloat *v) { GET_CTX(c); light(c, li, p, v); }
void glLightf(GLenum li, GLenum p, GLfloat v) {
    GET_CTX(c);
    if (light_count(p) != 1) { gl_error(c, GL_INVALID_ENUM); return; }
    float f[4] = { v, 0, 0, 0 };
    light(c, li, p, f);
}
void glLighti(GLenum li, GLenum p, GLint v) { glLightf(li, p, (float)v); }
void glLightiv(GLenum li, GLenum p, const GLint *v) {
    GET_CTX(c);
    float f[4] = { 0, 0, 0, 0 };
    int n = light_count(p);
    for (int i = 0; i < n; i++)
        f[i] = (p == GL_AMBIENT || p == GL_DIFFUSE || p == GL_SPECULAR) ? (float)(v[i] / 2147483647.0) : (float)v[i];
    light(c, li, p, f);
}

void glGetLightfv(GLenum li, GLenum p, GLfloat *v) {
    GET_CTX(c);
    unsigned i = li - GL_LIGHT0;
    if (i >= MAX_LIGHTS) { gl_error(c, GL_INVALID_ENUM); return; }
    const gl_light *L = &c->lt.light[i];
    switch (p) {
    case GL_AMBIENT: memcpy(v, L->ambient, 16); break;
    case GL_DIFFUSE: memcpy(v, L->diffuse, 16); break;
    case GL_SPECULAR: memcpy(v, L->specular, 16); break;
    case GL_POSITION: memcpy(v, L->pos, 16); break;
    case GL_SPOT_DIRECTION: memcpy(v, L->spot_dir, 12); break;
    case GL_SPOT_EXPONENT: v[0] = L->spot_exp; break;
    case GL_SPOT_CUTOFF: v[0] = L->spot_cutoff; break;
    case GL_CONSTANT_ATTENUATION: case GL_LINEAR_ATTENUATION: case GL_QUADRATIC_ATTENUATION:
        v[0] = L->att[p - GL_CONSTANT_ATTENUATION];
        break;
    default: gl_error(c, GL_INVALID_ENUM); break;
    }
}

static void light_model(gl_context *c, GLenum p, const float *v) {
    if (DL_ACTIVE(c)) {
        gl_dlarg a[5];
        a[0].u = p;
        for (int i = 0; i < 4; i++) a[1 + i].f = p == GL_LIGHT_MODEL_AMBIENT || i == 0 ? v[i] : 0;
        if (gl_dl_record(c, OP_LIGHT_MODEL, 5, a)) return;
    }
    NOT_IN_BEGIN(c);
    switch (p) {
    case GL_LIGHT_MODEL_AMBIENT: memcpy(c->lt.model_ambient, v, 16); break;
    case GL_LIGHT_MODEL_LOCAL_VIEWER: c->lt.local_viewer = v[0] != 0; break;
    case GL_LIGHT_MODEL_TWO_SIDE: c->lt.two_side = v[0] != 0; break;
    case GL_LIGHT_MODEL_COLOR_CONTROL: {
        GLenum m = (GLenum)v[0];
        if (m != GL_SINGLE_COLOR && m != GL_SEPARATE_SPECULAR_COLOR) { gl_error(c, GL_INVALID_ENUM); return; }
        c->lt.color_control = m;
        gl_state_changed(c);
        break;
    }
    default: gl_error(c, GL_INVALID_ENUM); break;
    }
}
void glLightModelfv(GLenum p, const GLfloat *v) {
    GET_CTX(c);
    float f[4] = { v[0], 0, 0, 0 };
    if (p == GL_LIGHT_MODEL_AMBIENT) memcpy(f, v, sizeof f);
    light_model(c, p, f);
}
void glLightModelf(GLenum p, GLfloat v) { GET_CTX(c); float f[4] = { v, 0, 0, 0 }; light_model(c, p, f); }
void glLightModeli(GLenum p, GLint v) { GET_CTX(c); float f[4] = { (float)v, 0, 0, 0 }; light_model(c, p, f); }
void glLightModeliv(GLenum p, const GLint *v) {
    GET_CTX(c);
    float f[4] = { (float)v[0], 0, 0, 0 };
    if (p == GL_LIGHT_MODEL_AMBIENT) for (int i = 0; i < 4; i++) f[i] = (float)(v[i] / 2147483647.0);
    light_model(c, p, f);
}

/* ---------------- materials ---------------- */
static void material(gl_context *c, GLenum face, GLenum p, const float *v) {
    if (DL_ACTIVE(c)) {
        gl_dlarg a[6];
        a[0].u = face; a[1].u = p;
        for (int i = 0; i < 4; i++) a[2 + i].f = p == GL_SHININESS && i ? 0 : v[i];
        if (gl_dl_record(c, OP_MATERIAL, 6, a)) return;
    }
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) { gl_error(c, GL_INVALID_ENUM); return; }
    /* (allowed between glBegin and glEnd: the vertices of a primitive are
     * lit when it ends, so the last material of the primitive is used) */
    for (int f = 0; f < 2; f++) {
        if ((f == 0 && face == GL_BACK) || (f == 1 && face == GL_FRONT)) continue;
        gl_material *m = &c->lt.mat[f];
        switch (p) {
        case GL_AMBIENT: memcpy(m->ambient, v, 16); break;
        case GL_DIFFUSE: memcpy(m->diffuse, v, 16); break;
        case GL_SPECULAR: memcpy(m->specular, v, 16); break;
        case GL_EMISSION: memcpy(m->emission, v, 16); break;
        case GL_AMBIENT_AND_DIFFUSE: memcpy(m->ambient, v, 16); memcpy(m->diffuse, v, 16); break;
        case GL_SHININESS:
            if (v[0] < 0 || v[0] > 128) { gl_error(c, GL_INVALID_VALUE); return; }
            m->shininess = v[0];
            break;
        default: gl_error(c, GL_INVALID_ENUM); return;
        }
    }
}
void glMaterialfv(GLenum face, GLenum p, const GLfloat *v) {
    GET_CTX(c);
    float f[4] = { v[0], 0, 0, 0 };
    if (p != GL_SHININESS) memcpy(f, v, sizeof f);
    material(c, face, p, f);
}
void glMaterialf(GLenum face, GLenum p, GLfloat v) {
    GET_CTX(c);
    if (p != GL_SHININESS) { gl_error(c, GL_INVALID_ENUM); return; }
    float f[4] = { v, 0, 0, 0 };
    material(c, face, p, f);
}
void glMateriali(GLenum face, GLenum p, GLint v) { glMaterialf(face, p, (float)v); }
void glMaterialiv(GLenum face, GLenum p, const GLint *v) {
    GET_CTX(c);
    float f[4] = { (float)v[0], 0, 0, 0 };
    if (p != GL_SHININESS) for (int i = 0; i < 4; i++) f[i] = (float)(v[i] / 2147483647.0);
    material(c, face, p, f);
}

void glGetMaterialfv(GLenum face, GLenum p, GLfloat *v) {
    GET_CTX(c);
    if (face != GL_FRONT && face != GL_BACK) { gl_error(c, GL_INVALID_ENUM); return; }
    const gl_material *m = &c->lt.mat[face == GL_BACK];
    switch (p) {
    case GL_AMBIENT: memcpy(v, m->ambient, 16); break;
    case GL_DIFFUSE: memcpy(v, m->diffuse, 16); break;
    case GL_SPECULAR: memcpy(v, m->specular, 16); break;
    case GL_EMISSION: memcpy(v, m->emission, 16); break;
    case GL_SHININESS: v[0] = m->shininess; break;
    default: gl_error(c, GL_INVALID_ENUM); break;
    }
}

void glColorMaterial(GLenum face, GLenum mode) {
    GET_CTX(c);
    REC_U(OP_COLOR_MATERIAL, face, mode);
    NOT_IN_BEGIN(c);
    if ((face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) ||
        (mode != GL_EMISSION && mode != GL_AMBIENT && mode != GL_DIFFUSE && mode != GL_SPECULAR && mode != GL_AMBIENT_AND_DIFFUSE)) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    c->lt.cm_face = face;
    c->lt.cm_mode = mode;
}

/* ---------------- fog ---------------- */
static void fog(gl_context *c, GLenum p, const float *v) {
    if (DL_ACTIVE(c)) {
        gl_dlarg a[5];
        a[0].u = p;
        for (int i = 0; i < 4; i++) a[1 + i].f = p == GL_FOG_COLOR || i == 0 ? v[i] : 0;
        if (gl_dl_record(c, OP_FOG, 5, a)) return;
    }
    NOT_IN_BEGIN(c);
    switch (p) {
    case GL_FOG_MODE: {
        GLenum m = (GLenum)v[0];
        if (m != GL_LINEAR && m != GL_EXP && m != GL_EXP2) { gl_error(c, GL_INVALID_ENUM); return; }
        c->fog.mode = m;
        break;
    }
    case GL_FOG_DENSITY:
        if (v[0] < 0) { gl_error(c, GL_INVALID_VALUE); return; }
        c->fog.density = v[0];
        break;
    case GL_FOG_START: c->fog.start = v[0]; break;
    case GL_FOG_END: c->fog.end = v[0]; break;
    case GL_FOG_COLOR:
        for (int i = 0; i < 4; i++) c->fog.color[i] = GL_CLAMPF(v[i], 0.0f, 1.0f);
        gl_state_changed(c);
        break;
    case GL_FOG_INDEX: break;
    default: gl_error(c, GL_INVALID_ENUM); break;
    }
}
void glFogfv(GLenum p, const GLfloat *v) {
    GET_CTX(c);
    float f[4] = { v[0], 0, 0, 0 };
    if (p == GL_FOG_COLOR) memcpy(f, v, sizeof f);
    fog(c, p, f);
}
void glFogf(GLenum p, GLfloat v) { GET_CTX(c); float f[4] = { v, 0, 0, 0 }; fog(c, p, f); }
void glFogi(GLenum p, GLint v) { GET_CTX(c); float f[4] = { (float)v, 0, 0, 0 }; fog(c, p, f); }
void glFogiv(GLenum p, const GLint *v) {
    GET_CTX(c);
    float f[4] = { (float)v[0], 0, 0, 0 };
    if (p == GL_FOG_COLOR) for (int i = 0; i < 4; i++) f[i] = (float)(v[i] / 2147483647.0);
    fog(c, p, f);
}
