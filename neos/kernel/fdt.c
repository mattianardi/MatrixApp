#include "fdt.h"
#include "shell.h"

/* ------------------------------------------------------------ pipes */
#define PIPE_CAP 65536
struct pipe { u8 *buf; u32 head, tail; int readers, writers; };

int pipe_create(struct file **r, struct file **w)
{
    struct pipe *p = kmalloc(sizeof *p);
    if (!p) return -E_NOMEM;
    p->buf = kmalloc(PIPE_CAP);
    if (!p->buf) { kfree(p); return -E_NOMEM; }
    p->head = p->tail = 0; p->readers = p->writers = 1;
    *r = file_new(FT_PIPE_R, 0); *w = file_new(FT_PIPE_W, 1);
    if (!*r || !*w) { if (*r) kfree(*r); if (*w) kfree(*w); kfree(p->buf); kfree(p); return -E_NOMEM; }
    (*r)->pipe = (*w)->pipe = p;
    return 0;
}

/* ------------------------------------------------------------ file objects */
struct file *file_new(int type, int flags)
{
    struct file *f = kmalloc(sizeof *f);
    if (!f) return NULL;
    memset(f, 0, sizeof *f);
    f->type = type; f->flags = flags; f->refs = 1;
    return f;
}
void file_get(struct file *f) { u64 fl = irq_save(); f->refs++; irq_restore(fl); }

void file_put(struct file *f)
{
    u64 fl = irq_save();
    int last = --f->refs == 0;
    irq_restore(fl);
    if (!last) return;
    if (f->pipe) {
        struct pipe *p = f->pipe;
        fl = irq_save();                                       /* count and "am I the last?" decided together, or both closers free it */
        if (f->type == FT_PIPE_R) p->readers--; else p->writers--;
        int dead = !p->readers && !p->writers;
        irq_restore(fl);
        wake_on(p, 64);
        if (dead) { kfree(p->buf); kfree(p); }
    }
    kfree(f);
}

long file_read(struct file *f, void *buf, size_t n)
{
    switch (f->type) {
    case FT_INODE: {
        long r = inode_read(f->ino, (u64)f->off, buf, n);
        if (r > 0) f->off += r;
        return r;
    }
    case FT_DIR: return -E_ISDIR;
    case FT_TTY: return tty_read(buf, n, f->flags & O_NONBLOCK);
    case FT_PIPE_R: {
        struct pipe *p = f->pipe;
        u64 fl = irq_save();
        while (p->head == p->tail) {
            if (!p->writers) { irq_restore(fl); return 0; }
            if (f->flags & O_NONBLOCK) { irq_restore(fl); return -E_AGAIN; }
            sleep_on_locked(p, ~0ull);
        }
        irq_restore(fl);
        size_t avail = p->head - p->tail, c = n < avail ? n : avail;
        u8 *o = buf;
        for (size_t i = 0; i < c; i++) o[i] = p->buf[(p->tail + i) % PIPE_CAP];
        fl = irq_save(); p->tail += (u32)c; irq_restore(fl);
        wake_on(p, 64);
        return (long)c;
    }
    }
    return -E_BADF;
}

long file_write(struct file *f, const void *buf, size_t n)
{
    switch (f->type) {
    case FT_INODE: {
        if (f->flags & O_APPEND) f->off = (i64)f->ino->size;
        long r = inode_write(f->ino, (u64)f->off, buf, n);
        if (r > 0) f->off += r;
        return r;
    }
    case FT_DIR: return -E_ISDIR;
    case FT_TTY: tty_write(buf, n); return (long)n;
    case FT_PIPE_W: {
        struct pipe *p = f->pipe;
        const u8 *in = buf; size_t done = 0;
        while (done < n) {
            u64 fl = irq_save();
            while (p->head - p->tail >= PIPE_CAP) {
                if (!p->readers) { irq_restore(fl); return done ? (long)done : -E_PIPE; }
                if (f->flags & O_NONBLOCK) { irq_restore(fl); return done ? (long)done : -E_AGAIN; }
                sleep_on_locked(p, ~0ull);
            }
            if (!p->readers) { irq_restore(fl); return done ? (long)done : -E_PIPE; }
            irq_restore(fl);
            size_t space = PIPE_CAP - (p->head - p->tail), c = n - done < space ? n - done : space;
            for (size_t i = 0; i < c; i++) p->buf[(p->head + i) % PIPE_CAP] = in[done + i];
            fl = irq_save(); p->head += (u32)c; irq_restore(fl);
            done += c;
            wake_on(p, 64);
        }
        return (long)done;
    }
    }
    return -E_BADF;
}

