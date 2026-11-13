/* Benchmark of Halcyon's 3D library on the host: a textured scene at
 * 640 x 480 (a mipmapped, fogged floor, lit textured boxes, a sphere,
 * translucent quads), drawn for about two seconds per configuration, the
 * frames per second printed. Not a pass / fail test: the number depends on
 * the computer. Built without sanitizers, -O2 as on Halcyon.
 *   bench [W H] [seconds] [--shot FILE.ppm]   (--shot: the first frame, to look at) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "GL/gl.h"
#include "GL/glu.h"
#include "GL/hgl.h"
#include "halmath.h"

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static GLuint tex_ground, tex_box;
static unsigned char img[256 * 256 * 3];

static void make_textures(void) {
    glGenTextures(1, &tex_ground);
    glBindTexture(GL_TEXTURE_2D, tex_ground);
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            int n = ((x * 7 + y * 13) ^ (x * y)) & 31;
            int c = ((x >> 5) + (y >> 5)) & 1;
            unsigned char *p = &img[(y * 256 + x) * 3];
            p[0] = (unsigned char)(c ? 60 + n : 90 + n); p[1] = (unsigned char)(c ? 120 + n : 150 + n); p[2] = (unsigned char)(40 + n);
        }
    gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, 256, 256, GL_RGB, GL_UNSIGNED_BYTE, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glGenTextures(1, &tex_box);
    glBindTexture(GL_TEXTURE_2D, tex_box);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int edge = x < 4 || y < 4 || x > 59 || y > 59;
            unsigned char *p = &img[(y * 64 + x) * 3];
            p[0] = (unsigned char)(edge ? 80 : 200 - (x ^ y) % 40); p[1] = (unsigned char)(edge ? 50 : 140); p[2] = (unsigned char)(edge ? 30 : 60);
        }
    gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, 64, 64, GL_RGB, GL_UNSIGNED_BYTE, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
}

static void box(void) {
    static const float n[6][3] = { { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
    static const float v[6][4][3] = {
        { { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } }, { { 1, -1, -1 }, { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 } },
        { { 1, -1, 1 }, { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 } }, { { -1, -1, -1 }, { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 } },
        { { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 }, { -1, 1, -1 } }, { { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 } } };
    static const float t[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    glBegin(GL_QUADS);
    for (int f = 0; f < 6; f++) {
        glNormal3fv(n[f]);
        for (int k = 0; k < 4; k++) { glTexCoord2fv(t[k]); glVertex3fv(v[f][k]); }
    }
    glEnd();
}

/* cfg: 0 everything, 1 no textures, 2 no lighting or fog */
static void frame(int w, int h, float time, int cfg, GLUquadric *q) {
    glClearColor(0.55f, 0.65f, 0.8f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(60, (double)w / h, 0.3, 120);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(12 * sinf(time * 0.3f), 3.5, 12 * cosf(time * 0.3f), 0, 0.5, 0, 0, 1, 0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    if (cfg != 2) {
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
        float pos[4] = { 0.4f, 1, 0.3f, 0 };
        glLightfv(GL_LIGHT0, GL_POSITION, pos);
        glEnable(GL_FOG);
        float fc[4] = { 0.55f, 0.65f, 0.8f, 1 };
        glFogfv(GL_FOG_COLOR, fc);
        glFogi(GL_FOG_MODE, GL_EXP2);
        glFogf(GL_FOG_DENSITY, 0.03f);
        glEnable(GL_COLOR_MATERIAL);
    }
    if (cfg != 1) glEnable(GL_TEXTURE_2D);
    /* the floor: 24 x 24 quads */
    glBindTexture(GL_TEXTURE_2D, tex_ground);
    glColor3f(1, 1, 1);
    glNormal3f(0, 1, 0);
    for (int j = 0; j < 24; j++) {
        glBegin(GL_QUAD_STRIP);
        for (int i = 0; i <= 24; i++) {
            float x = -60 + 5.0f * i, z0 = -60 + 5.0f * j, z1 = z0 + 5;
            glTexCoord2f(x / 10, z0 / 10); glVertex3f(x, 0, z0);   /* (counter-clockwise from above) */
            glTexCoord2f(x / 10, z1 / 10); glVertex3f(x, 0, z1);
        }
        glEnd();
    }
    /* 40 boxes */
    glBindTexture(GL_TEXTURE_2D, tex_box);
    for (int i = 0; i < 40; i++) {
        glPushMatrix();
        float a = i * 2.4f, r = 3 + (i % 8) * 1.6f;
        glTranslatef(r * cosf(a), 0.6f + 0.3f * sinf(time * 2 + i), r * sinf(a));
        glRotatef(time * 40 + i * 30, 0.3f, 1, 0.2f);
        glScalef(0.6f, 0.6f, 0.6f);
        glColor3f(0.6f + (i % 3) * 0.2f, 0.7f, 0.5f + (i % 2) * 0.4f);
        box();
        glPopMatrix();
    }
    /* a sphere */
    glPushMatrix();
    glTranslatef(0, 1.8f, 0);
    glRotatef(-90, 1, 0, 0);
    glColor3f(1, 1, 1);
    gluSphere(q, 1.6, 24, 16);
    glPopMatrix();
    /* translucent quads */
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (int i = 0; i < 6; i++) {
        glColor4f(1, 0.5f + 0.08f * i, 0.2f, 0.35f);
        glBegin(GL_QUADS);
        float x = -5 + 2.0f * i;
        glVertex3f(x, 0.2f, 5); glVertex3f(x + 1.5f, 0.2f, 5); glVertex3f(x + 1.5f, 2.5f, 5); glVertex3f(x, 2.5f, 5);
        glEnd();
    }
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_FOG);
    glDisable(GL_COLOR_MATERIAL);
}

int main(int argc, char **argv) {
    const char *shot = NULL;
    if (argc > 2 && !strcmp(argv[argc - 2], "--shot")) { shot = argv[argc - 1]; argc -= 2; }
    int w = argc > 2 ? atoi(argv[1]) : 640, h = argc > 2 ? atoi(argv[2]) : 480;
    double secs = argc > 3 ? atof(argv[3]) : argc == 2 ? atof(argv[1]) : 2.0;
    HGLRC rc = hglCreateMemoryContext(w, h, 0);
    if (!rc) return 1;
    hglMakeCurrent(rc);
    make_textures();
    GLUquadric *q = gluNewQuadric();
    gluQuadricTexture(q, GL_TRUE);
    static const char *names[] = { "textured, mipmapped (trilinear), lit, fog, blending",
                                   "untextured (lit, fog, blending)", "textured only (no lighting or fog)" };
    for (int cfg = 0; cfg < 3; cfg++) {
        double t0 = now(), t;
        int frames = 0;
        do {
            frame(w, h, frames * 0.04f, cfg, q);
            hglFinishFrame(rc);
            if (shot && !cfg && !frames) {
                int pw, ph, ps;
                unsigned *px = hglGetPixels(rc, &pw, &ph, &ps);
                FILE *f = fopen(shot, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", pw, ph);
                    for (int y = 0; y < ph; y++)
                        for (int x = 0; x < pw; x++) { unsigned p = px[y * ps + x]; fputc(p >> 16 & 255, f); fputc(p >> 8 & 255, f); fputc(p & 255, f); }
                    fclose(f);
                }
            }
            frames++;
            t = now() - t0;
        } while (t < secs || frames < 3);
        printf("bench: %dx%d %s: %.1f frames/s (%d frames in %.2f s)\n", w, h, names[cfg], frames / t, frames, t);
    }
    gluDeleteQuadric(q);
    hglDeleteContext(rc);
    return 0;
}
