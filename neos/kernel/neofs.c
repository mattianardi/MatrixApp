#include "fs.h"
#include "vfs.h"

/*
 * NeoFS: durable storage for the in-memory filesystem. The persistent roots (tunable neofs.roots - by default /data /home /root
 * /opt /var /mnt/c /etc/neos, so packages, the Windows C: drive and saved settings survive) are written as one snapshot into
 * one of two slots on the disk; a snapshot is only valid once its header (generation + SHA-256 of the payload) has been
 * written last, so a crash or power loss in the middle of a sync always leaves the previous snapshot intact.
 *   LBA 0 = header slot A, LBA 1 = header slot B, data A from LBA 8, data B from the middle of the disk.
 */
int ata_init(void);
u32 ata_sectors(void);
int ata_read(u32 lba, u32 n, void *buf);
int ata_write(u32 lba, u32 n, const void *buf);

struct hdr { char magic[8]; u64 gen, len; char sha[65]; char pad[3]; u64 hsum; };
static struct tunable *t_roots, *t_sync, *t_max, *t_on;
static int present, cur_slot = -1;
static u64 cur_gen, last_sync_ms; static char last_sha[65]; static u64 total_syncs, last_bytes, last_ms;
static int dirty_hint;

static u64 hdr_sum(const struct hdr *h)
{
    char hex[65]; sha256_hex(h, __builtin_offsetof(struct hdr, hsum), hex);
    u64 v = 0; for (int i = 0; i < 16; i++) v = v << 4 | (u64)(hex[i] <= '9' ? hex[i] - '0' : hex[i] - 'a' + 10);
    return v;
}
static u32 slot_lba(int s) { return s ? 8 + (ata_sectors() - 8) / 2 : 8; }
static u64 slot_cap(void) { return (u64)((ata_sectors() - 16) / 2) * 512; }

static int in_roots(const char *path)
{
    const char *r = t_roots->sval;
    while (*r) {
        while (*r == ' ') r++;
        size_t n = 0; while (r[n] && r[n] != ' ') n++;
        if (n && !strncmp(path, r, n) && (!path[n] || path[n] == '/')) return 1;
        r += n;
    }
    return 0;
}

/* ---- serialisation: "D mode path\n" | "L mode path\ttarget\n" | "F mode size path\n<bytes>\n" ---- */
struct ser { u8 *buf; size_t len, cap; int overflow; };
static void put(struct ser *s, const void *d, size_t n)
{
    if (s->buf) { if (s->len + n <= s->cap) memcpy(s->buf + s->len, d, n); else s->overflow = 1; }
    s->len += n;
}
static void walk_cb(struct inode *i, void *ctx)
{
    struct ser *s = ctx; char line[320];
    if (!in_roots(i->path)) return;
    u32 fmt = i->mode & S_IFMT;
    if (fmt == S_IFDIR) { int n = ksnprintf(line, sizeof line, "D %x %s\n", i->mode & 07777, i->path); put(s, line, (size_t)n); }
    else if (fmt == S_IFLNK) { int n = ksnprintf(line, sizeof line, "L %x %s\t%s\n", i->mode & 07777, i->path, i->link); put(s, line, (size_t)n); }
    else if (fmt == S_IFREG) {
        int n = ksnprintf(line, sizeof line, "F %x %lu %s\n", i->mode & 07777, (u64)i->size, i->path);
        put(s, line, (size_t)n); if (i->size) put(s, i->data, i->size); put(s, "\n", 1);
    }
}

