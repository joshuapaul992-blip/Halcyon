/* Halcyon's 3D library: GLU-style helpers (GL/glu.h). Camera and
 * projection matrices, picking, project / unproject, image scaling and
 * mipmap building, error strings and the quadrics (sphere, cylinder,
 * disk). Written on the public GL calls only. */
#include "glint.h"
#include "GL/glu.h"

/* ---------------- matrices ---------------- */
void gluPerspective(GLdouble fovy, GLdouble aspect, GLdouble zn, GLdouble zf) {
    float s, co;
    sincosf((float)(fovy * M_PI / 360.0), &s, &co);
    if (s == 0 || aspect == 0 || zn == zf) return;
    double f = (double)co / s;
    GLdouble m[16] = { 0 };
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zf + zn) / (zn - zf);
    m[11] = -1;
    m[14] = 2 * zf * zn / (zn - zf);
    glMultMatrixd(m);
}

void gluOrtho2D(GLdouble l, GLdouble r, GLdouble b, GLdouble t) { glOrtho(l, r, b, t, -1, 1); }

static void norm3d(double *v) {
    double l = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
}
static void cross3d(const double *a, const double *b, double *o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

void gluLookAt(GLdouble ex, GLdouble ey, GLdouble ez, GLdouble cx, GLdouble cy, GLdouble cz,
               GLdouble ux, GLdouble uy, GLdouble uz) {
    double f[3] = { cx - ex, cy - ey, cz - ez }, up[3] = { ux, uy, uz }, s[3], u[3];
    norm3d(f);
    cross3d(f, up, s);
    norm3d(s);
    cross3d(s, f, u);
    GLdouble m[16] = { s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0, 0, 0, 0, 1 };
    glMultMatrixd(m);
    glTranslated(-ex, -ey, -ez);
}

void gluPickMatrix(GLdouble x, GLdouble y, GLdouble w, GLdouble h, const GLint vp[4]) {
    if (w <= 0 || h <= 0) return;
    glTranslated((vp[2] - 2 * (x - vp[0])) / w, (vp[3] - 2 * (y - vp[1])) / h, 0);
    glScaled(vp[2] / w, vp[3] / h, 1);
}

static void mul4d(const double *m, const double *in, double *out) {
    for (int r = 0; r < 4; r++) out[r] = m[r] * in[0] + m[4 + r] * in[1] + m[8 + r] * in[2] + m[12 + r] * in[3];
}
static void matmul4d(const double *a, const double *b, double *r) {
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[col * 4 + k];
            r[col * 4 + row] = s;
        }
}
/* Gauss-Jordan with partial pivoting */
static int invert4d(const double *m, double *out) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) { a[r][k] = m[k * 4 + r]; a[r][k + 4] = r == k; }
    for (int col = 0; col < 4; col++) {
        int p = col;
        for (int r = col + 1; r < 4; r++) if (fabs(a[r][col]) > fabs(a[p][col])) p = r;
        if (a[p][col] == 0) return 0;
        if (p != col) for (int k = 0; k < 8; k++) { double t = a[p][k]; a[p][k] = a[col][k]; a[col][k] = t; }
        double d = a[col][col];
        for (int k = 0; k < 8; k++) a[col][k] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == col) continue;
            double f = a[r][col];
            if (f != 0) for (int k = 0; k < 8; k++) a[r][k] -= f * a[col][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) out[k * 4 + r] = a[r][k + 4];
    return 1;
}

GLint gluProject(GLdouble ox, GLdouble oy, GLdouble oz, const GLdouble model[16], const GLdouble proj[16],
                 const GLint vp[4], GLdouble *wx, GLdouble *wy, GLdouble *wz) {
    double in[4] = { ox, oy, oz, 1 }, e[4], c[4];
    mul4d(model, in, e);
    mul4d(proj, e, c);
    if (c[3] == 0) return GL_FALSE;
    for (int i = 0; i < 3; i++) c[i] /= c[3];
    *wx = vp[0] + (c[0] + 1) * vp[2] / 2;
    *wy = vp[1] + (c[1] + 1) * vp[3] / 2;
    *wz = (c[2] + 1) / 2;
    return GL_TRUE;
}

