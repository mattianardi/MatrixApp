/*
 * Nio - the NEOS language. A small dynamic language for systems glue: values, closures, pipelines, string
 * interpolation, and first-class access to what makes NEOS different: capability-gated system tools, the
 * Linux/Windows variable bus, path translation between both worlds, and the AI helper.
 *
 *   let procs = sys.threads() |> filter(.prio < 5) |> sort(.name)
 *   for p in procs { println("{p.name} runs at priority {p.prio}") }
 *   println(path.win("/mnt/c/Users/neos"))        // C:\Users\neos
 *   sh("ls /bin | wc -l") |> trim() |> int()
 *
 * Build:  gcc -O2 -static -o nio nio.c -lm        (add -DNIO_NEOS on NEOS to enable the system tool bridge)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#ifdef NIO_NEOS
#include <sys/syscall.h>
#endif

typedef long long i64;

/* ============================================================ values */
typedef struct Val Val;
typedef struct Str Str;
typedef struct List List;
typedef struct Map Map;
typedef struct Fn Fn;
typedef struct Node Node;
typedef struct Env Env;

enum { T_NIL, T_BOOL, T_INT, T_FLOAT, T_STR, T_LIST, T_MAP, T_FN, T_BUILTIN };
typedef Val (*Builtin)(int argc, Val *argv);

struct Val {
    int t;
    union { int b; i64 i; double d; Str *s; List *l; Map *m; Fn *f; Builtin bf; } u;
};
struct Str { size_t n; char p[]; };
struct List { size_t n, cap; Val *v; };
struct Map { size_t n, cap; Str **k; Val *v; };
struct Fn { Node *def; Env *env; const char *name; };

#define NIL ((Val){ T_NIL })
static Val mk_bool(int b) { Val v = { T_BOOL }; v.u.b = b; return v; }
static Val mk_int(i64 i) { Val v = { T_INT }; v.u.i = i; return v; }
static Val mk_float(double d) { Val v = { T_FLOAT }; v.u.d = d; return v; }
static Val mk_bf(Builtin f) { Val v = { T_BUILTIN }; v.u.bf = f; return v; }

static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { fputs("nio: out of memory\n", stderr); exit(70); } return p; }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) { fputs("nio: out of memory\n", stderr); exit(70); } return p; }

static Val mk_strn(const char *s, size_t n) { Str *r = xmalloc(sizeof *r + n + 1); r->n = n; memcpy(r->p, s, n); r->p[n] = 0; Val v = { T_STR }; v.u.s = r; return v; }
static Val mk_str(const char *s) { return mk_strn(s, strlen(s)); }
static Val mk_list(void) { List *l = xmalloc(sizeof *l); l->n = l->cap = 0; l->v = NULL; Val v = { T_LIST }; v.u.l = l; return v; }
static void list_push(List *l, Val x) { if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->v = xrealloc(l->v, l->cap * sizeof(Val)); } l->v[l->n++] = x; }
static Val mk_map(void) { Map *m = xmalloc(sizeof *m); m->n = m->cap = 0; m->k = NULL; m->v = NULL; Val v = { T_MAP }; v.u.m = m; return v; }
static Val *map_find(Map *m, const char *k, size_t kn)
{
    for (size_t i = 0; i < m->n; i++) if (m->k[i]->n == kn && !memcmp(m->k[i]->p, k, kn)) return &m->v[i];
    return NULL;
}
static void map_set(Map *m, const char *k, size_t kn, Val x)
{
    Val *e = map_find(m, k, kn);
    if (e) { *e = x; return; }
    if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 8; m->k = xrealloc(m->k, m->cap * sizeof(Str *)); m->v = xrealloc(m->v, m->cap * sizeof(Val)); }
    m->k[m->n] = mk_strn(k, kn).u.s; m->v[m->n++] = x;
}
static void map_put(Val m, const char *k, Val x) { map_set(m.u.m, k, strlen(k), x); }

/* ============================================================ errors */
static jmp_buf *err_jmp;
static Val err_val;
static char err_msg[512];
static int cur_line;

static void nio_throw(Val v) { if (err_jmp) { err_val = v; longjmp(*err_jmp, 1); } fprintf(stderr, "nio: uncaught error\n"); exit(1); }
static void fail(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char m[400]; vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    snprintf(err_msg, sizeof err_msg, "%s (line %d)", m, cur_line);
    nio_throw(mk_str(err_msg));
}

static const char *tname(int t) { static const char *n[] = { "nil", "bool", "int", "float", "str", "list", "map", "fn", "fn" }; return n[t]; }
static int truthy(Val v) { switch (v.t) { case T_NIL: return 0; case T_BOOL: return v.u.b; case T_INT: return v.u.i != 0; case T_FLOAT: return v.u.d != 0; case T_STR: return v.u.s->n != 0; case T_LIST: return v.u.l->n != 0; case T_MAP: return v.u.m->n != 0; } return 1; }

/* ============================================================ string building */
typedef struct { char *p; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n) { if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = xrealloc(b->p, b->cap); } memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0; }
static void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static void bputc(Buf *b, char c) { bput(b, &c, 1); }

static void show(Buf *b, Val v, int quote);
static void show_float(Buf *b, double d)
{
    char t[40];
    if (d == (double)(i64)d && fabs(d) < 1e15) snprintf(t, sizeof t, "%.1f", d); else snprintf(t, sizeof t, "%.10g", d);
    bputs(b, t);
}
static void show(Buf *b, Val v, int quote)
{
    char t[32];
    switch (v.t) {
    case T_NIL: bputs(b, "nil"); break;
    case T_BOOL: bputs(b, v.u.b ? "true" : "false"); break;
    case T_INT: snprintf(t, sizeof t, "%lld", v.u.i); bputs(b, t); break;
    case T_FLOAT: show_float(b, v.u.d); break;
    case T_STR:
        if (!quote) { bput(b, v.u.s->p, v.u.s->n); break; }
        bputc(b, '"');
        for (size_t i = 0; i < v.u.s->n; i++) { char c = v.u.s->p[i]; if (c == '"' || c == '\\') bputc(b, '\\'); if (c == '\n') bputs(b, "\\n"); else bputc(b, c); }
        bputc(b, '"'); break;
    case T_LIST: bputc(b, '['); for (size_t i = 0; i < v.u.l->n; i++) { if (i) bputs(b, ", "); show(b, v.u.l->v[i], 1); } bputc(b, ']'); break;
    case T_MAP: bputc(b, '{'); for (size_t i = 0; i < v.u.m->n; i++) { if (i) bputs(b, ", "); bput(b, v.u.m->k[i]->p, v.u.m->k[i]->n); bputs(b, ": "); show(b, v.u.m->v[i], 1); } bputc(b, '}'); break;
    case T_FN: bputs(b, "<fn "); bputs(b, v.u.f->name ? v.u.f->name : "lambda"); bputc(b, '>'); break;
    case T_BUILTIN: bputs(b, "<builtin>"); break;
    }
}
static Val to_str(Val v) { if (v.t == T_STR) return v; Buf b = { 0 }; show(&b, v, 0); Val r = mk_strn(b.p ? b.p : "", b.n); free(b.p); return r; }

static int equal(Val a, Val b)
{
    if (a.t != b.t) { if (a.t == T_INT && b.t == T_FLOAT) return (double)a.u.i == b.u.d; if (a.t == T_FLOAT && b.t == T_INT) return a.u.d == (double)b.u.i; return 0; }
    switch (a.t) {
    case T_NIL: return 1;
    case T_BOOL: return a.u.b == b.u.b;
    case T_INT: return a.u.i == b.u.i;
    case T_FLOAT: return a.u.d == b.u.d;
    case T_STR: return a.u.s->n == b.u.s->n && !memcmp(a.u.s->p, b.u.s->p, a.u.s->n);
    case T_LIST: if (a.u.l->n != b.u.l->n) return 0; for (size_t i = 0; i < a.u.l->n; i++) if (!equal(a.u.l->v[i], b.u.l->v[i])) return 0; return 1;
    case T_MAP: if (a.u.m->n != b.u.m->n) return 0; for (size_t i = 0; i < a.u.m->n; i++) { Val *o = map_find(b.u.m, a.u.m->k[i]->p, a.u.m->k[i]->n); if (!o || !equal(a.u.m->v[i], *o)) return 0; } return 1;
    default: return a.u.f == b.u.f;
    }
}
static int compare(Val a, Val b)
{
    if ((a.t == T_INT || a.t == T_FLOAT) && (b.t == T_INT || b.t == T_FLOAT)) {
        double x = a.t == T_INT ? (double)a.u.i : a.u.d, y = b.t == T_INT ? (double)b.u.i : b.u.d;
        if (a.t == T_INT && b.t == T_INT) return a.u.i < b.u.i ? -1 : a.u.i > b.u.i;
        return x < y ? -1 : x > y;
    }
    if (a.t == T_STR && b.t == T_STR) { size_t n = a.u.s->n < b.u.s->n ? a.u.s->n : b.u.s->n; int c = memcmp(a.u.s->p, b.u.s->p, n); return c ? (c < 0 ? -1 : 1) : (a.u.s->n < b.u.s->n ? -1 : a.u.s->n > b.u.s->n); }
    fail("cannot compare %s with %s", tname(a.t), tname(b.t));
    return 0;
}

/* ============================================================ lexer */
enum { K_EOF, K_NL, K_NUM, K_FLT, K_STR, K_RAW, K_ID, K_OP, K_KW };
typedef struct { int k, line; char *s; i64 i; double d; } Tok;
static Tok *toks; static size_t ntok, captok;
static const char *KW[] = { "let", "fn", "if", "else", "while", "for", "in", "return", "break", "continue", "and", "or", "not", "true", "false", "nil", "try", "catch", "throw", NULL };

static void tpush(Tok t) { if (ntok == captok) { captok = captok ? captok * 2 : 256; toks = xrealloc(toks, captok * sizeof(Tok)); } toks[ntok++] = t; }

