/* Halcyon's 3D library: the tile renderer.
 *
 * Set-up triangles (prim.c) and clears are queued in drawing order, each
 * with a snapshot of the per-pixel state, and binned into the 64 x 64 pixel
 * tiles they touch. When the frame is finished (or the queue fills, or
 * pixels are read) every tile is drawn on its own: its part of the color
 * and depth buffers stays in the cache while all its triangles are drawn,
 * and tiles share nothing, so threads can later draw different tiles.
 *
 * In a tile, a triangle's three edge functions are classified at the tile's
 * corners (64-bit), then per 8 x 8 block (32-bit): blocks outside are
 * skipped, blocks inside need no edge tests, and the rest test each pixel.
 * Pixels go four at a time through SSE2: depth test (float depth buffer),
 * perspective-correct attributes (a/w and 1/w planes, one division per
 * four pixels), texturing (nearest, bilinear, mipmaps with one level of
 * detail per four pixels, trilinear; repeat or clamp) for two units with
 * the GL_MODULATE / REPLACE / DECAL / BLEND / ADD functions, the specular
 * sum, fog, alpha test, blending, the color mask, then the writes. Colors
 * are 16-bit lanes (B G R A, two pixels per register) from the texture
 * stage on. */
#include "glint.h"

/* ---------------- the queue ---------------- */
#define TRI_CAP 8192
#define RS_CAP 512

void gl_raster_init(gl_context *c) {
    c->tri_cap = TRI_CAP;
    c->rs_cap = RS_CAP;
    c->tris = malloc(sizeof *c->tris * TRI_CAP);
    c->rstates = malloc(sizeof *c->rstates * RS_CAP);
    c->ntris = c->nrs = 0;
    c->rs_cur = NULL;
}

void gl_raster_free(gl_context *c) {
    if (c->bins) for (int i = 0; i < c->tiles_x * c->tiles_y; i++) free(c->bins[i].idx);
    free(c->bins);
    free(c->tris);
    free(c->rstates);
    c->bins = NULL;
    c->tris = NULL;
    c->rstates = NULL;
}

int gl_raster_resize(gl_context *c) {
    int tx = (c->w + TILE - 1) >> TILE_LOG, ty = (c->h + TILE - 1) >> TILE_LOG;
    if (tx == c->tiles_x && ty == c->tiles_y && c->bins) return 1;
    if (c->bins) for (int i = 0; i < c->tiles_x * c->tiles_y; i++) free(c->bins[i].idx);
    free(c->bins);
    c->bins = calloc((size_t)(tx * ty) + 1, sizeof *c->bins);
    c->tiles_x = c->bins ? tx : 0;
    c->tiles_y = c->bins ? ty : 0;
    return c->bins != NULL;
}

void gl_state_changed(gl_context *c) { c->rs_cur = NULL; }

static void build_rstate(gl_context *c, gl_rstate *rs) {
    memset(rs, 0, sizeof *rs);
    unsigned f = 0;
    if (c->en.depth_test && c->depth) {
        f |= RS_DEPTH_TEST;
        if (c->db.mask) f |= RS_DEPTH_WRITE;
    }
    rs->depth_func = c->db.func;
    if (c->en.alpha_test && c->cb.alpha_func != GL_ALWAYS) f |= RS_ALPHA_TEST;
    rs->alpha_func = c->cb.alpha_func;
    rs->alpha_ref255 = GL_CLAMPF(c->cb.alpha_ref, 0.0f, 1.0f) * 255;
    if (c->en.blend && !(c->cb.blend_src == GL_ONE && c->cb.blend_dst == GL_ZERO)) f |= RS_BLEND;
    rs->blend_src = c->cb.blend_src;
    rs->blend_dst = c->cb.blend_dst;
    if (c->en.fog) {
        f |= RS_FOG;
        for (int k = 0; k < 2; k++) {
            rs->fog_color[k * 4 + 0] = (uint16_t)(GL_CLAMPF(c->fog.color[2], 0.0f, 1.0f) * 255 + 0.5f);
            rs->fog_color[k * 4 + 1] = (uint16_t)(GL_CLAMPF(c->fog.color[1], 0.0f, 1.0f) * 255 + 0.5f);
            rs->fog_color[k * 4 + 2] = (uint16_t)(GL_CLAMPF(c->fog.color[0], 0.0f, 1.0f) * 255 + 0.5f);
            rs->fog_color[k * 4 + 3] = 0;
        }
    }
    if (c->en.lighting && c->lt.color_control == GL_SEPARATE_SPECULAR_COLOR) f |= RS_SPECULAR;
    uint32_t cm = (c->cb.mask[0] ? 0xFF0000u : 0) | (c->cb.mask[1] ? 0xFF00u : 0) | (c->cb.mask[2] ? 0xFFu : 0);
    rs->color_mask = cm;
    if (cm != 0xFFFFFF) f |= RS_COLOR_MASK;
    if (!cm) f |= RS_NO_COLOR;
    for (int u = 0; u < MAX_UNITS; u++) {
        if (!c->en.tex2d[u]) continue;
        gl_build_rs_unit(c, u, &rs->unit[u]);
        if (rs->unit[u].nlevels) f |= RS_TEX0 << u;
    }
    rs->flags = f;
}

