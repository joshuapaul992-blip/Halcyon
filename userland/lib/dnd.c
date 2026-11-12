/* Halcyon user-mode library: drag-and-drop helpers (see hal.h) */
#include "hal.h"

static int ext_ok(const char *path, const char *exts) {
    if (!exts) return 1;
    const char *slash = strrchr(path, '/'), *dot = strrchr(path, '.');
    if (!dot || (slash && dot < slash)) return 0;
    dot++;
    size_t el = strlen(dot);
    for (const char *e = exts; *e;) {
        const char *end = strchr(e, ';');
        size_t l = end ? (size_t)(end - e) : strlen(e);
        if (l == el) {
            size_t i = 0;
            while (i < l && tolower((unsigned char)e[i]) == tolower((unsigned char)dot[i])) i++;
            if (i == l) return 1;
        }
        if (!end) break;
        e = end + 1;
    }
    return 0;
}

/* the first path of a drag if it is a file of the right type: 0, else -1 */
static int first_path(int serial, const char *exts, char *path, int pathsz) {
    char buf[1024];
    int n = dnd_data(serial, buf, (int)sizeof buf - 1);
    if (n <= 0) return -1;
    buf[n < (int)sizeof buf - 1 ? n : (int)sizeof buf - 1] = 0;
    if ((int)strlen(buf) >= pathsz || !ext_ok(buf, exts)) return -1;
    hal_dirent_t st;
    if (stat(buf, &st) < 0 || st.is_dir) return -1;
    strlcpy(path, buf, (size_t)pathsz);
    return 0;
}

int dnd_open_target(const hal_event_t *ev, const char *exts, char *path, int pathsz) {
    if (ev->type != EVT_DRAG) return 0;
    char tmp[520];
    if (ev->button == DND_ENTER || ev->button == DND_OVER) {
        int ok = ev->dz == 1 && first_path(ev->id, exts, tmp, (int)sizeof tmp) == 0;
        dnd_accept(ev->win, ev->id, ok ? DND_COPY : DND_NONE, ok ? DND_COPY : 0);
        return 0;
    }
    if (ev->button == DND_DROP && ev->pressed) return first_path(ev->id, exts, path, pathsz) == 0;
    return 0;
}
