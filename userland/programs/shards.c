/* SHARDS.EXE - Shards: a small 3D game drawn by Halcyon's 3D library.
 *
 * Steer a hover sled over a platform floating in the evening sky and pick
 * up the glowing crystal shards before the time runs out; stone pillars
 * stand in the way. Each level has more shards and pillars and a little
 * less time per shard. Everything is made by the program: the platform's
 * tiles, the stone and the sled are procedural, the shards octahedra.
 *
 * Keys: arrows (or W A S D) steer, accelerate and brake; Enter starts;
 * P or Esc pauses; F2 a new game; F shows the frame rate; F1 help.
 *   SHARDS.EXE /demo      the sled steers itself (no time limit) and every
 *                         25th frame is logged with the frame rate (tests)
 *
 * The 3D frame is drawn straight into the window's canvas (hgl.h): no copy;
 * the text on top is drawn by the window manager's text calls into the
 * same canvas, then the window is told the frame is ready. */
#include "hal.h"
#include "GL/gl.h"
#include "GL/glu.h"
#include "GL/hgl.h"

#define ICON_GAME 34                        /* the kernel's ICON_GAME (build/gen/icons.h) */
#define KEY_F1_ 0x3A
#define KEY_F2_ 0x3B
#define ARENA 30.0f                         /* the platform: -ARENA..ARENA in x and z */
#define MAX_SHARDS 40
#define MAX_PILLARS 24
#define SLED_R 0.9f

enum { S_TITLE, S_PLAY, S_PAUSED, S_CLEAR, S_OVER };

typedef struct { float x, z, y, phase; int taken; float spin; unsigned color; } shard_t;
typedef struct { float x, z, r, h; } pillar_t;

static int win;
static HGLRC rc;
static int state = S_TITLE, demo, show_fps, level, score, best, collected;
static float time_left, msg_t;
static float px, pz, heading, vx, vz;       /* the sled */
static float cam[3], cam_look[3];
static int keys[8];                         /* left right up down */
static shard_t shards[MAX_SHARDS];
static int nshards;
static pillar_t pillars[MAX_PILLARS];
static int npillars;
static GLuint tex_tiles, tex_stone, tex_glow;
static GLUquadric *quad;
static unsigned rng = 1;
static float clock_s;
static int cw, ch;

static unsigned rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static float frand(void) { return (float)(rnd() & 0xFFFF) / 65535.0f; }