GLint gluUnProject(GLdouble wx, GLdouble wy, GLdouble wz, const GLdouble model[16], const GLdouble proj[16],
                   const GLint vp[4], GLdouble *ox, GLdouble *oy, GLdouble *oz) {
    double pm[16], inv[16];
    matmul4d(proj, model, pm);
    if (!invert4d(pm, inv) || vp[2] == 0 || vp[3] == 0) return GL_FALSE;
    double in[4] = { (wx - vp[0]) * 2 / vp[2] - 1, (wy - vp[1]) * 2 / vp[3] - 1, 2 * wz - 1, 1 }, o[4];
    mul4d(inv, in, o);
    if (o[3] == 0) return GL_FALSE;
    *ox = o[0] / o[3];
    *oy = o[1] / o[3];
    *oz = o[2] / o[3];
    return GL_TRUE;
}

/* ---------------- images ---------------- */
static int comps_of(GLenum f) {
    switch (f) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE: return 1;
    case GL_LUMINANCE_ALPHA: return 2;
    case GL_RGB: case GL_BGR: return 3;
    case GL_RGBA: case GL_BGRA: return 4;
    }
    return 0;
}
static int type_bytes(GLenum t) { return t == GL_UNSIGNED_BYTE ? 1 : t == GL_FLOAT ? 4 : 0; }

static int row_bytes(int w, int bpp, int rowlen, int align) {
    int len = (rowlen > 0 ? rowlen : w) * bpp;
    return (len + align - 1) / align * align;
}

/* an image in the given format to floats (n components per pixel, 0..1) */
static float *read_image(GLenum format, GLenum type, int w, int h, const void *data, int unpack) {
    int n = comps_of(format), tb = type_bytes(type);
    GLint align = 4, rowlen = 0, skip_rows = 0, skip_px = 0;
    if (unpack) {
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowlen);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_px);
    } else align = 1;
    float *out = malloc(sizeof(float) * (size_t)w * (size_t)h * (size_t)n);
    if (!out) return NULL;
    int rb = row_bytes(w, n * tb, rowlen, align);
    const unsigned char *base = (const unsigned char *)data + (size_t)skip_rows * (size_t)rb + (size_t)skip_px * (size_t)(n * tb);
    for (int y = 0; y < h; y++) {
        const unsigned char *p = base + (size_t)y * (size_t)rb;
        float *o = out + (size_t)y * (size_t)w * (size_t)n;
        for (int i = 0; i < w * n; i++) {
            if (tb == 1) o[i] = p[i] * (1.0f / 255);
            else { float f; memcpy(&f, p + 4 * i, 4); o[i] = f; }
        }
    }
    return out;
}

/* 1D resampling of rows of floats: a box filter when shrinking, linear when growing */
static void resample(const float *in, int nin, float *out, int nout, int n, int sin_, int sout) {
    if (nout == nin) {
        for (int i = 0; i < nin; i++) for (int k = 0; k < n; k++) out[(size_t)i * sout + k] = in[(size_t)i * sin_ + k];
        return;
    }
    if (nout < nin) {
        double scale = (double)nin / nout;
        for (int i = 0; i < nout; i++) {
            double a = i * scale, b = a + scale;
            for (int k = 0; k < n; k++) {
                double s = 0;
                for (int j = (int)a; j < nin && j < b; j++) {
                    double lo = j < a ? a : j, hi = j + 1 > b ? b : j + 1;
                    s += in[(size_t)j * sin_ + k] * (hi - lo);
                }
                out[(size_t)i * sout + k] = (float)(s / scale);
            }
        }
        return;
    }
    for (int i = 0; i < nout; i++) {
        double x = (i + 0.5) * nin / nout - 0.5;
        int j = (int)floor(x);
        double f = x - j;
        int j0 = j < 0 ? 0 : j, j1 = j + 1 >= nin ? nin - 1 : j + 1;
        if (j0 >= nin) j0 = nin - 1;
        for (int k = 0; k < n; k++)
            out[(size_t)i * sout + k] = (float)(in[(size_t)j0 * sin_ + k] * (1 - f) + in[(size_t)j1 * sin_ + k] * f);
    }
}

