/* phiwm — hyprland.conf-style config parser. Plain C99, no Windows headers,
 * so it can be unit-tested on any machine. Virtual-key values match winuser.h. */
#include "config.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VARS 64
#define MAX_DEPTH 8

typedef struct { char name[48]; char value[256]; } Var;

typedef struct {
    PhiConfig *c;
    Var vars[MAX_VARS];
    int nvars;
    char section[MAX_DEPTH][48];
    int depth;
    int line;
} Parser;

static const char *dispatchers[] = {
    "exec", "killactive", "movefocus", "movewindow", "swapwindow", "togglesplit",
    "togglefloating", "pseudo", "fullscreen", "workspace", "movetoworkspace",
    "movetoworkspacesilent", "focusmonitor", "splitratio", "resizeactive",
    "cyclenext", "layoutmsg", "exit", "reload", NULL
};

int phi_dispatcher_known(const char *name)
{
    for (int i = 0; dispatchers[i]; i++)
        if (!strcmp(dispatchers[i], name)) return 1;
    return 0;
}

void phi_config_defaults(PhiConfig *c)
{
    memset(c, 0, sizeof *c);
    c->gaps_in = 5;
    c->gaps_out = 10;
    c->rounding = 1;
    c->follow_mouse = 0;
    c->dw.ratio = 0.618;
    c->dw.force_split = 2;
    c->dw.preserve_split = 1;
    c->dw.width_mult = 1.0;
}

/* ---------- small string helpers ---------- */

static void copy(char *dst, size_t n, const char *src)
{
    if (!n) return;
    size_t l = strlen(src);
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = 0;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static int istarts(const char *s, const char *pre)
{
    for (; *pre; s++, pre++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return 0;
    return 1;
}

static void error(Parser *p, const char *fmt, ...)
{
    PhiConfig *c = p->c;
    if (c->nerr >= PHI_MAX_ERRS) { c->nerr++; return; }
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(c->err[c->nerr], sizeof c->err[0], "line %d: %s", p->line, msg);
    c->nerr++;
}

/* Split at the first `n` commas; the last field keeps the rest (commas included). */
static int split_commas(char *s, char **out, int n)
{
    int k = 0;
    out[k++] = s;
    while (k < n) {
        char *comma = strchr(s, ',');
        if (!comma) break;
        *comma = 0;
        s = comma + 1;
        out[k++] = s;
    }
    for (int i = 0; i < k; i++) out[i] = trim(out[i]);
    return k;
}

static int parse_bool(const char *v)
{
    return ieq(v, "true") || ieq(v, "yes") || ieq(v, "on") || atoi(v) != 0;
}

/* ---------- colors ---------- */

static int hexval(const char *s, int len, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < len; i++) {
        int ch = tolower((unsigned char)s[i]), d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        else return 0;
        v = v * 16 + (unsigned)d;
    }
    *out = v;
    return 1;
}

/* Accepts rgba(RRGGBBAA), rgb(RRGGBB), rgba(r,g,b,a), rgb(r,g,b), 0xAARRGGBB.
 * Gradients ("a b 45deg") use the first color. Returns 0xRRGGBB. */
static int parse_color(const char *v, unsigned *out)
{
    while (isspace((unsigned char)*v)) v++;
    unsigned x;
    if (istarts(v, "rgba(") || istarts(v, "rgb(")) {
        const char *in = strchr(v, '(') + 1;
        const char *close = strchr(in, ')');
        if (!close) return 0;
        int len = (int)(close - in);
        if (memchr(in, ',', (size_t)len)) {
            int r, g, b;
            if (sscanf(in, "%d , %d , %d", &r, &g, &b) != 3) return 0;
            *out = ((unsigned)(r & 255) << 16) | ((unsigned)(g & 255) << 8) | (unsigned)(b & 255);
            return 1;
        }
        if (len != 6 && len != 8) return 0;
        if (!hexval(in, 6, &x)) return 0;
        *out = x;
        return 1;
    }
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        size_t len = strspn(v + 2, "0123456789abcdefABCDEF");
        if (len == 8 && hexval(v + 4, 6, &x)) { *out = x; return 1; }
        if (len == 6 && hexval(v + 2, 6, &x)) { *out = x; return 1; }
    }
    return 0;
}

