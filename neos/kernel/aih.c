#include "agent.h"
#include "fs.h"
#include "vfs.h"
#include "varbus.h"
#include "proc.h"
#include "aih.h"

/*
 * AIH - "AI Help". The user types what they want in plain language (Italian or English); a plan comes back:
 *     SAY <explanation>
 *     STEP <neos|linux|windows|nio> <low|medium|high> <command...>
 * The plan is produced by an attached AI agent (requests are pushed over the agent channel, answers come back through
 * the capability-gated tool aih.answer) or, when no agent is attached or it does not answer in time, by a local
 * deterministic planner. Nothing runs without the user's confirmation, and every step then goes through the normal
 * shell / permission / audit path - the AI never gets a back door.
 */
static int strstr_pct(const char *s) { for (; *s; s++) if (*s == '%') return 1; return 0; }
static const char *strstr_after(const char *h, const char *n)       /* text following the first occurrence of n, or NULL */
{
    size_t nl = strlen(n);
    for (; *h; h++) { size_t i = 0; while (i < nl && to_lower((u8)h[i]) == to_lower((u8)n[i])) i++; if (i == nl) { const char *r = h + nl; while (*r == ' ' || *r == ':') r++; return r; } }
    return NULL;
}
#define QN 4
struct req { int id; volatile int state; char text[300]; char ctx[900]; char plan[AIH_PLAN_MAX]; char from[16]; int pushed; };
enum { R_FREE, R_PENDING, R_ANSWERED };
static struct req reqs[QN];
static int next_id = 1;
static volatile int planner_idx = -1;                      /* agent table index of the attached planner */
static u64 planner_seen;                                   /* tick of its last sign of life */
static struct tunable *t_timeout, *t_auto;

void aih_init(void)
{
    module_register("aih", "1.0", "natural-language command help: AI-agent or local planner, user-confirmed plans");
    t_timeout = tune_int ("aih", "timeout_s", "how long to wait for the attached AI before using the local planner", TF_LIVE, 20, 1, 300, NULL);
    t_auto    = tune_bool("aih", "autorun_low", "run plans made only of low-risk steps without asking", TF_LIVE | TF_NOAGENT, 0, NULL);
}
int aih_autorun_low(void) { return (int)t_auto->val; }

void aih_planner_set(int idx) { planner_idx = idx; planner_seen = ticks; }
int aih_planner_active(void) { return planner_idx >= 0 && agent_at(planner_idx) && ticks - planner_seen < ms_to_ticks(300000); }
int aih_planner_idx(void) { return planner_idx; }

/* ------------------------------------------------------------ context for an AI planner */
static void build_context(char *o, size_t n)
{
    char cwd[96] = "/";
    bus_get("PWD", NEOS_VIEW_LINUX, cwd, sizeof cwd);
    size_t l = 0;
    l += ksnprintf(o + l, n - l, "os=NEOS %s; cwd=%s; worlds=neos,linux(static ELF, BusyBox applets in /bin),windows(planned),nio; "
                   "format=SAY <text> | STEP <neos|linux|windows|nio> <low|medium|high> <command>; neos_commands=", NEOS_VERSION, cwd);
    extern const char *shell_command_name(int i);
    for (int i = 0; shell_command_name(i) && l + 20 < n; i++) l += ksnprintf(o + l, n - l, "%s%s", i ? "," : "", shell_command_name(i));
}

