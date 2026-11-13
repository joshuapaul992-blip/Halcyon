/* The scenes of the 3D library's tests (tests/gl/scenetest.c): each draws
 * one small picture with a feature of the pipeline, compared with its
 * reference in tests/gl/ref/NAME.qoi. Procedural textures only. */
#ifndef SCENES_H
#define SCENES_H
#include "GL/gl.h"
#include "GL/glu.h"
#include "GL/hgl.h"
#include "halmath.h"

#define SC_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* ---------------- textures ---------------- */
static unsigned char texbuf[256 * 256 * 4];

/* RGBA checkerboard: n x n texels, cells of c texels */
static void make_checker(int n, int c, const unsigned char a[4], const unsigned char b[4]) {
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            const unsigned char *s = ((x / c) + (y / c)) & 1 ? b : a;
            memcpy(&texbuf[(y * n + x) * 4], s, 4);
        }
}

static GLuint tex_checker(int n, int c, const unsigned char a[4], const unsigned char b[4], GLenum minf, GLenum magf) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    make_checker(n, c, a, b);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, texbuf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)magf);
    return t;
}

/* a 32 x 32 texture whose levels are told apart by colour: level 0 red
 * and white checks of 4 texels, level 1 green, 2 blue, 3 yellow, 4
 * magenta (each with white checks of 2 or 1 texels), level 5 (1 x 1) cyan */
