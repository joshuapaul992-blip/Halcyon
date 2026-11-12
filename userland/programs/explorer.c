/* EXPLORER.EXE - My Computer and the folder browser / file manager.
 *
 * The user-mode port of the kernel's Explorer (kernel/apps/explorer.c, with
 * the parts of fileops.c and progress.c it uses): My Computer with the
 * drives, folder views (icons, details), the task pane, Back / Up and the
 * address bar, selection, keyboard navigation and type-to-select, context
 * menus, rename, new folder / text document, delete, cut / copy / paste,
 * Properties, Format and Eject for drives, and a refresh when drives come
 * and go. Copies, moves and deletes run in a worker thread behind a
 * progress window with Cancel, so this window keeps painting meanwhile.
 *
 * Milestone 8 (docs/DESIGN-M8-EXPLORER.md): multiple selection (Ctrl and
 * Shift clicks, a rubber band, Ctrl+A, Shift+arrows, Ctrl+Space) with every
 * command working on the selection; drag and drop through the window
 * manager (SYS_DND_*: this window is a source and a target, to other
 * Explorers, the desktop, Notepad, Paint, Media Player); the Recycle Bin
 * (deletes go there, Shift+Delete and removable drives delete for good; its
 * own view lists, restores, deletes and empties; Properties sets each
 * drive's limit); sorting by column; and the view, sort order and task pane
 * kept in SETTINGS.INI (explorer.*).
 *
 * Usage: EXPLORER.EXE ["C:/folder" | "Recycle Bin"]   (no argument or "" = My Computer)
 *        EXPLORER.EXE -rbprops                         (only the Recycle Bin's Properties)
 *
 * The Explorer clipboard (Cut / Copy of items) is shared memory, so every
 * Explorer window sees it, as with the built-in version. */
#include "hal.h"

#define MAX_PATH 520
#define MAX_NAME 255
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define E_NODEV -15
#define ATTR_READONLY 0x01
#define ATTR_HIDDEN   0x02
#define ATTR_SYSTEM   0x04
#define ATTR_VOLUME   0x08
#define ATTR_ARCHIVE  0x20
#define ICON16(i) ((i) | (1 << 16))
#define ICON48(i) ((i) | (2 << 16))

/* layout (canvas coordinates: the menu bar is above the canvas) */
#define TOOLBAR_H 38
#define ADDR_H 28
#define STATUS_H 22
#define PANE_W 196
#define CELL_W 92
#define CELL_H 76
#define TILE_W 250
#define TILE_H 60
#define ROW_H 19
#define SB_W 17
#define TOP_Y (TOOLBAR_H + ADDR_H)

typedef struct item {
    char name[MAX_NAME + 1];
    int is_dir, icon;
    unsigned long long size;
    unsigned short year; unsigned char month, day, hour, minute, second;
    /* drives (My Computer) */
    char letter, kind, is_boot, readonly;
    char fs[8], device[16];
    unsigned long long total, free;
    unsigned vflags;                /* VOLF_* */
    unsigned char selected;
    /* the Recycle Bin: deleted when (year...), from where */
    unsigned rbid;
    char *orig;                     /* the folder it came from (malloc()ed) */
} item_t;

typedef struct link { hal_rect_t r; int id; } link_t;
#define MAX_LINKS 16

enum { CMD_NEWFOLDER = 1, CMD_NEWTEXT, CMD_RENAME, CMD_DELETE, CMD_COPY, CMD_CUT, CMD_PASTE, CMD_PROPS, CMD_CLOSE,
       CMD_OPEN, CMD_VIEW_ICONS, CMD_VIEW_DETAILS, CMD_REFRESH, CMD_BACK, CMD_UP, CMD_MYCOMP, CMD_MYDOCS,
       CMD_TERMINAL, CMD_SYSINFO, CMD_CONTROL, CMD_OPEN_NOTEPAD, CMD_ABOUT, CMD_FORMAT, CMD_EJECT,
       CMD_SELECT_ALL, CMD_INVERT, CMD_PANE, CMD_SORT0, CMD_SORT1, CMD_SORT2, CMD_SORT3, CMD_SORT_DESC,
       CMD_DELETE_PERM, CMD_RECYCLEBIN, CMD_RESTORE, CMD_RESTORE_ALL, CMD_EMPTY, CMD_RBPROPS,
       CMD_DROP_COPY, CMD_DROP_MOVE };
enum { ID_ADDR = 100, ID_GO, ID_VSB };

/* ---------------- state ---------------- */
static int win, cw = 700, ch = 460;
static char path[MAX_PATH];                 /* "" = My Computer (or the Recycle Bin: binview) */
static int binview;                         /* the Recycle Bin */
static item_t *items;
static int n, hover = -1, scroll, view;     /* view 0 icons, 1 details */
static int focus = -1, anchor = -1, nsel;   /* the keyboard's item, the start of Shift ranges, selected items */
static int sort_col, sort_desc, pane_on = 1;   /* saved: explorer.sort, explorer.sortdesc, explorer.pane */
static char back[16][MAX_PATH];
static int nback;
static unsigned vgen;
static unsigned long long disk_free;        /* of the folder's drive, at load time */
static link_t links[MAX_LINKS];
static int nlinks, link_hover = -1, tb_hover = -1, cursor = -1;
static int quit;                            /* the window was closed while busy */
static hal_theme_t th;
static int view_x, view_y, view_w, view_h;

/* the Explorer clipboard, shared by every Explorer window: NUL-separated paths */
#define CLIP_BYTES (60 * 1024)
typedef struct clip { volatile int cut, count; volatile unsigned seq; char path[CLIP_BYTES]; } clip_t;
static clip_t local_clip, *clip = &local_clip;

static void paint(void);
static void load(void);

/* ---------------- small helpers ---------------- */
static unsigned mix(unsigned a, unsigned b, int t) {
    unsigned ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    unsigned br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return (((ar * (255 - t) + br * t) / 255) << 16) | (((ag * (255 - t) + bg * t) / 255) << 8) | ((ab * (255 - t) + bb * t) / 255);
}
static unsigned lighten(unsigned c, int amt) { return amt >= 0 ? mix(c, 0xFFFFFF, amt) : mix(c, 0x000000, -amt); }
static void hline(int x, int y, int w, unsigned c) { if (w > 0) gfx_line(win, x, y, x + w - 1, y, c); }
static void vline(int x, int y, int h, unsigned c) { if (h > 0) gfx_line(win, x, y, x, y + h - 1, c); }
static void rounded(int x, int y, int w, int h, int rad, int corners, unsigned top, unsigned bot) {
    gfx_shade(win, x, y, w, h, top, bot, SHADE_ROUNDED, rad | (corners & 15) << 8);
}
static void rounded_outline(int x, int y, int w, int h, int rad, int corners, unsigned c) {
    gfx_shade(win, x, y, w, h, c, 0, SHADE_ROUNDED_OUTLINE, rad | (corners & 15) << 8);
}
/* a small padlock (14 x 17) over a read-only drive's icon */
static void draw_lock(int x, int y) {
    rounded_outline(x + 3, y, 8, 12, 4, 15, 0x505050);
    rounded_outline(x + 4, y + 1, 6, 10, 3, 15, 0x909090);
    rounded(x, y + 7, 14, 10, 2, 15, 0xFBE07A, 0xC8901C);
    rounded_outline(x, y + 7, 14, 10, 2, 15, 0x8A6408);
    gfx_fill(win, x + 6, y + 10, 2, 4, 0x5A4004);
}
static int in_rect(int x, int y, int w, int h, int px, int py) { return px >= x && py >= y && px < x + w && py < y + h; }

static const char *errstr(int e) {
    switch (e) {
    case 0: return "Success";
    case E_IO: return "I/O error";
    case E_NOMEM: return "Out of memory";
    case E_NOENT: return "File or directory not found";
    case E_EXIST: return "Already exists";
    case E_NOTDIR: return "Not a directory";
    case E_ISDIR: return "Is a directory";
    case E_NOSPC: return "Disk is full";
    case E_INVAL: return "Invalid argument";
    case E_TIMEOUT: return "Timed out";
    case -10: return "Device stalled";
    case E_NOTSUP: return "Not supported";
    case E_AGAIN: return "Try again";
    case E_NOTEMPTY: return "Directory is not empty";
    case E_ROFS: return "Read-only file system";
    case E_NODEV: return "No such device";
    case E_BUSY: return "Device busy";
    case -17: return "Name too long";
    case E_CANCELED: return "Canceled";
    case E_PERM: return "Access denied";
    }
    return "Unknown error";
}

/* ---------------- paths (as the kernel's vfs_normalize / join / dirname) ---------------- */
static int path_normalize(const char *in, char *out, int outsz) {
    if (!isalpha((unsigned char)in[0]) || in[1] != ':' || outsz < 4) return E_INVAL;
    int o = 0;
    out[o++] = (char)toupper((unsigned char)in[0]);
    out[o++] = ':';
    const char *s = in + 2;
    for (;;) {
        while (*s == '/' || *s == '\\') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != '/' && *e != '\\') e++;
        int len = (int)(e - s);
        if (len == 1 && s[0] == '.') {
        } else if (len == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 2 && out[o - 1] != '/') o--;
            if (o > 2) o--;
        } else {
            if (len > MAX_NAME || o + 1 + len + 1 > outsz) return E_INVAL;
            out[o++] = '/';
            memcpy(out + o, s, (size_t)len);
            o += len;
        }
        s = e;
    }
    if (o == 2) out[o++] = '/';
    out[o] = 0;
    return 0;
}
static void path_join(const char *dir, const char *name, char *out, int outsz) {
    strlcpy(out, dir, (size_t)outsz);
    int l = (int)strlen(out);
    if (l && out[l - 1] != '/') strlcat(out, "/", (size_t)outsz);
    strlcat(out, name, (size_t)outsz);
}
static const char *path_base(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}
static void path_dir(const char *p, char *out, int outsz) {
    strlcpy(out, p, (size_t)outsz);
    char *s = strrchr(out, '/');
    if (!s) return;
    if (s == out + 2) s[1] = 0;            /* "C:/x" -> "C:/" */
    else *s = 0;
}
static int path_is_root(const char *p) { return strlen(p) <= 3; }
static int prefix_nocase(const char *a, const char *b, size_t len) {
    for (size_t i = 0; i < len; i++) if (!b[i] || toupper((unsigned char)a[i]) != toupper((unsigned char)b[i])) return 0;
    return 1;
}

/* ---------------- Unicode-aware name order ---------------- */
/* simple uppercase mapping, as the kernel's uc_upper */
static unsigned uc_upper(unsigned c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c < 0x100) {
        if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
        if (c == 0xFF) return 0x178;
        if (c == 0xB5) return 0x39C;
        return c;
    }
    if (c <= 0x17F) {
        if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c - 1 : c;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        if (c == 0x17F) return 'S';
        return c;
    }
    if (c >= 0x370 && c <= 0x3FF) {
        if ((c >= 0x3B1 && c <= 0x3C1) || (c >= 0x3C3 && c <= 0x3CB)) return c - 32;
        if (c == 0x3C2) return 0x3A3;
        if (c == 0x3AC) return 0x386;
        if (c >= 0x3AD && c <= 0x3AF) return c - 37;
        if (c == 0x3CC) return 0x38C;
        if (c == 0x3CD || c == 0x3CE) return c - 63;
        return c;
    }
    if (c >= 0x400 && c <= 0x4FF) {
        if (c >= 0x430 && c <= 0x44F) return c - 32;
        if (c >= 0x450 && c <= 0x45F) return c - 80;
        if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x4FF)) return (c & 1) ? c - 1 : c;
        if (c >= 0x4C1 && c <= 0x4CE) return (c & 1) ? c : c - 1;
        return c;
    }
    if (c >= 0x1E00 && c <= 0x1EFF) return (c & 1) ? c - 1 : c;
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;
    return c;
}

/* the letter an accented letter sorts with (primary order): "École" next to
 * "Ecole", not after "Z"; Greek tonos and Cyrillic Ё likewise */
static unsigned fold(unsigned c) {
    c = uc_upper(c);
    if (c >= 0xC0 && c <= 0xDF) {
        static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOO\0OUUUUYTS";
        char b = latin1[c - 0xC0];
        return b ? (unsigned)b : c;
    }
    if (c >= 0x100 && c <= 0x17F) {
        static const char exta[] =
            "AAAAAACCCCCCCCDDDDEEEEEEEEEEGGGGGGGGHHHHIIIIIIIIIIIIJJKKKLLLLLLLLLLNNNNNNNNNOOOOOOOORRRRRRSSSSSSSSTTTTTTUUUUUUUUUUUUWWYYYZZZZZZS";
        return (unsigned)exta[c - 0x100];
    }
    switch (c) {
    case 0x386: return 0x391; case 0x388: return 0x395; case 0x389: return 0x397; case 0x38A: case 0x3AA: return 0x399;
    case 0x38C: return 0x39F; case 0x38E: case 0x3AB: return 0x3A5; case 0x38F: return 0x3A9;
    case 0x401: return 0x415;
    }
    return c;
}

/* numbers inside names compare by value, as in Windows XP: "File 9" before "File 10" */
static int number_cmp(const char *a, int *pa, int la, const char *b, int *pb, int lb) {
    int ea = *pa, eb = *pb;
    while (ea < la && isdigit((unsigned char)a[ea])) ea++;
    while (eb < lb && isdigit((unsigned char)b[eb])) eb++;
    int sa = *pa, sb = *pb;
    while (sa < ea - 1 && a[sa] == '0') sa++;
    while (sb < eb - 1 && b[sb] == '0') sb++;
    int r = (ea - sa) - (eb - sb);
    if (!r) r = memcmp(a + sa, b + sb, (size_t)(ea - sa));
    *pa = ea; *pb = eb;
    return r < 0 ? -1 : r > 0;
}

static int name_cmp(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    for (int pass = 0; pass < 2; pass++) {
        int pa = 0, pb = 0;
        for (;;) {
            if (pass == 0 && pa < la && pb < lb && isdigit((unsigned char)a[pa]) && isdigit((unsigned char)b[pb])) {
                int r = number_cmp(a, &pa, la, b, &pb, lb);
                if (r) return r;
                continue;
            }
            unsigned x = utf8_get(a, la, &pa), y = utf8_get(b, lb, &pb);
            if (pass == 0) { x = fold(x); y = fold(y); } else { x = uc_upper(x); y = uc_upper(y); }
            if (x != y) return x < y ? -1 : 1;
            if (!x) break;
        }
    }
    return strcmp(a, b);
}

static const char *file_type_name(const char *name, int is_dir);
static int date_cmp(const item_t *a, const item_t *b) {
    long long ka = ((long long)a->year << 40) | ((long long)a->month << 32) | ((long long)a->day << 24) | (a->hour << 16) | (a->minute << 8) | a->second;
    long long kb = ((long long)b->year << 40) | ((long long)b->month << 32) | ((long long)b->day << 24) | (b->hour << 16) | (b->minute << 8) | b->second;
    return ka < kb ? -1 : ka > kb;
}
/* by the sort column: folders: name, size, type, date modified; the Recycle
 * Bin: name, original location, date deleted, size. Folders come first. */
static int item_cmp(const item_t *a, const item_t *b) {
    if (a->is_dir != b->is_dir && !binview) return b->is_dir - a->is_dir;
    int r = 0;
    if (binview) {
        if (sort_col == 1) r = name_cmp(a->orig ? a->orig : "", b->orig ? b->orig : "");
        else if (sort_col == 2) r = date_cmp(a, b);
        else if (sort_col == 3) r = a->size < b->size ? -1 : a->size > b->size;
    } else {
        if (sort_col == 1) r = a->size < b->size ? -1 : a->size > b->size;
        else if (sort_col == 2) {
            char ta[32];        /* (file_type_name has one static buffer) */
            strlcpy(ta, file_type_name(a->name, a->is_dir), sizeof ta);
            r = name_cmp(ta, file_type_name(b->name, b->is_dir));
        }
        else if (sort_col == 3) r = date_cmp(a, b);
    }
    if (!r) r = name_cmp(a->name, b->name);
    return sort_desc ? -r : r;
}

/* merge sort: stable, n log n for big folders */
static void sort_items(item_t *it, int cnt) {
    if (cnt < 2) return;
    item_t *tmp = malloc(sizeof(item_t) * (size_t)cnt);
    if (!tmp) {
        for (int i = 1; i < cnt; i++) {
            item_t t = it[i];
            int j = i - 1;
            while (j >= 0 && item_cmp(&it[j], &t) > 0) { it[j + 1] = it[j]; j--; }
            it[j + 1] = t;
        }
        return;
    }
    for (int width = 1; width < cnt; width *= 2) {
        for (int lo = 0; lo < cnt; lo += 2 * width) {
            int mid = MIN(lo + width, cnt), hi = MIN(lo + 2 * width, cnt), i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = item_cmp(&it[j], &it[i]) < 0 ? it[j++] : it[i++];
            while (i < mid) tmp[k++] = it[i++];
            while (j < hi) tmp[k++] = it[j++];
        }
        memcpy(it, tmp, sizeof(item_t) * (size_t)cnt);
    }
    free(tmp);
}

/* ---------------- file types, sizes, times (as fileops.c) ---------------- */
static const char *ext_of(const char *name) {
    const char *d = strrchr(name, '.');
    return d ? d + 1 : "";
}
static int is_text_file(const char *name) {
    static const char *t[] = { "txt", "log", "ini", "cfg", "c", "h", "md", "bat", "asm", "s", "py", "csv", "json",
                               "xml", "htm", "html", "inf", "conf", "sh", "nfo", "me" };
    const char *e = ext_of(name);
    for (int i = 0; i < N(t); i++) if (!strcasecmp(e, t[i])) return 1;
    return 0;
}
static const char *file_type_name(const char *name, int is_dir) {
    if (is_dir) return "File Folder";
    const char *e = ext_of(name);
    if (!strcasecmp(e, "htm") || !strcasecmp(e, "html")) return "HTML Document";
    if (is_text_file(name)) return !strcasecmp(e, "ini") || !strcasecmp(e, "cfg") ? "Configuration Settings" : "Text Document";
    if (!strcasecmp(e, "bmp")) return "Bitmap Image";
    if (!strcasecmp(e, "wav")) return "Wave Sound";
    if (!strcasecmp(e, "efi")) return "EFI Application";
    if (!strcasecmp(e, "bin") || !strcasecmp(e, "sys")) return "System file";
    if (!strcasecmp(e, "exe") || !strcasecmp(e, "com")) return "Application";
    if (!strcasecmp(e, "img") || !strcasecmp(e, "iso")) return "Disk Image";
    if (!strcasecmp(e, "zip")) return "Compressed Folder";
    static char buf[24];
    if (*e && strlen(e) <= 8) {
        int k = 0;
        for (const char *p = e; *p; p++) buf[k++] = (char)toupper((unsigned char)*p);
        strlcpy(buf + k, " File", sizeof buf - (size_t)k);
        return buf;
    }
    return "File";
}
static int file_icon(const char *name, int is_dir) {
    if (is_dir) return HICON_FOLDER;
    const char *e = ext_of(name);
    if (!strcasecmp(e, "htm") || !strcasecmp(e, "html")) return HICON_WEB;
    if (is_text_file(name)) return HICON_FILE_TEXT;
    if (!strcasecmp(e, "bmp")) return HICON_FILE_IMAGE;
    if (!strcasecmp(e, "wav")) return HICON_FILE_AUDIO;
    if (!strcasecmp(e, "efi") || !strcasecmp(e, "exe") || !strcasecmp(e, "com")) return HICON_FILE_APP;
    if (!strcasecmp(e, "bin") || !strcasecmp(e, "sys")) return HICON_FILE_SYS;
    return HICON_FILE;
}
static void format_size(unsigned long long b, char *out, int sz) {
    if (b < 1024) snprintf(out, (size_t)sz, "%llu bytes", b);
    else if (b < 1024 * 1024) snprintf(out, (size_t)sz, "%llu KB", (b + 1023) / 1024);
    else if (b < 1024ull * 1024 * 1024) snprintf(out, (size_t)sz, "%llu.%llu MB", b >> 20, ((b & 0xFFFFF) * 10) >> 20);
    else snprintf(out, (size_t)sz, "%llu.%llu GB", b >> 30, ((b & 0x3FFFFFFF) * 10) >> 30);
}
static void format_size_exact(unsigned long long b, char *out, int sz) {
    char digits[32];
    snprintf(digits, sizeof digits, "%llu", b);
    int len = (int)strlen(digits), o = 0;
    for (int i = 0; i < len && o < sz - 8; i++) {
        out[o++] = digits[i];
        if ((len - i - 1) % 3 == 0 && i != len - 1) out[o++] = ',';
    }
    out[o] = 0;
    strlcat(out, " bytes", (size_t)sz);
}
static void format_time(int year, int month, int day, int hour, int minute, char *out, int sz) {
    if (!year) { out[0] = 0; return; }
    int h = hour % 12 == 0 ? 12 : hour % 12;
    snprintf(out, (size_t)sz, "%d/%d/%d %d:%02d %s", month, day, year, h, minute, hour < 12 ? "AM" : "PM");
}
static int drive_icon(int kind) {
    return kind == VOL_REMOVABLE ? HICON_USBDRIVE : kind == VOL_CDROM ? HICON_CDROM : kind == VOL_FLOPPY ? HICON_FLOPPY : HICON_HDD;
}
static const char *drive_kind(int kind) {
    return kind == VOL_REMOVABLE ? "Removable Disk" : kind == VOL_FLOPPY ? "Floppy Disk" : kind == VOL_CDROM ? "CD Drive" : "Local Disk";
}