static float *scale_floats(const float *in, int wi, int hi, int n, int wo, int ho) {
    float *tmp = malloc(sizeof(float) * (size_t)wo * (size_t)hi * (size_t)n);
    float *out = malloc(sizeof(float) * (size_t)wo * (size_t)ho * (size_t)n);
    if (!tmp || !out) { free(tmp); free(out); return NULL; }
    for (int y = 0; y < hi; y++) resample(in + (size_t)y * wi * n, wi, tmp + (size_t)y * wo * n, wo, n, n, n);
    for (int x = 0; x < wo; x++) resample(tmp + (size_t)x * n, hi, out + (size_t)x * n, ho, n, wo * n, wo * n);
    free(tmp);
    return out;
}

GLint gluScaleImage(GLenum format, GLsizei wi, GLsizei hi, GLenum typein, const void *datain,
                    GLsizei wo, GLsizei ho, GLenum typeout, GLvoid *dataout) {
    int n = comps_of(format);
    if (!n || !type_bytes(typein) || !type_bytes(typeout)) return GLU_INVALID_ENUM;
    if (wi <= 0 || hi <= 0 || wo <= 0 || ho <= 0) return wi < 0 || hi < 0 || wo < 0 || ho < 0 ? GLU_INVALID_VALUE : 0;
    float *in = read_image(format, typein, wi, hi, datain, 1);
    float *out = in ? scale_floats(in, wi, hi, n, wo, ho) : NULL;
    free(in);
    if (!out) return GLU_OUT_OF_MEMORY;
    GLint align = 4, rowlen = 0, skip_rows = 0, skip_px = 0;
    glGetIntegerv(GL_PACK_ALIGNMENT, &align);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &rowlen);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_px);
    int tb = type_bytes(typeout), rb = row_bytes(wo, n * tb, rowlen, align);
    unsigned char *base = (unsigned char *)dataout + (size_t)skip_rows * (size_t)rb + (size_t)skip_px * (size_t)(n * tb);
    for (int y = 0; y < ho; y++) {
        unsigned char *p = base + (size_t)y * (size_t)rb;
        const float *o = out + (size_t)y * wo * n;
        for (int i = 0; i < wo * n; i++) {
            float f = GL_CLAMPF(o[i], 0.0f, 1.0f);
            if (tb == 1) p[i] = (unsigned char)(f * 255 + 0.5f);
            else memcpy(p + 4 * i, &o[i], 4);
        }
    }
    free(out);
    return 0;
}

/* the power of two nearest to v (halfway: the larger one, as GLU does), at most lim */
static int pow2_near(int v, int lim) {
    int p = 1;
    while (p * 2 <= v) p *= 2;
    if (p * 2 <= lim && v - p >= p * 2 - v) p *= 2;
    return p > lim ? lim : p;
}

GLint gluBuild2DMipmaps(GLenum target, GLint internal, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *data) {
    int n = comps_of(format);
    if (target != GL_TEXTURE_2D || !n || !type_bytes(type)) return GLU_INVALID_ENUM;
    if (w < 1 || h < 1) return GLU_INVALID_VALUE;
    GLint maxsz = 2048;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxsz);
    int pw = pow2_near(w, maxsz), ph = pow2_near(h, maxsz);
    float *in = read_image(format, type, w, h, data, 1);
    float *lvl = in && (pw != w || ph != h) ? scale_floats(in, w, h, n, pw, ph) : in;
    if (lvl != in) free(in);
    if (!lvl) return GLU_OUT_OF_MEMORY;
    unsigned char *bytes = malloc((size_t)pw * (size_t)ph * (size_t)n);
    if (!bytes) { free(lvl); return GLU_OUT_OF_MEMORY; }
    /* our own images go up tightly packed, whatever the program's unpacking */
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    int lw = pw, lh = ph;
    for (int level = 0;; level++) {
        for (size_t i = 0; i < (size_t)lw * lh * n; i++) bytes[i] = (unsigned char)(GL_CLAMPF(lvl[i], 0.0f, 1.0f) * 255 + 0.5f);
        glTexImage2D(GL_TEXTURE_2D, level, internal, lw, lh, 0, format, GL_UNSIGNED_BYTE, bytes);
        if (lw == 1 && lh == 1) break;
        /* the next level: a 2 x 2 box (1 x 2 or 2 x 1 at the thin end) */
        int nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int k = 0; k < n; k++) {
                    int x0 = lw > 1 ? 2 * x : x, y0 = lh > 1 ? 2 * y : y;
                    int x1 = lw > 1 ? x0 + 1 : x0, y1 = lh > 1 ? y0 + 1 : y0;
                    float s = lvl[((size_t)y0 * lw + x0) * n + k] + lvl[((size_t)y0 * lw + x1) * n + k] +
                              lvl[((size_t)y1 * lw + x0) * n + k] + lvl[((size_t)y1 * lw + x1) * n + k];
                    lvl[((size_t)y * nw + x) * n + k] = s * 0.25f;   /* (in place: row y <= y0, x <= x0) */
                }
        lw = nw;
        lh = nh;
    }
    glPopClientAttrib();
    free(bytes);
    free(lvl);
    return 0;
}