static void lex(const char *src)
{
    int line = 1;
    ntok = 0;
    for (const char *p = src; *p;) {
        char c = *p;
        if (c == '\n') { tpush((Tok){ K_NL, line++ }); p++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { p++; continue; }
        if (c == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (c == '#' && (p == src || p[-1] == '\n') && p[1] == '!') { while (*p && *p != '\n') p++; continue; }
        if (isdigit((unsigned char)c)) {
            const char *q = p; int flt = 0;
            if (c == '0' && (p[1] == 'x' || p[1] == 'X')) { tpush((Tok){ K_NUM, line, NULL, strtoll(p, (char **)&q, 16) }); p = q; continue; }
            while (isdigit((unsigned char)*q) || *q == '_') q++;
            if (*q == '.' && isdigit((unsigned char)q[1])) { flt = 1; q++; while (isdigit((unsigned char)*q)) q++; }
            if (*q == 'e' || *q == 'E') { const char *r = q + 1; if (*r == '+' || *r == '-') r++; if (isdigit((unsigned char)*r)) { flt = 1; q = r; while (isdigit((unsigned char)*q)) q++; } }
            char tmp[64]; size_t n = 0; for (const char *s = p; s < q && n < 63; s++) if (*s != '_') tmp[n++] = *s; tmp[n] = 0;
            if (flt) tpush((Tok){ K_FLT, line, NULL, 0, atof(tmp) }); else tpush((Tok){ K_NUM, line, NULL, strtoll(tmp, NULL, 10) });
            p = q; continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            const char *q = p; while (isalnum((unsigned char)*q) || *q == '_') q++;
            char *s = strndup(p, (size_t)(q - p)); int kw = 0;
            for (int i = 0; KW[i]; i++) if (!strcmp(KW[i], s)) kw = 1;
            tpush((Tok){ kw ? K_KW : K_ID, line, s }); p = q; continue;
        }
        if (c == '"' || c == '\'') {
            char q = c; Buf b = { 0 }; p++;
            int depth = 0;
            while (*p && (*p != q || depth)) {
                if (*p == '\n') line++;
                if (q == '"' && *p == '{') depth++;
                if (q == '"' && *p == '}' && depth) depth--;
                if (*p == '\\' && p[1]) {
                    p++;
                    switch (*p) { case 'n': bputc(&b, '\n'); break; case 't': bputc(&b, '\t'); break; case 'r': bputc(&b, '\r'); break; case '0': bputc(&b, 0); break;
                                  case '{': bputc(&b, 1); break; case '}': bputc(&b, 2); break; default: bputc(&b, *p); }
                    p++; continue;
                }
                bputc(&b, *p++);
            }
            if (!*p) { fprintf(stderr, "nio: unterminated string at line %d\n", line); exit(2); }
            p++;
            tpush((Tok){ q == '"' ? K_STR : K_RAW, line, b.p ? b.p : strdup("") });
            continue;
        }
        static const char *ops[] = { "|>", "**", "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "..", "->", "=>", NULL };
        int done = 0;
        for (int i = 0; ops[i]; i++) if (!strncmp(p, ops[i], strlen(ops[i]))) { tpush((Tok){ K_OP, line, strdup(ops[i]) }); p += strlen(ops[i]); done = 1; break; }
        if (done) continue;
        if (strchr("+-*/%<>=(){}[],;:.|!?", c)) { char t[2] = { c, 0 }; tpush((Tok){ K_OP, line, strdup(t) }); p++; continue; }
        fprintf(stderr, "nio: unexpected character '%c' at line %d\n", c, line); exit(2);
    }
    tpush((Tok){ K_NL, line });
    tpush((Tok){ K_EOF, line });
}

/* ============================================================ AST + parser */
enum { N_NUM, N_FLT, N_STR, N_INTERP, N_ID, N_LIST, N_MAP, N_UN, N_BIN, N_AND, N_OR, N_CALL, N_INDEX, N_SLICE, N_MEMBER, N_PIPE, N_LAMBDA, N_FIELD, N_RANGE, N_IFX, N_TRYX,
       S_LET, S_ASSIGN, S_OPASSIGN, S_EXPR, S_BLOCK, S_IF, S_WHILE, S_FOR, S_FN, S_RET, S_BRK, S_CONT, S_TRY, S_THROW };
struct Node { int kind, line; char *s; i64 i; double d; Node *a, *b, *c; Node **v; int n; char **names; };

static Node *nn(int kind) { Node *n = xmalloc(sizeof *n); memset(n, 0, sizeof *n); n->kind = kind; return n; }
static size_t pos;
static Tok *T(void) { return &toks[pos]; }
static int is_op(const char *s) { return T()->k == K_OP && !strcmp(T()->s, s); }
static int is_kw(const char *s) { return T()->k == K_KW && !strcmp(T()->s, s); }
static void perr(const char *m) { fprintf(stderr, "nio: syntax error at line %d: %s\n", T()->line, m); exit(2); }
static void skip_nl(void) { while (T()->k == K_NL || is_op(";")) pos++; }
static void expect_op(const char *s) { if (!is_op(s)) { char m[64]; snprintf(m, sizeof m, "expected '%s'", s); perr(m); } pos++; }
static void push_child(Node *p, Node *c) { p->v = xrealloc(p->v, (size_t)(p->n + 1) * sizeof(Node *)); p->v[p->n++] = c; }

static Node *parse_expr(void);
static Node *parse_block(void);
static Node *parse_stmt(void);
static Node *parse_str_interp(const char *s, int line);

static Node *parse_primary(void)
{
    Tok *t = T(); Node *n;
    switch (t->k) {
    case K_NUM: n = nn(N_NUM); n->i = t->i; n->line = t->line; pos++; return n;
    case K_FLT: n = nn(N_FLT); n->d = t->d; n->line = t->line; pos++; return n;
    case K_RAW: n = nn(N_STR); n->s = t->s; n->line = t->line; pos++; return n;
    case K_STR: pos++; return parse_str_interp(t->s, t->line);
    case K_ID: n = nn(N_ID); n->s = t->s; n->line = t->line; pos++; return n;
    case K_KW:
        if (!strcmp(t->s, "true") || !strcmp(t->s, "false")) { n = nn(N_NUM); n->i = t->s[0] == 't'; n->s = strdup("bool"); pos++; return n; }
        if (!strcmp(t->s, "nil")) { n = nn(N_NUM); n->s = strdup("nil"); pos++; return n; }
        if (!strcmp(t->s, "fn")) {                                                          /* anonymous function: fn(a, b) { ... } */
            pos++; n = nn(N_LAMBDA); n->line = t->line; expect_op("(");
            while (!is_op(")")) { if (T()->k != K_ID) perr("parameter name expected"); n->names = xrealloc(n->names, (size_t)(n->n + 1) * sizeof(char *)); n->names[n->n++] = T()->s; pos++; if (is_op(",")) pos++; }
            expect_op(")"); n->a = parse_block(); return n;
        }
        if (!strcmp(t->s, "try")) {                                                         /* try as expression: let v = try { risky() } catch e { fallback } */
            pos++; n = nn(N_TRYX); n->line = t->line; n->a = parse_block(); skip_nl();
            if (!is_kw("catch")) perr("'catch' expected"); pos++;
            if (T()->k == K_ID) { n->s = T()->s; pos++; }
            n->b = parse_block(); return n;
        }
        if (!strcmp(t->s, "if")) {                                                          /* if as expression: if c { a } else { b } */
            pos++; n = nn(N_IFX); n->line = t->line; n->a = parse_expr(); n->b = parse_block();
            skip_nl(); if (is_kw("else")) { pos++; n->c = is_kw("if") ? parse_primary() : parse_block(); }
            return n;
        }
        break;
    case K_OP:
        if (!strcmp(t->s, "(")) { pos++; skip_nl(); n = parse_expr(); skip_nl(); expect_op(")"); return n; }
        if (!strcmp(t->s, "[")) {
            pos++; n = nn(N_LIST); n->line = t->line; skip_nl();
            while (!is_op("]")) { push_child(n, parse_expr()); skip_nl(); if (is_op(",")) { pos++; skip_nl(); } }
            expect_op("]"); return n;
        }
        if (!strcmp(t->s, "{")) {
            pos++; n = nn(N_MAP); n->line = t->line; skip_nl();
            while (!is_op("}")) {
                Node *k;
                if (T()->k == K_ID || T()->k == K_RAW || T()->k == K_STR) { k = nn(N_STR); k->s = T()->s; pos++; } else perr("map key expected");
                if (is_op(":")) { pos++; skip_nl(); push_child(n, k); push_child(n, parse_expr()); }
                else { Node *id = nn(N_ID); id->s = k->s; push_child(n, k); push_child(n, id); }           /* {x} shorthand for {x: x} */
                skip_nl(); if (is_op(",")) { pos++; skip_nl(); }
            }
            expect_op("}"); return n;
        }
        if (!strcmp(t->s, "|")) {                                                           /* lambda: |a, b| expr */
            pos++; n = nn(N_LAMBDA); n->line = t->line;
            while (!is_op("|")) { if (T()->k != K_ID) perr("parameter name expected"); n->names = xrealloc(n->names, (size_t)(n->n + 1) * sizeof(char *)); n->names[n->n++] = T()->s; pos++; if (is_op(",")) pos++; }
            pos++;
            if (is_op("{")) n->a = parse_block(); else { Node *r = nn(S_RET); r->a = parse_expr(); Node *b = nn(S_BLOCK); push_child(b, r); n->a = b; }
            return n;
        }
        if (!strcmp(t->s, ".") && toks[pos + 1].k == K_ID) {                                /* .field  ==  r.field of the implicit parameter */
            pos++; n = nn(N_MEMBER); n->line = t->line;
            Node *id = nn(N_ID); id->s = "r"; id->line = t->line; n->a = id; n->s = T()->s; pos++; return n;
        }
        break;
    }
    perr("unexpected token");
    return NULL;
}

static Node *parse_postfix(void)
{
    Node *e = parse_primary();
    for (;;) {
        if (is_op("(")) {
            Node *c = nn(N_CALL); c->line = T()->line; c->a = e; pos++; skip_nl();
            while (!is_op(")")) {
                if (is_op(".") && toks[pos + 1].k == K_ID) {                                 /* sort(.name), filter(.size > 10): the argument is a function of r */
                    Node *lam = nn(N_LAMBDA); lam->line = T()->line; lam->n = 1; lam->names = xmalloc(sizeof(char *)); lam->names[0] = "r";
                    Node *ret = nn(S_RET); ret->a = parse_expr(); Node *blk = nn(S_BLOCK); push_child(blk, ret); lam->a = blk;
                    push_child(c, lam);
                } else push_child(c, parse_expr());
                skip_nl(); if (is_op(",")) { pos++; skip_nl(); }
            }
            expect_op(")"); e = c;
        } else if (is_op("[")) {
            Node *x = nn(N_INDEX); x->line = T()->line; pos++; x->a = e;
            if (is_op(":")) { pos++; x->kind = N_SLICE; x->b = NULL; x->c = is_op("]") ? NULL : parse_expr(); }
            else { x->b = parse_expr(); if (is_op(":")) { pos++; x->kind = N_SLICE; x->c = is_op("]") ? NULL : parse_expr(); } }
            expect_op("]"); e = x;
        } else if (is_op(".") && toks[pos + 1].k == K_ID) {
            Node *m = nn(N_MEMBER); m->line = T()->line; pos++; m->a = e; m->s = T()->s; pos++; e = m;
        } else break;
    }
    return e;
}

static Node *parse_unary(void)
{
    if (is_op("-")) { Node *n = nn(N_UN); n->line = T()->line; n->s = "-"; pos++; n->a = parse_unary(); return n; }
    if (is_kw("not") || is_op("!")) { Node *n = nn(N_UN); n->line = T()->line; n->s = "not"; pos++; n->a = parse_unary(); return n; }
    Node *b = parse_postfix();
    if (is_op("**")) { Node *n = nn(N_BIN); n->line = T()->line; n->s = "**"; pos++; n->a = b; n->b = parse_unary(); return n; }
    return b;
}

static Node *bin_level(Node *(*sub)(void), const char **ops)
{
    Node *l = sub();
    for (;;) {
        int hit = 0;
        for (int i = 0; ops[i]; i++) if (is_op(ops[i])) {
            Node *n = nn(N_BIN); n->line = T()->line; n->s = (char *)ops[i]; pos++; skip_nl(); n->a = l; n->b = sub(); l = n; hit = 1; break;
        }
        if (!hit) return l;
    }
}
static Node *p_mul(void) { static const char *o[] = { "*", "/", "%", NULL }; return bin_level(parse_unary, o); }
static Node *p_add(void) { static const char *o[] = { "+", "-", NULL }; return bin_level(p_mul, o); }
static Node *p_range(void)
{
    Node *l = p_add();
    if (is_op("..")) { Node *n = nn(N_RANGE); n->line = T()->line; pos++; n->a = l; n->b = p_add(); return n; }
    return l;
}
static Node *p_cmp(void)
{
    Node *l = p_range();
    for (;;) {
        static const char *o[] = { "==", "!=", "<=", ">=", "<", ">", NULL };
        int hit = 0;
        for (int i = 0; o[i]; i++) if (is_op(o[i])) { Node *n = nn(N_BIN); n->line = T()->line; n->s = (char *)o[i]; pos++; n->a = l; n->b = p_range(); l = n; hit = 1; break; }
        if (!hit && is_kw("in")) { Node *n = nn(N_BIN); n->line = T()->line; n->s = "in"; pos++; n->a = l; n->b = p_range(); l = n; hit = 1; }
        if (!hit) return l;
    }
}
static Node *p_and(void) { Node *l = p_cmp(); while (is_kw("and")) { Node *n = nn(N_AND); pos++; skip_nl(); n->a = l; n->b = p_cmp(); l = n; } return l; }
static Node *p_or(void) { Node *l = p_and(); while (is_kw("or")) { Node *n = nn(N_OR); pos++; skip_nl(); n->a = l; n->b = p_and(); l = n; } return l; }

static size_t after_nl(void) { size_t q = pos; while (toks[q].k == K_NL) q++; return q; }
static Node *parse_expr(void)
{
    Node *l = p_or();
    for (;;) {                                                                              /* pipelines may continue on the next line */
        size_t q = after_nl();
        if (!(toks[q].k == K_OP && !strcmp(toks[q].s, "|>"))) return l;
        pos = q + 1; skip_nl();
        Node *n = nn(N_PIPE); n->line = T()->line; n->a = l;
        Node *r = p_or();
        n->b = r; l = n;
    }
}

static Node *parse_block(void)
{
    skip_nl();
    expect_op("{");
    Node *b = nn(S_BLOCK);
    skip_nl();
    while (!is_op("}")) { if (T()->k == K_EOF) perr("unterminated block"); push_child(b, parse_stmt()); skip_nl(); }
    pos++;
    return b;
}

static void end_stmt(void) { if (T()->k == K_NL || is_op(";")) { pos++; return; } if (is_op("}") || T()->k == K_EOF) return; perr("end of statement expected"); }

static Node *parse_stmt(void)
{
    Tok *t = T(); Node *n;
    if (t->k == K_KW) {
        if (!strcmp(t->s, "let")) {
            pos++; n = nn(S_LET); n->line = t->line;
            if (T()->k != K_ID) perr("name expected after let");
            n->s = T()->s; pos++; expect_op("="); skip_nl(); n->a = parse_expr(); end_stmt(); return n;
        }
        if (!strcmp(t->s, "fn") && toks[pos + 1].k == K_ID) {
            pos++; n = nn(S_FN); n->line = t->line; n->s = T()->s; pos++; expect_op("(");
            while (!is_op(")")) { if (T()->k != K_ID) perr("parameter name expected"); n->names = xrealloc(n->names, (size_t)(n->n + 1) * sizeof(char *)); n->names[n->n++] = T()->s; pos++; if (is_op(",")) pos++; }
            expect_op(")"); n->a = parse_block(); return n;
        }
        if (!strcmp(t->s, "if")) {
            pos++; n = nn(S_IF); n->line = t->line; n->a = parse_expr(); n->b = parse_block();
            size_t q = after_nl();
            if (toks[q].k == K_KW && !strcmp(toks[q].s, "else")) { pos = q + 1; skip_nl(); n->c = is_kw("if") ? parse_stmt() : parse_block(); }
            return n;
        }
        if (!strcmp(t->s, "while")) { pos++; n = nn(S_WHILE); n->line = t->line; n->a = parse_expr(); n->b = parse_block(); return n; }
        if (!strcmp(t->s, "for")) {
            pos++; n = nn(S_FOR); n->line = t->line;
            if (T()->k != K_ID) perr("loop variable expected");
            n->s = T()->s; pos++;
            if (is_op(",")) { pos++; n->names = xmalloc(sizeof(char *)); n->names[0] = T()->s; n->n = 1; pos++; }       /* for i, x in list */
            if (!is_kw("in")) perr("'in' expected"); pos++;
            n->a = parse_expr(); n->b = parse_block(); return n;
        }
        if (!strcmp(t->s, "return")) { pos++; n = nn(S_RET); n->line = t->line; if (T()->k != K_NL && !is_op("}") && !is_op(";")) n->a = parse_expr(); end_stmt(); return n; }
        if (!strcmp(t->s, "break")) { pos++; n = nn(S_BRK); end_stmt(); return n; }
        if (!strcmp(t->s, "continue")) { pos++; n = nn(S_CONT); end_stmt(); return n; }
        if (!strcmp(t->s, "throw")) { pos++; n = nn(S_THROW); n->line = t->line; n->a = parse_expr(); end_stmt(); return n; }
        if (!strcmp(t->s, "try")) {
            pos++; n = nn(S_TRY); n->line = t->line; n->a = parse_block(); skip_nl();
            if (!is_kw("catch")) perr("'catch' expected"); pos++;
            if (T()->k == K_ID) { n->s = T()->s; pos++; }
            n->b = parse_block(); return n;
        }
    }
    Node *e = parse_expr();
    if (is_op("=") || is_op("+=") || is_op("-=") || is_op("*=") || is_op("/=")) {
        const char *op = T()->s; int line = T()->line; pos++; skip_nl();
        n = nn(op[0] == '=' ? S_ASSIGN : S_OPASSIGN); n->line = line; n->s = (char *)op; n->a = e; n->b = parse_expr(); end_stmt(); return n;
    }
    n = nn(S_EXPR); n->line = e->line; n->a = e; end_stmt(); return n;
}

static Node *parse_str_interp(const char *s, int line)
{
    Node *parts = nn(N_INTERP); parts->line = line;
    Buf lit = { 0 };
    for (const char *p = s; *p;) {
        if (*p == 1) { bputc(&lit, '{'); p++; continue; }
        if (*p == 2) { bputc(&lit, '}'); p++; continue; }
        if (*p == '{') {
            int depth = 1; const char *q = p + 1;
            while (*q && depth) { if (*q == '{') depth++; else if (*q == '}') depth--; q++; }
            if (depth) { fprintf(stderr, "nio: unterminated {..} in string at line %d\n", line); exit(2); }
            char *src = strndup(p + 1, (size_t)(q - p - 2));
            if (lit.n) { Node *l = nn(N_STR); l->s = strndup(lit.p, lit.n); push_child(parts, l); lit.n = 0; if (lit.p) lit.p[0] = 0; }
            Tok *save = toks; size_t sn = ntok, sc = captok, sp = pos;                       /* sub-parse the embedded expression */
            toks = NULL; ntok = captok = 0; lex(src); pos = 0;
            for (size_t i = 0; i < ntok; i++) toks[i].line = line;
            Node *e = parse_expr();
            if (T()->k != K_NL && T()->k != K_EOF) perr("bad expression inside string");
            toks = save; ntok = sn; captok = sc; pos = sp;
            push_child(parts, e); free(src); p = q; continue;
        }
        bputc(&lit, *p++);
    }
    if (parts->n == 0) { Node *l = nn(N_STR); l->s = lit.p ? lit.p : strdup(""); return l; }
    if (lit.n) { Node *l = nn(N_STR); l->s = strndup(lit.p, lit.n); push_child(parts, l); }
    return parts;
}

/* ============================================================ environments + interpreter */
struct Env { Env *up; size_t n, cap; Str **k; Val *v; };
static Env *new_env(Env *up) { Env *e = xmalloc(sizeof *e); e->up = up; e->n = e->cap = 0; e->k = NULL; e->v = NULL; return e; }
static Val *env_find(Env *e, const char *k)
{
    size_t kn = strlen(k);
    for (; e; e = e->up) for (size_t i = e->n; i-- > 0;) if (e->k[i]->n == kn && !memcmp(e->k[i]->p, k, kn)) return &e->v[i];
    return NULL;
}
static void env_def(Env *e, const char *k, Val v)
{
    size_t kn = strlen(k);
    for (size_t i = 0; i < e->n; i++) if (e->k[i]->n == kn && !memcmp(e->k[i]->p, k, kn)) { e->v[i] = v; return; }
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 8; e->k = xrealloc(e->k, e->cap * sizeof(Str *)); e->v = xrealloc(e->v, e->cap * sizeof(Val)); }
    e->k[e->n] = mk_str(k).u.s; e->v[e->n++] = v;
}

static Env *globals;
enum { FL_OK, FL_BREAK, FL_CONT, FL_RET };
static Val ret_val, last_expr;

static Val eval(Node *n, Env *e);
static int exec(Node *n, Env *e);
static Val eval_block(Node *b, Env *e);

static Val call_fn(Val f, int argc, Val *argv)
{
    static int depth;
    if (f.t == T_BUILTIN) return f.u.bf(argc, argv);
    if (f.t != T_FN) fail("%s is not callable", tname(f.t));
    Node *d = f.u.f->def;
    if (argc != d->n) fail("%s expects %d argument(s), got %d", f.u.f->name ? f.u.f->name : "function", d->n, argc);
    if (++depth > 3000) { depth = 0; fail("recursion too deep"); }
    Env *ne = new_env(f.u.f->env);
    for (int i = 0; i < argc; i++) env_def(ne, d->names[i], argv[i]);
    ret_val = NIL;
    int r = exec(d->a, ne);
    depth--;
    Val out = r == FL_RET ? ret_val : NIL;
    ret_val = NIL;
    return out;
}

static Val arith(const char *op, Val a, Val b)
{
    if (op[0] == '+' && a.t == T_STR && b.t == T_STR) { Str *r = xmalloc(sizeof *r + a.u.s->n + b.u.s->n + 1); r->n = a.u.s->n + b.u.s->n; memcpy(r->p, a.u.s->p, a.u.s->n); memcpy(r->p + a.u.s->n, b.u.s->p, b.u.s->n); r->p[r->n] = 0; Val v = { T_STR }; v.u.s = r; return v; }
    if (op[0] == '+' && a.t == T_STR) return arith(op, a, to_str(b));
    if (op[0] == '+' && b.t == T_STR) return arith(op, to_str(a), b);
    if (op[0] == '+' && a.t == T_LIST && b.t == T_LIST) { Val r = mk_list(); for (size_t i = 0; i < a.u.l->n; i++) list_push(r.u.l, a.u.l->v[i]); for (size_t i = 0; i < b.u.l->n; i++) list_push(r.u.l, b.u.l->v[i]); return r; }
    if (op[0] == '*' && op[1] == 0 && a.t == T_STR && b.t == T_INT) { Buf bf = { 0 }; for (i64 i = 0; i < b.u.i; i++) bput(&bf, a.u.s->p, a.u.s->n); Val r = mk_strn(bf.p ? bf.p : "", bf.n); free(bf.p); return r; }
    if ((a.t != T_INT && a.t != T_FLOAT) || (b.t != T_INT && b.t != T_FLOAT)) fail("cannot apply '%s' to %s and %s", op, tname(a.t), tname(b.t));
    if (a.t == T_INT && b.t == T_INT) {
        i64 x = a.u.i, y = b.u.i;
        switch (op[0]) {
        case '+': return mk_int(x + y);
        case '-': return mk_int(x - y);
        case '*': if (op[1] == '*') { if (y < 0) return mk_float(pow((double)x, (double)y)); i64 r = 1; while (y--) r *= x; return mk_int(r); } return mk_int(x * y);
        case '/': if (!y) fail("division by zero"); return x % y ? mk_float((double)x / (double)y) : mk_int(x / y);
        case '%': if (!y) fail("division by zero"); return mk_int(((x % y) + y) % y);
        }
    }
    double x = a.t == T_INT ? (double)a.u.i : a.u.d, y = b.t == T_INT ? (double)b.u.i : b.u.d;
    switch (op[0]) {
    case '+': return mk_float(x + y);
    case '-': return mk_float(x - y);
    case '*': return op[1] == '*' ? mk_float(pow(x, y)) : mk_float(x * y);
    case '/': return mk_float(x / y);
    case '%': return mk_float(fmod(x, y));
    }
    return NIL;
}

static Val member_get(Val o, const char *k)
{
    if (o.t == T_MAP) { Val *v = map_find(o.u.m, k, strlen(k)); return v ? *v : NIL; }
    if (o.t == T_LIST && !strcmp(k, "len")) return mk_int((i64)o.u.l->n);
    if (o.t == T_STR && !strcmp(k, "len")) return mk_int((i64)o.u.s->n);
    if (o.t == T_NIL) fail("cannot read '%s' of nil", k);
    fail("%s has no field '%s'", tname(o.t), k);
    return NIL;
}

static Val index_get(Val o, Val i)
{
    if (o.t == T_LIST) { if (i.t != T_INT) fail("list index must be int"); i64 k = i.u.i < 0 ? i.u.i + (i64)o.u.l->n : i.u.i; if (k < 0 || k >= (i64)o.u.l->n) fail("list index %lld out of range", i.u.i); return o.u.l->v[k]; }
    if (o.t == T_STR) { if (i.t != T_INT) fail("string index must be int"); i64 k = i.u.i < 0 ? i.u.i + (i64)o.u.s->n : i.u.i; if (k < 0 || k >= (i64)o.u.s->n) fail("string index %lld out of range", i.u.i); return mk_strn(o.u.s->p + k, 1); }
    if (o.t == T_MAP) { Val s = to_str(i); Val *v = map_find(o.u.m, s.u.s->p, s.u.s->n); return v ? *v : NIL; }
    fail("cannot index %s", tname(o.t));
    return NIL;
}

static Val make_closure(Node *def, Env *e, const char *name) { Fn *f = xmalloc(sizeof *f); f->def = def; f->env = e; f->name = name; Val v = { T_FN }; v.u.f = f; return v; }

static Val field_fn(const char *field)
{
    static Node tmpl; (void)tmpl;
    Node *d = nn(N_LAMBDA); d->n = 1; d->names = xmalloc(sizeof(char *)); d->names[0] = "r";
    Node *ret = nn(S_RET); Node *m = nn(N_MEMBER); Node *id = nn(N_ID); id->s = "r"; m->a = id; m->s = (char *)field; ret->a = m;
    Node *b = nn(S_BLOCK); push_child(b, ret); d->a = b;
    return make_closure(d, globals, "field");
}

static Val eval(Node *n, Env *e)
{
    if (n->line) cur_line = n->line;
    switch (n->kind) {
    case N_NUM: if (n->s) { if (!strcmp(n->s, "bool")) return mk_bool((int)n->i); return NIL; } return mk_int(n->i);
    case N_FLT: return mk_float(n->d);
    case N_STR: return mk_str(n->s);
    case N_INTERP: { Buf b = { 0 }; for (int i = 0; i < n->n; i++) { Val v = eval(n->v[i], e); show(&b, v, 0); } Val r = mk_strn(b.p ? b.p : "", b.n); free(b.p); return r; }
    case N_ID: { Val *v = env_find(e, n->s); if (!v) fail("undefined name '%s'", n->s); return *v; }
    case N_LIST: { Val l = mk_list(); for (int i = 0; i < n->n; i++) list_push(l.u.l, eval(n->v[i], e)); return l; }
    case N_MAP: { Val m = mk_map(); for (int i = 0; i + 1 < n->n; i += 2) map_set(m.u.m, n->v[i]->s, strlen(n->v[i]->s), eval(n->v[i + 1], e)); return m; }
    case N_UN: { Val a = eval(n->a, e); if (n->s[0] == 'n') return mk_bool(!truthy(a)); if (a.t == T_INT) return mk_int(-a.u.i); if (a.t == T_FLOAT) return mk_float(-a.u.d); fail("cannot negate %s", tname(a.t)); }
    case N_AND: { Val a = eval(n->a, e); return truthy(a) ? eval(n->b, e) : a; }
    case N_OR: { Val a = eval(n->a, e); return truthy(a) ? a : eval(n->b, e); }
    case N_BIN: {
        Val a = eval(n->a, e), b = eval(n->b, e); const char *op = n->s;
        if (!strcmp(op, "==")) return mk_bool(equal(a, b));
        if (!strcmp(op, "!=")) return mk_bool(!equal(a, b));
        if (!strcmp(op, "<")) return mk_bool(compare(a, b) < 0);
        if (!strcmp(op, ">")) return mk_bool(compare(a, b) > 0);
        if (!strcmp(op, "<=")) return mk_bool(compare(a, b) <= 0);
        if (!strcmp(op, ">=")) return mk_bool(compare(a, b) >= 0);
        if (!strcmp(op, "in")) {
            if (b.t == T_LIST) { for (size_t i = 0; i < b.u.l->n; i++) if (equal(a, b.u.l->v[i])) return mk_bool(1); return mk_bool(0); }
            if (b.t == T_STR && a.t == T_STR) return mk_bool(memmem(b.u.s->p, b.u.s->n, a.u.s->p, a.u.s->n) != NULL);
            if (b.t == T_MAP) { Val k = to_str(a); return mk_bool(map_find(b.u.m, k.u.s->p, k.u.s->n) != NULL); }
            fail("'in' needs a list, string or map");
        }
        return arith(op, a, b);
    }
    case N_RANGE: { Val a = eval(n->a, e), b = eval(n->b, e); if (a.t != T_INT || b.t != T_INT) fail("range bounds must be int"); Val l = mk_list(); for (i64 i = a.u.i; i < b.u.i; i++) list_push(l.u.l, mk_int(i)); return l; }
    case N_CALL: {
        Val f = eval(n->a, e); int argc = n->n; Val *argv = xmalloc((size_t)(argc + 1) * sizeof(Val));
        for (int i = 0; i < argc; i++) argv[i] = eval(n->v[i], e);
        cur_line = n->line;
        Val r = call_fn(f, argc, argv); free(argv); return r;
    }
    case N_PIPE: {                                                                          /* a |> f(x)  ==  f(a, x) ;  a |> f  ==  f(a) */
        Val left = eval(n->a, e); Node *r = n->b;
        if (r->kind == N_CALL) {
            Val f = eval(r->a, e); int argc = r->n + 1; Val *argv = xmalloc((size_t)(argc + 1) * sizeof(Val));
            argv[0] = left; for (int i = 0; i < r->n; i++) argv[i + 1] = eval(r->v[i], e);
            cur_line = r->line; Val out = call_fn(f, argc, argv); free(argv); return out;
        }
        Val f = eval(r, e); cur_line = n->line; return call_fn(f, 1, &left);
    }
    case N_INDEX: { Val o = eval(n->a, e), i = eval(n->b, e); return index_get(o, i); }
    case N_SLICE: {
        Val o = eval(n->a, e); i64 len = o.t == T_LIST ? (i64)o.u.l->n : o.t == T_STR ? (i64)o.u.s->n : -1;
        if (len < 0) fail("cannot slice %s", tname(o.t));
        i64 lo = n->b ? eval(n->b, e).u.i : 0, hi = n->c ? eval(n->c, e).u.i : len;
        if (lo < 0) lo += len; if (hi < 0) hi += len; if (lo < 0) lo = 0; if (hi > len) hi = len; if (hi < lo) hi = lo;
        if (o.t == T_STR) return mk_strn(o.u.s->p + lo, (size_t)(hi - lo));
        Val l = mk_list(); for (i64 i = lo; i < hi; i++) list_push(l.u.l, o.u.l->v[i]); return l;
    }
    case N_MEMBER: return member_get(eval(n->a, e), n->s);
    case N_FIELD: return field_fn(n->s);
    case N_LAMBDA: return make_closure(n, e, NULL);
    case N_IFX: {
        if (truthy(eval(n->a, e))) return eval_block(n->b, e);
        if (n->c) return n->c->kind == S_BLOCK ? eval_block(n->c, e) : eval(n->c, e);
        return NIL;
    }
    case N_TRYX: {
        jmp_buf jb, *saved = err_jmp; err_jmp = &jb;
        if (!setjmp(jb)) { Val v = eval_block(n->a, e); err_jmp = saved; return v; }
        err_jmp = saved;
        Env *ce = new_env(e); if (n->s) env_def(ce, n->s, err_val);
        return eval_block(n->b, ce);
    }
    }
    fail("cannot evaluate node %d", n->kind);
    return NIL;
}

static Val eval_block(Node *b, Env *e)                                                      /* a block's value is its last expression statement */
{
    last_expr = NIL;
    int r = exec(b, e);
    Val v = r == FL_RET ? ret_val : last_expr;
    last_expr = NIL;
    return v;
}

static void assign_to(Node *target, Val v, Env *e)
{
    if (target->kind == N_ID) { Val *slot = env_find(e, target->s); if (!slot) fail("assignment to undefined name '%s' (use let)", target->s); *slot = v; return; }
    if (target->kind == N_INDEX) {
        Val o = eval(target->a, e), i = eval(target->b, e);
        if (o.t == T_LIST) { i64 k = i.u.i < 0 ? i.u.i + (i64)o.u.l->n : i.u.i; if (i.t != T_INT || k < 0 || k >= (i64)o.u.l->n) fail("list index out of range"); o.u.l->v[k] = v; return; }
        if (o.t == T_MAP) { Val s = to_str(i); map_set(o.u.m, s.u.s->p, s.u.s->n, v); return; }
        fail("cannot assign into %s", tname(o.t));
    }
    if (target->kind == N_MEMBER) { Val o = eval(target->a, e); if (o.t != T_MAP) fail("cannot set a field of %s", tname(o.t)); map_set(o.u.m, target->s, strlen(target->s), v); return; }
    fail("invalid assignment target");
}

static int exec(Node *n, Env *e)
{
    if (n->line) cur_line = n->line;
    switch (n->kind) {
    case S_BLOCK: {
        Env *be = new_env(e);
        for (int i = 0; i < n->n; i++) { int r = exec(n->v[i], be); if (r != FL_OK) return r; }
        return FL_OK;
    }
    case S_LET: env_def(e, n->s, eval(n->a, e)); return FL_OK;
    case S_FN: env_def(e, n->s, make_closure(n, e, n->s)); return FL_OK;
    case S_ASSIGN: assign_to(n->a, eval(n->b, e), e); return FL_OK;
    case S_OPASSIGN: { Val cur = eval(n->a, e), rhs = eval(n->b, e); char op[2] = { n->s[0], 0 }; assign_to(n->a, arith(op, cur, rhs), e); return FL_OK; }
    case S_EXPR: last_expr = eval(n->a, e); return FL_OK;
    case S_IF: if (truthy(eval(n->a, e))) return exec(n->b, e); if (n->c) return exec(n->c, e); return FL_OK;
    case S_WHILE: while (truthy(eval(n->a, e))) { int r = exec(n->b, e); if (r == FL_BREAK) break; if (r == FL_RET) return r; } return FL_OK;
    case S_FOR: {
        Val it = eval(n->a, e);
        Env *le = new_env(e);
        if (it.t == T_MAP) { Val ks = mk_list(); for (size_t i = 0; i < it.u.m->n; i++) list_push(ks.u.l, mk_strn(it.u.m->k[i]->p, it.u.m->k[i]->n)); it = ks; }
        if (it.t == T_STR) { Val cs = mk_list(); for (size_t i = 0; i < it.u.s->n; i++) list_push(cs.u.l, mk_strn(it.u.s->p + i, 1)); it = cs; }
        if (it.t != T_LIST) fail("cannot iterate over %s", tname(it.t));
        for (size_t i = 0; i < it.u.l->n; i++) {
            if (n->n) { env_def(le, n->s, mk_int((i64)i)); env_def(le, n->names[0], it.u.l->v[i]); } else env_def(le, n->s, it.u.l->v[i]);
            int r = exec(n->b, le); if (r == FL_BREAK) break; if (r == FL_RET) return r;
        }
        return FL_OK;
    }
    case S_RET: ret_val = n->a ? eval(n->a, e) : NIL; return FL_RET;
    case S_BRK: return FL_BREAK;
    case S_CONT: return FL_CONT;
    case S_THROW: nio_throw(eval(n->a, e)); return FL_OK;
    case S_TRY: {
        jmp_buf jb, *saved = err_jmp; err_jmp = &jb;
        int r = FL_OK;
        if (!setjmp(jb)) { r = exec(n->a, e); err_jmp = saved; return r; }
        err_jmp = saved;
        Env *ce = new_env(e); if (n->s) env_def(ce, n->s, err_val);
        return exec(n->b, ce);
    }
    }
    fail("cannot execute node %d", n->kind);
    return FL_OK;
}

/* ============================================================ builtins */
#define ARGS(n, name) do { if (argc != (n)) fail(name " expects %d argument(s)", (n)); } while (0)
#define NEEDSTR(v, name) do { if ((v).t != T_STR) fail(name " needs a string, got %s", tname((v).t)); } while (0)

static Val b_print(int argc, Val *argv) { Buf b = { 0 }; for (int i = 0; i < argc; i++) { if (i) bputc(&b, ' '); show(&b, argv[i], 0); } fwrite(b.p ? b.p : "", 1, b.n, stdout); free(b.p); return NIL; }
static Val b_println(int argc, Val *argv) { b_print(argc, argv); putchar('\n'); fflush(stdout); return NIL; }
static Val b_len(int argc, Val *argv) { ARGS(1, "len"); Val v = argv[0]; if (v.t == T_STR) return mk_int((i64)v.u.s->n); if (v.t == T_LIST) return mk_int((i64)v.u.l->n); if (v.t == T_MAP) return mk_int((i64)v.u.m->n); fail("len of %s", tname(v.t)); return NIL; }
static Val b_str(int argc, Val *argv) { ARGS(1, "str"); return to_str(argv[0]); }
static Val b_int(int argc, Val *argv)
{
    ARGS(1, "int"); Val v = argv[0];
    if (v.t == T_INT) return v; if (v.t == T_FLOAT) return mk_int((i64)v.u.d); if (v.t == T_BOOL) return mk_int(v.u.b);
    if (v.t == T_STR) { char *end; i64 r = strtoll(v.u.s->p, &end, 0); while (*end == ' ' || *end == '\n') end++; if (end == v.u.s->p || *end) fail("cannot convert \"%s\" to int", v.u.s->p); return mk_int(r); }
    fail("cannot convert %s to int", tname(v.t)); return NIL;
}
static Val b_float(int argc, Val *argv)
{
    ARGS(1, "float"); Val v = argv[0];
    if (v.t == T_FLOAT) return v; if (v.t == T_INT) return mk_float((double)v.u.i);
    if (v.t == T_STR) { char *end; double r = strtod(v.u.s->p, &end); if (end == v.u.s->p) fail("cannot convert \"%s\" to float", v.u.s->p); return mk_float(r); }
    fail("cannot convert %s to float", tname(v.t)); return NIL;
}
static Val b_type(int argc, Val *argv) { ARGS(1, "type"); return mk_str(tname(argv[0].t)); }
static Val b_range(int argc, Val *argv)
{
    i64 a = 0, b, s = 1;
    if (argc == 1) b = argv[0].u.i; else if (argc >= 2) { a = argv[0].u.i; b = argv[1].u.i; if (argc == 3) s = argv[2].u.i; } else { fail("range expects 1-3 arguments"); return NIL; }
    if (!s) fail("range step is zero");
    Val l = mk_list(); for (i64 i = a; s > 0 ? i < b : i > b; i += s) list_push(l.u.l, mk_int(i)); return l;
}
static Val b_push(int argc, Val *argv) { ARGS(2, "push"); if (argv[0].t != T_LIST) fail("push needs a list"); list_push(argv[0].u.l, argv[1]); return argv[0]; }
static Val b_pop(int argc, Val *argv) { ARGS(1, "pop"); if (argv[0].t != T_LIST || !argv[0].u.l->n) fail("pop needs a non-empty list"); return argv[0].u.l->v[--argv[0].u.l->n]; }
static Val b_keys(int argc, Val *argv) { ARGS(1, "keys"); if (argv[0].t != T_MAP) fail("keys needs a map"); Val l = mk_list(); for (size_t i = 0; i < argv[0].u.m->n; i++) list_push(l.u.l, mk_strn(argv[0].u.m->k[i]->p, argv[0].u.m->k[i]->n)); return l; }
static Val b_values(int argc, Val *argv) { ARGS(1, "values"); if (argv[0].t != T_MAP) fail("values needs a map"); Val l = mk_list(); for (size_t i = 0; i < argv[0].u.m->n; i++) list_push(l.u.l, argv[0].u.m->v[i]); return l; }
static Val b_has(int argc, Val *argv) { ARGS(2, "has"); if (argv[0].t != T_MAP) fail("has needs a map"); Val k = to_str(argv[1]); return mk_bool(map_find(argv[0].u.m, k.u.s->p, k.u.s->n) != NULL); }
static Val b_join(int argc, Val *argv)
{
    if (argc < 1 || argv[0].t != T_LIST) fail("join(list, sep)");
    const char *sep = argc > 1 && argv[1].t == T_STR ? argv[1].u.s->p : "";
    Buf b = { 0 }; for (size_t i = 0; i < argv[0].u.l->n; i++) { if (i) bputs(&b, sep); show(&b, argv[0].u.l->v[i], 0); }
    Val r = mk_strn(b.p ? b.p : "", b.n); free(b.p); return r;
}
static Val b_split(int argc, Val *argv)
{
    if (argc < 1) fail("split(str, sep)"); NEEDSTR(argv[0], "split");
    Val l = mk_list(); Str *s = argv[0].u.s;
    if (argc < 2 || argv[1].u.s->n == 0) {                                                  /* whitespace */
        for (size_t i = 0; i < s->n;) { while (i < s->n && isspace((unsigned char)s->p[i])) i++; size_t j = i; while (j < s->n && !isspace((unsigned char)s->p[j])) j++; if (j > i) list_push(l.u.l, mk_strn(s->p + i, j - i)); i = j; }
        return l;
    }
    NEEDSTR(argv[1], "split"); Str *sep = argv[1].u.s; size_t start = 0;
    for (size_t i = 0; i + sep->n <= s->n;) { if (!memcmp(s->p + i, sep->p, sep->n)) { list_push(l.u.l, mk_strn(s->p + start, i - start)); i += sep->n; start = i; } else i++; }
    list_push(l.u.l, mk_strn(s->p + start, s->n - start)); return l;
}
static Val b_replace(int argc, Val *argv)
{
    ARGS(3, "replace"); NEEDSTR(argv[0], "replace"); NEEDSTR(argv[1], "replace"); NEEDSTR(argv[2], "replace");
    Str *s = argv[0].u.s, *a = argv[1].u.s, *b = argv[2].u.s; if (!a->n) return argv[0];
    Buf o = { 0 }; for (size_t i = 0; i < s->n;) { if (i + a->n <= s->n && !memcmp(s->p + i, a->p, a->n)) { bput(&o, b->p, b->n); i += a->n; } else bputc(&o, s->p[i++]); }
    Val r = mk_strn(o.p ? o.p : "", o.n); free(o.p); return r;
}
static Val b_upper(int argc, Val *argv) { ARGS(1, "upper"); NEEDSTR(argv[0], "upper"); Val r = mk_strn(argv[0].u.s->p, argv[0].u.s->n); for (size_t i = 0; i < r.u.s->n; i++) r.u.s->p[i] = (char)toupper((unsigned char)r.u.s->p[i]); return r; }
static Val b_lower(int argc, Val *argv) { ARGS(1, "lower"); NEEDSTR(argv[0], "lower"); Val r = mk_strn(argv[0].u.s->p, argv[0].u.s->n); for (size_t i = 0; i < r.u.s->n; i++) r.u.s->p[i] = (char)tolower((unsigned char)r.u.s->p[i]); return r; }
static Val b_trim(int argc, Val *argv) { ARGS(1, "trim"); NEEDSTR(argv[0], "trim"); Str *s = argv[0].u.s; size_t a = 0, b = s->n; while (a < b && isspace((unsigned char)s->p[a])) a++; while (b > a && isspace((unsigned char)s->p[b - 1])) b--; return mk_strn(s->p + a, b - a); }
static Val b_starts(int argc, Val *argv) { ARGS(2, "starts"); NEEDSTR(argv[0], "starts"); NEEDSTR(argv[1], "starts"); return mk_bool(argv[0].u.s->n >= argv[1].u.s->n && !memcmp(argv[0].u.s->p, argv[1].u.s->p, argv[1].u.s->n)); }
static Val b_ends(int argc, Val *argv) { ARGS(2, "ends"); NEEDSTR(argv[0], "ends"); NEEDSTR(argv[1], "ends"); size_t a = argv[0].u.s->n, b = argv[1].u.s->n; return mk_bool(a >= b && !memcmp(argv[0].u.s->p + a - b, argv[1].u.s->p, b)); }
static Val b_find(int argc, Val *argv) { ARGS(2, "find"); NEEDSTR(argv[0], "find"); NEEDSTR(argv[1], "find"); char *p = memmem(argv[0].u.s->p, argv[0].u.s->n, argv[1].u.s->p, argv[1].u.s->n); return mk_int(p ? p - argv[0].u.s->p : -1); }
static Val b_abs(int argc, Val *argv) { ARGS(1, "abs"); if (argv[0].t == T_INT) return mk_int(argv[0].u.i < 0 ? -argv[0].u.i : argv[0].u.i); return mk_float(fabs(argv[0].u.d)); }
static Val b_sqrt(int argc, Val *argv) { ARGS(1, "sqrt"); return mk_float(sqrt(argv[0].t == T_INT ? (double)argv[0].u.i : argv[0].u.d)); }
static Val b_floor(int argc, Val *argv) { ARGS(1, "floor"); return mk_int((i64)floor(argv[0].t == T_INT ? (double)argv[0].u.i : argv[0].u.d)); }
static Val minmax(int argc, Val *argv, int want)
{
    Val *items = argv; size_t n = (size_t)argc;
    if (argc == 1 && argv[0].t == T_LIST) { items = argv[0].u.l->v; n = argv[0].u.l->n; }
    if (!n) fail("min/max of nothing");
    Val best = items[0]; for (size_t i = 1; i < n; i++) if (compare(items[i], best) * want > 0) best = items[i];
    return best;
}
static Val b_min(int argc, Val *argv) { return minmax(argc, argv, -1); }
static Val b_max(int argc, Val *argv) { return minmax(argc, argv, 1); }
static Val b_sum(int argc, Val *argv) { ARGS(1, "sum"); if (argv[0].t != T_LIST) fail("sum needs a list"); Val acc = mk_int(0); for (size_t i = 0; i < argv[0].u.l->n; i++) acc = arith("+", acc, argv[0].u.l->v[i]); return acc; }
static Val b_map(int argc, Val *argv) { ARGS(2, "map"); if (argv[0].t != T_LIST) fail("map needs a list"); Val l = mk_list(); for (size_t i = 0; i < argv[0].u.l->n; i++) list_push(l.u.l, call_fn(argv[1], 1, &argv[0].u.l->v[i])); return l; }
static Val b_filter(int argc, Val *argv) { ARGS(2, "filter"); if (argv[0].t != T_LIST) fail("filter needs a list"); Val l = mk_list(); for (size_t i = 0; i < argv[0].u.l->n; i++) if (truthy(call_fn(argv[1], 1, &argv[0].u.l->v[i]))) list_push(l.u.l, argv[0].u.l->v[i]); return l; }
static Val b_reduce(int argc, Val *argv) { ARGS(3, "reduce"); if (argv[0].t != T_LIST) fail("reduce needs a list"); Val acc = argv[2]; for (size_t i = 0; i < argv[0].u.l->n; i++) { Val a[2] = { acc, argv[0].u.l->v[i] }; acc = call_fn(argv[1], 2, a); } return acc; }
static Val b_any(int argc, Val *argv) { ARGS(2, "any"); for (size_t i = 0; i < argv[0].u.l->n; i++) if (truthy(call_fn(argv[1], 1, &argv[0].u.l->v[i]))) return mk_bool(1); return mk_bool(0); }
static Val b_all(int argc, Val *argv) { ARGS(2, "all"); for (size_t i = 0; i < argv[0].u.l->n; i++) if (!truthy(call_fn(argv[1], 1, &argv[0].u.l->v[i]))) return mk_bool(0); return mk_bool(1); }
static Val b_first(int argc, Val *argv) { ARGS(1, "first"); if (argv[0].t != T_LIST || !argv[0].u.l->n) return NIL; return argv[0].u.l->v[0]; }
static Val b_last(int argc, Val *argv) { ARGS(1, "last"); if (argv[0].t != T_LIST || !argv[0].u.l->n) return NIL; return argv[0].u.l->v[argv[0].u.l->n - 1]; }
static Val b_reverse(int argc, Val *argv) { ARGS(1, "reverse"); Val l = mk_list(); for (size_t i = argv[0].u.l->n; i-- > 0;) list_push(l.u.l, argv[0].u.l->v[i]); return l; }
static Val b_enumerate(int argc, Val *argv) { ARGS(1, "enumerate"); Val l = mk_list(); for (size_t i = 0; i < argv[0].u.l->n; i++) { Val p = mk_list(); list_push(p.u.l, mk_int((i64)i)); list_push(p.u.l, argv[0].u.l->v[i]); list_push(l.u.l, p); } return l; }
static Val b_zip(int argc, Val *argv) { ARGS(2, "zip"); Val l = mk_list(); size_t n = argv[0].u.l->n < argv[1].u.l->n ? argv[0].u.l->n : argv[1].u.l->n; for (size_t i = 0; i < n; i++) { Val p = mk_list(); list_push(p.u.l, argv[0].u.l->v[i]); list_push(p.u.l, argv[1].u.l->v[i]); list_push(l.u.l, p); } return l; }

static Val sort_cmpf; static int sort_desc;
static int sort_cmp(const void *a, const void *b)
{
    Val x = *(const Val *)a, y = *(const Val *)b;
    if (sort_cmpf.t == T_FN || sort_cmpf.t == T_BUILTIN) { x = call_fn(sort_cmpf, 1, &x); y = call_fn(sort_cmpf, 1, &y); }
    int c = compare(x, y); return sort_desc ? -c : c;
}
static Val b_sort(int argc, Val *argv)
{
    if (argc < 1 || argv[0].t != T_LIST) fail("sort(list, key?, desc?)");
    Val l = mk_list(); for (size_t i = 0; i < argv[0].u.l->n; i++) list_push(l.u.l, argv[0].u.l->v[i]);
    sort_cmpf = argc > 1 ? argv[1] : NIL; sort_desc = argc > 2 && truthy(argv[2]);
    if (l.u.l->n > 1) {                                                                     /* stable insertion sort: predictable for small system lists */
        for (size_t i = 1; i < l.u.l->n; i++) { Val x = l.u.l->v[i]; size_t j = i; while (j > 0 && sort_cmp(&l.u.l->v[j - 1], &x) > 0) { l.u.l->v[j] = l.u.l->v[j - 1]; j--; } l.u.l->v[j] = x; }
    }
    return l;
}
static Val b_contains(int argc, Val *argv) { ARGS(2, "contains"); Node n = { 0 }; (void)n; if (argv[0].t == T_STR) { NEEDSTR(argv[1], "contains"); return mk_bool(memmem(argv[0].u.s->p, argv[0].u.s->n, argv[1].u.s->p, argv[1].u.s->n) != NULL); } if (argv[0].t == T_LIST) { for (size_t i = 0; i < argv[0].u.l->n; i++) if (equal(argv[0].u.l->v[i], argv[1])) return mk_bool(1); return mk_bool(0); } fail("contains needs a string or list"); return NIL; }

/* ---- JSON ---- */
static void json_out(Buf *b, Val v)
{
    switch (v.t) {
    case T_NIL: bputs(b, "null"); break;
    case T_STR: { bputc(b, '"'); for (size_t i = 0; i < v.u.s->n; i++) { unsigned char c = (unsigned char)v.u.s->p[i]; if (c == '"' || c == '\\') { bputc(b, '\\'); bputc(b, (char)c); } else if (c == '\n') bputs(b, "\\n"); else if (c < 32) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); bputs(b, t); } else bputc(b, (char)c); } bputc(b, '"'); break; }
    case T_LIST: bputc(b, '['); for (size_t i = 0; i < v.u.l->n; i++) { if (i) bputc(b, ','); json_out(b, v.u.l->v[i]); } bputc(b, ']'); break;
    case T_MAP: bputc(b, '{'); for (size_t i = 0; i < v.u.m->n; i++) { if (i) bputc(b, ','); json_out(b, (Val){ T_STR, .u.s = v.u.m->k[i] }); bputc(b, ':'); json_out(b, v.u.m->v[i]); } bputc(b, '}'); break;
    default: show(b, v, 0);
    }
}
static Val b_tojson(int argc, Val *argv) { ARGS(1, "tojson"); Buf b = { 0 }; json_out(&b, argv[0]); Val r = mk_strn(b.p ? b.p : "", b.n); free(b.p); return r; }

