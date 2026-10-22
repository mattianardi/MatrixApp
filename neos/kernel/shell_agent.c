#include "shell.h"
#include "agent.h"

/* `agent ...`: the operator's console for the agent subsystem. Only the human at the console can grant. */
static void err(const char *m) { kprintf("\x1b[91m%s\x1b[0m\n", m); }
static const char *pname(int p) { return p == POL_ALLOW ? "allow" : p == POL_ASK ? "ask" : "deny"; }

static int list(struct table *tb)
{
    tb_init(tb, "agent", "kind", "state", "capabilities", "calls", "denied", "asked", NULL);
    for (int i = 0; i < AG_MAX; i++) {
        struct agent *a = agent_at(i);
        if (!a || tb_newrow(tb) < 0) continue;
        char caps[40] = "";
        for (int c = 0; c < NCAPS; c++) if (a->caps & (1u << c)) { if (caps[0]) strlcat(caps, ",", sizeof caps); strlcat(caps, cap_names[c], sizeof caps); }
        tb_set(tb, 0, "%s", a->name); tb_set(tb, 1, "%s", a->kind == AK_NATIVE ? "native" : "remote");
        tb_set(tb, 2, "%s", a->state == AS_ACTIVE ? (agent_is_frozen() ? "FROZEN" : "active") : "suspended");
        tb_set(tb, 3, "%s", caps[0] ? caps : "-"); tb_set(tb, 4, "%lu", a->calls); tb_set(tb, 5, "%lu", a->denied); tb_set(tb, 6, "%lu", a->asked);
    }
    return 0;
}

