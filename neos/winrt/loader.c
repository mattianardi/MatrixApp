#include "winrt.h"

Module *g_main_module;
static Module *modules;

void rt_die(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    fputs("winrt: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap);
    exit(127);
}

/* ------------------------------------------------------------ paths: the same rules as the NEOS kernel's variable bus */
char *path_win_to_lin(const char *w, char *o, size_t n)
{
    size_t l = 0;
    #define PUT(c) do { if (l + 1 < n) o[l] = (c); l++; } while (0)
    if (w[0] == '\\' && w[1] == '\\') {
        const char *r = w + 2;
        if (!strncasecmp(r, "neos\\root", 9) && (r[9] == '\\' || !r[9])) { r += 9; if (!*r) PUT('/'); }
        else { for (const char *s = "/mnt/unc/"; *s; s++) PUT(*s); }
        for (; *r; r++) PUT(*r == '\\' ? '/' : *r);
    } else if (isalpha((unsigned char)w[0]) && w[1] == ':') {
        for (const char *s = "/mnt/"; *s; s++) PUT(*s);
        PUT((char)tolower((unsigned char)w[0]));
        const char *r = w + 2;
        if (*r != '\\' && *r != '/' && *r) PUT('/');
        for (; *r; r++) PUT(*r == '\\' ? '/' : *r);
    } else if (w[0] == '\\') {
        for (const char *s = "/mnt/c"; *s; s++) PUT(*s);
        for (const char *r = w; *r; r++) PUT(*r == '\\' ? '/' : *r);
    } else for (const char *r = w; *r; r++) PUT(*r == '\\' ? '/' : *r);
    if (n) o[l < n ? l : n - 1] = 0;
    return o;
}

char *path_lin_to_win(const char *p, char *o, size_t n)
{
    size_t l = 0;
    if (!strncmp(p, "/mnt/unc/", 9)) { PUT('\\'); PUT('\\'); p += 9; }
    else if (!strncmp(p, "/mnt/", 5) && isalpha((unsigned char)p[5]) && (p[6] == '/' || !p[6])) { PUT((char)toupper((unsigned char)p[5])); PUT(':'); p += 6; if (!*p) PUT('\\'); }
    else if (p[0] == '/') { for (const char *s = "\\\\neos\\root"; *s; s++) PUT(*s); if (!p[1]) { PUT('\\'); p++; } }
    for (; *p; p++) PUT(*p == '/' ? '\\' : *p);
    if (n) o[l < n ? l : n - 1] = 0;
    return o;
    #undef PUT
}

/* ------------------------------------------------------------ PE loading */
static uint8_t *read_all(const char *path, size_t *n)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st; fstat(fd, &st);
    uint8_t *b = malloc((size_t)st.st_size + 1);
    size_t got = 0; ssize_t r;
    while (got < (size_t)st.st_size && (r = read(fd, b + got, (size_t)st.st_size - got)) > 0) got += (size_t)r;
    close(fd); *n = got;
    return b;
}

static int prot_of(DWORD ch) { int p = 0; if (ch & 0x40000000) p |= PROT_READ; if (ch & 0x80000000) p |= PROT_WRITE; if (ch & 0x20000000) p |= PROT_EXEC; return p ? p : PROT_READ; }

/* an unresolved import becomes a thunk that reports itself when - and only if - the program actually calls it */
static void unresolved(const char *what) { fprintf(stderr, "\nwinrt: unimplemented Windows API called: %s\n", what); fflush(stderr); _exit(127); }
static void *make_stub(const char *dll, const char *name)
{
    static uint8_t *page; static size_t used;
    if (!page || used + 32 > 4096) { page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); used = 0; }
    char *label = malloc(160); snprintf(label, 160, "%s!%s", dll, name);
    uint8_t *p = page + used; used += 32;
    p[0] = 0x48; p[1] = 0xB9; memcpy(p + 2, &label, 8);                          /* mov rcx, label */
    void *h = (void *)unresolved;
    p[10] = 0x48; p[11] = 0xB8; memcpy(p + 12, &h, 8);                           /* mov rax, unresolved */
    p[20] = 0xFF; p[21] = 0xE0;                                                  /* jmp rax */
    return p;
}

