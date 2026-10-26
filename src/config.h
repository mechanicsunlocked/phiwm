/* phiwm — config parser for a hyprland.conf-style file (platform independent) */
#ifndef PHI_CONFIG_H
#define PHI_CONFIG_H

#include "layout.h"

#define PHI_MAX_BINDS 200
#define PHI_MAX_RULES 64
#define PHI_MAX_EXEC  16
#define PHI_MAX_ERRS  8
#define PHI_MAX_MATCH 3

enum { PHI_MOD_SUPER = 1, PHI_MOD_SHIFT = 2, PHI_MOD_CTRL = 4, PHI_MOD_ALT = 8 };

typedef struct {
    unsigned mods;
    unsigned vk;        /* Windows virtual-key code, 0 when bound by position */
    unsigned scan;      /* set-1 scancode for code:N binds */
    int      repeat;    /* binde: fire again while held */
    char     disp[32];  /* dispatcher name */
    char     arg[256];
} PhiBind;

enum { PHI_FIELD_CLASS = 1, PHI_FIELD_TITLE, PHI_FIELD_EXE };
enum { PHI_RULE_FLOAT = 1, PHI_RULE_TILE, PHI_RULE_WORKSPACE, PHI_RULE_PSEUDO };

typedef struct {
    int action;
    int ws;             /* 1-based, PHI_RULE_WORKSPACE */
    int silent;
    int nmatch;
    struct { int field; char pat[160]; } m[PHI_MAX_MATCH];
} PhiRule;

typedef struct {
    int      gaps_in, gaps_out;
    unsigned active_border, inactive_border;   /* 0xRRGGBB */
    int      has_active, has_inactive;
    int      rounding;
    int      follow_mouse;
    PhiDwindle dw;

    int     nbinds;
    PhiBind binds[PHI_MAX_BINDS];
    int     nrules;
    PhiRule rules[PHI_MAX_RULES];
    int     nexec_once;
    char    exec_once[PHI_MAX_EXEC][512];
    int     nexec;
    char    exec[PHI_MAX_EXEC][512];

    int     nerr;
    char    err[PHI_MAX_ERRS][200];
} PhiConfig;

void phi_config_defaults(PhiConfig *c);
/* Parses UTF-8 text into c (which should hold defaults). Returns error count. */
int  phi_config_parse(PhiConfig *c, const char *text);
/* Case-insensitive match of a compiled rule pattern ('*', '?', '|' alternatives). */
int  phi_pattern_match(const char *pat, const char *s);
int  phi_dispatcher_known(const char *name);

#endif
