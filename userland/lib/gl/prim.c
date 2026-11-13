/* Halcyon's 3D library: primitives. Assembly of points, lines and polygons
 * from processed vertices, clipping in homogeneous coordinates (the six
 * frustum planes, the near plane included, and up to six user planes),
 * culling and the facing of two-sided lighting, polygon modes, and the
 * set-up of triangles for the tile renderer: vertices snapped to 1/16
 * pixel, integer edge functions with the top-left fill rule, and planes for
 * every attribute (perspective-correct: a/w and 1/w are interpolated).
 * Lines and points become quads of their width. */
#include "glint.h"

#define SUBPIX 16                          /* 4 bits of sub-pixel precision */

void gl_compute_win(gl_context *c, gl_vertex *v) {
    if (v->flags & GLV_WIN) return;
    float w = v->clip[3];
    float iw = w != 0 ? 1 / w : 1e30f;
    float nx = v->clip[0] * iw, ny = v->clip[1] * iw, nz = v->clip[2] * iw;
    v->win[0] = c->vp.x + (nx + 1) * 0.5f * c->vp.w;
    v->win[1] = c->h - (c->vp.y + (ny + 1) * 0.5f * c->vp.h);
    v->win[2] = c->vp.znear + (nz + 1) * 0.5f * (c->vp.zfar - c->vp.znear);
    v->win[3] = iw;
    v->flags |= GLV_WIN;
}

/* ---------------- triangle set-up ---------------- */
static void plane(float *p, float u0, float u1, float u2, float ex1, float ey1, float ex2, float ey2, float inv) {
    float d1 = u1 - u0, d2 = u2 - u0;
    p[0] = u0;
    p[1] = (d1 * ey2 - d2 * ey1) * inv;
    p[2] = (d2 * ex1 - d1 * ex2) * inv;
}

/* v: the three vertices (window coordinates set), side: which colors
 * (0 front, 1 back), flat: the vertex whose color a flat-shaded triangle
 * takes (or NULL), offset: apply the polygon offset */
