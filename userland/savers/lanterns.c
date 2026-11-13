/* LANTERNS.SCR - Lantern Valley: a 3D screen saver drawn by Halcyon's 3D
 * library (libgl). The view glides up a winding valley at dusk while paper
 * lanterns rise from the river and drift into the sky.
 *
 * Everything is made here, nothing is loaded: the valley is a height field
 * of value noise (repeating along the valley, so the flight never ends)
 * lit by the low sun, with a mipmapped grass texture made of noise; the
 * river is a translucent quad strip; the sky a gradient with stars; the
 * lanterns camera-facing quads with a soft glow texture, blended additively.
 * Fog in the horizon's colour hides the end of the land.
 *
 * The frame is drawn straight into the saver's pixels (a buffer context on
 * the window's canvas): no copy. The 10th, 20th, 50th and 100th frames
 * and every 1000th are logged with the frame rate (the boot test waits for
 * them). */
#define SAVER_MS 33
#include "saver.h"
#include "GL/gl.h"
#include "GL/glu.h"
#include "GL/hgl.h"

SAVER_NAME("Lantern Valley");

#define TX 48                       /* cells across the valley */
#define TZ 128                      /* cells along it (then it repeats) */
#define CELL 2.0f
#define PERIOD (TZ * CELL)
#define AHEAD 44                    /* rows drawn in front of the view */
#define NLANTERNS 56
#define NSTARS 90

static HGLRC rc;
static GLuint tex_grass, tex_glow;
static float H[TZ][TX + 1];         /* heights */
static float N[TZ][TX + 1][3];      /* normals */
static unsigned char COL[TZ][TX + 1][3];
static float t_sec;
static unsigned frames, log_frames;
static uint64_t log_t0;
static int built;

typedef struct { float x, y, z, vy, phase, size, hue; } lantern_t;
static lantern_t lan[NLANTERNS];
static float star[NSTARS][3];

/* ---------------- the land ---------------- */
static float frand(void) { return (float)(sv_rand() & 0xFFFF) / 65535.0f; }

/* value noise on a lattice that repeats every `per` cells along z */
static float lattice[TZ][64];             /* (z repeats every TZ cells at most) */
static float noise(float x, float z, int per) {
    float fx = floorf(x), fz = floorf(z);
    int ix = (int)fx, iz = (int)fz;
    float ux = x - fx, uz = z - fz;
    ux = ux * ux * (3 - 2 * ux);
    uz = uz * uz * (3 - 2 * uz);
    int x0 = ix & 63, x1 = (ix + 1) & 63, z0 = ((iz % per) + per) % per, z1 = (z0 + 1) % per;
    float a = lattice[z0][x0], b = lattice[z0][x1], c = lattice[z1][x0], d = lattice[z1][x1];
    return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uz;
}

static float valley_x(float z) { return 9.0f * sinf(z * (2 * (float)M_PI * 2 / PERIOD)) + 4.0f * sinf(z * (2 * (float)M_PI * 3 / PERIOD)); }

static float height_at(float x, float z) {
    float d = x - valley_x(z);
    float v = 0.03f * d * d;
    if (v > 16) v = 16 + (v - 16) * 0.25f;
    /* (z in cells, the lattice repeats every TZ / 4, TZ / 2, TZ cells) */
    float zc = -z / CELL, xc = x / CELL + 64;
    float n = noise(xc * 0.25f, zc * 0.25f, TZ / 4) * 2.4f + noise(xc * 0.5f, zc * 0.5f, TZ / 2) * 1.2f + noise(xc, zc, TZ) * 0.5f;
    /* rougher on the slopes, lower and smoother along the river */
    float k = 1.1f + 1.3f * fminf(1.0f, fabsf(d) / 10);
    return v + n * k - 2.8f;
}