static GLuint tex_mip_colored(GLenum minf) {
    static const unsigned char col[6][4] = { { 230, 30, 30, 255 }, { 30, 200, 30, 255 }, { 40, 60, 230, 255 },
                                             { 230, 220, 30, 255 }, { 220, 40, 220, 255 }, { 30, 220, 220, 255 } };
    static const unsigned char white[4] = { 255, 255, 255, 255 };
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    for (int l = 0; l <= 5; l++) {
        int n = 32 >> l, c = l == 0 ? 4 : l < 3 ? 2 : 1;
        make_checker(n, c, col[l], l == 5 ? col[l] : white);
        glTexImage2D(GL_TEXTURE_2D, l, GL_RGB, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, texbuf);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return t;
}

static void ortho_unit(void) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1, 1, -1, 1, -10, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void persp(double fov, int w, int h, double zn, double zf) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(fov, (double)w / h, zn, zf);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void clear_to(float r, float g, float b) {
    glClearColor(r, g, b, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void quad_tex(float x0, float y0, float x1, float y1, float z, float s0, float t0, float s1, float t1) {
    glBegin(GL_QUADS);
    glTexCoord2f(s0, t0); glVertex3f(x0, y0, z);
    glTexCoord2f(s1, t0); glVertex3f(x1, y0, z);
    glTexCoord2f(s1, t1); glVertex3f(x1, y1, z);
    glTexCoord2f(s0, t1); glVertex3f(x0, y1, z);
    glEnd();
}

/* a floor from z = 2 to z = -60 at y = -1, the texture repeated 32 times */
static void floor_plane(void) {
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(-8, -1, 2);
    glTexCoord2f(4, 0); glVertex3f(8, -1, 2);
    glTexCoord2f(4, 32); glVertex3f(8, -1, -60);
    glTexCoord2f(0, 32); glVertex3f(-8, -1, -60);
    glEnd();
}

/* a grid of n x n quads in the z = 0 plane from -1 to 1, normal +z */
static void grid(int n) {
    glNormal3f(0, 0, 1);
    for (int j = 0; j < n; j++) {
        glBegin(GL_QUAD_STRIP);
        for (int i = 0; i <= n; i++) {
            float x = -1 + 2.0f * i / n;
            glTexCoord2f((float)i / n, (float)(j + 1) / n); glVertex3f(x, -1 + 2.0f * (j + 1) / n, 0);
            glTexCoord2f((float)i / n, (float)j / n); glVertex3f(x, -1 + 2.0f * j / n, 0);
        }
        glEnd();
    }
}

/* ---------------- the scenes ---------------- */
static void s_gouraud(int w, int h) {
    ortho_unit();
    clear_to(0, 0, 0);
    glBegin(GL_TRIANGLES);
    glColor3f(1, 0, 0); glVertex2f(-0.9f, -0.9f);
    glColor3f(0, 1, 0); glVertex2f(0.9f, -0.9f);
    glColor3f(0, 0, 1); glVertex2f(0, 0.9f);
    glEnd();
}

static void s_flat(int w, int h) {
    ortho_unit();
    clear_to(0, 0, 0);
    glShadeModel(GL_FLAT);
    glBegin(GL_QUAD_STRIP);                 /* each quad takes the colour of its last vertex */
    for (int i = 0; i <= 4; i++) {
        float x = -0.9f + 0.45f * i;
        glColor3f(i & 1 ? 1.0f : 0.2f, i & 2 ? 1.0f : 0.2f, i >= 3 ? 1.0f : 0.3f);
        glVertex2f(x, 0.8f);
        glVertex2f(x, 0.1f);
    }
    glEnd();
    glBegin(GL_TRIANGLES);                  /* a triangle takes its third vertex's colour */
    glColor3f(1, 0, 0); glVertex2f(-0.8f, -0.9f);
    glColor3f(0, 1, 0); glVertex2f(0.8f, -0.9f);
    glColor3f(1, 1, 0); glVertex2f(0, -0.1f);
    glEnd();
}

static void s_depth(int w, int h) {
    persp(50, w, h, 1, 20);
    clear_to(0.1f, 0.1f, 0.1f);
    glEnable(GL_DEPTH_TEST);
    glTranslatef(0, 0, -4);
    glColor3f(0.2f, 0.8f, 0.2f);            /* green, facing the viewer */
    glRectf(-1, -1, 1, 1);
    glColor3f(0.9f, 0.2f, 0.2f);            /* red, through it: its left half behind, its right half in front */
    glBegin(GL_QUADS);
    glVertex3f(-1.4f, -0.5f, -1); glVertex3f(1.4f, -0.5f, 1); glVertex3f(1.4f, 0.5f, 1); glVertex3f(-1.4f, 0.5f, -1);
    glEnd();
    glColor3f(0.2f, 0.3f, 1);               /* blue, drawn last but behind both */
    glBegin(GL_TRIANGLES);
    glVertex3f(-1.6f, -1.4f, -2); glVertex3f(1.6f, -1.4f, -2); glVertex3f(0, 1.6f, -2);
    glEnd();
}

static void s_depth_funcs(int w, int h) {
    ortho_unit();
    clear_to(0, 0, 0);
    glEnable(GL_DEPTH_TEST);
    glColor3f(0.3f, 0.3f, 0.3f);            /* a gray square at depth 0.5 */
    glRectf(-0.8f, -0.8f, 0.8f, 0.8f);
    /* left: GL_GREATER passes behind it only */
    glDepthFunc(GL_GREATER);
    glColor3f(1, 0, 0);
    glBegin(GL_QUADS); glVertex3f(-1, 0.1f, -0.5f); glVertex3f(0, 0.1f, -0.5f); glVertex3f(0, 0.9f, -0.5f); glVertex3f(-1, 0.9f, -0.5f); glEnd();
    /* right: GL_EQUAL at the same depth */
    glDepthFunc(GL_EQUAL);
    glColor3f(0, 1, 0);
    glRectf(0.1f, 0.1f, 1, 0.9f);
    /* bottom: depth mask off: a blue quad in front writes no depth, so the
     * yellow one behind it, drawn after, covers it */
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
    glColor3f(0, 0, 1);
    glBegin(GL_QUADS); glVertex3f(-0.9f, -0.9f, 0.5f); glVertex3f(0.5f, -0.9f, 0.5f); glVertex3f(0.5f, -0.1f, 0.5f); glVertex3f(-0.9f, -0.1f, 0.5f); glEnd();
    glDepthMask(GL_TRUE);
    glColor3f(1, 1, 0);
    glBegin(GL_QUADS); glVertex3f(-0.5f, -0.7f, 0.2f); glVertex3f(0.9f, -0.7f, 0.2f); glVertex3f(0.9f, -0.3f, 0.2f); glVertex3f(-0.5f, -0.7f + 0.4f, 0.2f); glEnd();
}

static const unsigned char C_RED[4] = { 220, 40, 40, 255 }, C_WHITE[4] = { 250, 250, 250, 255 }, C_BLUE[4] = { 40, 60, 210, 255 },
                           C_YELLOW[4] = { 240, 210, 40, 255 }, C_DARK[4] = { 30, 30, 40, 255 };

static void s_tex_mag(int w, int h, GLenum f) {
    ortho_unit();
    clear_to(0, 0, 0);
    glEnable(GL_TEXTURE_2D);
    tex_checker(4, 1, C_RED, C_YELLOW, f, f);
    glColor3f(1, 1, 1);
    quad_tex(-1, -1, 1, 1, 0, 0, 0, 1, 1);
}
static void s_tex_mag_nearest(int w, int h) { s_tex_mag(w, h, GL_NEAREST); }
static void s_tex_mag_linear(int w, int h) { s_tex_mag(w, h, GL_LINEAR); }

static void s_tex_min(int w, int h, GLenum f) {
    persp(60, w, h, 0.5, 100);
    clear_to(0.5f, 0.5f, 0.5f);
    glEnable(GL_TEXTURE_2D);
    tex_mip_colored(f);
    glColor3f(1, 1, 1);
    floor_plane();
}
static void s_tex_nearest(int w, int h) { s_tex_min(w, h, GL_NEAREST); }
static void s_tex_linear(int w, int h) { s_tex_min(w, h, GL_LINEAR); }
static void s_tex_nearest_mipmap_nearest(int w, int h) { s_tex_min(w, h, GL_NEAREST_MIPMAP_NEAREST); }
static void s_tex_linear_mipmap_nearest(int w, int h) { s_tex_min(w, h, GL_LINEAR_MIPMAP_NEAREST); }
static void s_tex_nearest_mipmap_linear(int w, int h) { s_tex_min(w, h, GL_NEAREST_MIPMAP_LINEAR); }
static void s_tex_linear_mipmap_linear(int w, int h) { s_tex_min(w, h, GL_LINEAR_MIPMAP_LINEAR); }

/* gluBuild2DMipmaps of a 48 x 40 image (not a power of two: scaled first) */
static void s_tex_glu_mipmap(int w, int h) {
    persp(60, w, h, 0.5, 100);
    clear_to(0.5f, 0.5f, 0.5f);
    static unsigned char img[40][48][3];
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 48; x++) {
            int stripe = (x / 6 + y / 5) & 1;
            img[y][x][0] = (unsigned char)(stripe ? 240 : 20);
            img[y][x][1] = (unsigned char)(x * 5);
            img[y][x][2] = (unsigned char)(stripe ? 20 : 200);
        }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, 48, 40, GL_RGB, GL_UNSIGNED_BYTE, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glEnable(GL_TEXTURE_2D);
    glColor3f(1, 1, 1);
    floor_plane();
}

static void s_tex_wrap(int w, int h) {
    ortho_unit();
    clear_to(0, 0, 0);
    glEnable(GL_TEXTURE_2D);
    tex_checker(8, 2, C_BLUE, C_WHITE, GL_NEAREST, GL_NEAREST);
    glColor3f(1, 1, 1);
    /* left: clamped (the edge texels stretch out), right: repeated */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    quad_tex(-1, -1, -0.05f, 1, 0, -0.5f, -0.5f, 1.5f, 1.5f);
    glFinish();                             /* (the parameters change: the left quad keeps its own) */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    quad_tex(0.05f, -1, 1, 1, 0, -0.5f, -0.5f, 1.5f, 1.5f);
}

/* rows: texture formats (RGB, RGBA, LUMINANCE, ALPHA, INTENSITY); columns:
 * GL_MODULATE, REPLACE, DECAL, BLEND, ADD; orange fragments, a blue
 * environment colour; the texture: a horizontal ramp, its alpha a vertical one */
static void s_tex_env(int w, int h) {
    ortho_unit();
    clear_to(0.15f, 0.15f, 0.15f);
    static const GLenum fmt[5] = { GL_RGB, GL_RGBA, GL_LUMINANCE, GL_ALPHA, GL_INTENSITY };
    static const GLenum env[5] = { GL_MODULATE, GL_REPLACE, GL_DECAL, GL_BLEND, GL_ADD };
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            unsigned char *p = &texbuf[(y * 16 + x) * 4];
            p[0] = (unsigned char)(x * 17); p[1] = (unsigned char)(255 - x * 17); p[2] = (unsigned char)(x < 8 ? 255 : 60);
            p[3] = (unsigned char)(y * 17);
        }
    GLuint t[5];
    glGenTextures(5, t);
    for (int i = 0; i < 5; i++) {
        glBindTexture(GL_TEXTURE_2D, t[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt[i], 16, 16, 0, GL_RGBA, GL_UNSIGNED_BYTE, texbuf);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    float ec[4] = { 0.1f, 0.3f, 1, 0.5f };
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, ec);
    glEnable(GL_TEXTURE_2D);
    glColor4f(1, 0.6f, 0.2f, 0.8f);
    for (int r = 0; r < 5; r++)
        for (int e = 0; e < 5; e++) {
            glBindTexture(GL_TEXTURE_2D, t[r]);
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, (GLint)env[e]);
            float x0 = -1 + 0.4f * e, y1 = 1 - 0.4f * r;
            quad_tex(x0 + 0.02f, y1 - 0.38f, x0 + 0.38f, y1 - 0.02f, 0, 0, 0, 1, 1);
        }
}

/* a checkerboard on a quad seen at a steep angle: straight lines of checks
 * that shrink with distance (affine interpolation would bend them) */
static void s_persp(int w, int h) {
    persp(60, w, h, 0.5, 50);
    clear_to(0, 0, 0.2f);
    glEnable(GL_TEXTURE_2D);
    tex_checker(16, 2, C_WHITE, C_DARK, GL_NEAREST, GL_NEAREST);
    glColor3f(1, 1, 1);
    glTranslatef(0, 0, -3);
    glRotatef(-70, 1, 0, 0);
    glRotatef(20, 0, 0, 1);
    glBegin(GL_TRIANGLES);                  /* two triangles: the diagonal must not show */
    glTexCoord2f(0, 0); glVertex3f(-2, -4, 0);
    glTexCoord2f(1, 0); glVertex3f(2, -4, 0);
    glTexCoord2f(1, 1); glVertex3f(2, 4, 0);
    glTexCoord2f(0, 0); glVertex3f(-2, -4, 0);
    glTexCoord2f(1, 1); glVertex3f(2, 4, 0);
    glTexCoord2f(0, 1); glVertex3f(-2, 4, 0);
    glEnd();
}

static void lit_sphere_setup(int w, int h) {
    persp(45, w, h, 1, 20);
    clear_to(0.05f, 0.05f, 0.1f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_NORMALIZE);
}

static void s_light_dir(int w, int h) {
    lit_sphere_setup(w, h);
    float pos[4] = { -1, 1, 1, 0 }, spec[4] = { 1, 1, 1, 1 }, diff[4] = { 0.9f, 0.15f, 0.1f, 1 }, amb[4] = { 0.3f, 0.05f, 0.05f, 1 };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);  /* from the upper left, in eye coordinates */
    glMaterialfv(GL_FRONT, GL_DIFFUSE, diff);
    glMaterialfv(GL_FRONT, GL_AMBIENT, amb);
    glMaterialfv(GL_FRONT, GL_SPECULAR, spec);
    glMaterialf(GL_FRONT, GL_SHININESS, 40);
    glTranslatef(0, 0, -5);
    GLUquadric *q = gluNewQuadric();
    gluSphere(q, 1.6, 32, 24);
    gluDeleteQuadric(q);
}