static void setup_tri(gl_context *c, const gl_vertex *v0, const gl_vertex *v1, const gl_vertex *v2, int side,
                      const gl_vertex *flat, int offset) {
    int X0 = _mm_cvtss_si32(_mm_set_ss(v0->win[0] * SUBPIX)), Y0 = _mm_cvtss_si32(_mm_set_ss(v0->win[1] * SUBPIX));
    int X1 = _mm_cvtss_si32(_mm_set_ss(v1->win[0] * SUBPIX)), Y1 = _mm_cvtss_si32(_mm_set_ss(v1->win[1] * SUBPIX));
    int X2 = _mm_cvtss_si32(_mm_set_ss(v2->win[0] * SUBPIX)), Y2 = _mm_cvtss_si32(_mm_set_ss(v2->win[1] * SUBPIX));
    int64_t area = (int64_t)(X1 - X0) * (Y2 - Y0) - (int64_t)(Y1 - Y0) * (X2 - X0);
    if (area == 0) return;
    if (area < 0) {                        /* the rasterizer wants one winding */
        const gl_vertex *t = v1; v1 = v2; v2 = t;
        int tx = X1; X1 = X2; X2 = tx;
        int ty = Y1; Y1 = Y2; Y2 = ty;
        area = -area;
    }
    /* pixels whose centers can be inside */
    int minX = GL_MIN(X0, GL_MIN(X1, X2)), maxX = GL_MAX(X0, GL_MAX(X1, X2));
    int minY = GL_MIN(Y0, GL_MIN(Y1, Y2)), maxY = GL_MAX(Y0, GL_MAX(Y1, Y2));
    int px0 = (minX - SUBPIX / 2 + SUBPIX - 1) >> 4, px1 = ((maxX - SUBPIX / 2) >> 4) + 1;
    int py0 = (minY - SUBPIX / 2 + SUBPIX - 1) >> 4, py1 = ((maxY - SUBPIX / 2) >> 4) + 1;
    px0 = GL_MAX(px0, c->clip_x0); py0 = GL_MAX(py0, c->clip_y0);
    px1 = GL_MIN(px1, c->clip_x1); py1 = GL_MIN(py1, c->clip_y1);
    if (px0 >= px1 || py0 >= py1) return;

    /* the record first: making room may draw the queue and start the
     * snapshots again, which the state pointer must come after */
    gl_tri *t = gl_queue_tri(c);
    if (!t) return;
    const gl_rstate *rs = gl_get_rstate(c);
    if (!rs) { c->ntris--; return; }
    t->rs = rs;
    t->flags = 0;
    t->minx = px0; t->miny = py0; t->maxx = px1; t->maxy = py1;
    const int X[3] = { X0, X1, X2 }, Y[3] = { Y0, Y1, Y2 };
    for (int e = 0; e < 3; e++) {
        int i = e, j = e == 2 ? 0 : e + 1;
        int a = Y[i] - Y[j], b = X[j] - X[i];
        int64_t cc = -((int64_t)a * X[i] + (int64_t)b * Y[i]);
        if (!(a > 0 || (a == 0 && b > 0))) cc -= 1;         /* top-left rule: other edges exclude their pixels */
        t->a[e] = a; t->b[e] = b; t->c[e] = cc;
    }
    /* attribute planes around vertex 0 (snapped positions) */
    float x0 = X0 * (1.0f / SUBPIX), y0 = Y0 * (1.0f / SUBPIX);
    float ex1 = X1 * (1.0f / SUBPIX) - x0, ey1 = Y1 * (1.0f / SUBPIX) - y0;
    float ex2 = X2 * (1.0f / SUBPIX) - x0, ey2 = Y2 * (1.0f / SUBPIX) - y0;
    float inv = (float)(256.0 / (double)area);
    t->x0 = x0; t->y0 = y0;
    const gl_vertex *V[3] = { v0, v1, v2 };
    float q[3] = { v0->win[3], v1->win[3], v2->win[3] };
#define PLANE(slot, expr) do { float u_[3]; for (int k_ = 0; k_ < 3; k_++) { const gl_vertex *vv = V[k_]; (void)vv; u_[k_] = (expr); } \
                               plane(t->p[slot], u_[0], u_[1], u_[2], ex1, ey1, ex2, ey2, inv); } while (0)
    PLANE(A_Z, vv->win[2]);
    if (offset) {
        float m = GL_MAX(fabsf(t->p[A_Z][1]), fabsf(t->p[A_Z][2]));
        t->p[A_Z][0] += c->poly.offset_factor * m + c->poly.offset_units * (1.0f / 8388608);
    }
    PLANE(A_Q, q[k_]);
    /* colors: constant ones without perspective (also for flat shading) */
    const float *c0 = flat ? flat->col[side] : v0->col[side];
    int smooth = !flat && (memcmp(v0->col[side], v1->col[side], 16) || memcmp(v0->col[side], v2->col[side], 16));
    if (smooth) {
        t->flags |= TF_SMOOTH;
        for (int k = 0; k < 4; k++) PLANE(A_R + k, vv->col[side][k] * 255 * q[k_]);
    } else {
        for (int k = 0; k < 4; k++) { t->p[A_R + k][0] = c0[k] * 255; t->p[A_R + k][1] = t->p[A_R + k][2] = 0; }
    }
    if (rs->flags & RS_SPECULAR) {
        const float *s0 = flat ? flat->spec[side] : v0->spec[side];
        int ssmooth = !flat && (memcmp(v0->spec[side], v1->spec[side], 12) || memcmp(v0->spec[side], v2->spec[side], 12));
        if (ssmooth) {
            t->flags |= TF_SPEC_SMOOTH;
            for (int k = 0; k < 3; k++) PLANE(A_SR + k, vv->spec[side][k] * 255 * q[k_]);
        } else {
            for (int k = 0; k < 3; k++) { t->p[A_SR + k][0] = s0[k] * 255; t->p[A_SR + k][1] = t->p[A_SR + k][2] = 0; }
        }
    }
    for (int u = 0; u < MAX_UNITS; u++) {
        if (!(rs->flags & (RS_TEX0 << u))) continue;
        int base = u ? A_S1 : A_S0;
        PLANE(base, vv->tex[u][0] * q[k_]);
        PLANE(base + 1, vv->tex[u][1] * q[k_]);
        if (v0->tex[u][3] != 1 || v1->tex[u][3] != 1 || v2->tex[u][3] != 1) {
            t->flags |= u ? TF_TEXQ1 : TF_TEXQ0;
            PLANE(base + 2, vv->tex[u][3] * q[k_]);
        }
    }
    if (rs->flags & RS_FOG) PLANE(A_FOG, vv->fog * q[k_]);