static const char *jp;
static void jws(void) { while (*jp == ' ' || *jp == '\n' || *jp == '\t' || *jp == '\r') jp++; }
static Val jparse(void)
{
    jws();
    if (*jp == '{') {
        Val m = mk_map(); jp++; jws();
        while (*jp && *jp != '}') { Val k = jparse(); jws(); if (*jp != ':') fail("json: ':' expected"); jp++; Val v = jparse(); map_set(m.u.m, k.u.s->p, k.u.s->n, v); jws(); if (*jp == ',') jp++; jws(); }
        if (*jp != '}') fail("json: '}' expected"); jp++; return m;
    }
    if (*jp == '[') { Val l = mk_list(); jp++; jws(); while (*jp && *jp != ']') { list_push(l.u.l, jparse()); jws(); if (*jp == ',') jp++; jws(); } if (*jp != ']') fail("json: ']' expected"); jp++; return l; }
    if (*jp == '"') {
        Buf b = { 0 }; jp++;
        while (*jp && *jp != '"') {
            if (*jp == '\\') { jp++; switch (*jp) { case 'n': bputc(&b, '\n'); break; case 't': bputc(&b, '\t'); break; case 'r': bputc(&b, '\r'); break;
                case 'u': { unsigned c = 0; sscanf(jp + 1, "%4x", &c); jp += 4; if (c < 128) bputc(&b, (char)c); else if (c < 0x800) { bputc(&b, (char)(0xC0 | c >> 6)); bputc(&b, (char)(0x80 | (c & 63))); } else { bputc(&b, (char)(0xE0 | c >> 12)); bputc(&b, (char)(0x80 | ((c >> 6) & 63))); bputc(&b, (char)(0x80 | (c & 63))); } break; }
                default: bputc(&b, *jp); } jp++; }
            else bputc(&b, *jp++);
        }
        if (*jp != '"') fail("json: unterminated string"); jp++;
        Val r = mk_strn(b.p ? b.p : "", b.n); free(b.p); return r;
    }
    if (!strncmp(jp, "true", 4)) { jp += 4; return mk_bool(1); }
    if (!strncmp(jp, "false", 5)) { jp += 5; return mk_bool(0); }
    if (!strncmp(jp, "null", 4)) { jp += 4; return NIL; }
    char *end; double d = strtod(jp, &end);
    if (end == jp) fail("json: unexpected character '%c'", *jp ? *jp : '?');
    int isint = 1; for (const char *q = jp; q < end; q++) if (*q == '.' || *q == 'e' || *q == 'E') isint = 0;
    Val r = isint ? mk_int(strtoll(jp, NULL, 10)) : mk_float(d); jp = end; return r;
}
static Val b_json(int argc, Val *argv) { ARGS(1, "json"); NEEDSTR(argv[0], "json"); jp = argv[0].u.s->p; Val v = jparse(); jws(); if (*jp) fail("json: trailing data"); return v; }