/* ---------- keys ---------- */

static const struct { const char *name; unsigned vk; } keynames[] = {
    { "return", 0x0D }, { "enter", 0x0D }, { "kp_enter", 0x0D }, { "space", 0x20 },
    { "tab", 0x09 }, { "escape", 0x1B }, { "esc", 0x1B }, { "backspace", 0x08 },
    { "delete", 0x2E }, { "insert", 0x2D }, { "home", 0x24 }, { "end", 0x23 },
    { "prior", 0x21 }, { "page_up", 0x21 }, { "pageup", 0x21 },
    { "next", 0x22 }, { "page_down", 0x22 }, { "pagedown", 0x22 },
    { "left", 0x25 }, { "up", 0x26 }, { "right", 0x27 }, { "down", 0x28 },
    { "print", 0x2C }, { "pause", 0x13 }, { "menu", 0x5D },
    { "minus", 0xBD }, { "equal", 0xBB }, { "plus", 0xBB }, { "comma", 0xBC },
    { "period", 0xBE }, { "slash", 0xBF }, { "semicolon", 0xBA }, { "apostrophe", 0xDE },
    { "grave", 0xC0 }, { "bracketleft", 0xDB }, { "bracketright", 0xDD },
    { "backslash", 0xDC }, { "less", 0xE2 },
    { "xf86audioraisevolume", 0xAF }, { "xf86audiolowervolume", 0xAE },
    { "xf86audiomute", 0xAD }, { "xf86audionext", 0xB0 }, { "xf86audioprev", 0xB1 },
    { "xf86audioplay", 0xB3 }, { "xf86audiopause", 0xB3 },
    { NULL, 0 }
};

/* Returns 1 = ok, 0 = unknown, -1 = valid but unsupported here (mouse). */
static int parse_key(const char *k, unsigned *vk, unsigned *scan)
{
    *vk = 0; *scan = 0;
    if (istarts(k, "mouse")) return -1;
    if (istarts(k, "code:")) {
        int code = atoi(k + 5);
        int sc = code - 8;               /* xkb keycode -> evdev == PC set-1 scancode */
        if (sc < 1 || sc > 0x58) return 0;
        *scan = (unsigned)sc;
        return 1;
    }
    if (strlen(k) == 1) {
        int ch = toupper((unsigned char)k[0]);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) { *vk = (unsigned)ch; return 1; }
    }
    if ((k[0] == 'f' || k[0] == 'F') && isdigit((unsigned char)k[1])) {
        int n = atoi(k + 1);
        if (n >= 1 && n <= 24) { *vk = 0x70u + (unsigned)(n - 1); return 1; }
    }
    if (istarts(k, "kp_") && isdigit((unsigned char)k[3]) && !k[4]) { *vk = 0x60u + (unsigned)(k[3] - '0'); return 1; }
    for (int i = 0; keynames[i].name; i++)
        if (ieq(k, keynames[i].name)) { *vk = keynames[i].vk; return 1; }
    return 0;
}

static unsigned parse_mods(const char *s)
{
    char up[128];
    size_t i = 0;
    for (; s[i] && i < sizeof up - 1; i++) up[i] = (char)toupper((unsigned char)s[i]);
    up[i] = 0;
    unsigned m = 0;
    if (strstr(up, "SUPER") || strstr(up, "WIN") || strstr(up, "LOGO") || strstr(up, "MOD4")) m |= PHI_MOD_SUPER;
    if (strstr(up, "SHIFT")) m |= PHI_MOD_SHIFT;
    if (strstr(up, "CTRL") || strstr(up, "CONTROL")) m |= PHI_MOD_CTRL;
    if (strstr(up, "ALT") || strstr(up, "MOD1")) m |= PHI_MOD_ALT;
    return m;
}