/* "x (2).txt": a name not taken yet in dir */
static int unique_name(const char *dir, const char *base, char *out, int outsz) {
    hal_dirent_t st;
    path_join(dir, base, out, outsz);
    if (stat(out, &st) < 0) return 0;
    char nm[MAX_NAME + 1], ext[MAX_NAME + 1] = "";
    strlcpy(nm, base, sizeof nm);
    char *dot = strrchr(nm, '.');
    if (dot && dot != nm) { strlcpy(ext, dot, sizeof ext); *dot = 0; }
    for (int i = 2; i < 1000; i++) {
        char cand[MAX_NAME + 16];
        snprintf(cand, sizeof cand, "%s (%d)%s", nm, i, ext);
        path_join(dir, cand, out, outsz);
        if (stat(out, &st) < 0) return 0;
    }
    return E_EXIST;
}

/* a whole directory listing (malloc()ed) */
static int list_dir(const char *p, hal_dirent_t **out) {
    *out = NULL;
    int cnt = readdir(p, NULL, 0);
    if (cnt < 0) return cnt;
    int cap = cnt + 16;
    hal_dirent_t *e = malloc(sizeof(hal_dirent_t) * (size_t)cap);
    if (!e) return E_NOMEM;
    cnt = readdir(p, e, cap);
    if (cnt < 0) { free(e); return cnt; }
    *out = e;
    return cnt;
}

/* ---------------- copy / move / delete in a worker thread (as fileops.c) ---------------- */
/* An operation works on a list of items. FOP_RECYCLE moves them to the
 * Recycle Bin (per-item results: E_NOSPC / E_NOTSUP are asked about
 * afterwards), FOP_PURGE deletes bin entries for good (drive in src[i][0],
 * id in ids[i]). */
enum { FOP_COPY = 1, FOP_MOVE, FOP_DELETE, FOP_RECYCLE, FOP_PURGE };
typedef struct fileop {
    int kind, n;
    char (*src)[MAX_PATH], (*dst)[MAX_PATH];
    unsigned char *replace;             /* per item: replace what is at dst (folders merge) */
    unsigned *ids;
    int *res;                           /* per item: 0 or E_* */
    volatile int cancel, done, counting;
    int result;                         /* E_CANCELED, or the first error */
    volatile unsigned long long total_bytes, done_bytes;
    volatile unsigned total_items, done_items;
    hal_mutex_t lock;                   /* current */
    char current[MAX_NAME + 1];
    int cur_replace;                    /* the item being copied replaces files */
    uint64_t start_ms;
} fileop_t;

#define FOP_CHUNK (128 * 1024)

static void op_free(fileop_t *op) {
    if (!op) return;
    free(op->src); free(op->dst); free(op->replace); free(op->ids); free(op->res);
    free(op);
}

static fileop_t *op_new(int kind, int n) {
    fileop_t *op = calloc(1, sizeof *op);
    if (!op) return NULL;
    size_t k = (size_t)(n > 0 ? n : 1);
    op->kind = kind;
    op->src = calloc(k, MAX_PATH);
    op->dst = calloc(k, MAX_PATH);
    op->replace = calloc(k, 1);
    op->ids = calloc(k, sizeof(unsigned));
    op->res = calloc(k, sizeof(int));
    if (!op->src || !op->dst || !op->replace || !op->ids || !op->res) { op_free(op); return NULL; }
    return op;
}

static void fop_set_current(fileop_t *op, const char *p) {
    mutex_lock(&op->lock);
    strlcpy(op->current, path_base(p), sizeof op->current);
    mutex_unlock(&op->lock);
}

static void fop_count(fileop_t *op, const char *p) {
    hal_dirent_t st;
    if (op->cancel || stat(p, &st) < 0) return;
    op->total_items++;
    if (!st.is_dir) { op->total_bytes += st.size; return; }
    hal_dirent_t *ents;
    int cnt = list_dir(p, &ents);
    if (cnt <= 0) { free(ents); return; }
    char *child = malloc(MAX_PATH);
    for (int i = 0; i < cnt && child && !op->cancel; i++) {
        path_join(p, ents[i].name, child, MAX_PATH);
        fop_count(op, child);
    }
    free(child);
    free(ents);
}

static int fop_copy_file(fileop_t *op, const char *src, const char *dst) {
    char *buf = malloc(FOP_CHUNK);
    if (!buf) return E_NOMEM;
    hal_dirent_t st;
    if (op->cur_replace && stat(dst, &st) == 0 && !st.is_dir) unlink(dst);     /* replace (asked before) */
    int in = open(src, O_READ), out = -1, r = in < 0 ? in : 0;
    if (!r) { out = open(dst, O_WRITE | O_CREATE | O_EXCL); if (out < 0) r = out; }
    while (!r) {
        if (op->cancel) { r = E_CANCELED; break; }
        long got = read(in, buf, FOP_CHUNK);
        if (got <= 0) { r = (int)got; break; }
        long put = write(out, buf, (size_t)got);
        if (put != got) { r = put < 0 ? (int)put : E_NOSPC; break; }
        op->done_bytes += (unsigned long long)got;
    }
    if (in >= 0) close(in);
    if (out >= 0) {
        int rc = close(out);
        if (!r && rc < 0) r = rc;
        if (r) unlink(dst);             /* no half-copied files */
    }
    free(buf);
    return r;
}

static int fop_copy(fileop_t *op, const char *src, const char *dst) {
    hal_dirent_t st;
    int r = stat(src, &st);
    if (r < 0) return r;
    fop_set_current(op, src);
    op->done_items++;
    if (!st.is_dir) return fop_copy_file(op, src, dst);
    size_t sl = strlen(src);
    if (prefix_nocase(src, dst, sl) && (dst[sl] == '/' || dst[sl] == 0)) return E_INVAL;    /* into itself */
    r = mkdir(dst);
    if (r < 0 && r != E_EXIST) return r;
    hal_dirent_t *ents;
    int cnt = list_dir(src, &ents);
    if (cnt < 0) return cnt;
    char *a = malloc(MAX_PATH), *b = malloc(MAX_PATH);
    r = (a && b) ? 0 : E_NOMEM;
    for (int i = 0; i < cnt && r >= 0; i++) {
        if (op->cancel) { r = E_CANCELED; break; }
        path_join(src, ents[i].name, a, MAX_PATH);
        path_join(dst, ents[i].name, b, MAX_PATH);
        r = fop_copy(op, a, b);
    }
    free(a); free(b); free(ents);
    return r < 0 ? r : 0;
}

static int fop_delete(fileop_t *op, const char *p) {
    hal_dirent_t st;
    int r = stat(p, &st);
    if (r < 0) return r;
    if (st.is_dir) {
        hal_dirent_t *ents;
        int cnt = list_dir(p, &ents);
        if (cnt < 0) return cnt;
        char *child = malloc(MAX_PATH);
        r = child ? 0 : E_NOMEM;
        for (int i = 0; i < cnt && !r; i++) {
            if (op->cancel) { r = E_CANCELED; break; }
            path_join(p, ents[i].name, child, MAX_PATH);
            r = fop_delete(op, child);
        }
        free(child);
        free(ents);
        if (r) return r;
    }
    fop_set_current(op, p);
    r = unlink(p);
    op->done_items++;
    if (!st.is_dir) op->done_bytes += st.size;
    return r;
}

static int same_drive(const char *a, const char *b) { return toupper((unsigned char)a[0]) == toupper((unsigned char)b[0]); }

static int fop_item(fileop_t *op, int i) {
    const char *src = op->src[i], *dst = op->dst[i];
    hal_dirent_t a, b;
    int r;
    op->cur_replace = op->replace[i];
    switch (op->kind) {
    case FOP_RECYCLE:
        fop_set_current(op, src);
        r = recycle_delete(src);
        op->done_items++;
        return r > 0 ? 0 : r;
    case FOP_PURGE:
        fop_set_current(op, src + 2);
        r = recycle_purge(src[0], op->ids[i]);
        op->done_items++;
        return r;
    case FOP_DELETE:
        return fop_delete(op, src);
    }
    /* a file over a folder or the other way round: the old one goes first */
    if (op->replace[i] && stat(src, &a) == 0 && stat(dst, &b) == 0 && a.is_dir != b.is_dir) {
        int k = op->kind;
        op->kind = FOP_DELETE;
        r = fop_delete(op, dst);
        op->kind = k;
        if (r) return r;
    }
    if (op->kind == FOP_MOVE && same_drive(src, dst)) {
        if (op->replace[i] && stat(dst, &b) == 0 && !b.is_dir) unlink(dst);
        fop_set_current(op, src);
        r = rename(src, dst);
        if (r != E_EXIST) { op->done_items++; return r; }
        /* onto an existing folder: merge (copy, then delete the source) */
    }
    r = fop_copy(op, src, dst);
    /* a move across drives deletes the source only after a full copy */
    if (!r && op->kind == FOP_MOVE) {
        op->kind = FOP_DELETE;
        r = fop_delete(op, src);
        op->kind = FOP_MOVE;
    }
    return r;
}

static void fileop_execute(fileop_t *op) {
    op->start_ms = time_ms();
    op->counting = 1;
    if (op->kind == FOP_RECYCLE || op->kind == FOP_PURGE) op->total_items = (unsigned)op->n;
    else for (int i = 0; i < op->n && !op->cancel; i++) fop_count(op, op->src[i]);
    op->counting = 0;
    int r = op->cancel ? E_CANCELED : 0;
    for (int i = 0; i < op->n && r != E_CANCELED; i++) {
        if (op->cancel) { r = E_CANCELED; break; }
        op->res[i] = fop_item(op, i);
        if (op->res[i] == E_CANCELED) r = E_CANCELED;
        else if (op->res[i] < 0 && !r && op->kind != FOP_RECYCLE) r = op->res[i];
    }
    op->result = r;
    __sync_synchronize();
    op->done = 1;
}

static int fop_worker(void *arg) { fileop_execute(arg); return 0; }

/* events that still matter while a dialog of ours is open: the main window
 * keeps painting and following its size (it takes no input meanwhile) */
static void layout(void);
static void passive_event(const hal_event_t *ev) {
    if (ev->win != win) return;
    if (ev->type == EVT_PAINT) paint();
    else if (ev->type == EVT_RESIZE) { cw = ev->x; ch = ev->y; layout(); }
    else if (ev->type == EVT_CLOSE) quit = 1;
    else if (ev->type == EVT_DRAG && (ev->button == DND_ENTER || ev->button == DND_OVER)) dnd_accept(win, ev->id, DND_NONE, 0);
}

/* the progress window (as progress.c) */
enum { ID_WHAT = 400, ID_WHERE, ID_BAR, ID_TIME, ID_CANCEL_OP };

static void short_dir(const char *p, char *out, int sz) {
    char d[MAX_PATH];
    path_dir(p, d, sizeof d);
    const char *b = path_base(d);
    snprintf(out, (size_t)sz, "%s", *b ? b : d);
}

/* label text: at most max bytes, cut at a character boundary */
static void cut_text(char *s, int max) {
    int len = (int)strlen(s);
    if (len <= max) return;
    int pos = 0, last = 0;
    while (pos < len && pos <= max) { last = pos; utf8_get(s, len, &pos); }
    if (pos <= max) last = pos;
    s[last] = 0;
}

static int op_deletes(const fileop_t *op) { return op->kind == FOP_DELETE || op->kind == FOP_PURGE; }

static void progress_update(int pw, fileop_t *op, char *last_what, char *last_time, int *last_val) {
    char t[MAX_NAME + 64], cur[MAX_NAME + 1];
    int del = op_deletes(op);
    const char *verb = del ? "Deleting" : op->kind == FOP_RECYCLE ? "Recycling" : op->kind == FOP_MOVE ? "Moving" : "Copying";
    mutex_lock(&op->lock);
    strlcpy(cur, op->current, sizeof cur);
    mutex_unlock(&op->lock);
    if (op->counting) snprintf(t, sizeof t, "Preparing to %s...", del || op->kind == FOP_RECYCLE ? "delete" : op->kind == FOP_MOVE ? "move" : "copy");
    else snprintf(t, sizeof t, "%s '%s'", verb, cur);
    cut_text(t, 250);
    if (strcmp(t, last_what)) { strlcpy(last_what, t, 256); ctl_set_text(pw, ID_WHAT, t); }
    /* progress by bytes; by items for deletes and empty files */
    int val = 0;
    unsigned long long tb = op->total_bytes, db = op->done_bytes;
    unsigned ti = op->total_items, di = op->done_items;
    if (tb && (op->kind == FOP_COPY || op->kind == FOP_MOVE)) val = (int)(db * 1000 / tb);
    else if (ti) val = (int)((unsigned long long)di * 1000 / ti);
    val = MIN(val, 1000);
    if (val != *last_val) { *last_val = val; ctl_set_value(pw, ID_BAR, val); }
    unsigned long long el = time_ms() - op->start_ms;
    if (op->counting || !val || el < 1500) snprintf(t, sizeof t, " ");
    else {
        unsigned long long left = el * (1000 - (unsigned long long)val) / (unsigned long long)val / 1000;
        if (left >= 120) snprintf(t, sizeof t, "%llu minutes remaining", (left + 30) / 60);
        else snprintf(t, sizeof t, "%llu second%s remaining", left + 1, left ? "s" : "");
    }
    if (strcmp(t, last_time)) { strlcpy(last_time, t, 64); ctl_set_text(pw, ID_TIME, t); }
}

/* run op in a worker thread behind a progress window (modal for ours);
 * returns op->result */
static int fileop_run(fileop_t *op) {
    op->done = 0;
    op->cancel = 0;
    if (!op->n) return 0;
    int tid = thread_create(fop_worker, op);
    if (tid < 0) { fileop_execute(op); return op->result; }    /* no thread: do it here */
    hal_event_t ev;
    /* quick operations finish without a window */
    uint64_t show_at = time_ms() + 300;
    while (!op->done) {
        uint64_t now = time_ms();
        if (now >= show_at) break;
        if (win_get_event(&ev, (int)(show_at - now))) passive_event(&ev);
    }
    if (!op->done) {
        int del = op_deletes(op) || op->kind == FOP_RECYCLE;
        const char *title = del ? "Deleting..." : op->kind == FOP_MOVE ? "Moving..." : "Copying...";
        int pw = win_create(title, 380, 150, WIN_DIALOG);
        if (pw >= 0) {
            win_set_icon(pw, del ? HICON_RECYCLE : HICON_FOLDER);
            ctl_create(pw, CTL_LABEL, 14, 12, 352, 16, "", ID_WHAT, 0);
            char a[MAX_NAME + 1], b[MAX_NAME + 1], line[2 * MAX_NAME + 32];
            if (op->kind == FOP_PURGE) strlcpy(line, "From the Recycle Bin", sizeof line);
            else {
                short_dir(op->src[0], a, sizeof a);
                if (del) snprintf(line, sizeof line, "From '%s'", a);
                else { short_dir(op->dst[0], b, sizeof b); snprintf(line, sizeof line, "From '%s' to '%s'", a, b); }
            }
            cut_text(line, 250);
            ctl_create(pw, CTL_LABEL, 14, 32, 352, 16, line, ID_WHERE, 0);
            ctl_create(pw, CTL_PROGRESS, 14, 56, 352, 16, "", ID_BAR, 0);
            ctl_create(pw, CTL_LABEL, 14, 80, 250, 16, " ", ID_TIME, 0);
            ctl_create(pw, CTL_BUTTON, 291, 112, 75, 23, "Cancel", ID_CANCEL_OP, CTL_CANCEL);
            ctl_focus(pw, ID_CANCEL_OP);
            win_set_owner(pw, win, 1);
            char last_what[256] = "", last_time[64] = "";
            int last_val = -1, canceling = 0;
            progress_update(pw, op, last_what, last_time, &last_val);
            uint64_t next = time_ms() + 100;
            while (!op->done) {
                uint64_t now = time_ms();
                int wait_ms = now >= next ? 0 : (int)(next - now);
                if (win_get_event(&ev, wait_ms)) {
                    if (ev.win == pw) {
                        int cancel_now = (ev.type == EVT_COMMAND && ev.id == ID_CANCEL_OP) || ev.type == EVT_CLOSE ||
                                         (ev.type == EVT_KEY && ev.pressed && ev.id == KEY_ESC);
                        if (cancel_now && !canceling) {
                            canceling = 1;
                            op->cancel = 1;
                            ctl_set_text(pw, ID_CANCEL_OP, "Canceling...");
                            ctl_set_style(pw, ID_CANCEL_OP, CTL_DISABLED);
                        }
                    } else {
                        if (ev.type == EVT_CLOSE && ev.win == win) op->cancel = 1;
                        passive_event(&ev);
                    }
                }
                if (time_ms() >= next) { progress_update(pw, op, last_what, last_time, &last_val); next = time_ms() + 100; }
            }
            win_destroy(pw);
        } else {
            while (!op->done) if (win_get_event(&ev, 100)) passive_event(&ev);
        }
    }
    thread_join(tid);
    return op->result;
}

/* ---------------- selection ---------------- */
static void sel_clear(void) { for (int i = 0; i < n; i++) items[i].selected = 0; nsel = 0; }
static void sel_set(int i, int on) {
    if (i < 0 || i >= n || items[i].selected == (on != 0)) return;
    items[i].selected = on != 0;
    nsel += on ? 1 : -1;
}
static void sel_only(int i) { sel_clear(); sel_set(i, 1); focus = anchor = i; }
static void sel_range(int a, int b, int add) {
    if (!add) sel_clear();
    if (a < 0) a = b;
    for (int i = MIN(a, b); i <= MAX(a, b); i++) sel_set(i, 1);
}
static int sel_first(void) {
    for (int i = 0; i < n; i++) if (items[i].selected) return i;
    return -1;
}
/* the one item commands act on when only one makes sense */
static int sel_single(void) { return nsel == 1 ? sel_first() : -1; }

