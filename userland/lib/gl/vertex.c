/* Halcyon's 3D library: vertices. glBegin / glEnd and the vertex arrays
 * collect raw vertices; gl_process transforms them (modelview, projection,
 * clip codes), lights them (up to 8 directional, point and spot lights,
 * materials, color material, two-sided), generates and transforms texture
 * coordinates (texgen, texture matrices) and computes the fog factor. */
#include "glint.h"

/* ---------------- immediate mode ---------------- */
static int is_prim(GLenum m) { return m <= GL_POLYGON; }

void glBegin(GLenum mode) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { gl_dlarg a = { .u = mode }; if (gl_dl_record(c, OP_BEGIN, 1, &a)) return; }
    if (c->in_begin) { gl_error(c, GL_INVALID_OPERATION); return; }
    if (!is_prim(mode)) { gl_error(c, GL_INVALID_ENUM); return; }
    c->in_begin = 1;
    c->prim = mode;
    c->nraw = 0;
}

void glEnd(void) {
    GET_CTX(c);
    if (DL_ACTIVE(c) && gl_dl_record(c, OP_END, 0, 0)) return;
    if (!c->in_begin) { gl_error(c, GL_INVALID_OPERATION); return; }
    c->in_begin = 0;
    gl_flush_prims(c);
    c->nraw = 0;
}

int gl_reserve_vtx(gl_context *c, int n) {
    if (n <= c->vtx_cap) return 1;
    int cap = c->vtx_cap ? c->vtx_cap : 256;
    while (cap < n) cap *= 2;
    gl_vertex *v = realloc(c->vtx, sizeof *v * (size_t)cap);
    if (!v) { gl_error(c, GL_OUT_OF_MEMORY); return 0; }
    c->vtx = v;
    c->vtx_cap = cap;
    return 1;
}

void gl_flush_prims(gl_context *c) {
    if (!c->nraw || !gl_reserve_vtx(c, c->nraw)) return;
    gl_process(c, c->raw, c->nraw, c->vtx);
    gl_draw(c, c->prim, c->vtx, NULL, c->nraw);
}

void gl_vertex4(gl_context *c, float x, float y, float z, float w) {
    if (DL_ACTIVE(c)) {
        gl_dlarg a[4] = { { .f = x }, { .f = y }, { .f = z }, { .f = w } };
        if (gl_dl_record(c, OP_VERTEX, 4, a)) return;
    }
    if (!c->in_begin) return;                 /* (undefined in GL; ignored) */
    if (c->nraw == c->raw_cap) {
        int cap = c->raw_cap ? c->raw_cap * 2 : 1024;
        gl_rawvert *r = realloc(c->raw, sizeof *r * (size_t)cap);
        if (!r) { gl_error(c, GL_OUT_OF_MEMORY); return; }
        c->raw = r;
        c->raw_cap = cap;
    }
    gl_rawvert *v = &c->raw[c->nraw++];
    v->obj[0] = x; v->obj[1] = y; v->obj[2] = z; v->obj[3] = w;
    memcpy(v->col, c->cur.color, sizeof v->col);
    memcpy(v->nrm, c->cur.normal, sizeof v->nrm);
    memcpy(v->tex, c->cur.tex, sizeof v->tex);
    v->edge = c->cur.edge;
}

void gl_color4(gl_context *c, float r, float g, float b, float a) {
    if (DL_ACTIVE(c)) {
        gl_dlarg x[4] = { { .f = r }, { .f = g }, { .f = b }, { .f = a } };
        if (gl_dl_record(c, OP_COLOR, 4, x)) return;
    }
    c->cur.color[0] = r; c->cur.color[1] = g; c->cur.color[2] = b; c->cur.color[3] = a;
}

void gl_normal3(gl_context *c, float x, float y, float z) {
    if (DL_ACTIVE(c)) {
        gl_dlarg a[3] = { { .f = x }, { .f = y }, { .f = z } };
        if (gl_dl_record(c, OP_NORMAL, 3, a)) return;
    }
    c->cur.normal[0] = x; c->cur.normal[1] = y; c->cur.normal[2] = z;
}

void gl_texcoord4(gl_context *c, int unit, float s, float t, float r, float q) {
    if (DL_ACTIVE(c)) {
        gl_dlarg a[5] = { { .i = unit }, { .f = s }, { .f = t }, { .f = r }, { .f = q } };
        if (gl_dl_record(c, OP_TEXCOORD, 5, a)) return;
    }
    if ((unsigned)unit >= MAX_UNITS) { gl_error(c, GL_INVALID_ENUM); return; }
    float *v = c->cur.tex[unit];
    v[0] = s; v[1] = t; v[2] = r; v[3] = q;
}