/* ---------------- textures ---------------- */
static void make_textures(void) {
    static unsigned char img[128 * 128 * 3];
    /* platform tiles: bevelled slabs, slightly different shades */
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) {
            int tx = x & 31, ty = y & 31, tile = (x >> 5) + (y >> 5) * 4;
            int edge = tx < 2 || ty < 2 || tx > 29 || ty > 29;
            int bev = (tx < 4 || ty < 4) ? 18 : (tx > 27 || ty > 27) ? -18 : 0;
            int n = (int)(rnd() % 14) - 7, shade = ((tile * 37) % 5) * 6;
            int v = edge ? 70 : 168 + bev + n + shade;
            unsigned char *p = &img[(y * 128 + x) * 3];
            p[0] = (unsigned char)(v * 0.86f); p[1] = (unsigned char)(v * 0.90f); p[2] = (unsigned char)v;
        }
    glGenTextures(1, &tex_tiles);
    glBindTexture(GL_TEXTURE_2D, tex_tiles);
    gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, 128, 128, GL_RGB, GL_UNSIGNED_BYTE, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    /* stone: layered bands with noise */
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float band = sinf(y * 0.55f + sinf(x * 0.2f) * 1.5f);
            int v = 120 + (int)(band * 22) + (int)(rnd() % 30) - 15;
            unsigned char *p = &img[(y * 64 + x) * 3];
            p[0] = (unsigned char)(v + 12); p[1] = (unsigned char)v; p[2] = (unsigned char)(v - 14);
        }
    glGenTextures(1, &tex_stone);
    glBindTexture(GL_TEXTURE_2D, tex_stone);
    gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, 64, 64, GL_RGB, GL_UNSIGNED_BYTE, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    /* glow: a soft round spot (intensity) */
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            float dx = (x - 15.5f) / 16, dy = (y - 15.5f) / 16, d = sqrtf(dx * dx + dy * dy);
            float a = d >= 1 ? 0 : (1 - d) * (1 - d);
            img[y * 32 + x] = (unsigned char)(a * 255);
        }
    glGenTextures(1, &tex_glow);
    glBindTexture(GL_TEXTURE_2D, tex_glow);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gluBuild2DMipmaps(GL_TEXTURE_2D, GL_INTENSITY, 32, 32, GL_LUMINANCE, GL_UNSIGNED_BYTE, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* ---------------- the game ---------------- */
static int free_spot(float x, float z, float r) {
    if (fabsf(x) > ARENA - r - 1 || fabsf(z) > ARENA - r - 1) return 0;
    if (x * x + z * z < (r + 4) * (r + 4)) return 0;          /* the start stays clear */
    for (int i = 0; i < npillars; i++) {
        float dx = x - pillars[i].x, dz = z - pillars[i].z, rr = r + pillars[i].r + 1.5f;
        if (dx * dx + dz * dz < rr * rr) return 0;
    }
    for (int i = 0; i < nshards; i++) {
        float dx = x - shards[i].x, dz = z - shards[i].z;
        if (dx * dx + dz * dz < (r + 2) * (r + 2)) return 0;
    }
    return 1;
}

static void start_level(int lv) {
    static const unsigned colors[6] = { 0x40E0FF, 0xFF60D0, 0x80FF60, 0xFFD040, 0xA080FF, 0xFF8040 };
    level = lv;
    npillars = nshards = 0;
    int np = 3 + 2 * lv, ns = 6 + 2 * lv;
    if (np > MAX_PILLARS) np = MAX_PILLARS;
    if (ns > MAX_SHARDS) ns = MAX_SHARDS;
    for (int tries = 0; npillars < np && tries < 2000; tries++) {
        float r = 0.8f + frand() * 1.0f, x = (frand() * 2 - 1) * ARENA, z = (frand() * 2 - 1) * ARENA;
        if (!free_spot(x, z, r)) continue;
        pillars[npillars].x = x; pillars[npillars].z = z; pillars[npillars].r = r; pillars[npillars].h = 3 + frand() * 5;
        npillars++;
    }
    for (int tries = 0; nshards < ns && tries < 4000; tries++) {
        float x = (frand() * 2 - 1) * ARENA, z = (frand() * 2 - 1) * ARENA;
        if (!free_spot(x, z, 0.8f)) continue;
        shard_t *s = &shards[nshards++];
        s->x = x; s->z = z; s->y = 1.2f; s->phase = frand() * 6.28f; s->taken = 0; s->spin = 0;
        s->color = colors[nshards % 6];
    }
    collected = 0;
    time_left = 20.0f + nshards * (4.5f - (lv > 6 ? 1.5f : lv * 0.25f));
    px = pz = 0;
    heading = 0;
    vx = vz = 0;
}

static void new_game(void) {
    score = 0;
    start_level(1);
    state = S_PLAY;
}

static void push_out(float cx, float cz, float r) {
    float dx = px - cx, dz = pz - cz, d = sqrtf(dx * dx + dz * dz), min = r + SLED_R;
    if (d >= min || d == 0) return;
    float nx = dx / d, nz = dz / d;
    px = cx + nx * min;
    pz = cz + nz * min;
    float vn = vx * nx + vz * nz;
    if (vn < 0) { vx -= 1.6f * vn * nx; vz -= 1.6f * vn * nz; vx *= 0.7f; vz *= 0.7f; }
}

/* the autopilot of /demo: towards the nearest shard, around pillars */
static void autopilot(int *l, int *r, int *up, int *down) {
    int best_i = -1;
    float bd = 1e9f;
    for (int i = 0; i < nshards; i++) {
        if (shards[i].taken) continue;
        float dx = shards[i].x - px, dz = shards[i].z - pz, d = dx * dx + dz * dz;
        if (d < bd) { bd = d; best_i = i; }
    }
    *l = *r = *up = *down = 0;
    if (best_i < 0) return;
    float tx = shards[best_i].x - px, tz = shards[best_i].z - pz;
    /* steer away from a pillar close ahead */
    float fx = -sinf(heading), fz = -cosf(heading);
    for (int i = 0; i < npillars; i++) {
        float dx = pillars[i].x - px, dz = pillars[i].z - pz, ahead = dx * fx + dz * fz;
        float side = dx * -fz + dz * fx;
        if (ahead > 0 && ahead < 6 && fabsf(side) < pillars[i].r + 1.5f) { tx += side > 0 ? fz * 6 : -fz * 6; tz += side > 0 ? -fx * 6 : fx * 6; }
    }
    float want = atan2f(-tx, -tz), diff = want - heading;
    while (diff > (float)M_PI) diff -= 2 * (float)M_PI;
    while (diff < -(float)M_PI) diff += 2 * (float)M_PI;
    if (diff > 0.08f) *l = 1;
    if (diff < -0.08f) *r = 1;
    if (fabsf(diff) < 0.9f) *up = 1;
    else *down = sqrtf(vx * vx + vz * vz) > 6;
}

static void update(float dt) {
    clock_s += dt;
    if (state == S_CLEAR || state == S_OVER || state == S_TITLE) msg_t += dt;
    if (state == S_CLEAR && msg_t > 2.5f) { start_level(level + 1); state = S_PLAY; }
    if (state != S_PLAY) return;
    int l = keys[0], r = keys[1], up = keys[2], down = keys[3];
    if (demo) autopilot(&l, &r, &up, &down);
    if (l) heading += 2.6f * dt;
    if (r) heading -= 2.6f * dt;
    float fx = -sinf(heading), fz = -cosf(heading);
    float acc = up ? 24.0f : down ? -16.0f : 0;
    vx += fx * acc * dt;
    vz += fz * acc * dt;
    /* the sled grips sideways and glides forwards */
    float fwd = vx * fx + vz * fz, side = vx * -fz + vz * fx;
    fwd *= powf(0.55f, dt);
    side *= powf(0.02f, dt);
    if (fwd > 20) fwd = 20;
    if (fwd < -8) fwd = -8;
    vx = fx * fwd - fz * side;
    vz = fz * fwd + fx * side;
    px += vx * dt;
    pz += vz * dt;
    for (int i = 0; i < npillars; i++) push_out(pillars[i].x, pillars[i].z, pillars[i].r);
    float lim = ARENA - SLED_R - 0.4f;
    if (px > lim) { px = lim; if (vx > 0) vx = -vx * 0.6f; }
    if (px < -lim) { px = -lim; if (vx < 0) vx = -vx * 0.6f; }
    if (pz > lim) { pz = lim; if (vz > 0) vz = -vz * 0.6f; }
    if (pz < -lim) { pz = -lim; if (vz < 0) vz = -vz * 0.6f; }
    for (int i = 0; i < nshards; i++) {
        shard_t *s = &shards[i];
        if (s->taken) continue;
        float dx = s->x - px, dz = s->z - pz;
        if (dx * dx + dz * dz < 2.2f * 2.2f) {
            s->taken = 1;
            collected++;
            score += 100 * level;
        }
    }
    if (!demo) time_left -= dt;
    if (collected == nshards) {
        score += (int)time_left * 10;
        state = S_CLEAR;
        msg_t = 0;
    } else if (time_left <= 0) {
        time_left = 0;
        state = S_OVER;
        msg_t = 0;
        if (score > best) { best = score; setting_set_int("shards.best", best); }
    }
}

/* ---------------- drawing ---------------- */
static void color3(unsigned c, float k) { glColor3f(((c >> 16) & 255) / 255.0f * k, ((c >> 8) & 255) / 255.0f * k, (c & 255) / 255.0f * k); }

static void sky(void) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 0, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glBegin(GL_QUAD_STRIP);
    glColor3f(0.36f, 0.52f, 0.62f); glVertex2f(0, 0); glVertex2f(1, 0);
    glColor3f(0.48f, 0.60f, 0.66f); glVertex2f(0, 0.5f); glVertex2f(1, 0.5f);
    glColor3f(0.20f, 0.24f, 0.42f); glVertex2f(0, 0.8f); glVertex2f(1, 0.8f);
    glColor3f(0.07f, 0.06f, 0.18f); glVertex2f(0, 1); glVertex2f(1, 1);
    glEnd();
    glEnable(GL_DEPTH_TEST);
}

