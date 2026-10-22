#include "kernel.h"

#define MAX_MODULES 48
static struct { const char *name, *ver, *desc; } mods[MAX_MODULES];
static int nmods;

void module_register(const char *name, const char *version, const char *desc)
{
    if (nmods < MAX_MODULES) mods[nmods++] = (typeof(mods[0])){ name, version, desc };
}
int module_count(void) { return nmods; }
void module_get(int i, const char **name, const char **version, const char **desc)
{
    *name = mods[i].name; *version = mods[i].ver; *desc = mods[i].desc;
}