const gl_rstate *gl_get_rstate(gl_context *c) {
    if (c->rs_cur) return c->rs_cur;
    if (c->nrs == c->rs_cap) return NULL;      /* (gl_queue_tri made room) */
    gl_rstate *rs = &c->rstates[c->nrs++];
    build_rstate(c, rs);
    c->rs_cur = rs;
    return rs;
}

/* a record for a triangle or a clear; room for a state snapshot too */
gl_tri *gl_queue_tri(gl_context *c) {
    if (!c->tris || !c->rstates || !c->bins) return NULL;
    if (c->ntris == c->tri_cap || (!c->rs_cur && c->nrs == c->rs_cap)) {
        gl_rstate keep;
        int had = c->rs_cur != NULL;
        if (had) keep = *c->rs_cur;
        gl_render(c);
        if (had) { c->rstates[0] = keep; c->nrs = 1; c->rs_cur = &c->rstates[0]; }
    }
    return &c->tris[c->ntris++];
}

void gl_bin_tri(gl_context *c, gl_tri *t) {
    uint32_t id = (uint32_t)(t - c->tris);
    int tx0 = t->minx >> TILE_LOG, tx1 = (t->maxx - 1) >> TILE_LOG;
    int ty0 = t->miny >> TILE_LOG, ty1 = (t->maxy - 1) >> TILE_LOG;
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++) {
            gl_bin *b = &c->bins[ty * c->tiles_x + tx];
            if (b->n == b->cap) {
                int cap = b->cap ? b->cap * 2 : 64;
                uint32_t *p = realloc(b->idx, sizeof *p * (size_t)cap);
                if (!p) { gl_error(c, GL_OUT_OF_MEMORY); continue; }
                b->idx = p;
                b->cap = cap;
            }
            b->idx[b->n++] = id;
        }
}

static uint32_t pack_color(const float *col) {
    unsigned r = (unsigned)(GL_CLAMPF(col[0], 0.0f, 1.0f) * 255 + 0.5f), g = (unsigned)(GL_CLAMPF(col[1], 0.0f, 1.0f) * 255 + 0.5f);
    unsigned b = (unsigned)(GL_CLAMPF(col[2], 0.0f, 1.0f) * 255 + 0.5f);
    return r << 16 | g << 8 | b;
}

void gl_queue_clear(gl_context *c, GLbitfield mask) {
    if (!c->depth) mask &= ~(GLbitfield)GL_DEPTH_BUFFER_BIT;
    uint32_t cm = (c->cb.mask[0] ? 0xFF0000u : 0) | (c->cb.mask[1] ? 0xFF00u : 0) | (c->cb.mask[2] ? 0xFFu : 0);
    if (!cm) mask &= ~(GLbitfield)GL_COLOR_BUFFER_BIT;
    if (!c->db.mask) mask &= ~(GLbitfield)GL_DEPTH_BUFFER_BIT;
    if (!(mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT))) return;
    if (c->clip_x0 >= c->clip_x1 || c->clip_y0 >= c->clip_y1) return;
    gl_tri *t = gl_queue_tri(c);
    if (!t) return;
    t->rs = NULL;
    t->flags = TF_CLEAR;
    t->minx = c->clip_x0; t->miny = c->clip_y0; t->maxx = c->clip_x1; t->maxy = c->clip_y1;
    t->a[0] = (int)pack_color(c->cb.clear);
    t->a[1] = (int)mask;
    t->a[2] = (int)cm;
    t->p[0][0] = GL_CLAMPF(c->db.clear, 0.0f, 1.0f);
    gl_bin_tri(c, t);
    c->frame_dirty = 1;
}

/* ---------------- clears ---------------- */
static void clear_tile(gl_context *c, const gl_tri *t, int tx0, int ty0, int tx1, int ty1) {
    int x0 = GL_MAX(t->minx, tx0), y0 = GL_MAX(t->miny, ty0), x1 = GL_MIN(t->maxx, tx1), y1 = GL_MIN(t->maxy, ty1);
    if (x0 >= x1 || y0 >= y1) return;
    unsigned mask = (unsigned)t->a[1];
    if (mask & GL_COLOR_BUFFER_BIT) {
        uint32_t col = (uint32_t)t->a[0], cm = (uint32_t)t->a[2];
        __m128i v = _mm_set1_epi32((int)col);
        for (int y = y0; y < y1; y++) {
            unsigned *row = c->color + (size_t)y * c->stride;
            int x = x0;
            if (cm == 0xFFFFFF) {
                for (; x + 4 <= x1; x += 4) _mm_storeu_si128((__m128i *)(row + x), v);
                for (; x < x1; x++) row[x] = col;
            } else {
                for (; x < x1; x++) row[x] = (row[x] & ~cm) | (col & cm);
            }
        }
    }
    if (mask & GL_DEPTH_BUFFER_BIT) {
        __m128 z = _mm_set1_ps(t->p[0][0]);
        for (int y = y0; y < y1; y++) {
            float *row = c->depth + (size_t)y * c->dstride;
            int x = x0;
            for (; x < x1 && (x & 3); x++) row[x] = t->p[0][0];
            for (; x + 4 <= x1; x += 4) _mm_store_ps(row + x, z);
            for (; x < x1; x++) row[x] = t->p[0][0];
        }
    }
}

