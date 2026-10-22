#pragma once
#include "kernel.h"

/* ramfs.c: volatile in-memory filesystem (persistence arrives with the storage driver) */
int  fs_write(const char *path, const void *data, size_t n);
int  fs_read(const char *path, const u8 **data, size_t *n);
int  fs_remove(const char *path);
int  fs_remove_tree(const char *prefix);
int  fs_count(void);
int  fs_info(int i, char *path, size_t pn, size_t *size);
void fs_norm(const char *in, char *out, size_t n);       /* Windows or Linux syntax, relative to $PWD */
void fs_init(void);
size_t fs_bytes(void);

void sha256_hex(const void *data, size_t n, char out[65]);

/* pkg.c */
struct table;
void pkg_init(void);
int  pkg_update(const char **err);
int  pkg_install(const char *name, const char **err);
int  pkg_remove(const char *name, const char **err);
int  pkg_table(struct table *t);
int  pkg_info(const char *name);
int  pkg_record(const char *name, const char *ver, const char *origin);
int  pkg_stage_cache(const u8 *d, size_t n, const char **err);
int  pkg_installed_table(struct table *t);
int  pkg_install_blob(const char *name, const char *ver, const u8 *d, size_t n, const char **err);
int  pkg_extract_root(const u8 *d, size_t n, const char **err);
int  pkg_build(const char *name, const char *ver, const char *dir, char *outpath, size_t on, char sha[65], size_t *size, const char **err);