void *pe_export(Module *m, const char *name)
{
    if (!m->nt->Opt.Dir[0].Size) return NULL;
    ExportDir *ed = (ExportDir *)(m->base + m->nt->Opt.Dir[0].VirtualAddress);
    DWORD *names = (DWORD *)(m->base + ed->AddressOfNames), *funcs = (DWORD *)(m->base + ed->AddressOfFunctions);
    WORD *ords = (WORD *)(m->base + ed->AddressOfNameOrdinals);
    for (DWORD i = 0; i < ed->NumberOfNames; i++) if (!strcmp((char *)m->base + names[i], name)) return m->base + funcs[ords[i]];
    return NULL;
}

static Module *find_loaded(const char *dll)
{
    char base[64]; size_t i = 0;
    for (; dll[i] && i < 63; i++) base[i] = (char)tolower((unsigned char)dll[i]);
    base[i] = 0;
    for (Module *m = modules; m; m = m->next) {
        char mn[64]; size_t k = 0; for (; m->name[k] && k < 63; k++) mn[k] = (char)tolower((unsigned char)m->name[k]); mn[k] = 0;
        if (!strcmp(mn, base)) return m;
    }
    return NULL;
}

void *pe_resolve_import(const char *dll, const char *name, int ordinal)
{
    (void)ordinal;
    Module *m = find_loaded(dll);
    if (m && name) { void *p = pe_export(m, name); if (p) return p; }
    if (name) { void *p = rt_resolve(dll, name); if (p) return p; }
    if (!m && name) {                                                            /* a real DLL next to the program or in the Windows directory */
        char w[300], l[300];
        snprintf(w, sizeof w, "%s\\%s", g_exe_path_win, dll);
        char dir[260]; strlcpy(dir, g_exe_path_lin, sizeof dir); char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
        snprintf(l, sizeof l, "%s/%s", dir, dll);
        if (access(l, R_OK) == 0) { Module *d = pe_load(l, dll, 1); if (d) { void *p = pe_export(d, name); if (p) return p; } }
    }
    return make_stub(dll, name ? name : "(ordinal)");
}

static void apply_relocs(Module *m, long delta)
{
    NtHeaders *nt = m->nt;
    if (!delta || !nt->Opt.Dir[5].Size) return;
    uint8_t *p = m->base + nt->Opt.Dir[5].VirtualAddress, *end = p + nt->Opt.Dir[5].Size;
    while (p < end) {
        RelocBlock *rb = (RelocBlock *)p;
        if (rb->SizeOfBlock < 8) break;
        WORD *ent = (WORD *)(p + 8);
        for (DWORD i = 0; i < (rb->SizeOfBlock - 8) / 2; i++) {
            int type = ent[i] >> 12; uint8_t *where = m->base + rb->VirtualAddress + (ent[i] & 0xFFF);
            if (type == 10) *(QWORD *)where += (QWORD)delta;
            else if (type == 3) *(DWORD *)where += (DWORD)delta;
        }
        p += rb->SizeOfBlock;
    }
}

static void bind_imports(Module *m)
{
    NtHeaders *nt = m->nt;
    if (!nt->Opt.Dir[1].Size) return;
    for (ImportDesc *d = (ImportDesc *)(m->base + nt->Opt.Dir[1].VirtualAddress); d->Name; d++) {
        const char *dll = (const char *)m->base + d->Name;
        QWORD *oft = (QWORD *)(m->base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk)), *iat = (QWORD *)(m->base + d->FirstThunk);
        for (; *oft; oft++, iat++) {
            if (*oft >> 63) *iat = (QWORD)pe_resolve_import(dll, NULL, (int)(*oft & 0xFFFF));
            else *iat = (QWORD)pe_resolve_import(dll, (const char *)m->base + (*oft & 0x7FFFFFFF) + 2, 0);
        }
    }
}