int neofs_sync(int force)
{
    if (!present || !t_on->val) return -1;
    struct ser s = { 0 };
    vfs_walk(walk_cb, &s);                                    /* pass 1: size */
    if (s.len > (size_t)t_max->val * 1048576 || s.len > slot_cap()) return -2;
    size_t pages = (s.len + 4095) / 4096 + 1;
    u64 pa = pmm_alloc(pages);
    if (!pa) return -3;
    u8 *buf = P2V(pa);
    size_t want = s.len; s.buf = buf; s.cap = pages * 4096; s.len = 0;
    vfs_walk(walk_cb, &s);                                    /* pass 2: bytes (the tree may have changed in between: then retry next time) */
    int rc = 0;
    if (s.overflow || s.len != want) { rc = -4; goto out; }
    char sha[65]; sha256_hex(buf, s.len, sha);
    if (!force && !strcmp(sha, last_sha)) goto out;           /* nothing changed since the last snapshot */
    u64 t0 = time_ns();
    int slot = cur_slot == 0 ? 1 : 0;
    u32 nsec = (u32)((s.len + 511) / 512);
    memset(buf + s.len, 0, nsec * 512 - s.len);
    if (nsec && ata_write(slot_lba(slot), nsec, buf)) { rc = -5; goto out; }
    struct hdr h; static u8 sector[512];
    memset(&h, 0, sizeof h); memcpy(h.magic, "NEOFS1\0", 8); h.gen = cur_gen + 1; h.len = s.len; strlcpy(h.sha, sha, sizeof h.sha); h.hsum = hdr_sum(&h);
    memset(sector, 0, 512); memcpy(sector, &h, sizeof h);
    if (ata_write((u32)slot, 1, sector)) { rc = -5; goto out; }   /* the header goes last: this is the commit */
    cur_slot = slot; cur_gen = h.gen; strlcpy(last_sha, sha, sizeof last_sha);
    total_syncs++; last_bytes = s.len; last_ms = (time_ns() - t0) / 1000000;
out:
    for (size_t k = 0; k < pages; k++) pmm_free(pa + k * 4096, 1);
    return rc;
}

static int read_hdr(int slot, struct hdr *h)
{
    static u8 sector[512];
    if (ata_read((u32)slot, 1, sector)) return -1;
    memcpy(h, sector, sizeof *h);
    if (memcmp(h->magic, "NEOFS1\0", 8) || h->hsum != hdr_sum(h) || h->len > slot_cap()) return -1;
    return 0;
}

static int apply(const u8 *b, size_t n)
{
    size_t p = 0; int files = 0;
    while (p < n) {
        const u8 *nl = NULL; for (size_t k = p; k < n; k++) if (b[k] == '\n') { nl = b + k; break; }
        if (!nl) break;
        char line[320]; size_t ll = (size_t)(nl - (b + p)); if (ll >= sizeof line) return -1;
        memcpy(line, b + p, ll); line[ll] = 0; p += ll + 1;
        u32 mode = 0; char *q = line + 2;
        while (is_digit(*q) || (*q >= 'a' && *q <= 'f')) mode = mode * 16 + (u32)(*q <= '9' ? *q - '0' : *q - 'a' + 10), q++;
        while (*q == ' ') q++;
        if (line[0] == 'D') { struct inode *d = vfs_lookup(q); if (!d) vfs_mkdir(q, mode); }
        else if (line[0] == 'L') { char *tab = strchr(q, '\t'); if (tab) { *tab = 0; if (!vfs_lookup(q)) vfs_symlink(tab + 1, q); } }
        else if (line[0] == 'F') {
            u64 sz = 0; while (is_digit(*q)) sz = sz * 10 + (u64)(*q++ - '0');
            while (*q == ' ') q++;
            if (p + sz + 1 > n) return -1;
            struct inode *i = vfs_lookup(q);
            if (!i) vfs_create(q, S_IFREG | mode, &i);
            if (i) { inode_truncate(i, 0); if (sz) inode_write(i, 0, b + p, sz); i->mode = S_IFREG | mode; files++; }
            p += sz + 1;
        } else return -1;
    }
    return files;
}

static void apply_settings(void)
{
    const u8 *d; size_t n;
    if (fs_read("/etc/neos/tunables.conf", &d, &n)) return;
    char line[96]; size_t i = 0; int applied = 0;
    while (i < n) {
        size_t l = 0; while (i + l < n && d[i + l] != '\n') l++;
        if (l && l < sizeof line) {
            memcpy(line, d + i, l); line[l] = 0;
            char *eq = strchr(line, '=');
            if (eq) { *eq = 0; const char *m = ""; if (tune_set(line, eq + 1, &m) >= 0) applied++; }
        }
        i += l + 1;
    }
    kprintf("[neofs] %d saved settings applied\n", applied);
}