#define C_ gl_context *c = gl_cur; if (!c) return
void glVertex2f(GLfloat x, GLfloat y) { C_; gl_vertex4(c, x, y, 0, 1); }
void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { C_; gl_vertex4(c, x, y, z, 1); }
void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w) { C_; gl_vertex4(c, x, y, z, w); }
void glVertex2d(GLdouble x, GLdouble y) { C_; gl_vertex4(c, (float)x, (float)y, 0, 1); }
void glVertex3d(GLdouble x, GLdouble y, GLdouble z) { C_; gl_vertex4(c, (float)x, (float)y, (float)z, 1); }
void glVertex4d(GLdouble x, GLdouble y, GLdouble z, GLdouble w) { C_; gl_vertex4(c, (float)x, (float)y, (float)z, (float)w); }
void glVertex2i(GLint x, GLint y) { C_; gl_vertex4(c, (float)x, (float)y, 0, 1); }
void glVertex3i(GLint x, GLint y, GLint z) { C_; gl_vertex4(c, (float)x, (float)y, (float)z, 1); }
void glVertex2s(GLshort x, GLshort y) { C_; gl_vertex4(c, x, y, 0, 1); }
void glVertex3s(GLshort x, GLshort y, GLshort z) { C_; gl_vertex4(c, x, y, z, 1); }
void glVertex2fv(const GLfloat *v) { C_; gl_vertex4(c, v[0], v[1], 0, 1); }
void glVertex3fv(const GLfloat *v) { C_; gl_vertex4(c, v[0], v[1], v[2], 1); }
void glVertex4fv(const GLfloat *v) { C_; gl_vertex4(c, v[0], v[1], v[2], v[3]); }
void glVertex2dv(const GLdouble *v) { C_; gl_vertex4(c, (float)v[0], (float)v[1], 0, 1); }
void glVertex3dv(const GLdouble *v) { C_; gl_vertex4(c, (float)v[0], (float)v[1], (float)v[2], 1); }
void glVertex2iv(const GLint *v) { C_; gl_vertex4(c, (float)v[0], (float)v[1], 0, 1); }
void glVertex3iv(const GLint *v) { C_; gl_vertex4(c, (float)v[0], (float)v[1], (float)v[2], 1); }
void glColor3f(GLfloat r, GLfloat g, GLfloat b) { C_; gl_color4(c, r, g, b, 1); }
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { C_; gl_color4(c, r, g, b, a); }
void glColor3d(GLdouble r, GLdouble g, GLdouble b) { C_; gl_color4(c, (float)r, (float)g, (float)b, 1); }
void glColor4d(GLdouble r, GLdouble g, GLdouble b, GLdouble a) { C_; gl_color4(c, (float)r, (float)g, (float)b, (float)a); }
void glColor3ub(GLubyte r, GLubyte g, GLubyte b) { C_; gl_color4(c, r / 255.0f, g / 255.0f, b / 255.0f, 1); }
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { C_; gl_color4(c, r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f); }
void glColor3fv(const GLfloat *v) { C_; gl_color4(c, v[0], v[1], v[2], 1); }
void glColor4fv(const GLfloat *v) { C_; gl_color4(c, v[0], v[1], v[2], v[3]); }
void glColor3ubv(const GLubyte *v) { glColor3ub(v[0], v[1], v[2]); }
void glColor4ubv(const GLubyte *v) { glColor4ub(v[0], v[1], v[2], v[3]); }
void glColor3dv(const GLdouble *v) { glColor3d(v[0], v[1], v[2]); }
void glColor4dv(const GLdouble *v) { glColor4d(v[0], v[1], v[2], v[3]); }
void glNormal3f(GLfloat x, GLfloat y, GLfloat z) { C_; gl_normal3(c, x, y, z); }
void glNormal3d(GLdouble x, GLdouble y, GLdouble z) { C_; gl_normal3(c, (float)x, (float)y, (float)z); }
void glNormal3fv(const GLfloat *v) { C_; gl_normal3(c, v[0], v[1], v[2]); }
void glNormal3dv(const GLdouble *v) { C_; gl_normal3(c, (float)v[0], (float)v[1], (float)v[2]); }
void glNormal3b(GLbyte x, GLbyte y, GLbyte z) { C_; gl_normal3(c, (2 * x + 1) / 255.0f, (2 * y + 1) / 255.0f, (2 * z + 1) / 255.0f); }
void glTexCoord1f(GLfloat s) { C_; gl_texcoord4(c, 0, s, 0, 0, 1); }
void glTexCoord2f(GLfloat s, GLfloat t) { C_; gl_texcoord4(c, 0, s, t, 0, 1); }
void glTexCoord3f(GLfloat s, GLfloat t, GLfloat r) { C_; gl_texcoord4(c, 0, s, t, r, 1); }
void glTexCoord4f(GLfloat s, GLfloat t, GLfloat r, GLfloat q) { C_; gl_texcoord4(c, 0, s, t, r, q); }
void glTexCoord2d(GLdouble s, GLdouble t) { C_; gl_texcoord4(c, 0, (float)s, (float)t, 0, 1); }
void glTexCoord2i(GLint s, GLint t) { C_; gl_texcoord4(c, 0, (float)s, (float)t, 0, 1); }
void glTexCoord2fv(const GLfloat *v) { C_; gl_texcoord4(c, 0, v[0], v[1], 0, 1); }
void glTexCoord3fv(const GLfloat *v) { C_; gl_texcoord4(c, 0, v[0], v[1], v[2], 1); }
void glTexCoord4fv(const GLfloat *v) { C_; gl_texcoord4(c, 0, v[0], v[1], v[2], v[3]); }
void glTexCoord2dv(const GLdouble *v) { C_; gl_texcoord4(c, 0, (float)v[0], (float)v[1], 0, 1); }
void glMultiTexCoord1f(GLenum u, GLfloat s) { C_; gl_texcoord4(c, (int)(u - GL_TEXTURE0), s, 0, 0, 1); }
void glMultiTexCoord2f(GLenum u, GLfloat s, GLfloat t) { C_; gl_texcoord4(c, (int)(u - GL_TEXTURE0), s, t, 0, 1); }
void glMultiTexCoord3f(GLenum u, GLfloat s, GLfloat t, GLfloat r) { C_; gl_texcoord4(c, (int)(u - GL_TEXTURE0), s, t, r, 1); }
void glMultiTexCoord4f(GLenum u, GLfloat s, GLfloat t, GLfloat r, GLfloat q) { C_; gl_texcoord4(c, (int)(u - GL_TEXTURE0), s, t, r, q); }
void glMultiTexCoord2d(GLenum u, GLdouble s, GLdouble t) { C_; gl_texcoord4(c, (int)(u - GL_TEXTURE0), (float)s, (float)t, 0, 1); }
void glMultiTexCoord2fv(GLenum u, const GLfloat *v) { C_; gl_texcoord4(c, (int)(u - GL_TEXTURE0), v[0], v[1], 0, 1); }
void glMultiTexCoord3fv(GLenum u, const GLfloat *v) { C_; gl_texcoord4(c, (int)(u - GL_TEXTURE0), v[0], v[1], v[2], 1); }
void glMultiTexCoord4fv(GLenum u, const GLfloat *v) { C_; gl_texcoord4(c, (int)(u - GL_TEXTURE0), v[0], v[1], v[2], v[3]); }
void glEdgeFlag(GLboolean flag) {
    C_;
    if (DL_ACTIVE(c)) { gl_dlarg a = { .i = flag }; if (gl_dl_record(c, OP_EDGE_FLAG, 1, &a)) return; }
    c->cur.edge = flag ? GL_TRUE : GL_FALSE;
}

void glRectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2) {
    glBegin(GL_POLYGON);
    glVertex2f(x1, y1); glVertex2f(x2, y1); glVertex2f(x2, y2); glVertex2f(x1, y2);
    glEnd();
}
void glRecti(GLint x1, GLint y1, GLint x2, GLint y2) { glRectf((float)x1, (float)y1, (float)x2, (float)y2); }
void glRectd(GLdouble x1, GLdouble y1, GLdouble x2, GLdouble y2) { glRectf((float)x1, (float)y1, (float)x2, (float)y2); }

/* ---------------- vertex arrays ---------------- */
static int type_size(GLenum t) {
    switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT: return 4;
    case GL_DOUBLE: return 8;
    }
    return 0;
}

static void set_array(gl_context *c, gl_array *a, GLint size, GLenum type, GLsizei stride, const void *p, int minsz, int maxsz) {
    if (size < minsz || size > maxsz || stride < 0) { gl_error(c, GL_INVALID_VALUE); return; }
    if (!type_size(type)) { gl_error(c, GL_INVALID_ENUM); return; }
    a->size = size; a->type = type; a->stride = stride; a->ptr = p;
}
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { C_; set_array(c, &c->cl.vertex, size, type, stride, p, 2, 4); }
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid *p) { C_; set_array(c, &c->cl.normal, 3, type, stride, p, 3, 3); }
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { C_; set_array(c, &c->cl.color, size, type, stride, p, 3, 4); }
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid *p) { C_; set_array(c, &c->cl.tex[c->cl.active], size, type, stride, p, 1, 4); }
void glEdgeFlagPointer(GLsizei stride, const GLvoid *p) { C_; c->cl.edge.stride = stride; c->cl.edge.ptr = p; c->cl.edge.type = GL_UNSIGNED_BYTE; c->cl.edge.size = 1; }