/* ---------------- pixel helpers ---------------- */
typedef struct { __m128i lo, hi; } px16;   /* 4 pixels, 16-bit B G R A lanes: pixels 0-1, 2-3 */

static inline px16 unpack8(__m128i p) {
    px16 r = { _mm_unpacklo_epi8(p, _mm_setzero_si128()), _mm_unpackhi_epi8(p, _mm_setzero_si128()) };
    return r;
}
static inline __m128i pack8(px16 a) { return _mm_packus_epi16(a.lo, a.hi); }

/* one 32-bit value per pixel to the four lanes of each pixel */
static inline px16 bcast(__m128i v) {
    __m128i p = _mm_packs_epi32(v, v);
    __m128i q = _mm_unpacklo_epi16(p, p);
    px16 r = { _mm_unpacklo_epi32(q, q), _mm_unpackhi_epi32(q, q) };
    return r;
}
/* SoA floats (0..255) to pixels */
static inline px16 soa_px(__m128 r, __m128 g, __m128 b, __m128 a) {
    __m128i bg = _mm_packs_epi32(_mm_cvtps_epi32(b), _mm_cvtps_epi32(g));
    __m128i ra = _mm_packs_epi32(_mm_cvtps_epi32(r), _mm_cvtps_epi32(a));
    __m128i x = _mm_unpacklo_epi16(bg, ra), y = _mm_unpackhi_epi16(bg, ra);
    px16 o = { _mm_unpacklo_epi16(x, y), _mm_unpackhi_epi16(x, y) };
    return o;
}
static inline __m128 clamp255(__m128 v) { return _mm_min_ps(_mm_max_ps(v, _mm_setzero_ps()), _mm_set1_ps(255)); }

/* a * (b + 1) >> 8: modulation (255 * 255 -> 255) */
static inline __m128i mod16(__m128i a, __m128i b) {
    return _mm_srli_epi16(_mm_mullo_epi16(a, _mm_add_epi16(b, _mm_set1_epi16(1))), 8);
}
/* (a * (256 - w) + b * w) >> 8, w 0..256 per lane */
static inline __m128i lerp16(__m128i a, __m128i b, __m128i w) {
    __m128i iw = _mm_sub_epi16(_mm_set1_epi16(256), w);
    return _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(a, iw), _mm_mullo_epi16(b, w)), 8);
}
static inline px16 lerp_px(px16 a, px16 b, px16 w) { px16 r = { lerp16(a.lo, b.lo, w.lo), lerp16(a.hi, b.hi, w.hi) }; return r; }
static inline __m128i alpha16(__m128i v) { return _mm_shufflehi_epi16(_mm_shufflelo_epi16(v, 0xFF), 0xFF); }
static inline __m128i to256(__m128i v) { return _mm_add_epi16(v, _mm_srli_epi16(v, 7)); }   /* 0..255 -> 0..256 */
static inline __m128i sel16(__m128i m, __m128i a, __m128i b) { return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b)); }

static inline __m128i floor_i(__m128 x) {
    __m128i t = _mm_cvttps_epi32(x);
    return _mm_add_epi32(t, _mm_castps_si128(_mm_cmpgt_ps(_mm_cvtepi32_ps(t), x)));
}
static inline __m128i wrap(__m128i i, int clamp, int size) {
    if (clamp) {
        __m128i mx = _mm_set1_epi32(size - 1);
        i = _mm_andnot_si128(_mm_srai_epi32(i, 31), i);
        __m128i gt = _mm_cmpgt_epi32(i, mx);
        return _mm_or_si128(_mm_and_si128(gt, mx), _mm_andnot_si128(gt, i));
    }
    return _mm_and_si128(i, _mm_set1_epi32(size - 1));
}
static inline __m128i fetch4(const uint32_t *tex, __m128i idx) {
    int i[4];
    _mm_storeu_si128((__m128i *)i, idx);
    return _mm_setr_epi32((int)tex[i[0]], (int)tex[i[1]], (int)tex[i[2]], (int)tex[i[3]]);
}

