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