/* ---- tables ---- */
static void print_table(Val rows)
{
    if (rows.t != T_LIST || !rows.u.l->n || rows.u.l->v[0].t != T_MAP) return;
    Map *first = rows.u.l->v[0].u.m; size_t nc = first->n; size_t *w = calloc(nc, sizeof *w);
    char ***cells = xmalloc(rows.u.l->n * sizeof *cells);
    for (size_t c = 0; c < nc; c++) w[c] = first->k[c]->n;
    for (size_t r = 0; r < rows.u.l->n; r++) {
        cells[r] = xmalloc(nc * sizeof(char *));
        for (size_t c = 0; c < nc; c++) {
            Val *v = rows.u.l->v[r].t == T_MAP ? map_find(rows.u.l->v[r].u.m, first->k[c]->p, first->k[c]->n) : NULL;
            Buf b = { 0 }; if (v) show(&b, *v, 0); cells[r][c] = b.p ? b.p : strdup(""); if (strlen(cells[r][c]) > w[c]) w[c] = strlen(cells[r][c]);
        }
    }
    for (size_t c = 0; c < nc; c++) printf("%-*.*s  ", (int)w[c], (int)first->k[c]->n, first->k[c]->p); putchar('\n');
    for (size_t r = 0; r < rows.u.l->n; r++) { for (size_t c = 0; c < nc; c++) printf("%-*s  ", (int)w[c], cells[r][c]); putchar('\n'); }
    fflush(stdout);
}
static Val b_table(int argc, Val *argv) { ARGS(1, "table"); print_table(argv[0]); return argv[0]; }