/* a point light close above a tessellated floor: a bright spot that fades with distance */
static void s_light_point(int w, int h) {
    lit_sphere_setup(w, h);
    float black[4] = { 0, 0, 0, 1 }, white[4] = { 1, 1, 1, 1 };
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, black);
    glTranslatef(0, 0, -4);
    glRotatef(-60, 1, 0, 0);
    float pos[4] = { 0.4f, 0.3f, 0.5f, 1 };  /* (set with the modelview of the floor) */
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
    glLightf(GL_LIGHT0, GL_CONSTANT_ATTENUATION, 0.2f);
    glLightf(GL_LIGHT0, GL_QUADRATIC_ATTENUATION, 2.0f);
    float diff[4] = { 0.3f, 0.9f, 0.4f, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, diff);
    glScalef(2, 2, 1);
    grid(24);
}

/* a spot light on a wall: a bright disc with a soft edge, nothing outside its cone */
static void s_light_spot(int w, int h) {
    lit_sphere_setup(w, h);
    float black[4] = { 0, 0, 0, 1 }, amb[4] = { 0.1f, 0.1f, 0.15f, 1 };
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    float pos[4] = { -0.6f, 0.5f, -1.0f, 1 }, dir[4] = { 0.25f, -0.25f, -1, 0 };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glLightfv(GL_LIGHT0, GL_SPOT_DIRECTION, dir);
    glLightf(GL_LIGHT0, GL_SPOT_CUTOFF, 25);
    glLightf(GL_LIGHT0, GL_SPOT_EXPONENT, 8);
    glLightfv(GL_LIGHT0, GL_SPECULAR, black);
    float diff[4] = { 1, 0.85f, 0.5f, 1 };
    glMaterialfv(GL_FRONT, GL_DIFFUSE, diff);
    glMaterialfv(GL_FRONT, GL_AMBIENT, diff);
    glTranslatef(0, 0, -4);
    glScalef(2.2f, 2.2f, 1);
    grid(40);
}

