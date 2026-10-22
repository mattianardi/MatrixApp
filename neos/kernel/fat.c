#include "usb.h"
#include "fs.h"
#include "vfs.h"

/*
 * FAT12/16/32 volume of the USB stick, mounted as a RAM copy at /mnt/usb (the same model as the rest of NEOS: files live in the
 * filesystem; NeoFS/`usb sync` make them durable). `usb mount` reads every file (up to usb.mount_max_mb); `usb sync` / `usb umount`
 * write back what changed: rewritten files, new files and directories (long names included), deletions.
 */
#define SECT 512
struct fnode { char path[256]; u32 cluster, size, dir_cluster, idx; u8 nlfn, isdir, seen; u32 hash; };
#define MAXN 2048
static struct fnode *nodes; static int nn;
static int mounted; static char root[32];
static u64 part_lba; static u32 spc, rsvd, nfats, fatsz, root_ents, tot_sectors, root_clu, ncl;
static u64 fat_lba, root_lba, data_lba; static int ftype;                 /* 12 / 16 / 32 */
static u8 *fat; static size_t fat_bytes; static int fat_dirty;
static u32 free_hint = 2;

static int rd(u64 lba, u32 n, void *b) { return usb_blk_read(part_lba + lba, n, b); }
static int wr(u64 lba, u32 n, const void *b) { return usb_blk_write(part_lba + lba, n, b); }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

static u32 fat_get(u32 c)
{
    if (ftype == 32) return le32(fat + c * 4) & 0x0FFFFFFF;
    if (ftype == 16) return le16(fat + c * 2);
    u32 o = c + c / 2; u32 v = le16(fat + o);
    return (c & 1) ? v >> 4 : v & 0xFFF;
}
static void fat_set(u32 c, u32 val)
{
    fat_dirty = 1;
    if (ftype == 32) { put32(fat + c * 4, (le32(fat + c * 4) & 0xF0000000) | (val & 0x0FFFFFFF)); return; }
    if (ftype == 16) { put16(fat + c * 2, (u16)val); return; }
    u32 o = c + c / 2; u16 v = le16(fat + o);
    v = (c & 1) ? (u16)((v & 0x000F) | (val << 4)) : (u16)((v & 0xF000) | (val & 0xFFF));
    put16(fat + o, v);
}
static int is_eoc(u32 v) { return ftype == 32 ? v >= 0x0FFFFFF8 : ftype == 16 ? v >= 0xFFF8 : v >= 0xFF8; }
static u32 eoc_val(void) { return ftype == 32 ? 0x0FFFFFFF : ftype == 16 ? 0xFFFF : 0xFFF; }
static u64 clu_lba(u32 c) { return data_lba + (u64)(c - 2) * spc; }

static u32 alloc_cluster(void)
{
    for (u32 n = 0; n < ncl; n++) {
        u32 c = 2 + (free_hint - 2 + n) % ncl;
        if (fat_get(c) == 0) { fat_set(c, eoc_val()); free_hint = c + 1; return c; }
    }
    return 0;
}
static void free_chain(u32 c) { while (c >= 2 && c < ncl + 2 && !is_eoc(c)) { u32 nx = fat_get(c); fat_set(c, 0); c = nx; if (nx == 0) break; } }

/* the bytes of a cluster chain, as one buffer (kmalloc'd) */
static u8 *read_chain(u32 c, size_t limit, size_t *outlen)
{
    size_t csz = (size_t)spc * SECT, cap = csz * 4, len = 0;
    u8 *b = kmalloc(cap); if (!b) return NULL;
    while (c >= 2 && c < ncl + 2 && !is_eoc(c) && len < limit) {
        if (len + csz > cap) { size_t nc = cap * 2; u8 *nb = kmalloc(nc); if (!nb) { kfree(b); return NULL; } memcpy(nb, b, len); kfree(b); b = nb; cap = nc; }
        if (rd(clu_lba(c), spc, b + len)) { kfree(b); return NULL; }
        len += csz; c = fat_get(c);
    }
    *outlen = len;
    return b;
}

