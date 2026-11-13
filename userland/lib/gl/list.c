/* Halcyon's 3D library: display lists. While a list is compiled, the calls
 * that can be recorded (vertices and their attributes, matrices, enables,
 * the state setters, texture binding and parameters, clears, nested
 * glCallList) are appended to it as an opcode with its arguments; playing
 * it back calls the same functions again. Vertex arrays are recorded as
 * the vertices they give (as GL says: the data at compile time). Not
 * recorded (they run at once, as when no list is compiled): texture images
 * (glTexImage2D, glTexSubImage2D; texture objects keep images instead),
 * pixel storage, glReadPixels, glGet*, the client state calls, glFinish and
 * glFlush. */
#include "glint.h"

/* a command: one header (op | n << 8) and n arguments */

static gl_list *find(gl_context *c, GLuint name) {
    for (int i = 0; i < c->nlists; i++) if (c->lists[i] && c->lists[i]->name == name) return c->lists[i];
    return NULL;
}

static void list_free(gl_list *l) {
    if (!l) return;
    free(l->ops);
    free(l);
}

void gl_lists_free(gl_context *c) {
    for (int i = 0; i < c->nlists; i++) list_free(c->lists[i]);
    free(c->lists);
    c->lists = NULL;
    c->nlists = 0;
    list_free(c->compiling);
    c->compiling = NULL;
}

int gl_dl_record(gl_context *c, int op, int n, const gl_dlarg *args) {
    gl_list *l = c->compiling;
    if (!l) return 0;
    if (l->n + n + 1 > l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        while (cap < l->n + n + 1) cap *= 2;
        gl_dlarg *p = realloc(l->ops, sizeof *p * (size_t)cap);
        if (!p) { gl_error(c, GL_OUT_OF_MEMORY); return c->list_mode == GL_COMPILE; }
        l->ops = p;
        l->cap = cap;
    }
    l->ops[l->n++].u = (unsigned)op | (unsigned)n << 8;
    for (int i = 0; i < n; i++) l->ops[l->n++] = args[i];
    return c->list_mode == GL_COMPILE;
}

GLuint glGenLists(GLsizei range) {
    GET_CTX_R(c, 0);
    if (c->in_begin) { gl_error(c, GL_INVALID_OPERATION); return 0; }
    if (range < 0) { gl_error(c, GL_INVALID_VALUE); return 0; }
    if (range == 0) return 0;
    /* the first run of range unused names */
    GLuint start = 1;
    for (;;) {
        GLuint k;
        for (k = 0; k < (GLuint)range; k++) if (find(c, start + k)) break;
        if (k == (GLuint)range) break;
        start += k + 1;
    }
    /* reserve them: empty lists */
    for (GLuint k = 0; k < (GLuint)range; k++) {
        gl_list *l = calloc(1, sizeof *l);
        gl_list **p = realloc(c->lists, sizeof *p * (size_t)(c->nlists + 1));
        if (!l || !p) { free(l); if (p) c->lists = p; gl_error(c, GL_OUT_OF_MEMORY); return 0; }
        c->lists = p;
        l->name = start + k;
        c->lists[c->nlists++] = l;
    }
    return start;
}

void glDeleteLists(GLuint list, GLsizei range) {
    GET_CTX(c);
    NOT_IN_BEGIN(c);
    if (range < 0) { gl_error(c, GL_INVALID_VALUE); return; }
    for (int i = 0; i < c->nlists; i++) {
        gl_list *l = c->lists[i];
        if (l && l->name >= list && l->name - list < (GLuint)range) {
            list_free(l);
            c->lists[i] = c->lists[--c->nlists];
            i--;
        }
    }
}

GLboolean glIsList(GLuint list) {
    GET_CTX_R(c, GL_FALSE);
    return find(c, list) ? GL_TRUE : GL_FALSE;      /* (glGenLists makes empty lists) */
}

void glNewList(GLuint list, GLenum mode) {
    GET_CTX(c);
    NOT_IN_BEGIN(c);
    if (list == 0) { gl_error(c, GL_INVALID_VALUE); return; }
    if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) { gl_error(c, GL_INVALID_ENUM); return; }
    if (c->compiling) { gl_error(c, GL_INVALID_OPERATION); return; }
    gl_list *l = calloc(1, sizeof *l);
    if (!l) { gl_error(c, GL_OUT_OF_MEMORY); return; }
    l->name = list;
    c->compiling = l;
    c->list_mode = mode;
}

