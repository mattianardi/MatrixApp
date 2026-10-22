#include "net.h"

static struct tunable *t_idle, *t_agent;

static int find_hdr_end(const char *b, size_t n)
{
    for (size_t i = 0; i + 3 < n; i++) if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') return (int)i + 4;
    return -1;
}

static int hdr_value(const char *hdr, int hlen, const char *name, char *out, size_t on)
{
    size_t nl = strlen(name);
    for (int i = 0; i < hlen;) {
        int e = i; while (e < hlen && hdr[e] != '\n') e++;
        if ((size_t)(e - i) > nl && !strncmp(hdr + i, name, nl) && hdr[i + nl] == ':') {
            int s = i + (int)nl + 1; while (s < e && hdr[s] == ' ') s++;
            int k = e; while (k > s && (hdr[k - 1] == '\r' || hdr[k - 1] == ' ')) k--;
            size_t l = (size_t)(k - s); if (l >= on) l = on - 1;
            memcpy(out, hdr + s, l); out[l] = 0;
            return 0;
        }
        i = e + 1;
    }
    return -1;
}

static int get_once(const char *url, void *buf, size_t cap, size_t *len, int *status, const char **err, char *loc, size_t locn)
{
    if (!t_idle) {
        t_idle  = tune_int("http", "idle_ms", "abort a transfer after this much silence", TF_LIVE, 5000, 100, 120000, NULL);
        t_agent = tune_str("http", "user_agent", "User-Agent header", TF_LIVE, "NEOS/" NEOS_VERSION, NULL);
    }
    if (!strncmp(url, "https://", 8)) { *err = "https:// needs TLS, which NEOS does not have yet"; return -1; }
    if (strncmp(url, "http://", 7)) { *err = "only http:// URLs are supported"; return -1; }
    char host[64], path[160]; u16 port = 80;
    const char *p = url + 7; size_t hn = 0;
    while (*p && *p != '/' && *p != ':' && hn < sizeof host - 1) host[hn++] = *p++;
    host[hn] = 0;
    if (*p == ':') { p++; u32 v = 0; while (is_digit(*p)) v = v * 10 + (*p++ - '0'); port = (u16)v; }
    strlcpy(path, *p ? p : "/", sizeof path);
    u32 ip;
    if (net_resolve(host, &ip)) { *err = "cannot resolve host"; return -1; }
    struct tcp_conn *c = tcp_connect(ip, port, 4000);
    if (!c) { *err = "connection failed"; return -1; }

    char req[320];
    int rl = ksnprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: */*\r\nConnection: close\r\n\r\n",
                       path, host, t_agent->sval);
    if (tcp_send(c, req, rl)) { tcp_close(c); *err = "send failed"; return -1; }

    char *b = buf; size_t got = 0; int hend = -1; long clen = -1; int rc = 0;
    for (;;) {
        if (got >= cap) { *err = "response larger than the buffer"; rc = -1; break; }
        int n = tcp_recv(c, b + got, (int)(cap - got), (u64)t_idle->val);
        if (n == 0) break;
        if (n < 0) { *err = n == -2 ? "connection reset" : "timeout"; rc = -1; break; }
        got += n;
        if (hend < 0 && (hend = find_hdr_end(b, got)) > 0) {
            char v[32];
            if (!hdr_value(b, hend, "Content-Length", v, sizeof v)) { i64 x; if (!parse_i64(v, &x)) clen = (long)x; }
        }
        if (hend > 0 && clen >= 0 && got >= (size_t)hend + (size_t)clen) break;
    }
    tcp_close(c);
    if (rc) return rc;
    if (hend < 0) { *err = "malformed response"; return -1; }
    *status = 0;
    if (!strncmp(b, "HTTP/1.", 7)) *status = (b[9] - '0') * 100 + (b[10] - '0') * 10 + (b[11] - '0');
    if (loc) { loc[0] = 0; hdr_value(b, hend, "Location", loc, locn); }
    size_t body = got - (size_t)hend;
    if (clen >= 0 && body > (size_t)clen) body = (size_t)clen;
    memmove(b, b + hend, body);
    *len = body;
    return 0;
}

int http_get(const char *url, void *buf, size_t cap, size_t *len, int *status, const char **err)
{
    static const char *dummy; if (!err) err = &dummy;
    char cur[200], loc[200];
    strlcpy(cur, url, sizeof cur);
    for (int hop = 0; hop < 4; hop++) {
        if (get_once(cur, buf, cap, len, status, err, loc, sizeof loc)) return -1;
        if ((*status == 301 || *status == 302 || *status == 307 || *status == 308) && loc[0]) { strlcpy(cur, loc, sizeof cur); continue; }
        return 0;
    }
    *err = "too many redirects";
    return -1;
}