static gl_array *client_array(gl_context *c, GLenum a) {
    switch (a) {
    case GL_VERTEX_ARRAY: return &c->cl.vertex;
    case GL_NORMAL_ARRAY: return &c->cl.normal;
    case GL_COLOR_ARRAY: return &c->cl.color;
    case GL_TEXTURE_COORD_ARRAY: return &c->cl.tex[c->cl.active];
    case GL_EDGE_FLAG_ARRAY: return &c->cl.edge;
    }
    return NULL;
}
void glEnableClientState(GLenum a) { C_; gl_array *p = client_array(c, a); if (p) p->enabled = 1; else gl_error(c, GL_INVALID_ENUM); }
void glDisableClientState(GLenum a) { C_; gl_array *p = client_array(c, a); if (p) p->enabled = 0; else gl_error(c, GL_INVALID_ENUM); }
void glClientActiveTexture(GLenum u) {
    C_;
    if (u - GL_TEXTURE0 >= MAX_UNITS) { gl_error(c, GL_INVALID_ENUM); return; }
    c->cl.active = (int)(u - GL_TEXTURE0);
}

void glInterleavedArrays(GLenum format, GLsizei stride, const GLvoid *pointer) {
    C_;
    /* tex size, color size (4ub = -4), normal, vertex size */
    int tc = 0, cc = 0, nn = 0, vc = 3;
    switch (format) {
    case GL_V2F: vc = 2; break;
    case GL_V3F: break;
    case GL_C4UB_V2F: cc = -4; vc = 2; break;
    case GL_C4UB_V3F: cc = -4; break;
    case GL_C3F_V3F: cc = 3; break;
    case GL_N3F_V3F: nn = 1; break;
    case GL_C4F_N3F_V3F: cc = 4; nn = 1; break;
    case GL_T2F_V3F: tc = 2; break;
    case GL_T4F_V4F: tc = 4; vc = 4; break;
    case GL_T2F_C4UB_V3F: tc = 2; cc = -4; break;
    case GL_T2F_C3F_V3F: tc = 2; cc = 3; break;
    case GL_T2F_N3F_V3F: tc = 2; nn = 1; break;
    case GL_T2F_C4F_N3F_V3F: tc = 2; cc = 4; nn = 1; break;
    case GL_T4F_C4F_N3F_V4F: tc = 4; cc = 4; nn = 1; vc = 4; break;
    default: gl_error(c, GL_INVALID_ENUM); return;
    }
    int csize = cc < 0 ? 4 : cc * 4;
    int size = tc * 4 + csize + nn * 12 + vc * 4;
    if (!stride) stride = size;
    const char *p = pointer;
    gl_array *t = &c->cl.tex[c->cl.active];
    t->enabled = tc != 0;
    if (tc) { t->size = tc; t->type = GL_FLOAT; t->stride = stride; t->ptr = p; p += tc * 4; }
    c->cl.color.enabled = cc != 0;
    if (cc) { c->cl.color.size = cc < 0 ? 4 : cc; c->cl.color.type = cc < 0 ? GL_UNSIGNED_BYTE : GL_FLOAT; c->cl.color.stride = stride; c->cl.color.ptr = p; p += csize; }
    c->cl.normal.enabled = nn != 0;
    if (nn) { c->cl.normal.size = 3; c->cl.normal.type = GL_FLOAT; c->cl.normal.stride = stride; c->cl.normal.ptr = p; p += 12; }
    c->cl.vertex.enabled = 1;
    c->cl.vertex.size = vc; c->cl.vertex.type = GL_FLOAT; c->cl.vertex.stride = stride; c->cl.vertex.ptr = p;
    c->cl.edge.enabled = 0;
    for (int u = 0; u < MAX_UNITS; u++) if (u != c->cl.active) c->cl.tex[u].enabled = 0;
}