/* four texels of one level (s, t in texture coordinates 0..1) */
static px16 sample_level(const gl_rs_unit *u, int L, __m128 s, __m128 t, int linear) {
    int wl = u->wlog[L], hl = u->hlog[L], W = 1 << wl, H = 1 << hl;
    __m128 us = _mm_mul_ps(s, _mm_set1_ps((float)W)), vs = _mm_mul_ps(t, _mm_set1_ps((float)H));
    /* keep the integers in range (the repeat masks them, the clamp limits them) */
    __m128 lim = _mm_set1_ps(4194304.0f);
    us = _mm_min_ps(_mm_max_ps(us, _mm_sub_ps(_mm_setzero_ps(), lim)), lim);
    vs = _mm_min_ps(_mm_max_ps(vs, _mm_sub_ps(_mm_setzero_ps(), lim)), lim);
    const uint32_t *tex = u->level[L];
    __m128i shift = _mm_cvtsi32_si128(wl);
    if (!linear) {
        __m128i iu = wrap(floor_i(us), u->clamp_s, W), iv = wrap(floor_i(vs), u->clamp_t, H);
        return unpack8(fetch4(tex, _mm_add_epi32(_mm_sll_epi32(iv, shift), iu)));
    }
    us = _mm_sub_ps(us, _mm_set1_ps(0.5f));
    vs = _mm_sub_ps(vs, _mm_set1_ps(0.5f));
    __m128i iu = floor_i(us), iv = floor_i(vs);
    __m128i fu = _mm_cvttps_epi32(_mm_mul_ps(_mm_sub_ps(us, _mm_cvtepi32_ps(iu)), _mm_set1_ps(256)));
    __m128i fv = _mm_cvttps_epi32(_mm_mul_ps(_mm_sub_ps(vs, _mm_cvtepi32_ps(iv)), _mm_set1_ps(256)));
    __m128i one = _mm_set1_epi32(1);
    __m128i u0 = wrap(iu, u->clamp_s, W), u1 = wrap(_mm_add_epi32(iu, one), u->clamp_s, W);
    __m128i r0 = _mm_sll_epi32(wrap(iv, u->clamp_t, H), shift), r1 = _mm_sll_epi32(wrap(_mm_add_epi32(iv, one), u->clamp_t, H), shift);
    px16 t00 = unpack8(fetch4(tex, _mm_add_epi32(r0, u0))), t10 = unpack8(fetch4(tex, _mm_add_epi32(r0, u1)));
    px16 t01 = unpack8(fetch4(tex, _mm_add_epi32(r1, u0))), t11 = unpack8(fetch4(tex, _mm_add_epi32(r1, u1)));
    px16 wu = bcast(fu), wv = bcast(fv);
    return lerp_px(lerp_px(t00, t10, wu), lerp_px(t01, t11, wu), wv);
}

static inline float fast_log2(float x) {
    union { float f; uint32_t u; } b = { x };
    float e = (float)((int)(b.u >> 23) - 127);
    b.u = (b.u & 0x7FFFFF) | 0x3F800000;
    float m = b.f;
    return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}

/* ---------------- one triangle in one tile ---------------- */
typedef struct tri_ctx {
    const gl_tri *t;
    const gl_rstate *rs;
    px16 color, spec;                      /* when constant */
} tri_ctx;

static inline __m128 attr(const float p[3], float fx, float fy) {
    return _mm_add_ps(_mm_set1_ps(p[0] + p[1] * fx + p[2] * fy), _mm_mul_ps(_mm_set1_ps(p[1]), _mm_setr_ps(0, 1, 2, 3)));
}

/* the texture of one unit for 4 pixels at (fx, fy) relative to the
 * triangle's reference point; w = 1 / (1/w) */
static px16 texture_unit(const tri_ctx *tc, int unit, float fx, float fy, __m128 w) {
    const gl_tri *t = tc->t;
    const gl_rs_unit *u = &tc->rs->unit[unit];
    int base = unit ? A_S1 : A_S0;
    const float *ps = t->p[base], *pt = t->p[base + 1];
    const float *pq = (t->flags & (unit ? TF_TEXQ1 : TF_TEXQ0)) ? t->p[base + 2] : t->p[A_Q];
    __m128 wq = w;
    if (pq != t->p[A_Q]) wq = _mm_div_ps(_mm_set1_ps(1), attr(pq, fx, fy));
    __m128 s = _mm_mul_ps(attr(ps, fx, fy), wq), tt = _mm_mul_ps(attr(pt, fx, fy), wq);
    /* the level of detail of these four pixels (at the second one) */
    float lambda = 0;
    if (u->mip || u->min_linear != u->mag_linear) {
        float x = fx + 1.5f;
        float Q = pq[0] + pq[1] * x + pq[2] * fy;
        if (Q != 0) {
            float iq = 1 / Q;
            float sv = (ps[0] + ps[1] * x + ps[2] * fy) * iq, tv = (pt[0] + pt[1] * x + pt[2] * fy) * iq;
            float dsdx = (ps[1] - sv * pq[1]) * iq * u->w0, dsdy = (ps[2] - sv * pq[2]) * iq * u->w0;
            float dtdx = (pt[1] - tv * pq[1]) * iq * u->h0, dtdy = (pt[2] - tv * pq[2]) * iq * u->h0;
            float rx = dsdx * dsdx + dtdx * dtdx, ry = dsdy * dsdy + dtdy * dtdy;
            float r2 = rx > ry ? rx : ry;
            lambda = r2 > 0 ? 0.5f * fast_log2(r2) : -100;
        }
    }
    /* magnification below the switch-over point */
    float cpt = u->mag_linear && !u->min_linear && u->mip ? 0.5f : 0;
    if (lambda <= cpt) return sample_level(u, 0, s, tt, u->mag_linear);
    if (!u->mip) return sample_level(u, 0, s, tt, u->min_linear);
    int maxl = u->nlevels - 1;
    if (u->mip == 1) {
        int d = lambda <= 0.5f ? 0 : (int)(lambda + 0.5f);
        if (d > maxl) d = maxl;
        return sample_level(u, d, s, tt, u->min_linear);
    }
    int d0 = (int)lambda;
    if (d0 >= maxl) return sample_level(u, maxl, s, tt, u->min_linear);
    int f = (int)((lambda - (float)d0) * 256);
    px16 a = sample_level(u, d0, s, tt, u->min_linear);
    if (f <= 0) return a;
    px16 b = sample_level(u, d0 + 1, s, tt, u->min_linear);
    __m128i fw = _mm_set1_epi16((short)f);
    px16 w2 = { fw, fw };
    return lerp_px(a, b, w2);
}