/* directory stream access: cluster 0 means the fixed FAT12/16 root directory */
static u8 *read_dir(u32 clu, size_t *len)
{
    if (clu == 0 && ftype != 32) {
        size_t n = (size_t)root_ents * 32; u8 *b = kmalloc(n ? n : 32); if (!b) return NULL;
        if (rd(root_lba, (u32)(n / SECT), b)) { kfree(b); return NULL; }
        *len = n; return b;
    }
    return read_chain(clu ? clu : root_clu, 64u << 20, len);
}
static int write_dir_entry_bytes(u32 clu, u32 idx, const u8 *ent)           /* 32 bytes at entry `idx` of the directory */
{
    u8 sec[SECT]; u32 off = idx * 32; u64 lba;
    if (clu == 0 && ftype != 32) lba = root_lba + off / SECT;
    else {
        u32 c = clu ? clu : root_clu, cs = spc * SECT, k = off / cs;
        while (k-- && c >= 2 && !is_eoc(c)) c = fat_get(c);
        if (c < 2 || is_eoc(c)) return -1;
        lba = clu_lba(c) + (off % cs) / SECT;
    }
    if (rd(lba, 1, sec)) return -1;
    memcpy(sec + off % SECT, ent, 32);
    return wr(lba, 1, sec);
}

static u32 hash_bytes(const u8 *d, size_t n) { u32 h = 2166136261u; for (size_t i = 0; i < n; i++) h = (h ^ d[i]) * 16777619u; return h ^ (u32)n; }

static struct fnode *add_node(const char *path, u32 cluster, u32 size, u32 dir_cluster, u32 idx, u8 nlfn, u8 isdir)
{
    if (nn >= MAXN) return NULL;
    struct fnode *f = &nodes[nn++]; memset(f, 0, sizeof *f);
    strlcpy(f->path, path, sizeof f->path); f->cluster = cluster; f->size = size; f->dir_cluster = dir_cluster; f->idx = idx; f->nlfn = nlfn; f->isdir = isdir;
    return f;
}

static struct tunable *t_max;
static size_t copied;

static int scan_dir(u32 clu, const char *vpath, int depth)
{
    size_t dl; u8 *d = read_dir(clu, &dl); if (!d) return -1;
    char lfn[260]; int lfn_n = 0; lfn[0] = 0; u8 lfn_chk = 0;
    for (u32 i = 0; (size_t)(i + 1) * 32 <= dl; i++) {
        u8 *e = d + (size_t)i * 32;
        if (e[0] == 0) break;
        if (e[0] == 0xE5) { lfn_n = 0; lfn[0] = 0; continue; }
        if (e[11] == 0x0F) {                                          /* long-name piece: 13 UCS-2 chars, last piece first */
            int seq = e[0] & 0x1F; if (seq < 1 || seq > 20) continue;
            if (e[0] & 0x40) { memset(lfn, 0, sizeof lfn); lfn_n = seq; lfn_chk = e[13]; }
            static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (int k = 0; k < 13; k++) { u16 ch = le16(e + pos[k]); int at = (seq - 1) * 13 + k; if (at < 255 && ch && ch != 0xFFFF) lfn[at] = ch < 128 ? (char)ch : '_'; }
            continue;
        }
        if (e[11] & 0x08) { lfn_n = 0; continue; }                    /* volume label */
        char nm[16]; int k = 0;
        for (int j = 0; j < 8 && e[j] != ' '; j++) nm[k++] = (e[12] & 8) ? (char)to_lower(e[j]) : (char)e[j];
        if (e[8] != ' ') { nm[k++] = '.'; for (int j = 8; j < 11 && e[j] != ' '; j++) nm[k++] = (e[12] & 16) ? (char)to_lower(e[j]) : (char)e[j]; }
        nm[k] = 0;
        u8 sum = 0; for (int j = 0; j < 11; j++) sum = (u8)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + e[j]);
        const char *name = (lfn_n && lfn[0] && sum == lfn_chk) ? lfn : nm;
        int nl = (lfn_n && lfn[0] && sum == lfn_chk) ? lfn_n : 0;
        int dot = !strcmp(nm, ".") || !strcmp(nm, "..");
        u32 first = (u32)le16(e + 26) | (ftype == 32 ? (u32)le16(e + 20) << 16 : 0), size = le32(e + 28);
        lfn_n = 0; lfn[0] = 0;
        if (dot) continue;
        char path[256]; ksnprintf(path, sizeof path, "%s/%s", vpath, name);
        if (e[11] & 0x10) {
            vfs_mkdir(path, 0755);
            struct fnode *f = add_node(path, first, 0, clu, i, (u8)nl, 1); if (f) f->seen = 1;
            if (depth < 12 && first >= 2 && scan_dir(first, path, depth + 1)) { kfree(d); return -1; }
        } else {
            if (copied + size > (size_t)t_max->val * 1048576) { kprintf("[usb] skipping %s (over usb.mount_max_mb)\n", path); continue; }
            size_t got = 0; u8 *data = size ? read_chain(first, size, &got) : NULL;
            if (size && !data) { kprintf("[usb] cannot read %s\n", path); continue; }
            struct inode *ino; vfs_create(path, S_IFREG | 0644, &ino);
            if (ino) { inode_truncate(ino, 0); if (size) inode_write(ino, 0, data, size); }
            struct fnode *f = add_node(path, first, size, clu, i, (u8)nl, 0); if (f) f->hash = hash_bytes(data ? data : (u8 *)"", size);
            if (data) kfree(data);
            copied += size;
        }
    }
    kfree(d);
    return 0;
}