/* two-sided lighting: an open cylinder, the outside yellow, the inside blue,
 * and colour material on three small spheres */
static void s_light_twoside(int w, int h) {
    lit_sphere_setup(w, h);
    float pos[4] = { 0.5f, 1, 1, 0 };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    float front[4] = { 0.9f, 0.8f, 0.1f, 1 }, back[4] = { 0.2f, 0.4f, 1, 1 };
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, front);
    glMaterialfv(GL_BACK, GL_AMBIENT_AND_DIFFUSE, back);
    glPushMatrix();
    glTranslatef(-0.4f, 0.3f, -6);
    glRotatef(-60, 1, 0, 0);
    glRotatef(20, 0, 1, 0);
    GLUquadric *q = gluNewQuadric();
    gluCylinder(q, 1, 1, 1.6, 24, 2);
    glPopMatrix();
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_FALSE);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
    for (int i = 0; i < 3; i++) {
        glPushMatrix();
        glTranslatef(-1.2f + 1.2f * i, -1.3f, -6);
        glColor3f(i == 0 ? 1.0f : 0.1f, i == 1 ? 1.0f : 0.1f, i == 2 ? 1.0f : 0.1f);
        gluSphere(q, 0.5, 16, 12);
        glPopMatrix();
    }
    gluDeleteQuadric(q);
}

/* separate specular colour on a textured sphere: the highlight stays white
 * over the dark checks */
static void s_light_specular(int w, int h) {
    lit_sphere_setup(w, h);
    float pos[4] = { 1, 1, 1, 0 }, spec[4] = { 1, 1, 1, 1 }, diff[4] = { 1, 1, 1, 1 };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glMaterialfv(GL_FRONT, GL_DIFFUSE, diff);
    glMaterialfv(GL_FRONT, GL_SPECULAR, spec);
    glMaterialf(GL_FRONT, GL_SHININESS, 20);
    glLightModeli(GL_LIGHT_MODEL_COLOR_CONTROL, GL_SEPARATE_SPECULAR_COLOR);
    glEnable(GL_TEXTURE_2D);
    tex_checker(16, 2, C_BLUE, C_DARK, GL_LINEAR, GL_NEAREST);
    glTranslatef(0, 0, -5);
    glRotatef(-90, 1, 0, 0);
    GLUquadric *q = gluNewQuadric();
    gluQuadricTexture(q, GL_TRUE);
    gluSphere(q, 1.6, 32, 24);
    gluDeleteQuadric(q);
}

/* a row of white posts going away, in fog */
static void s_fog(int w, int h, GLenum mode) {
    persp(60, w, h, 0.5, 60);
    clear_to(0.6f, 0.7f, 0.8f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_FOG);
    float fc[4] = { 0.6f, 0.7f, 0.8f, 1 };
    glFogfv(GL_FOG_COLOR, fc);
    glFogi(GL_FOG_MODE, (GLint)mode);
    glFogf(GL_FOG_START, 2);
    glFogf(GL_FOG_END, 25);
    glFogf(GL_FOG_DENSITY, mode == GL_EXP ? 0.12f : 0.08f);
    glColor3f(0.8f, 0.25f, 0.1f);           /* the ground: one big quad, fogged per vertex */
    glBegin(GL_QUADS);
    glVertex3f(-20, -1, 0); glVertex3f(20, -1, 0); glVertex3f(20, -1, -40); glVertex3f(-20, -1, -40);
    glEnd();
    for (int i = 0; i < 10; i++) {
        float z = -2.0f - i * 3.0f;
        for (int side = -1; side <= 1; side += 2) {
            glColor3f(1, 1, 1);
            glBegin(GL_QUADS);
            glVertex3f(side * 1.5f - 0.3f, -1, z); glVertex3f(side * 1.5f + 0.3f, -1, z);
            glVertex3f(side * 1.5f + 0.3f, 1.2f, z); glVertex3f(side * 1.5f - 0.3f, 1.2f, z);
            glEnd();
        }
    }
}
static void s_fog_linear(int w, int h) { s_fog(w, h, GL_LINEAR); }
static void s_fog_exp(int w, int h) { s_fog(w, h, GL_EXP); }
static void s_fog_exp2(int w, int h) { s_fog(w, h, GL_EXP2); }

/* translucent quads (source alpha) over checks, and additive ones */
static void s_blend(int w, int h) {
    ortho_unit();
    clear_to(0, 0, 0);
    glEnable(GL_TEXTURE_2D);
    tex_checker(8, 1, C_WHITE, C_DARK, GL_NEAREST, GL_NEAREST);
    glColor3f(1, 1, 1);
    quad_tex(-1, -1, 1, 1, 0, 0, 0, 2, 2);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 0, 0, 0.5f); glRectf(-0.9f, 0.0f, 0.2f, 0.9f);
    glColor4f(0, 1, 0, 0.5f); glRectf(-0.5f, 0.3f, 0.6f, 0.95f);
    glColor4f(0, 0, 1, 0.75f); glRectf(-0.2f, 0.1f, 0.9f, 0.6f);
    glBlendFunc(GL_ONE, GL_ONE);
    glColor3f(0.6f, 0, 0); glRectf(-0.9f, -0.9f, 0.1f, -0.1f);
    glColor3f(0, 0, 0.6f); glRectf(-0.4f, -0.8f, 0.9f, -0.3f);
    /* a smooth alpha ramp: transparent at the left, opaque at the right */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_QUADS);
    glColor4f(1, 1, 0, 0); glVertex2f(-1, -1); glColor4f(1, 1, 0, 1); glVertex2f(1, -1);
    glColor4f(1, 1, 0, 1); glVertex2f(1, -0.95f); glColor4f(1, 1, 0, 0); glVertex2f(-1, -0.95f);
    glEnd();
}

