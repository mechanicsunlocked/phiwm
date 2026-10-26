/* Native tests for the config parser: parses res/default.conf plus edge cases. */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    buf[n] = 0;
    fclose(f);
    return buf;
}

static const PhiBind *find_bind(const PhiConfig *c, unsigned mods, unsigned vk, unsigned scan)
{
    for (int i = 0; i < c->nbinds; i++)
        if (c->binds[i].mods == mods && c->binds[i].vk == vk && c->binds[i].scan == scan) return &c->binds[i];
    return NULL;
}

int main(int argc, char **argv)
{
    static PhiConfig c;
    const char *path = argc > 1 ? argv[1] : "../res/default.conf";
    char *text = slurp(path);
    if (!text) { printf("cannot read %s\n", path); return 1; }

    phi_config_defaults(&c);
    int errs = phi_config_parse(&c, text);
    for (int i = 0; i < c.nerr && i < PHI_MAX_ERRS; i++) printf("  config error: %s\n", c.err[i]);
    CHECK(errs == 0);
    CHECK(c.gaps_in == 5 && c.gaps_out == 10);
    CHECK(c.has_active && c.active_border == 0x33ccff);
    CHECK(c.has_inactive && c.inactive_border == 0x595959);
    CHECK(fabs(c.dw.ratio - 0.618) < 1e-9);
    CHECK(c.dw.force_split == 2 && c.dw.preserve_split == 1);
    CHECK(c.rounding == 10);

    const PhiBind *b = find_bind(&c, PHI_MOD_SUPER, 0x0D, 0);
    CHECK(b && !strcmp(b->disp, "exec") && !strcmp(b->arg, "wt.exe"));
    b = find_bind(&c, PHI_MOD_SUPER, 0, 2);            /* code:10 -> scancode 2 ("1") */
    CHECK(b && !strcmp(b->disp, "workspace") && !strcmp(b->arg, "1"));
    b = find_bind(&c, PHI_MOD_SUPER | PHI_MOD_SHIFT, 0, 11); /* code:19 -> "0" */
    CHECK(b && !strcmp(b->disp, "movetoworkspace") && !strcmp(b->arg, "10"));
    b = find_bind(&c, PHI_MOD_SUPER, 0xBD, 0);
    CHECK(b && b->repeat && !strcmp(b->arg, "-100 0"));
    b = find_bind(&c, PHI_MOD_SUPER | PHI_MOD_SHIFT, 0x25, 0);
    CHECK(b && !strcmp(b->disp, "swapwindow") && !strcmp(b->arg, "l"));
    printf("default.conf: %d binds, %d rules\n", c.nbinds, c.nrules);

    /* rules */
    CHECK(c.nrules >= 4);
    CHECK(c.rules[0].action == 1 && c.rules[0].m[0].field == PHI_FIELD_TITLE);
    CHECK(phi_pattern_match(c.rules[0].m[0].pat, "Picture-in-Picture"));
    CHECK(phi_pattern_match(c.rules[0].m[0].pat, "picture in picture"));
    CHECK(!phi_pattern_match(c.rules[0].m[0].pat, "Picture-in-Picture - Edge"));
    CHECK(phi_pattern_match(c.rules[1].m[0].pat, "CalculatorApp.exe"));
    CHECK(phi_pattern_match(c.rules[3].m[0].pat, "PowerToys.Settings.exe"));
    CHECK(!phi_pattern_match(c.rules[3].m[0].pat, "PowerToys"));

    /* an Omarchy/Hyprland style snippet with things phiwm must tolerate */
    const char *snippet =
        "source = ~/.local/share/omarchy/default/hypr/bindings.conf\n"
        "monitor=,preferred,auto,1\n"
        "env = XCURSOR_SIZE,24\n"
        "$mainMod = SUPER # comment\n"
        "general {\n  gaps_in = 3\n  gaps_out = 6 12 6 12\n  border_size = 2\n"
        "  col.active_border = rgb(ff0000)\n}\n"
        "animations {\n  enabled = yes\n  bezier = myBezier, 0.05, 0.9, 0.1, 1.05\n}\n"
        "dwindle:default_split_ratio = 1.0\n"
        "bindd = $mainMod, RETURN, Terminal, exec, wt.exe -p \"Ubuntu, dev\"\n"
        "bindm = $mainMod, mouse:272, movewindow\n"
        "bind = $mainMod, mouse_down, workspace, e+1\n"
        "bind = SUPER_SHIFT, Q, killactive\n"
        "bind = , XF86AudioMute, exec, nircmd mutesysvolume 2\n"
        "bind = SUPER, X, togglegroup\n"
        "bind = SUPER, Return, exec, alacritty.exe\n"
        "windowrulev2 = float, class:^(org.gnome.Calculator|pavucontrol)$\n"
        "windowrulev2 = opacity 0.9, class:.*\n"
        "windowrule = workspace 4 silent, class:^(Spotify)$, title:.*Premium.*\n"
        "windowrulev2 = float, xwayland:1\n"
        "color = ##ff0000\n";
    phi_config_defaults(&c);
    errs = phi_config_parse(&c, snippet);
    for (int i = 0; i < c.nerr && i < PHI_MAX_ERRS; i++) printf("  expected error: %s\n", c.err[i]);
    CHECK(errs == 2);  /* togglegroup + xwayland matcher */
    CHECK(c.gaps_in == 3 && c.gaps_out == 6);
    CHECK(c.active_border == 0xff0000);
    CHECK(fabs(c.dw.ratio - 0.5) < 1e-9);
    b = find_bind(&c, PHI_MOD_SUPER, 0x0D, 0);
    CHECK(b && !strcmp(b->arg, "alacritty.exe"));   /* later bind replaced the bindd */
    CHECK(c.nbinds == 3);
    b = find_bind(&c, PHI_MOD_SUPER | PHI_MOD_SHIFT, 'Q', 0);
    CHECK(b && !strcmp(b->disp, "killactive"));
    b = find_bind(&c, 0, 0xAD, 0);
    CHECK(b && !strcmp(b->arg, "nircmd mutesysvolume 2"));
    CHECK(c.nrules == 2);
    CHECK(phi_pattern_match(c.rules[0].m[0].pat, "pavucontrol"));
    CHECK(phi_pattern_match(c.rules[0].m[0].pat, "org.gnome.Calculator"));
    CHECK(c.rules[1].action == PHI_RULE_WORKSPACE && c.rules[1].ws == 4 && c.rules[1].silent);
    CHECK(c.rules[1].nmatch == 2 && phi_pattern_match(c.rules[1].m[1].pat, "Spotify Premium"));

    /* comma inside exec args survives */
    phi_config_defaults(&c);
    phi_config_parse(&c, "bind = ALT, T, exec, cmd /c echo a, b, c\n");
    CHECK(c.nbinds == 1 && !strcmp(c.binds[0].arg, "cmd /c echo a, b, c"));
    CHECK(c.binds[0].mods == PHI_MOD_ALT && c.binds[0].vk == 'T');

    /* garbage */
    phi_config_defaults(&c);
    errs = phi_config_parse(&c, "general {\nnonsense\n}\n}\nbind = SUPER, NOPE, exec, x\ndwindle {\n");
    CHECK(errs == 4);

    free(text);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("config: all tests passed\n");
    return 0;
}