static void build_land(void) {
    for (int z = 0; z < TZ; z++)
        for (int x = 0; x < 64; x++) lattice[z][x] = frand();
    for (int j = 0; j < TZ; j++)
        for (int i = 0; i <= TX; i++) H[j][i] = height_at(-TX + CELL * i, -CELL * j);
    for (int j = 0; j < TZ; j++)
        for (int i = 0; i <= TX; i++) {
            float hl = H[j][i > 0 ? i - 1 : i], hr = H[j][i < TX ? i + 1 : i];
            float hf = H[(j + 1) % TZ][i], hb = H[(j + TZ - 1) % TZ][i];
            float nx = hl - hr, nz = hf - hb, ny = 2 * CELL;   /* (-dH/dx, 1, -dH/dz) * 2 CELL; z falls as j grows */
            float l = sqrtf(nx * nx + ny * ny + nz * nz);
            N[j][i][0] = nx / l; N[j][i][1] = ny / l; N[j][i][2] = nz / l;
            float h = H[j][i], steep = 1 - N[j][i][1];
            float g = h < 0.6f ? 0.0f : 1.0f;                  /* sand by the river */
            float rock = fminf(1.0f, steep * 4 + (h > 14 ? (h - 14) * 0.15f : 0));
            float r = 0.85f * (1 - g) + g * (0.32f + rock * 0.28f);
            float gg = 0.75f * (1 - g) + g * (0.52f - rock * 0.12f);
            float b = 0.50f * (1 - g) + g * (0.20f + rock * 0.18f);
            COL[j][i][0] = (unsigned char)(r * 255); COL[j][i][1] = (unsigned char)(gg * 255); COL[j][i][2] = (unsigned char)(b * 255);
        }
}

static void build_textures(void) {
    static unsigned char img[128 * 128 * 3];
    /* grass: light noise, a few darker blades */
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) {
            int v = 200 + (int)(sv_rand() % 40) - 20;
            if ((sv_rand() & 15) == 0) v -= 50;
            float n = noise(x * 0.125f + 3, y * 0.125f + 7, 16) * 50;
            v += (int)n - 25;
            v = v < 0 ? 0 : v > 255 ? 255 : v;
            unsigned char *p = &img[(y * 128 + x) * 3];
            p[0] = p[1] = p[2] = (unsigned char)v;
        }
    glGenTextures(1, &tex_grass);
    glBindTexture(GL_TEXTURE_2D, tex_grass);
    gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, 128, 128, GL_RGB, GL_UNSIGNED_BYTE, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    /* the glow: a soft disc with a bright middle (luminance and alpha) */
    static unsigned char glow[64 * 64 * 2];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            float dx = (x - 31.5f) / 32, dy = (y - 31.5f) / 32, d = sqrtf(dx * dx + dy * dy);
            float a = d >= 1 ? 0 : (1 - d) * (1 - d);
            float core = d < 0.22f ? 1 : d < 0.3f ? (0.3f - d) / 0.08f : 0;
            float l = a * 0.8f + core * 0.6f;
            glow[(y * 64 + x) * 2] = (unsigned char)(fminf(1.0f, l) * 255);
            glow[(y * 64 + x) * 2 + 1] = (unsigned char)(fminf(1.0f, a + core) * 255);
        }
    glGenTextures(1, &tex_glow);
    glBindTexture(GL_TEXTURE_2D, tex_glow);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gluBuild2DMipmaps(GL_TEXTURE_2D, GL_LUMINANCE_ALPHA, 64, 64, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, glow);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* ---------------- lanterns ---------------- */
static float cam_z(void) { return -t_sec * 5.5f; }

static void lantern_spawn(lantern_t *l, int anywhere) {
    float zc = cam_z();
    l->z = zc - 10 - frand() * (AHEAD * CELL - 16);
    l->x = valley_x(l->z) + (frand() - 0.5f) * 14;
    float g = height_at(l->x, l->z);
    if (g < 0) g = 0;
    l->y = g + 0.3f + (anywhere ? frand() * 14 : frand() * 1.5f);
    l->vy = 0.5f + frand() * 0.7f;
    l->phase = frand() * 6.28f;
    l->size = 0.55f + frand() * 0.35f;
    l->hue = frand();
}