/* ---------------- model ---------------- */
static void update_menus(void);
static int  rb_full(void);

static void update_title(void) {
    char t[MAX_PATH + 16];
    if (binview) strlcpy(t, "Recycle Bin", sizeof t);
    else if (!path[0]) strlcpy(t, "My Computer", sizeof t);
    else if (path_is_root(path)) {
        hal_volinfo_t vi;
        int ok = volume_info(path[0], &vi) == 0 && vi.label[0];
        snprintf(t, sizeof t, "%s (%c:)", ok ? vi.label : "Local Disk", path[0]);
    } else strlcpy(t, path_base(path), sizeof t);
    win_set_title(win, t);
    win_set_icon(win, binview ? (rb_full() ? HICON_RECYCLE_FULL : HICON_RECYCLE) : !path[0] ? HICON_COMPUTER : HICON_FOLDER_OPEN);
    ctl_set_text(win, ID_ADDR, binview ? "Recycle Bin" : path[0] ? path : "My Computer");
}

static void free_items(void) {
    for (int i = 0; i < n; i++) free(items[i].orig);
    free(items);
    items = NULL;
    n = 0;
}

static unsigned rbgen;                      /* the bins' generation when the view was loaded */
static int rb_icon = HICON_RECYCLE;         /* empty or full (Other Places) */
static int rb_full(void) { return recycle_list(0, NULL, 0) > 0; }

/* the Recycle Bin's entries of every drive */
static void load_bin(void) {
    hal_rbinfo_t ri;
    if (recycle_info(0, &ri) == 0) rbgen = ri.generation;
    int cnt = recycle_list(0, NULL, 0);
    if (cnt <= 0) { items = calloc(1, sizeof(item_t)); return; }
    hal_rbentry_t *e = malloc(sizeof *e * (size_t)(cnt + 8));
    items = calloc((size_t)cnt + 8, sizeof(item_t));
    if (!e || !items) { free(e); return; }
    cnt = recycle_list(0, e, cnt + 8);
    for (int i = 0; i < cnt; i++) {
        item_t *it = &items[n++];
        strlcpy(it->name, path_base(e[i].orig), sizeof it->name);
        char d[MAX_PATH];
        path_dir(e[i].orig, d, sizeof d);
        it->orig = strdup(d);
        it->is_dir = e[i].is_dir;
        it->size = e[i].size;
        it->year = e[i].year; it->month = e[i].month; it->day = e[i].day;
        it->hour = e[i].hour; it->minute = e[i].minute; it->second = e[i].second;
        it->rbid = e[i].id;
        it->letter = e[i].drive;
        it->icon = file_icon(it->name, it->is_dir);
    }
    free(e);
}

static void load(void) {
    free_items();
    focus = anchor = -1; hover = -1; nsel = 0; scroll = 0;
    vgen = vfs_generation();
    disk_free = 0;
    if (binview) {
        load_bin();
        sort_items(items, n);
    } else if (!path[0]) {
        items = calloc(26, sizeof(item_t));
        for (char c = 'A'; c <= 'Z' && items; c++) {
            hal_volinfo_t vi;
            if (volume_info(c, &vi) < 0) continue;
            item_t *it = &items[n++];
            it->letter = vi.letter;
            it->kind = vi.kind;
            it->is_boot = vi.is_boot;
            it->readonly = vi.readonly;
            it->vflags = vi.flags;
            strlcpy(it->fs, vi.fs, sizeof it->fs);
            strlcpy(it->device, vi.device, sizeof it->device);
            snprintf(it->name, sizeof it->name, "%s (%c:)", vi.label[0] ? vi.label : drive_kind(vi.kind), vi.letter);
            it->is_dir = 1;
            it->icon = drive_icon(vi.kind);
            it->total = vi.total;
            it->free = vi.free_bytes;
        }
    } else {
        hal_dirent_t *ents;
        int cnt = list_dir(path, &ents);
        if (cnt < 0) {
            char msg[MAX_PATH + 64];
            snprintf(msg, sizeof msg, "%s is not accessible.\n\n%s", path, errstr(cnt));
            path[0] = 0;
            msgbox(win, "Explorer", msg, MBX_ICON_ERROR, MBX_OK);
            load();
            return;
        }
        items = calloc((size_t)cnt + 1, sizeof(item_t));
        int root = path_is_root(path);
        for (int i = 0; i < cnt && items; i++) {
            if (ents[i].attr & (ATTR_HIDDEN | ATTR_VOLUME)) continue;
            if (root && !strcasecmp(ents[i].name, "RECYCLED") && ents[i].is_dir) continue;   /* the Recycle Bin's folder */
            item_t *it = &items[n++];
            strlcpy(it->name, ents[i].name, sizeof it->name);
            it->is_dir = ents[i].is_dir;
            it->size = ents[i].size;
            it->year = ents[i].year; it->month = ents[i].month; it->day = ents[i].day;
            it->hour = ents[i].hour; it->minute = ents[i].minute; it->second = ents[i].second;
            it->icon = file_icon(it->name, it->is_dir);
        }
        free(ents);
        sort_items(items, n);
        hal_volinfo_t vi;
        if (volume_info(path[0], &vi) == 0) disk_free = vi.free_bytes;
    }
    update_title();
    update_menus();
    paint();
}

static int find_item(const char *nm, unsigned rbid, char drive) {
    for (int i = 0; i < n; i++) {
        if (binview ? items[i].rbid == rbid && items[i].letter == drive : !strcasecmp(items[i].name, nm)) return i;
    }
    return -1;
}

static void ensure_visible(int i);
/* load again, keeping the selection, the focus and the scroll position where the items still exist */
static void reload_keep(void) {
    int cnt = 0;
    typedef struct { char name[MAX_NAME + 1]; unsigned id; char drive; } selkey_t;
    selkey_t *keys = nsel ? malloc(sizeof(selkey_t) * (size_t)nsel) : NULL;
    selkey_t fk = { "", 0, 0 };
    for (int i = 0; i < n && keys; i++)
        if (items[i].selected && cnt < nsel) { strlcpy(keys[cnt].name, items[i].name, sizeof keys[cnt].name); keys[cnt].id = items[i].rbid; keys[cnt].drive = items[i].letter; cnt++; }
    if (focus >= 0 && focus < n) { strlcpy(fk.name, items[focus].name, sizeof fk.name); fk.id = items[focus].rbid; fk.drive = items[focus].letter; }
    int sc = scroll, oldfocus = focus;
    load();
    for (int i = 0; i < cnt; i++) sel_set(find_item(keys[i].name, keys[i].id, keys[i].drive), 1);
    free(keys);
    focus = fk.name[0] ? find_item(fk.name, fk.id, fk.drive) : -1;
    if (focus < 0 && oldfocus >= 0 && n) focus = MIN(oldfocus, n - 1);
    anchor = focus;
    scroll = sc;
    update_menus();
    paint();
}

/* select these names (after a paste or a drop) */
static void select_names(char (*names)[MAX_PATH], int cnt) {
    sel_clear();
    for (int i = 0; i < cnt; i++) {
        int k = find_item(path_base(names[i]), 0, 0);
        if (k >= 0) { sel_set(k, 1); focus = anchor = k; }
    }
    ensure_visible(focus);
    update_menus();
    paint();
}

static void navigate(const char *to, int push) {
    char norm[MAX_PATH] = "";
    int bin = !strcasecmp(to, "Recycle Bin") || !strcasecmp(to, "::recycle");
    if (to[0] && strcasecmp(to, "My Computer") && !bin) {
        if (path_normalize(to, norm, sizeof norm) < 0) {
            msgbox(win, "Explorer", "The path is not valid.", MBX_ICON_ERROR, MBX_OK);
            return;
        }
        hal_dirent_t st;
        if (stat(norm, &st) < 0) {
            char msg[MAX_PATH + 64];
            snprintf(msg, sizeof msg, "Cannot find '%s'.", to);
            msgbox(win, "Explorer", msg, MBX_ICON_ERROR, MBX_OK);
            return;
        }
        if (!st.is_dir) { shell_open(norm); return; }
    }
    if (push && (strcmp(path, norm) || bin != binview)) {
        if (nback == 16) { memmove(back[0], back[1], sizeof(back[0]) * 15); nback--; }
        strlcpy(back[nback++], binview ? "Recycle Bin" : path, MAX_PATH);
    }
    strlcpy(path, norm, sizeof path);
    binview = bin;
    load();
}

static void go_up(void) {
    if (!path[0]) { if (binview) navigate("", 1); return; }
    if (path_is_root(path)) { navigate("", 1); return; }
    char p[MAX_PATH];
    path_dir(path, p, sizeof p);
    navigate(p, 1);
}

static void item_path(int i, char *out, int sz) {
    if (binview) {
        char d[MAX_PATH];
        strlcpy(d, items[i].orig ? items[i].orig : "", sizeof d);
        path_join(d, items[i].name, out, sz);
        return;
    }
    if (!path[0]) { snprintf(out, (size_t)sz, "%c:/", items[i].letter); return; }
    path_join(path, items[i].name, out, sz);
}

static void open_item(int i) {
    if (i < 0 || i >= n || binview) return;
    char p[MAX_PATH];
    item_path(i, p, sizeof p);
    if (items[i].is_dir) navigate(p, 1);
    else {
        int r = shell_open(p);
        if (r < 0) {
            char msg[MAX_PATH + 64];
            snprintf(msg, sizeof msg, "Cannot open '%s'.\n\n%s", items[i].name, errstr(r));
            msgbox(win, "Explorer", msg, MBX_ICON_ERROR, MBX_OK);
        }
    }
}

/* the selected items' paths as one NUL-separated list (malloc()ed) */
static char *selected_paths(int *len, int *count) {
    size_t cap = 4096, o = 0;
    char *b = malloc(cap), p[MAX_PATH];
    *count = 0;
    for (int i = 0; i < n && b; i++) {
        if (!items[i].selected) continue;
        item_path(i, p, sizeof p);
        size_t l = strlen(p) + 1;
        if (o + l + 1 > cap) { cap = cap * 2 + l; char *nb = realloc(b, cap); if (!nb) { free(b); return NULL; } b = nb; }
        memcpy(b + o, p, l);
        o += l;
        (*count)++;
    }
    if (b) b[o] = 0;
    *len = (int)o;
    return b;
}

/* ---------------- settings (SETTINGS.INI, explorer.*) ---------------- */
static int setting_int(const char *key, int def) {
    char v[16];
    if (shell_setting_get(key, v, sizeof v) <= 0) return def;
    char *end;
    long x = strtol(v, &end, 10);
    return *end ? def : (int)x;
}
static void save_int(const char *key, int v) {
    char b[16];
    snprintf(b, sizeof b, "%d", v);
    shell_setting_set(key, b);
}
static void load_settings(void) {
    view = CLAMP(setting_int("explorer.view", 0), 0, 1);
    sort_col = CLAMP(setting_int("explorer.sort", 0), 0, 3);
    sort_desc = setting_int("explorer.sortdesc", 0) != 0;
    pane_on = setting_int("explorer.pane", 1) != 0;
}

/* ---------------- layout ---------------- */
#define IS_COMPUTER (!path[0] && !binview)
static int pane_w(void) { return pane_on && cw >= 560 ? PANE_W : 0; }

static void compute_view(void) {
    view_x = pane_w();
    view_y = TOP_Y;
    view_w = cw - view_x;
    view_h = MAX(0, ch - TOP_Y - STATUS_H);
}

static int cols(void) {
    int c = IS_COMPUTER && view == 0 ? TILE_W : CELL_W;
    return MAX(1, (view_w - 20) / c);
}

/* the columns of the details view: x of each (and the end), titles, alignment */
#define NCOLS 4
static void columns(int *xs, const char **titles, int *align) {
    int cwid = view_w - SB_W;
    static const char *const folder[NCOLS] = { "Name", "Size", "Type", "Date Modified" };
    static const char *const comp[NCOLS] = { "Name", "Total Size", "Type", "Free Space" };
    static const char *const bin[NCOLS] = { "Name", "Original Location", "Date Deleted", "Size" };
    int w[NCOLS];
    if (binview) {
        int rest = MAX(240, cwid - 210);
        w[2] = 130; w[3] = 80;
        w[0] = MAX(120, rest * 45 / 100);
        w[1] = MAX(80, rest - w[0]);
    } else {
        w[1] = 80; w[2] = 120; w[3] = 130;
        w[0] = MAX(160, cwid - 330);
    }
    xs[0] = 0;
    for (int i = 0; i < NCOLS; i++) {
        xs[i + 1] = xs[i] + w[i];
        if (titles) titles[i] = binview ? bin[i] : IS_COMPUTER ? comp[i] : folder[i];
        if (align) align[i] = binview ? (i == 3 ? 2 : 0) : (i == 1 ? 2 : 0);
    }
    xs[NCOLS] = MAX(xs[NCOLS], cwid);
}

static hal_rect_t item_rect(int i) {
    hal_rect_t r;
    if (view == 1) {
        int xs[NCOLS + 1];
        columns(xs, NULL, NULL);
        r.x = view_x; r.y = view_y + 22 + i * ROW_H - scroll; r.w = xs[1]; r.h = ROW_H;
        return r;
    }
    int c = cols();
    if (IS_COMPUTER) {
        r.x = view_x + 10 + (i % c) * TILE_W; r.y = view_y + 34 + (i / c) * (TILE_H + 8) - scroll;
        r.w = TILE_W - 10; r.h = TILE_H;
        return r;
    }
    r.x = view_x + 6 + (i % c) * CELL_W; r.y = view_y + 8 + (i / c) * CELL_H - scroll;
    r.w = CELL_W - 4; r.h = CELL_H - 4;
    return r;
}

static int content_height(void) {
    if (view == 1) return 22 + n * ROW_H + 4;
    int c = cols(), rows = (n + c - 1) / c;
    if (IS_COMPUTER) return 34 + rows * (TILE_H + 8) + 8;
    return 8 + rows * CELL_H + 8;
}

static int max_scroll(void) { return MAX(0, content_height() - view_h); }

static void ensure_visible(int i) {
    if (i < 0 || i >= n) return;
    hal_rect_t r = item_rect(i);
    int top = view_y + (view == 1 ? 22 : 0);
    if (r.y < top) scroll -= top - r.y;
    if (r.y + r.h > view_y + view_h) scroll += r.y + r.h - (view_y + view_h);
    scroll = CLAMP(scroll, 0, max_scroll());
}

static void layout(void) {
    compute_view();
    ctl_move(win, ID_ADDR, 62, TOOLBAR_H + 3, MAX(20, cw - 62 - 50), 22);
    ctl_move(win, ID_GO, cw - 46, TOOLBAR_H + 3, 40, 22);
    ctl_move(win, ID_VSB, view_x + view_w - SB_W, view_y, SB_W, MAX(SB_W * 2, view_h));
}

/* ---------------- painting ---------------- */
static int drop_hl = -1;                    /* the item a drag is over (a folder or drive it would go into) */
static int band_on, band_x0, band_y0, band_x1, band_y1;    /* the rubber band: content y (+ scroll) */

static void draw_arrow_btn(int x, int y, int dir, int enabled) {
    /* round button with an arrow: dir 0 = left, 1 = up */
    unsigned top = enabled ? 0x8FD883 : 0xD8D8D0, bot = enabled ? 0x2E9A3A : 0xB0B0A8;
    rounded(x, y, 26, 26, 13, 15, top, bot);
    rounded_outline(x, y, 26, 26, 13, 15, enabled ? 0x1F6A26 : 0x9A9A90);
    unsigned c = 0xFFFFFF;
    int cx = x + 13, cy = y + 13;
    if (dir == 0) {
        for (int i = 0; i < 6; i++) vline(cx - 6 + i, cy - i, 2 * i + 1, c);
        gfx_fill(win, cx, cy - 2, 7, 5, c);
    } else {
        for (int i = 0; i < 6; i++) hline(cx - i, cy - 6 + i, 2 * i + 1, c);
        gfx_fill(win, cx - 2, cy, 5, 7, c);
    }
}

static hal_rect_t tb_rect(int i) {
    static const int xs[] = { 6, 84, 152, 238 };
    static const int ws[] = { 74, 64, 82, 82 };
    hal_rect_t r = { xs[i], 2, ws[i], TOOLBAR_H - 4 };
    return r;
}

static void paint_toolbar(void) {
    gfx_gradient(win, 0, 0, cw, TOOLBAR_H, lighten(th.face, 50), th.face);
    hline(0, TOOLBAR_H - 1, cw, lighten(th.face, -25));
    static const char *labels[] = { "Back", "Up", "Folders", "Views" };
    for (int i = 0; i < 4; i++) {
        hal_rect_t r = tb_rect(i);
        int en = i == 0 ? nback > 0 : i == 1 ? (path[0] != 0 || binview) : 1;
        if (en && tb_hover == i) {
            rounded(r.x, r.y, r.w, r.h, 3, 15, 0xFFFFFF, lighten(th.face, -15));
            rounded_outline(r.x, r.y, r.w, r.h, 3, 15, lighten(th.face, -60));
        }
        if (i < 2) draw_arrow_btn(r.x + 3, r.y + 4, i, en);
        else gfx_icon(win, r.x + 6, r.y + 8, ICON16(i == 2 ? HICON_FOLDER_OPEN : (view ? HICON_FILE_TEXT : HICON_FILE)));
        int tx = r.x + (i < 2 ? 32 : 26);
        gfx_text_box(win, tx, r.y, r.w - 30, r.h, i == 3 ? (view ? "Details" : "Icons") : labels[i], en ? 0x000000 : 0xA0A0A0, FONT_UI, 0);
    }
    /* address bar */
    int ay = TOOLBAR_H;
    gfx_fill(win, 0, ay, cw, ADDR_H, th.face);
    gfx_text_box(win, 8, ay, 50, ADDR_H, "Address", 0x6D6D6D, FONT_UI, 0);
    hline(0, ay + ADDR_H - 1, cw, lighten(th.face, -25));
}

static void add_link(int x, int y, int w, int h, int id) {
    if (nlinks < MAX_LINKS) { link_t *l = &links[nlinks++]; l->r.x = x; l->r.y = y; l->r.w = w; l->r.h = h; l->id = id; }
}

/* one box of the task pane (pane coordinates: the clip origin is the pane's corner) */
static int pane_box(int y, int wdt, const char *title, int nlines, const char **lines, const int *icons, const int *ids) {
    int hx = 12, hw = wdt - 24;
    rounded(hx, y, hw, 24, 5, 3, 0xFFFFFF, lighten(th.sidebar_top, 120));
    gfx_text_box(win, hx + 10, y, hw - 12, 24, title, lighten(th.menu_header_bottom, -40), FONT_UI_BOLD, 0);
    int bh = nlines * 20 + 12, by = y + 24;
    gfx_fill(win, hx, by, hw, bh, lighten(th.sidebar_top, 165));
    gfx_rect(win, hx, by, hw, bh, 0xFFFFFF);
    for (int i = 0; i < nlines; i++) {
        int lx = hx + 8, ly = by + 6 + i * 20, lw = hw - 12;
        int hov = ids && link_hover == ids[i];
        if (icons && icons[i] >= 0) gfx_icon(win, lx, ly + 2, ICON16(icons[i]));
        int tx = lx + (icons && icons[i] >= 0 ? 22 : 0);
        unsigned tc = ids ? lighten(th.menu_header_bottom, hov ? 40 : -20) : 0x202020;
        gfx_text_box(win, tx, ly, lw - (tx - lx), 20, lines[i], tc, FONT_UI, 0);
        if (hov) hline(tx, ly + 15, MIN(text_width(lines[i], FONT_UI), lw - (tx - lx)), tc);
        if (ids) add_link(lx, ly + TOP_Y, lw, 20, ids[i]);
    }
    return y + 24 + bh + 12;
}