/* ---- OS: files, processes, time ---- */
static Val read_file(const char *path)
{
    FILE *f = fopen(path, "rb"); if (!f) fail("cannot open %s: %s", path, strerror(errno));
    Buf b = { 0 }; char tmp[4096]; size_t n; while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) bput(&b, tmp, n); fclose(f);
    Val r = mk_strn(b.p ? b.p : "", b.n); free(b.p); return r;
}
static Val fs_read(int argc, Val *argv) { ARGS(1, "fs.read"); NEEDSTR(argv[0], "fs.read"); return read_file(argv[0].u.s->p); }
static Val fs_write_(int argc, Val *argv, const char *mode)
{
    ARGS(2, "fs.write"); NEEDSTR(argv[0], "fs.write"); Val s = to_str(argv[1]);
    FILE *f = fopen(argv[0].u.s->p, mode); if (!f) fail("cannot write %s: %s", argv[0].u.s->p, strerror(errno));
    fwrite(s.u.s->p, 1, s.u.s->n, f); fclose(f); return mk_int((i64)s.u.s->n);
}
static Val fs_write(int argc, Val *argv) { return fs_write_(argc, argv, "wb"); }
static Val fs_append(int argc, Val *argv) { return fs_write_(argc, argv, "ab"); }
static Val fs_exists(int argc, Val *argv) { ARGS(1, "fs.exists"); NEEDSTR(argv[0], "fs.exists"); return mk_bool(access(argv[0].u.s->p, F_OK) == 0); }
static Val fs_remove(int argc, Val *argv) { ARGS(1, "fs.remove"); NEEDSTR(argv[0], "fs.remove"); return mk_bool(unlink(argv[0].u.s->p) == 0); }
static Val fs_mkdir(int argc, Val *argv) { ARGS(1, "fs.mkdir"); NEEDSTR(argv[0], "fs.mkdir"); return mk_bool(mkdir(argv[0].u.s->p, 0755) == 0); }
static Val fs_list(int argc, Val *argv)
{
    ARGS(1, "fs.list"); NEEDSTR(argv[0], "fs.list");
    DIR *d = opendir(argv[0].u.s->p); if (!d) fail("cannot list %s: %s", argv[0].u.s->p, strerror(errno));
    Val l = mk_list(); struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' && (!de->d_name[1] || (de->d_name[1] == '.' && !de->d_name[2]))) continue;
        char full[1024]; snprintf(full, sizeof full, "%s/%s", argv[0].u.s->p, de->d_name);
        struct stat st; Val row = mk_map(); map_put(row, "name", mk_str(de->d_name));
        if (stat(full, &st) == 0) { map_put(row, "size", mk_int((i64)st.st_size)); map_put(row, "dir", mk_bool(S_ISDIR(st.st_mode))); } else { map_put(row, "size", mk_int(0)); map_put(row, "dir", mk_bool(0)); }
        list_push(l.u.l, row);
    }
    closedir(d);
    return b_sort(2, (Val[]){ l, field_fn("name") });
}
static Val b_sleep(int argc, Val *argv) { ARGS(1, "sleep"); i64 ms = argv[0].t == T_FLOAT ? (i64)argv[0].u.d : argv[0].u.i; fflush(stdout); usleep((useconds_t)(ms * 1000)); return NIL; }
static Val b_now(int argc, Val *argv) { (void)argc; (void)argv; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return mk_int((i64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000); }
static Val b_exit(int argc, Val *argv) { fflush(stdout); exit(argc ? (int)argv[0].u.i : 0); return NIL; }
static Val b_input(int argc, Val *argv) { if (argc) b_print(1, argv); fflush(stdout); char line[1024]; if (!fgets(line, sizeof line, stdin)) return NIL; size_t n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--; return mk_strn(line, n); }

static Val run_cmd(int argc, Val *argv, int capture, int shell)
{
    if (argc < 1) fail("run needs a command");
    int pfd[2] = { -1, -1 };
    if (capture && pipe(pfd)) fail("pipe failed");
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) fail("fork failed: %s", strerror(errno));
    if (pid == 0) {
        if (capture) { dup2(pfd[1], 1); close(pfd[0]); close(pfd[1]); }
        char **av = xmalloc((size_t)(argc + 4) * sizeof(char *)); int k = 0;
        if (shell) { av[k++] = "/bin/busybox"; av[k++] = "sh"; av[k++] = "-c"; av[k++] = (char *)to_str(argv[0]).u.s->p; }
        else for (int i = 0; i < argc; i++) av[k++] = (char *)to_str(argv[i]).u.s->p;
        av[k] = NULL;
        execv(av[0], av);
        if (!shell) { char alt[300]; snprintf(alt, sizeof alt, "/bin/%s", av[0]); execv(alt, av); snprintf(alt, sizeof alt, "/usr/bin/%s", av[0]); execv(alt, av); }
        fprintf(stderr, "nio: cannot run %s: %s\n", av[0], strerror(errno)); _exit(127);
    }
    Buf out = { 0 };
    if (capture) { close(pfd[1]); char tmp[4096]; ssize_t n; while ((n = read(pfd[0], tmp, sizeof tmp)) > 0) bput(&out, tmp, (size_t)n); close(pfd[0]); }
    int st = 0; waitpid(pid, &st, 0);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    if (capture) { Val r = mk_strn(out.p ? out.p : "", out.n); free(out.p); return r; }
    return mk_int(code);
}
static Val b_run(int argc, Val *argv) { return run_cmd(argc, argv, 0, 0); }
static Val b_sh(int argc, Val *argv) { return run_cmd(argc, argv, 1, 1); }
static Val b_capture(int argc, Val *argv) { return run_cmd(argc, argv, 1, 0); }