static void box(float x0, float y0, float z0, float x1, float y1, float z1) {
    glBegin(GL_QUADS);
    glNormal3f(0, 1, 0); glVertex3f(x0, y1, z1); glVertex3f(x1, y1, z1); glVertex3f(x1, y1, z0); glVertex3f(x0, y1, z0);
    glNormal3f(0, 0, 1); glVertex3f(x0, y0, z1); glVertex3f(x1, y0, z1); glVertex3f(x1, y1, z1); glVertex3f(x0, y1, z1);
    glNormal3f(0, 0, -1); glVertex3f(x1, y0, z0); glVertex3f(x0, y0, z0); glVertex3f(x0, y1, z0); glVertex3f(x1, y1, z0);
    glNormal3f(1, 0, 0); glVertex3f(x1, y0, z1); glVertex3f(x1, y0, z0); glVertex3f(x1, y1, z0); glVertex3f(x1, y1, z1);
    glNormal3f(-1, 0, 0); glVertex3f(x0, y0, z0); glVertex3f(x0, y0, z1); glVertex3f(x0, y1, z1); glVertex3f(x0, y1, z0);
    glNormal3f(0, -1, 0); glVertex3f(x0, y0, z0); glVertex3f(x1, y0, z0); glVertex3f(x1, y0, z1); glVertex3f(x0, y0, z1);
    glEnd();
}