/* ---------------- drawing ---------------- */
static void sky(int w, int h) {
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 1, 0, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_FOG);
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUAD_STRIP);
    glColor3f(0.98f, 0.62f, 0.38f); glVertex2f(0, 0.35f); glVertex2f(1, 0.35f);
    glColor3f(0.70f, 0.42f, 0.52f); glVertex2f(0, 0.62f); glVertex2f(1, 0.62f);
    glColor3f(0.10f, 0.10f, 0.28f); glVertex2f(0, 1); glVertex2f(1, 1);
    glEnd();
    glBegin(GL_QUADS);                 /* (below the horizon: the fog colour) */
    glColor3f(0.98f, 0.62f, 0.38f);
    glVertex2f(0, 0); glVertex2f(1, 0); glVertex2f(1, 0.35f); glVertex2f(0, 0.35f);
    glEnd();
    glPointSize(w > 800 ? 2 : 1);
    glBegin(GL_POINTS);
    for (int i = 0; i < NSTARS; i++) {
        float tw = 0.6f + 0.4f * sinf(t_sec * 2 + star[i][2] * 40);
        glColor3f(tw, tw, tw * 0.9f + 0.1f);
        glVertex2f(star[i][0], star[i][1]);
    }
    glEnd();
}

static void land(float zc) {
    int jc = (int)floorf(-zc / CELL);
    glEnable(GL_LIGHTING);
    glEnable(GL_COLOR_MATERIAL);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex_grass);
    for (int j = jc - 1; j < jc + AHEAD; j++) {
        int a = ((j % TZ) + TZ) % TZ, b = (a + 1) % TZ;
        float z0 = -CELL * j, z1 = -CELL * (j + 1);
        glBegin(GL_QUAD_STRIP);
        for (int i = 0; i <= TX; i++) {
            float x = -TX + CELL * i;
            /* (the far row first: counter-clockwise seen from above) */
            glColor3ubv(COL[b][i]); glNormal3fv(N[b][i]); glTexCoord2f(x * 0.2f, z1 * 0.2f); glVertex3f(x, H[b][i], z1);
            glColor3ubv(COL[a][i]); glNormal3fv(N[a][i]); glTexCoord2f(x * 0.2f, z0 * 0.2f); glVertex3f(x, H[a][i], z0);
        }
        glEnd();
    }
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glDisable(GL_COLOR_MATERIAL);
    /* the river: translucent, the sky's colours in it */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glBegin(GL_QUAD_STRIP);
    for (int j = jc - 1; j <= jc + AHEAD; j++) {
        float z = -CELL * j, cx = valley_x(z);
        float shimmer = 0.06f * sinf(z * 0.7f + t_sec * 1.7f);
        glColor4f(0.45f + shimmer, 0.40f, 0.62f, 0.72f);
        glVertex3f(cx - 7, 0.15f, z);     /* (counter-clockwise from above: j runs away from the view) */
        glVertex3f(cx + 7, 0.15f, z);
    }
    glEnd();
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

