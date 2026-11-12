/* Halcyon OS - monitor arrangement math (docs/DESIGN-M10-DISPLAY.md).
 *
 * The desktop is one coordinate space; each monitor shows a rectangle of
 * it. Monitors never overlap and every one touches another along an edge
 * (an arrangement Display Properties can draw and the mouse can cross).
 * Pure functions on arrays of rectangles: used by the display core
 * (arrangement from SETTINGS.INI), the window manager (pointer, window
 * places, maximize) and Display Properties (dragging monitors), and tested
 * on the host (tests/host/layouttest.c). */
#ifndef LAYOUT_H
#define LAYOUT_H
#include "rect.h"

#define LAYOUT_MAX 16

/* two rectangles share an edge segment of positive length */
int  layout_adjacent(rect_t a, rect_t b);
/* index of the monitor containing (x, y), or -1 */
int  layout_at(const rect_t *m, int n, int x, int y);
/* the point of the monitors' union nearest to (x, y) (the pointer stays on
 * a monitor); returns that monitor */
int  layout_clamp_point(const rect_t *m, int n, int *x, int *y);
/* the monitor showing most of r; if none shows any of it, the one nearest
 * to r's centre */
int  layout_best(const rect_t *m, int n, rect_t r);
/* Place monitor i next to the others (which stay where they are): its own
 * place if it touches one of them without overlapping any, else the free
 * place along their edges nearest to it. */
void layout_snap(rect_t *m, int n, int i);
/* Make an arrangement valid: m[first] stays, the others are snapped in
 * order of their distance from it. Then the union's top left corner moves
 * to (0, 0); *dx / *dy (may be NULL) get the shift applied. */
void layout_normalize(rect_t *m, int n, int first, int *dx, int *dy);
/* the bounding box of the monitors */
rect_t layout_bounds(const rect_t *m, int n);
/* Put a window frame f on the work areas wa[0..n): onto the work area that
 * shows most of it (wa[prefer] when none shows any, or when prefer >= 0
 * and force is set), moved (never resized, unless it is larger than the
 * work area) so that it lies inside it. */
rect_t layout_fit(const rect_t *wa, int n, rect_t f, int prefer, int force);
/* is enough of f's top strip (title bar, height th) on some monitor that
 * the window can be grabbed? (at least min_w pixels wide) */
int  layout_grabbable(const rect_t *m, int n, rect_t f, int th, int min_w);

#endif
