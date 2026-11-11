/* TEMPORARY VFS stubs until the storage stack is merged */
#include "fs/vfs.h"
#include "core/lib.h"
#define W __attribute__((weak))
W int vfs_volume_count(void) { return 0; }
W volume_t *vfs_volume(int i) { return NULL; }
W volume_t *vfs_volume_by_letter(char l) { return NULL; }
W u32 vfs_generation(void) { return 0; }
W int vfs_list(const char *p, vfs_dirent_t **o) { *o = NULL; return E_NODEV; }
W int vfs_stat(const char *p, vfs_dirent_t *o) { return E_NODEV; }
W int vfs_sync_all(void) { return 0; }
W void vfs_join(const char *d, const char *n, char *o, size_t s) { snprintf(o, s, "%s/%s", d, n); }
W const char *vfs_basename(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
W void vfs_dirname(const char *p, char *o, size_t s) { strlcpy(o, p, s); }
W int vfs_statfs(char l, u64 *t, u64 *f) { *t = *f = 0; return E_NODEV; }
W int vfs_normalize(const char *in, char *out, size_t n) { strlcpy(out, in, n); return 0; }
W int vfs_mkdir(const char *p) { return E_NODEV; }
W int vfs_unlink(const char *p) { return E_NODEV; }
W int vfs_rename(const char *a, const char *b) { return E_NODEV; }
W int vfs_copy(const char *a, const char *b) { return E_NODEV; }
W int vfs_read_file(const char *p, char **d, u32 *s) { return E_NODEV; }
W int vfs_write_file(const char *p, const void *d, u32 s) { return E_NODEV; }
W int vfs_open(const char *p, int f, vfs_file_t **o) { return E_NODEV; }
W int vfs_write(vfs_file_t *f, const void *b, u32 l) { return E_NODEV; }
W int vfs_close(vfs_file_t *f) { return 0; }
W blockdev_t *blockdev_first(void) { return NULL; }