#undef PLANE
    gl_bin_tri(c, t);
    c->tris_frame++;
}

/* ---------------- lines and points: quads ---------------- */
static void quad(gl_context *c, gl_vertex q[4], const gl_vertex *flat, int side, int offset) {
    setup_tri(c, &q[0], &q[1], &q[2], side, flat, offset);
    setup_tri(c, &q[0], &q[2], &q[3], side, flat, offset);
}

/* a line between two vertices with window coordinates: a parallelogram one
 * width across, displaced along the minor axis (as GL's wide lines) */
static void line_win(gl_context *c, gl_vertex *a, gl_vertex *b, const gl_vertex *flat, int offset) {
    float dx = b->win[0] - a->win[0], dy = b->win[1] - a->win[1];
    float hw = GL_MAX(c->line_width, 1.0f) * 0.5f;
    gl_vertex q[4] = { *a, *a, *b, *b };
    if (fabsf(dx) >= fabsf(dy)) {
        q[0].win[1] -= hw; q[1].win[1] += hw; q[2].win[1] += hw; q[3].win[1] -= hw;
    } else {
        q[0].win[0] -= hw; q[1].win[0] += hw; q[2].win[0] += hw; q[3].win[0] -= hw;
    }
    quad(c, q, flat, 0, offset);
}

static void point_win(gl_context *c, gl_vertex *a, int offset) {
    float h = GL_MAX(c->point_size, 1.0f) * 0.5f;
    gl_vertex q[4] = { *a, *a, *a, *a };
    q[0].win[0] -= h; q[0].win[1] -= h;
    q[1].win[0] += h; q[1].win[1] -= h;
    q[2].win[0] += h; q[2].win[1] += h;
    q[3].win[0] -= h; q[3].win[1] += h;
    quad(c, q, NULL, 0, offset);
}

/* ---------------- clipping ---------------- */
static float plane_dist(gl_context *c, const gl_vertex *v, int p) {
    const float *cl = v->clip;
    switch (p) {
    case 0: return cl[3] + cl[0];
    case 1: return cl[3] - cl[0];
    case 2: return cl[3] + cl[1];
    case 3: return cl[3] - cl[1];
    case 4: return cl[3] + cl[2];
    case 5: return cl[3] - cl[2];
    }
    const float *pl = c->xf.clip_plane[p - 6];
    return pl[0] * v->eye[0] + pl[1] * v->eye[1] + pl[2] * v->eye[2] + pl[3] * v->eye[3];
}

static void lerp_vertex(gl_vertex *o, const gl_vertex *a, const gl_vertex *b, float t) {
    const float *fa = (const float *)a, *fb = (const float *)b;
    float *fo = (float *)o;
    for (unsigned k = 0; k < GLV_LERP; k++) fo[k] = fa[k] + (fb[k] - fa[k]) * t;
    o->flags = 0;
    o->clipmask = 0;
    o->edge = a->edge;
}