/* the texture environment: fragment f, texel x */
static inline __m128i env16(const gl_rs_unit *u, __m128i f, __m128i x) {
    __m128i amask = _mm_setr_epi16(0, 0, 0, -1, 0, 0, 0, -1);
    __m128i r;
    switch (u->env) {
    case GL_REPLACE:
        r = x;
        if (u->keep_rgb) r = sel16(amask, r, f);
        if (u->keep_a) r = sel16(amask, f, r);
        return r;
    case GL_DECAL: {
        __m128i ta = to256(alpha16(x));
        r = lerp16(f, x, ta);
        if (u->keep_rgb) return f;
        return sel16(amask, f, r);
    }
    case GL_BLEND: {
        __m128i ec = _mm_loadu_si128((const __m128i *)u->env_color);
        __m128i b = lerp16(f, ec, to256(x));        /* Cf (1 - Ct) + Cc Ct */
        __m128i m = mod16(f, x);
        if (u->intensity) return b;
        r = sel16(amask, m, b);
        if (u->keep_rgb) r = sel16(amask, r, f);
        return r;
    }
    case GL_ADD: {
        __m128i s = _mm_min_epi16(_mm_add_epi16(f, x), _mm_set1_epi16(255));
        if (u->intensity) return s;
        r = sel16(amask, mod16(f, x), s);
        if (u->keep_rgb) r = sel16(amask, r, f);
        return r;
    }
    default:                                        /* GL_MODULATE */
        return mod16(f, x);
    }
}

static inline px16 env_px(const gl_rs_unit *u, px16 f, px16 x) {
    px16 r = { env16(u, f.lo, x.lo), env16(u, f.hi, x.hi) };
    return r;
}

/* blend factor (0..256 per lane) */
static inline __m128i factor16(GLenum f, __m128i s, __m128i d) {
    switch (f) {
    case GL_ZERO: return _mm_setzero_si128();
    case GL_ONE: return _mm_set1_epi16(256);
    case GL_SRC_COLOR: return to256(s);
    case GL_ONE_MINUS_SRC_COLOR: return _mm_sub_epi16(_mm_set1_epi16(256), to256(s));
    case GL_SRC_ALPHA: return to256(alpha16(s));
    case GL_ONE_MINUS_SRC_ALPHA: return _mm_sub_epi16(_mm_set1_epi16(256), to256(alpha16(s)));
    case GL_DST_COLOR: return to256(d);
    case GL_ONE_MINUS_DST_COLOR: return _mm_sub_epi16(_mm_set1_epi16(256), to256(d));
    case GL_DST_ALPHA: return _mm_set1_epi16(256);          /* the frame buffer has no alpha: 1 */
    case GL_ONE_MINUS_DST_ALPHA: return _mm_setzero_si128();
    case GL_SRC_ALPHA_SATURATE: return _mm_setr_epi16(0, 0, 0, 256, 0, 0, 0, 256);   /* min(As, 1 - Ad) = 0 */
    }
    return _mm_set1_epi16(256);
}
static inline __m128i blend16(const gl_rstate *rs, __m128i s, __m128i d) {
    __m128i fs = factor16(rs->blend_src, s, d), fd = factor16(rs->blend_dst, s, d);
    __m128i a = _mm_srli_epi16(_mm_mullo_epi16(s, fs), 8), b = _mm_srli_epi16(_mm_mullo_epi16(d, fd), 8);
    return _mm_min_epi16(_mm_add_epi16(a, b), _mm_set1_epi16(255));
}

static inline __m128 depth_cmp(GLenum func, __m128 z, __m128 zb) {
    switch (func) {
    case GL_NEVER: return _mm_setzero_ps();
    case GL_LESS: return _mm_cmplt_ps(z, zb);
    case GL_EQUAL: return _mm_cmpeq_ps(z, zb);
    case GL_LEQUAL: return _mm_cmple_ps(z, zb);
    case GL_GREATER: return _mm_cmpgt_ps(z, zb);
    case GL_NOTEQUAL: return _mm_cmpneq_ps(z, zb);
    case GL_GEQUAL: return _mm_cmpge_ps(z, zb);
    }
    return _mm_castsi128_ps(_mm_set1_epi32(-1));
}