/* ---------------- strings ---------------- */
const GLubyte *gluErrorString(GLenum e) {
    switch (e) {
    case GL_NO_ERROR: return (const GLubyte *)"no error";
    case GL_INVALID_ENUM: return (const GLubyte *)"invalid enumerant";
    case GL_INVALID_VALUE: return (const GLubyte *)"invalid value";
    case GL_INVALID_OPERATION: return (const GLubyte *)"invalid operation";
    case GL_STACK_OVERFLOW: return (const GLubyte *)"stack overflow";
    case GL_STACK_UNDERFLOW: return (const GLubyte *)"stack underflow";
    case GL_OUT_OF_MEMORY: return (const GLubyte *)"out of memory";
    case GLU_INVALID_ENUM: return (const GLubyte *)"invalid enumerant (GLU)";
    case GLU_INVALID_VALUE: return (const GLubyte *)"invalid value (GLU)";
    case GLU_OUT_OF_MEMORY: return (const GLubyte *)"out of memory (GLU)";
    }
    return NULL;
}

const GLubyte *gluGetString(GLenum name) {
    if (name == GLU_VERSION) return (const GLubyte *)"1.3 Halcyon";
    if (name == GLU_EXTENSIONS) return (const GLubyte *)"";
    return NULL;
}

/* ---------------- quadrics ---------------- */
struct GLUquadric { GLenum normals, orientation, style; GLboolean texture; };

GLUquadric *gluNewQuadric(void) {
    GLUquadric *q = malloc(sizeof *q);
    if (q) { q->normals = GLU_SMOOTH; q->orientation = GLU_OUTSIDE; q->style = GLU_FILL; q->texture = GL_FALSE; }
    return q;
}
void gluDeleteQuadric(GLUquadric *q) { free(q); }
void gluQuadricNormals(GLUquadric *q, GLenum n) { if (q) q->normals = n; }
void gluQuadricTexture(GLUquadric *q, GLboolean t) { if (q) q->texture = t; }
void gluQuadricOrientation(GLUquadric *q, GLenum o) { if (q) q->orientation = o; }
void gluQuadricDrawStyle(GLUquadric *q, GLenum s) { if (q) q->style = s; }

static void style_begin(const GLUquadric *q) {
    if (q->style == GLU_FILL) return;
    glPushAttrib(GL_POLYGON_BIT);
    glPolygonMode(GL_FRONT_AND_BACK, q->style == GLU_LINE ? GL_LINE : GL_POINT);
}
static void style_end(const GLUquadric *q) { if (q->style != GLU_FILL) glPopAttrib(); }

/* one vertex of a quadric: position, normal (flipped inside), texture coordinate */
static void qvert(const GLUquadric *q, float x, float y, float z, float nx, float ny, float nz, float s, float t) {
    if (q->normals != GLU_NONE) {
        if (q->orientation == GLU_INSIDE) glNormal3f(-nx, -ny, -nz);
        else glNormal3f(nx, ny, nz);
    }
    if (q->texture) glTexCoord2f(s, t);
    glVertex3f(x, y, z);
}

/* the sphere: around the z axis, stacks from +z to -z; s goes around, t
 * from 0 at -z to 1 at +z */