static void lanterns(float zc, float dt) {
    float m[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    float rx = m[0], ry = m[4], rz = m[8], ux = m[1], uy = m[5], uz = m[9];   /* the view's right and up */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex_glow);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glDepthMask(GL_FALSE);
    glDisable(GL_FOG);                 /* (additive light: faded by distance below instead) */
    glBegin(GL_QUADS);
    for (int i = 0; i < NLANTERNS; i++) {
        lantern_t *l = &lan[i];
        l->y += l->vy * dt;
        float sway = 0.35f * sinf(t_sec * 0.8f + l->phase);
        if (l->y > 34 || l->z > zc - 4) lantern_spawn(l, 0);
        float x = l->x + sway, y = l->y, z = l->z;
        float dist = zc - z, fade = dist > 60 ? fmaxf(0.0f, 1 - (dist - 60) / 30) : 1;
        float flick = 0.85f + 0.15f * sinf(t_sec * 9 + l->phase * 7);
        float s = l->size;
        glColor4f(1.0f, 0.55f + 0.2f * l->hue, 0.22f + 0.1f * l->hue, 0.9f * fade * flick);
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

static void draw(saver_t *s, float dt) {
    int w = s->w, h = s->h;
    glViewport(0, 0, w, h);
    sky(w, h);
    float zc = cam_z();
    float cx = valley_x(zc) * 0.85f, cy = fmaxf(height_at(cx, zc), 0.0f) + 4.5f + 1.2f * sinf(t_sec * 0.21f);
    float lz = zc - 26, lx = valley_x(lz);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(58, (double)w / (h ? h : 1), 0.4, AHEAD * CELL);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(cx, cy, zc, lx, cy - 2.5f, lz, 0, 1, 0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_FOG);
    float fog[4] = { 0.98f, 0.62f, 0.38f, 1 };
    glFogfv(GL_FOG_COLOR, fog);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, AHEAD * CELL * 0.35f);
    glFogf(GL_FOG_END, AHEAD * CELL * 0.95f);
    /* the low sun ahead, warm; a blue-violet sky light from above */
    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHT1);
    float sun[4] = { 0.3f, 0.35f, -1, 0 }, sunc[4] = { 1.0f, 0.78f, 0.55f, 1 };
    float skyl[4] = { -0.2f, 1, 0.3f, 0 }, skyc[4] = { 0.30f, 0.30f, 0.50f, 1 }, amb[4] = { 0.22f, 0.18f, 0.25f, 1 };
    glLightfv(GL_LIGHT0, GL_POSITION, sun);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, sunc);
    glLightfv(GL_LIGHT1, GL_POSITION, skyl);
    glLightfv(GL_LIGHT1, GL_DIFFUSE, skyc);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    land(zc);
    glDisable(GL_CULL_FACE);
    lanterns(zc, dt);
}

static void saver_start(saver_t *s) {
    if (!rc) {
        rc = hglCreateBufferContext(s->px, s->w, s->h, s->stride, 0);
        if (!rc) { sv_clear(s, 0); return; }
        hglMakeCurrent(rc);
        build_land();
        build_textures();
        for (int i = 0; i < NLANTERNS; i++) lantern_spawn(&lan[i], 1);
        for (int i = 0; i < NSTARS; i++) { star[i][0] = frand(); star[i][1] = 0.62f + frand() * 0.38f; star[i][2] = frand(); }
        built = 1;
        log_t0 = time_ms();
    } else hglSetBuffer(rc, s->px, s->w, s->h, s->stride);
    draw(s, 0);
    hglFinishFrame(rc);
}

static int saver_frame(saver_t *s, int dt) {
    if (!built) return 0;
    t_sec += dt * 0.001f;
    draw(s, dt * 0.001f);
    hglFinishFrame(rc);
    frames++;
    /* the frame rate since the last line: at 10, 20, 50 and 100 frames, then every 1000 */
    if (frames == 10 || frames == 20 || frames == 50 || frames == 100 || frames % 1000 == 0) {
        uint64_t now = time_ms();
        unsigned ms = (unsigned)(now - log_t0);
        unsigned n = frames - log_frames, tenths = ms ? (unsigned)((uint64_t)n * 10000 / ms) : 0;
        printf("lanterns: %u frames rendered (%dx%d), %u.%u frames/s\n", frames, s->w, s->h, tenths / 10, tenths % 10);
        log_t0 = now;
        log_frames = frames;
    }
    return 1;
}

static void saver_config(void) { sv_no_options("Lantern Valley"); }