void glEndList(void) {
    GET_CTX(c);
    if (!c->compiling) { gl_error(c, GL_INVALID_OPERATION); return; }
    if (c->in_begin && c->list_mode == GL_COMPILE_AND_EXECUTE) { gl_error(c, GL_INVALID_OPERATION); return; }
    gl_list *l = c->compiling;
    c->compiling = NULL;
    c->list_mode = 0;
    /* replaces a list of that name */
    for (int i = 0; i < c->nlists; i++)
        if (c->lists[i] && c->lists[i]->name == l->name) { list_free(c->lists[i]); c->lists[i] = l; return; }
    gl_list **p = realloc(c->lists, sizeof *p * (size_t)(c->nlists + 1));
    if (!p) { list_free(l); gl_error(c, GL_OUT_OF_MEMORY); return; }
    c->lists = p;
    c->lists[c->nlists++] = l;
}

#define U(k) (a[k].u)
#define I(k) (a[k].i)
#define F(k) (a[k].f)

static void play(gl_context *c, const gl_list *l) {
    for (int k = 0; k < l->n;) {
        unsigned h = l->ops[k].u;
        int op = (int)(h & 0xFF), n = (int)(h >> 8);
        const gl_dlarg *a = &l->ops[k + 1];
        k += n + 1;
        switch (op) {
        case OP_BEGIN: glBegin(U(0)); break;
        case OP_END: glEnd(); break;
        case OP_VERTEX: gl_vertex4(c, F(0), F(1), F(2), F(3)); break;
        case OP_COLOR: gl_color4(c, F(0), F(1), F(2), F(3)); break;
        case OP_NORMAL: gl_normal3(c, F(0), F(1), F(2)); break;
        case OP_TEXCOORD: gl_texcoord4(c, I(0), F(1), F(2), F(3), F(4)); break;
        case OP_EDGE_FLAG: glEdgeFlag((GLboolean)I(0)); break;
        case OP_MATERIAL: { float v[4] = { F(2), F(3), F(4), F(5) }; glMaterialfv(U(0), U(1), v); break; }
        case OP_LIGHT: { float v[4] = { F(2), F(3), F(4), F(5) }; glLightfv(U(0), U(1), v); break; }
        case OP_LIGHT_MODEL: { float v[4] = { F(1), F(2), F(3), F(4) }; glLightModelfv(U(0), v); break; }
        case OP_COLOR_MATERIAL: glColorMaterial(U(0), U(1)); break;
        case OP_MATRIX_MODE: glMatrixMode(U(0)); break;
        case OP_LOAD_IDENTITY: glLoadIdentity(); break;
        case OP_LOAD_MATRIX: case OP_MULT_MATRIX: {
            float m[16];
            for (int i = 0; i < 16; i++) m[i] = F(i);
            if (op == OP_LOAD_MATRIX) glLoadMatrixf(m); else glMultMatrixf(m);
            break;
        }
        case OP_PUSH_MATRIX: glPushMatrix(); break;
        case OP_POP_MATRIX: glPopMatrix(); break;
        case OP_ROTATE: glRotatef(F(0), F(1), F(2), F(3)); break;
        case OP_TRANSLATE: glTranslatef(F(0), F(1), F(2)); break;
        case OP_SCALE: glScalef(F(0), F(1), F(2)); break;
        case OP_FRUSTUM: glFrustum(F(0), F(1), F(2), F(3), F(4), F(5)); break;
        case OP_ORTHO: glOrtho(F(0), F(1), F(2), F(3), F(4), F(5)); break;
        case OP_ENABLE: glEnable(U(0)); break;
        case OP_DISABLE: glDisable(U(0)); break;
        case OP_BIND_TEXTURE: glBindTexture(U(0), U(1)); break;
        case OP_TEX_ENV: { float v[4] = { F(2), F(3), F(4), F(5) }; glTexEnvfv(U(0), U(1), v); break; }
        case OP_TEX_PARAMETER: { float v[4] = { F(2), F(3), F(4), F(5) }; glTexParameterfv(U(0), U(1), v); break; }
        case OP_TEX_GEN: {
            float v[4] = { F(2), F(3), F(4), F(5) };
            if (U(1) == GL_TEXTURE_GEN_MODE) glTexGenf(U(0), U(1), v[0]); else glTexGenfv(U(0), U(1), v);
            break;
        }
        case OP_SHADE_MODEL: glShadeModel(U(0)); break;
        case OP_BLEND_FUNC: glBlendFunc(U(0), U(1)); break;
        case OP_ALPHA_FUNC: glAlphaFunc(U(0), F(1)); break;
        case OP_DEPTH_FUNC: glDepthFunc(U(0)); break;
        case OP_DEPTH_MASK: glDepthMask((GLboolean)U(0)); break;
        case OP_COLOR_MASK: glColorMask((GLboolean)U(0), (GLboolean)U(1), (GLboolean)U(2), (GLboolean)U(3)); break;
        case OP_CULL_FACE: glCullFace(U(0)); break;
        case OP_FRONT_FACE: glFrontFace(U(0)); break;
        case OP_FOG: { float v[4] = { F(1), F(2), F(3), F(4) }; glFogfv(U(0), v); break; }
        case OP_CALL_LIST: glCallList(U(0)); break;
        case OP_ACTIVE_TEXTURE: glActiveTexture(U(0)); break;
        case OP_POLYGON_MODE: glPolygonMode(U(0), U(1)); break;
        case OP_POLYGON_OFFSET: glPolygonOffset(F(0), F(1)); break;
        case OP_LINE_WIDTH: glLineWidth(F(0)); break;
        case OP_POINT_SIZE: glPointSize(F(0)); break;
        case OP_CLEAR: glClear(U(0)); break;
        case OP_CLEAR_COLOR: glClearColor(F(0), F(1), F(2), F(3)); break;
        case OP_CLEAR_DEPTH: glClearDepth(F(0)); break;
        case OP_VIEWPORT: glViewport(I(0), I(1), I(2), I(3)); break;
        case OP_SCISSOR: glScissor(I(0), I(1), I(2), I(3)); break;
        case OP_HINT: glHint(U(0), U(1)); break;
        case OP_PUSH_ATTRIB: glPushAttrib(U(0)); break;
        case OP_POP_ATTRIB: glPopAttrib(); break;
        case OP_LIST_BASE: glListBase(U(0)); break;
        case OP_DEPTH_RANGE: glDepthRange(F(0), F(1)); break;
        case OP_CLIP_PLANE: { double e[4] = { F(1), F(2), F(3), F(4) }; glClipPlane(U(0), e); break; }
        }
    }
}

