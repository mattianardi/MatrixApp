#pragma once
#include "kernel.h"
#include "neos_api.h"

/*
 * Variable bus: ONE canonical store, many views. A variable is written from the
 * Linux side ($VAR, "/mnt/c/x", "a:b") or the Windows side (%VAR%, "C:\x", "a;b")
 * and read back in either convention; paths, path lists and well-known names
 * (HOME <-> USERPROFILE ...) are translated by the interpreter thread.
 */
enum { VT_STR, VT_INT, VT_BOOL, VT_PATH, VT_PLIST };

struct bus_var_info { char name[40]; int type; int view; unsigned ver; };
typedef void (*bus_cb)(const char *name, const char *canonical);
typedef void (*bus_provider_fn)(char *buf, size_t n);

void  bus_init(void);
void  bus_start(void);                                   /* spawn the interpreter thread */
int   bus_set(const char *name, const char *text, int view, int sync, const char **err);
int   bus_unset(const char *name, int view);
long  bus_get(const char *name, int view, char *buf, unsigned long len);
long  bus_post_set(const char *name, const char *value, int view);
void  bus_declare(const char *name, int type);
int   bus_count(void);
int   bus_info(int i, struct bus_var_info *o);
void  bus_expand(const char *in, char *out, size_t n);   /* $VAR ${VAR} ${VAR@win} %VAR% %VAR@lin% */
int   bus_env_block(int view, char *buf, size_t n);      /* NAME=VALUE\n ... for a process of that world */
void  bus_subscribe(const char *prefix, bus_cb cb);
void  bus_provider(const char *name, bus_provider_fn fn);
int   bus_event(int i, char *name, size_t n, unsigned *ver, int *view, u64 *tick);   /* newest first */
const char *bus_type_name(int t);
int   path_win_to_posix(const char *w, char *out, size_t n);
int   path_posix_to_win(const char *p, char *out, size_t n);
