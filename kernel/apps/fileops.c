/* Halcyon OS - shared file helpers */
#include "fileops.h"
#include "core/lib.h"
#include "core/mm.h"
#include "icons.h"

int fileops_delete_tree(const char *path) {
    vfs_dirent_t st;
    int r = vfs_stat(path, &st);
    if (r < 0) return r;
    if (st.is_dir) {
        vfs_dirent_t *ents;
        int n = vfs_list(path, &ents);
        if (n < 0) return n;
        char *child = kmalloc(VFS_MAX_PATH);
        for (int i = 0; i < n; i++) {
            vfs_join(path, ents[i].name, child, VFS_MAX_PATH);
            r = fileops_delete_tree(child);
            if (r < 0) { kfree(child); kfree(ents); return r; }
        }
        kfree(child);
        kfree(ents);
    }
    return vfs_unlink(path);
}

int fileops_copy_tree(const char *src, const char *dst) {
    vfs_dirent_t st;
    int r = vfs_stat(src, &st);
    if (r < 0) return r;
    if (!st.is_dir) return vfs_copy(src, dst);
    /* refuse to copy a directory into itself */
    size_t sl = strlen(src);
    if (!strncasecmp(src, dst, sl) && (dst[sl] == '/' || dst[sl] == 0)) return E_INVAL;
    r = vfs_mkdir(dst);
    if (r < 0 && r != E_EXIST) return r;
    vfs_dirent_t *ents;
    int n = vfs_list(src, &ents);
    if (n < 0) return n;
    char *a = kmalloc(VFS_MAX_PATH), *b = kmalloc(VFS_MAX_PATH);
    for (int i = 0; i < n && r >= 0; i++) {
        vfs_join(src, ents[i].name, a, VFS_MAX_PATH);
        vfs_join(dst, ents[i].name, b, VFS_MAX_PATH);
        r = fileops_copy_tree(a, b);
    }
    kfree(a); kfree(b); kfree(ents);
    return r < 0 ? r : 0;
}

int fileops_unique_name(const char *dir, const char *base, char *out, int outsz) {
    vfs_dirent_t st;
    vfs_join(dir, base, out, outsz);
    if (vfs_stat(out, &st) < 0) return 0;
    char name[VFS_MAX_NAME + 1], ext[VFS_MAX_NAME + 1] = "";
    strlcpy(name, base, sizeof name);
    char *dot = strrchr(name, '.');
    if (dot && dot != name) { strlcpy(ext, dot, sizeof ext); *dot = 0; }
    for (int i = 2; i < 1000; i++) {
        char cand[VFS_MAX_NAME + 1];
        snprintf(cand, sizeof cand, "%s (%d)%s", name, i, ext);
        vfs_join(dir, cand, out, outsz);
        if (vfs_stat(out, &st) < 0) return 0;
    }
    return E_EXIST;
}

static const char *ext_of(const char *name) {
    const char *d = strrchr(name, '.');
    return d ? d + 1 : "";
}

int is_text_file(const char *name) {
    static const char *t[] = { "txt", "log", "ini", "cfg", "c", "h", "md", "bat", "asm", "s", "py", "csv", "json",
                               "xml", "htm", "html", "inf", "conf", "sh", "nfo", "me" };
    const char *e = ext_of(name);
    for (size_t i = 0; i < ARRAY_SIZE(t); i++) if (!strcasecmp(e, t[i])) return 1;
    return 0;
}

const char *file_type_name(const char *name, int is_dir) {
    if (is_dir) return "File Folder";
    const char *e = ext_of(name);
    if (is_text_file(name)) return !strcasecmp(e, "ini") || !strcasecmp(e, "cfg") ? "Configuration Settings" : "Text Document";
    if (!strcasecmp(e, "bmp")) return "Bitmap Image";
    if (!strcasecmp(e, "efi")) return "EFI Application";
    if (!strcasecmp(e, "bin") || !strcasecmp(e, "sys")) return "System file";
    if (!strcasecmp(e, "exe") || !strcasecmp(e, "com")) return "Application";
    if (!strcasecmp(e, "img") || !strcasecmp(e, "iso")) return "Disk Image";
    if (!strcasecmp(e, "zip")) return "Compressed Folder";
    static char buf[24];
    if (*e && strlen(e) <= 8) {
        int n = 0;
        for (const char *p = e; *p; p++) buf[n++] = (char)toupper(*p);
        strlcpy(buf + n, " File", sizeof buf - n);
        return buf;
    }
    return "File";
}

int file_icon(const char *name, int is_dir) {
    if (is_dir) return ICON_FOLDER;
    const char *e = ext_of(name);
    if (is_text_file(name)) return ICON_FILE_TEXT;
    if (!strcasecmp(e, "bmp")) return ICON_FILE_IMAGE;
    if (!strcasecmp(e, "efi") || !strcasecmp(e, "exe") || !strcasecmp(e, "com")) return ICON_FILE_APP;
    if (!strcasecmp(e, "bin") || !strcasecmp(e, "sys")) return ICON_FILE_SYS;
    return ICON_FILE;
}

void format_size(u64 b, char *out, int n) {
    if (b < 1024) snprintf(out, n, "%lu bytes", b);
    else if (b < 1024 * 1024) snprintf(out, n, "%lu KB", (b + 1023) / 1024);
    else if (b < 1024ull * 1024 * 1024) snprintf(out, n, "%lu.%lu MB", b >> 20, ((b & 0xFFFFF) * 10) >> 20);
    else snprintf(out, n, "%lu.%lu GB", b >> 30, ((b & 0x3FFFFFFF) * 10) >> 30);
}

void format_size_exact(u64 b, char *out, int n) {
    char digits[32];
    snprintf(digits, sizeof digits, "%lu", b);
    int len = (int)strlen(digits), o = 0;
    for (int i = 0; i < len && o < n - 8; i++) {
        out[o++] = digits[i];
        if ((len - i - 1) % 3 == 0 && i != len - 1) out[o++] = ',';
    }
    out[o] = 0;
    strlcat(out, " bytes", n);
}

void format_time(const vfs_time_t *t, char *out, int n) {
    if (!t->year) { strlcpy(out, "", n); return; }
    int h = t->hour % 12 == 0 ? 12 : t->hour % 12;
    snprintf(out, n, "%d/%d/%d %d:%02d %s", t->month, t->day, t->year, h, t->minute, t->hour < 12 ? "AM" : "PM");
}

int drive_icon(volume_t *v) {
    if (!v->bd) return ICON_HDD;
    blockdev_t *d = v->bd->parent ? v->bd->parent : v->bd;
    if (d->type == BDEV_DISK_USB) return ICON_USBDRIVE;
    if (!strcmp(v->fs_name, "FAT12") && d->sector_count <= 5760) return ICON_FLOPPY;
    return ICON_HDD;
}

const char *drive_kind(volume_t *v) {
    int ic = drive_icon(v);
    if (ic == ICON_USBDRIVE) return "Removable Disk";
    if (ic == ICON_FLOPPY) return "Floppy Disk";
    return "Local Disk";
}