/* 4 pixels at (x, y) of the frame buffer, the coverage mask m */
static void shade4(gl_context *c, const tri_ctx *tc, int x, int y, __m128i m) {
    const gl_tri *t = tc->t;
    const gl_rstate *rs = tc->rs;
    unsigned fl = rs->flags;
    float fx = (float)x + 0.5f - t->x0, fy = (float)y + 0.5f - t->y0;
    float *zp = c->depth ? c->depth + (size_t)y * c->dstride + x : NULL;
    unsigned *cp = c->color + (size_t)y * c->stride + x;
    unsigned tail[4];
    int ntail = 0;
    if (x + 4 > c->w) {                    /* the last pixels of a row: through a copy */
        ntail = c->w - x;
        for (int i = 0; i < 4; i++) tail[i] = i < ntail ? cp[i] : 0;
        cp = tail;
    }
    __m128 z = _mm_setzero_ps(), zb = _mm_setzero_ps();
    if (fl & (RS_DEPTH_TEST | RS_DEPTH_WRITE)) z = attr(t->p[A_Z], fx, fy);
    if (fl & RS_DEPTH_TEST) {
        zb = _mm_load_ps(zp);
        m = _mm_and_si128(m, _mm_castps_si128(depth_cmp(rs->depth_func, z, zb)));
        if (!_mm_movemask_epi8(m)) return;
    }
    if (!(fl & RS_NO_COLOR) || (fl & RS_ALPHA_TEST)) {
        __m128 w = _mm_div_ps(_mm_set1_ps(1), attr(t->p[A_Q], fx, fy));
        px16 f;
        if (t->flags & TF_SMOOTH)
            f = soa_px(clamp255(_mm_mul_ps(attr(t->p[A_R], fx, fy), w)), clamp255(_mm_mul_ps(attr(t->p[A_G], fx, fy), w)),
                       clamp255(_mm_mul_ps(attr(t->p[A_B], fx, fy), w)), clamp255(_mm_mul_ps(attr(t->p[A_A], fx, fy), w)));
        else f = tc->color;
        if (fl & RS_TEX0) f = env_px(&rs->unit[0], f, texture_unit(tc, 0, fx, fy, w));
        if (fl & RS_TEX1) f = env_px(&rs->unit[1], f, texture_unit(tc, 1, fx, fy, w));
        if (fl & RS_SPECULAR) {
            px16 s;
            if (t->flags & TF_SPEC_SMOOTH)
                s = soa_px(clamp255(_mm_mul_ps(attr(t->p[A_SR], fx, fy), w)), clamp255(_mm_mul_ps(attr(t->p[A_SG], fx, fy), w)),
                           clamp255(_mm_mul_ps(attr(t->p[A_SB], fx, fy), w)), _mm_setzero_ps());
            else s = tc->spec;
            __m128i k255 = _mm_set1_epi16(255);
            f.lo = _mm_min_epi16(_mm_add_epi16(f.lo, s.lo), k255);
            f.hi = _mm_min_epi16(_mm_add_epi16(f.hi, s.hi), k255);
        }
        if (fl & RS_FOG) {
            __m128 ff = _mm_mul_ps(_mm_mul_ps(attr(t->p[A_FOG], fx, fy), w), _mm_set1_ps(256));
            ff = _mm_min_ps(_mm_max_ps(ff, _mm_setzero_ps()), _mm_set1_ps(256));
            px16 fw = bcast(_mm_cvtps_epi32(ff));
            __m128i amask = _mm_setr_epi16(0, 0, 0, -1, 0, 0, 0, -1);
            __m128i fc = _mm_loadu_si128((const __m128i *)rs->fog_color);
            f.lo = sel16(amask, f.lo, lerp16(fc, f.lo, fw.lo));
            f.hi = sel16(amask, f.hi, lerp16(fc, f.hi, fw.hi));
        }
        __m128i out = pack8(f);
        if (fl & RS_ALPHA_TEST) {
            __m128 a = _mm_cvtepi32_ps(_mm_srli_epi32(out, 24)), ref = _mm_set1_ps(rs->alpha_ref255);
            m = _mm_and_si128(m, _mm_castps_si128(depth_cmp(rs->alpha_func, a, ref)));
            if (!_mm_movemask_epi8(m)) return;
        }
        if (!(fl & RS_NO_COLOR)) {
            __m128i old = _mm_loadu_si128((const __m128i *)cp);
            if (fl & RS_BLEND) {
                px16 d = unpack8(old);
                f.lo = blend16(rs, f.lo, d.lo);
                f.hi = blend16(rs, f.hi, d.hi);
                out = pack8(f);
            }
            __m128i wm = _mm_and_si128(m, _mm_set1_epi32((int)rs->color_mask));
            _mm_storeu_si128((__m128i *)cp, _mm_or_si128(_mm_and_si128(out, wm), _mm_andnot_si128(wm, old)));
        }
    }
    if (fl & RS_DEPTH_WRITE) {
        if (!(fl & RS_DEPTH_TEST)) zb = _mm_load_ps(zp);
        __m128 mf = _mm_castsi128_ps(m);
        _mm_store_ps(zp, _mm_or_ps(_mm_and_ps(mf, z), _mm_andnot_ps(mf, zb)));
    }
    if (ntail) {
        unsigned *dst = c->color + (size_t)y * c->stride + x;
        for (int i = 0; i < ntail; i++) dst[i] = tail[i];
    }
}

