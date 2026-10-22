#pragma once
#include "kernel.h"

/*
 * NEOS agents. An agent (an in-kernel controller or an external AI model attached over the
 * agent channel) is a first-class principal with an identity, capabilities scoped to names /
 * paths / value ranges, a per-risk approval policy, a rate limit, an audit trail and a journal
 * that can undo what it changed. Every agent action goes through agent_call() - the only gate.
 * Capabilities are granted by the human operator only; an agent can never widen its own.
 */
enum { CAP_OBSERVE, CAP_TUNE, CAP_THREAD, CAP_VARS, CAP_FSR, CAP_FSW, CAP_PKG, CAP_NET, CAP_DEBUG, CAP_EXP, CAP_ADMIN, NCAPS };
extern const char *const cap_names[NCAPS];
enum { RISK_READ, RISK_REV, RISK_IRREV };
enum { POL_ALLOW, POL_ASK, POL_DENY };
enum { AK_NATIVE, AK_REMOTE };
enum { AS_ACTIVE, AS_SUSPENDED };

#define AG_MAX     32
#define AG_JOURNAL 32

struct jent { u8 kind; int a; char name[48]; char old[96]; u8 *blob; size_t blen; int had; };
struct agent {
    int used;
    char name[16];
    u8 kind, state;
    u32 caps;
    char scope[NCAPS][96];
    u8 policy[3];
    char token_hash[65];
    u64 tokens_ms, last_ms;               /* rate limiter: token bucket in milli-tokens */
    u64 calls, denied, asked;
    size_t fs_used;
    struct jent jr[AG_JOURNAL];
    int jn;
};

struct jbuf { char *b; size_t n, len; };
void jp(struct jbuf *j, const char *fmt, ...);
void jstr(struct jbuf *j, const char *s);

struct tool {
    const char *name, *params, *desc;
    u8 cap, risk;
    int (*check)(struct agent *a, int argc, char **argv, const char **why);   /* scope check, before any approval */
    int (*run)(struct agent *a, int argc, char **argv, struct jbuf *out);     /* 0 = ok (out = JSON), else code (out = message) */
};
extern const struct tool agent_tools[];
extern const int agent_ntools;

/* registry (operator side) */
void agent_init(void);
struct agent *agent_find(const char *name);
struct agent *agent_add(const char *name, int kind, const char *secret, char *token_out, size_t tn, const char **err);
int  agent_remove(const char *name);
int  agent_grant(struct agent *a, const char *cap, const char *scope, const char **err);   /* scope NULL = whole capability */
int  agent_revoke(struct agent *a, const char *cap);
int  agent_cap_index(const char *name);
int  agent_authenticate(struct agent *a, const char *token);
struct agent *agent_at(int i);
void agent_freeze(void);
void agent_thaw(void);
int  agent_is_frozen(void);
int  agent_index(const struct agent *a);

/* gate */
int  agent_call(struct agent *a, int argc, char **argv, struct jbuf *out, int operator_approved);

/* scope helpers used by the tools */
int  scope_allows(const struct agent *a, int cap, const char *target, int has_val, i64 val);
int  scope_minprio(const struct agent *a);

/* approvals */
struct approval { int id; int agent; char tool[24]; char args[64]; volatile int state; u64 tick; };
int  approval_count_pending(void);
int  approval_get(int idx, struct approval *out);
int  approval_decide(int id, int allow);

/* audit */
struct audit_rec { u64 seq, tick; u8 agent; char tool[24]; char args[56]; int rc; u8 approver; char hash[65]; };
int  audit_count(void);
int  audit_get(int back, struct audit_rec *out);       /* 0 = newest */
int  audit_verify(void);                               /* 0 = chain intact */
void audit_add(int agent, const char *tool, const char *args, int rc, int approver);

/* journal */
void journal_push(struct agent *a, u8 kind, int ai, const char *name, const char *old, const u8 *blob, size_t blen, int had);
int  journal_rollback(struct agent *a, int n, struct jbuf *out);
enum { J_TUNE, J_VAR, J_PRIO, J_FS, J_PKG };

/* metrics used by experiments and the tuner */
int  metric_value(const char *name, i64 *out);
void metric_reset(void);

/* channel + native tuner */
void agentd_start(void);
void agent_tuner_run(const char *metric);

/* shared by tools/channel */
extern volatile int agents_frozen;
int  agent_exec_line(struct agent *a, char *line, struct jbuf *out, int operator_approved);
