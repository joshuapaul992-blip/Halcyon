/* Host tests of Halcyon's 3D library (userland/lib/gl), built with ASan and
 * UBSan from the same sources as Halcyon's libgl.a (GL_HOST) and the same
 * float math (userland/lib/math.c).
 *
 * 1. Scenes (scenes.h): each draws a small picture into a memory context;
 *    it is compared with tests/gl/ref/NAME.qoi. Tolerance: a pixel differs
 *    when one of its channels is off by more than 24; a scene passes with
 *    at most 1 % of its pixels differing (edge pixels may flip with the last
 *    bit of a float) and a mean channel difference under 1.0.
 * 2. Exact checks: the same picture from immediate mode, glDrawElements,
 *    glDrawArrays, glArrayElement, glInterleavedArrays with
 *    glDrawRangeElements and a display list; a queue that overflows (and
 *    its state snapshots) against one drawn in small pieces; glReadPixels.
 * 3. API checks: errors, glGet, the attribute stacks, GLU.
 *
 *   scenetest [-v]            run everything
 *   scenetest --write         write the references again (then look at them!)
 *   scenetest --out DIR       also write each scene's picture to DIR
 *   scenetest NAME...         only these scenes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qoi.h"
#include "scenes.h"

#ifndef REFDIR
#define REFDIR "tests/gl/ref"
#endif

static int checks, fails, verbose;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* a scene's picture (malloc'ed, w * h pixels) */
static unsigned *render(void (*draw)(int, int), int w, int h) {
    HGLRC rc = hglCreateMemoryContext(w, h, 0);
    if (!rc) return NULL;
    hglMakeCurrent(rc);
    draw(w, h);
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) printf("  (GL error %#x while drawing)\n", e);
    int pw, ph, ps;
    unsigned *px = hglGetPixels(rc, &pw, &ph, &ps);
    unsigned *out = malloc(sizeof *out * (size_t)w * (size_t)h);
    for (int y = 0; y < h; y++) memcpy(out + (size_t)y * w, px + (size_t)y * ps, sizeof *out * (size_t)w);
    hglMakeCurrent(NULL);
    hglDeleteContext(rc);
    return out;
}

/* pixels off by more than 24 in a channel, and the mean channel difference */
static void compare(const unsigned *a, const unsigned *b, int n, int *bad, double *mean) {
    long sum = 0;
    *bad = 0;
    for (int i = 0; i < n; i++) {
        int worst = 0;
        for (int s = 0; s < 24; s += 8) {
            int d = abs((int)((a[i] >> s) & 255) - (int)((b[i] >> s) & 255));
            sum += d;
            if (d > worst) worst = d;
        }
        if (worst > 24) (*bad)++;
    }
    *mean = (double)sum / (3.0 * n);
}

static int write_ppm(const char *path, const unsigned *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { fputc((int)(px[i] >> 16) & 255, f); fputc((int)(px[i] >> 8) & 255, f); fputc((int)px[i] & 255, f); }
    return fclose(f) == 0;
}

static int run_scene(const scene_t *s, int write, const char *outdir) {
    unsigned *px = render(s->draw, s->w, s->h);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.qoi", REFDIR, s->name);
    if (!px) { CHECK(0, "%s: no context", s->name); return 0; }
    if (outdir) {
        char op[512];
        snprintf(op, sizeof op, "%s/%s.qoi", outdir, s->name);
        qoi_write(op, px, s->w, s->h);
    }
    if (write) {
        int ok = qoi_write(path, px, s->w, s->h);
        CHECK(ok, "%s: cannot write %s", s->name, path);
        if (verbose) printf("wrote %s (%s)\n", path, s->what);
        free(px);
        return ok;
    }
    int rw, rh;
    unsigned *ref = qoi_read(path, &rw, &rh);
    if (!ref || rw != s->w || rh != s->h) {
        CHECK(0, "%s: no reference %s (scenetest --write makes it)", s->name, path);
        free(px);
        free(ref);
        return 0;
    }
    int bad;
    double mean;
    compare(px, ref, s->w * s->h, &bad, &mean);
    int limit = s->w * s->h / 100;
    int ok = bad <= limit && mean < 1.0;
    CHECK(ok, "%s: %d pixels differ (at most %d), mean difference %.3f", s->name, bad, limit, mean);
    if (verbose || !ok) printf("  %-28s %4d pixels differ, mean %.3f  (%s)\n", s->name, bad, mean, s->what);
    if (!ok && outdir) {
        char op[512];
        snprintf(op, sizeof op, "%s/%s.actual.ppm", outdir, s->name);
        write_ppm(op, px, s->w, s->h);
    }
    free(px);
    free(ref);
    return ok;
}