static void platform(void) {
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex_tiles);
    glColor3f(1, 1, 1);
    glNormal3f(0, 1, 0);
    const int n = 20;
    float step = 2 * ARENA / n;
    for (int j = 0; j < n; j++) {
        float z0 = -ARENA + step * j, z1 = z0 + step;
        glBegin(GL_QUAD_STRIP);
        for (int i = 0; i <= n; i++) {
            float x = -ARENA + step * i;
            glTexCoord2f(x / 6, z0 / 6); glVertex3f(x, 0, z0);   /* (counter-clockwise from above: z0 < z1) */
            glTexCoord2f(x / 6, z1 / 6); glVertex3f(x, 0, z1);
        }
        glEnd();
    }
    /* the sides of the slab, in stone */
    glBindTexture(GL_TEXTURE_2D, tex_stone);
    glColor3f(0.75f, 0.7f, 0.75f);
    glBegin(GL_QUADS);
    for (int s = 0; s < 4; s++) {
        float a = ARENA, d = 3;
        float c[4][2] = { { -a, a }, { a, a }, { a, -a }, { -a, -a } };
        float *p = c[s], *q = c[(s + 1) & 3];
        float nx = (p[0] + q[0]) / (2 * a), nz = (p[1] + q[1]) / (2 * a);
        glNormal3f(nx, 0, nz);
        glTexCoord2f(0, 0); glVertex3f(p[0], -d, p[1]);
        glTexCoord2f(10, 0); glVertex3f(q[0], -d, q[1]);
        glTexCoord2f(10, 1); glVertex3f(q[0], 0, q[1]);
        glTexCoord2f(0, 1); glVertex3f(p[0], 0, p[1]);
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
    /* a glowing rim */
    glDisable(GL_LIGHTING);
    float g = 0.6f + 0.25f * sinf(clock_s * 2);
    glColor3f(0.3f * g, 0.9f * g, 1.0f * g);
    float a = ARENA, t = 0.25f, hgt = 0.35f;
    glEnable(GL_LIGHTING);
    float em[4] = { 0.25f * g, 0.75f * g, 0.9f * g, 1 }, none[4] = { 0, 0, 0, 1 };
    glMaterialfv(GL_FRONT, GL_EMISSION, em);
    box(-a, 0, -a, a, hgt, -a + t);
    box(-a, 0, a - t, a, hgt, a);
    box(-a, 0, -a, -a + t, hgt, a);
    box(a - t, 0, -a, a, hgt, a);
    glMaterialfv(GL_FRONT, GL_EMISSION, none);
}

static void pillars_draw(void) {
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex_stone);
    gluQuadricTexture(quad, GL_TRUE);
    glColor3f(0.85f, 0.8f, 0.78f);
    for (int i = 0; i < npillars; i++) {
        pillar_t *p = &pillars[i];
        glPushMatrix();
        glTranslatef(p->x, 0, p->z);
        glRotatef(-90, 1, 0, 0);
        gluCylinder(quad, p->r, p->r * 0.85f, p->h, 14, 1);
        glTranslatef(0, 0, p->h);
        gluDisk(quad, 0, p->r * 0.85f, 14, 1);
        glPopMatrix();
    }
    glDisable(GL_TEXTURE_2D);
}