/* the alpha test cuts discs out of quads; the cut parts write no depth, so
 * a quad drawn behind afterwards shows through the holes */
static void s_alpha_test(int w, int h) {
    ortho_unit();
    clear_to(0.2f, 0.2f, 0.25f);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            float dx = (x - 15.5f) / 16, dy = (y - 15.5f) / 16;
            float d = sqrtf(dx * dx + dy * dy);
            unsigned char *p = &texbuf[(y * 32 + x) * 4];
            p[0] = 255; p[1] = (unsigned char)(128 + x * 4); p[2] = 40;
            p[3] = (unsigned char)SC_CLAMP(255 * (1.2f - d), 0.0f, 255.0f);
        }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, texbuf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glColor3f(1, 1, 1);
    quad_tex(-0.95f, -0.2f, 0.25f, 0.95f, 0.5f, 0, 0, 1, 1);
    glAlphaFunc(GL_LESS, 0.3f);             /* only the outer ring */
    quad_tex(-0.25f, -0.95f, 0.95f, 0.2f, 0.5f, 0, 0, 1, 1);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_TEXTURE_2D);
    glColor3f(0.1f, 0.4f, 1);
    glBegin(GL_QUADS); glVertex3f(-1, -1, -0.5f); glVertex3f(1, -1, -0.5f); glVertex3f(1, 1, -0.5f); glVertex3f(-1, 1, -0.5f); glEnd();
}

/* geometry through the near plane: a floor that starts behind the eye and a
 * wall crossing the near plane at an angle */
static void s_clip_near(int w, int h) {
    persp(70, w, h, 0.5, 100);
    clear_to(0.3f, 0.5f, 0.8f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);
    tex_checker(16, 4, C_WHITE, C_RED, GL_LINEAR, GL_NEAREST);
    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);                  /* one triangle, its corners far outside every plane */
    glTexCoord2f(0, 0); glVertex3f(-100, -1, 50);
    glTexCoord2f(20, 0); glVertex3f(100, -1, 50);
    glTexCoord2f(10, 30); glVertex3f(0, -1, -80);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);                      /* the wall: from behind the eye to the far right */
    glColor3f(0.2f, 0.8f, 0.3f); glVertex3f(0.6f, -1, 3); glVertex3f(3, -1, -6);
    glColor3f(0.1f, 0.3f, 0.1f); glVertex3f(3, 1.5f, -6); glVertex3f(0.6f, 1.5f, 3);
    glEnd();
}

/* two user clip planes cut a lit sphere; the inside shows (two-sided lighting) */
static void s_clip_user(int w, int h) {
    lit_sphere_setup(w, h);
    float pos[4] = { 0.3f, 0.6f, 1, 0 };
    glLightfv(GL_LIGHT0, GL_POSITION, pos);
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE);
    float front[4] = { 0.8f, 0.8f, 0.8f, 1 }, back[4] = { 0.9f, 0.3f, 0.1f, 1 };
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, front);
    glMaterialfv(GL_BACK, GL_AMBIENT_AND_DIFFUSE, back);
    glTranslatef(0, 0, -5);
    GLdouble p0[4] = { -1, 0, 0, 0.5 };     /* keeps x <= 0.5 */
    GLdouble p1[4] = { 0, -0.5, -1, 0.4 };  /* keeps z <= 0.4 - y / 2: the front cap goes, the inside shows */
    glClipPlane(GL_CLIP_PLANE0, p0);
    glClipPlane(GL_CLIP_PLANE1, p1);
    glEnable(GL_CLIP_PLANE0);
    glEnable(GL_CLIP_PLANE1);
    glRotatef(30, 0, 1, 0);                 /* (the planes were set before: they stay put) */
    GLUquadric *q = gluNewQuadric();
    gluSphere(q, 1.5, 32, 24);
    gluDeleteQuadric(q);
}

/* culling: the left half culls back faces, the right half front faces;
 * counter-clockwise triangles are red, clockwise green; the bottom row
 * takes clockwise as the front */
static void s_cull(int w, int h) {
    ortho_unit();
    clear_to(0, 0, 0);
    glEnable(GL_CULL_FACE);
    for (int row = 0; row < 2; row++) {
        glFrontFace(row ? GL_CW : GL_CCW);
        for (int half = 0; half < 2; half++) {
            glCullFace(half ? GL_FRONT : GL_BACK);
            float x0 = half ? 0.05f : -0.95f, y0 = row ? -0.95f : 0.05f;
            glColor3f(1, 0.2f, 0.2f);       /* counter-clockwise */
            glBegin(GL_TRIANGLES);
            glVertex2f(x0, y0); glVertex2f(x0 + 0.42f, y0); glVertex2f(x0 + 0.21f, y0 + 0.85f);
            glEnd();
            glColor3f(0.2f, 1, 0.2f);       /* clockwise */
            glBegin(GL_TRIANGLES);
            glVertex2f(x0 + 0.48f, y0); glVertex2f(x0 + 0.69f, y0 + 0.85f); glVertex2f(x0 + 0.9f, y0);
            glEnd();
        }
    }
    glCullFace(GL_FRONT_AND_BACK);           /* nothing at all */
    glColor3f(1, 1, 1);
    glRectf(-1, -1, 1, 1);
}

/* two texture units: a checkerboard times a light map (a soft disc), and
 * on the lower quad the light map added */
