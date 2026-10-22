#include "shell.h"

void tb_init(struct table *t, const char *c0, ...)
{
    memset(t, 0, sizeof *t);
    va_list ap; va_start(ap, c0);
    for (const char *h = c0; h && t->ncols < TB_COLS; h = va_arg(ap, const char *))
        strlcpy(t->head[t->ncols++], h, sizeof t->head[0]);
    va_end(ap);
}

int tb_newrow(struct table *t)
{
    if (t->nrows == TB_ROWS) { t->more = 1; return -1; }
    return t->nrows++;
}

void tb_set(struct table *t, int col, const char *fmt, ...)
{
    if (t->nrows == 0 || col >= t->ncols) return;
    va_list ap; va_start(ap, fmt);
    kvsnprintf(t->cell[t->nrows - 1][col], TB_CELL, fmt, ap);
    va_end(ap);
}

void tb_widths(const struct table *t, int *w, int maxw)
{
    for (int c = 0; c < t->ncols; c++) {
        int m = (int)strlen(t->head[c]);
        for (int r = 0; r < t->nrows; r++) { int l = (int)strlen(t->cell[r][c]); if (l > m) m = l; }
        w[c] = m > maxw ? maxw : m;
    }
}

static int numeric(const char *s) { i64 v; return !parse_i64(s, &v); }

void tb_format_row(const struct table *t, int row, const int *w, char *buf, size_t n)
{
    size_t l = 0;
    for (int c = 0; c < t->ncols && l + 1 < n; c++) {
        const char *s = row < 0 ? t->head[c] : t->cell[row][c];
        int right = row >= 0 && numeric(s) && s[0];
        l += ksnprintf(buf + l, n - l, right ? "%*.*s  " : "%-*.*s  ", w[c], w[c], s);
    }
    while (l && buf[l - 1] == ' ') buf[--l] = 0;
}

void tb_render(const struct table *t)
{
    int w[TB_COLS]; char line[200];
    tb_widths(t, w, 38);
    tb_format_row(t, -1, w, line, sizeof line);
    kprintf("\x1b[1;36m%s\x1b[0m\n", line);
    for (int r = 0; r < t->nrows; r++) { tb_format_row(t, r, w, line, sizeof line); kprintf("%s\n", line); }
    if (t->more) kprintf("\x1b[90m... (more rows not shown)\x1b[0m\n");
}

static void json_str(const char *s)
{
    kputc('"');
    for (; *s; s++) { if (*s == '"' || *s == '\\') kputc('\\'); kputc(*s); }
    kputc('"');
}

void tb_json(const struct table *t)
{
    kprintf("[");
    for (int r = 0; r < t->nrows; r++) {
        kprintf(r ? ",\n {" : "\n {");
        for (int c = 0; c < t->ncols; c++) {
            if (c) kprintf(", ");
            json_str(t->head[c]); kprintf(": ");
            if (numeric(t->cell[r][c]) && t->cell[r][c][0]) kprintf("%s", t->cell[r][c]); else json_str(t->cell[r][c]);
        }
        kprintf("}");
    }
    kprintf("\n]\n");
}

static int colidx(const struct table *t, const char *name)
{
    for (int c = 0; c < t->ncols; c++) if (!strcasecmp(t->head[c], name)) return c;
    return -1;
}

static int cmp_cells(const char *a, const char *b)
{
    i64 x, y;
    if (!parse_i64(a, &x) && !parse_i64(b, &y)) return x < y ? -1 : x > y;
    return strcmp(a, b);
}

static void drop_rows(struct table *t, const char *keep)    /* keep[r] != 0 survives */
{
    int o = 0;
    for (int r = 0; r < t->nrows; r++) if (keep[r]) { if (o != r) memcpy(t->cell[o], t->cell[r], sizeof t->cell[0]); o++; }
    t->nrows = o;
}