#define MAX_CLIP_VERTS (3 + 12)
static void draw_polygon_clipped(gl_context *c, gl_vertex **pv, int n, unsigned mask, const gl_vertex *flat, int side_hint);

/* line in clip space: Liang-Barsky against the planes in mask */
static int clip_line(gl_context *c, const gl_vertex *a, const gl_vertex *b, unsigned mask, gl_vertex *oa, gl_vertex *ob) {
    float t0 = 0, t1 = 1;
    for (int p = 0; p < 12; p++) {
        if (!(mask & (1u << p))) continue;
        float da = plane_dist(c, a, p), db = plane_dist(c, b, p);
        if (da < 0 && db < 0) return 0;
        if (da < 0) { float t = da / (da - db); if (t > t0) t0 = t; }
        else if (db < 0) { float t = da / (da - db); if (t < t1) t1 = t; }
    }
    if (t0 > t1) return 0;
    if (t0 > 0) lerp_vertex(oa, a, b, t0); else *oa = *a;
    if (t1 < 1) lerp_vertex(ob, a, b, t1); else *ob = *b;
    return 1;
}

static void draw_line(gl_context *c, gl_vertex *a, gl_vertex *b, int offset) {
    unsigned cm = a->clipmask | b->clipmask;
    if (a->clipmask & b->clipmask) return;
    gl_vertex ca, cb;
    if (cm) {
        if (!clip_line(c, a, b, cm, &ca, &cb)) return;
        a = &ca; b = &cb;
    }
    gl_compute_win(c, a);
    gl_compute_win(c, b);
    const gl_vertex *flat = c->lt.shade_model == GL_FLAT ? b : NULL;
    line_win(c, a, b, flat, offset);
}

static void draw_point(gl_context *c, gl_vertex *a, int offset) {
    if (a->clipmask) return;
    gl_compute_win(c, a);
    point_win(c, a, offset);
}

/* facing of a triangle in window coordinates: 1 front */
static int front_facing(gl_context *c, const gl_vertex *a, const gl_vertex *b, const gl_vertex *d) {
    float area = (b->win[0] - a->win[0]) * (d->win[1] - a->win[1]) - (b->win[1] - a->win[1]) * (d->win[0] - a->win[0]);
    /* window y points down here: counter-clockwise on screen is a negative area */
    return c->poly.front == GL_CCW ? area < 0 : area > 0;
}

/* a triangle of processed vertices; edges: bit 0 for a-b, 1 b-c, 2 c-a (polygon modes) */
static void draw_triangle(gl_context *c, gl_vertex *a, gl_vertex *b, gl_vertex *d, const gl_vertex *provoking, unsigned edges) {
    if (a->clipmask & b->clipmask & d->clipmask) return;
    const gl_vertex *flat = c->lt.shade_model == GL_FLAT ? provoking : NULL;
    unsigned any = a->clipmask | b->clipmask | d->clipmask;
    int fill_both = c->poly.mode[0] == GL_FILL && c->poly.mode[1] == GL_FILL;
    if (any && fill_both) {                /* clip the polygon, then fill it */
        gl_vertex *pv[3] = { a, b, d };
        draw_polygon_clipped(c, pv, 3, any, flat, 0);
        return;
    }
    if (any) {
        /* polygon modes with clipping: the facing from the unclipped vertices
         * where they are in front of the eye, else from a clipped copy */
        gl_vertex *pv[3] = { a, b, d };
        if ((a->clipmask | b->clipmask | d->clipmask) & 16) { draw_polygon_clipped(c, pv, 3, any, flat, 0); return; }
    }
    gl_compute_win(c, a);
    gl_compute_win(c, b);
    gl_compute_win(c, d);
    int front = front_facing(c, a, b, d);
    if (c->en.cull_face) {
        if (c->poly.cull == GL_FRONT_AND_BACK) return;
        if ((c->poly.cull == GL_BACK) == !front) return;
    }
    int side = c->lt.two_side && c->en.lighting && !front ? 1 : 0;
    GLenum mode = c->poly.mode[front ? 0 : 1];
    if (mode == GL_FILL) {
        setup_tri(c, a, b, d, side, flat, c->en.offset_fill);
        return;
    }
    /* outlines and points: the colors of the chosen side */
    gl_vertex *vs[3] = { a, b, d };
    gl_vertex cp[3];
    for (int k = 0; k < 3; k++) {
        cp[k] = *vs[k];
        if (side) { memcpy(cp[k].col[0], cp[k].col[1], 16); memcpy(cp[k].spec[0], cp[k].spec[1], 16); }
        if (flat) { memcpy(cp[k].col[0], flat->col[side], 16); memcpy(cp[k].spec[0], flat->spec[side], 16); }
    }
    if (mode == GL_LINE) {
        for (int e = 0; e < 3; e++) {
            if (!(edges & (1u << e))) continue;
            gl_vertex *p = &cp[e], *q = &cp[e == 2 ? 0 : e + 1];
            if (p->clipmask | q->clipmask) {
                p->flags &= ~(unsigned)GLV_WIN;
                q->flags &= ~(unsigned)GLV_WIN;
                draw_line(c, p, q, c->en.offset_line);
            } else line_win(c, p, q, NULL, c->en.offset_line);
        }
    } else {
        for (int k = 0; k < 3; k++) {
            if (cp[k].clipmask) continue;
            point_win(c, &cp[k], c->en.offset_point);
        }
    }
}