static void s_multitex(int w, int h) {
    ortho_unit();
    clear_to(0, 0, 0);
    GLuint base = tex_checker(16, 2, C_YELLOW, C_BLUE, GL_LINEAR, GL_NEAREST);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            float dx = (x - 15.5f) / 16, dy = (y - 15.5f) / 16;
            texbuf[y * 32 + x] = (unsigned char)SC_CLAMP(255 * (1 - sqrtf(dx * dx + dy * dy)), 0.0f, 255.0f);
        }
    GLuint lm;
    glGenTextures(1, &lm);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, lm);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 32, 32, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, texbuf);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glEnable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, base);
    glEnable(GL_TEXTURE_2D);
    glColor3f(1, 1, 1);
    for (int i = 0; i < 2; i++) {
        glActiveTexture(GL_TEXTURE1);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, i ? GL_ADD : GL_MODULATE);
        float y0 = i ? -1 : 0.02f, y1 = i ? -0.02f : 1;
        glBegin(GL_QUADS);
        glMultiTexCoord2f(GL_TEXTURE0, 0, 0); glMultiTexCoord2f(GL_TEXTURE1, 0, 0); glVertex2f(-1, y0);
        glMultiTexCoord2f(GL_TEXTURE0, 2, 0); glMultiTexCoord2f(GL_TEXTURE1, 1, 0); glVertex2f(1, y0);
        glMultiTexCoord2f(GL_TEXTURE0, 2, 1); glMultiTexCoord2f(GL_TEXTURE1, 1, 1); glVertex2f(1, y1);
        glMultiTexCoord2f(GL_TEXTURE0, 0, 1); glMultiTexCoord2f(GL_TEXTURE1, 0, 1); glVertex2f(-1, y1);
        glEnd();
    }
}

/* four viewports with one scene each; the scissor box cuts the last two
 * and limits a clear */
static void s_viewport_scissor(int w, int h) {
    clear_to(0.1f, 0.1f, 0.1f);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1, 1, -1, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    for (int i = 0; i < 4; i++) {
        int vx = (i & 1) * w / 2, vy = (i >> 1) * h / 2;
        glViewport(vx, vy, w / 2, h / 2);
        if (i >= 2) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(vx + 4, vy + 8, w / 4, h / 4);
            glClearColor(0.5f, 0.0f, 0.5f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glBegin(GL_TRIANGLES);
        glColor3f(i == 0, i == 1, i >= 2); glVertex2f(-0.9f, -0.9f);
        glColor3f(1, 1, i == 3); glVertex2f(0.9f, -0.9f);
        glColor3f(0.5f, 0.5f, 0.5f); glVertex2f(0, 0.9f);
        glEnd();
        glDisable(GL_SCISSOR_TEST);
    }
}

/* lines and points of several widths and sizes, line strips and loops,
 * smooth-shaded lines */
static void s_lines_points(int w, int h) {
    ortho_unit();
    clear_to(0, 0, 0);
    for (int i = 0; i < 4; i++) {
        glLineWidth(1.0f + i);
        glBegin(GL_LINES);
        glColor3f(1, 0, 0); glVertex2f(-0.9f, 0.9f - 0.15f * i);
        glColor3f(0, 1, 0); glVertex2f(-0.1f, 0.75f - 0.15f * i);
        glEnd();
    }
    glLineWidth(1);
    glColor3f(1, 1, 0);
    glBegin(GL_LINE_LOOP);
    glVertex2f(0.1f, 0.1f); glVertex2f(0.9f, 0.1f); glVertex2f(0.9f, 0.9f); glVertex2f(0.1f, 0.9f);
    glEnd();
    glColor3f(0, 1, 1);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i <= 8; i++) glVertex2f(-0.9f + 0.2f * i, (i & 1) ? -0.15f : -0.45f);
    glEnd();
    for (int i = 0; i < 5; i++) {
        glPointSize(1.0f + 2 * i);
        glBegin(GL_POINTS);
        glColor3f(1, 0.5f + 0.1f * i, 1);
        glVertex2f(-0.8f + 0.4f * i, -0.75f);
        glEnd();
    }
}

/* polygon modes and offset: a filled sphere with its wireframe drawn on it
 * (offset keeps the lines in front), and a sphere drawn as points */
static void s_polymode(int w, int h) {
    lit_sphere_setup(w, h);
    GLUquadric *q = gluNewQuadric();
    glPushMatrix();
    glTranslatef(-0.9f, 0, -5);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1, 1);
    gluSphere(q, 1, 12, 8);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_LIGHTING);
    glColor3f(1, 1, 0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    gluSphere(q, 1, 12, 8);
    glPopMatrix();
    glPolygonMode(GL_FRONT_AND_BACK, GL_POINT);
    glPointSize(2);
    glColor3f(0.3f, 1, 1);
    glTranslatef(1.1f, 0, -5);
    gluSphere(q, 0.9, 10, 8);
    gluDeleteQuadric(q);
}

/* texture coordinate generation: object-linear stripes on a sphere and a
 * sphere map on another; the texture matrix turns the third */
static void s_texgen(int w, int h) {
    lit_sphere_setup(w, h);
    glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    tex_checker(16, 4, C_WHITE, C_BLUE, GL_LINEAR, GL_LINEAR);
    glColor3f(1, 1, 1);
    GLUquadric *q = gluNewQuadric();
    glEnable(GL_TEXTURE_GEN_S);
    glEnable(GL_TEXTURE_GEN_T);
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
    glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
    float ps[4] = { 0.5f, 0, 0, 0.5f }, pt[4] = { 0, 0, 0.5f, 0.5f };
    glTexGenfv(GL_S, GL_OBJECT_PLANE, ps);
    glTexGenfv(GL_T, GL_OBJECT_PLANE, pt);
    glPushMatrix();
    glTranslatef(-1.1f, 0.6f, -6);
    glRotatef(30, 1, 0, 0);
    gluSphere(q, 0.9, 24, 16);
    glPopMatrix();
    glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
    glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
    glPushMatrix();
    glTranslatef(1.1f, 0.6f, -6);
    gluSphere(q, 0.9, 24, 16);
    glPopMatrix();
    glDisable(GL_TEXTURE_GEN_S);
    glDisable(GL_TEXTURE_GEN_T);
    glMatrixMode(GL_TEXTURE);
    glRotatef(30, 0, 0, 1);
    glScalef(3, 1, 1);
    glMatrixMode(GL_MODELVIEW);
    glTranslatef(0, -1.1f, -6);
    quad_tex(-1.5f, -0.8f, 1.5f, 0.8f, 0, 0, 0, 1, 1);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    gluDeleteQuadric(q);
}