/* a soft dark spot under something hovering (blended, offset towards the eye) */
static void shadow(float x, float z, float r, float a) {
    glColor4f(0, 0, 0.05f, a);
    glBegin(GL_TRIANGLE_FAN);
    glVertex3f(x, 0.02f, z);
    glColor4f(0, 0, 0.05f, 0);
    for (int k = 12; k >= 0; k--) {
        float an = k * (2 * (float)M_PI / 12);
        glVertex3f(x + r * cosf(an), 0.02f, z + r * sinf(an));
    }
    glEnd();
}

static void octahedron(void) {
    static const float v[6][3] = { { 0, 1, 0 }, { 0, -1, 0 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    static const int f[8][3] = { { 0, 4, 2 }, { 0, 2, 5 }, { 0, 5, 3 }, { 0, 3, 4 }, { 1, 2, 4 }, { 1, 5, 2 }, { 1, 3, 5 }, { 1, 4, 3 } };
    glBegin(GL_TRIANGLES);
    for (int i = 0; i < 8; i++) {
        const float *a = v[f[i][0]], *b = v[f[i][1]], *c = v[f[i][2]];
        glNormal3f((a[0] + b[0] + c[0]) / 1.732f, (a[1] + b[1] + c[1]) / 1.732f, (a[2] + b[2] + c[2]) / 1.732f);
        glVertex3fv(a); glVertex3fv(b); glVertex3fv(c);
    }
    glEnd();
}

static void shards_draw(void) {
    float spec[4] = { 1, 1, 1, 1 };
    glMaterialfv(GL_FRONT, GL_SPECULAR, spec);
    glMaterialf(GL_FRONT, GL_SHININESS, 50);
    for (int i = 0; i < nshards; i++) {
        shard_t *s = &shards[i];
        if (s->taken) continue;
        float bob = 0.3f * sinf(clock_s * 2.2f + s->phase);
        float pulse = 0.35f + 0.25f * sinf(clock_s * 4 + s->phase);
        unsigned c = s->color;
        float em[4] = { ((c >> 16) & 255) / 255.0f * pulse, ((c >> 8) & 255) / 255.0f * pulse, (c & 255) / 255.0f * pulse, 1 };
        glMaterialfv(GL_FRONT, GL_EMISSION, em);
        color3(c, 0.8f);
        glPushMatrix();
        glTranslatef(s->x, s->y + bob, s->z);
        glRotatef(clock_s * 90 + s->phase * 57, 0, 1, 0);
        glScalef(0.55f, 0.95f, 0.55f);
        octahedron();
        glPopMatrix();
    }
    float none[4] = { 0, 0, 0, 1 };
    glMaterialfv(GL_FRONT, GL_EMISSION, none);
    glMaterialfv(GL_FRONT, GL_SPECULAR, none);
}

static void sled_draw(void) {
    glPushMatrix();
    glTranslatef(px, 0.45f + 0.06f * sinf(clock_s * 6), pz);
    glRotatef(heading * (180 / (float)M_PI), 0, 1, 0);
    /* lean into turns */
    float side = vx * cosf(heading) - vz * sinf(heading);
    glRotatef(-side * 2.5f, 0, 0, 1);
    float spec[4] = { 0.8f, 0.8f, 0.8f, 1 };
    glMaterialfv(GL_FRONT, GL_SPECULAR, spec);
    glMaterialf(GL_FRONT, GL_SHININESS, 30);
    glColor3f(0.92f, 0.32f, 0.18f);
    /* the hull: a wedge, its nose at -z */
    glBegin(GL_TRIANGLES);
    const float nose[3] = { 0, 0.15f, -1.4f }, bl[3] = { -0.75f, 0, 0.9f }, br[3] = { 0.75f, 0, 0.9f }, top[3] = { 0, 0.45f, 0.6f };
    const float bl2[3] = { -0.75f, 0.25f, 0.9f }, br2[3] = { 0.75f, 0.25f, 0.9f };
    glNormal3f(-0.55f, 0.75f, -0.35f); glVertex3fv(nose); glVertex3fv(bl2); glVertex3fv(top);
    glNormal3f(0.55f, 0.75f, -0.35f); glVertex3fv(nose); glVertex3fv(top); glVertex3fv(br2);
    glNormal3f(-0.9f, -0.2f, -0.35f); glVertex3fv(nose); glVertex3fv(bl); glVertex3fv(bl2);
    glNormal3f(0.9f, -0.2f, -0.35f); glVertex3fv(nose); glVertex3fv(br2); glVertex3fv(br);
    glNormal3f(0, -1, 0); glVertex3fv(nose); glVertex3fv(br); glVertex3fv(bl);
    glNormal3f(0, 0.5f, 0.9f); glVertex3fv(bl2); glVertex3fv(br2); glVertex3fv(top);
    glNormal3f(0, 0, 1); glVertex3fv(bl); glVertex3fv(br); glVertex3fv(br2);
    glNormal3f(0, 0, 1); glVertex3fv(bl); glVertex3fv(br2); glVertex3fv(bl2);
    glEnd();
    /* the canopy and the fins */
    glColor3f(0.15f, 0.2f, 0.3f);
    box(-0.22f, 0.2f, -0.45f, 0.22f, 0.5f, 0.2f);
    glColor3f(0.9f, 0.85f, 0.75f);
    box(-0.95f, 0.05f, 0.4f, -0.75f, 0.35f, 0.95f);
    box(0.75f, 0.05f, 0.4f, 0.95f, 0.35f, 0.95f);
    float none[4] = { 0, 0, 0, 1 };
    glMaterialfv(GL_FRONT, GL_SPECULAR, none);
    glPopMatrix();
}

/* additive glows: under the shards and behind the sled's engines */
static void glows(void) {
    float m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    float rx = m[0], ry = m[4], rz = m[8], ux = m[1], uy = m[5], uz = m[9];
    glDisable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex_glow);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);
    glBegin(GL_QUADS);
    for (int i = 0; i <= nshards; i++) {
        float x, y, z, s;
        unsigned c;
        if (i == nshards) {                 /* the engine glow */
            float speed = sqrtf(vx * vx + vz * vz);
            x = px + sinf(heading) * 1.1f; z = pz + cosf(heading) * 1.1f; y = 0.6f;
            s = 0.5f + speed * 0.03f;
            c = 0xFF9040;
        } else {
            shard_t *sh = &shards[i];
            if (sh->taken) continue;
            x = sh->x; z = sh->z; y = sh->y + 0.3f * sinf(clock_s * 2.2f + sh->phase);
            s = 1.6f;
            c = sh->color;
        }
        glColor4f(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, 0.7f);
        glTexCoord2f(0, 0); glVertex3f(x - (rx + ux) * s, y - (ry + uy) * s, z - (rz + uz) * s);
        glTexCoord2f(1, 0); glVertex3f(x + (rx - ux) * s, y + (ry - uy) * s, z + (rz - uz) * s);
        glTexCoord2f(1, 1); glVertex3f(x + (rx + ux) * s, y + (ry + uy) * s, z + (rz + uz) * s);
        glTexCoord2f(0, 1); glVertex3f(x - (rx - ux) * s, y - (ry - uy) * s, z - (rz - uz) * s);
    }
    glEnd();
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);
}