static void parse_bind(Parser *p, const char *kw, char *value)
{
    PhiConfig *c = p->c;
    const char *flags = kw + 4;   /* after "bind" */
    int has_desc = strchr(flags, 'd') != NULL;
    int repeat = strchr(flags, 'e') != NULL;
    if (strchr(flags, 'm')) return;  /* mouse binds: not supported (yet) */

    char *f[5] = { 0 };
    int n = split_commas(value, f, has_desc ? 5 : 4);
    int need = has_desc ? 4 : 3;
    if (n < need) { error(p, "%s needs MODS, key, dispatcher", kw); return; }
    const char *disp = f[has_desc ? 3 : 2];
    const char *arg = n > need ? f[need] : "";

    unsigned vk, scan;
    int k = parse_key(f[1], &vk, &scan);
    if (k < 0) return;               /* mouse buttons / wheel: silently skipped */
    if (k == 0) { error(p, "unknown key '%s'", f[1]); return; }
    if (!phi_dispatcher_known(disp)) { error(p, "unsupported dispatcher '%s'", disp); return; }
    if (c->nbinds >= PHI_MAX_BINDS) { error(p, "too many binds"); return; }

    PhiBind *b = &c->binds[c->nbinds];
    unsigned mods = parse_mods(f[0]);
    /* A later bind for the same combo replaces the earlier one, like re-binding in hyprland. */
    for (int i = 0; i < c->nbinds; i++)
        if (c->binds[i].mods == mods && c->binds[i].vk == vk && c->binds[i].scan == scan) { b = &c->binds[i]; break; }
    if (b == &c->binds[c->nbinds]) c->nbinds++;
    b->mods = mods;
    b->vk = vk;
    b->scan = scan;
    b->repeat = repeat;
    copy(b->disp, sizeof b->disp, disp);
    copy(b->arg, sizeof b->arg, arg);
}

/* ---------- window rules ---------- */

