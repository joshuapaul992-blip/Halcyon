/* Halcyon's window binding for the 3D library (like WGL on Windows or GLX
 * on X11): a context draws straight into a window's canvas (the pixels M9
 * maps into the program), so a frame costs no copy.
 *
 *   int win = win_create("Demo", 640, 480, WIN_RESIZABLE);
 *   HGLRC rc = hglCreateContext(win, 0);
 *   hglMakeCurrent(rc);
 *   for (;;) {
 *       ... events; on EVT_RESIZE: hglResize(rc); glViewport(...) ...
 *       glClear(...); draw ...
 *       hglSwapBuffers(rc);         // the frame is finished and shown
 *   }
 *   hglDeleteContext(rc);
 *
 * The context's color buffer is the canvas itself (one buffer: drawing
 * happens when the frame is finished, tile by tile, then the window is told
 * that its canvas changed). The depth buffer belongs to the context. Calls
 * go to the current context of the program (one at a time; threads share
 * it). For tests and off-screen work a context can also draw into memory. */
#ifndef HAL_HGL_H
#define HAL_HGL_H
#include "gl.h"

typedef struct gl_context *HGLRC;

#define HGL_NO_DEPTH 0x01            /* no depth buffer (2D drawing only) */

HGLRC hglCreateContext(int win, unsigned flags);     /* NULL: no canvas or no memory */
int   hglMakeCurrent(HGLRC rc);                      /* NULL: none current; 1 on success */
HGLRC hglGetCurrentContext(void);
void  hglDeleteContext(HGLRC rc);
/* after EVT_RESIZE: maps the canvas again, resizes the depth buffer;
 * the viewport is not changed (call glViewport). 1 on success */
int   hglResize(HGLRC rc);
/* finish the frame (draw what was queued) and show it (the window repaints) */
void  hglSwapBuffers(HGLRC rc);
/* finish the frame without telling the window (screen savers: their frame
 * is shown by saver.h), then the rectangle that changed */
void  hglFinishFrame(HGLRC rc);
/* frames finished, and frames per second over the last second (0 at first) */
unsigned hglFrameCount(HGLRC rc);
float hglFramesPerSecond(HGLRC rc);

/* ---- drawing into memory (tests, off-screen pictures) ---- */
/* a context with its own 0x00RRGGBB color buffer of w x h pixels */
HGLRC hglCreateMemoryContext(int w, int h, unsigned flags);
/* a context that draws into the caller's pixels (stride in pixels) */
HGLRC hglCreateBufferContext(unsigned *pixels, int w, int h, int stride, unsigned flags);
/* the color buffer (row 0 at the top, as on screen) */
unsigned *hglGetPixels(HGLRC rc, int *w, int *h, int *stride);
/* point a buffer context at other pixels (or a new size) */
int   hglSetBuffer(HGLRC rc, unsigned *pixels, int w, int h, int stride);

#endif