/* element i of an array as floats (colors of integer types normalized) */
static void fetch(const gl_array *a, int i, float *out, int normalize) {
    int ts = type_size(a->type);
    const char *p = (const char *)a->ptr + (size_t)i * (size_t)(a->stride ? a->stride : ts * a->size);
    for (int k = 0; k < a->size; k++) {
        float v;
        switch (a->type) {
        case GL_FLOAT: { float f; memcpy(&f, p + 4 * k, 4); v = f; break; }
        case GL_DOUBLE: { double d; memcpy(&d, p + 8 * k, 8); v = (float)d; break; }
        case GL_UNSIGNED_BYTE: v = ((const unsigned char *)p)[k]; if (normalize) v *= 1 / 255.0f; break;
        case GL_BYTE: v = ((const signed char *)p)[k]; if (normalize) v = (2 * v + 1) / 255.0f; break;
        case GL_UNSIGNED_SHORT: { unsigned short s; memcpy(&s, p + 2 * k, 2); v = s; if (normalize) v *= 1 / 65535.0f; break; }
        case GL_SHORT: { short s; memcpy(&s, p + 2 * k, 2); v = s; if (normalize) v = (2 * v + 1) / 65535.0f; break; }
        case GL_UNSIGNED_INT: { unsigned u; memcpy(&u, p + 4 * k, 4); v = (float)u; if (normalize) v = (float)(u / 4294967295.0); break; }
        default: { int s; memcpy(&s, p + 4 * k, 4); v = (float)s; if (normalize) v = (float)((2.0 * s + 1) / 4294967295.0); break; }
        }
        out[k] = v;
    }
}

/* raw vertex i from the enabled arrays and the current values */
static void array_vertex(gl_context *c, int i, gl_rawvert *v) {
    float t[4] = { 0, 0, 0, 1 };
    fetch(&c->cl.vertex, i, t, 0);
    memcpy(v->obj, t, sizeof v->obj);
    if (c->cl.color.enabled) {
        float col[4] = { 0, 0, 0, 1 };
        fetch(&c->cl.color, i, col, 1);
        memcpy(v->col, col, sizeof v->col);
        memcpy(c->cur.color, col, sizeof col);            /* (the current color follows, as GL says) */
    } else memcpy(v->col, c->cur.color, sizeof v->col);
    if (c->cl.normal.enabled) {
        fetch(&c->cl.normal, i, v->nrm, 1);
        memcpy(c->cur.normal, v->nrm, sizeof v->nrm);
    } else memcpy(v->nrm, c->cur.normal, sizeof v->nrm);
    for (int u = 0; u < MAX_UNITS; u++) {
        if (c->cl.tex[u].enabled) {
            float tc[4] = { 0, 0, 0, 1 };
            fetch(&c->cl.tex[u], i, tc, 0);
            memcpy(v->tex[u], tc, sizeof tc);
            memcpy(c->cur.tex[u], tc, sizeof tc);
        } else memcpy(v->tex[u], c->cur.tex[u], sizeof v->tex[u]);
    }
    if (c->cl.edge.enabled && c->cl.edge.ptr) {
        v->edge = ((const unsigned char *)c->cl.edge.ptr)[(size_t)i * (size_t)(c->cl.edge.stride ? c->cl.edge.stride : 1)] != 0;
    } else v->edge = c->cur.edge;
}

/* while a list compiles (or between glBegin and glEnd), arrays go through
 * the immediate-mode calls, so they are recorded */
static void array_element_immediate(gl_context *c, int i) {
    gl_rawvert v;
    array_vertex(c, i, &v);
    if (c->cl.color.enabled) gl_color4(c, v.col[0], v.col[1], v.col[2], v.col[3]);
    if (c->cl.normal.enabled) gl_normal3(c, v.nrm[0], v.nrm[1], v.nrm[2]);
    for (int u = 0; u < MAX_UNITS; u++)
        if (c->cl.tex[u].enabled) gl_texcoord4(c, u, v.tex[u][0], v.tex[u][1], v.tex[u][2], v.tex[u][3]);
    if (c->cl.vertex.enabled) gl_vertex4(c, v.obj[0], v.obj[1], v.obj[2], v.obj[3]);
}

void glArrayElement(GLint i) { C_; array_element_immediate(c, i); }

static unsigned index_at(GLenum type, const void *idx, int k) {
    switch (type) {
    case GL_UNSIGNED_BYTE: return ((const unsigned char *)idx)[k];
    case GL_UNSIGNED_SHORT: { unsigned short s; memcpy(&s, (const char *)idx + 2 * k, 2); return s; }
    default: { unsigned u; memcpy(&u, (const char *)idx + 4 * k, 4); return u; }
    }
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    C_;
    if (!is_prim(mode)) { gl_error(c, GL_INVALID_ENUM); return; }
    if (count < 0 || first < 0) { gl_error(c, GL_INVALID_VALUE); return; }
    NOT_IN_BEGIN(c);
    if (!c->cl.vertex.enabled || !count) return;
    if (DL_ACTIVE(c)) {
        glBegin(mode);
        for (int i = 0; i < count; i++) array_element_immediate(c, first + i);
        glEnd();
        return;
    }
    if (!gl_reserve_vtx(c, count)) return;
    gl_rawvert *raw = malloc(sizeof *raw * (size_t)count);
    if (!raw) { gl_error(c, GL_OUT_OF_MEMORY); return; }
    for (int i = 0; i < count; i++) array_vertex(c, first + i, &raw[i]);
    gl_process(c, raw, count, c->vtx);
    free(raw);
    gl_draw(c, mode, c->vtx, NULL, count);
}