/* ---------------- exact checks ---------------- */
static int mesh_mode;
static void s_mesh_mode(int w, int h) { mesh_setup(w, h); mesh_draw(mesh_mode); }

static void exact_arrays(void) {
    static const char *names[] = { "immediate", "glDrawElements", "glDrawArrays", "glArrayElement",
                                   "glInterleavedArrays + glDrawRangeElements", "a display list" };
    mesh_mode = 0;
    unsigned *ref = render(s_mesh_mode, 64, 64);
    for (int m = 1; m <= 5; m++) {
        mesh_mode = m;
        unsigned *px = render(s_mesh_mode, 64, 64);
        CHECK(px && ref && !memcmp(px, ref, 64 * 64 * 4), "%s does not give the immediate-mode picture", names[m]);
        free(px);
    }
    free(ref);
}

/* many tiny triangles, each with its own state: the queue (8192 records) and
 * the snapshots (512) overflow and are drawn in between */
static int overflow_flush;
static void s_overflow(int w, int h) {
    ortho_unit();
    clear_to(0, 0, 0);
    glEnable(GL_BLEND);
    for (int i = 0; i < 20000; i++) {
        float x = -1 + 2.0f * (i % 128) / 128, y = -1 + 2.0f * ((i / 128) % 128) / 128;
        glBlendFunc(GL_ONE, i & 1 ? GL_ONE : GL_ZERO);          /* a new snapshot every time */
        glColor3f((float)(i % 7) / 7, (float)(i % 11) / 11, (float)(i % 13) / 13);
        glBegin(GL_TRIANGLES);
        glVertex2f(x, y); glVertex2f(x + 0.03f, y); glVertex2f(x, y + 0.05f);
        glEnd();
        if (overflow_flush && i % 100 == 99) glFinish();
    }
}
static void exact_overflow(void) {
    overflow_flush = 0;
    unsigned *a = render(s_overflow, 128, 128);
    overflow_flush = 1;
    unsigned *b = render(s_overflow, 128, 128);
    CHECK(a && b && !memcmp(a, b, 128 * 128 * 4), "a full queue gives another picture than one drawn in pieces");
    free(a);
    free(b);
}