/* ---- worlds: paths and variables (same rules as the NEOS kernel's variable bus) ---- */
static Val path_to_win(const char *p)
{
    Buf b = { 0 };
    if (!strncmp(p, "/mnt/unc/", 9)) { bputs(&b, "\\\\"); p += 9; }
    else if (!strncmp(p, "/mnt/", 5) && isalpha((unsigned char)p[5]) && (p[6] == '/' || !p[6])) { bputc(&b, (char)toupper((unsigned char)p[5])); bputc(&b, ':'); p += 6; if (!*p) bputc(&b, '\\'); }
    else if (p[0] == '/') { bputs(&b, "\\\\neos\\root"); if (!p[1]) { bputc(&b, '\\'); p++; } }
    for (; *p; p++) bputc(&b, *p == '/' ? '\\' : *p);
    Val r = mk_strn(b.p ? b.p : "", b.n); free(b.p); return r;
}
static Val path_to_lin(const char *w)
{
    Buf b = { 0 };
    if (w[0] == '\\' && w[1] == '\\') { const char *r = w + 2; if (!strncmp(r, "neos\\root", 9) && (r[9] == '\\' || !r[9])) { r += 9; if (!*r) bputc(&b, '/'); } else bputs(&b, "/mnt/unc/"); for (; *r; r++) bputc(&b, *r == '\\' ? '/' : *r); }
    else if (isalpha((unsigned char)w[0]) && w[1] == ':') { bputs(&b, "/mnt/"); bputc(&b, (char)tolower((unsigned char)w[0])); const char *r = w + 2; if (*r != '\\' && *r != '/' && *r) bputc(&b, '/'); for (; *r; r++) bputc(&b, *r == '\\' ? '/' : *r); }
    else if (w[0] == '\\') { bputs(&b, "/mnt/c"); for (const char *r = w; *r; r++) bputc(&b, *r == '\\' ? '/' : *r); }
    else for (const char *r = w; *r; r++) bputc(&b, *r == '\\' ? '/' : *r);
    Val r = mk_strn(b.p ? b.p : "", b.n); free(b.p); return r;
}
static Val p_win(int argc, Val *argv) { ARGS(1, "path.win"); NEEDSTR(argv[0], "path.win"); return path_to_win(argv[0].u.s->p); }
static Val p_lin(int argc, Val *argv) { ARGS(1, "path.lin"); NEEDSTR(argv[0], "path.lin"); return path_to_lin(argv[0].u.s->p); }
static Val p_join(int argc, Val *argv) { Buf b = { 0 }; for (int i = 0; i < argc; i++) { Val s = to_str(argv[i]); if (i && b.n && b.p[b.n - 1] != '/') bputc(&b, '/'); bput(&b, s.u.s->p, s.u.s->n); } Val r = mk_strn(b.p ? b.p : "", b.n); free(b.p); return r; }
static Val p_base(int argc, Val *argv) { ARGS(1, "path.base"); NEEDSTR(argv[0], "path.base"); Str *s = argv[0].u.s; size_t i = s->n; while (i && s->p[i - 1] != '/' && s->p[i - 1] != '\\') i--; return mk_strn(s->p + i, s->n - i); }
static Val p_dir(int argc, Val *argv) { ARGS(1, "path.dir"); NEEDSTR(argv[0], "path.dir"); Str *s = argv[0].u.s; size_t i = s->n; while (i && s->p[i - 1] != '/' && s->p[i - 1] != '\\') i--; if (i > 1) i--; return mk_strn(s->p, i); }