/* the colour mask: the red channel only, then green only, on a white-on-black
 * picture; a masked clear */
static void s_colormask(int w, int h) {
    ortho_unit();
    clear_to(0, 0, 0);
    glColor3f(1, 1, 1);
    glColorMask(GL_TRUE, GL_FALSE, GL_FALSE, GL_TRUE);
    glRectf(-0.9f, -0.9f, 0.3f, 0.9f);
    glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
    glRectf(-0.3f, -0.9f, 0.9f, 0.9f);
    glColorMask(GL_FALSE, GL_FALSE, GL_TRUE, GL_TRUE);
    glClearColor(1, 1, 1, 1);
    glEnable(GL_SCISSOR_TEST);
    glScissor(w / 4, h / 4, w / 2, h / 2);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

/* a mesh with colours, normals and texture coordinates: the same picture
 * whether drawn in immediate mode or from vertex arrays (exact check) */
#define MESH_N 9
static float mesh_v[MESH_N * MESH_N][3], mesh_n[MESH_N * MESH_N][3], mesh_t[MESH_N * MESH_N][2];
static unsigned char mesh_c[MESH_N * MESH_N][4];
static unsigned short mesh_i[(MESH_N - 1) * (MESH_N - 1) * 6];
static void mesh_build(void) {
    for (int j = 0; j < MESH_N; j++)
        for (int i = 0; i < MESH_N; i++) {
            int k = j * MESH_N + i;
            float x = -1 + 2.0f * i / (MESH_N - 1), y = -1 + 2.0f * j / (MESH_N - 1);
            float z = 0.3f * sinf(x * 3) * cosf(y * 2);
            mesh_v[k][0] = x; mesh_v[k][1] = y; mesh_v[k][2] = z;
            float nx = -0.9f * cosf(x * 3) * cosf(y * 2), ny = 0.6f * sinf(x * 3) * sinf(y * 2), l = sqrtf(nx * nx + ny * ny + 1);
            mesh_n[k][0] = nx / l; mesh_n[k][1] = ny / l; mesh_n[k][2] = 1 / l;
            mesh_t[k][0] = (float)i / (MESH_N - 1) * 2; mesh_t[k][1] = (float)j / (MESH_N - 1) * 2;
            mesh_c[k][0] = (unsigned char)(128 + 127 * i / (MESH_N - 1)); mesh_c[k][1] = (unsigned char)(255 - 20 * j);
            mesh_c[k][2] = (unsigned char)(80 + 20 * i); mesh_c[k][3] = 255;
        }
    int n = 0;
    for (int j = 0; j < MESH_N - 1; j++)
        for (int i = 0; i < MESH_N - 1; i++) {
            unsigned short a = (unsigned short)(j * MESH_N + i), b = (unsigned short)(a + 1), c = (unsigned short)(a + MESH_N), d = (unsigned short)(c + 1);
            mesh_i[n++] = a; mesh_i[n++] = b; mesh_i[n++] = d;
            mesh_i[n++] = a; mesh_i[n++] = d; mesh_i[n++] = c;
        }
}
static void mesh_setup(int w, int h) {
    mesh_build();
    persp(50, w, h, 1, 20);
    clear_to(0.1f, 0.1f, 0.15f);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glEnable(GL_TEXTURE_2D);
    tex_checker(8, 2, C_WHITE, C_YELLOW, GL_LINEAR, GL_LINEAR);
    glTranslatef(0, 0, -3.2f);
    glRotatef(-35, 1, 0, 0);
    glRotatef(15, 0, 0, 1);
}
/* mode 0 immediate, 1 glDrawElements, 2 glDrawArrays (expanded), 3 glArrayElement,
 * 4 glInterleavedArrays + glDrawRangeElements, 5 a display list */
static void mesh_draw(int mode) {
    int ni = (MESH_N - 1) * (MESH_N - 1) * 6;
    if (mode == 0 || mode == 5) {
        GLuint list = 0;
        if (mode == 5) { list = glGenLists(1); glNewList(list, GL_COMPILE); }
        glBegin(GL_TRIANGLES);
        for (int i = 0; i < ni; i++) {
            int k = mesh_i[i];
            glColor4ubv(mesh_c[k]);
            glNormal3fv(mesh_n[k]);
            glTexCoord2fv(mesh_t[k]);
            glVertex3fv(mesh_v[k]);
        }
        glEnd();
        if (mode == 5) { glEndList(); glCallList(list); glDeleteLists(list, 1); }
        return;
    }
    if (mode == 4) {
        static float inter[MESH_N * MESH_N][8];   /* GL_T2F_N3F_V3F; the colour stays current */
        for (int k = 0; k < MESH_N * MESH_N; k++) {
            memcpy(inter[k], mesh_t[k], 8); memcpy(inter[k] + 2, mesh_n[k], 12); memcpy(inter[k] + 5, mesh_v[k], 12);
        }
        glInterleavedArrays(GL_T2F_N3F_V3F, 0, inter);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, mesh_c);
        glEnableClientState(GL_COLOR_ARRAY);
        glDrawRangeElements(GL_TRIANGLES, 0, MESH_N * MESH_N - 1, ni, GL_UNSIGNED_SHORT, mesh_i);
    } else {
        glVertexPointer(3, GL_FLOAT, 0, mesh_v);
        glNormalPointer(GL_FLOAT, 0, mesh_n);
        glTexCoordPointer(2, GL_FLOAT, 0, mesh_t);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, mesh_c);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        if (mode == 1) glDrawElements(GL_TRIANGLES, ni, GL_UNSIGNED_SHORT, mesh_i);
        else if (mode == 2) {
            static float ev[sizeof mesh_i / 2][3], en[sizeof mesh_i / 2][3], et[sizeof mesh_i / 2][2];
            static unsigned char ec[sizeof mesh_i / 2][4];
            for (int i = 0; i < ni; i++) {
                int k = mesh_i[i];
                memcpy(ev[i], mesh_v[k], 12); memcpy(en[i], mesh_n[k], 12); memcpy(et[i], mesh_t[k], 8); memcpy(ec[i], mesh_c[k], 4);
            }
            glVertexPointer(3, GL_FLOAT, 0, ev);
            glNormalPointer(GL_FLOAT, 0, en);
            glTexCoordPointer(2, GL_FLOAT, 0, et);
            glColorPointer(4, GL_UNSIGNED_BYTE, 0, ec);
            glDrawArrays(GL_TRIANGLES, 0, ni);
        } else {
            glBegin(GL_TRIANGLES);
            for (int i = 0; i < ni; i++) glArrayElement(mesh_i[i]);
            glEnd();
        }
    }
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
}
static void s_mesh(int w, int h) { mesh_setup(w, h); mesh_draw(0); }