Module *pe_load(const char *path, const char *name, int is_dll)
{
    size_t n; uint8_t *f = read_all(path, &n);
    if (!f) return NULL;
    DosHeader *dh = (DosHeader *)f;
    if (n < 0x200 || dh->e_magic != 0x5A4D) rt_die("%s is not a PE file", name);
    NtHeaders *nt = (NtHeaders *)(f + dh->e_lfanew);
    if (nt->Signature != 0x4550) rt_die("%s: bad PE signature", name);
    if (nt->File.Machine != 0x8664 || nt->Opt.Magic != 0x20B) rt_die("%s is not a 64-bit Windows program (32-bit PE is not supported)", name);
    size_t size = (nt->Opt.SizeOfImage + 0xFFF) & ~0xFFFul;
    uint8_t *want = (uint8_t *)nt->Opt.ImageBase;
    uint8_t *base = mmap(want, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (base == MAP_FAILED) base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) rt_die("cannot map %s (%zu bytes)", name, size);
    memcpy(base, f, nt->Opt.SizeOfHeaders);
    Section *sec = (Section *)((uint8_t *)&nt->Opt + nt->File.SizeOfOptionalHeader);
    for (int i = 0; i < nt->File.NumberOfSections; i++) if (sec[i].SizeOfRawData && sec[i].PointerToRawData + sec[i].SizeOfRawData <= n)
        memcpy(base + sec[i].VirtualAddress, f + sec[i].PointerToRawData, sec[i].SizeOfRawData < sec[i].VirtualSize || !sec[i].VirtualSize ? sec[i].SizeOfRawData : sec[i].VirtualSize);
    Module *m = calloc(1, sizeof *m);
    strlcpy(m->name, name, sizeof m->name);
    m->base = base; m->size = size; m->nt = (NtHeaders *)(base + dh->e_lfanew); m->is_dll = is_dll;
    m->entry = (QWORD)base + m->nt->Opt.AddressOfEntryPoint;
    m->next = modules; modules = m;
    apply_relocs(m, (long)(base - want));
    bind_imports(m);
    sec = (Section *)((uint8_t *)&m->nt->Opt + m->nt->File.SizeOfOptionalHeader);
    for (int i = 0; i < m->nt->File.NumberOfSections; i++) {
        size_t sz = (sec[i].VirtualSize ? sec[i].VirtualSize : sec[i].SizeOfRawData);
        sz = (sz + 0xFFF) & ~0xFFFul;
        if (sz) mprotect(base + sec[i].VirtualAddress, sz, prot_of(sec[i].Characteristics));
    }
    free(f);
    return m;
}

/* ------------------------------------------------------------ TLS (implicit thread-local storage of the image) */
void pe_setup_tls_for_thread(void **tls_array);
void *teb_tls_slot(void);
static DWORD tls_next_index;
static Module *tls_modules[16];

void pe_setup_tls_for_thread(void **tls_array)
{
    for (DWORD i = 0; i < tls_next_index; i++) {
        if (tls_array[i]) continue;
        Module *m = tls_modules[i];
        TlsDir *td = (TlsDir *)(m->base + m->nt->Opt.Dir[9].VirtualAddress);
        size_t raw = (size_t)(td->EndRaw - td->StartRaw), tot = raw + td->SizeOfZeroFill;
        uint8_t *blk = calloc(1, tot ? tot : 1);
        if (raw) memcpy(blk, (void *)td->StartRaw, raw);
        tls_array[i] = blk;
    }
}

static void pe_register_tls(Module *m)
{
    if (!m->nt->Opt.Dir[9].Size || tls_next_index >= 16) return;
    TlsDir *td = (TlsDir *)(m->base + m->nt->Opt.Dir[9].VirtualAddress);
    if (td->AddressOfIndex) *(DWORD *)td->AddressOfIndex = tls_next_index;
    tls_modules[tls_next_index++] = m;
}

int pe_call_entry(Module *m, int reason)
{
    if (!m->loaded && reason == 1) {
        pe_register_tls(m);
        m->loaded = 1;
        pe_setup_tls_for_thread(*(void ***)(void *)teb_tls_slot());
    }
    if (m->nt->Opt.Dir[9].Size && reason == 1) {                                  /* TLS callbacks run before the entry point */
        TlsDir *td = (TlsDir *)(m->base + m->nt->Opt.Dir[9].VirtualAddress);
        if (td->AddressOfCallBacks) for (QWORD *cb = (QWORD *)td->AddressOfCallBacks; *cb; cb++)
            ((void (WINAPI *)(void *, DWORD, void *))*cb)(m->base, 1, NULL);
    }
    if (m->is_dll) return m->entry == (QWORD)m->base ? 1 : ((BOOL (WINAPI *)(void *, DWORD, void *))m->entry)(m->base, (DWORD)reason, NULL);
    return ((int (WINAPI *)(void))m->entry)();
}

Module *pe_module_by_base(void *b) { for (Module *m = modules; m; m = m->next) if ((void *)m->base == b) return m; return NULL; }