/* the selected items' total size (files; folders count as their own size, 0) */
static unsigned long long sel_bytes(int *files, int *dirs) {
    unsigned long long b = 0;
    *files = *dirs = 0;
    for (int i = 0; i < n; i++) {
        if (!items[i].selected) continue;
        if (items[i].is_dir && !binview) { (*dirs)++; continue; }
        (*files)++;
        b += items[i].size;
    }
    return b;
}

static void paint_pane(void) {
    int pw = pane_w();
    nlinks = 0;
    if (!pw) return;
    int ph = ch - TOP_Y - STATUS_H;
    if (ph <= 0) return;
    gfx_clip_origin(win, 0, TOP_Y, pw, ph);
    gfx_gradient(win, 0, 0, pw, ph, th.sidebar_top, th.sidebar_bottom);
    int y = 12;
    int one = sel_single();
    if (binview) {
        const char *l[] = { "Empty the Recycle Bin", !nsel ? "Restore all items" : nsel == 1 ? "Restore this item" : "Restore the selected items" };
        const int ic[] = { HICON_RECYCLE, HICON_BACK };
        const int id[] = { CMD_EMPTY, nsel ? CMD_RESTORE : CMD_RESTORE_ALL };
        y = pane_box(y, pw, "Recycle Bin Tasks", n ? 2 : 0, l, ic, id);
    } else if (!path[0]) {
        const char *l[] = { "View system information", "Change a setting", "Open Command Prompt" };
        const int ic[] = { HICON_SYSINFO, HICON_CONTROL, HICON_TERMINAL };
        const int id[] = { CMD_SYSINFO, CMD_CONTROL, CMD_TERMINAL };
        y = pane_box(y, pw, "System Tasks", 3, l, ic, id);
    } else if (nsel) {
        const char *l1[] = { "Rename this item", "Copy this item", "Delete this item", "Make a new folder" };
        const char *ln[] = { "Copy these items", "Move these items", "Delete these items", "Make a new folder" };
        const int ic1[] = { HICON_FILE_TEXT, HICON_DOCUMENTS, HICON_RECYCLE, HICON_FOLDER };
        const int icn[] = { HICON_DOCUMENTS, HICON_FOLDER_OPEN, HICON_RECYCLE, HICON_FOLDER };
        const int id1[] = { CMD_RENAME, CMD_COPY, CMD_DELETE, CMD_NEWFOLDER };
        const int idn[] = { CMD_COPY, CMD_CUT, CMD_DELETE, CMD_NEWFOLDER };
        y = pane_box(y, pw, "File and Folder Tasks", 4, one >= 0 ? l1 : ln, one >= 0 ? ic1 : icn, one >= 0 ? id1 : idn);
    } else {
        int pc = clip->count > 0;
        const char *l[] = { "Make a new folder", "New text document", pc ? "Paste here" : "Open Command Prompt" };
        const int ic[] = { HICON_FOLDER, HICON_FILE_TEXT, pc ? HICON_DOCUMENTS : HICON_TERMINAL };
        const int id[] = { CMD_NEWFOLDER, CMD_NEWTEXT, pc ? CMD_PASTE : CMD_TERMINAL };
        y = pane_box(y, pw, "File and Folder Tasks", 3, l, ic, id);
    }
    {
        const char *l[] = { "My Computer", "My Documents", binview ? "Control Panel" : "Recycle Bin" };
        const int ic[] = { HICON_COMPUTER, HICON_DOCUMENTS, binview ? HICON_CONTROL : rb_icon };
        const int id[] = { CMD_MYCOMP, CMD_MYDOCS, binview ? CMD_CONTROL : CMD_RECYCLEBIN };
        y = pane_box(y, pw, "Other Places", 3, l, ic, id);
    }
    /* details box */
    char l1[MAX_NAME + 1], l2[MAX_PATH], l3[64], l4[64], l5[64];
    const char *lines[5] = { l1, l2, l3, l4, l5 };
    int k;
    if (nsel > 1) {
        int files, dirs;
        unsigned long long b = sel_bytes(&files, &dirs);
        snprintf(l1, sizeof l1, "%d items selected.", nsel);
        char s[32];
        format_size(b, s, sizeof s);
        snprintf(l2, sizeof l2, "Total File Size: %s", s);
        k = 2;
        if (!binview && dirs) { snprintf(l3, sizeof l3, "%d file%s, %d folder%s", files, files == 1 ? "" : "s", dirs, dirs == 1 ? "" : "s"); k = 3; }
    } else if (one >= 0) {
        item_t *it = &items[one];
        strlcpy(l1, it->name, sizeof l1);
        if (binview) {
            snprintf(l2, sizeof l2, "From: %s", it->orig ? it->orig : "");
            char t[48], s[32];
            format_time(it->year, it->month, it->day, it->hour, it->minute, t, sizeof t);
            snprintf(l3, sizeof l3, "Deleted: %s", t);
            format_size(it->size, s, sizeof s);
            snprintf(l4, sizeof l4, "Size: %s", s);
            k = 4;
        } else if (!path[0]) {
            snprintf(l2, sizeof l2, "%s, %s", drive_kind(it->kind), it->fs);
            char a[32], b[32];
            format_size(it->free, a, sizeof a);
            format_size(it->total, b, sizeof b);
            snprintf(l3, sizeof l3, "Free Space: %s", a);
            snprintf(l4, sizeof l4, "Total Size: %s", b);
            k = 4;
            if (it->readonly && it->kind != VOL_CDROM) { strlcpy(l5, "Read-only", sizeof l5); k = 5; }
        } else {
            strlcpy(l2, file_type_name(it->name, it->is_dir), sizeof l2);
            k = 2;
            if (!it->is_dir) { char s[32]; format_size(it->size, s, sizeof s); snprintf(l3, sizeof l3, "Size: %s", s); k = 3; }
            char t[48];
            format_time(it->year, it->month, it->day, it->hour, it->minute, t, sizeof t);
            if (t[0]) { snprintf(k == 3 ? l4 : l3, 64, "Modified: %s", t); k++; }
        }
    } else if (binview) {
        strlcpy(l1, "Recycle Bin", sizeof l1);
        snprintf(l2, sizeof l2, "%d item%s", n, n == 1 ? "" : "s");
        k = 2;
    } else if (path[0]) {
        strlcpy(l1, path_is_root(path) ? path : path_base(path), sizeof l1);
        strlcpy(l2, "File Folder", sizeof l2);
        k = 2;
    } else {
        strlcpy(l1, "My Computer", sizeof l1);
        strlcpy(l2, "System Folder", sizeof l2);
        k = 2;
    }
    pane_box(y, pw, "Details", k, lines, NULL, NULL);
    gfx_clip_reset(win);
}

/* an icon view name: up to two lines, broken at a space when possible */
static void split_name(const char *nm, int maxw, char *l1, char *l2) {
    int len = (int)strlen(nm), n1 = len;
    l2[0] = 0;
    if (text_width(nm, FONT_UI) > maxw) {
        int acc = 0, lastsp = -1, pos = 0;
        while (pos < len) {
            int q = pos;
            unsigned cp = utf8_get(nm, len, &q);
            int w = text_char_width(cp, FONT_UI);
            if (acc + w > maxw && pos > 0) break;
            if (cp == ' ') lastsp = pos;
            acc += w;
            pos = q;
        }
        n1 = pos;
        if (lastsp > 0 && n1 < len) n1 = lastsp + 1;
    }
    memcpy(l1, nm, (size_t)n1);
    l1[n1] = 0;
    int k = n1;
    while (k > 0 && l1[k - 1] == ' ') l1[--k] = 0;
    if (k < len) strlcpy(l2, nm + k + (nm[k] == ' '), MAX_NAME + 1);
}

/* a translucent rectangle straight into the window's pixels (the rubber band) */
static void blend_rect(int x, int y, int w, int h, unsigned c, int a) {
    hal_canvas_t cv;
    if (win_canvas(win, &cv)) return;
    int x0 = MAX(x, view_x), y0 = MAX(y, view_y), x1 = MIN(x + w, view_x + view_w - SB_W), y1 = MIN(y + h, view_y + view_h);
    x1 = MIN(x1, cv.w); y1 = MIN(y1, cv.h);
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
            unsigned *p = &cv.pixels[yy * cv.stride + xx];
            *p = mix(*p, c, a);
        }
}

static void band_rect(int *x, int *y, int *w, int *h) {
    int y0 = band_y0 - scroll, y1 = band_y1 - scroll;
    *x = MIN(band_x0, band_x1); *w = MAX(band_x0, band_x1) - *x;
    *y = MIN(y0, y1); *h = MAX(y0, y1) - *y;
}

/* the cell text of the details view */
static void cell(int i, int col, char *buf, int sz) {
    item_t *it = &items[i];
    buf[0] = 0;
    if (binview) {
        if (col == 1) strlcpy(buf, it->orig ? it->orig : "", (size_t)sz);
        else if (col == 2) format_time(it->year, it->month, it->day, it->hour, it->minute, buf, sz);
        else if (col == 3) format_size(it->size, buf, sz);
    } else if (!path[0]) {
        if (col == 1) format_size(it->total, buf, sz);
        else if (col == 2) strlcpy(buf, drive_kind(it->kind), (size_t)sz);
        else if (col == 3) format_size(it->free, buf, sz);
    } else {
        if (col == 1 && !it->is_dir) format_size(it->size, buf, sz);
        else if (col == 2) strlcpy(buf, file_type_name(it->name, it->is_dir), (size_t)sz);
        else if (col == 3) format_time(it->year, it->month, it->day, it->hour, it->minute, buf, sz);
    }
}

static void focus_rect(int x, int y, int w, int h) { gfx_shade(win, x, y, w, h, 0, 0, SHADE_FOCUS, 0); }

static void paint_view(void) {
    int vx = view_x, vy = view_y, vw = view_w, vh = view_h;
    if (vw <= SB_W || vh <= 0) return;
    gfx_fill(win, vx, vy, vw - SB_W, vh, 0xFFFFFF);
    gfx_clip(win, vx, vy, vw - SB_W, vh);
    if (view == 1) {
        /* details */
        int cwid = vw - SB_W, xs[NCOLS + 1], align[NCOLS];
        const char *hdr[NCOLS];
        columns(xs, hdr, align);
        for (int i = 0; i < n; i++) {
            hal_rect_t r = item_rect(i);
            if (r.y + r.h < vy + 22 || r.y > vy + vh) continue;
            item_t *it = &items[i];
            int s = it->selected || i == drop_hl;
            if (s) gfx_fill(win, r.x + 2, r.y, xs[1] - 4, r.h, th.select);
            else if (i == hover) gfx_fill(win, r.x + 2, r.y, xs[1] - 4, r.h, lighten(th.select, 200));
            gfx_icon(win, r.x + 6, r.y + 1, ICON16(it->icon));
            gfx_text_box(win, r.x + 26, r.y, xs[1] - 30, r.h, it->name, s ? th.select_text : 0x000000, FONT_UI, 0);
            if (i == focus) focus_rect(r.x + 2, r.y, xs[1] - 4, r.h);
            for (int c = 1; c < NCOLS; c++) {
                char buf[MAX_PATH];
                cell(i, c, buf, sizeof buf);
                gfx_text_box(win, r.x + xs[c] + 8, r.y, xs[c + 1] - xs[c] - 14, r.h, buf, 0x000000, FONT_UI, align[c]);
            }
        }
        /* header, with the sort column's arrow */
        gfx_gradient(win, vx, vy, cwid, 21, 0xFFFFFF, 0xEBEADB);
        hline(vx, vy + 20, cwid, 0xD6D2C2);
        for (int i = 0; i < NCOLS; i++) {
            int x0 = vx + xs[i], wd = xs[i + 1] - xs[i];
            int sorted = !IS_COMPUTER && i == sort_col;
            gfx_text_box(win, x0 + 8, vy, wd - (sorted ? 26 : 12), 21, hdr[i], 0x000000, FONT_UI, align[i]);
            if (sorted) {
                int ax = x0 + wd - 14, ay = vy + 8;
                for (int k = 0; k < 4; k++) hline(ax + (sort_desc ? k : 3 - k), ay + k, sort_desc ? 7 - 2 * k : 1 + 2 * k, 0x8A8678);
            }
            vline(vx + xs[i + 1] - 1, vy + 3, 15, 0xD6D2C2);
        }
        if (!n) gfx_text(win, vx + 14, vy + 30, binview ? "The Recycle Bin is empty." : "This folder is empty.", 0x6D6D6D, FONT_UI);
    } else if (IS_COMPUTER) {
        /* My Computer tiles */
        gfx_text(win, vx + 12, vy + 10 - scroll, "Files Stored on This Computer", lighten(th.menu_header_bottom, -40), FONT_UI_BOLD);
        gfx_shade(win, vx + 12, vy + 26 - scroll, vw - 60, 1, th.sidebar_bottom, 0xFFFFFF, SHADE_GRADIENT_H, 0);
        for (int i = 0; i < n; i++) {
            hal_rect_t r = item_rect(i);
            item_t *it = &items[i];
            if (it->selected || i == drop_hl) rounded(r.x, r.y, r.w, r.h, 3, 15, lighten(th.select, 170), lighten(th.select, 140));
            else if (i == hover) rounded(r.x, r.y, r.w, r.h, 3, 15, lighten(th.select, 215), lighten(th.select, 200));
            if (i == focus && nsel > 1) focus_rect(r.x, r.y, r.w, r.h);
            gfx_icon(win, r.x + 6, r.y + 6, ICON48(it->icon));
            if (it->readonly && it->kind != VOL_CDROM) draw_lock(r.x + 38, r.y + 36);
            gfx_text_box(win, r.x + 60, r.y + 4, r.w - 64, 16, it->name, 0x000000, FONT_UI_BOLD, 0);
            char a[32], b[32], line[80];
            format_size(it->free, a, sizeof a);
            format_size(it->total, b, sizeof b);
            snprintf(line, sizeof line, "%s free of %s", a, b);
            gfx_text_box(win, r.x + 60, r.y + 20, r.w - 64, 16, line, 0x6D6D6D, FONT_UI, 0);
            int bx = r.x + 60, by = r.y + 40, bw = r.w - 70, bh = 10;
            gfx_fill(win, bx, by, bw, bh, 0xFFFFFF);
            gfx_rect(win, bx, by, bw, bh, 0x8C8C8C);
            int used = it->total ? (int)((it->total - it->free) * (unsigned long long)(bw - 2) / it->total) : 0;
            if (used > 0) gfx_gradient(win, bx + 1, by + 1, used, bh - 2, 0x74A9F4, 0x2860C8);
        }
        if (!n) gfx_text(win, vx + 14, vy + 40, "No drives found.", 0x6D6D6D, FONT_UI);
    } else {
        for (int i = 0; i < n; i++) {
            hal_rect_t r = item_rect(i);
            if (r.y + r.h < vy || r.y > vy + vh) continue;
            item_t *it = &items[i];
            int s = it->selected || i == drop_hl;
            int ix = r.x + (r.w - 32) / 2;
            if (s) gfx_shade(win, ix, r.y + 4, 32, 32, th.select, (unsigned)it->icon, SHADE_ICON_TINTED, 100);
            else gfx_icon(win, ix, r.y + 4, it->icon);
            char l1[MAX_NAME + 1], l2[MAX_NAME + 1];
            int maxw = r.w - 4;
            split_name(it->name, maxw, l1, l2);
            int ty = r.y + 40, fx = r.x + r.w, fx1 = r.x, lines = 0;
            for (int line = 0; line < 2; line++) {
                const char *t = line ? l2 : l1;
                if (!*t) continue;
                int tw = MIN(text_width(t, FONT_UI), maxw);
                int tx = r.x + (r.w - tw) / 2, tyy = ty + line * 13;
                if (s) gfx_fill(win, tx - 2, tyy, tw + 4, 13, th.select);
                gfx_text_box(win, tx, tyy, tw, 13, t, s ? th.select_text : 0x000000, FONT_UI, 1);
                fx = MIN(fx, tx - 2); fx1 = MAX(fx1, tx + tw + 2); lines = line + 1;
            }
            if (i == focus && lines) focus_rect(fx, ty, fx1 - fx, lines * 13);
        }
        if (!n) gfx_text(win, vx + 14, vy + 14, binview ? "The Recycle Bin is empty." : "This folder is empty.", 0x6D6D6D, FONT_UI);
    }
    gfx_clip_reset(win);
    if (band_on) {
        int x, y, w, h;
        band_rect(&x, &y, &w, &h);
        blend_rect(x, y, w, h, th.select, 60);
        gfx_clip(win, vx, vy + (view == 1 ? 22 : 0), vw - SB_W, vh - (view == 1 ? 22 : 0));
        gfx_rect(win, x, y, MAX(1, w), MAX(1, h), th.select);
        gfx_clip_reset(win);
    }
    ctl_scroll_set(win, ID_VSB, content_height(), vh, scroll, 40);
}

static void paint_status(void) {
    int sy = ch - STATUS_H;
    gfx_fill(win, 0, sy, cw, STATUS_H, th.face);
    hline(0, sy, cw, lighten(th.face, -30));
    char st[MAX_NAME + 96], right[64] = "";
    if (!path[0] && !binview) {
        if (nsel) snprintf(st, sizeof st, "%d of %d drive%s selected", nsel, n, n == 1 ? "" : "s");
        else snprintf(st, sizeof st, "%d drive%s", n, n == 1 ? "" : "s");
    } else if (nsel) {
        int files, dirs;
        unsigned long long b = sel_bytes(&files, &dirs);
        snprintf(st, sizeof st, "%d object%s selected", nsel, nsel == 1 ? "" : "s");
        if (files) format_size(b, right, sizeof right);
    } else {
        snprintf(st, sizeof st, "%d object%s", n, n == 1 ? "" : "s");
        if (binview) {
            unsigned long long b = 0;
            for (int i = 0; i < n; i++) b += items[i].size;
            format_size(b, right, sizeof right);
        } else {
            char f[32];
            format_size(disk_free, f, sizeof f);
            snprintf(right, sizeof right, "Disk free space: %s", f);
        }
    }
    int rw = right[0] ? MIN(220, text_width(right, FONT_UI) + 24) : 0;
    gfx_text_box(win, 8, sy + 1, cw - 16 - rw, STATUS_H - 1, st, 0x000000, FONT_UI, 0);
    if (rw) {
        vline(cw - rw - 4, sy + 3, STATUS_H - 6, lighten(th.face, -40));
        gfx_text_box(win, cw - rw + 4, sy + 1, rw - 12, STATUS_H - 1, right, 0x000000, FONT_UI, 0);
    }
}

static void paint(void) {
    gfx_theme(&th);
    compute_view();
    scroll = CLAMP(scroll, 0, max_scroll());
    gfx_clip_reset(win);
    paint_toolbar();
    paint_pane();
    paint_view();
    paint_status();
    win_invalidate(win);
}

