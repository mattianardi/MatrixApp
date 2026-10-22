#include "agent.h"

static const char *strstr_json(const char *h, const char *n)
{
    for (; *h; h++) { size_t i = 0; while (n[i] && h[i] == n[i]) i++; if (!n[i]) return h; }
    return NULL;
}
static i64 atoi64(const char *s) { i64 v = 0; int neg = 0; if (*s == '-') { neg = 1; s++; } while (is_digit(*s)) v = v * 10 + (*s++ - '0'); return neg ? -v : v; }

/*
 * "tuner": a native agent. It is an ordinary principal: it can only do what the operator granted
 * (observe + experiment + a few settings with value ranges), through the same gate, and everything
 * it does is audited and journaled. Method: coordinate search - for each knob try neighbouring
 * values with experiment.run, which keeps a change only if the metric improves by >2%, else reverts.
 */
struct knob { const char *name; const char *vals[6]; int n; };
static const struct knob knobs[] = {
    { "timer.hz",      { "250", "500", "1000", "2000", "4000" }, 5 },
    { "sched.quantum", { "2", "5", "10", "20" }, 4 },
    { "net.poll_ms",   { "1", "2", "5" }, 3 },
    { "ui.refresh_ms", { "250", "500", "1000", "2000" }, 4 },
    { "console.batch", { "128", "512", "2048" }, 3 },
};
static volatile int running;
static char metric[24] = "rt.cost";

static int run_exp(struct agent *a, const char *knob, const char *val, i64 *base, i64 *trial, int *kept)
{
    static char ob[512]; struct jbuf o = { ob, sizeof ob, 0 };
    char kv[64], *argv[5] = { "experiment.run", metric, "300", kv, NULL };
    ksnprintf(kv, sizeof kv, "%s=%s", knob, val);
    int rc = agent_call(a, 4, argv, &o, 0);
    if (rc) { kprintf("[tuner] %s=%s -> refused (%d: %s)\n", knob, val, rc, ob); return rc; }
    const char *p = strstr_json(ob, "\"baseline\":"); *base = p ? atoi64(p + 11) : 0;
    p = strstr_json(ob, "\"trial\":"); *trial = p ? atoi64(p + 8) : 0;
    *kept = strstr_json(ob, "\"kept\":true") != NULL;
    return 0;
}

static void tuner_thread(void *arg)
{
    (void)arg;
    struct agent *a = agent_find("tuner");
    kprintf("[tuner] start: minimising %s over %d knobs\n", metric, (int)(sizeof knobs / sizeof *knobs));
    for (unsigned k = 0; k < sizeof knobs / sizeof *knobs && a; k++) {
        const struct knob *kn = &knobs[k];
        struct tunable *t = tune_find(kn->name);
        if (!t) continue;
        char cur[48]; tune_format(t, cur, sizeof cur);
        int ci = -1;
        for (int i = 0; i < kn->n; i++) if (!strcmp(kn->vals[i], cur)) ci = i;
        if (ci < 0) { kprintf("[tuner] %s=%s is off the search grid, skipping\n", kn->name, cur); continue; }
        for (int dir = -1; dir <= 1; dir += 2) {
            for (;;) {
                int ni = ci + dir;
                if (ni < 0 || ni >= kn->n) break;
                i64 base, trial; int kept;
                if (run_exp(a, kn->name, kn->vals[ni], &base, &trial, &kept)) break;      /* refused by the gate: stop this knob */
                kprintf("[tuner] %-14s %s -> %-5s  %s %ld -> %ld  %s\n", kn->name, kn->vals[ci], kn->vals[ni], metric, base, trial, kept ? "KEPT" : "reverted");
                if (!kept) break;
                ci = ni;
            }
        }
        kprintf("[tuner] %s settled at %s\n", kn->name, kn->vals[ci]);
    }
    kprintf("[tuner] done. Review with `agent log`, undo with `agent rollback tuner 99`.\n");
    running = 0;
}

void agent_tuner_run(const char *m)
{
    if (!agent_find("tuner")) { kprintf("create the agent first: agent add tuner native; agent grant tuner observe experiment 'tune:timer.hz[250..4000],sched.quantum[2..20],net.poll_ms[1..5],ui.refresh_ms[250..2000],console.batch[128..2048]'\n"); return; }
    if (running) { kprintf("tuner already running\n"); return; }
    if (m && m[0]) strlcpy(metric, m, sizeof metric);
    running = 1;
    thread_create("tuner", 5, tuner_thread, NULL);
}