int tb_filter(struct table *t, int argc, char **argv, const char **err)
{
    const char *cmd = argv[0];
    if (!strcmp(cmd, "where")) {
        char expr[96] = "";
        for (int i = 1; i < argc; i++) strlcat(expr, argv[i], sizeof expr);
        char *op = NULL; int oplen = 0;
        for (char *p = expr; *p; p++) {
            if ((p[0] == '!' || p[0] == '<' || p[0] == '>') && p[1] == '=') { op = p; oplen = 2; break; }
            if (p[0] == '=' || p[0] == '<' || p[0] == '>' || p[0] == '~') { op = p; oplen = 1; break; }
        }
        if (!op) { *err = "usage: where <col><op><value>   op: = != < > <= >= ~"; return -1; }
        char o[3] = { op[0], oplen == 2 ? op[1] : 0, 0 }, col[24], *val = op + oplen;
        size_t cl = (size_t)(op - expr); if (cl >= sizeof col) cl = sizeof col - 1;
        memcpy(col, expr, cl); col[cl] = 0;
        int ci = colidx(t, col);
        if (ci < 0) { *err = "unknown column"; return -1; }
        char keep[TB_ROWS];
        for (int r = 0; r < t->nrows; r++) {
            const char *c = t->cell[r][ci]; int k;
            if (o[0] == '~') { k = 0; for (size_t i = 0; c[i] && !k; i++) { size_t j = 0; while (val[j] && to_lower(c[i + j]) == to_lower(val[j])) j++; k = !val[j]; } }
            else {
                int d = cmp_cells(c, val);
                if (!strcasecmp(c, val)) d = 0;
                k = o[0] == '=' ? d == 0 : o[0] == '!' ? d != 0 : o[1] == '=' ? (o[0] == '<' ? d <= 0 : d >= 0) : (o[0] == '<' ? d < 0 : d > 0);
            }
            keep[r] = (char)k;
        }
        drop_rows(t, keep);
    } else if (!strcmp(cmd, "sort")) {
        if (argc < 2) { *err = "usage: sort <col> [desc]"; return -1; }
        int ci = colidx(t, argv[1]), desc = argc > 2 && !strcmp(argv[2], "desc");
        if (ci < 0) { *err = "unknown column"; return -1; }
        char tmp[TB_COLS][TB_CELL];
        for (int i = 1; i < t->nrows; i++)                  /* insertion sort: stable, no allocation */
            for (int j = i; j > 0; j--) {
                int d = cmp_cells(t->cell[j - 1][ci], t->cell[j][ci]);
                if (desc ? d >= 0 : d <= 0) break;
                memcpy(tmp, t->cell[j - 1], sizeof tmp); memcpy(t->cell[j - 1], t->cell[j], sizeof tmp); memcpy(t->cell[j], tmp, sizeof tmp);
            }
    } else if (!strcmp(cmd, "head") || !strcmp(cmd, "tail")) {
        i64 n = 10;
        if (argc > 1 && parse_i64(argv[1], &n)) { *err = "expected a count"; return -1; }
        if (n < 0) n = 0;
        if (n < t->nrows) {
            if (cmd[0] == 't') { int s = t->nrows - (int)n; for (int r = 0; r < (int)n; r++) memcpy(t->cell[r], t->cell[s + r], sizeof t->cell[0]); }
            t->nrows = (int)n;
        }
    } else if (!strcmp(cmd, "cols")) {
        int idx[TB_COLS], n = 0;
        for (int i = 1; i < argc && n < TB_COLS; i++) {
            idx[n] = colidx(t, argv[i]);
            if (idx[n] < 0) { *err = "unknown column"; return -1; }
            n++;
        }
        char h[TB_COLS][16], row[TB_COLS][TB_CELL];
        for (int i = 0; i < n; i++) strlcpy(h[i], t->head[idx[i]], sizeof h[0]);
        for (int r = 0; r < t->nrows; r++) {
            for (int i = 0; i < n; i++) strlcpy(row[i], t->cell[r][idx[i]], TB_CELL);
            for (int i = 0; i < n; i++) strlcpy(t->cell[r][i], row[i], TB_CELL);
        }
        memcpy(t->head, h, sizeof h); t->ncols = n;
    } else if (!strcmp(cmd, "count")) {
        int n = t->nrows;
        tb_init(t, "count", NULL); tb_newrow(t); tb_set(t, 0, "%d", n);
    } else { *err = "unknown filter"; return -1; }
    return 0;
}