/* ---------------- menus ---------------- */
static const hal_menu_item_t bar[] = {
    { "&File", 0, 0, MENU_TITLE, 0, 0 },
    { "&Open", "Enter", CMD_OPEN, MENU_BOLD, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "New &Folder", 0, CMD_NEWFOLDER, 0, HICON_FOLDER, 0 },
    { "New &Text Document", 0, CMD_NEWTEXT, 0, HICON_FILE_TEXT, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "&Delete", "Del", CMD_DELETE, 0, HICON_RECYCLE, 0 },
    { "Delete &Permanently", "Shift+Del", CMD_DELETE_PERM, 0, 0, 0 },
    { "Rena&me", "F2", CMD_RENAME, 0, 0, 0 },
    { "P&roperties", 0, CMD_PROPS, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "&Close", 0, CMD_CLOSE, 0, 0, 0 },
    { "&Edit", 0, 0, MENU_TITLE, 0, 0 },
    { "Cu&t", "Ctrl+X", CMD_CUT, 0, 0, 0 },
    { "&Copy", "Ctrl+C", CMD_COPY, 0, 0, 0 },
    { "&Paste", "Ctrl+V", CMD_PASTE, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "Select &All", "Ctrl+A", CMD_SELECT_ALL, 0, 0, 0 },
    { "&Invert Selection", 0, CMD_INVERT, 0, 0, 0 },
    { "&View", 0, 0, MENU_TITLE, 0, 0 },
    { "Ico&ns", 0, CMD_VIEW_ICONS, MENU_CHECKED, 0, 0 },
    { "&Details", 0, CMD_VIEW_DETAILS, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "&Task Pane", 0, CMD_PANE, MENU_CHECKED, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "Arrange by &Name", 0, CMD_SORT0, MENU_CHECKED, 0, 0 },
    { "Arrange by &Size", 0, CMD_SORT1, 0, 0, 0 },
    { "Arrange by T&ype", 0, CMD_SORT2, 0, 0, 0 },
    { "Arrange by &Modified", 0, CMD_SORT3, 0, 0, 0 },
    { "D&escending", 0, CMD_SORT_DESC, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "&Refresh", "F5", CMD_REFRESH, 0, 0, 0 },
    { "&Go", 0, 0, MENU_TITLE, 0, 0 },
    { "&Back", "Alt+Left", CMD_BACK, 0, 0, 0 },
    { "&Up One Level", "Backspace", CMD_UP, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "My &Computer", 0, CMD_MYCOMP, 0, HICON_COMPUTER, 0 },
    { "My &Documents", 0, CMD_MYDOCS, 0, HICON_DOCUMENTS, 0 },
    { "&Recycle Bin", 0, CMD_RECYCLEBIN, 0, HICON_RECYCLE, 0 },
    { "&Help", 0, 0, MENU_TITLE, 0, 0 },
    { "&About Halcyon", 0, CMD_ABOUT, 0, HICON_HELP, 0 },
};

