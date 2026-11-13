/* Halcyon's 3D library: the matrix stacks (modelview, projection, one
 * texture stack per unit) and the matrices derived from them. */
#include "glint.h"

void mat_identity(glmat *a) {
    memset(a->m, 0, sizeof a->m);
    a->m[0] = a->m[5] = a->m[10] = a->m[15] = 1;
    a->identity = 1;
}

void mat_mul(glmat *r, const glmat *a, const glmat *b) {
    if (b->identity) { if (r != a) *r = *a; return; }
    if (a->identity) { if (r != b) *r = *b; return; }
    float t[16];
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a->m[k * 4 + row] * b->m[col * 4 + k];
            t[col * 4 + row] = s;
        }
    memcpy(r->m, t, sizeof t);
    r->identity = 0;
}

void mat_xform4(float o[4], const glmat *a, const float v[4]) {
    const float *m = a->m;
    float x = v[0], y = v[1], z = v[2], w = v[3];
    o[0] = m[0] * x + m[4] * y + m[8] * z + m[12] * w;
    o[1] = m[1] * x + m[5] * y + m[9] * z + m[13] * w;
    o[2] = m[2] * x + m[6] * y + m[10] * z + m[14] * w;
    o[3] = m[3] * x + m[7] * y + m[11] * z + m[15] * w;
}

/* general 4 x 4 inverse by cofactors (double inside) */
int mat_invert(float out[16], const float m[16]) {
    double a[16], inv[16];
    for (int i = 0; i < 16; i++) a[i] = m[i];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (det == 0) return 0;
    det = 1.0 / det;
    for (int i = 0; i < 16; i++) out[i] = (float)(inv[i] * det);
    return 1;
}

glmat *gl_top_matrix(gl_context *c) {
    switch (c->xf.matrix_mode) {
    case GL_PROJECTION: return &c->proj[c->proj_top];
    case GL_TEXTURE: return &c->texm[c->tx.active][c->tex_top[c->tx.active]];
    default: return &c->mv[c->mv_top];
    }
}

static void changed(gl_context *c) {
    if (c->xf.matrix_mode == GL_MODELVIEW) c->nmat_dirty = 1;
    if (c->xf.matrix_mode != GL_TEXTURE) c->mvp_dirty = 1;
}

/* the combined matrix and the normal matrix, when they are needed */
void gl_update_matrices(gl_context *c) {
    if (c->mvp_dirty) {
        mat_mul(&c->mvp, &c->proj[c->proj_top], &c->mv[c->mv_top]);
        c->mvp_dirty = 0;
    }
    if (c->nmat_dirty) {
        const float *m = c->mv[c->mv_top].m;
        /* inverse transpose of the upper 3 x 3 */
        double a = m[0], b = m[4], cc = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
        double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
        double det = a * A + b * B + cc * C;
        if (det == 0) det = 1;
        double inv = 1.0 / det;
        float *n = c->nmat;                /* n * v: rows of the inverse's transpose */
        n[0] = (float)(A * inv); n[1] = (float)(B * inv); n[2] = (float)(C * inv);
        n[3] = (float)(-(b * i - cc * h) * inv); n[4] = (float)((a * i - cc * g) * inv); n[5] = (float)(-(a * h - b * g) * inv);
        n[6] = (float)((b * f - cc * e) * inv); n[7] = (float)(-(a * f - cc * d) * inv); n[8] = (float)((a * e - b * d) * inv);
        /* GL_RESCALE_NORMAL: 1 / the length of the normal matrix's third row... of
         * the modelview's inverse, i.e. a uniform scale is undone */
        double l = sqrt((double)n[6] * n[6] + (double)n[7] * n[7] + (double)n[8] * n[8]);
        c->nscale = l > 0 ? (float)(1.0 / l) : 1;
        c->nmat_dirty = 0;
    }
}

static int record(gl_context *c, int op, int n, const float *f) {
    gl_dlarg a[16];
    for (int i = 0; i < n; i++) a[i].f = f[i];
    return gl_dl_record(c, op, n, a);
}

void glMatrixMode(GLenum mode) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { gl_dlarg a = { .u = mode }; if (gl_dl_record(c, OP_MATRIX_MODE, 1, &a)) return; }
    NOT_IN_BEGIN(c);
    if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE) { gl_error(c, GL_INVALID_ENUM); return; }
    c->xf.matrix_mode = mode;
}