int fat_mount(const char *where, const char **err)
{
    if (!usb_blk_present()) { *err = "no USB storage device"; return -1; }
    if (mounted) { *err = "already mounted (usb umount first)"; return -1; }
    static u8 sec[SECT];
    t_max = tune_find("usb.mount_max_mb");
    if (!t_max) t_max = tune_int("usb", "mount_max_mb", "most data `usb mount` copies into RAM (MiB)", TF_LIVE, 64, 1, 512, NULL);
    part_lba = 0;
    if (usb_blk_read(0, 1, sec)) { *err = "cannot read the device"; return -1; }
    if (le16(sec + 510) != 0xAA55) { *err = "no partition table or FAT signature"; return -1; }
    if (sec[0] != 0xEB && sec[0] != 0xE9) {                                          /* MBR: first usable partition */
        u32 start = 0;
        for (int i = 0; i < 4; i++) { u8 *pe = sec + 446 + i * 16; u8 t = pe[4]; if (t == 1 || t == 4 || t == 6 || t == 0xB || t == 0xC || t == 0xE) { start = le32(pe + 8); break; } if (t == 0xEE) { *err = "GPT disks are not supported yet"; return -1; } }
        if (!start) { *err = "no FAT partition found"; return -1; }
        part_lba = start;
        if (usb_blk_read(part_lba, 1, sec)) { *err = "cannot read the FAT partition"; return -1; }
    }
    if (le16(sec + 11) != SECT) { *err = "unsupported sector size"; return -1; }
    spc = sec[13]; rsvd = le16(sec + 14); nfats = sec[16]; root_ents = le16(sec + 17);
    tot_sectors = le16(sec + 19) ? le16(sec + 19) : le32(sec + 32);
    fatsz = le16(sec + 22) ? le16(sec + 22) : le32(sec + 36);
    root_clu = le32(sec + 44);
    if (!spc || !nfats || !fatsz || !tot_sectors) { *err = "not a FAT volume"; return -1; }
    u32 root_secs = (root_ents * 32 + SECT - 1) / SECT;
    fat_lba = rsvd; root_lba = rsvd + (u64)nfats * fatsz; data_lba = root_lba + root_secs;
    ncl = (tot_sectors - (u32)data_lba) / spc;
    ftype = ncl < 4085 ? 12 : ncl < 65525 ? 16 : 32;
    fat_bytes = (size_t)fatsz * SECT;
    if (fat_bytes > (16u << 20)) { *err = "FAT too large"; return -1; }
    fat = kmalloc(fat_bytes); nodes = kmalloc(sizeof(struct fnode) * MAXN);
    if (!fat || !nodes) { *err = "out of memory"; return -1; }
    if (rd(fat_lba, fatsz, fat)) { kfree(fat); kfree(nodes); *err = "cannot read the FAT"; return -1; }
    strlcpy(root, where, sizeof root); vfs_mkdir(root, 0755);
    nn = 0; copied = 0; fat_dirty = 0; free_hint = 2;
    mounted = 1;
    if (scan_dir(ftype == 32 ? root_clu : 0, root, 0)) { mounted = 0; kfree(fat); kfree(nodes); *err = "read error while scanning"; return -1; }
    kprintf("mounted FAT%d volume at %s: %d entries, %lu KiB copied\n", ftype, root, nn, (u64)copied / 1024);
    return 0;
}

/* ---- write back ---- */
static u8 short_sum(const u8 *n) { u8 s = 0; for (int i = 0; i < 11; i++) s = (u8)(((s & 1) ? 0x80 : 0) + (s >> 1) + n[i]); return s; }

