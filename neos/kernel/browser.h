#pragma once
#include "kernel.h"

/* The NEOS browser engine: fetches pages (http://, file://, neos://), parses an HTML subset and lays it out into
 * a list of drawable items. The workspace (gui.c) draws the items and feeds mouse/keys back. */
enum { IT_TEXT, IT_RULE, IT_INPUT, IT_BTN, IT_BULLET };
enum { FT_TEXT, FT_PASS, FT_SUBMIT, FT_CHECK, FT_HIDDEN };
struct bitem { short x, y, w, h; u8 kind, scale, bold, ul; u32 col; const char *text; u8 len; short link, field; };
struct bfield { char name[24], value[64]; u8 type; u8 checked; short form; };

#define BR_URL 200
extern char br_urlbar[BR_URL];
extern int  br_focus;                        /* -2 address bar, -1 nothing, >=0 form field */
void br_init(void);
void br_layout(int width);
int  br_nitems(void);
const struct bitem *br_item(int i);
const struct bfield *br_field(int i);
int  br_doc_height(void);
const char *br_title(void);
const char *br_status(void);
int  br_loading(void);
int  br_tick(void);                          /* from the GUI thread: finish a finished load; non-zero = redraw needed */
void br_go(const char *url, int typed);      /* typed = entered by the person (may open neos:// from anywhere) */
void br_back(void); void br_forward(void); void br_reload(void);
void br_follow(int link);
void br_submit(int field);
void br_key(int c);                          /* edit the focused text */
void br_toggle(int field);
int  br_can_back(void); int br_can_forward(void);