static void draw_elements(gl_context *c, GLenum mode, GLsizei count, GLenum type, const void *indices) {
    if (!is_prim(mode) || (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT)) {
        gl_error(c, GL_INVALID_ENUM);
        return;
    }
    if (count < 0) { gl_error(c, GL_INVALID_VALUE); return; }
    NOT_IN_BEGIN(c);
    if (!c->cl.vertex.enabled || !count || !indices) return;
    if (DL_ACTIVE(c)) {
        glBegin(mode);
        for (int i = 0; i < count; i++) array_element_immediate(c, (int)index_at(type, indices, i));
        glEnd();
        return;
    }
    /* each vertex in the range used is processed once */
    unsigned lo = ~0u, hi = 0;
    for (int i = 0; i < count; i++) {
        unsigned k = index_at(type, indices, i);
        if (k < lo) lo = k;
        if (k > hi) hi = k;
    }
    unsigned range = hi - lo + 1;
    unsigned *idx = malloc(sizeof *idx * (size_t)count);
    if (!idx) { gl_error(c, GL_OUT_OF_MEMORY); return; }
    gl_rawvert *raw;
    int nv;
    if (range <= (unsigned)count * 4 + 64) {         /* dense: the whole range */
        nv = (int)range;
        raw = malloc(sizeof *raw * (size_t)nv);
        if (raw) for (int i = 0; i < nv; i++) array_vertex(c, (int)(lo + (unsigned)i), &raw[i]);
        for (int i = 0; i < count; i++) idx[i] = index_at(type, indices, i) - lo;
    } else {                                          /* sparse: each index on its own */
        nv = count;
        raw = malloc(sizeof *raw * (size_t)nv);
        if (raw) for (int i = 0; i < nv; i++) { array_vertex(c, (int)index_at(type, indices, i), &raw[i]); idx[i] = (unsigned)i; }
    }
    if (!raw || !gl_reserve_vtx(c, nv)) {
        free(raw);
        free(idx);
        if (!raw) gl_error(c, GL_OUT_OF_MEMORY);
        return;
    }
    gl_process(c, raw, nv, c->vtx);
    free(raw);
    gl_draw(c, mode, c->vtx, idx, count);
    free(idx);
}
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) { C_; draw_elements(c, mode, count, type, indices); }
void glDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const GLvoid *indices) {
    C_;
    if (end < start) { gl_error(c, GL_INVALID_VALUE); return; }
    draw_elements(c, mode, count, type, indices);
}

/* ---------------- processing ---------------- */
static inline float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void norm3(float *v) {
    float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (l > 0) { l = 1 / sqrtf(l); v[0] *= l; v[1] *= l; v[2] *= l; }
}
static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* x^e for the specular term (e 0..128) */
static inline float spec_pow(float x, float e) {
    if (x <= 0) return e == 0 ? 1 : 0;
    if (e == 0) return 1;
    if (e == 1) return x;
    return powf(x, e);
}