static void render(void) {
    glViewport(0, 0, cw, ch);
    glDisable(GL_FOG);
    glDisable(GL_LIGHTING);
    sky();
    glClear(GL_DEPTH_BUFFER_BIT);
    /* the camera: behind the sled (circling the platform on the title screen) */
    float want[3], look[3];
    if (state == S_TITLE) {
        float a = clock_s * 0.15f;
        want[0] = sinf(a) * 34; want[1] = 14; want[2] = cosf(a) * 34;
        look[0] = 0; look[1] = 0; look[2] = 0;
    } else {
        float fx = -sinf(heading), fz = -cosf(heading);
        want[0] = px - fx * 7.5f; want[1] = 4.2f; want[2] = pz - fz * 7.5f;
        look[0] = px + fx * 4; look[1] = 0.8f; look[2] = pz + fz * 4;
    }
    float k = state == S_TITLE ? 1 : 0.18f;
    for (int i = 0; i < 3; i++) { cam[i] += (want[i] - cam[i]) * k; cam_look[i] += (look[i] - cam_look[i]) * k; }
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(62, (double)cw / (ch ? ch : 1), 0.3, 140);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(cam[0], cam[1], cam[2], cam_look[0], cam_look[1], cam_look[2], 0, 1, 0);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_FOG);
    float fog[4] = { 0.40f, 0.55f, 0.64f, 1 };
    glFogfv(GL_FOG_COLOR, fog);
    glFogi(GL_FOG_MODE, GL_EXP2);
    glFogf(GL_FOG_DENSITY, 0.016f);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHT1);
    glEnable(GL_COLOR_MATERIAL);
    glEnable(GL_NORMALIZE);
    float sun[4] = { -0.5f, 0.8f, 0.35f, 0 }, sunc[4] = { 1, 0.92f, 0.8f, 1 };
    float fill[4] = { 0.6f, 0.3f, -0.7f, 0 }, fillc[4] = { 0.25f, 0.3f, 0.45f, 1 }, amb[4] = { 0.28f, 0.28f, 0.34f, 1 };
    glLightfv(GL_LIGHT0, GL_POSITION, sun);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, sunc);
    glLightfv(GL_LIGHT0, GL_SPECULAR, sunc);
    glLightfv(GL_LIGHT1, GL_POSITION, fill);
    glLightfv(GL_LIGHT1, GL_DIFFUSE, fillc);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    platform();
    pillars_draw();
    shards_draw();
    if (state != S_TITLE) sled_draw();
    /* shadows on the platform */
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1, -2);
    for (int i = 0; i < nshards; i++) if (!shards[i].taken) shadow(shards[i].x, shards[i].z, 0.9f, 0.35f);
    if (state != S_TITLE) shadow(px, pz, 1.5f, 0.5f);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glows();
    glDisable(GL_FOG);
}

