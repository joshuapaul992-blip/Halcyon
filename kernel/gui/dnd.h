/* Halcyon OS - drag and drop between windows of any program and the desktop
 * (gui/dnd.c; see docs/DESIGN-M8-EXPLORER.md).
 *
 * A source starts a drag of a list of paths while a mouse button is held
 * (dnd_begin; programs: SYS_DND_BEGIN). From then on the window manager
 * hands the pointer to this module: it draws the drag image (the first
 * item's icon and the number of items) at the pointer, finds the target
 * under it - a window with on_dnd, or the desktop - and sends it
 * DND_ENTER / DND_OVER / DND_LEAVE with the position and the modifier keys.
 * The target answers with the effect a drop would have there (DND_COPY,
 * DND_MOVE or DND_NONE) and every effect it could have; the badge next to
 * the pointer follows (+ copy, nothing move, a no-entry sign none). Kernel
 * targets answer at once; programs answer later (SYS_DND_ACCEPT), and until
 * they do nothing can be dropped there. Releasing the button sends
 * DND_DROP with the effect (the right button first asks with a "Copy Here /
 * Move Here / Cancel" menu), Esc or the other button cancels; the source
 * gets DND_END with the effect either way. The paths stay readable
 * (dnd_paths, SYS_DND_DATA) until the next drag starts. */
#ifndef DND_H
#define DND_H
#include "wm.h"
#include "core/sysabi.h"

typedef struct dnd_event {
    int type;                /* DND_ENTER .. DND_END */
    int x, y;                /* a window: client coordinates; the desktop: screen coordinates */
    int mods;                /* MOD_* */
    int button;              /* MOUSE_LEFT or MOUSE_RIGHT: the button that drags */
    u32 serial;
    int count, allowed;      /* paths; effects the source allows */
    int effect;              /* DND_DROP: the effect chosen; DND_END: the drop's effect, 0 = none */
} dnd_event_t;

/* what on_dnd returns for DND_ENTER / DND_OVER */
#define DND_ANSWER(effect, possible) ((effect) | ((possible) << 8))
#define DND_LATER (-1)       /* the answer comes through dnd_answer */

/* start a drag from window src (NULL: a kernel source without a window) of
 * count NUL-separated paths (len bytes); icon: ICON_* of the image. Needs a
 * mouse button held. Returns the serial number, or E_AGAIN / E_BUSY / E_NOMEM */
int  dnd_begin(window_t *src, const char *paths, int len, int count, int allowed, int icon);
int  dnd_active(void);
/* the paths of drag serial (the current one or the last dropped), NUL-separated; NULL if gone */
const char *dnd_paths(u32 serial, int *len, int *count);
/* a target's late answer (programs) */
void dnd_answer(window_t *w, u32 serial, int effect, int possible);
/* move the pointer of a drag to screen x, y and drop there (test hooks) */
void dnd_test_move(int x, int y);
void dnd_test_drop(void);

/* window manager hooks (gui/wm.c) */
void dnd_pointer(int x, int y, int old_buttons, int buttons);
int  dnd_key(const input_event_t *k);         /* 1: consumed */
void dnd_paint(gc_t *g, rect_t clip);
void dnd_window_gone(window_t *w);
void dnd_tick(void);

/* the desktop as a target (gui/desktop.c) */
int  desktop_dnd(dnd_event_t *e);
window_t *taskbar_window_at(int x, int y);     /* the window of the task button there, or NULL */

#endif