static void draw_tri_tile(gl_context *c, const gl_tri *t, int tx0, int ty0, int tx1, int ty1) {
    int x0 = GL_MAX(t->minx, tx0), y0 = GL_MAX(t->miny, ty0), x1 = GL_MIN(t->maxx, tx1), y1 = GL_MIN(t->maxy, ty1);
    if (x0 >= x1 || y0 >= y1) return;
    /* the edges at the corners of this part of the tile (64-bit) */
    int ne = 0, e0[3], sx[3], sy[3];
    int64_t px0 = (int64_t)x0 * 16 + 8, py0 = (int64_t)y0 * 16 + 8;
    int64_t dx = (int64_t)(x1 - 1 - x0) * 16, dy = (int64_t)(y1 - 1 - y0) * 16;
    for (int e = 0; e < 3; e++) {
        int64_t a = t->a[e], b = t->b[e];
        int64_t v00 = a * px0 + b * py0 + t->c[e], v10 = v00 + a * dx, v01 = v00 + b * dy, v11 = v10 + b * dy;
        int64_t mn = GL_MIN(GL_MIN(v00, v10), GL_MIN(v01, v11)), mx = GL_MAX(GL_MAX(v00, v10), GL_MAX(v01, v11));
        if (mx < 0) return;                /* the tile is outside this edge */
        if (mn >= 0) continue;             /* inside it everywhere: no test */
        e0[ne] = (int)v00;
        sx[ne] = (int)(a * 16);
        sy[ne] = (int)(b * 16);
        ne++;
    }
    tri_ctx tc;
    tc.t = t;
    tc.rs = t->rs;
    if (!(t->flags & TF_SMOOTH))
        tc.color = soa_px(_mm_set1_ps(GL_CLAMPF(t->p[A_R][0], 0.0f, 255.0f)), _mm_set1_ps(GL_CLAMPF(t->p[A_G][0], 0.0f, 255.0f)),
                          _mm_set1_ps(GL_CLAMPF(t->p[A_B][0], 0.0f, 255.0f)), _mm_set1_ps(GL_CLAMPF(t->p[A_A][0], 0.0f, 255.0f)));
    if (!(t->flags & TF_SPEC_SMOOTH))
        tc.spec = soa_px(_mm_set1_ps(GL_CLAMPF(t->p[A_SR][0], 0.0f, 255.0f)), _mm_set1_ps(GL_CLAMPF(t->p[A_SG][0], 0.0f, 255.0f)),
                         _mm_set1_ps(GL_CLAMPF(t->p[A_SB][0], 0.0f, 255.0f)), _mm_setzero_ps());
    __m128i lanes = _mm_setr_epi32(0, 1, 2, 3);
    __m128i estep[3];
    for (int k = 0; k < ne; k++) estep[k] = _mm_setr_epi32(0, sx[k], 2 * sx[k], 3 * sx[k]);
    __m128i xlo = _mm_set1_epi32(x0 - 1), xhi = _mm_set1_epi32(x1);
    for (int by = y0 & ~7; by < y1; by += 8) {
        int by0 = GL_MAX(by, y0), by1 = GL_MIN(by + 8, y1);
        for (int bx = x0 & ~7; bx < x1; bx += 8) {
            int bx0 = GL_MAX(bx, x0), bx1 = GL_MIN(bx + 8, x1);
            unsigned partial = 0;
            int reject = 0;
            for (int k = 0; k < ne; k++) {
                int ev = e0[k] + (bx0 - x0) * sx[k] + (by0 - y0) * sy[k];
                int ex = (bx1 - 1 - bx0) * sx[k], ey = (by1 - 1 - by0) * sy[k];
                int c00 = ev, c10 = ev + ex, c01 = ev + ey, c11 = ev + ex + ey;
                int mn = GL_MIN(GL_MIN(c00, c10), GL_MIN(c01, c11)), mx = GL_MAX(GL_MAX(c00, c10), GL_MAX(c01, c11));
                if (mx < 0) { reject = 1; break; }
                if (mn < 0) partial |= 1u << k;
            }
            if (reject) continue;
            for (int y = by0; y < by1; y++) {
                for (int gx = bx; gx < bx + 8 && gx < x1; gx += 4) {
                    if (gx + 4 <= x0) continue;
                    __m128i xs = _mm_add_epi32(_mm_set1_epi32(gx), lanes);
                    __m128i m = _mm_and_si128(_mm_cmpgt_epi32(xs, xlo), _mm_cmplt_epi32(xs, xhi));
                    if (partial) {
                        __m128i acc = _mm_setzero_si128();
                        for (int k = 0; k < ne; k++) {
                            if (!(partial & (1u << k))) continue;
                            int ev = e0[k] + (gx - x0) * sx[k] + (y - y0) * sy[k];
                            acc = _mm_or_si128(acc, _mm_add_epi32(_mm_set1_epi32(ev), estep[k]));
                        }
                        m = _mm_andnot_si128(_mm_srai_epi32(acc, 31), m);
                    }
                    if (!_mm_movemask_epi8(m)) continue;
                    shade4(c, &tc, gx, y, m);
                }
            }
        }
    }
}

/* ---------------- drawing the queue ---------------- */
static void render_tile(gl_context *c, int tx, int ty) {
    gl_bin *b = &c->bins[ty * c->tiles_x + tx];
    int x0 = tx << TILE_LOG, y0 = ty << TILE_LOG;
    int x1 = GL_MIN(x0 + TILE, c->w), y1 = GL_MIN(y0 + TILE, c->h);
    for (int i = 0; i < b->n; i++) {
        const gl_tri *t = &c->tris[b->idx[i]];
        if (t->flags & TF_CLEAR) clear_tile(c, t, x0, y0, x1, y1);
        else draw_tri_tile(c, t, x0, y0, x1, y1);
    }
    b->n = 0;
}