typedef struct scene { const char *name; int w, h; void (*draw)(int w, int h); const char *what; } scene_t;
static const scene_t scenes[] = {
    { "gouraud", 64, 64, s_gouraud, "a smooth-shaded triangle (red, green, blue corners)" },
    { "flat", 64, 64, s_flat, "flat shading: quads and a triangle take their last vertex's colour" },
    { "depth", 64, 64, s_depth, "depth test: a red quad through a green one, a blue triangle behind both" },
    { "depth_funcs", 64, 64, s_depth_funcs, "GL_GREATER, GL_EQUAL, depth mask off" },
    { "tex_mag_nearest", 64, 64, s_tex_mag_nearest, "a 4 x 4 texture magnified, GL_NEAREST" },
    { "tex_mag_linear", 64, 64, s_tex_mag_linear, "a 4 x 4 texture magnified, GL_LINEAR" },
    { "tex_nearest", 64, 64, s_tex_nearest, "a floor, minified GL_NEAREST (level 0 only: red checks)" },
    { "tex_linear", 64, 64, s_tex_linear, "a floor, minified GL_LINEAR (level 0 only)" },
    { "tex_nearest_mipmap_nearest", 64, 64, s_tex_nearest_mipmap_nearest, "colour-coded levels, hard bands" },
    { "tex_linear_mipmap_nearest", 64, 64, s_tex_linear_mipmap_nearest, "colour-coded levels, hard bands, bilinear" },
    { "tex_nearest_mipmap_linear", 64, 64, s_tex_nearest_mipmap_linear, "colour-coded levels blended" },
    { "tex_linear_mipmap_linear", 64, 64, s_tex_linear_mipmap_linear, "colour-coded levels blended (trilinear)" },
    { "tex_glu_mipmap", 64, 64, s_tex_glu_mipmap, "gluBuild2DMipmaps of a 48 x 40 image, trilinear" },
    { "tex_wrap", 64, 64, s_tex_wrap, "clamp (left) and repeat (right)" },
    { "tex_env", 80, 80, s_tex_env, "texture formats (rows) by environment modes (columns)" },
    { "persp", 64, 64, s_persp, "perspective-correct texturing of a steep quad" },
    { "light_dir", 64, 64, s_light_dir, "a directional light: diffuse and specular on a red sphere" },
    { "light_point", 64, 64, s_light_point, "a point light with attenuation above a floor" },
    { "light_spot", 64, 64, s_light_spot, "a spot light on a wall" },
    { "light_twoside", 64, 64, s_light_twoside, "two-sided lighting, colour material" },
    { "light_specular", 64, 64, s_light_specular, "separate specular colour on a textured sphere" },
    { "fog_linear", 64, 64, s_fog_linear, "linear fog" },
    { "fog_exp", 64, 64, s_fog_exp, "exponential fog" },
    { "fog_exp2", 64, 64, s_fog_exp2, "squared exponential fog" },
    { "blend", 64, 64, s_blend, "alpha blending and additive blending" },
    { "alpha_test", 64, 64, s_alpha_test, "the alpha test: cut-out discs, no depth where cut" },
    { "clip_near", 64, 64, s_clip_near, "clipping at the near plane" },
    { "clip_user", 64, 64, s_clip_user, "two user clip planes" },
    { "cull", 64, 64, s_cull, "culling of back and front faces, both windings" },
    { "multitex", 64, 64, s_multitex, "two texture units: modulate and add" },
    { "viewport_scissor", 64, 64, s_viewport_scissor, "four viewports, scissored drawing and clears" },
    { "lines_points", 64, 64, s_lines_points, "lines, strips, loops, wide lines and big points" },
    { "polymode", 64, 64, s_polymode, "polygon modes and polygon offset" },
    { "texgen", 64, 64, s_texgen, "object-linear and sphere-map texgen, the texture matrix" },
    { "colormask", 64, 64, s_colormask, "colour masks on drawing and a scissored clear" },
    { "mesh", 64, 64, s_mesh, "a lit, textured, coloured mesh (also from vertex arrays, exactly)" },
};
#define NSCENES ((int)(sizeof scenes / sizeof scenes[0]))

#endif