static void fat_time(u8 *e)
{
    u64 t = vfs_now(); u32 days = (u32)(t / 86400), secs = (u32)(t % 86400);
    int y = 1970; for (;;) { int dy = (y % 4 == 0) ? 366 : 365; if (days < (u32)dy) break; days -= (u32)dy; y++; }
    static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }; int m = 0;
    for (; m < 12; m++) { int d = dm[m] + (m == 1 && y % 4 == 0); if (days < (u32)d) break; days -= (u32)d; }
    u16 date = (u16)(((y - 1980) << 9) | ((m + 1) << 5) | (days + 1)), tm = (u16)(((secs / 3600) << 11) | (((secs / 60) % 60) << 5) | ((secs % 60) / 2));
    put16(e + 14, tm); put16(e + 16, date); put16(e + 18, date); put16(e + 22, tm); put16(e + 24, date);
}

static int write_chain(const u8 *data, u32 size, u32 *first)
{
    *first = 0;
    if (!size) return 0;
    size_t csz = (size_t)spc * SECT; u32 prev = 0;
    u8 *tmp = kmalloc(csz); if (!tmp) return -1;
    for (size_t off = 0; off < size; off += csz) {
        u32 c = alloc_cluster(); if (!c) { kfree(tmp); return -1; }
        if (prev) fat_set(prev, c); else *first = c;
        memset(tmp, 0, csz); memcpy(tmp, data + off, size - off < csz ? size - off : csz);
        if (wr(clu_lba(c), spc, tmp)) { kfree(tmp); return -1; }
        prev = c;
    }
    kfree(tmp);
    return 0;
}

/* find `count` consecutive free entries in a directory (extending it with a new cluster when it is full) */
static int dir_find_free(u32 clu, int count, u32 *idx_out)
{
    size_t dl; u8 *d = read_dir(clu, &dl); if (!d) return -1;
    u32 total = (u32)(dl / 32), run = 0, start = 0;
    for (u32 i = 0; i < total; i++) {
        if (d[i * 32] == 0 || d[i * 32] == 0xE5) { if (!run) start = i; if (++run >= (u32)count) { kfree(d); *idx_out = start; return 0; } } else run = 0;
    }
    kfree(d);
    if (clu == 0 && ftype != 32) return -1;                                          /* the FAT12/16 root cannot grow */
    u32 c = clu ? clu : root_clu; while (!is_eoc(fat_get(c))) c = fat_get(c);
    u32 n = alloc_cluster(); if (!n) return -1;
    fat_set(c, n);
    u8 *z = kmalloc((size_t)spc * SECT); if (!z) return -1; memset(z, 0, (size_t)spc * SECT);
    int r = wr(clu_lba(n), spc, z); kfree(z);
    *idx_out = total - (run ? run : 0) + 0; if (run) *idx_out = start; else *idx_out = total;
    return r;
}

static int make_short(u32 dir, const char *name, u8 out[11])
{
    char base[9] = "", ext[4] = ""; int bi = 0, ei = 0; const char *dot = strrchr(name, '.');
    for (const char *p = name; *p && (!dot || p < dot); p++) { char c = *p; if (c == ' ' || c == '.') continue; if (is_alpha(c)) c = (char)to_upper(c); else if (!is_digit(c) && !strchr("_-$~!#%&'()@^`{}", c)) c = '_'; if (bi < 8) base[bi++] = c; }
    if (dot) for (const char *p = dot + 1; *p && ei < 3; p++) { char c = *p; if (is_alpha(c)) c = (char)to_upper(c); else if (!is_digit(c)) c = '_'; ext[ei++] = c; }
    if (!bi) base[bi++] = '_';
    size_t dl; u8 *d = read_dir(dir, &dl); if (!d) return -1;
    for (int n = 0; n < 1000; n++) {
        char b2[9]; strlcpy(b2, base, sizeof b2);
        if (n || strlen(name) > 12 || (dot && strlen(dot + 1) > 3) || strchr(name, ' ')) { char suf[8]; int sl = ksnprintf(suf, sizeof suf, "~%d", n + 1); if (bi + sl > 8) b2[8 - sl] = 0; strlcat(b2, suf, sizeof b2); }
        memset(out, ' ', 11); memcpy(out, b2, strlen(b2)); memcpy(out + 8, ext, (size_t)ei);
        int clash = 0; for (size_t i = 0; i + 32 <= dl; i += 32) if (d[i] != 0 && d[i] != 0xE5 && d[i + 11] != 0x0F && !memcmp(d + i, out, 11)) clash = 1;
        if (!clash) { kfree(d); return 0; }
    }
    kfree(d); return -1;
}