/* text with a shadow, centred if x < 0 */
static void text(int x, int y, const char *s, unsigned color, int font) {
    if (x < 0) x = (cw - gfx_text_width(s, font)) / 2;
    gfx_text(win, x + 1, y + 1, s, 0x101018, font);
    gfx_text(win, x, y, s, color, font);
}

static void hud(void) {
    char b[160];
    if (state == S_TITLE) {
        text(-1, ch / 2 - 60, "SHARDS", 0xFFE070, FONT_HUGE);
        text(-1, ch / 2, "Collect the glowing shards before the time runs out.", 0xFFFFFF, FONT_UI);
        text(-1, ch / 2 + 20, "Arrows or W A S D steer.  P pauses.  F shows the frame rate.", 0xE0E8FF, FONT_UI);
        text(-1, ch / 2 + 50, "Press Enter to start", 0xFFFFFF, FONT_UI_BOLD);
        if (best) { snprintf(b, sizeof b, "Best score: %d", best); text(-1, ch / 2 + 74, b, 0xC0D0FF, FONT_UI); }
    } else {
        int t = (int)ceilf(time_left);
        snprintf(b, sizeof b, "Level %d     Shards %d / %d     Time %d:%02d     Score %d%s", level, collected, nshards, t / 60, t % 60,
                 score, demo ? "     (demo)" : "");
        text(10, 8, b, time_left < 10 && !demo && ((int)(time_left * 4) & 1) ? 0xFF6060 : 0xFFFFFF, FONT_UI_BOLD);
        if (state == S_PAUSED) text(-1, ch / 2 - 10, "Paused - press P to go on", 0xFFFFFF, FONT_LARGE);
        if (state == S_CLEAR) { snprintf(b, sizeof b, "Level %d cleared!", level); text(-1, ch / 2 - 20, b, 0xFFE070, FONT_LARGE); }
        if (state == S_OVER) {
            text(-1, ch / 2 - 30, "Time is up", 0xFF8060, FONT_LARGE);
            snprintf(b, sizeof b, "Score %d   (best %d)   -   press Enter to play again", score, best);
            text(-1, ch / 2 + 4, b, 0xFFFFFF, FONT_UI_BOLD);
        }
    }
    if (show_fps) {
        float f = hglFramesPerSecond(rc);
        snprintf(b, sizeof b, "%d.%d frames/s  %dx%d", (int)f, (int)(f * 10) % 10, cw, ch);
        text(cw - gfx_text_width(b, FONT_UI_BOLD) - 10, ch - 24, b, 0x80FF80, FONT_UI_BOLD);
    }
}

