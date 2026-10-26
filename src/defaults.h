/* phiwm — built-in settings: keybinds, window rules and layout constants.
 *
 * There is no config file: this is the whole configuration. Plain C99 and
 * no Windows headers, so tests/test_defaults.c can check it on any machine.
 */
#ifndef PHI_DEFAULTS_H
#define PHI_DEFAULTS_H

#include "layout.h"

#define PHI_GAPS_IN          5          /* px between windows (at 96 dpi) */
#define PHI_GAPS_OUT         10         /* px to the screen edge */
#define PHI_ACTIVE_BORDER    0x33ccff   /* 0xRRGGBB, drawn natively by Windows 11 */
#define PHI_INACTIVE_BORDER  0x595959
#define PHI_ROUNDING         1          /* Windows 11 rounded corners */
#define PHI_FOLLOW_MOUSE     1          /* hovering a window focuses it, like hyprland */

enum { PHI_MOD_SUPER = 1, PHI_MOD_SHIFT = 2, PHI_MOD_CTRL = 4, PHI_MOD_ALT = 8 };

typedef struct {
    unsigned    mods;
    unsigned    vk;      /* Windows virtual-key code, 0 when bound by position */
    unsigned    scan;    /* set-1 scancode: a key position, the same on every layout */
    int         repeat;  /* fires again while the key is held */
    const char *disp;    /* dispatcher name, see dispatch() in wm.c */
    const char *arg;
} PhiBind;

enum { PHI_FIELD_CLASS = 1, PHI_FIELD_TITLE, PHI_FIELD_EXE };
enum { PHI_RULE_FLOAT = 1, PHI_RULE_TILE, PHI_RULE_WORKSPACE, PHI_RULE_PSEUDO };

typedef struct {
    int         action;
    int         ws;      /* 1-based, PHI_RULE_WORKSPACE only */
    int         silent;  /* PHI_RULE_WORKSPACE: don't switch to it */
    int         field;   /* what `pat` is matched against */
    const char *pat;     /* glob: '*', '?', '|' alternatives; case-insensitive, whole string */
} PhiRule;

extern const PhiDwindle phi_dwindle;
extern const PhiBind    phi_binds[];
extern const int        phi_nbinds;
extern const PhiRule    phi_rules[];
extern const int        phi_nrules;

int phi_pattern_match(const char *pat, const char *s);
int phi_dispatcher_known(const char *name);

#endif