/* env.lin("PATH") / env.win("PATH"): the shared variable bus renders one value in either convention */
static Val tool_call(const char *cmd, int *rc);
static Val env_view(const char *name, int win)
{
    char c[300]; snprintf(c, sizeof c, "var.get %s %s", name, win ? "win" : "lin");
    int rc; Val r = tool_call(c, &rc);
    if (rc == 0 && r.t == T_MAP) { Val *v = map_find(r.u.m, "value", 5); if (v) return *v; }
    const char *e = getenv(name); return e ? mk_str(e) : NIL;
}
static Val e_lin(int argc, Val *argv) { ARGS(1, "env.lin"); NEEDSTR(argv[0], "env.lin"); return env_view(argv[0].u.s->p, 0); }
static Val e_win(int argc, Val *argv) { ARGS(1, "env.win"); NEEDSTR(argv[0], "env.win"); return env_view(argv[0].u.s->p, 1); }
static Val e_get(int argc, Val *argv) { ARGS(1, "env.get"); NEEDSTR(argv[0], "env.get"); const char *e = getenv(argv[0].u.s->p); return e ? mk_str(e) : NIL; }

/* ---- system tools (NEOS only): the same capability gate every AI agent goes through ---- */
static Val tool_call(const char *cmd, int *rc)
{
#ifdef NIO_NEOS
    static char out[8192];
    long r = syscall(1000, cmd, out, sizeof out);
    *rc = (int)r;
    if (r != 0) return mk_str(out);
    jp = out; return jparse();
#else
    (void)cmd; *rc = -1; return mk_str("system tools are only available on NEOS");
#endif
}
static Val s_tool(int argc, Val *argv)
{
    if (argc < 1) fail("sys.tool(name, args...)");
    Buf b = { 0 };
    for (int i = 0; i < argc; i++) {
        Val s = to_str(argv[i]); if (i) bputc(&b, ' ');
        int plain = s.u.s->n > 0; for (size_t k = 0; k < s.u.s->n; k++) if (!isalnum((unsigned char)s.u.s->p[k]) && !strchr("._-/=:,[]@", s.u.s->p[k])) plain = 0;
        if (plain) bput(&b, s.u.s->p, s.u.s->n); else { bputc(&b, '"'); for (size_t k = 0; k < s.u.s->n; k++) { if (s.u.s->p[k] == '"') bputc(&b, '\\'); bputc(&b, s.u.s->p[k]); } bputc(&b, '"'); }
    }
    int rc; Val r = tool_call(b.p, &rc); free(b.p);
    if (rc != 0) { Buf m = { 0 }; bputs(&m, "tool failed ("); char t[16]; snprintf(t, sizeof t, "%d", rc); bputs(&m, t); bputs(&m, "): "); show(&m, r, 0); Val e = mk_strn(m.p, m.n); free(m.p); nio_throw(e); }
    return r;
}
static Val s_tune(int argc, Val *argv)
{
    if (argc == 1) { Val a[2] = { mk_str("tune.get"), argv[0] }; Val r = s_tool(2, a); return member_get(r, "value"); }
    if (argc == 2) { Val a[3] = { mk_str("tune.set"), argv[0], to_str(argv[1]) }; return s_tool(3, a); }
    fail("sys.tune(name) or sys.tune(name, value)"); return NIL;
}
static Val s_threads(int argc, Val *argv) { (void)argc; (void)argv; Val a[1] = { mk_str("thread.list") }; return s_tool(1, a); }
static Val s_info(int argc, Val *argv) { (void)argc; (void)argv; Val a[1] = { mk_str("sys.info") }; return s_tool(1, a); }
static Val s_mem(int argc, Val *argv) { (void)argc; (void)argv; Val a[1] = { mk_str("mem.info") }; return s_tool(1, a); }
static Val s_rt(int argc, Val *argv) { (void)argc; (void)argv; Val a[1] = { mk_str("rt.stats") }; return s_tool(1, a); }
static Val s_whoami(int argc, Val *argv) { (void)argc; (void)argv; Val a[1] = { mk_str("whoami") }; return s_tool(1, a); }