/* ---------------- API checks ---------------- */
static void api_checks(void) {
    HGLRC rc = hglCreateMemoryContext(32, 24, 0);
    CHECK(rc != NULL, "memory context");
    hglMakeCurrent(rc);
    CHECK(hglGetCurrentContext() == rc, "current context");
    CHECK(glGetError() == GL_NO_ERROR, "no error at the start");
    CHECK(!strcmp((const char *)glGetString(GL_VERSION), "1.3 Halcyon"), "version string");
    CHECK(strstr((const char *)glGetString(GL_EXTENSIONS), "GL_ARB_multitexture") != NULL, "extensions");
    glEnable(0x1234);
    CHECK(glGetError() == GL_INVALID_ENUM, "glEnable of nothing: GL_INVALID_ENUM");
    glBegin(GL_TRIANGLES);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnd();
    CHECK(glGetError() == GL_INVALID_OPERATION, "glClear inside glBegin: GL_INVALID_OPERATION");
    glEnd();
    CHECK(glGetError() == GL_INVALID_OPERATION, "glEnd without glBegin");
    glMatrixMode(GL_PROJECTION);
    for (int i = 0; i < 4; i++) glPushMatrix();
    CHECK(glGetError() == GL_STACK_OVERFLOW, "projection stack (4) overflows");
    for (int i = 0; i < 4; i++) glPopMatrix();
    CHECK(glGetError() == GL_STACK_UNDERFLOW, "projection stack underflows");
    glMatrixMode(GL_MODELVIEW);

    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    CHECK(vp[0] == 0 && vp[1] == 0 && vp[2] == 32 && vp[3] == 24, "the viewport starts as the frame buffer");
    glTranslatef(1, 2, 3);
    glScalef(2, 2, 2);
    GLfloat m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    CHECK(m[0] == 2 && m[5] == 2 && m[10] == 2 && m[12] == 1 && m[13] == 2 && m[14] == 3 && m[15] == 1, "modelview matrix");
    glGetFloatv(GL_TRANSPOSE_MODELVIEW_MATRIX, m);
    CHECK(m[3] == 1 && m[7] == 2 && m[11] == 3, "transposed modelview matrix");
    GLint depth;
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &depth);
    CHECK(depth == 2, "two texture units");

    /* attribute stack */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthFunc(GL_GREATER);
    glPushAttrib(GL_COLOR_BUFFER_BIT | GL_ENABLE_BIT);
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_LIGHTING);
    glPopAttrib();
    GLint bs, bd, df;
    glGetIntegerv(GL_BLEND_SRC, &bs);
    glGetIntegerv(GL_BLEND_DST, &bd);
    glGetIntegerv(GL_DEPTH_FUNC, &df);
    CHECK(glIsEnabled(GL_BLEND) && !glIsEnabled(GL_LIGHTING) && bs == GL_SRC_ALPHA && bd == GL_ONE, "glPopAttrib restores colour buffer and enables");
    CHECK(df == GL_LEQUAL, "glPopAttrib leaves groups not pushed alone");
    glPopAttrib();
    CHECK(glGetError() == GL_STACK_UNDERFLOW, "attribute stack underflows");
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glPopClientAttrib();
    GLint al;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &al);
    CHECK(al == 1, "glPopClientAttrib restores pixel storage");

    /* lights are kept in eye coordinates */
    glLoadIdentity();
    glTranslatef(0, 0, -5);
    float pos[4] = { 1, 2, 3, 1 }, got[4];
    glLightfv(GL_LIGHT1, GL_POSITION, pos);
    glGetLightfv(GL_LIGHT1, GL_POSITION, got);
    CHECK(got[0] == 1 && got[1] == 2 && got[2] == -2 && got[3] == 1, "light position in eye coordinates");

    /* textures */
    GLuint t[3];
    glGenTextures(3, t);
    CHECK(t[0] && t[1] != t[0] && t[2] != t[1] && glIsTexture(t[1]), "texture names");
    glBindTexture(GL_TEXTURE_2D, t[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 30, 16, 0, GL_RGB, GL_UNSIGNED_BYTE, texbuf);
    CHECK(glGetError() == GL_INVALID_VALUE, "texture sizes are powers of two");
    static unsigned char img[20 * 12 * 3];
    for (int i = 0; i < (int)sizeof img; i++) img[i] = (unsigned char)(i * 7);
    CHECK(gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, 20, 12, GL_RGB, GL_UNSIGNED_BYTE, img) == 0, "gluBuild2DMipmaps");
    GLint lw, lh;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &lw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &lh);
    CHECK(lw == 16 && lh == 16, "gluBuild2DMipmaps: 20 x 12 becomes 16 x 16 (%d x %d)", lw, lh);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 4, GL_TEXTURE_WIDTH, &lw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 5, GL_TEXTURE_WIDTH, &lh);
    CHECK(lw == 1 && lh == 0, "gluBuild2DMipmaps: every level down to 1 x 1");
    glDeleteTextures(3, t);
    CHECK(!glIsTexture(t[1]), "deleted texture");

    /* gluScaleImage: a 2 x 1 image to 4 x 1 (linear) and 4 x 2 to 2 x 1 (box) */
    unsigned char in[8] = { 0, 100, 200, 255, 10, 22, 30, 40 }, out[8];
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    CHECK(gluScaleImage(GL_LUMINANCE, 4, 2, GL_UNSIGNED_BYTE, in, 2, 1, GL_UNSIGNED_BYTE, out) == 0 &&
          out[0] == 33 && out[1] == 131, "gluScaleImage box filter (%d %d)", out[0], out[1]);
    unsigned char two[2] = { 0, 200 };
    CHECK(gluScaleImage(GL_LUMINANCE, 2, 1, GL_UNSIGNED_BYTE, two, 4, 1, GL_UNSIGNED_BYTE, out) == 0 &&
          out[0] == 0 && out[1] == 50 && out[2] == 150 && out[3] == 200, "gluScaleImage linear (%d %d %d %d)", out[0], out[1], out[2], out[3]);

    /* gluProject / gluUnProject round trip */
    GLdouble mv[16], pj[16], wx, wy, wz, ox, oy, oz;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(60, 4.0 / 3, 1, 50);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(3, 2, 5, 0, 0, 0, 0, 1, 0);
    glGetDoublev(GL_MODELVIEW_MATRIX, mv);
    glGetDoublev(GL_PROJECTION_MATRIX, pj);
    GLint view[4] = { 0, 0, 320, 240 };
    gluProject(0.5, -0.25, 1, mv, pj, view, &wx, &wy, &wz);
    gluUnProject(wx, wy, wz, mv, pj, view, &ox, &oy, &oz);
    CHECK(fabs(ox - 0.5) < 1e-3 && fabs(oy + 0.25) < 1e-3 && fabs(oz - 1) < 1e-3, "gluProject / gluUnProject (%f %f %f)", ox, oy, oz);
    gluProject(0, 0, 0, mv, pj, view, &wx, &wy, &wz);
    CHECK(fabs(wx - 160) < 1e-3 && fabs(wy - 120) < 1e-3, "gluLookAt centres the point looked at");

    /* glReadPixels: the bottom-left pixel is GL's (0, 0) */
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 32, 0, 24, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glDisable(GL_BLEND);                    /* (still on from the attribute test) */
    glClearColor(0, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColor3ub(255, 128, 0);
    glRecti(0, 0, 4, 2);                    /* the bottom-left corner */
    unsigned char rp[4 * 3];
    glReadPixels(0, 0, 4, 1, GL_RGB, GL_UNSIGNED_BYTE, rp);
    CHECK(rp[0] == 255 && rp[1] == 128 && rp[2] == 0 && rp[9] == 255, "glReadPixels at the bottom left (%d %d %d)", rp[0], rp[1], rp[2]);
    int pw, ph, ps;
    unsigned *px = hglGetPixels(rc, &pw, &ph, &ps);
    CHECK(px[(size_t)(ph - 1) * ps] == 0xFF8000 && px[0] == 0x0000FF, "the frame buffer's last row is GL's first");
    float zf;
    glReadPixels(10, 10, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &zf);
    CHECK(zf == 1.0f, "the depth buffer is cleared to 1");

    /* display lists: glGenLists, nesting, glCallLists with a base */
    GLuint base = glGenLists(3);
    CHECK(base != 0 && glIsList(base + 2) && !glIsList(base + 3), "glGenLists");
    glNewList(base, GL_COMPILE);
    glTranslatef(1, 0, 0);
    glEndList();
    glNewList(base + 1, GL_COMPILE_AND_EXECUTE);
    glCallList(base);
    glCallList(base);
    glEndList();                            /* executed once: x + 2 */
    glListBase(base);
    GLubyte ids[2] = { 1, 0 };
    glCallLists(2, GL_UNSIGNED_BYTE, ids);  /* x + 3 */
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    CHECK(m[12] == 5, "display lists: nested calls and glCallLists (x %g)", m[12]);
    glDeleteLists(base, 3);
    CHECK(!glIsList(base), "glDeleteLists");
    CHECK(glGetError() == GL_NO_ERROR, "no error at the end (%#x)", glGetError());

    /* a buffer context resized */
    static unsigned buf[40 * 30];
    HGLRC bc = hglCreateBufferContext(buf, 20, 10, 40, 0);
    hglMakeCurrent(bc);
    glClearColor(1, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    CHECK(buf[0] == 0xFF0000 && buf[19] == 0xFF0000 && buf[20] == 0 && buf[40 * 9] == 0xFF0000 && buf[40 * 10] == 0,
          "a buffer context draws in its rectangle only");
    CHECK(hglSetBuffer(bc, buf, 40, 30, 40), "hglSetBuffer");
    glViewport(0, 0, 40, 30);
    glClearColor(0, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    CHECK(buf[40 * 30 - 1] == 0x00FF00, "after hglSetBuffer the whole new size");
    hglMakeCurrent(NULL);
    hglDeleteContext(bc);
    hglDeleteContext(rc);
    CHECK(hglGetCurrentContext() == NULL, "no current context after deleting it");
    glClear(GL_COLOR_BUFFER_BIT);           /* (no context: nothing happens) */
}

int main(int argc, char **argv) {
    int write = 0;
    const char *outdir = NULL;
    const char *only[64];
    int nonly = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--write")) write = 1;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outdir = argv[++i];
        else if (nonly < 64) only[nonly++] = argv[i];
    }
    int nscenes = 0, scene_fails = 0;
    for (int i = 0; i < NSCENES; i++) {
        int take = !nonly;
        for (int k = 0; k < nonly; k++) if (!strcmp(only[k], scenes[i].name)) take = 1;
        if (!take) continue;
        nscenes++;
        if (!run_scene(&scenes[i], write, outdir)) scene_fails++;
    }
    if (write) {
        printf("scenes: %d references written to %s\n", nscenes, REFDIR);
        return fails != 0;
    }
    if (!nonly) {
        exact_arrays();
        exact_overflow();
        api_checks();
    }
    printf("gl: %d scenes (%d failed), %d checks in all, %d failed\n", nscenes, scene_fails, checks, fails);
    return fails != 0;
}
