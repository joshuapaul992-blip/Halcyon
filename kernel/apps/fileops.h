/* Halcyon OS - shared file helpers for the shell applications */
#ifndef FILEOPS_H
#define FILEOPS_H
#include "core/types.h"
#include "fs/vfs.h"

int  fileops_delete_tree(const char *path);                 /* file or directory (recursive) */
int  fileops_copy_tree(const char *src, const char *dst);   /* file or directory (recursive) */
int  fileops_unique_name(const char *dir, const char *base, char *out, int outsz);  /* "Copy of x", "x (2)" */
const char *file_type_name(const char *name, int is_dir);
int  file_icon(const char *name, int is_dir);
void format_size(u64 bytes, char *out, int outsz);           /* "1.5 MB" */
void format_size_exact(u64 bytes, char *out, int outsz);     /* "1,572,864 bytes" */
void format_time(const vfs_time_t *t, char *out, int outsz); /* "11/11/2026 7:23 AM" */
int  drive_icon(volume_t *v);
const char *drive_kind(volume_t *v);                         /* "Local Disk", "Removable Disk" */
int  is_text_file(const char *name);

#endif