/* Turns a hyprland regex like ^(kitty|org\.foo\..*)$ into "kitty|org.foo.*". */
static void compile_pattern(const char *src, char *dst, size_t n)
{
    char tmp[256];
    copy(tmp, sizeof tmp, src);
    char *s = trim(tmp);
    size_t l = strlen(s);
    if (l && s[0] == '^') { s++; l--; }
    if (l && s[l - 1] == '$' && (l < 2 || s[l - 2] != '\\')) s[--l] = 0;
    if (l >= 2 && s[0] == '(' && s[l - 1] == ')') { s[l - 1] = 0; s++; l -= 2; }
    size_t o = 0;
    for (size_t i = 0; s[i] && o + 2 < n; i++) {
        if (s[i] == '\\' && s[i + 1]) { dst[o++] = s[++i]; continue; }
        if (s[i] == '.' && s[i + 1] == '*') { dst[o++] = '*'; i++; continue; }
        if (s[i] == '.' && s[i + 1] == '+') { dst[o++] = '?'; dst[o++] = '*'; i++; continue; }
        if (s[i] == '.') { dst[o++] = '?'; continue; }
        if (s[i] == '(' || s[i] == ')') continue;
        dst[o++] = s[i];
    }
    dst[o] = 0;
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

static void parse_rule(Parser *p, char *value)
{
    PhiConfig *c = p->c;
    char *f[1 + PHI_MAX_MATCH + 4] = { 0 };
    int n = split_commas(value, f, 1 + PHI_MAX_MATCH + 3);
    if (n < 2) { error(p, "windowrule needs a rule and a match"); return; }

    PhiRule r;
    memset(&r, 0, sizeof r);
    char act[64];
    copy(act, sizeof act, f[0]);
    if (ieq(act, "float")) r.action = PHI_RULE_FLOAT;
    else if (ieq(act, "tile")) r.action = PHI_RULE_TILE;
    else if (ieq(act, "pseudo")) r.action = PHI_RULE_PSEUDO;
    else if (istarts(act, "workspace")) {
        r.action = PHI_RULE_WORKSPACE;
        r.ws = atoi(act + 9);
        r.silent = strstr(act, "silent") != NULL;
        if (r.ws < 1 || r.ws > 10) { error(p, "workspace rule needs 1-10"); return; }
    } else {
        return;  /* size, opacity, move, ... have no meaning here: ignore quietly */
    }

    for (int i = 1; i < n; i++) {
        if (r.nmatch >= PHI_MAX_MATCH) { error(p, "too many matchers in rule"); return; }
        char *m = f[i];
        int field;
        const char *pat;
        if (istarts(m, "class:")) { field = PHI_FIELD_CLASS; pat = m + 6; }
        else if (istarts(m, "initialclass:")) { field = PHI_FIELD_CLASS; pat = m + 13; }
        else if (istarts(m, "title:")) { field = PHI_FIELD_TITLE; pat = m + 6; }
        else if (istarts(m, "initialtitle:")) { field = PHI_FIELD_TITLE; pat = m + 13; }
        else if (istarts(m, "exe:")) { field = PHI_FIELD_EXE; pat = m + 4; }
        else if (strchr(m, ':') && !strchr(m, '(') && !strchr(m, '^')) {
            error(p, "unsupported matcher '%s' (rule skipped)", m);
            return;
        } else { field = PHI_FIELD_CLASS; pat = m; }  /* old v1 syntax: bare class regex */
        r.m[r.nmatch].field = field;
        compile_pattern(pat, r.m[r.nmatch].pat, sizeof r.m[0].pat);
        r.nmatch++;
    }
    if (c->nrules >= PHI_MAX_RULES) { error(p, "too many window rules"); return; }
    c->rules[c->nrules++] = r;
}

/* ---------- variables ---------- */

static const char *var_get(Parser *p, const char *name, size_t len)
{
    /* longest name wins, so $mainMod is not shadowed by $main */
    const char *best = NULL;
    size_t bestlen = 0;
    for (int i = 0; i < p->nvars; i++) {
        size_t l = strlen(p->vars[i].name);
        if (l <= len && l > bestlen && !strncmp(p->vars[i].name, name, l)) { best = p->vars[i].value; bestlen = l; }
    }
    return best ? best : NULL;
}

static void substitute(Parser *p, const char *in, char *out, size_t n)
{
    size_t o = 0;
    while (*in && o + 1 < n) {
        if (*in == '$') {
            const char *s = in + 1;
            size_t len = 0;
            while (isalnum((unsigned char)s[len]) || s[len] == '_') len++;
            const char *v = len ? var_get(p, s, len) : NULL;
            if (v) {
                size_t vl = 0;
                for (int i = 0; i < p->nvars; i++)
                    if (p->vars[i].value == v) { vl = strlen(p->vars[i].name); break; }
                for (const char *q = v; *q && o + 1 < n; q++) out[o++] = *q;
                in = s + vl;
                continue;
            }
        }
        out[o++] = *in++;
    }
    out[o] = 0;
}

static void set_var(Parser *p, const char *name, const char *value)
{
    for (int i = 0; i < p->nvars; i++)
        if (!strcmp(p->vars[i].name, name)) { copy(p->vars[i].value, sizeof p->vars[i].value, value); return; }
    if (p->nvars >= MAX_VARS) { error(p, "too many variables"); return; }
    copy(p->vars[p->nvars].name, sizeof p->vars[0].name, name);
    copy(p->vars[p->nvars].value, sizeof p->vars[0].value, value);
    p->nvars++;
}

/* ---------- options ---------- */

static void set_option(Parser *p, const char *key, char *v)
{
    PhiConfig *c = p->c;
    unsigned col;
    if (ieq(key, "general:gaps_in")) c->gaps_in = atoi(v);
    else if (ieq(key, "general:gaps_out")) c->gaps_out = atoi(v);
    else if (ieq(key, "general:col.active_border")) {
        if (ieq(v, "default")) c->has_active = 0;
        else if (parse_color(v, &col)) { c->active_border = col; c->has_active = 1; }
        else error(p, "bad color '%s'", v);
    } else if (ieq(key, "general:col.inactive_border")) {
        if (ieq(v, "default")) c->has_inactive = 0;
        else if (parse_color(v, &col)) { c->inactive_border = col; c->has_inactive = 1; }
        else error(p, "bad color '%s'", v);
    } else if (ieq(key, "general:layout")) {
        if (!ieq(v, "dwindle")) error(p, "only the dwindle layout is supported");
    } else if (ieq(key, "decoration:rounding")) c->rounding = atoi(v);
    else if (ieq(key, "dwindle:default_split_ratio")) {
        double r = atof(v);
        if (r < 0.1 || r > 1.9) error(p, "default_split_ratio must be 0.1 - 1.9");
        else c->dw.ratio = r / 2.0;
    } else if (ieq(key, "dwindle:force_split")) c->dw.force_split = atoi(v);
    else if (ieq(key, "dwindle:preserve_split")) c->dw.preserve_split = parse_bool(v);
    else if (ieq(key, "dwindle:split_width_multiplier")) {
        double m = atof(v);
        c->dw.width_mult = m > 0.1 ? m : 1.0;
    } else if (ieq(key, "input:follow_mouse")) c->follow_mouse = atoi(v);
    /* everything else (animations, monitor, env, input:kb_*, misc, ...) is ignored
     * on purpose so hyprland snippets can be pasted in as-is */
}

static void handle_line(Parser *p, char *line)
{
    PhiConfig *c = p->c;
    /* strip comments; "##" is a literal '#' */
    char clean[1024];
    size_t o = 0;
    for (size_t i = 0; line[i] && o + 1 < sizeof clean; i++) {
        if (line[i] == '#') {
            if (line[i + 1] == '#') { clean[o++] = '#'; i++; continue; }
            break;
        }
        clean[o++] = line[i];
    }
    clean[o] = 0;
    char *s = trim(clean);
    if (!*s) return;

    if (!strcmp(s, "}")) {
        if (p->depth > 0) p->depth--;
        else error(p, "unmatched '}'");
        return;
    }
    size_t l = strlen(s);
    if (s[l - 1] == '{') {
        s[l - 1] = 0;
        s = trim(s);
        if (p->depth >= MAX_DEPTH) { error(p, "sections nested too deep"); return; }
        copy(p->section[p->depth++], sizeof p->section[0], s);
        return;
    }

    char *eq = strchr(s, '=');
    if (!eq) { error(p, "expected 'key = value'"); return; }
    *eq = 0;
    char *key = trim(s);
    char raw[1024];
    copy(raw, sizeof raw, eq + 1);
    char *rawt = trim(raw);

    if (key[0] == '$') { set_var(p, key + 1, rawt); return; }

    char value[1024];
    substitute(p, rawt, value, sizeof value);
    char *v = trim(value);

    char full[256] = "";
    for (int i = 0; i < p->depth; i++) {
        strncat(full, p->section[i], sizeof full - strlen(full) - 1);
        strncat(full, ":", sizeof full - strlen(full) - 1);
    }
    strncat(full, key, sizeof full - strlen(full) - 1);

    if (istarts(key, "bind") && p->depth == 0) { parse_bind(p, key, v); return; }
    if ((ieq(key, "windowrule") || ieq(key, "windowrulev2")) && p->depth == 0) { parse_rule(p, v); return; }
    if (ieq(key, "exec-once") && p->depth == 0) {
        if (c->nexec_once < PHI_MAX_EXEC) copy(c->exec_once[c->nexec_once++], sizeof c->exec_once[0], v);
        else error(p, "too many exec-once lines");
        return;
    }
    if (ieq(key, "exec") && p->depth == 0) {
        if (c->nexec < PHI_MAX_EXEC) copy(c->exec[c->nexec++], sizeof c->exec[0], v);
        else error(p, "too many exec lines");
        return;
    }
    set_option(p, full, v);
}

int phi_config_parse(PhiConfig *c, const char *text)
{
    Parser *p = calloc(1, sizeof *p);
    if (!p) return -1;
    p->c = c;
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text += 3;
    const char *s = text;
    while (*s) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        char line[1024];
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, s, len);
        line[len] = 0;
        if (len && line[len - 1] == '\r') line[len - 1] = 0;
        p->line++;
        handle_line(p, line);
        if (!nl) break;
        s = nl + 1;
    }
    if (p->depth) error(p, "missing '}' at end of file");
    int n = c->nerr;
    free(p);
    return n;
}