void glLoadIdentity(void) {
    GET_CTX(c);
    if (DL_ACTIVE(c) && record(c, OP_LOAD_IDENTITY, 0, 0)) return;
    NOT_IN_BEGIN(c);
    mat_identity(gl_top_matrix(c));
    changed(c);
}

static void load(gl_context *c, const float *m, int mult) {
    if (DL_ACTIVE(c) && record(c, mult ? OP_MULT_MATRIX : OP_LOAD_MATRIX, 16, m)) return;
    NOT_IN_BEGIN(c);
    glmat t;
    memcpy(t.m, m, sizeof t.m);
    t.identity = 0;
    glmat *top = gl_top_matrix(c);
    if (mult) mat_mul(top, top, &t);
    else *top = t;
    changed(c);
}
void glLoadMatrixf(const GLfloat *m) { GET_CTX(c); load(c, m, 0); }
void glMultMatrixf(const GLfloat *m) { GET_CTX(c); load(c, m, 1); }
void glLoadMatrixd(const GLdouble *m) { GET_CTX(c); float f[16]; for (int i = 0; i < 16; i++) f[i] = (float)m[i]; load(c, f, 0); }
void glMultMatrixd(const GLdouble *m) { GET_CTX(c); float f[16]; for (int i = 0; i < 16; i++) f[i] = (float)m[i]; load(c, f, 1); }
void glLoadTransposeMatrixf(const GLfloat *m) { GET_CTX(c); float f[16]; for (int i = 0; i < 16; i++) f[i] = m[(i & 3) * 4 + (i >> 2)]; load(c, f, 0); }
void glMultTransposeMatrixf(const GLfloat *m) { GET_CTX(c); float f[16]; for (int i = 0; i < 16; i++) f[i] = m[(i & 3) * 4 + (i >> 2)]; load(c, f, 1); }
void glLoadTransposeMatrixd(const GLdouble *m) { GET_CTX(c); float f[16]; for (int i = 0; i < 16; i++) f[i] = (float)m[(i & 3) * 4 + (i >> 2)]; load(c, f, 0); }
void glMultTransposeMatrixd(const GLdouble *m) { GET_CTX(c); float f[16]; for (int i = 0; i < 16; i++) f[i] = (float)m[(i & 3) * 4 + (i >> 2)]; load(c, f, 1); }

void glPushMatrix(void) {
    GET_CTX(c);
    if (DL_ACTIVE(c) && record(c, OP_PUSH_MATRIX, 0, 0)) return;
    NOT_IN_BEGIN(c);
    int *top, depth;
    glmat *stack;
    switch (c->xf.matrix_mode) {
    case GL_PROJECTION: top = &c->proj_top; depth = PROJECTION_DEPTH; stack = c->proj; break;
    case GL_TEXTURE: top = &c->tex_top[c->tx.active]; depth = TEXTURE_DEPTH; stack = c->texm[c->tx.active]; break;
    default: top = &c->mv_top; depth = MODELVIEW_DEPTH; stack = c->mv; break;
    }
    if (*top + 1 >= depth) { gl_error(c, GL_STACK_OVERFLOW); return; }
    stack[*top + 1] = stack[*top];
    (*top)++;
}

void glPopMatrix(void) {
    GET_CTX(c);
    if (DL_ACTIVE(c) && record(c, OP_POP_MATRIX, 0, 0)) return;
    NOT_IN_BEGIN(c);
    int *top = c->xf.matrix_mode == GL_PROJECTION ? &c->proj_top :
               c->xf.matrix_mode == GL_TEXTURE ? &c->tex_top[c->tx.active] : &c->mv_top;
    if (*top == 0) { gl_error(c, GL_STACK_UNDERFLOW); return; }
    (*top)--;
    changed(c);
}

static void mult(gl_context *c, const float *m) {
    glmat t;
    memcpy(t.m, m, sizeof t.m);
    t.identity = 0;
    glmat *top = gl_top_matrix(c);
    mat_mul(top, top, &t);
    changed(c);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { float f[4] = { angle, x, y, z }; if (record(c, OP_ROTATE, 4, f)) return; }
    NOT_IN_BEGIN(c);
    float l = sqrtf(x * x + y * y + z * z);
    if (l == 0) return;
    x /= l; y /= l; z /= l;
    float s, co;
    sincosf(angle * (float)(M_PI / 180), &s, &co);
    float t = 1 - co;
    float m[16] = {
        x * x * t + co,     y * x * t + z * s, x * z * t - y * s, 0,
        x * y * t - z * s,  y * y * t + co,    y * z * t + x * s, 0,
        x * z * t + y * s,  y * z * t - x * s, z * z * t + co,    0,
        0, 0, 0, 1 };
    mult(c, m);
}
void glRotated(GLdouble a, GLdouble x, GLdouble y, GLdouble z) { glRotatef((float)a, (float)x, (float)y, (float)z); }