int file_poll_ready(struct file *f, int events)
{
    int r = 0;
    if (f->type == FT_PIPE_R) { if (events & 1) r |= (f->pipe->head != f->pipe->tail || !f->pipe->writers) ? 1 : 0; }
    else if (f->type == FT_PIPE_W) { if (events & 4) r |= 4; }
    else if (f->type == FT_TTY) { if ((events & 1) && tty_has_input()) r |= 1; if (events & 4) r |= 4; }
    else r = events & 5;
    return r;
}

/* ------------------------------------------------------------ fd tables */
struct fdtable *fdt_new(void)
{
    struct fdtable *t = kmalloc(sizeof *t);
    if (!t) return NULL;
    memset(t, 0, sizeof *t);
    t->refs = 1;
    return t;
}

struct fdtable *fdt_clone(struct fdtable *s)
{
    struct fdtable *t = fdt_new();
    if (!t) return NULL;
    for (int i = 0; i < MAX_FDS; i++) if (s->f[i]) { t->f[i] = s->f[i]; file_get(s->f[i]); t->cloexec[i] = s->cloexec[i]; }
    return t;
}

void fdt_release(struct fdtable *t)
{
    u64 fl = irq_save();
    int last = --t->refs == 0;
    irq_restore(fl);
    if (!last) return;
    for (int i = 0; i < MAX_FDS; i++) if (t->f[i]) file_put(t->f[i]);
    kfree(t);
}

struct file *fdt_get(struct fdtable *t, int fd) { return fd >= 0 && fd < MAX_FDS ? t->f[fd] : NULL; }

int fdt_install(struct fdtable *t, struct file *f, int minfd, int cloexec)
{
    for (int i = minfd < 0 ? 0 : minfd; i < MAX_FDS; i++) if (!t->f[i]) { t->f[i] = f; t->cloexec[i] = (u8)cloexec; return i; }
    return -E_MFILE;
}

int fdt_close(struct fdtable *t, int fd)
{
    if (fd < 0 || fd >= MAX_FDS || !t->f[fd]) return -E_BADF;
    struct file *f = t->f[fd];
    t->f[fd] = NULL;
    file_put(f);
    return 0;
}

void fdt_close_cloexec(struct fdtable *t)
{
    for (int i = 0; i < MAX_FDS; i++) if (t->f[i] && t->cloexec[i]) fdt_close(t, i);
}

/* ------------------------------------------------------------ terminal (cooked) */
static char line_q[4096];
static u32 q_head, q_tail;            /* completed lines available to readers */
static char cur[256];
static int cur_len, eof_pending;

void tty_init(void) { }

int tty_has_input(void) { return q_head != q_tail || eof_pending; }

void tty_feed(int k)
{
    if (k == 4) { if (!cur_len) eof_pending = 1; else { for (int i = 0; i < cur_len; i++) line_q[q_head++ % sizeof line_q] = cur[i]; cur_len = 0; } wake_on(line_q, 8); return; }
    if (k == '\b' || k == 127) { if (cur_len) { cur_len--; kprintf("\b \b"); } return; }
    if (k == '\n' || k == '\r') {
        kputc('\n');
        for (int i = 0; i < cur_len; i++) line_q[q_head++ % sizeof line_q] = cur[i];
        line_q[q_head++ % sizeof line_q] = '\n';
        cur_len = 0;
        wake_on(line_q, 8);
        return;
    }
    if (k >= 32 && k < 127 && cur_len < (int)sizeof cur - 1) { cur[cur_len++] = (char)k; kputc((char)k); }
}

long tty_read(void *buf, size_t n, int nonblock)
{
    u8 *o = buf;
    u64 fl = irq_save();
    while (q_head == q_tail && !eof_pending) {
        if (nonblock) { irq_restore(fl); return -E_AGAIN; }
        sleep_on_locked(line_q, ticks + ms_to_ticks(20));          /* keys arrive by polling: re-check periodically */
        irq_restore(fl);
        extern int tty_pump(void);
        tty_pump();
        fl = irq_save();
    }
    irq_restore(fl);
    if (q_head == q_tail) { eof_pending = 0; return 0; }
    size_t c = 0;
    while (c < n && q_tail != q_head) { char ch = line_q[q_tail++ % sizeof line_q]; o[c++] = (u8)ch; if (ch == '\n') break; }
    return (long)c;
}

void tty_write(const void *buf, size_t n)
{
    const char *s = buf;
    for (size_t i = 0; i < n; i++) kputc(s[i]);
}