/* ------------------------------------------------------------ local planner */
static void lower_copy(char *d, const char *s, size_t n) { size_t i = 0; for (; s[i] && i + 1 < n; i++) d[i] = (char)to_lower((u8)s[i]); d[i] = 0; }
static int has(const char *t, const char *k) { size_t kl = strlen(k); for (; *t; t++) if (!strncmp(t, k, kl)) return 1; return 0; }
static int hasany(const char *t, const char *ks) {                                  /* ks: '|' separated */
    char k[40];
    for (const char *p = ks; *p;) {
        size_t n = 0; while (p[n] && p[n] != '|') n++;
        if (n && n < sizeof k) { memcpy(k, p, n); k[n] = 0; if (has(t, k)) return 1; }
        p += n; if (*p == '|') p++;
    }
    return 0;
}
static int word_after(const char *orig, const char *low, const char *kw, char *out, size_t n)    /* next token after kw, original case */
{
    size_t kl = strlen(kw);
    for (const char *p = low; *p; p++) if (!strncmp(p, kw, kl)) {
        const char *q = orig + (p - low) + kl;
        while (*q == ' ') q++;
        size_t i = 0; while (q[i] && q[i] != ' ' && i + 1 < n) { out[i] = q[i]; i++; }
        out[i] = 0;
        return i > 0;
    }
    return 0;
}
static int first_int(const char *t, i64 *v)
{
    for (; *t; t++) if (is_digit(*t)) { *v = 0; while (is_digit(*t)) *v = *v * 10 + (*t++ - '0'); return 1; }
    return 0;
}
static int risk_of(const char *cmd)
{
    static const char *const high[] = { "rm", "del", "erase", "rmdir", "pkill", "kill", "poweroff", "reboot", "format", "mkfs", "dd", "shutdown", "unpin", NULL };
    char w[24]; size_t i = 0;
    while (cmd[i] == ' ') cmd++;
    while (cmd[i] && cmd[i] != ' ' && i + 1 < sizeof w) { w[i] = cmd[i]; i++; }
    w[i] = 0;
    for (int k = 0; high[k]; k++) if (!strcmp(w, high[k])) return 2;
    static const char *const med[] = { "tune", "pkg", "export", "set", "unset", "renice", "write", "agent", "net", "run", "mv", "cp", "chmod", "mkdir", "cd", NULL };
    for (int k = 0; med[k]; k++) if (!strcmp(w, med[k])) return 1;
    return 0;
}
#define STEP(world, risk, ...) do { char c_[200]; ksnprintf(c_, sizeof c_, __VA_ARGS__); l += ksnprintf(o + l, n - l, "STEP %s %s %s\n", world, risk, c_); } while (0)
#define SAY(...) do { l += ksnprintf(o + l, n - l, "SAY "); l += ksnprintf(o + l, n - l, __VA_ARGS__); l += ksnprintf(o + l, n - l, "\n"); } while (0)