void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { float f[3] = { x, y, z }; if (record(c, OP_TRANSLATE, 3, f)) return; }
    NOT_IN_BEGIN(c);
    glmat *top = gl_top_matrix(c);
    float *m = top->m;                     /* m * T: only the last column changes */
    for (int i = 0; i < 4; i++) m[12 + i] += m[i] * x + m[4 + i] * y + m[8 + i] * z;
    top->identity = 0;
    changed(c);
}
void glTranslated(GLdouble x, GLdouble y, GLdouble z) { glTranslatef((float)x, (float)y, (float)z); }

void glScalef(GLfloat x, GLfloat y, GLfloat z) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { float f[3] = { x, y, z }; if (record(c, OP_SCALE, 3, f)) return; }
    NOT_IN_BEGIN(c);
    glmat *top = gl_top_matrix(c);
    float *m = top->m;
    for (int i = 0; i < 4; i++) { m[i] *= x; m[4 + i] *= y; m[8 + i] *= z; }
    top->identity = 0;
    changed(c);
}
void glScaled(GLdouble x, GLdouble y, GLdouble z) { glScalef((float)x, (float)y, (float)z); }

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { float a[6] = { (float)l, (float)r, (float)b, (float)t, (float)n, (float)f }; if (record(c, OP_FRUSTUM, 6, a)) return; }
    NOT_IN_BEGIN(c);
    if (n <= 0 || f <= 0 || l == r || b == t || n == f) { gl_error(c, GL_INVALID_VALUE); return; }
    float m[16] = { 0 };
    m[0] = (float)(2 * n / (r - l));
    m[5] = (float)(2 * n / (t - b));
    m[8] = (float)((r + l) / (r - l));
    m[9] = (float)((t + b) / (t - b));
    m[10] = (float)(-(f + n) / (f - n));
    m[11] = -1;
    m[14] = (float)(-2 * f * n / (f - n));
    mult(c, m);
}

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { float a[6] = { (float)l, (float)r, (float)b, (float)t, (float)n, (float)f }; if (record(c, OP_ORTHO, 6, a)) return; }
    NOT_IN_BEGIN(c);
    if (l == r || b == t || n == f) { gl_error(c, GL_INVALID_VALUE); return; }
    float m[16] = { 0 };
    m[0] = (float)(2 / (r - l));
    m[5] = (float)(2 / (t - b));
    m[10] = (float)(-2 / (f - n));
    m[12] = (float)(-(r + l) / (r - l));
    m[13] = (float)(-(t + b) / (t - b));
    m[14] = (float)(-(f + n) / (f - n));
    m[15] = 1;
    mult(c, m);
}

/* user clip planes: kept in eye coordinates (transformed by the inverse of
 * the modelview matrix when given) */
void glClipPlane(GLenum plane, const GLdouble *eq) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) {
        gl_dlarg a[5];
        a[0].u = plane;
        for (int i = 0; i < 4; i++) a[i + 1].f = (float)eq[i];
        if (gl_dl_record(c, OP_CLIP_PLANE, 5, a)) return;
    }
    NOT_IN_BEGIN(c);
    unsigned i = plane - GL_CLIP_PLANE0;
    if (i >= MAX_CLIP_PLANES) { gl_error(c, GL_INVALID_ENUM); return; }
    float inv[16];
    if (!mat_invert(inv, c->mv[c->mv_top].m)) { gl_error(c, GL_INVALID_OPERATION); return; }
    /* p_eye = p_obj * M^-1 (a row vector times the matrix) */
    for (int k = 0; k < 4; k++)
        c->xf.clip_plane[i][k] = (float)(eq[0] * inv[k * 4 + 0] + eq[1] * inv[k * 4 + 1] + eq[2] * inv[k * 4 + 2] + eq[3] * inv[k * 4 + 3]);
}

void glGetClipPlane(GLenum plane, GLdouble *eq) {
    GET_CTX(c);
    unsigned i = plane - GL_CLIP_PLANE0;
    if (i >= MAX_CLIP_PLANES) { gl_error(c, GL_INVALID_ENUM); return; }
    for (int k = 0; k < 4; k++) eq[k] = c->xf.clip_plane[i][k];
}
