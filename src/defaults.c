/* phiwm — built-in settings. See defaults.h. */
#include "defaults.h"
#include <ctype.h>
#include <string.h>

/* Dwindle: every new window takes half of the focused one, clockwise around the
 * spiral, and a split keeps its direction once made. */
const PhiDwindle phi_dwindle = { 0.5, 1, 1.0 };

/* ---------- keybinds ---------- */

/* virtual keys (winuser.h values) and set-1 scancodes */
enum {
    K_RETURN = 0x0D, K_TAB = 0x09,
    K_LEFT = 0x25, K_UP = 0x26, K_RIGHT = 0x27, K_DOWN = 0x28,
    K_MINUS = 0xBD, K_EQUAL = 0xBB,                       /* VK_OEM_MINUS, VK_OEM_PLUS */
    SC_1 = 2, SC_2, SC_3, SC_4, SC_5, SC_6, SC_7, SC_8, SC_9, SC_0   /* the digit row */
};

#define S   PHI_MOD_SUPER
#define SS  (PHI_MOD_SUPER | PHI_MOD_SHIFT)
#define SC  (PHI_MOD_SUPER | PHI_MOD_CTRL)
#define SCS (PHI_MOD_SUPER | PHI_MOD_CTRL | PHI_MOD_SHIFT)
#define SA  (PHI_MOD_SUPER | PHI_MOD_ALT)
#define SAS (PHI_MOD_SUPER | PHI_MOD_ALT | PHI_MOD_SHIFT)

#define KEY(mods, vk, disp, arg)   { mods, vk, 0, 0, disp, arg }   /* by virtual key */
#define KEYE(mods, vk, disp, arg)  { mods, vk, 0, 1, disp, arg }   /* same, repeats while held */
#define POS(mods, sc, disp, arg)   { mods, 0, sc, 0, disp, arg }   /* by key position */

const PhiBind phi_binds[] = {
    /* apps */
    KEY(S, K_RETURN, "exec", "wt.exe"),
    KEY(S, 'B',      "exec", "msedge.exe"),
    KEY(S, 'E',      "exec", "explorer.exe"),

    /* windows */
    KEY(S,  'W', "killactive", ""),
    KEY(S,  'J', "togglesplit", ""),
    KEY(S,  'P', "pseudo", ""),
    KEY(S,  'V', "togglefloating", ""),     /* replaces Win+V clipboard history */
    KEY(S,  'F', "fullscreen", "0"),
    KEY(SS, 'F', "fullscreen", "1"),

    KEY(S, K_LEFT,  "movefocus", "l"),
    KEY(S, K_RIGHT, "movefocus", "r"),
    KEY(S, K_UP,    "movefocus", "u"),
    KEY(S, K_DOWN,  "movefocus", "d"),

    KEY(SS, K_LEFT,  "swapwindow", "l"),
    KEY(SS, K_RIGHT, "swapwindow", "r"),
    KEY(SS, K_UP,    "swapwindow", "u"),
    KEY(SS, K_DOWN,  "swapwindow", "d"),

    KEYE(S,  K_MINUS, "resizeactive", "-100 0"),
    KEYE(S,  K_EQUAL, "resizeactive", "100 0"),
    KEYE(SS, K_MINUS, "resizeactive", "0 -100"),
    KEYE(SS, K_EQUAL, "resizeactive", "0 100"),

    KEY(SA,  K_TAB, "cyclenext", ""),
    KEY(SAS, K_TAB, "cyclenext", "prev"),

    /* workspaces: the digit row by position, so it works on every layout */
    POS(S, SC_1, "workspace", "1"),
    POS(S, SC_2, "workspace", "2"),
    POS(S, SC_3, "workspace", "3"),
    POS(S, SC_4, "workspace", "4"),
    POS(S, SC_5, "workspace", "5"),
    POS(S, SC_6, "workspace", "6"),
    POS(S, SC_7, "workspace", "7"),
    POS(S, SC_8, "workspace", "8"),
    POS(S, SC_9, "workspace", "9"),
    POS(S, SC_0, "workspace", "10"),

    POS(SS, SC_1, "movetoworkspace", "1"),
    POS(SS, SC_2, "movetoworkspace", "2"),
    POS(SS, SC_3, "movetoworkspace", "3"),
    POS(SS, SC_4, "movetoworkspace", "4"),
    POS(SS, SC_5, "movetoworkspace", "5"),
    POS(SS, SC_6, "movetoworkspace", "6"),
    POS(SS, SC_7, "movetoworkspace", "7"),
    POS(SS, SC_8, "movetoworkspace", "8"),
    POS(SS, SC_9, "movetoworkspace", "9"),
    POS(SS, SC_0, "movetoworkspace", "10"),

    KEY(S,  K_TAB, "workspace", "e+1"),
    KEY(SS, K_TAB, "workspace", "e-1"),

    /* phiwm */
    KEY(SC,  'R', "refresh", ""),
    KEY(SCS, 'Q', "exit", ""),
};
const int phi_nbinds = sizeof phi_binds / sizeof phi_binds[0];

/* ---------- window rules ---------- */

const PhiRule phi_rules[] = {
    { PHI_RULE_FLOAT, 0, 0, PHI_FIELD_TITLE, "Picture-in-Picture|Picture in picture" },
    { PHI_RULE_FLOAT, 0, 0, PHI_FIELD_EXE,   "CalculatorApp.exe" },
    { PHI_RULE_FLOAT, 0, 0, PHI_FIELD_CLASS, "OperationStatusWindow" },     /* Explorer copy dialog */
    { PHI_RULE_FLOAT, 0, 0, PHI_FIELD_EXE,   "PowerToys.*" },
};
const int phi_nrules = sizeof phi_rules / sizeof phi_rules[0];

/* ---------- helpers ---------- */

static const char *dispatchers[] = {
    "exec", "killactive", "movefocus", "movewindow", "swapwindow", "togglesplit",
    "togglefloating", "pseudo", "fullscreen", "workspace", "movetoworkspace",
    "movetoworkspacesilent", "focusmonitor", "splitratio", "resizeactive",
    "cyclenext", "layoutmsg", "refresh", "exit", NULL
};

int phi_dispatcher_known(const char *name)
{
    for (int i = 0; dispatchers[i]; i++)
        if (!strcmp(dispatchers[i], name)) return 1;
    return 0;
}

static int glob(const char *p, const char *pe, const char *s)
{
    while (p < pe) {
        if (*p == '*') {
            p++;
            if (p == pe) return 1;
            for (; *s; s++) if (glob(p, pe, s)) return 1;
            return glob(p, pe, s);
        }
        if (!*s) return 0;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
        p++; s++;
    }
    return *s == 0;
}

int phi_pattern_match(const char *pat, const char *s)
{
    if (!s) s = "";
    const char *start = pat;
    for (;;) {
        const char *bar = strchr(start, '|');
        const char *end = bar ? bar : start + strlen(start);
        if (glob(start, end, s)) return 1;
        if (!bar) return 0;
        start = bar + 1;
    }
}
