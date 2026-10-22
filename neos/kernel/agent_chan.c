#include "agent.h"
#include "shell.h"

/*
 * Agent channel "NAP/1" on the second serial port (COM2): a physical link, so no network exposure
 * and no TLS needed. One line in, one line out:
 *   HELLO                        -> OK NAP/1 NEOS <version>
 *   AUTH <agent> <token>         -> OK auth <agent>        | ERR 401
 *   CALL <seq> <tool> [args...]  -> OK <seq> <json>        | ERR <seq> <code> <message>
 *   BYE
 * Arguments use shell-style quoting. The token is stored only as a SHA-256 in the kernel.
 */
#define COM2 0x2F8
static int present;
static struct tunable *t_enable, *t_poll;

static void com2_putc(char c)
{
    for (int i = 0; i < 20000 && !(inb(COM2 + 5) & 0x20); i++) { }
    outb(COM2, (u8)c);
}
static void com2_puts(const char *s) { while (*s) com2_putc(*s++); }
static int com2_getc(void) { return (inb(COM2 + 5) & 1) ? inb(COM2) : -1; }

static void reply(const char *kind, const char *seq, const char *body)
{
    com2_puts(kind); com2_putc(' ');
    if (seq) { com2_puts(seq); com2_putc(' '); }
    com2_puts(body); com2_puts("\r\n");
}

static void agentd(void *arg)
{
    (void)arg;
    static char line[1536], outbuf[4096];
    struct jbuf out = { outbuf, sizeof outbuf, 0 };
    struct agent *cur = NULL;
    int len = 0, fails = 0; u64 locked_until = 0;
    for (;;) {
        int c = com2_getc();
        if (c < 0) { thread_sleep_ms((u64)t_poll->val); continue; }
        if (c == '\r') continue;
        if (c != '\n') { if (len < (int)sizeof line - 1) line[len++] = (char)c; else len = -1000000; continue; }
        if (len < 0) { reply("ERR", NULL, "400 line too long"); len = 0; continue; }
        line[len] = 0; len = 0;
        char *argv[16];
        char copy[1536]; strlcpy(copy, line, sizeof copy);
        int argc = shell_tokenize(copy, argv, 16);
        if (!argc) continue;

        if (!strcmp(argv[0], "HELLO")) { ksnprintf(outbuf, sizeof outbuf, "NAP/1 %s %s", NEOS_NAME, NEOS_VERSION); reply("OK", NULL, outbuf); }
        else if (!strcmp(argv[0], "AUTH") && argc >= 3) {
            if (ticks < locked_until) { reply("ERR", NULL, "429 too many failures, try later"); continue; }
            struct agent *a = agent_find(argv[1]);
            if (a && a->kind == AK_REMOTE && !agent_authenticate(a, argv[2])) {
                cur = a; fails = 0;
                audit_add(agent_index(a), "auth", "ok", 0, 0);
                ksnprintf(outbuf, sizeof outbuf, "auth %s", a->name); reply("OK", NULL, outbuf);
            } else {
                cur = NULL;
                audit_add(0, "auth", "failed", 401, 0);
                thread_sleep_ms(500);                                     /* slow down guessing */
                if (++fails >= 5) { locked_until = ticks + ms_to_ticks(30000); fails = 0; }
                reply("ERR", NULL, "401 authentication failed");
            }
        }
        else if (!strcmp(argv[0], "BYE")) { cur = NULL; reply("OK", NULL, "bye"); }
        else if (!strcmp(argv[0], "CALL") && argc >= 3) {
            if (!cur || !cur->used) { reply("ERR", argv[1], "401 not authenticated"); continue; }
            int rc = agent_call(cur, argc - 2, argv + 2, &out, 0);
            if (rc == 0) reply("OK", argv[1], outbuf);
            else { char m[300]; ksnprintf(m, sizeof m, "%d %s", rc, outbuf); reply("ERR", argv[1], m); }
        }
        else reply("ERR", NULL, "400 unknown command");
    }
}

void agentd_start(void)
{
    module_register("agentd", "1.0", "agent channel NAP/1 on COM2 (serial, token auth, one line per call)");
    t_enable = tune_bool("agent", "channel", "accept external agents on COM2", TF_BOOT | TF_NOAGENT, 1, NULL);
    t_poll   = tune_int ("agent", "channel_poll_ms", "COM2 polling interval", TF_LIVE | TF_NOAGENT, 2, 1, 100, NULL);
    outb(COM2 + 7, 0xAE);
    present = inb(COM2 + 7) == 0xAE;                  /* scratch register works only if a UART exists */
    if (!present || !t_enable->val) return;
    outb(COM2 + 1, 0x00); outb(COM2 + 3, 0x80); outb(COM2 + 0, 0x01); outb(COM2 + 1, 0x00);
    outb(COM2 + 3, 0x03); outb(COM2 + 2, 0xC7); outb(COM2 + 4, 0x0B);
    while (inb(COM2 + 5) & 1) inb(COM2);
    thread_create("agentd", 4, agentd, NULL);
}