void gluSphere(GLUquadric *q, GLdouble radius, GLint slices, GLint stacks) {
    if (!q || slices < 2 || stacks < 1) return;
    float r = (float)radius;
    style_begin(q);
    for (int i = 0; i < stacks; i++) {
        float rho0 = (float)M_PI * i / stacks, rho1 = (float)M_PI * (i + 1) / stacks;
        float s0, c0, s1, c1;
        sincosf(rho0, &s0, &c0);
        sincosf(rho1, &s1, &c1);
        glBegin(GL_QUAD_STRIP);
        for (int j = 0; j <= slices; j++) {
            float th = j == slices ? 0 : 2 * (float)M_PI * j / slices, st, ct;
            sincosf(th, &st, &ct);
            float a[3] = { ct * s0, st * s0, c0 }, b[3] = { ct * s1, st * s1, c1 };
            float u = (float)j / slices;
            const float *p = a, *pq = b;
            float tp = 1 - (float)i / stacks, tq = 1 - (float)(i + 1) / stacks;
            if (q->orientation == GLU_INSIDE) { const float *t = p; p = pq; pq = t; float tt = tp; tp = tq; tq = tt; }
            qvert(q, p[0] * r, p[1] * r, p[2] * r, p[0], p[1], p[2], u, tp);
            qvert(q, pq[0] * r, pq[1] * r, pq[2] * r, pq[0], pq[1], pq[2], u, tq);
        }
        glEnd();
    }
    style_end(q);
}

/* the cylinder: along the z axis from 0 (radius base) to height (radius top) */
void gluCylinder(GLUquadric *q, GLdouble base, GLdouble top, GLdouble height, GLint slices, GLint stacks) {
    if (!q || slices < 2 || stacks < 1) return;
    float h = (float)height, rb = (float)base, rt = (float)top;
    /* the normal leans by the slope of the side */
    float len = sqrtf(h * h + (rb - rt) * (rb - rt));
    float nz = len > 0 ? (rb - rt) / len : 0, nxy = len > 0 ? h / len : 1;
    style_begin(q);
    for (int i = 0; i < stacks; i++) {
        float z0 = h * i / stacks, z1 = h * (i + 1) / stacks;
        float r0 = rb + (rt - rb) * i / stacks, r1 = rb + (rt - rb) * (i + 1) / stacks;
        glBegin(GL_QUAD_STRIP);
        for (int j = 0; j <= slices; j++) {
            float th = j == slices ? 0 : 2 * (float)M_PI * j / slices, st, ct;
            sincosf(th, &st, &ct);
            float u = (float)j / slices, t0 = (float)i / stacks, t1 = (float)(i + 1) / stacks;
            if (q->orientation == GLU_INSIDE) {
                qvert(q, ct * r0, st * r0, z0, ct * nxy, st * nxy, nz, u, t0);
                qvert(q, ct * r1, st * r1, z1, ct * nxy, st * nxy, nz, u, t1);
            } else {
                qvert(q, ct * r1, st * r1, z1, ct * nxy, st * nxy, nz, u, t1);
                qvert(q, ct * r0, st * r0, z0, ct * nxy, st * nxy, nz, u, t0);
            }
        }
        glEnd();
    }
    style_end(q);
}

/* the disk: in the z = 0 plane, facing +z, from radius inner to outer */
void gluDisk(GLUquadric *q, GLdouble inner, GLdouble outer, GLint slices, GLint loops) {
    if (!q || slices < 2 || loops < 1 || outer <= 0) return;
    float ri = (float)inner, ro = (float)outer;
    style_begin(q);
    for (int l = 0; l < loops; l++) {
        float a = ri + (ro - ri) * l / loops, b = ri + (ro - ri) * (l + 1) / loops;
        glBegin(GL_QUAD_STRIP);
        for (int j = 0; j <= slices; j++) {
            float th = j == slices ? 0 : 2 * (float)M_PI * j / slices, st, ct;
            sincosf(th, &st, &ct);
            float p = a, pq = b;
            if (q->orientation == GLU_INSIDE) { p = b; pq = a; }
            qvert(q, ct * p, st * p, 0, 0, 0, 1, 0.5f + ct * p / (2 * ro), 0.5f + st * p / (2 * ro));
            qvert(q, ct * pq, st * pq, 0, 0, 0, 1, 0.5f + ct * pq / (2 * ro), 0.5f + st * pq / (2 * ro));
        }
        glEnd();
    }
    style_end(q);
}
