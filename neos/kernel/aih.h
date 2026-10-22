#pragma once
#include "kernel.h"
#define AIH_PLAN_MAX 2048
void aih_init(void);
int  aih_autorun_low(void);
void aih_planner_set(int agent_idx);
int  aih_planner_active(void);
int  aih_planner_idx(void);
int  aih_local_plan(const char *text, char *plan, size_t n);
int  aih_request(const char *text, char *plan, size_t n, char *from, size_t fn);   /* blocking: AI planner or local fallback */
int  aih_event_next(int *id, char *text, size_t tn, char *ctx, size_t cn);          /* for the agent channel */
int  aih_answer(int id, const char *plan, const char *from);