int neofs_load(void)
{
    if (!present) return -1;
    struct hdr a, b; int ga = read_hdr(0, &a), gb = read_hdr(1, &b);
    int slot = (ga == 0 && (gb || a.gen > b.gen)) ? 0 : (gb == 0 ? 1 : -1);
    if (slot < 0) { kprintf("[neofs] disk has no valid snapshot yet (fresh disk)\n"); return 0; }
    struct hdr *h = slot ? &b : &a;
    size_t pages = (h->len + 4095) / 4096 + 1; u64 pa = pmm_alloc(pages);
    if (!pa) return -1;
    u8 *buf = P2V(pa); int rc = -1; char sha[65];
    if (h->len && ata_read(slot_lba(slot), (u32)((h->len + 511) / 512), buf)) goto out;
    sha256_hex(buf, h->len, sha);
    if (strcmp(sha, h->sha)) {                                  /* the newest snapshot is damaged: fall back to the other slot */
        kprintf("[neofs] slot %c is damaged, trying the other one\n", 'A' + slot);
        struct hdr *o = slot ? &a : &b; int ok = slot ? ga : gb;
        if (ok) goto out;
        slot = !slot; h = o;
        if (h->len && ata_read(slot_lba(slot), (u32)((h->len + 511) / 512), buf)) goto out;
        sha256_hex(buf, h->len, sha);
        if (strcmp(sha, h->sha)) goto out;
    }
    rc = apply(buf, h->len);
    if (rc >= 0) { cur_slot = slot; cur_gen = h->gen; strlcpy(last_sha, h->sha, sizeof last_sha); kprintf("[neofs] restored snapshot #%lu: %d files from the disk\n", h->gen, rc); }
out:
    for (size_t k = 0; k < pages; k++) pmm_free(pa + k * 4096, 1);
    if (rc < 0) kprintf("[neofs] could not restore the disk snapshot\n");
    else apply_settings();
    return rc;
}

int neofs_format(void)
{
    static u8 z[512]; if (!present) return -1;
    memset(z, 0, 512);
    ata_write(0, 1, z); ata_write(1, 1, z);
    cur_slot = -1; cur_gen = 0; last_sha[0] = 0;
    return 0;
}

int neofs_save_settings(void)
{
    static char buf[2048]; tune_save_text(buf, sizeof buf);
    return fs_write("/etc/neos/tunables.conf", buf, strlen(buf));
}

void neofs_status(void)
{
    if (!present) { kprintf("no disk attached (start QEMU with -drive file=disk.img,format=raw,if=ide)\n"); return; }
    kprintf("disk      : %lu MiB, ATA PIO\n", (u64)ata_sectors() / 2048);
    char shortsha[20]; strlcpy(shortsha, last_sha[0] ? last_sha : "none", 17);
    kprintf("snapshot  : #%lu in slot %c, sha %s, %lu syncs this boot (last %lu bytes in %lu ms)\n", cur_gen, cur_slot < 0 ? '-' : 'A' + cur_slot, shortsha, total_syncs, last_bytes, last_ms);
    kprintf("persisted : %s\n", t_roots->sval);
    kprintf("auto-sync : every %lu s (%s)\n", (u64)t_sync->val, t_on->val ? "on" : "off");
}

static void neofs_thread(void *arg)
{
    (void)arg;
    for (;;) {
        thread_sleep_ms((u64)t_sync->val * 1000);
        if (t_sync->val) neofs_sync(0);
    }
}

void neofs_init(void)
{
    module_register("neofs", "1.0", "durable storage: ATA disk, crash-safe double-slot snapshots of the persistent roots");
    t_on    = tune_bool("neofs", "enable", "write snapshots to the disk", TF_LIVE, 1, NULL);
    t_roots = tune_str ("neofs", "roots", "directories kept on the disk (space separated)", TF_LIVE, "/data /home /root /opt /var /mnt/c /etc/neos /etc/ssl /lib64 /lib/x86_64-linux-gnu", NULL);
    t_sync  = tune_int ("neofs", "sync_s", "automatic snapshot period in seconds (0 = only on `disk sync` and shutdown)", TF_LIVE, 5, 0, 3600, NULL);
    t_max   = tune_int ("neofs", "max_mb", "largest snapshot accepted (MiB)", TF_LIVE, 16, 1, 128, NULL);
    present = ata_init() == 0;
    (void)dirty_hint; (void)last_sync_ms;
}

void neofs_start(void) { if (present) thread_create("neofs", 7, neofs_thread, NULL); }
int neofs_present(void) { return present; }