/* Sutherland-Hodgman against each plane in mask, then a fan */
static void draw_polygon_clipped(gl_context *c, gl_vertex **pv, int n, unsigned mask, const gl_vertex *flat, int side_hint) {
    gl_vertex store[MAX_CLIP_VERTS * 2];
    int used = 0;
    gl_vertex *buf_a[MAX_CLIP_VERTS + 2], *buf_b[MAX_CLIP_VERTS + 2];
    gl_vertex **in = buf_a, **out = buf_b;
    for (int i = 0; i < n; i++) in[i] = pv[i];
    gl_vertex flat_copy;
    if (flat) { flat_copy = *flat; flat = &flat_copy; }
    for (int p = 0; p < 12 && n >= 3; p++) {
        if (!(mask & (1u << p))) continue;
        int m = 0;
        for (int i = 0; i < n; i++) {
            gl_vertex *a = in[i], *b = in[i + 1 == n ? 0 : i + 1];
            float da = plane_dist(c, a, p), db = plane_dist(c, b, p);
            if (da >= 0) out[m++] = a;
            if ((da >= 0) != (db >= 0) && used < MAX_CLIP_VERTS * 2 && m < MAX_CLIP_VERTS) {
                gl_vertex *nv = &store[used++];
                /* interpolate from the inside vertex, so shared edges clip the same way */
                if (da >= 0) lerp_vertex(nv, a, b, da / (da - db));
                else lerp_vertex(nv, b, a, db / (db - da));
                out[m++] = nv;
            }
            if (m >= MAX_CLIP_VERTS) break;
        }
        gl_vertex **t = in; in = out; out = t;
        n = m;
    }
    if (n < 3) return;
    for (int i = 0; i < n; i++) gl_compute_win(c, in[i]);
    /* the facing of the whole clipped polygon */
    float area = 0;
    for (int i = 0; i < n; i++) {
        const gl_vertex *a = in[i], *b = in[i + 1 == n ? 0 : i + 1];
        area += a->win[0] * b->win[1] - b->win[0] * a->win[1];
    }
    if (area == 0) return;
    int front = c->poly.front == GL_CCW ? area < 0 : area > 0;
    (void)side_hint;
    if (c->en.cull_face) {
        if (c->poly.cull == GL_FRONT_AND_BACK) return;
        if ((c->poly.cull == GL_BACK) == !front) return;
    }
    int side = c->lt.two_side && c->en.lighting && !front ? 1 : 0;
    GLenum mode = c->poly.mode[front ? 0 : 1];
    if (mode == GL_FILL) {
        for (int i = 1; i + 1 < n; i++) setup_tri(c, in[0], in[i], in[i + 1], side, flat, c->en.offset_fill);
        return;
    }
    for (int i = 0; i < n; i++) {           /* (clipped outlines: every edge is drawn) */
        gl_vertex a = *in[i], b = *in[i + 1 == n ? 0 : i + 1];
        if (side) { memcpy(a.col[0], a.col[1], 16); memcpy(b.col[0], b.col[1], 16); }
        if (mode == GL_LINE) line_win(c, &a, &b, NULL, c->en.offset_line);
        else point_win(c, &a, c->en.offset_point);
    }
}