/* ---- the AI helper from inside Nio ---- */
static Val b_ai(int argc, Val *argv)
{
    ARGS(1, "ai"); NEEDSTR(argv[0], "ai");
    Val a[2] = { mk_str("aih.plan"), argv[0] };
    Val r = s_tool(2, a);
    return member_get(r, "plan");
}

static void def(const char *name, Builtin f) { env_def(globals, name, mk_bf(f)); }
static void ns(const char *name, ...)
{
    Val m = mk_map(); va_list ap; va_start(ap, name);
    for (;;) { const char *k = va_arg(ap, const char *); if (!k) break; Builtin f = va_arg(ap, Builtin); map_put(m, k, mk_bf(f)); }
    va_end(ap); env_def(globals, name, m);
}

static void init_globals(int argc, char **argv, int first)
{
    globals = new_env(NULL);
    def("print", b_print); def("println", b_println); def("len", b_len); def("str", b_str); def("int", b_int); def("float", b_float); def("type", b_type);
    def("range", b_range); def("push", b_push); def("pop", b_pop); def("keys", b_keys); def("values", b_values); def("has", b_has); def("join", b_join); def("split", b_split);
    def("replace", b_replace); def("upper", b_upper); def("lower", b_lower); def("trim", b_trim); def("starts", b_starts); def("ends", b_ends); def("find", b_find);
    def("abs", b_abs); def("sqrt", b_sqrt); def("floor", b_floor); def("min", b_min); def("max", b_max); def("sum", b_sum); def("map", b_map); def("filter", b_filter);
    def("reduce", b_reduce); def("any", b_any); def("all", b_all); def("first", b_first); def("last", b_last); def("reverse", b_reverse); def("enumerate", b_enumerate);
    def("zip", b_zip); def("sort", b_sort); def("contains", b_contains); def("json", b_json); def("tojson", b_tojson); def("table", b_table);
    def("sleep", b_sleep); def("now", b_now); def("exit", b_exit); def("input", b_input); def("run", b_run); def("sh", b_sh); def("capture", b_capture); def("ai", b_ai);
    ns("fs", "read", fs_read, "write", fs_write, "append", fs_append, "exists", fs_exists, "remove", fs_remove, "mkdir", fs_mkdir, "list", fs_list, NULL);
    ns("path", "win", p_win, "lin", p_lin, "join", p_join, "base", p_base, "dir", p_dir, NULL);
    ns("sys", "tool", s_tool, "tune", s_tune, "threads", s_threads, "info", s_info, "mem", s_mem, "rt", s_rt, "whoami", s_whoami, NULL);
    Val en = mk_map();
    extern char **environ;
    for (char **e = environ; *e; e++) { char *eq = strchr(*e, '='); if (eq) map_set(en.u.m, *e, (size_t)(eq - *e), mk_str(eq + 1)); }
    map_put(en, "get", mk_bf(e_get)); map_put(en, "lin", mk_bf(e_lin)); map_put(en, "win", mk_bf(e_win));
    env_def(globals, "env", en);
    Val args = mk_list(); for (int i = first; i < argc; i++) list_push(args.u.l, mk_str(argv[i]));
    env_def(globals, "args", args);
    env_def(globals, "PI", mk_float(3.14159265358979323846));
}

/* ============================================================ driver */
static int run_source(const char *src, int repl)
{
    jmp_buf jb; err_jmp = &jb;
    if (setjmp(jb)) {
        Buf b = { 0 }; show(&b, err_val, 0); fprintf(stderr, "nio: error: %s\n", b.p ? b.p : ""); free(b.p);
        err_jmp = NULL; return 1;
    }
    lex(src); pos = 0;
    Node *prog = nn(S_BLOCK);
    skip_nl();
    while (T()->k != K_EOF) { push_child(prog, parse_stmt()); skip_nl(); }
    for (int i = 0; i < prog->n; i++) {
        Node *s = prog->v[i];
        if (repl && s->kind == S_EXPR) { Val v = eval(s->a, globals); if (v.t != T_NIL) { if (v.t == T_LIST && v.u.l->n && v.u.l->v[0].t == T_MAP) print_table(v); else { Buf b = { 0 }; show(&b, v, 1); puts(b.p); free(b.p); } } }
        else { int r = exec(s, globals); (void)r; }
    }
    err_jmp = NULL;
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1; const char *code = NULL;
    if (argc > 1 && !strcmp(argv[1], "-e") && argc > 2) { code = argv[2]; i = 3; }
    else if (argc > 1 && !strcmp(argv[1], "--version")) { puts("Nio 1.0 - the NEOS language"); return 0; }
    if (!code && argc > 1) {
        init_globals(argc, argv, 2);
        FILE *f = fopen(argv[1], "rb"); if (!f) { fprintf(stderr, "nio: cannot open %s: %s\n", argv[1], strerror(errno)); return 66; }
        Buf b = { 0 }; char tmp[4096]; size_t n; while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) bput(&b, tmp, n); fclose(f);
        int r = run_source(b.p ? b.p : "", 0); fflush(stdout); return r;
    }
    init_globals(argc, argv, i);
    if (code) { int r = run_source(code, 0); fflush(stdout); return r; }
    puts("Nio 1.0 - the NEOS language. Ctrl-D exits.");
    char line[2048];
    for (;;) {
        fputs("nio> ", stdout); fflush(stdout);
        Buf src = { 0 };
        if (!fgets(line, sizeof line, stdin)) { putchar('\n'); return 0; }
        bputs(&src, line);
        int depth = 0; for (char *c = src.p; *c; c++) { if (*c == '{' || *c == '(' || *c == '[') depth++; else if (*c == '}' || *c == ')' || *c == ']') depth--; }
        while (depth > 0) { fputs("...  ", stdout); fflush(stdout); if (!fgets(line, sizeof line, stdin)) break; bputs(&src, line); for (char *c = line; *c; c++) { if (*c == '{' || *c == '(' || *c == '[') depth++; else if (*c == '}' || *c == ')' || *c == ']') depth--; } }
        run_source(src.p, 1); free(src.p);
    }
}