int aih_local_plan(const char *text, char *o, size_t n)
{
    char t[300], w[100]; i64 num; size_t l = 0;
    lower_copy(t, text, sizeof t);
    o[0] = 0;

    /* 1. the user typed a command from another world: translate it (the "polyglot" parser) */
    char first[24]; size_t fi = 0;
    while (t[fi] == ' ') fi++;
    size_t fs = fi; while (t[fi] && t[fi] != ' ' && fi - fs + 1 < sizeof first) { first[fi - fs] = t[fi]; fi++; }
    first[fi - fs] = 0;
    const char *rest = text + fi; while (*rest == ' ') rest++;
    struct { const char *win; const char *fmt; const char *note; } xl[] = {
        { "dir", "ls %s", "Windows 'dir' -> NEOS 'ls'" }, { "type", "cat %s", "Windows 'type' -> NEOS 'cat'" }, { "cls", "clear", "Windows 'cls' -> NEOS 'clear'" },
        { "ipconfig", "net", "Windows 'ipconfig' -> NEOS 'net'" }, { "tasklist", "ps", "Windows 'tasklist' -> NEOS 'ps'" }, { "taskkill", "pkill %s", "Windows 'taskkill' -> NEOS 'pkill'" },
        { "ver", "about", "Windows 'ver' -> NEOS 'about'" }, { "del", "rm %s", "Windows 'del' -> NEOS 'rm'" }, { "erase", "rm %s", "Windows 'erase' -> NEOS 'rm'" },
        { "ifconfig", "net", "Linux 'ifconfig' -> NEOS 'net'" }, { "top", "ps | sort cpu% desc | head 10", "Linux 'top' -> NEOS 'ps' (pin it for a live view)" },
        { "free", "mem", "Linux 'free' -> NEOS 'mem'" }, { "lsmod", "mods", "Linux 'lsmod' -> NEOS 'mods'" }, { "sysctl", "tune list %s", "Linux 'sysctl' -> NEOS 'tune'" },
        { "clear", "clear", "" }, { "history", "history", "" }, { "reboot", "reboot", "" }, { "poweroff", "poweroff", "" }, { "shutdown", "poweroff", "" }, { NULL, NULL, NULL } };
    for (int i = 0; xl[i].win; i++) if (!strcmp(first, xl[i].win) && (rest[0] || !strstr_pct(xl[i].fmt))) {
        char c[200]; ksnprintf(c, sizeof c, xl[i].fmt, rest);
        while (strlen(c) && c[strlen(c) - 1] == ' ') c[strlen(c) - 1] = 0;
        if (xl[i].note[0]) SAY("%s.", xl[i].note); else SAY("Comando NEOS equivalente.");
        STEP("neos", risk_of(c) == 2 ? "high" : risk_of(c) == 1 ? "medium" : "low", "%s", c);
        return 0;
    }
    static const char *const lin[] = { "grep", "egrep", "awk", "sed", "find", "cut", "sort", "uniq", "wc", "head", "tail", "tr", "xargs", "tee", "cp", "mv", "ln", "chmod", "touch", "mkdir", "date", "env", "uname", "id", "which", "basename", "dirname", "tar", "gzip", "diff", "od", "md5sum", "sha256sum", "seq", "sleep", "expr", "test", NULL };
    for (int i = 0; lin[i]; i++) if (!strcmp(first, lin[i])) {
        SAY("'%s' e' un comando Linux: lo eseguo con la shell Linux (BusyBox).", first);
        STEP("linux", risk_of(first) == 2 ? "high" : "medium", "%s", text);
        return 0;
    }
    if (!strncmp(t, "echo %", 6) || !strncmp(t, "set ", 4) || has(t, "%path%") || has(t, "%userprofile%")) {
        SAY("Variabile in stile Windows: il bus condiviso la rende in entrambi i mondi.");
        STEP("neos", "low", "echo %s", rest[0] ? text + 5 : "%PATH%");
        return 0;
    }

    /* 2. natural language (Italian / English) */
    if (hasany(t, "memoria|memory|ram|mem libera")) { SAY("Mostro l'uso della memoria fisica e dell'heap del kernel."); STEP("neos", "low", "mem"); return 0; }
    if (hasany(t, "latenza|jitter|real-time|realtime|real time|tempo reale|determinis")) { SAY("Statistiche in tempo reale: jitter del tick, latenza di risveglio, costo del timer."); STEP("neos", "low", "rt"); return 0; }
    if (hasany(t, "ottimizz|optimi|migliora le prestazioni|tune the system|make it faster")) {
        SAY("Creo un agente 'tuner' con permessi limitati e gli faccio cercare i valori migliori (tutto resta annullabile con 'agent rollback tuner 99').");
        STEP("neos", "medium", "agent add tuner native");
        STEP("neos", "medium", "agent grant tuner observe experiment tune:timer.hz[250..4000],sched.quantum[2..20],net.poll_ms[1..5]");
        STEP("neos", "medium", "agent tuner run rt.cost");
        return 0;
    }
    if (hasany(t, "processi|processes|thread|cpu|carico|quali programmi|what is running|cosa sta girando")) { SAY("Elenco i thread ordinati per uso CPU; con '| pin' resta aggiornato sullo schermo."); STEP("neos", "low", "ps | sort cpu%% desc | head 10"); return 0; }
    if (hasany(t, "variabili|variables|ambiente|environment") && !has(t, "imposta")) { SAY("Le variabili condivise, mostrate sia in forma Linux sia in forma Windows."); STEP("neos", "low", "vars | cols name linux windows"); return 0; }
    if (word_after(text, t, "ping ", w, sizeof w) && !has(t, "spiega")) { SAY("Invio richieste ICMP a %s.", w); STEP("neos", "low", "ping %s", w); return 0; }
    if (hasany(t, "rete|network|indirizzo ip|ip address|internet|connessione") && !hasany(t, "installa|install")) { SAY("Stato della rete (indirizzo, gateway, DNS, pacchetti)."); STEP("neos", "low", "net"); return 0; }
    if (hasany(t, "installa|install|scarica il pacchetto|get package")) {
        const char *kw = has(t, "installa") ? "installa " : "install ";
        if (word_after(text, t, kw, w, sizeof w) || word_after(text, t, "pacchetto ", w, sizeof w) || word_after(text, t, "package ", w, sizeof w)) {
            SAY("Aggiorno l'indice dei pacchetti e installo '%s' (verifica SHA-256; serve pkg.repo raggiungibile).", w);
            STEP("neos", "medium", "pkg update"); STEP("neos", "medium", "pkg install %s", w); return 0;
        }
    }
    if (hasany(t, "quantum") && first_int(t, &num)) { SAY("Imposto la fetta di tempo del round-robin a %ld tick (effetto immediato).", num); STEP("neos", "medium", "tune set sched.quantum %ld", num); return 0; }
    if (hasany(t, "frequenza del timer|timer hz|tick rate|hz") && first_int(t, &num)) { SAY("Imposto la frequenza del tick a %ld Hz.", num); STEP("neos", "medium", "tune set timer.hz %ld", num); return 0; }
    if (hasany(t, "impostazioni|settings|tunable|parametri|configurazione")) { SAY("Tutte le impostazioni regolabili, con valore, default e modalita' (live/boot)."); STEP("neos", "low", "tune list"); return 0; }
    if (hasany(t, "windows") && hasany(t, "percorso|path")) {
        if (word_after(text, t, "percorso ", w, sizeof w) || word_after(text, t, "path ", w, sizeof w)) { SAY("Traduco il percorso nell'altro mondo."); STEP("neos", "low", "path %s %s", w[0] == '/' ? "win" : "lin", w); return 0; }
    }
    if (hasany(t, "calcola|calculate|compute") ) {
        const char *e2 = strstr_after(text, "calcola"); if (!e2) e2 = strstr_after(text, "calculate"); if (!e2) e2 = strstr_after(text, "compute");
        if (e2 && *e2) { SAY("Uso il linguaggio Nio per calcolarlo."); STEP("nio", "low", "println(%s)", e2); return 0; }
    }
    if (hasany(t, "elenca|list|mostra i file|show files|cosa c'e|contenuto di") && (word_after(text, t, " in ", w, sizeof w) || word_after(text, t, " di ", w, sizeof w) || word_after(text, t, "elenca ", w, sizeof w) || (w[0] = 0, 1))) {
        if (!w[0] || !strchr(w, '/')) strlcpy(w, "/", sizeof w);
        SAY("Elenco il contenuto di %s.", w); STEP("neos", "low", "ls %s", w); return 0;
    }
    if (hasany(t, "mostra il file|show file|leggi|read file|apri il file|cat ")) {
        if (word_after(text, t, "file ", w, sizeof w) || word_after(text, t, "leggi ", w, sizeof w)) { SAY("Mostro il contenuto di %s.", w); STEP("neos", "low", "cat %s", w); return 0; }
    }
    if (hasany(t, "cancella|elimina|delete|remove file|rimuovi il file")) {
        if (word_after(text, t, "file ", w, sizeof w) || word_after(text, t, "cancella ", w, sizeof w) || word_after(text, t, "elimina ", w, sizeof w) || word_after(text, t, "delete ", w, sizeof w)) { SAY("Cancello %s. Operazione irreversibile: chiedo conferma esplicita.", w); STEP("neos", "high", "rm %s", w); return 0; }
    }
    if (hasany(t, "uccidi|termina il processo|kill process|ferma il processo") && first_int(t, &num)) { SAY("Termino il processo %ld.", num); STEP("neos", "high", "pkill %ld", num); return 0; }
    if (hasany(t, "spegni|shutdown|power off|spegnere")) { SAY("Spengo la macchina."); STEP("neos", "high", "poweroff"); return 0; }
    if (hasany(t, "riavvia|reboot|restart")) { SAY("Riavvio la macchina (applica le impostazioni 'boot')."); STEP("neos", "high", "reboot"); return 0; }
    if (hasany(t, "esegui|avvia|run |launch|lancia")) {
        if (word_after(text, t, "esegui ", w, sizeof w) || word_after(text, t, "avvia ", w, sizeof w) || word_after(text, t, "run ", w, sizeof w) || word_after(text, t, "lancia ", w, sizeof w) || word_after(text, t, "launch ", w, sizeof w)) {
            const char *world = (strlen(w) > 4 && !strcasecmp(w + strlen(w) - 4, ".exe")) ? "windows" : "neos";
            SAY("Avvio %s (%s).", w, !strcmp(world, "windows") ? "programma Windows" : "programma Linux o NEOS");
            STEP(world, "medium", "%s%s", !strcmp(world, "neos") ? "run " : "", w); return 0;
        }
    }
    if (hasany(t, "chi sei|cos'e neos|what is neos|versione|version|about")) { SAY("NEOS: sistema operativo real-time con agenti AI nel kernel, programmi Linux e Windows, linguaggio Nio."); STEP("neos", "low", "about"); return 0; }
    if (hasany(t, "aiuto|help|come si|come faccio|how do i|cosa posso|what can")) {
        SAY("Posso: mostrare memoria/CPU/rete, cambiare impostazioni, installare pacchetti, eseguire programmi Linux/Windows, tradurre percorsi, ottimizzare il sistema. Scrivi cosa vuoi fare.");
        STEP("neos", "low", "help | head 25"); return 0;
    }
    SAY("Non ho capito la richiesta. Prova a dire cosa vuoi fare (es. 'mostra la memoria', 'installa hello', 'imposta il quantum a 5', 'elenca i file in /bin'). Con un agente AI collegato le richieste libere vengono interpretate dall'AI.");
    return 0;
}