static void key(const hal_event_t *ev) {
    int down = ev->pressed;
    int k = -1;
    switch (ev->id) {
    case KEY_LEFT: k = 0; break;
    case KEY_RIGHT: k = 1; break;
    case KEY_UP: k = 2; break;
    case KEY_DOWN: k = 3; break;
    }
    if (ev->vk == 'A') k = 0;
    if (ev->vk == 'D') k = 1;
    if (ev->vk == 'W') k = 2;
    if (ev->vk == 'S') k = 3;
    if (k >= 0) { keys[k] = down; return; }
    if (!down) return;
    if (ev->id == KEY_F1_) help_open("3d");
    else if (ev->id == KEY_F2_) new_game();
    else if (ev->vk == 'F') show_fps = !show_fps;
    else if (ev->id == KEY_ENTER || ev->id == KEY_KP_ENTER || ev->id == KEY_SPACE) {
        if (state == S_TITLE || state == S_OVER) new_game();
    } else if (ev->vk == 'P' || ev->id == KEY_ESC) {
        if (state == S_PLAY) state = S_PAUSED;
        else if (state == S_PAUSED) state = S_PLAY;
    }
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "/demo") || !strcmp(argv[i], "-demo") || !strcmp(argv[i], "--demo")) demo = 1;
    getrandom(&rng, sizeof rng);
    rng |= 1;
    win = win_create("Shards", 640, 480, WIN_RESIZABLE);
    if (win < 0) return 1;
    win_set_icon(win, ICON_GAME);
    win_set_min_size(win, 320, 240);
    rc = hglCreateContext(win, 0);
    if (!rc) { msgbox(win, "Shards", "The 3D library could not draw into the window.", MBX_ICON_ERROR, MBX_OK); return 1; }
    hglMakeCurrent(rc);
    win_size(win, &cw, &ch);
    best = setting_get_int("shards.best", 0);
    make_textures();
    quad = gluNewQuadric();
    start_level(1);
    if (demo) {
        state = S_PLAY;
        show_fps = 1;
        hal_rect_t wr;
        win_get_rect(win, &wr);
        printf("shards: demo, window client at %d,%d size %dx%d\n", wr.x, wr.y, wr.w, wr.h);
    }
    uint64_t last = time_ms(), log_t0 = last;
    unsigned frames = 0;
    for (;;) {
        hal_event_t ev;
        int r;
        /* the events that are there; while paused, wait for one */
        while ((r = win_get_event(&ev, state == S_PAUSED ? 250 : 0)) != 0) {
            if (r < 0) return 0;
            if (ev.type == EVT_CLOSE) return 0;
            if (ev.type == EVT_KEY) key(&ev);
            if (ev.type == EVT_FOCUS && !ev.pressed) memset(keys, 0, sizeof keys);
            if (ev.type == EVT_RESIZE) { hglResize(rc); win_size(win, &cw, &ch); }
            if (state == S_PAUSED && ev.type != EVT_KEY) break;
        }
        uint64_t now = time_ms();
        float dt = (float)(now - last) * 0.001f;
        last = now;
        if (dt > 0.1f) dt = 0.1f;               /* (after a stall: no huge jump) */
        update(dt);
        render();
        hglFinishFrame(rc);
        hud();
        win_invalidate_rect(win, 0, 0, cw, ch);
        frames++;
        if (demo && frames % 25 == 0) {
            unsigned ms = (unsigned)(now - log_t0);
            printf("shards: %u frames rendered (%dx%d), %u.%u frames/s, level %d, %d shards collected\n", frames, cw, ch,
                   ms ? 25000 / ms : 0, ms ? (250000 / ms) % 10 : 0, level, collected);
            log_t0 = now;
        }
        /* at most about 60 frames a second */
        uint64_t spent = time_ms() - now;
        if (spent < 16) {
            if (win_get_event(&ev, (int)(16 - spent)) > 0) {
                if (ev.type == EVT_CLOSE) return 0;
                if (ev.type == EVT_KEY) key(&ev);
                if (ev.type == EVT_RESIZE) { hglResize(rc); win_size(win, &cw, &ch); }
            }
        }
    }
}