void gl_render(gl_context *c) {
    if (!c->ntris) return;
    for (int ty = 0; ty < c->tiles_y; ty++)
        for (int tx = 0; tx < c->tiles_x; tx++) render_tile(c, tx, ty);
    c->tris_drawn += (unsigned long long)c->ntris;
    c->ntris = 0;
    c->nrs = 0;
    c->rs_cur = NULL;
}

/* ---------------- GL calls ---------------- */
void glClear(GLbitfield mask) {
    GET_CTX(c);
    if (DL_ACTIVE(c)) { gl_dlarg a = { .u = mask }; if (gl_dl_record(c, OP_CLEAR, 1, &a)) return; }
    NOT_IN_BEGIN(c);
    if (mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_ACCUM_BUFFER_BIT)) {
        gl_error(c, GL_INVALID_VALUE);
        return;
    }
    gl_queue_clear(c, mask);
}

void glFinish(void) { GET_CTX(c); NOT_IN_BEGIN(c); gl_render(c); }
void glFlush(void) { GET_CTX(c); NOT_IN_BEGIN(c); gl_render(c); }

static int pack_row_bytes(gl_context *c, int w, int bpp) {
    int len = (c->cl.pack_row > 0 ? c->cl.pack_row : w) * bpp;
    int al = c->cl.pack_align;
    return (len + al - 1) / al * al;
}

void glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, GLvoid *pixels) {
    GET_CTX(c);
    NOT_IN_BEGIN(c);
    if (w < 0 || h < 0) { gl_error(c, GL_INVALID_VALUE); return; }
    int comps;
    switch (format) {
    case GL_RGB: case GL_BGR: comps = 3; break;
    case GL_RGBA: case GL_BGRA: comps = 4; break;
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT: comps = 1; break;
    case GL_LUMINANCE_ALPHA: comps = 2; break;
    default: gl_error(c, GL_INVALID_ENUM); return;
    }
    int bsize = type == GL_UNSIGNED_BYTE ? 1 : type == GL_FLOAT || type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 0;
    if (!bsize) { gl_error(c, GL_INVALID_ENUM); return; }
    if (format == GL_DEPTH_COMPONENT && !c->depth) { gl_error(c, GL_INVALID_OPERATION); return; }
    gl_render(c);
    int rowb = pack_row_bytes(c, w, comps * bsize);
    unsigned char *out = (unsigned char *)pixels + (size_t)c->cl.pack_skip_rows * (size_t)rowb + (size_t)c->cl.pack_skip_px * (size_t)(comps * bsize);
    for (int j = 0; j < h; j++) {
        int gy = y + j;                    /* GL row (from the bottom) */
        unsigned char *o = out + (size_t)j * (size_t)rowb;
        for (int i = 0; i < w; i++) {
            int gx = x + i, sy = c->h - 1 - gy;
            float v[4] = { 0, 0, 0, 1 };
            int inside = gx >= 0 && gx < c->w && gy >= 0 && gy < c->h;
            if (format == GL_DEPTH_COMPONENT) {
                v[0] = inside ? c->depth[(size_t)sy * c->dstride + gx] : 0;
            } else if (inside) {
                unsigned p = c->color[(size_t)sy * c->stride + gx];
                v[0] = ((p >> 16) & 255) / 255.0f; v[1] = ((p >> 8) & 255) / 255.0f; v[2] = (p & 255) / 255.0f;
            }
            float cv[4];
            switch (format) {
            case GL_RGB: cv[0] = v[0]; cv[1] = v[1]; cv[2] = v[2]; break;
            case GL_BGR: cv[0] = v[2]; cv[1] = v[1]; cv[2] = v[0]; break;
            case GL_RGBA: cv[0] = v[0]; cv[1] = v[1]; cv[2] = v[2]; cv[3] = 1; break;
            case GL_BGRA: cv[0] = v[2]; cv[1] = v[1]; cv[2] = v[0]; cv[3] = 1; break;
            case GL_GREEN: cv[0] = v[1]; break;
            case GL_BLUE: cv[0] = v[2]; break;
            case GL_ALPHA: cv[0] = 1; break;
            case GL_LUMINANCE: cv[0] = GL_MIN(1.0f, v[0] + v[1] + v[2]); break;
            case GL_LUMINANCE_ALPHA: cv[0] = GL_MIN(1.0f, v[0] + v[1] + v[2]); cv[1] = 1; break;
            default: cv[0] = v[0]; break;   /* GL_RED, GL_DEPTH_COMPONENT */
            }
            for (int k = 0; k < comps; k++) {
                float f = cv[k];
                unsigned char *d = o + (size_t)(i * comps + k) * (size_t)bsize;
                if (type == GL_UNSIGNED_BYTE) *d = (unsigned char)(f * 255 + 0.5f);
                else if (type == GL_FLOAT) memcpy(d, &f, 4);
                else if (type == GL_UNSIGNED_SHORT) { unsigned short s = (unsigned short)(f * 65535 + 0.5f); memcpy(d, &s, 2); }
                else { unsigned u = (unsigned)((double)f * 4294967295.0 + 0.5); memcpy(d, &u, 4); }
            }
        }
    }
}