/* ------------------------------------------------------------ request/answer plumbing */
int aih_request(const char *text, char *plan, size_t n, char *from, size_t fn)
{
    if (aih_planner_active()) {
        struct req *r = NULL;
        u64 fl = irq_save();
        for (int i = 0; i < QN; i++) if (reqs[i].state == R_FREE) { r = &reqs[i]; break; }
        if (r) { r->state = 3; irq_restore(fl); } else irq_restore(fl);
        if (r) {
            r->id = next_id++; r->pushed = 0; r->plan[0] = 0;
            strlcpy(r->text, text, sizeof r->text); build_context(r->ctx, sizeof r->ctx);
            r->state = R_PENDING;
            u64 until = ticks + ms_to_ticks((u64)t_timeout->val * 1000);
            while (r->state == R_PENDING && ticks < until) thread_sleep_ms(50);
            if (r->state == R_ANSWERED) {
                strlcpy(plan, r->plan, n); strlcpy(from, r->from, fn);
                r->state = R_FREE;
                return 0;
            }
            r->state = R_FREE;
            planner_idx = -1;                              /* silent: treat it as gone until it announces itself again */
        }
    }
    strlcpy(from, "local", fn);
    return aih_local_plan(text, plan, n);
}

int aih_event_next(int *id, char *text, size_t tn, char *ctx, size_t cn)
{
    for (int i = 0; i < QN; i++) if (reqs[i].state == R_PENDING && !reqs[i].pushed) {
        reqs[i].pushed = 1; *id = reqs[i].id;
        strlcpy(text, reqs[i].text, tn); strlcpy(ctx, reqs[i].ctx, cn);
        return 0;
    }
    return -1;
}

int aih_answer(int id, const char *plan, const char *from)
{
    for (int i = 0; i < QN; i++) if (reqs[i].state == R_PENDING && reqs[i].id == id) {
        strlcpy(reqs[i].plan, plan, sizeof reqs[i].plan); strlcpy(reqs[i].from, from, sizeof reqs[i].from);
        reqs[i].state = R_ANSWERED;
        return 0;
    }
    return -1;
}