/* ---------------- assembly ---------------- */
void gl_draw(gl_context *c, GLenum mode, gl_vertex *v, const unsigned *idx, int n) {
#define V(i) (&v[idx ? idx[i] : (unsigned)(i)])
    c->frame_dirty = 1;
    int i;
    switch (mode) {
    case GL_POINTS:
        for (i = 0; i < n; i++) draw_point(c, V(i), c->en.offset_point);
        break;
    case GL_LINES:
        for (i = 0; i + 1 < n; i += 2) draw_line(c, V(i), V(i + 1), c->en.offset_line);
        break;
    case GL_LINE_STRIP:
    case GL_LINE_LOOP:
        for (i = 0; i + 1 < n; i++) draw_line(c, V(i), V(i + 1), c->en.offset_line);
        if (mode == GL_LINE_LOOP && n > 2) draw_line(c, V(n - 1), V(0), c->en.offset_line);
        break;
    case GL_TRIANGLES:
        for (i = 0; i + 2 < n; i += 3) draw_triangle(c, V(i), V(i + 1), V(i + 2), V(i + 2), (V(i)->edge ? 1 : 0) + (V(i + 1)->edge ? 2 : 0) + (V(i + 2)->edge ? 4 : 0));
        break;
    case GL_TRIANGLE_STRIP:
        for (i = 0; i + 2 < n; i++) {
            if (i & 1) draw_triangle(c, V(i + 1), V(i), V(i + 2), V(i + 2), 7);
            else draw_triangle(c, V(i), V(i + 1), V(i + 2), V(i + 2), 7);
        }
        break;
    case GL_TRIANGLE_FAN:
        for (i = 1; i + 1 < n; i++) draw_triangle(c, V(0), V(i), V(i + 1), V(i + 1), 7);
        break;
    case GL_QUADS:
        for (i = 0; i + 3 < n; i += 4) {
            /* two triangles; the diagonal is no edge of the quad */
            unsigned e0 = (V(i)->edge ? 1 : 0) | (V(i + 1)->edge ? 2 : 0);
            unsigned e1 = (V(i + 2)->edge ? 2 : 0) | (V(i + 3)->edge ? 4 : 0);
            draw_triangle(c, V(i), V(i + 1), V(i + 2), V(i + 3), e0);
            draw_triangle(c, V(i), V(i + 2), V(i + 3), V(i + 3), e1);
        }
        break;
    case GL_QUAD_STRIP:
        for (i = 0; i + 3 < n; i += 2) {
            draw_triangle(c, V(i), V(i + 1), V(i + 3), V(i + 3), 3);
            draw_triangle(c, V(i), V(i + 3), V(i + 2), V(i + 3), 6);
        }
        break;
    case GL_POLYGON:
        for (i = 1; i + 1 < n; i++) {
            unsigned e = (V(i)->edge ? 2 : 0);
            if (i == 1 && V(0)->edge) e |= 1;
            if (i + 2 == n && V(n - 1)->edge) e |= 4;
            draw_triangle(c, V(0), V(i), V(i + 1), V(0), e);
        }
        break;
    }
#undef V
}