/* the lit color of one face (material m, normal n in eye space, unit length) */
static void light_face(gl_context *c, const gl_material *m, const float *n, const float *eye, float *col, float *spec) {
    const gl_lighting *lt = &c->lt;
    float r = m->emission[0] + m->ambient[0] * lt->model_ambient[0];
    float g = m->emission[1] + m->ambient[1] * lt->model_ambient[1];
    float b = m->emission[2] + m->ambient[2] * lt->model_ambient[2];
    float sr = 0, sg = 0, sb = 0;
    float view[3] = { 0, 0, 1 };
    if (lt->local_viewer) { view[0] = -eye[0]; view[1] = -eye[1]; view[2] = -eye[2]; norm3(view); }
    for (int i = 0; i < MAX_LIGHTS; i++) {
        if (!c->en.light[i]) continue;
        const gl_light *L = &lt->light[i];
        float dir[3], att = 1;
        if (L->pos[3] == 0) {
            dir[0] = L->pos[0]; dir[1] = L->pos[1]; dir[2] = L->pos[2];
            norm3(dir);
        } else {
            float pw = 1 / L->pos[3];
            dir[0] = L->pos[0] * pw - eye[0]; dir[1] = L->pos[1] * pw - eye[1]; dir[2] = L->pos[2] * pw - eye[2];
            float d2 = dot3(dir, dir), d = sqrtf(d2);
            if (d > 0) { float id = 1 / d; dir[0] *= id; dir[1] *= id; dir[2] *= id; }
            if (L->att[1] != 0 || L->att[2] != 0 || L->att[0] != 1) {
                float den = L->att[0] + L->att[1] * d + L->att[2] * d2;
                att = den > 0 ? 1 / den : 1;
            }
            if (L->spot_cutoff != 180) {
                float cosang = -dot3(dir, L->spot_dir);
                if (cosang < L->spot_cos) continue;    /* outside the cone: nothing from this light */
                att *= spec_pow(cosang, L->spot_exp);
            }
        }
        r += att * L->ambient[0] * m->ambient[0];
        g += att * L->ambient[1] * m->ambient[1];
        b += att * L->ambient[2] * m->ambient[2];
        float nl = dot3(n, dir);
        if (nl <= 0) continue;
        float d = att * nl;
        r += d * L->diffuse[0] * m->diffuse[0];
        g += d * L->diffuse[1] * m->diffuse[1];
        b += d * L->diffuse[2] * m->diffuse[2];
        float h[3] = { dir[0] + view[0], dir[1] + view[1], dir[2] + view[2] };
        norm3(h);
        float nh = dot3(n, h);
        if (nh > 0 && (L->specular[0] != 0 || L->specular[1] != 0 || L->specular[2] != 0)) {
            float s = att * spec_pow(nh, m->shininess);
            sr += s * L->specular[0] * m->specular[0];
            sg += s * L->specular[1] * m->specular[1];
            sb += s * L->specular[2] * m->specular[2];
        }
    }
    if (lt->color_control == GL_SEPARATE_SPECULAR_COLOR) {
        spec[0] = clamp01(sr); spec[1] = clamp01(sg); spec[2] = clamp01(sb); spec[3] = 0;
    } else {
        r += sr; g += sg; b += sb;
        spec[0] = spec[1] = spec[2] = spec[3] = 0;
    }
    col[0] = clamp01(r); col[1] = clamp01(g); col[2] = clamp01(b); col[3] = clamp01(m->diffuse[3]);
}

/* glColorMaterial: the vertex color in place of a material color */
static void apply_color_material(gl_context *c, gl_material *m, const float *col) {
    switch (c->lt.cm_mode) {
    case GL_EMISSION: memcpy(m->emission, col, 16); break;
    case GL_AMBIENT: memcpy(m->ambient, col, 16); break;
    case GL_DIFFUSE: memcpy(m->diffuse, col, 16); break;
    case GL_SPECULAR: memcpy(m->specular, col, 16); break;
    default: memcpy(m->ambient, col, 16); memcpy(m->diffuse, col, 16); break;
    }
}

static float texgen_coord(gl_context *c, int u, int k, const gl_rawvert *in, const float *eye, const float *sphere) {
    const gl_texunit *tu = &c->tx.unit[u];
    switch (tu->gen_mode[k]) {
    case GL_OBJECT_LINEAR: { const float *p = tu->obj_plane[k]; return p[0] * in->obj[0] + p[1] * in->obj[1] + p[2] * in->obj[2] + p[3] * in->obj[3]; }
    case GL_EYE_LINEAR: { const float *p = tu->eye_plane[k]; return p[0] * eye[0] + p[1] * eye[1] + p[2] * eye[2] + p[3] * eye[3]; }
    case GL_SPHERE_MAP: return k < 2 ? sphere[k] : 0;
    }
    return 0;
}