static int create_entry(u32 dir, const char *name, int isdir, u32 first, u32 size, struct fnode *out)
{
    u8 sn[11]; if (make_short(dir, name, sn)) return -1;
    int nl = ((int)strlen(name) + 12) / 13, need = nl + 1; u32 idx;
    if (dir_find_free(dir, need, &idx)) return -1;
    u8 chk = short_sum(sn); size_t len = strlen(name);
    for (int k = 0; k < nl; k++) {                                                   /* long-name entries, last piece first */
        u8 e[32]; memset(e, 0xFF, 32); int seq = nl - k;
        e[0] = (u8)(seq | (k == 0 ? 0x40 : 0)); e[11] = 0x0F; e[12] = 0; e[13] = chk; put16(e + 26, 0);
        static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        for (int j = 0; j < 13; j++) { size_t at = (size_t)(seq - 1) * 13 + (size_t)j; u16 ch = at < len ? (u8)name[at] : at == len ? 0 : 0xFFFF; put16(e + pos[j], ch); }
        if (write_dir_entry_bytes(dir, idx + (u32)k, e)) return -1;
    }
    u8 e[32]; memset(e, 0, 32); memcpy(e, sn, 11); e[11] = isdir ? 0x10 : 0x20;
    fat_time(e); put16(e + 26, (u16)first); if (ftype == 32) put16(e + 20, (u16)(first >> 16)); put32(e + 28, size);
    if (write_dir_entry_bytes(dir, idx + (u32)nl, e)) return -1;
    if (out) { out->dir_cluster = dir; out->idx = idx + (u32)nl; out->nlfn = (u8)nl; }
    return 0;
}

static int update_entry(struct fnode *f, u32 first, u32 size)
{
    size_t dl; u8 *d = read_dir(f->dir_cluster, &dl); if (!d) return -1;
    u8 e[32]; memcpy(e, d + (size_t)f->idx * 32, 32); kfree(d);
    put16(e + 26, (u16)first); if (ftype == 32) put16(e + 20, (u16)(first >> 16)); put32(e + 28, size); fat_time(e);
    return write_dir_entry_bytes(f->dir_cluster, f->idx, e);
}

static void delete_entry(struct fnode *f)
{
    size_t dl; u8 *d = read_dir(f->dir_cluster, &dl); if (!d) return;
    for (int k = 0; k <= f->nlfn; k++) { u8 e[32]; memcpy(e, d + (size_t)(f->idx - (u32)k) * 32, 32); e[0] = 0xE5; write_dir_entry_bytes(f->dir_cluster, f->idx - (u32)k, e); }
    kfree(d);
}

static struct fnode *find_dir_node(const char *vpath) { if (!strcmp(vpath, root)) return NULL; for (int i = 0; i < nn; i++) if (nodes[i].isdir && !strcmp(nodes[i].path, vpath)) return &nodes[i]; return NULL; }

struct sync_ctx { char (*dirs)[256]; int ndirs; };
static void sync_cb(struct inode *i, void *ctxp)
{
    struct sync_ctx *sc = ctxp;
    size_t rl = strlen(root);
    if (strncmp(i->path, root, rl) || i->path[rl] != '/') return;
    for (int k = 0; k < nn; k++) if (!strcmp(nodes[k].path, i->path)) { nodes[k].seen = 1; return; }
    if ((i->mode & S_IFMT) == S_IFDIR && sc->ndirs < 256) strlcpy(sc->dirs[sc->ndirs++], i->path, 256);       /* a directory the stick does not have yet */
}

static int depth_of(const char *p) { int d = 0; for (; *p; p++) if (*p == '/') d++; return d; }