void glCallList(GLuint list) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { gl_dlarg a = { .u = list }; if (gl_dl_record(c, OP_CALL_LIST, 1, &a)) return; }
    if (c->list_depth >= MAX_LIST_NESTING) return;
    gl_list *l = find(c, list);
    if (!l) return;
    c->list_depth++;
    play(c, l);
    c->list_depth--;
}

void glCallLists(GLsizei n, GLenum type, const GLvoid *lists) {
    GET_CTX(c);
    if (n < 0) { gl_error(c, GL_INVALID_VALUE); return; }
    const unsigned char *p = lists;
    for (int i = 0; i < n; i++) {
        GLuint id;
        switch (type) {
        case GL_BYTE: id = (GLuint)(int)((const signed char *)p)[i]; break;
        case GL_UNSIGNED_BYTE: id = p[i]; break;
        case GL_SHORT: { short s; memcpy(&s, p + 2 * i, 2); id = (GLuint)(int)s; break; }
        case GL_UNSIGNED_SHORT: { unsigned short s; memcpy(&s, p + 2 * i, 2); id = s; break; }
        case GL_INT: case GL_UNSIGNED_INT: { unsigned u; memcpy(&u, p + 4 * i, 4); id = u; break; }
        case GL_FLOAT: { float f; memcpy(&f, p + 4 * i, 4); id = (GLuint)(int)f; break; }
        case GL_2_BYTES: id = (GLuint)p[2 * i] << 8 | p[2 * i + 1]; break;
        case GL_3_BYTES: id = (GLuint)p[3 * i] << 16 | (GLuint)p[3 * i + 1] << 8 | p[3 * i + 2]; break;
        case GL_4_BYTES: id = (GLuint)p[4 * i] << 24 | (GLuint)p[4 * i + 1] << 16 | (GLuint)p[4 * i + 2] << 8 | p[4 * i + 3]; break;
        default: gl_error(c, GL_INVALID_ENUM); return;
        }
        glCallList(c->list_base + id);
    }
}

void glListBase(GLuint base) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { gl_dlarg a = { .u = base }; if (gl_dl_record(c, OP_LIST_BASE, 1, &a)) return; }
    NOT_IN_BEGIN(c);
    c->list_base = base;
}