void gl_process(gl_context *c, const gl_rawvert *in, int n, gl_vertex *out) {
    gl_update_matrices(c);
    const glmat *mv = &c->mv[c->mv_top], *mvp = &c->mvp;
    int lighting = c->en.lighting, two = lighting && c->lt.two_side;
    int cm = lighting && c->en.color_material;
    int cm_front = cm && c->lt.cm_face != GL_BACK, cm_back = cm && c->lt.cm_face != GL_FRONT;
    int fog = c->en.fog;
    int nclip = 0;
    for (int i = 0; i < MAX_CLIP_PLANES; i++) if (c->en.clip[i]) nclip = i + 1;
    int texgen[MAX_UNITS], texmat[MAX_UNITS], sphere_needed = 0;
    for (int u = 0; u < MAX_UNITS; u++) {
        texgen[u] = c->en.texgen[u][0] || c->en.texgen[u][1] || c->en.texgen[u][2] || c->en.texgen[u][3];
        texmat[u] = !c->texm[u][c->tex_top[u]].identity;
        for (int k = 0; k < 2; k++)
            if (c->en.texgen[u][k] && c->tx.unit[u].gen_mode[k] == GL_SPHERE_MAP) sphere_needed = 1;
    }
    gl_material mat[2] = { c->lt.mat[0], c->lt.mat[1] };
    float fog_scale = c->fog.end != c->fog.start ? 1 / (c->fog.end - c->fog.start) : 0;

    for (int i = 0; i < n; i++) {
        const gl_rawvert *r = &in[i];
        gl_vertex *v = &out[i];
        mat_xform4(v->eye, mv, r->obj);
        mat_xform4(v->clip, mvp, r->obj);
        v->flags = 0;
        v->edge = r->edge;
        /* clip codes: the frustum, then the user planes */
        float x = v->clip[0], y = v->clip[1], z = v->clip[2], w = v->clip[3];
        unsigned cmk = 0;
        if (x < -w) cmk |= 1;
        if (x > w) cmk |= 2;
        if (y < -w) cmk |= 4;
        if (y > w) cmk |= 8;
        if (z < -w) cmk |= 16;
        if (z > w) cmk |= 32;
        for (int p = 0; p < nclip; p++) {
            if (!c->en.clip[p]) continue;
            const float *pl = c->xf.clip_plane[p];
            if (pl[0] * v->eye[0] + pl[1] * v->eye[1] + pl[2] * v->eye[2] + pl[3] * v->eye[3] < 0) cmk |= 64u << p;
        }
        v->clipmask = cmk;

        float nrm[3] = { 0, 0, 1 };
        if (lighting || sphere_needed) {
            const float *nm = c->nmat;
            nrm[0] = nm[0] * r->nrm[0] + nm[1] * r->nrm[1] + nm[2] * r->nrm[2];
            nrm[1] = nm[3] * r->nrm[0] + nm[4] * r->nrm[1] + nm[5] * r->nrm[2];
            nrm[2] = nm[6] * r->nrm[0] + nm[7] * r->nrm[1] + nm[8] * r->nrm[2];
            if (c->en.normalize || c->en.rescale_normal) norm3(nrm);
        }
        if (lighting) {
            if (cm_front) apply_color_material(c, &mat[0], r->col);
            if (cm_back) apply_color_material(c, &mat[1], r->col);
            light_face(c, &mat[0], nrm, v->eye, v->col[0], v->spec[0]);
            if (two) {
                float bn[3] = { -nrm[0], -nrm[1], -nrm[2] };
                light_face(c, &mat[1], bn, v->eye, v->col[1], v->spec[1]);
            } else {
                memcpy(v->col[1], v->col[0], sizeof v->col[1]);
                memcpy(v->spec[1], v->spec[0], sizeof v->spec[1]);
            }
        } else {
            for (int k = 0; k < 4; k++) v->col[0][k] = v->col[1][k] = clamp01(r->col[k]);
            memset(v->spec, 0, sizeof v->spec);
        }

        float sphere[2] = { 0, 0 };
        if (sphere_needed) {
            float u[3] = { v->eye[0], v->eye[1], v->eye[2] };
            norm3(u);
            float d = 2 * dot3(nrm, u);
            float rf[3] = { u[0] - nrm[0] * d, u[1] - nrm[1] * d, u[2] - nrm[2] * d + 1 };
            float m = 2 * sqrtf(rf[0] * rf[0] + rf[1] * rf[1] + rf[2] * rf[2]);
            if (m > 0) { sphere[0] = rf[0] / m + 0.5f; sphere[1] = rf[1] / m + 0.5f; }
        }
        for (int u = 0; u < MAX_UNITS; u++) {
            float t[4] = { r->tex[u][0], r->tex[u][1], r->tex[u][2], r->tex[u][3] };
            if (texgen[u])
                for (int k = 0; k < 4; k++)
                    if (c->en.texgen[u][k]) t[k] = texgen_coord(c, u, k, r, v->eye, sphere);
            if (texmat[u]) mat_xform4(v->tex[u], &c->texm[u][c->tex_top[u]], t);
            else memcpy(v->tex[u], t, sizeof t);
        }

        if (fog) {
            float d = fabsf(v->eye[2]), f;
            switch (c->fog.mode) {
            case GL_EXP: f = expf(-c->fog.density * d); break;
            case GL_EXP2: { float e = c->fog.density * d; f = expf(-e * e); break; }
            default: f = (c->fog.end - d) * fog_scale; break;
            }
            v->fog = clamp01(f);
        } else v->fog = 1;
    }
    /* the color material follows the last color (GL changes the material) */
    if (cm && n) {
        if (cm_front) c->lt.mat[0] = mat[0];
        if (cm_back) c->lt.mat[1] = mat[1];
    }
}
