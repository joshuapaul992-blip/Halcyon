/* Halcyon's 3D library: the window binding (GL/hgl.h), like WGL or GLX.
 * A context's color buffer is the window's canvas, mapped into the program
 * by M9's win_canvas: the tile renderer draws into it directly, and a swap
 * only tells the window manager which rectangle changed (no copy per
 * frame). The depth buffer is the context's own. After EVT_RESIZE the
 * canvas is mapped again (the kernel keeps the old one alive until then,
 * so drawing during a resize never touches freed memory). */
#include "glint.h"

HGLRC hglCreateContext(int win, unsigned flags) {
    hal_canvas_t cv;
    if (win < 0 || win_canvas(win, &cv) != 0 || !cv.pixels) return NULL;
    gl_context *c = gl_context_new(cv.pixels, cv.w, cv.h, cv.stride, flags);
    if (!c) return NULL;
    c->win = win;
    return c;
}

int hglResize(HGLRC c) {
    if (!c || c->win < 0) return 0;
    hal_canvas_t cv;
    if (win_canvas(c->win, &cv) != 0 || !cv.pixels) return 0;
    return gl_set_framebuffer(c, cv.pixels, cv.w, cv.h, cv.stride);
}

void hglSwapBuffers(HGLRC c) {
    if (!c) return;
    gl_render(c);
    int x0 = c->dirty_x0, y0 = c->dirty_y0, x1 = c->dirty_x1, y1 = c->dirty_y1;
    gl_frame_done(c);
    /* only the pixels the frame touched (a whole-window scene: all of it) */
    if (c->win >= 0 && x1 > x0 && y1 > y0) win_invalidate_rect(c->win, x0, y0, x1 - x0, y1 - y0);
}
