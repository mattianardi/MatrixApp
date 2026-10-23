#include "chr.h"
#include "vfs.h"

static struct { const struct chrops *ops; int minor; } chrs[CHR_MAX];
int chr_register(const char *path, const struct chrops *ops, int minor)
{
    for (int i = 0; i < CHR_MAX; i++) if (!chrs[i].ops) {
        struct inode *ino;
        if (vfs_create(path, S_IFCHR | 0666, &ino)) return -1;
        ino->dev = DEV_CHR0 + i; chrs[i].ops = ops; chrs[i].minor = minor;
        return i;
    }
    return -1;
}
const struct chrops *chr_ops(int slot) { return slot >= 0 && slot < CHR_MAX ? chrs[slot].ops : NULL; }
int chr_minor(int slot) { return chrs[slot].minor; }