static int make_dir(const char *dir, const char **err)
{
    char parent[256]; strlcpy(parent, dir, sizeof parent); char *sl = strrchr(parent, '/'); *sl = 0;
    struct fnode *pn = find_dir_node(parent); u32 pc = pn ? pn->cluster : (ftype == 32 ? root_clu : 0);
    u32 nc = alloc_cluster(); if (!nc) { *err = "stick is full"; return -1; }
    u8 *z = kmalloc((size_t)spc * SECT); if (!z) { *err = "out of memory"; return -1; }
    memset(z, 0, (size_t)spc * SECT);
    memset(z, ' ', 11); z[0] = '.'; z[11] = 0x10; put16(z + 26, (u16)nc); if (ftype == 32) put16(z + 20, (u16)(nc >> 16)); fat_time(z);
    memset(z + 32, ' ', 11); z[32] = '.'; z[33] = '.'; z[32 + 11] = 0x10; put16(z + 32 + 26, (u16)(pn ? pn->cluster : 0)); fat_time(z + 32);
    int r = wr(clu_lba(nc), spc, z); kfree(z);
    if (r) { *err = "write error"; return -1; }
    struct fnode tmp; memset(&tmp, 0, sizeof tmp);
    if (create_entry(pc, sl + 1, 1, nc, 0, &tmp)) { *err = "cannot create the directory"; return -1; }
    struct fnode *nf = add_node(dir, nc, 0, pc, tmp.idx, tmp.nlfn, 1); if (nf) nf->seen = 1;
    return 0;
}

int fat_sync(const char **err)
{
    if (!mounted) { *err = "nothing is mounted"; return -1; }
    int changed = 0, created = 0, removed = 0;
    for (int i = 0; i < nn; i++) nodes[i].seen = 0;
    static char dirs[256][256];
    struct sync_ctx sc = { dirs, 0 }; vfs_walk(sync_cb, &sc);
    for (int i = 0; i < nn; i++) if (!nodes[i].seen && !nodes[i].isdir && nodes[i].path[0]) {           /* deleted in the RAM copy */
        free_chain(nodes[i].cluster); delete_entry(&nodes[i]); removed++; nodes[i].path[0] = 0;
    }
    for (int depth = 1; depth < 16; depth++)                                                         /* new directories, shallowest first */
        for (int k = 0; k < sc.ndirs; k++) if (depth_of(dirs[k]) - depth_of(root) == depth && !find_dir_node(dirs[k])) { if (make_dir(dirs[k], err)) return -1; created++; }
    char p[256]; size_t sz;
    for (int j = 0; !fs_info(j, p, sizeof p, &sz); j++) {
        size_t rl = strlen(root); if (strncmp(p, root, rl) || p[rl] != '/') continue;
        const u8 *data; size_t n; if (fs_read(p, &data, &n)) continue;
        struct fnode *f = NULL; for (int k = 0; k < nn; k++) if (nodes[k].path[0] && !nodes[k].isdir && !strcmp(nodes[k].path, p)) { f = &nodes[k]; break; }
        if (f && f->hash == hash_bytes(data, n)) continue;
        u32 first;
        if (f) free_chain(f->cluster);
        if (write_chain(data, (u32)n, &first)) { *err = "stick is full or write error"; return -1; }
        if (f) {
            if (update_entry(f, first, (u32)n)) { *err = "cannot update the directory entry"; return -1; }
            f->cluster = first; f->size = (u32)n; f->hash = hash_bytes(data, n); changed++;
        } else {
            char parent[256]; strlcpy(parent, p, sizeof parent); char *sl = strrchr(parent, '/'); *sl = 0;
            struct fnode *pn = find_dir_node(parent); u32 pc = pn ? pn->cluster : (ftype == 32 ? root_clu : 0);
            struct fnode tmp; memset(&tmp, 0, sizeof tmp);
            if (create_entry(pc, sl + 1, 0, first, (u32)n, &tmp)) { *err = "cannot create the file (directory full?)"; return -1; }
            struct fnode *nf = add_node(p, first, (u32)n, pc, tmp.idx, tmp.nlfn, 0); if (nf) { nf->hash = hash_bytes(data, n); nf->seen = 1; }
            created++;
        }
    }
    if (fat_dirty) {
        for (u32 c = 0; c < nfats; c++) if (wr(fat_lba + (u64)c * fatsz, fatsz, fat)) { *err = "cannot write the FAT"; return -1; }
        fat_dirty = 0;
    }
    kprintf("usb sync: %d rewritten, %d created, %d removed\n", changed, created, removed);
    return 0;
}

void fat_umount(void)
{
    if (!mounted) return;
    fs_remove_tree(root);
    kfree(fat); kfree(nodes); fat = NULL; nodes = NULL; nn = 0; mounted = 0;
}