int c_agent(int argc, char **argv, struct ctx *c)
{
    const char *sub = argc > 1 ? argv[1] : "list", *e = "";
    if (!strcmp(sub, "list")) return list(c->tb);

    if (!strcmp(sub, "tools")) {
        static const char *rn[] = { "read", "reversible", "irreversible" };
        tb_init(c->tb, "tool", "cap", "risk", "params", NULL);
        for (int i = 0; i < agent_ntools; i++) {
            if (tb_newrow(c->tb) < 0) break;
            tb_set(c->tb, 0, "%s", agent_tools[i].name); tb_set(c->tb, 1, "%s", cap_names[agent_tools[i].cap]);
            tb_set(c->tb, 2, "%s", rn[agent_tools[i].risk]); tb_set(c->tb, 3, "%s", agent_tools[i].params);
        }
        return 0;
    }
    if (!strcmp(sub, "pending")) {
        tb_init(c->tb, "id", "agent", "tool", "args", "waiting_s", NULL);
        struct approval ap;
        for (int i = 0; !approval_get(i, &ap); i++) {
            struct agent *a = agent_at(ap.agent);
            if (tb_newrow(c->tb) < 0) break;
            tb_set(c->tb, 0, "%d", ap.id); tb_set(c->tb, 1, "%s", a ? a->name : "?"); tb_set(c->tb, 2, "%s", ap.tool);
            tb_set(c->tb, 3, "%s", ap.args); tb_set(c->tb, 4, "%lu", (ticks - ap.tick) / timer_hz());
        }
        return 0;
    }
    if (!strcmp(sub, "log") && !(argc > 2 && !strcmp(argv[2], "verify"))) {
        i64 n = 15; if (argc > 2) parse_i64(argv[2], &n);
        tb_init(c->tb, "seq", "ms_ago", "agent", "tool", "args", "rc", "by", "hash", NULL);
        struct audit_rec r;
        for (int i = 0; i < n && !audit_get(i, &r); i++) {
            struct agent *a = agent_at(r.agent);
            if (tb_newrow(c->tb) < 0) break;
            tb_set(c->tb, 0, "%lu", r.seq); tb_set(c->tb, 1, "%lu", (ticks - r.tick) * 1000 / timer_hz()); tb_set(c->tb, 2, "%s", a ? a->name : "?");
            tb_set(c->tb, 3, "%s", r.tool); tb_set(c->tb, 4, "%s", r.args); tb_set(c->tb, 5, "%d", r.rc);
            tb_set(c->tb, 6, "%s", r.approver ? "operator" : "-"); tb_set(c->tb, 7, "%.10s", r.hash);
        }
        return 0;
    }
    if (!strcmp(sub, "journal") && argc > 2) {
        struct agent *a = agent_find(argv[2]);
        if (!a) { if (!c->pure) err("no such agent"); return -1; }
        tb_init(c->tb, "n", "kind", "target", "old", NULL);
        static const char *kn[] = { "tune", "var", "prio", "fs", "pkg" };
        for (int i = a->jn - 1; i >= 0; i--) {
            if (tb_newrow(c->tb) < 0) break;
            tb_set(c->tb, 0, "%d", a->jn - i); tb_set(c->tb, 1, "%s", kn[a->jr[i].kind]); tb_set(c->tb, 2, "%s", a->jr[i].name);
            tb_set(c->tb, 3, "%s", a->jr[i].had ? (a->jr[i].blen ? "(file content)" : a->jr[i].old) : "(did not exist)");
        }
        return 0;
    }
    if (c->pure) return -1;

    if (!strcmp(sub, "log")) {
        int bad = audit_verify();
        if (bad) kprintf("\x1b[91maudit chain BROKEN at record %d\x1b[0m\n", bad);
        else kprintf("audit chain intact: %d records, each hash covers its predecessor\n", audit_count());
        return bad ? -1 : 0;
    }
    if (!strcmp(sub, "add") && argc > 3) {
        char tok[80] = ""; const char *secret = NULL;
        for (int i = 4; i < argc; i++) if (!strncmp(argv[i], "secret=", 7)) secret = argv[i] + 7;
        struct agent *a = agent_add(argv[2], !strcmp(argv[3], "remote") ? AK_REMOTE : AK_NATIVE, secret, tok, sizeof tok, &e);
        if (!a) { err(e); return -1; }
        kprintf("agent '%s' (%s) created with NO capabilities. Grant what it needs: agent grant %s <cap[:scope]>...\n", a->name, argv[3], a->name);
        if (tok[0]) kprintf("\x1b[1;93mtoken (shown once, give it to the agent out of band):\x1b[0m %s\n", tok);
        return 0;
    }
    if (!strcmp(sub, "grant") && argc > 3) {
        struct agent *a = agent_find(argv[2]);
        if (!a) { err("no such agent"); return -1; }
        for (int i = 3; i < argc; i++) {
            char *colon = strchr(argv[i], ':');
            if (colon) *colon = 0;
            if (agent_grant(a, argv[i], colon ? colon + 1 : NULL, &e)) { err(e); return -1; }
            kprintf("granted %s%s%s to %s\n", argv[i], colon ? " scope " : "", colon ? colon + 1 : "", a->name);
        }
        return 0;
    }
    if (!strcmp(sub, "revoke") && argc > 2) {
        struct agent *a = agent_find(argv[2]);
        if (!a || agent_revoke(a, argc > 3 ? argv[3] : NULL)) { err("no such agent or capability"); return -1; }
        return 0;
    }
    if (!strcmp(sub, "policy") && argc > 4) {
        struct agent *a = agent_find(argv[2]);
        int r = !strcmp(argv[3], "read") ? RISK_READ : !strcmp(argv[3], "reversible") ? RISK_REV : !strcmp(argv[3], "irreversible") ? RISK_IRREV : -1;
        int p = !strcmp(argv[4], "allow") ? POL_ALLOW : !strcmp(argv[4], "ask") ? POL_ASK : !strcmp(argv[4], "deny") ? POL_DENY : -1;
        if (!a || r < 0 || p < 0) { err("usage: agent policy <name> read|reversible|irreversible allow|ask|deny"); return -1; }
        a->policy[r] = (u8)p;
        kprintf("%s: %s actions -> %s\n", a->name, argv[3], pname(p));
        return 0;
    }
    if ((!strcmp(sub, "suspend") || !strcmp(sub, "resume") || !strcmp(sub, "remove")) && argc > 2) {
        struct agent *a = agent_find(argv[2]);
        if (!a) { err("no such agent"); return -1; }
        if (sub[0] == 'r' && sub[1] == 'e' && sub[2] == 'm') agent_remove(argv[2]);
        else a->state = sub[0] == 's' ? AS_SUSPENDED : AS_ACTIVE;
        return 0;
    }
    if (!strcmp(sub, "freeze")) { agent_freeze(); kprintf("all agents frozen (thaw with `agent thaw`; hotkey Ctrl+\\)\n"); return 0; }
    if (!strcmp(sub, "thaw"))   { agent_thaw(); kprintf("agents active again\n"); return 0; }
    if ((!strcmp(sub, "approve") || !strcmp(sub, "deny")) && argc > 2) {
        i64 id;
        if (parse_i64(argv[2], &id) || approval_decide((int)id, sub[0] == 'a')) { err("no such pending request (see `agent pending`)"); return -1; }
        return 0;
    }
    if (!strcmp(sub, "rollback") && argc > 2) {
        struct agent *a = agent_find(argv[2]);
        i64 n = 1; if (argc > 3) parse_i64(argv[3], &n);
        if (!a) { err("no such agent"); return -1; }
        static char ob[1024]; struct jbuf o = { ob, sizeof ob, 0 };
        int d = journal_rollback(a, (int)n, &o);
        kprintf("undid %d change(s): %s\n", d, ob);
        return 0;
    }
    if (!strcmp(sub, "tuner") && argc > 2 && !strcmp(argv[2], "run")) { agent_tuner_run(argc > 3 ? argv[3] : NULL); return 0; }
    if (!strcmp(sub, "call") && argc > 3) {
        struct agent *a = agent_find(argv[2]);
        if (!a) { err("no such agent"); return -1; }
        static char ob[4096]; struct jbuf o = { ob, sizeof ob, 0 };
        int rc = agent_call(a, argc - 3, argv + 3, &o, 1);          /* the operator is the approver */
        kprintf("%s rc=%d %s\n", rc ? "\x1b[91m" : "\x1b[92m", rc, "\x1b[0m");
        kprintf("%s\n", ob);
        return rc ? -1 : 0;
    }
    err("usage: agent [list | tools | add <n> native|remote [secret=..] | grant <n> <cap[:scope]>.. | revoke <n> [cap] | policy <n> <risk> <allow|ask|deny>\n"
        "       | suspend|resume|remove <n> | freeze | thaw | pending | approve <id> | deny <id> | log [n|verify] | journal <n> | rollback <n> [k]\n"
        "       | call <n> <tool> [args] | tuner run [metric]]");
    return -1;
}