/* the Recycle Bin's own menus */
static const hal_menu_item_t binbar[] = {
    { "&File", 0, 0, MENU_TITLE, 0, 0 },
    { "R&estore", 0, CMD_RESTORE, MENU_BOLD, HICON_BACK, 0 },
    { "&Delete", "Del", CMD_DELETE, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "Empty Recycle &Bin", 0, CMD_EMPTY, 0, HICON_RECYCLE, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "P&roperties", 0, CMD_PROPS, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "&Close", 0, CMD_CLOSE, 0, 0, 0 },
    { "&Edit", 0, 0, MENU_TITLE, 0, 0 },
    { "Select &All", "Ctrl+A", CMD_SELECT_ALL, 0, 0, 0 },
    { "&Invert Selection", 0, CMD_INVERT, 0, 0, 0 },
    { "&View", 0, 0, MENU_TITLE, 0, 0 },
    { "Ico&ns", 0, CMD_VIEW_ICONS, MENU_CHECKED, 0, 0 },
    { "&Details", 0, CMD_VIEW_DETAILS, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "&Task Pane", 0, CMD_PANE, MENU_CHECKED, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "Arrange by &Name", 0, CMD_SORT0, MENU_CHECKED, 0, 0 },
    { "Arrange by &Original Location", 0, CMD_SORT1, 0, 0, 0 },
    { "Arrange by Date De&leted", 0, CMD_SORT2, 0, 0, 0 },
    { "Arrange by &Size", 0, CMD_SORT3, 0, 0, 0 },
    { "D&escending", 0, CMD_SORT_DESC, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "&Refresh", "F5", CMD_REFRESH, 0, 0, 0 },
    { "&Go", 0, 0, MENU_TITLE, 0, 0 },
    { "&Back", "Alt+Left", CMD_BACK, 0, 0, 0 },
    { "&Up One Level", "Backspace", CMD_UP, 0, 0, 0 },
    { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
    { "My &Computer", 0, CMD_MYCOMP, 0, HICON_COMPUTER, 0 },
    { "My &Documents", 0, CMD_MYDOCS, 0, HICON_DOCUMENTS, 0 },
    { "&Help", 0, 0, MENU_TITLE, 0, 0 },
    { "&About Halcyon", 0, CMD_ABOUT, 0, HICON_HELP, 0 },
};

static int clip_shown, bar_bin = -1;        /* whether the pane and menus offer Paste; which bar is up */
static void update_menus(void) {
    if (bar_bin != binview) {
        bar_bin = binview;
        if (binview) menu_set(win, binbar, N(binbar)); else menu_set(win, bar, N(bar));
    }
    clip_shown = clip->count > 0;
    int one = sel_single();
    menu_enable(win, CMD_BACK, nback > 0);
    menu_enable(win, CMD_UP, path[0] != 0 || binview);
    menu_enable(win, CMD_PASTE, path[0] && clip->count > 0);
    menu_enable(win, CMD_CUT, path[0] && nsel);
    menu_enable(win, CMD_COPY, path[0] && nsel);
    menu_enable(win, CMD_DELETE, (path[0] || binview) && nsel);
    menu_enable(win, CMD_DELETE_PERM, path[0] && nsel);
    menu_enable(win, CMD_RENAME, path[0] && one >= 0);
    menu_enable(win, CMD_OPEN, !binview && nsel);
    menu_enable(win, CMD_NEWFOLDER, path[0] != 0);
    menu_enable(win, CMD_NEWTEXT, path[0] != 0);
    menu_enable(win, CMD_RESTORE, nsel > 0);
    menu_enable(win, CMD_EMPTY, n > 0);
    menu_enable(win, CMD_SELECT_ALL, n > 0);
    menu_enable(win, CMD_INVERT, n > 0);
    menu_check(win, CMD_VIEW_ICONS, view == 0);
    menu_check(win, CMD_VIEW_DETAILS, view == 1);
    menu_check(win, CMD_PANE, pane_on);
    for (int i = 0; i < 4; i++) {
        menu_check(win, CMD_SORT0 + i, sort_col == i);
        menu_enable(win, CMD_SORT0 + i, !IS_COMPUTER);
    }
    menu_check(win, CMD_SORT_DESC, sort_desc);
    menu_enable(win, CMD_SORT_DESC, !IS_COMPUTER);
}

static void context_menu(int i, int x, int y) {
    if (binview) {
        if (i >= 0) {
            hal_menu_item_t m[] = {
                { "R&estore", 0, CMD_RESTORE, MENU_BOLD, HICON_BACK, 0 },
                { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
                { "&Delete", 0, CMD_DELETE, 0, 0, 0 },
                { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
                { "P&roperties", 0, CMD_PROPS, 0, 0, 0 },
            };
            menu_popup(win, m, N(m), x, y);
        } else {
            hal_menu_item_t m[] = {
                { "View: &Icons", 0, CMD_VIEW_ICONS, view == 0 ? MENU_CHECKED : 0, 0, 0 },
                { "View: &Details", 0, CMD_VIEW_DETAILS, view == 1 ? MENU_CHECKED : 0, 0, 0 },
                { "R&efresh", 0, CMD_REFRESH, 0, 0, 0 },
                { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
                { "Empty Recycle &Bin", 0, CMD_EMPTY, n ? 0 : MENU_DISABLED, HICON_RECYCLE, 0 },
                { "Restore &All", 0, CMD_RESTORE_ALL, n ? 0 : MENU_DISABLED, 0, 0 },
                { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
                { "P&roperties", 0, CMD_RBPROPS, 0, 0, 0 },
            };
            menu_popup(win, m, N(m), x, y);
        }
    } else if (i >= 0 && !path[0]) {
        item_t *it = &items[i];
        int cd = it->kind == VOL_CDROM;
        hal_menu_item_t m[] = {
            { "&Open", 0, CMD_OPEN, MENU_BOLD, 0, 0 },
            { "&Eject", 0, CMD_EJECT, cd ? 0 : MENU_DISABLED, HICON_CDROM, 0 },     /* CD drives only */
            { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
            { "Form&at...", 0, CMD_FORMAT, (cd || it->is_boot || it->readonly) ? MENU_DISABLED : 0, 0, 0 },
            { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
            { "P&roperties", 0, CMD_PROPS, 0, 0, 0 },
        };
        menu_popup(win, m, N(m), x, y);
    } else if (i >= 0) {
        int one = sel_single();
        hal_menu_item_t m[] = {
            { "&Open", 0, CMD_OPEN, MENU_BOLD, 0, 0 },
            { "Open with &Notepad", 0, CMD_OPEN_NOTEPAD, one >= 0 && !items[one].is_dir ? 0 : MENU_DISABLED, HICON_NOTEPAD, 0 },
            { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
            { "Cu&t", 0, CMD_CUT, 0, 0, 0 },
            { "&Copy", 0, CMD_COPY, 0, 0, 0 },
            { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
            { "&Delete", 0, CMD_DELETE, 0, 0, 0 },
            { "Rena&me", 0, CMD_RENAME, one >= 0 ? 0 : MENU_DISABLED, 0, 0 },
            { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
            { "P&roperties", 0, CMD_PROPS, 0, 0, 0 },
        };
        menu_popup(win, m, N(m), x, y);
    } else if (path[0]) {
        hal_menu_item_t m[] = {
            { "View: &Icons", 0, CMD_VIEW_ICONS, view == 0 ? MENU_CHECKED : 0, 0, 0 },
            { "View: &Details", 0, CMD_VIEW_DETAILS, view == 1 ? MENU_CHECKED : 0, 0, 0 },
            { "R&efresh", 0, CMD_REFRESH, 0, 0, 0 },
            { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
            { "&Paste", 0, CMD_PASTE, clip->count ? 0 : MENU_DISABLED, 0, 0 },
            { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
            { "New &Folder", 0, CMD_NEWFOLDER, 0, HICON_FOLDER, 0 },
            { "New &Text Document", 0, CMD_NEWTEXT, 0, HICON_FILE_TEXT, 0 },
            { 0, 0, 0, MENU_SEPARATOR, 0, 0 },
            { "Command Prompt &Here", 0, CMD_TERMINAL, 0, HICON_TERMINAL, 0 },
            { "P&roperties", 0, CMD_PROPS, 0, 0, 0 },
        };
        menu_popup(win, m, N(m), x, y);
    }
}

/* ---------------- dialogs ---------------- */
/* wrapped text as the kernel's labels draw it: lines of at most width px */
static int wrap_text(int w, const char *s, int width, int x, int y, int draw) {
    int y0 = y, lh = text_height(FONT_UI) + 1;
    int len = (int)strlen(s), p = 0;
    while (p < len) {
        int k = 0, wd = 0, last_space = -1;
        while (p + k < len && s[p + k] != '\n') {
            int q = p + k;
            int cw2 = text_char_width(utf8_get(s, len, &q), FONT_UI);
            if (wd + cw2 > width && k > 0) break;
            if (s[p + k] == ' ') last_space = k;
            wd += cw2;
            k = q - p;
        }
        int brk = k;
        if (p + k < len && s[p + k] != '\n' && last_space > 0) brk = last_space;
        if (draw) gfx_text_n(w, x, y, s + p, brk, 0x000000, FONT_UI);
        y += lh;
        p += brk;
        if (p < len && (s[p] == ' ' || s[p] == '\n')) p++;
    }
    return y - y0;
}

/* a message box with any icon and up to four buttons (labels); returns the
 * index of the button chosen, or the last one (Cancel) for Esc / close */
static int ask(const char *title, const char *text, int icon, const char *const *labels, int nb, int def) {
    int tw = text_width(text, FONT_UI);
    int textw = CLAMP(tw + 10, 220, 420);
    int texth = wrap_text(win, text, textw, 0, 0, 0);
    int bw = 0;
    for (int i = 0; i < nb; i++) bw += MAX(75, text_width(labels[i], FONT_UI) + 20) + 8;
    int dw = MAX(textw + 76, bw + 20), dh = MAX(texth, 36) + 72;
    int d = win_create(title, dw, dh, WIN_DIALOG);
    if (d < 0) return nb - 1;
    win_set_icon(d, icon);
    int x = dw - bw - 4;
    for (int i = 0; i < nb; i++) {
        int w = MAX(75, text_width(labels[i], FONT_UI) + 20);
        ctl_create(d, CTL_BUTTON, x, dh - 36, w, 23, labels[i], 1 + i, i == def ? CTL_DEFAULT : i == nb - 1 ? CTL_CANCEL : 0);
        x += w + 8;
    }
    ctl_focus(d, 1 + def);
    if (win >= 0) win_set_owner(d, win, 1);
    int result = -1;
    while (result < 0) {
        hal_event_t ev;
        if (!win_get_event(&ev, -1)) continue;
        if (ev.win != d) { passive_event(&ev); continue; }
        if (ev.type == EVT_PAINT) {
            gfx_fill(d, 0, 0, dw, dh, th.face);
            gfx_icon(d, 16, 16, icon);
            wrap_text(d, text, textw, 62, 16 + (texth < 32 ? (32 - texth) / 2 : 0), 1);
            win_invalidate(d);
        } else if (ev.type == EVT_COMMAND && ev.id >= 1 && ev.id <= nb) result = ev.id - 1;
        else if (ev.type == EVT_CLOSE || (ev.type == EVT_KEY && ev.pressed && ev.id == KEY_ESC)) result = nb - 1;
        else if (ev.type == EVT_KEY && ev.pressed && (ev.id == KEY_ENTER || ev.id == KEY_KP_ENTER)) result = ctl_get_focus(d) - 1 >= 0 ? ctl_get_focus(d) - 1 : def;
    }
    win_destroy(d);
    return result;
}

/* a message box with any icon (Properties shows the item's own icon) */
static void info_box(const char *title, const char *text, int icon) {
    static const char *const ok[] = { "OK" };
    ask(title, text, icon, ok, 1, 0);
}

static int yes_no(const char *title, const char *text, int icon) {
    static const char *const yn[] = { "Yes", "No" };
    return ask(title, text, icon, yn, 2, 0) == 0;
}

/* ---------------- commands ---------------- */
static void report(const char *what, int err) {
    char msg[MAX_NAME + 160];
    snprintf(msg, sizeof msg, "%s\n\n%s", what, errstr(err));
    msgbox(win, "Error", msg, MBX_ICON_ERROR, MBX_OK);
}

static void cmd_new(int folder) {
    if (!path[0]) return;
    char p[MAX_PATH];
    unique_name(path, folder ? "New Folder" : "New Text Document.txt", p, sizeof p);
    char nm[MAX_NAME + 1];
    strlcpy(nm, path_base(p), sizeof nm);
    if (!inputbox(win, folder ? "New Folder" : "New Text Document", folder ? "Name of the new folder:" : "Name of the new document:", nm, sizeof nm) || !nm[0]) return;
    path_join(path, nm, p, sizeof p);
    int r;
    if (folder) r = mkdir(p);
    else {
        r = open(p, O_WRITE | O_CREATE | O_TRUNC);
        if (r >= 0) r = close(r);
    }
    if (r < 0) { report(folder ? "Cannot create the folder." : "Cannot create the file.", r); return; }
    load();
    char one[1][MAX_PATH];
    strlcpy(one[0], p, MAX_PATH);
    select_names(one, 1);
}

static void cmd_rename(void) {
    int i = sel_single();
    if (!path[0] || i < 0) return;
    char nm[MAX_NAME + 1];
    strlcpy(nm, items[i].name, sizeof nm);
    if (!inputbox(win, "Rename", "New name:", nm, sizeof nm) || !nm[0] || !strcmp(nm, items[i].name)) return;
    char one[1][MAX_PATH], from[MAX_PATH];
    item_path(i, from, sizeof from);
    path_join(path, nm, one[0], MAX_PATH);
    int r = rename(from, one[0]);
    if (r < 0) { report("Cannot rename the item.", r); return; }
    load();
    select_names(one, 1);
}

/* "This folder already contains...": 0 replace, 1 replace all, 2 skip, 3 cancel */
static int ask_replace(const char *name, int is_dir, int moving) {
    char msg[MAX_NAME + 400];
    if (is_dir)
        snprintf(msg, sizeof msg, "This folder already contains a folder named '%s'.\n\nIf the files in the existing folder have the "
                 "same names as files in the folder you are %s, they will be replaced. Do you still want to %s the folder?",
                 name, moving ? "moving" : "copying", moving ? "move" : "copy");
    else snprintf(msg, sizeof msg, "This folder already contains a file named '%s'.\n\nWould you like to replace the existing file?", name);
    static const char *const b[] = { "Yes", "Yes to All", "No", "Cancel" };
    return ask(is_dir ? "Confirm Folder Replace" : "Confirm File Replace", msg, HICON_QUESTION, b, 4, 0);
}

/* copy or move a list of paths into folder dir, asking before anything is
 * replaced; the new items are selected when dir is this folder */
static void transfer(const char *list, int count, const char *dir, int kind) {
    hal_volinfo_t vi;
    if (volume_info(dir[0], &vi) == 0 && vi.readonly) {
        char msg[300];
        snprintf(msg, sizeof msg, "Nothing can be %s to %c: because the drive is read-only.\n\n%s", kind == FOP_MOVE ? "moved" : "copied",
                 dir[0], vi.ro_reason[0] ? vi.ro_reason : "");
        msgbox(win, "Explorer", msg, MBX_ICON_WARNING, MBX_OK);
        return;
    }
    fileop_t *op = op_new(kind, count);
    if (!op) return;
    int k = 0, all = 0, cancel = 0;
    const char *p = list;
    for (int i = 0; i < count && *p && !cancel; i++, p += strlen(p) + 1) {
        char srcdir[MAX_PATH], *dst = op->dst[k];
        const char *base = path_base(p);
        path_dir(p, srcdir, sizeof srcdir);
        int same = !strcasecmp(srcdir, dir);
        if (same && kind == FOP_MOVE) continue;                /* already here */
        size_t sl = strlen(p);
        if (prefix_nocase(p, dir, sl) && (dir[sl] == '/' || !dir[sl])) {
            char msg[MAX_NAME + 120];
            snprintf(msg, sizeof msg, "Cannot %s '%s': the destination folder is inside it.", kind == FOP_MOVE ? "move" : "copy", base);
            msgbox(win, "Explorer", msg, MBX_ICON_ERROR, MBX_OK);
            continue;
        }
        if (same) {
            char cand[MAX_NAME + 16];
            snprintf(cand, sizeof cand, "Copy of %s", base);
            unique_name(dir, cand, dst, MAX_PATH);
        } else {
            hal_dirent_t st, ss;
            path_join(dir, base, dst, MAX_PATH);
            if (stat(dst, &st) == 0) {
                int a = all ? 0 : ask_replace(base, st.is_dir && stat(p, &ss) == 0 && ss.is_dir, kind == FOP_MOVE);
                if (a == 3) { cancel = 1; break; }
                if (a == 2) continue;
                if (a == 1) all = 1;
                op->replace[k] = 1;
            }
        }
        strlcpy(op->src[k], p, MAX_PATH);
        k++;
    }
    op->n = cancel ? 0 : k;
    int r = fileop_run(op);
    if (!quit) {
        if (r < 0 && r != E_CANCELED) report(kind == FOP_MOVE ? "Cannot move the items." : "Cannot copy the items.", r);
        if (!binview && path[0] && !strcasecmp(dir, path)) { load(); select_names(op->dst, op->n); }
        else reload_keep();
    }
    op_free(op);
}

/* move paths to the Recycle Bin (perm: delete them for good), after the questions Windows asks */
static void delete_paths(const char *list, int count, int perm) {
    if (!count) return;
    char drive = list[0];
    hal_rbinfo_t ri;
    hal_volinfo_t vi;
    if (recycle_info(drive, &ri) < 0) ri.supported = E_NODEV;
    if (ri.supported == E_ROFS || ri.supported == E_NODEV) {
        char msg[400];
        int ok = volume_info(drive, &vi) == 0;
        snprintf(msg, sizeof msg, "Nothing on %c: can be deleted: the drive is read-only.\n\n%s", drive,
                 ok && vi.ro_reason[0] ? vi.ro_reason : "");
        msgbox(win, "Cannot Delete", msg, MBX_ICON_WARNING, MBX_OK);
        return;
    }
    int tobin = !perm && ri.supported == 0 && !(ri.flags & RBF_NUKE);
    hal_dirent_t st;
    int isdir = count == 1 && stat(list, &st) == 0 && st.is_dir;
    char msg[MAX_NAME + 200];
    const char *title = count > 1 ? "Confirm Multiple File Delete" : isdir ? "Confirm Folder Delete" : "Confirm File Delete";
    if (tobin) {
        if (count == 1) snprintf(msg, sizeof msg, isdir ? "Are you sure you want to remove the folder '%s' and move all its contents to the Recycle Bin?"
                                                        : "Are you sure you want to send '%s' to the Recycle Bin?", path_base(list));
        else snprintf(msg, sizeof msg, "Are you sure you want to send these %d items to the Recycle Bin?", count);
        if ((ri.flags & RBF_CONFIRM) && !yes_no(title, msg, HICON_RECYCLE)) return;
    } else {
        const char *why = ri.supported == E_NOTSUP ? "\n\nThis drive has no Recycle Bin: the items cannot be restored." : "";
        if (count == 1) snprintf(msg, sizeof msg, isdir ? "Are you sure you want to permanently delete the folder '%s' and all of its contents?%s"
                                                        : "Are you sure you want to permanently delete '%s'?%s", path_base(list), why);
        else snprintf(msg, sizeof msg, "Are you sure you want to permanently delete these %d items?%s", count, why);
        if (!yes_no(title, msg, HICON_WARNING)) return;
    }
    fileop_t *op = op_new(tobin ? FOP_RECYCLE : FOP_DELETE, count);
    if (!op) return;
    const char *p = list;
    for (int i = 0; i < count && *p; i++, p += strlen(p) + 1) strlcpy(op->src[op->n++], p, MAX_PATH);
    int r = fileop_run(op);
    if (quit) { op_free(op); return; }
    if (tobin) {
        /* too big for the bin (or no bin after all): ask, then delete those for good */
        fileop_t *big = op_new(FOP_DELETE, op->n);
        int err = 0;
        for (int i = 0; big && i < op->n; i++) {
            int e = op->res[i];
            if (e == E_NOSPC || e == E_NOTSUP) strlcpy(big->src[big->n++], op->src[i], MAX_PATH);
            else if (e < 0 && !err) err = e;
        }
        if (big && big->n) {
            if (big->n == 1) snprintf(msg, sizeof msg, "'%s' is too big for the Recycle Bin.\n\nDo you want to permanently delete it?", path_base(big->src[0]));
            else snprintf(msg, sizeof msg, "%d of the items are too big for the Recycle Bin.\n\nDo you want to permanently delete them?", big->n);
            if (yes_no("Delete", msg, HICON_WARNING)) r = fileop_run(big);
        }
        op_free(big);
        if (err && r >= 0) r = err;
    }
    if (!quit && r < 0 && r != E_CANCELED) report("Cannot delete the items.", r);
    op_free(op);
    if (!quit) reload_keep();
}

static void cmd_delete(int perm) {
    if (binview) {
        /* entries of the bin: for good */
        if (!nsel) return;
        int one = sel_single();
        char msg[MAX_NAME + 120];
        if (one >= 0) snprintf(msg, sizeof msg, "Are you sure you want to permanently delete '%s'?", items[one].name);
        else snprintf(msg, sizeof msg, "Are you sure you want to permanently delete these %d items?", nsel);
        if (!yes_no(nsel > 1 ? "Confirm Multiple File Delete" : "Confirm File Delete", msg, HICON_WARNING)) return;
        fileop_t *op = op_new(FOP_PURGE, nsel);
        for (int i = 0; op && i < n; i++) {
            if (!items[i].selected) continue;
            snprintf(op->src[op->n], MAX_PATH, "%c:%s", items[i].letter, items[i].name);
            op->ids[op->n++] = items[i].rbid;
        }
        int r = op ? fileop_run(op) : E_NOMEM;
        op_free(op);
        if (quit) return;
        if (r < 0 && r != E_CANCELED) report("Cannot delete the items.", r);
        reload_keep();
        return;
    }
    if (!path[0] || !nsel) return;
    int len, count;
    char *list = selected_paths(&len, &count);
    if (list) delete_paths(list, count, perm);
    free(list);
}

static void cmd_copy(int cut) {
    if (!path[0] || !nsel) return;
    int len, count;
    char *list = selected_paths(&len, &count);
    if (!list) return;
    if (len + 1 > CLIP_BYTES) { msgbox(win, "Explorer", "Too many items to copy at once.", MBX_ICON_WARNING, MBX_OK); free(list); return; }
    clip->count = 0;
    memcpy(clip->path, list, (size_t)len + 1);
    clip->cut = cut;
    clip->seq++;
    clip->count = count;
    free(list);
    update_menus();
    paint();
}

static void cmd_paste(void) {
    int count = clip->count, cut = clip->cut;
    if (!path[0] || count <= 0) return;
    char *list = malloc(CLIP_BYTES);
    if (!list) return;
    memcpy(list, clip->path, CLIP_BYTES);
    list[CLIP_BYTES - 1] = list[CLIP_BYTES - 2] = 0;
    unsigned seq = clip->seq;
    /* items that are gone are left out */
    int alive = 0;
    for (const char *p = list; *p; p += strlen(p) + 1) { hal_dirent_t st; if (stat(p, &st) == 0) alive++; }
    if (!alive) {
        msgbox(win, "Paste", "The copied items no longer exist.", MBX_ICON_WARNING, MBX_OK);
        if (clip->seq == seq) clip->count = 0;
        free(list);
        update_menus();
        return;
    }
    transfer(list, count, path, cut ? FOP_MOVE : FOP_COPY);
    if (cut && clip->seq == seq) { clip->count = 0; clip->seq++; }   /* moved: nothing left to paste */
    free(list);
    update_menus();
}

static void cmd_select_all(int invert) {
    for (int i = 0; i < n; i++) sel_set(i, invert ? !items[i].selected : 1);
    if (focus < 0 && n) focus = anchor = 0;
    update_menus();
    paint();
}

/* ---------------- drive properties ---------------- */
enum { ID_DP_WRITE = 500, ID_DP_OK, ID_DP_CANCEL };

static const char write_warning[] =
    "Halcyon can write to this NTFS drive, but it is an internal disk with Windows on it.\n\n"
    "- Writing to it from Halcyon could damage files that Windows needs. Keep backups of "
    "everything important on it.\n"
    "- Shut Windows down completely before you start Halcyon: with Fast Startup "
    "(or hibernation) Windows keeps the drive in use, and Halcyon then keeps it read-only.\n\n"
    "Do you want Halcyon to write to this drive?";

typedef struct dprops {
    int d, w, h;
    hal_volinfo_t vi;
    int bar_y, sep1, sep2, warn_y, icon;
} dprops_t;

static void dp_paint(const dprops_t *p) {
    int d = p->d, W = p->w;
    gfx_fill(d, 0, 0, W, p->h, th.face);
    gfx_icon(d, 14, 12, p->icon);
    for (int i = 0; i < 2; i++) {
        int y = i ? p->sep2 : p->sep1;
        gfx_line(d, 14, y, W - 15, y, 0xACA899);
        gfx_line(d, 14, y + 1, W - 15, y + 1, 0xFFFFFF);
    }
    /* colour keys of the used / free lines, and the bar */
    gfx_fill(d, 14, p->bar_y - 66, 12, 12, 0x0000FF);
    gfx_rect(d, 14, p->bar_y - 66, 12, 12, 0x000000);
    gfx_fill(d, 14, p->bar_y - 48, 12, 12, 0xFF00FF);
    gfx_rect(d, 14, p->bar_y - 48, 12, 12, 0x000000);
    int bw = W - 28;
    int used = p->vi.total ? (int)((p->vi.total - p->vi.free_bytes) * (unsigned long long)(bw - 4) / p->vi.total) : 0;
    gfx_bevel(d, 14, p->bar_y, bw, 16, 0);
    gfx_fill(d, 16, p->bar_y + 2, bw - 4, 12, 0xFF00FF);
    if (used > 0) gfx_fill(d, 16, p->bar_y + 2, MIN(used, bw - 4), 12, 0x0000FF);
    if (p->warn_y) gfx_icon(d, 14, p->warn_y, HICON_WARNING);
    win_invalidate(d);
}

static void dp_row(int d, int y, const char *k, const char *v) {
    ctl_create(d, CTL_LABEL, 14, y, 96, 16, k, 0, 0);
    ctl_create(d, CTL_LABEL, 112, y, 220, 16, v, 0, 0);
}

static void dp_size(int d, int y, const char *k, unsigned long long v) {
    char a[48], b[48];
    format_size_exact(v, a, sizeof a);
    format_size(v, b, sizeof b);
    ctl_create(d, CTL_LABEL, 34, y, 80, 16, k, 0, 0);
    ctl_create(d, CTL_LABEL, 120, y, 140, 16, a, 0, 0);
    ctl_create(d, CTL_LABEL, 262, y, 70, 16, b, 0, 0);
}

/* the General page of a drive's properties: type, file system, space, and
 * for read-only drives why; NTFS drives on internal disks can be allowed
 * writing here (hal_volinfo_t VOLF_WRITE_OPT) */
static void drive_props(char letter, const char *model) {
    dprops_t p;
    memset(&p, 0, sizeof p);
    if (volume_info(letter, &p.vi) < 0) return;
    hal_volinfo_t *vi = &p.vi;
    int ro = vi->readonly && vi->kind != VOL_CDROM, opt = (vi->flags & VOLF_WRITE_OPT) != 0;
    int was = (vi->flags & VOLF_WRITE_OK) != 0;
    p.w = 346;
    p.h = 330 + (ro ? 72 : 0) + (opt ? 28 : 0);
    p.icon = drive_icon(vi->kind);
    char name[96], title[128], buf[128];
    snprintf(name, sizeof name, "%s (%c:)", vi->label[0] ? vi->label : drive_kind(vi->kind), vi->letter);
    snprintf(title, sizeof title, "%s Properties", name);
    int d = win_create(title, p.w, p.h, WIN_DIALOG);
    if (d < 0) return;
    p.d = d;
    win_set_icon(d, drive_icon(vi->kind));
    ctl_create(d, CTL_LABEL, 62, 20, p.w - 76, 16, name, 0, CTL_BOLD);
    p.sep1 = 54;
    int y = 66;
    dp_row(d, y, "Type:", drive_kind(vi->kind)); y += 20;
    dp_row(d, y, "File system:", vi->fs[0] ? vi->fs : "?"); y += 20;
    snprintf(buf, sizeof buf, "%s (%s)", model && model[0] ? model : "?", vi->device[0] ? vi->device : "?");
    dp_row(d, y, "Device:", buf); y += 20;
    if (vi->serial) {
        snprintf(buf, sizeof buf, "%04X-%04X", (unsigned)(vi->serial >> 16) & 0xFFFF, (unsigned)vi->serial & 0xFFFF);
        dp_row(d, y, "Serial number:", buf);
    }
    y += 26;
    p.sep2 = y;
    y += 12;
    dp_size(d, y, "Used space:", vi->total - vi->free_bytes); y += 18;
    dp_size(d, y, "Free space:", vi->free_bytes); y += 24;
    dp_size(d, y, "Capacity:", vi->total); y += 26;
    p.bar_y = y;
    y += 28;
    if (ro) {
        p.warn_y = y;
        ctl_create(d, CTL_LABEL, 56, y, p.w - 70, 16, "This drive is read-only.", 0, CTL_BOLD);
        ctl_create(d, CTL_LABEL, 56, y + 16, p.w - 70, 54,
                   vi->ro_reason[0] ? vi->ro_reason : "The file system is mounted read-only.", 0, CTL_WRAP);
        y += 72;
    }
    if (opt) {
        ctl_create(d, CTL_CHECKBOX, 14, y, p.w - 28, 18, "Allow Halcyon to write to this drive", ID_DP_WRITE, 0);
        ctl_set_value(d, ID_DP_WRITE, was);
    }
    ctl_create(d, CTL_BUTTON, p.w - 2 * 83, p.h - 36, 75, 23, "OK", ID_DP_OK, CTL_DEFAULT);
    ctl_create(d, CTL_BUTTON, p.w - 83, p.h - 36, 75, 23, "Cancel", ID_DP_CANCEL, CTL_CANCEL);
    ctl_focus(d, ID_DP_OK);
    win_set_owner(d, win, 1);
    for (int done = 0; !done;) {
        hal_event_t ev;
        if (!win_get_event(&ev, -1)) continue;
        if (ev.win != d) { passive_event(&ev); continue; }
        if (ev.type == EVT_PAINT) dp_paint(&p);
        else if (ev.type == EVT_CLOSE || (ev.type == EVT_KEY && ev.pressed && ev.id == KEY_ESC)) done = 1;
        else if (ev.type == EVT_COMMAND && ev.id == ID_DP_CANCEL) done = 1;
        else if (ev.type == EVT_COMMAND && ev.id == ID_DP_WRITE) {
            /* ticking the box: say what it means first */
            if (ctl_get_value(d, ID_DP_WRITE) &&
                msgbox(d, "Allow Writing", write_warning, MBX_ICON_WARNING, MBX_YESNO) != IDR_YES)
                ctl_set_value(d, ID_DP_WRITE, 0);
        } else if ((ev.type == EVT_COMMAND && ev.id == ID_DP_OK) ||
                   (ev.type == EVT_KEY && ev.pressed && (ev.id == KEY_ENTER || ev.id == KEY_KP_ENTER))) {
            int want = opt ? ctl_get_value(d, ID_DP_WRITE) != 0 : was;
            done = 1;
            if (want != was) {
                int r = volume_set_write(letter, want);
                if (r < 0) {
                    char msg[300];
                    snprintf(msg, sizeof msg, "The setting could not be changed.\n\n%s",
                             r == E_BUSY ? "Files on the drive are open. Close them (and any window showing the "
                                           "drive's files from a program), then try again." : errstr(r));
                    msgbox(d, "Drive Properties", msg, MBX_ICON_WARNING, MBX_OK);
                    if (r == E_BUSY) done = 0;
                }
            }
        }
    }
    win_destroy(d);
    load();             /* (the read-only state and free space may have changed) */
}


/* ---------------- the Recycle Bin ---------------- */
static void set_cursor(int c);
enum { ID_RB_LIST = 600, ID_RB_PCT, ID_RB_PCTL, ID_RB_USED, ID_RB_NUKE, ID_RB_CONFIRM, ID_RB_OK, ID_RB_CANCEL };

typedef struct rbdrive { char drive; hal_rbinfo_t ri; int pct; char line[96]; } rbdrive_t;

static void rbp_show(int d, rbdrive_t *dv, int i) {
    char t[96], a[32], b[32];
    snprintf(t, sizeof t, "%d", dv[i].pct);
    ctl_set_text(d, ID_RB_PCT, t);
    format_size(dv[i].ri.total / 100 * (unsigned long long)dv[i].pct, a, sizeof a);
    snprintf(t, sizeof t, "percent of %c: (at most %s)", dv[i].drive, a);
    ctl_set_text(d, ID_RB_PCTL, t);
    format_size(dv[i].ri.bytes, a, sizeof a);
    format_size(dv[i].ri.limit, b, sizeof b);
    snprintf(t, sizeof t, "In the Recycle Bin now: %s of %s", a, b);
    ctl_set_text(d, ID_RB_USED, t);
}

/* how full the selected drive's bin is */
static void rbp_bar(int d, const rbdrive_t *dv, int nd, int cur, int W) {
    int bw = W - 28;
    gfx_fill(d, 14, 256, bw, 12, th.face);
    gfx_bevel(d, 14, 256, bw, 12, 0);
    if (nd && dv[cur].ri.limit) {
        int used = (int)MIN((unsigned long long)(bw - 4), dv[cur].ri.bytes * (unsigned long long)(bw - 4) / dv[cur].ri.limit);
        if (used > 0) gfx_gradient(d, 16, 258, used, 8, 0x74A9F4, 0x2860C8);
    }
    win_invalidate(d);
}

static int rbp_commit(int d, rbdrive_t *dv, int i) {
    char t[16];
    ctl_get_text(d, ID_RB_PCT, t, sizeof t);
    char *end;
    long v = strtol(t, &end, 10);
    if (!t[0] || *end || v < 0 || v > 100) {
        msgbox(d, "Recycle Bin Properties", "Type a number from 0 to 100: the share of the drive the Recycle Bin may use.", MBX_ICON_WARNING, MBX_OK);
        return 0;
    }
    dv[i].pct = (int)v;
    return 1;
}

/* Recycle Bin Properties: each drive's limit, and the options for all drives */
static void rb_props(void) {
    rbdrive_t dv[26];
    int nd = 0;
    const char *lines[26];
    for (char c = 'A'; c <= 'Z'; c++) {
        hal_rbinfo_t ri;
        hal_volinfo_t vi;
        if (volume_info(c, &vi) < 0 || recycle_info(c, &ri) < 0 || ri.supported) continue;
        rbdrive_t *e = &dv[nd];
        e->drive = c;
        e->ri = ri;
        e->pct = ri.percent;
        char s[32];
        format_size(ri.bytes, s, sizeof s);
        snprintf(e->line, sizeof e->line, "%c:  %s  -  %d item%s, %s", c, vi.label[0] ? vi.label : drive_kind(vi.kind), ri.count,
                 ri.count == 1 ? "" : "s", s);
        lines[nd] = e->line;
        nd++;
    }
    hal_rbinfo_t g;
    if (recycle_info(0, &g) < 0) g.flags = RBF_CONFIRM;
    int W = 390, H = 396;
    int d = win_create("Recycle Bin Properties", W, H, WIN_DIALOG);
    if (d < 0) return;
    win_set_icon(d, rb_full() ? HICON_RECYCLE_FULL : HICON_RECYCLE);
    ctl_create(d, CTL_LABEL, 62, 14, W - 76, 16, "Recycle Bin", 0, CTL_BOLD);
    ctl_create(d, CTL_LABEL, 62, 32, W - 76, 16, "Deleted files wait here until it is emptied.", 0, 0);
    ctl_create(d, CTL_LABEL, 14, 64, W - 28, 16, "Every drive has its own Recycle Bin:", 0, 0);
    ctl_create(d, CTL_LISTBOX, 14, 84, W - 28, 92, 0, ID_RB_LIST, 0);
    if (nd) ctl_list_set(d, ID_RB_LIST, lines, nd);
    else { const char *none[] = { "(no drive here can have a Recycle Bin)" }; ctl_list_set(d, ID_RB_LIST, none, 1); }
    ctl_create(d, CTL_LABEL, 14, 186, W - 28, 16, "Maximum size of the Recycle Bin of the selected drive:", 0, 0);
    ctl_create(d, CTL_TEXTBOX, 14, 206, 46, 22, "", ID_RB_PCT, nd ? 0 : CTL_DISABLED);
    ctl_create(d, CTL_LABEL, 68, 209, W - 82, 16, "", ID_RB_PCTL, 0);
    ctl_create(d, CTL_LABEL, 14, 238, W - 28, 16, "", ID_RB_USED, 0);
    ctl_create(d, CTL_CHECKBOX, 14, 286, W - 28, 18, "Do not move files to the Recycle Bin: delete them at once", ID_RB_NUKE, 0);
    ctl_create(d, CTL_CHECKBOX, 14, 310, W - 28, 18, "Display delete confirmation dialog", ID_RB_CONFIRM, 0);
    ctl_set_value(d, ID_RB_NUKE, (g.flags & RBF_NUKE) != 0);
    ctl_set_value(d, ID_RB_CONFIRM, (g.flags & RBF_CONFIRM) != 0);
    ctl_create(d, CTL_BUTTON, W - 2 * 83, H - 36, 75, 23, "OK", ID_RB_OK, CTL_DEFAULT);
    ctl_create(d, CTL_BUTTON, W - 83, H - 36, 75, 23, "Cancel", ID_RB_CANCEL, CTL_CANCEL);
    int cur = 0;
    if (nd) { ctl_set_value(d, ID_RB_LIST, 0); rbp_show(d, dv, 0); }
    ctl_focus(d, ID_RB_OK);
    if (win >= 0) win_set_owner(d, win, 1);
    for (int done = 0; !done;) {
        hal_event_t ev;
        if (!win_get_event(&ev, -1)) continue;
        if (ev.win != d) { passive_event(&ev); continue; }
        if (ev.type == EVT_PAINT) {
            gfx_theme(&th);
            gfx_fill(d, 0, 0, W, H, th.face);
            gfx_icon(d, 14, 12, rb_full() ? HICON_RECYCLE_FULL : HICON_RECYCLE);
            for (int k = 0; k < 2; k++) {
                int y = k ? 276 : 54;
                gfx_line(d, 14, y, W - 15, y, 0xACA899);
                gfx_line(d, 14, y + 1, W - 15, y + 1, 0xFFFFFF);
            }
            rbp_bar(d, dv, nd, cur, W);
        } else if (ev.type == EVT_CLOSE || (ev.type == EVT_KEY && ev.pressed && ev.id == KEY_ESC) ||
                   (ev.type == EVT_COMMAND && ev.id == ID_RB_CANCEL)) {
            done = 1;
        } else if (ev.type == EVT_COMMAND && ev.id == ID_RB_LIST && nd) {
            int s = ctl_get_value(d, ID_RB_LIST);
            if (s >= 0 && s < nd && s != cur) {
                if (!rbp_commit(d, dv, cur)) { ctl_set_value(d, ID_RB_LIST, cur); continue; }
                cur = s;
                rbp_show(d, dv, cur);
                rbp_bar(d, dv, nd, cur, W);
            }
        } else if ((ev.type == EVT_COMMAND && ev.id == ID_RB_OK) ||
                   (ev.type == EVT_KEY && ev.pressed && (ev.id == KEY_ENTER || ev.id == KEY_KP_ENTER))) {
            if (nd && !rbp_commit(d, dv, cur)) continue;
            int r = 0;
            for (int i = 0; i < nd; i++)
                if (dv[i].pct != dv[i].ri.percent) { int e = recycle_set(dv[i].drive, dv[i].pct, 0); if (e < 0 && !r) r = e; }
            unsigned flags = (ctl_get_value(d, ID_RB_NUKE) ? RBF_NUKE : 0) | (ctl_get_value(d, ID_RB_CONFIRM) ? RBF_CONFIRM : 0);
            if (flags != g.flags) { int e = recycle_set(0, 0, flags); if (e < 0 && !r) r = e; }
            if (r < 0) {
                char msg[160];
                snprintf(msg, sizeof msg, "The settings could not be saved.\n\n%s", errstr(r));
                msgbox(d, "Recycle Bin Properties", msg, MBX_ICON_WARNING, MBX_OK);
            }
            done = 1;
        }
    }
    win_destroy(d);
}

static void cmd_restore(int all) {
    int errors = 0, cancel = 0;
    for (int i = 0; i < n && !cancel; i++) {
        if (!all && !items[i].selected) continue;
        char full[MAX_PATH], out[MAX_PATH];
        item_path(i, full, sizeof full);
        int mode = RBR_FAIL;
        hal_dirent_t st;
        if (stat(full, &st) == 0) {
            char msg[MAX_PATH + 200];
            snprintf(msg, sizeof msg, "There is already %s named '%s' in '%s'.\n\nDo you want to replace it with the one from the Recycle Bin?",
                     st.is_dir ? "a folder" : "a file", items[i].name, items[i].orig ? items[i].orig : "");
            static const char *const b[] = { "Replace", "Keep Both", "Skip", "Cancel" };
            int a = ask("Confirm Restore", msg, HICON_QUESTION, b, 4, 1);
            if (a == 3) break;
            if (a == 2) continue;
            mode = a == 0 ? RBR_REPLACE : RBR_RENAME;
        }
        int r = recycle_restore(items[i].letter, items[i].rbid, mode, out, sizeof out);
        if (r < 0 && !errors++) {
            char msg[MAX_NAME + 100];
            snprintf(msg, sizeof msg, "Cannot restore '%s'.", items[i].name);
            report(msg, r);
        }
    }
    reload_keep();
}

static void cmd_empty(void) {
    /* every bin's entries (also from a folder window: Empty Recycle Bin in its pane) */
    int cnt = recycle_list(0, NULL, 0);
    if (cnt <= 0) return;
    hal_rbentry_t *e = malloc(sizeof *e * (size_t)(cnt + 8));
    if (!e) return;
    cnt = recycle_list(0, e, cnt + 8);
    char msg[MAX_NAME + 100];
    if (cnt == 1) snprintf(msg, sizeof msg, "Are you sure you want to permanently delete '%s'?", path_base(e[0].orig));
    else snprintf(msg, sizeof msg, "Are you sure you want to delete all of the %d items in the Recycle Bin?", cnt);
    if (cnt > 0 && yes_no(cnt > 1 ? "Confirm Multiple File Delete" : "Confirm File Delete", msg, HICON_WARNING)) {
        fileop_t *op = op_new(FOP_PURGE, cnt);
        for (int i = 0; op && i < cnt; i++) {
            snprintf(op->src[op->n], MAX_PATH, "%c:%s", e[i].drive, path_base(e[i].orig));
            op->ids[op->n++] = e[i].id;
        }
        int r = op ? fileop_run(op) : E_NOMEM;
        op_free(op);
        if (!quit && r < 0 && r != E_CANCELED) report("The Recycle Bin could not be emptied.", r);
    }
    free(e);
    if (!quit) reload_keep();
}

/* ---------------- properties ---------------- */
static void tree_total(const char *p, unsigned long long *bytes, int *files, int *dirs, int depth) {
    hal_dirent_t st;
    if (stat(p, &st) < 0) return;
    if (!st.is_dir) { *bytes += st.size; (*files)++; return; }
    (*dirs)++;
    if (depth > 32) return;
    hal_dirent_t *ents;
    int cnt = list_dir(p, &ents);
    char *child = malloc(MAX_PATH);
    for (int i = 0; i < cnt && child; i++) {
        path_join(p, ents[i].name, child, MAX_PATH);
        tree_total(child, bytes, files, dirs, depth + 1);
    }
    free(child);
    free(ents);
}

static void cmd_props(void) {
    char msg[1100], sz[48], szx[48], tm[48], ct[48];
    int one = sel_single();
    if (binview) {
        if (!nsel) { rb_props(); reload_keep(); return; }
        if (one >= 0) {
            item_t *it = &items[one];
            format_size(it->size, sz, sizeof sz);
            format_size_exact(it->size, szx, sizeof szx);
            format_time(it->year, it->month, it->day, it->hour, it->minute, tm, sizeof tm);
            snprintf(msg, sizeof msg, "%s\n\nType: %s\nOriginal location: %s\nDate deleted: %s\nSize: %s (%s)", it->name,
                     file_type_name(it->name, it->is_dir), it->orig ? it->orig : "", tm, sz, szx);
            info_box("Properties", msg, it->icon);
            return;
        }
        int f, dd;
        unsigned long long b = sel_bytes(&f, &dd);
        format_size(b, sz, sizeof sz);
        format_size_exact(b, szx, sizeof szx);
        snprintf(msg, sizeof msg, "%d items in the Recycle Bin\n\nSize: %s (%s)", nsel, sz, szx);
        info_box("Properties", msg, HICON_RECYCLE_FULL);
        return;
    }
    if (!nsel) {
        if (!path[0]) { shell_app(SHELL_APP_SYSINFO, NULL); return; }
        snprintf(msg, sizeof msg, "Location: %s\nContains: %d items", path, n);
        info_box("Properties", msg, HICON_FOLDER);
        return;
    }
    if (one < 0) {
        if (!path[0]) return;
        /* several items: how many, and how big all of it is */
        unsigned long long bytes = 0;
        int files = 0, dirs = 0;
        set_cursor(CUR_WAIT);
        for (int i = 0; i < n; i++) {
            if (!items[i].selected) continue;
            char p[MAX_PATH];
            item_path(i, p, sizeof p);
            tree_total(p, &bytes, &files, &dirs, 0);
        }
        set_cursor(CUR_ARROW);
        format_size(bytes, sz, sizeof sz);
        format_size_exact(bytes, szx, sizeof szx);
        snprintf(msg, sizeof msg, "%d items selected\n\nLocation: %s\nSize: %s (%s)\nContains: %d file%s, %d folder%s", nsel, path, sz, szx,
                 files, files == 1 ? "" : "s", dirs, dirs == 1 ? "" : "s");
        info_box("Properties", msg, HICON_DOCUMENTS);
        return;
    }
    item_t *it = &items[one];
    char p[MAX_PATH];
    item_path(one, p, sizeof p);
    hal_statx_t x;
    memset(&x, 0, sizeof x);
    int have = stat_ex(p, &x) == 0;
    if (!path[0]) {
        drive_props(it->letter, have ? x.model : "");
        return;
    }
    hal_dirent_t *st = &x.d;
    if (!have && stat(p, st) < 0) memset(st, 0, sizeof *st);
    format_size(st->size, sz, sizeof sz);
    format_size_exact(st->size, szx, sizeof szx);
    format_time(st->year, st->month, st->day, st->hour, st->minute, tm, sizeof tm);
    format_time(x.cyear, x.cmonth, x.cday, x.chour, x.cminute, ct, sizeof ct);
    char size_line[128] = "";
    if (!it->is_dir) snprintf(size_line, sizeof size_line, "Size: %s (%s)", sz, szx);
    snprintf(msg, sizeof msg, "%s\n\nType: %s\nLocation: %s\n%s\n\nCreated: %s\nModified: %s\nShort name: %s\nAttributes:%s%s%s%s",
             it->name, file_type_name(it->name, it->is_dir), path, size_line,
             ct[0] ? ct : "unknown", tm[0] ? tm : "unknown", x.short_name,
             (st->attr & ATTR_READONLY) ? " Read-only" : "", (st->attr & ATTR_HIDDEN) ? " Hidden" : "",
             (st->attr & ATTR_SYSTEM) ? " System" : "", (st->attr & ATTR_ARCHIVE) ? " Archive" : "");
    info_box("Properties", msg, it->icon);
}

static void cmd_format(void) {
    int i = sel_single();
    if (path[0] || binview || i < 0) return;
    int r = shell_format(items[i].letter);
    if (r == E_BUSY) msgbox(win, "Format", "A Format window is already open.", MBX_ICON_WARNING, MBX_OK);
    else if (r < 0) report("Cannot format the disk.", r);
    /* the drive list refreshes when the volume comes back (vfs_generation) */
}

static void cmd_eject(void) {
    int i = sel_single();
    if (path[0] || binview || i < 0) return;
    int r = eject(items[i].letter);
    if (r && r != E_NODEV) msgbox(win, "Eject", errstr(r), MBX_ICON_WARNING, MBX_OK);
    load();
}

static void tool(int app, const char *arg) {
    int r = shell_app(app, arg);
    if (r < 0 && r != E_BUSY) report("Cannot open the program.", r);
}

/* sort again (the selection stays with its items) */
static void resort(void) {
    char fname[MAX_NAME + 1] = "";
    unsigned fid = 0;
    char fdrive = 0;
    if (focus >= 0 && focus < n) { strlcpy(fname, items[focus].name, sizeof fname); fid = items[focus].rbid; fdrive = items[focus].letter; }
    if (!IS_COMPUTER) sort_items(items, n);
    focus = anchor = fname[0] ? find_item(fname, fid, fdrive) : -1;
    save_int("explorer.sort", sort_col);
    save_int("explorer.sortdesc", sort_desc);
    update_menus();
    paint();
}

static void set_view(int v) {
    if (v == view) return;
    view = v;
    scroll = 0;
    ensure_visible(focus);
    save_int("explorer.view", view);
    update_menus();
    paint();
}

static void open_selected(void) {
    if (binview) { cmd_props(); return; }
    int one = sel_single();
    if (one >= 0 || !nsel) { open_item(one >= 0 ? one : focus); return; }
    /* several: every file opens; a folder only if it is the only thing left */
    int dir = -1;
    for (int i = 0; i < n; i++) {
        if (!items[i].selected) continue;
        if (items[i].is_dir) { if (dir < 0) dir = i; continue; }
        open_item(i);
    }
    if (dir >= 0) open_item(dir);
}

static void command(int id) {
    switch (id) {
    case CMD_OPEN: open_selected(); break;
    case CMD_OPEN_NOTEPAD: {
        int i = sel_single();
        if (i >= 0 && path[0] && !items[i].is_dir) { char p[MAX_PATH]; item_path(i, p, sizeof p); tool(SHELL_APP_NOTEPAD, p); }
        break;
    }
    case CMD_NEWFOLDER: cmd_new(1); break;
    case CMD_NEWTEXT: cmd_new(0); break;
    case CMD_RENAME: cmd_rename(); break;
    case CMD_DELETE: cmd_delete(0); break;
    case CMD_DELETE_PERM: cmd_delete(1); break;
    case CMD_COPY: cmd_copy(0); break;
    case CMD_CUT: cmd_copy(1); break;
    case CMD_PASTE: cmd_paste(); break;
    case CMD_PROPS: cmd_props(); break;
    case CMD_FORMAT: cmd_format(); break;
    case CMD_EJECT: cmd_eject(); break;
    case CMD_CLOSE: quit = 1; break;
    case CMD_VIEW_ICONS: set_view(0); break;
    case CMD_VIEW_DETAILS: set_view(1); break;
    case CMD_PANE:
        pane_on = !pane_on;
        save_int("explorer.pane", pane_on);
        layout();
        update_menus();
        paint();
        break;
    case CMD_SORT0: case CMD_SORT1: case CMD_SORT2: case CMD_SORT3:
        if (IS_COMPUTER) break;
        sort_col = id - CMD_SORT0;
        resort();
        break;
    case CMD_SORT_DESC: if (!IS_COMPUTER) { sort_desc = !sort_desc; resort(); } break;
    case CMD_SELECT_ALL: cmd_select_all(0); break;
    case CMD_INVERT: cmd_select_all(1); break;
    case CMD_REFRESH: reload_keep(); break;
    case CMD_BACK: if (nback) { nback--; char p[MAX_PATH]; strlcpy(p, back[nback], sizeof p); navigate(p, 0); } break;
    case CMD_UP: go_up(); break;
    case CMD_MYCOMP: navigate("", 1); break;
    case CMD_MYDOCS: navigate("C:/Documents", 1); break;
    case CMD_RECYCLEBIN: navigate("Recycle Bin", 1); break;
    case CMD_RESTORE: cmd_restore(0); break;
    case CMD_RESTORE_ALL: cmd_restore(1); break;
    case CMD_EMPTY: cmd_empty(); break;
    case CMD_RBPROPS: rb_props(); if (binview) reload_keep(); break;
    case CMD_TERMINAL: tool(SHELL_APP_TERMINAL, path[0] ? path : NULL); break;
    case CMD_SYSINFO: tool(SHELL_APP_SYSINFO, NULL); break;
    case CMD_CONTROL: tool(SHELL_APP_CONTROL, NULL); break;
    case CMD_ABOUT: tool(SHELL_APP_ABOUT, NULL); break;
    }
}

/* ---------------- events ---------------- */
static int item_at(int px, int py) {
    if (!in_rect(view_x, view_y, view_w - SB_W, view_h, px, py)) return -1;
    if (view == 1 && py < view_y + 22) return -1;
    for (int i = 0; i < n; i++) {
        hal_rect_t r = item_rect(i);
        if (in_rect(r.x, r.y, r.w, r.h, px, py)) return i;
    }
    return -1;
}

static void set_cursor(int c) {
    if (c != cursor) { cursor = c; win_set_cursor(win, c); }
}

static void mouse_move(int x, int y) {
    int h = item_at(x, y), lh = -1, tbh = -1;
    for (int i = 0; i < nlinks; i++) if (in_rect(links[i].r.x, links[i].r.y, links[i].r.w, links[i].r.h, x, y)) lh = links[i].id;
    for (int i = 0; i < 4; i++) { hal_rect_t r = tb_rect(i); if (in_rect(r.x, r.y, r.w, r.h, x, y)) tbh = i; }
    set_cursor(lh >= 0 ? CUR_HAND : CUR_ARROW);
    if (h != hover || lh != link_hover || tbh != tb_hover) {
        hover = h; link_hover = lh; tb_hover = tbh;
        paint();
    }
}

/* a press on an item: what happens at the release if there was no drag */
enum { PEND_NONE, PEND_ONLY, PEND_TOGGLE };
static int press_item = -1, press_btn, press_x, press_y, press_pend;
#define PRESS_BG (-2)                       /* a right press on the background */
static unsigned char *band_base;            /* the selection when the band started (Ctrl / Shift) */
static int band_ctrl;

/* the items under the band are selected (Ctrl: toggled against the selection before it) */
static void band_update(void) {
    int bx, by, bw, bh;
    band_rect(&bx, &by, &bw, &bh);
    for (int i = 0; i < n; i++) {
        hal_rect_t r = item_rect(i);
        int hit = bw > 0 && bh > 0 && r.x < bx + bw && bx < r.x + r.w && r.y < by + bh && by < r.y + r.h;
        int base = band_base ? band_base[i] : 0;
        sel_set(i, band_ctrl ? base ^ hit : base | hit);
        if (hit && focus < 0) focus = anchor = i;
    }
}

static void band_start(int x, int y, int ctrl, int shift) {
    band_on = 1;
    band_x0 = band_x1 = x;
    band_y0 = band_y1 = y + scroll;
    band_ctrl = ctrl;
    free(band_base);
    band_base = NULL;
    if ((ctrl || shift) && n) {
        band_base = malloc((size_t)n);
        for (int i = 0; band_base && i < n; i++) band_base[i] = items[i].selected;
    }
}

static void start_drag(void) {
    press_item = -1;
    press_pend = PEND_NONE;
    if (!path[0] || binview || !nsel) return;            /* drives and the bin's entries stay put */
    int len, count;
    char *list = selected_paths(&len, &count);
    if (!list || !count) { free(list); return; }
    hal_volinfo_t vi;
    int allowed = DND_COPY | DND_MOVE;
    if (volume_info(path[0], &vi) == 0 && vi.readonly) allowed = DND_COPY;
    int icon = focus >= 0 && focus < n && items[focus].selected ? items[focus].icon : items[sel_first()].icon;
    dnd_begin(win, list, len + 1, count, allowed, icon);
    free(list);
}

static void header_click(int x) {
    if (IS_COMPUTER) return;
    int xs[NCOLS + 1];
    columns(xs, NULL, NULL);
    for (int c = 0; c < NCOLS; c++)
        if (x >= view_x + xs[c] && x < view_x + xs[c + 1]) {
            if (c == sort_col) sort_desc = !sort_desc;
            else { sort_col = c; sort_desc = 0; }
            resort();
            return;
        }
}

static void mouse_down(const hal_event_t *ev) {
    int x = ev->x, y = ev->y;
    if (y >= TOOLBAR_H && y < TOP_Y) return;    /* the address bar and Go button handle their clicks */
    ctl_focus(win, 0);                      /* keys to the view, not the address bar */
    for (int i = 0; i < 4; i++) {
        hal_rect_t r = tb_rect(i);
        if (in_rect(r.x, r.y, r.w, r.h, x, y) && ev->button == MOUSE_LEFT) {
            command(i == 0 ? CMD_BACK : i == 1 ? CMD_UP : i == 2 ? CMD_MYCOMP : view ? CMD_VIEW_ICONS : CMD_VIEW_DETAILS);
            return;
        }
    }
    for (int i = 0; i < nlinks; i++)
        if (in_rect(links[i].r.x, links[i].r.y, links[i].r.w, links[i].r.h, x, y) && ev->button == MOUSE_LEFT) { command(links[i].id); return; }
    if (!in_rect(view_x, view_y, view_w - SB_W, view_h, x, y)) return;
    if (view == 1 && y < view_y + 22) { if (ev->button == MOUSE_LEFT) header_click(x); return; }
    int i = item_at(x, y);
    int ctrl = (ev->mods & MOD_CTRL) != 0, shift = (ev->mods & MOD_SHIFT) != 0;
    if (ev->type == EVT_DBLCLICK && ev->button == MOUSE_LEFT && i >= 0 && !ctrl && !shift) {
        sel_only(i);
        if (binview) cmd_props(); else open_item(i);
        return;
    }
    press_item = -1;
    press_pend = PEND_NONE;
    if (i < 0) {
        /* the background: a rubber band (left), the folder's menu (right, at the release) */
        if (!ctrl && !shift) sel_clear();
        if (ev->button == MOUSE_LEFT) band_start(x, y, ctrl, shift);
        else if (ev->button == MOUSE_RIGHT) press_item = PRESS_BG;
        update_menus();
        paint();
        return;
    }
    press_item = i;
    press_btn = ev->button;
    press_x = x;
    press_y = y;
    if (ev->button == MOUSE_RIGHT) {
        if (!items[i].selected) sel_only(i); else focus = i;
    } else if (shift) {
        sel_range(anchor < 0 ? i : anchor, i, ctrl);
        focus = i;
        if (anchor < 0) anchor = i;
    } else if (ctrl) {
        if (items[i].selected) press_pend = PEND_TOGGLE;      /* off at the release, unless it is dragged */
        else sel_set(i, 1);
        focus = anchor = i;
    } else {
        if (items[i].selected && nsel > 1) press_pend = PEND_ONLY;   /* keep the selection for a drag */
        else sel_only(i);
        focus = anchor = i;
    }
    update_menus();
    paint();
}

static void mouse_held(const hal_event_t *ev) {
    int x = ev->x, y = ev->y;
    if (band_on) {
        /* the view scrolls when the band reaches its edge */
        int top = view_y + (view == 1 ? 22 : 0), old = scroll;
        if (y < top) scroll = MAX(0, scroll - 16);
        else if (y > view_y + view_h) scroll = MIN(max_scroll(), scroll + 16);
        band_x1 = CLAMP(x, view_x, view_x + view_w - SB_W - 1);
        band_y1 = CLAMP(y, top, view_y + view_h - 1) + scroll;
        (void)old;
        band_update();
        paint_status();
        paint_pane();
        paint_view();
        win_invalidate(win);
        return;
    }
    if (press_item >= 0 && (ev->buttons & press_btn) && abs(x - press_x) + abs(y - press_y) > 4) start_drag();
}

static void mouse_up(const hal_event_t *ev) {
    if (band_on && ev->button == MOUSE_LEFT) {
        band_on = 0;
        free(band_base);
        band_base = NULL;
        update_menus();
        paint();
        return;
    }
    if (press_item == PRESS_BG && ev->button == MOUSE_RIGHT) {
        press_item = -1;
        context_menu(-1, ev->x, ev->y);
        return;
    }
    if (press_item >= 0 && ev->button == press_btn) {
        int i = press_item;
        press_item = -1;
        if (press_pend == PEND_ONLY) sel_only(i);
        else if (press_pend == PEND_TOGGLE) sel_set(i, 0);
        press_pend = PEND_NONE;
        update_menus();
        paint();
        if (ev->button == MOUSE_RIGHT) context_menu(i, ev->x, ev->y);
    }
}

/* ---------------- this window as a drop target ---------------- */
static char *drag_buf;                      /* the paths of the drag over us */
static char *watch;                         /* items we let others move: reload when they go */
static uint64_t watch_until;
static int drag_serial, drag_count;

static int drag_fetch(int serial) {
    if (serial == drag_serial && drag_buf) return 0;
    free(drag_buf);
    drag_buf = NULL;
    drag_serial = 0;
    int len = dnd_data(serial, NULL, 0);
    if (len <= 0) return -1;
    drag_buf = malloc((size_t)len + 2);
    if (!drag_buf) return -1;
    len = dnd_data(serial, drag_buf, len);
    if (len <= 0) { free(drag_buf); drag_buf = NULL; return -1; }
    drag_buf[len] = drag_buf[len + 1] = 0;
    drag_serial = serial;
    drag_count = 0;
    for (const char *p = drag_buf; *p; p += strlen(p) + 1) drag_count++;
    return 0;
}

/* where a drop at x, y goes: 0 nowhere, 1 into folder dir (*hl: the item
 * that is that folder, or -1 for this folder), 2 the Recycle Bin */
static int drop_target(int x, int y, char *dir, int *hl) {
    *hl = -1;
    if (!in_rect(view_x, view_y, view_w - SB_W, view_h, x, y) || (view == 1 && y < view_y + 22)) return 0;
    if (binview) return 2;
    int i = item_at(x, y);
    if (i >= 0 && items[i].is_dir) {
        item_path(i, dir, MAX_PATH);
        *hl = i;
        return 1;
    }
    if (!path[0]) return 0;
    strlcpy(dir, path, MAX_PATH);
    return 1;
}

static int is_bin_path(const char *p) {
    return strlen(p) >= 11 && prefix_nocase(p + 3, "RECYCLED", 8) && (p[11] == '/' || !p[11]);
}

/* the effect of dropping the drag's paths there: the drive decides (moves
 * on the same drive, copies to another), Ctrl copies, Shift moves */
static int drop_effect(int kind, const char *dir, int mods, int right, int *possible) {
    *possible = 0;
    if (!drag_buf || !drag_count) return DND_NONE;
    if (kind == 2) {
        int any = 0;
        for (const char *p = drag_buf; *p; p += strlen(p) + 1) {
            hal_rbinfo_t ri;
            if (is_bin_path(p)) return DND_NONE;
            if (recycle_info(p[0], &ri) == 0 && ri.supported != E_ROFS && ri.supported != E_NODEV) any = 1;
        }
        if (any) *possible = DND_MOVE;
        return *possible;
    }
    hal_volinfo_t vi;
    if (volume_info(dir[0], &vi) < 0 || vi.readonly) return DND_NONE;
    int all_here = 1;
    for (const char *p = drag_buf; *p; p += strlen(p) + 1) {
        size_t l = strlen(p);
        if (!strcasecmp(p, dir) || (prefix_nocase(p, dir, l) && dir[l] == '/')) return DND_NONE;    /* into itself */
        char d[MAX_PATH];
        path_dir(p, d, sizeof d);
        if (strcasecmp(d, dir)) all_here = 0;
    }
    int eff = same_drive(drag_buf, dir) ? DND_MOVE : DND_COPY;
    if (mods & MOD_CTRL) eff = DND_COPY;
    else if (mods & MOD_SHIFT) eff = DND_MOVE;
    *possible = DND_COPY | DND_MOVE;
    if (all_here) {
        /* back into its own folder: nothing, unless a copy is asked for */
        *possible = DND_COPY;
        if (eff == DND_MOVE && !right) return DND_NONE;
        eff = DND_COPY;
    }
    return eff;
}

static void on_drag(const hal_event_t *ev) {
    char dir[MAX_PATH];
    int hl, possible;
    switch (ev->button) {
    case DND_ENTER:
    case DND_OVER: {
        drag_fetch(ev->id);
        int kind = drop_target(ev->x, ev->y, dir, &hl);
        int eff = kind ? drop_effect(kind, dir, ev->mods, ev->buttons == MOUSE_RIGHT, &possible) : DND_NONE;
        if (!eff) { hl = -1; possible = 0; }
        dnd_accept(win, ev->id, eff, possible);
        /* scroll when the pointer rests near the top or bottom */
        int moved = 0;
        if (in_rect(view_x, view_y, view_w, view_h, ev->x, ev->y)) {
            int top = view_y + (view == 1 ? 22 : 0);
            if (ev->y < top + 12 && scroll > 0) { scroll = MAX(0, scroll - 12); moved = 1; }
            else if (ev->y > view_y + view_h - 12 && scroll < max_scroll()) { scroll = MIN(max_scroll(), scroll + 12); moved = 1; }
        }
        if (hl != drop_hl || moved) { drop_hl = hl; paint(); }
        break;
    }
    case DND_LEAVE:
        if (drop_hl >= 0) { drop_hl = -1; paint(); }
        break;
    case DND_DROP: {
        drag_fetch(ev->id);
        int kind = drop_target(ev->x, ev->y, dir, &hl);
        if (drop_hl >= 0) { drop_hl = -1; paint(); }
        if (!kind || !ev->pressed || !drag_buf) break;
        /* (questions and progress windows below take other drags meanwhile) */
        int len = 0;
        for (const char *p = drag_buf; *p; p += strlen(p) + 1) len += (int)strlen(p) + 1;
        char *list = malloc((size_t)len + 1);
        if (!list) break;
        memcpy(list, drag_buf, (size_t)len);
        list[len] = 0;
        int count = drag_count;
        if (kind == 2) delete_paths(list, count, 0);
        else transfer(list, count, dir, ev->pressed == DND_COPY ? FOP_COPY : FOP_MOVE);
        free(list);
        break;
    }
    case DND_END:
        /* we were the source: the target works on its own time (questions,
         * progress), so watch the items for a while and show it when they go */
        if (ev->pressed == DND_MOVE && drag_fetch(ev->id) == 0) {
            free(watch);
            int len = 0;
            for (const char *p = drag_buf; *p; p += strlen(p) + 1) len += (int)strlen(p) + 1;
            watch = malloc((size_t)len + 1);
            if (watch) { memcpy(watch, drag_buf, (size_t)len); watch[len] = 0; watch_until = time_ms() + 120000; }
        }
        break;
    }
}

/* ---------------- keys ---------------- */
static unsigned first_char(const char *s) {
    int pos = 0;
    return utf8_get(s, (int)strlen(s), &pos);
}

static void key(const hal_event_t *ev) {
    if (ctl_get_focus(win) == ID_ADDR && ev->id != KEY_F5) return;   /* typing in the address bar */
    int c = view == 1 ? 1 : cols(), nf = focus;
    int ctrl = (ev->mods & MOD_CTRL) != 0, shift = (ev->mods & MOD_SHIFT) != 0;
    int page = view == 1 ? MAX(1, (view_h - 22) / ROW_H) : c * MAX(1, view_h / (IS_COMPUTER ? TILE_H + 8 : CELL_H));
    switch (ev->id) {
    case KEY_ENTER: case KEY_KP_ENTER: open_selected(); return;
    case KEY_BACKSPACE: go_up(); return;
    case KEY_DELETE: cmd_delete(shift); return;
    case KEY_F2: cmd_rename(); return;
    case KEY_F5: command(CMD_REFRESH); return;
    case KEY_SPACE:
        if (focus < 0) return;
        if (ctrl) sel_set(focus, !items[focus].selected); else sel_set(focus, 1);
        anchor = focus;
        update_menus();
        paint();
        return;
    case KEY_MENU: menu_key: {
        int i = focus >= 0 && items[focus].selected ? focus : sel_first();
        hal_rect_t r = { view_x + 20, view_y + 20, 0, 0 };
        if (i >= 0) { ensure_visible(i); paint(); r = item_rect(i); }
        context_menu(i, r.x + MIN(r.w, 40), r.y + MIN(r.h, 12));
        return;
    }
    case KEY_LEFT: if (ev->mods & MOD_ALT) { command(CMD_BACK); return; } nf = view == 1 ? nf : nf - 1; break;
    case KEY_RIGHT: nf = view == 1 ? nf : nf + 1; break;
    case KEY_UP: nf -= c; break;
    case KEY_DOWN: nf = nf < 0 ? 0 : nf + c; break;
    case KEY_PAGEUP: nf = nf < 0 ? 0 : nf - page; break;
    case KEY_PAGEDOWN: nf = nf < 0 ? 0 : nf + page; break;
    case KEY_HOME: nf = 0; break;
    case KEY_END: nf = n - 1; break;
    default:
        if (ev->id == KEY_F10 && shift) goto menu_key;
        if (ev->mods & (MOD_CTRL | MOD_ALT)) {
            if (key_shortcut(ev, 'C')) cmd_copy(0);
            else if (key_shortcut(ev, 'X')) cmd_copy(1);
            else if (key_shortcut(ev, 'V')) cmd_paste();
            else if (key_shortcut(ev, 'A')) cmd_select_all(0);
            return;
        }
        if (ev->ch > 32 && ev->ch != 127 && n) {
            /* jump to the next item starting with the typed character */
            for (int j = 1; j <= n; j++) {
                int idx = (focus + j + n) % n;
                if (fold(first_char(items[idx].name)) == fold(ev->ch)) { nf = idx; break; }
            }
            shift = ctrl = 0;
        }
        break;
    }
    if (!n) return;
    nf = CLAMP(nf, 0, n - 1);
    if (nf == focus && nsel) return;
    if (shift) {
        /* Shift+arrows: from the anchor to here (Ctrl+Shift: added to the selection) */
        if (anchor < 0) anchor = focus >= 0 ? focus : nf;
        sel_range(anchor, nf, ctrl);
        focus = nf;
    } else if (ctrl) {
        focus = nf;                         /* Ctrl+arrows: the focus moves alone (Ctrl+Space selects) */
    } else {
        sel_only(nf);
    }
    ensure_visible(nf);
    update_menus();
    paint();
}

static void drives_changed(void) {
    /* drives came or went: if ours vanished go to My Computer */
    hal_volinfo_t vi;
    if (path[0] && volume_info(path[0], &vi) < 0) path[0] = 0;
    reload_keep();
}

static void timer(void) {
    if (vfs_generation() != vgen) { drives_changed(); return; }
    if (watch) {
        int gone = 0;
        for (const char *p = watch; *p && !gone; p += strlen(p) + 1) { hal_dirent_t st; gone = stat(p, &st) < 0; }
        if (gone || time_ms() > watch_until) { free(watch); watch = NULL; }
        if (gone) reload_keep();
    }
    hal_rbinfo_t ri;
    if (recycle_info(0, &ri) == 0 && ri.generation != rbgen) {
        /* a bin changed (here or in another window) */
        rbgen = ri.generation;
        int ic = rb_full() ? HICON_RECYCLE_FULL : HICON_RECYCLE;
        if (binview) { reload_keep(); win_set_icon(win, ic); }
        else if (ic != rb_icon) { rb_icon = ic; paint(); }
        rb_icon = ic;
        return;
    }
    if ((clip->count > 0) != clip_shown) { update_menus(); paint(); }   /* another Explorer cut or pasted */
}

int main(int argc, char **argv) {
    int h = shm_open("halcyon.explorer.clip2", sizeof(clip_t), SHM_CREATE);
    if (h >= 0) {
        size_t sz = 0;
        clip_t *c = shm_map(h, &sz);
        if (c && sz >= sizeof(clip_t)) clip = c;
    }
    gfx_theme(&th);
    if (argc > 1 && !strcmp(argv[1], "-rbprops")) {    /* the desktop's Recycle Bin > Properties */
        win = -1;
        rb_props();
        return 0;
    }
    load_settings();
    win = win_create("My Computer", cw, ch, WIN_RESIZABLE);
    if (win < 0) { printf("explorer: no window (%d)\n", win); return 1; }
    win_set_icon(win, HICON_COMPUTER);
    win_set_min_size(win, 352, 200);
    ctl_create(win, CTL_TEXTBOX, 62, TOOLBAR_H + 3, cw - 62 - 50, 22, "", ID_ADDR, CTL_NOTIFY);
    ctl_create(win, CTL_BUTTON, cw - 46, TOOLBAR_H + 3, 40, 22, "Go", ID_GO, 0);
    ctl_create(win, CTL_VSCROLL, 0, 0, SB_W, 40, 0, ID_VSB, 0);
    ctl_focus(win, 0);
    win_size(win, &cw, &ch);
    layout();
    rb_icon = rb_full() ? HICON_RECYCLE_FULL : HICON_RECYCLE;
    const char *start = argc > 1 ? argv[1] : "";
    if (start[0]) navigate(start, 0);
    if (!items) load();                     /* My Computer (or the start path was a file) */
    win_set_timer(win, 500);

    while (!quit) {
        hal_event_t ev;
        if (!win_get_event(&ev, -1) || ev.win != win) continue;
        switch (ev.type) {
        case EVT_PAINT: paint(); break;
        case EVT_CLOSE: quit = 1; break;
        case EVT_RESIZE: cw = ev.x; ch = ev.y; layout(); break;
        case EVT_MOUSE_MOVE:
            if (ev.buttons && (band_on || press_item >= 0)) mouse_held(&ev);
            else mouse_move(ev.x, ev.y);
            break;
        case EVT_MOUSE_LEAVE: mouse_move(-1, -1); break;
        case EVT_WHEEL: scroll = CLAMP(scroll - ev.dz * 57, 0, max_scroll()); paint(); break;
        case EVT_MOUSE_DOWN: case EVT_DBLCLICK: mouse_down(&ev); break;
        case EVT_MOUSE_UP: mouse_up(&ev); break;
        case EVT_KEY: if (ev.pressed) key(&ev); break;
        case EVT_MENU: command(ev.id); break;
        case EVT_DRAG: on_drag(&ev); break;
        case EVT_SCROLL: if (ev.id == ID_VSB) { scroll = CLAMP(ev.x, 0, max_scroll()); paint(); } break;
        case EVT_COMMAND:
            if ((ev.id == ID_ADDR && ev.button == CMD_ACTIVATE) || ev.id == ID_GO) {
                char a[MAX_PATH];
                ctl_get_text(win, ID_ADDR, a, sizeof a);
                navigate(a, 1);
            }
            break;
        case EVT_TIMER: timer(); break;
        }
    }
    win_destroy(win);
    return 0;
}
