/* Native tests for the built-in settings: the bind table is sane and the
 * window-rule patterns match what they are meant to. */
#include "defaults.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const PhiBind *find_bind(unsigned mods, unsigned vk, unsigned scan)
{
    for (int i = 0; i < phi_nbinds; i++)
        if (phi_binds[i].mods == mods && phi_binds[i].vk == vk && phi_binds[i].scan == scan) return &phi_binds[i];
    return NULL;
}

int main(void)
{
    /* every bind names a known dispatcher, has exactly one of vk/scan, and is unique */
    for (int i = 0; i < phi_nbinds; i++) {
        const PhiBind *b = &phi_binds[i];
        CHECK(phi_dispatcher_known(b->disp));
        CHECK((b->vk != 0) != (b->scan != 0));
        CHECK(b->scan <= 0x58);
        CHECK(b->arg != NULL);
        for (int k = 0; k < i; k++)
            CHECK(!(phi_binds[k].mods == b->mods && phi_binds[k].vk == b->vk && phi_binds[k].scan == b->scan));
    }
    CHECK(phi_nbinds == 47);
    CHECK(!phi_dispatcher_known("reload"));

    const PhiBind *b = find_bind(PHI_MOD_SUPER, 0x0D, 0);
    CHECK(b && !strcmp(b->disp, "exec") && !strcmp(b->arg, "wt.exe"));
    b = find_bind(PHI_MOD_SUPER, 0, 2);                      /* the "1" key position */
    CHECK(b && !strcmp(b->disp, "workspace") && !strcmp(b->arg, "1"));
    b = find_bind(PHI_MOD_SUPER | PHI_MOD_SHIFT, 0, 11);     /* the "0" key position */
    CHECK(b && !strcmp(b->disp, "movetoworkspace") && !strcmp(b->arg, "10"));
    b = find_bind(PHI_MOD_SUPER, 0xBD, 0);
    CHECK(b && b->repeat && !strcmp(b->arg, "-100 0"));
    b = find_bind(PHI_MOD_SUPER | PHI_MOD_SHIFT, 0x25, 0);
    CHECK(b && !strcmp(b->disp, "swapwindow") && !strcmp(b->arg, "l"));
    b = find_bind(PHI_MOD_SUPER | PHI_MOD_CTRL | PHI_MOD_SHIFT, 'Q', 0);
    CHECK(b && !strcmp(b->disp, "exit"));

    /* rules */
    CHECK(phi_nrules == 4);
    CHECK(phi_rules[0].action == PHI_RULE_FLOAT && phi_rules[0].field == PHI_FIELD_TITLE);
    CHECK(phi_pattern_match(phi_rules[0].pat, "Picture-in-Picture"));
    CHECK(phi_pattern_match(phi_rules[0].pat, "picture in picture"));
    CHECK(!phi_pattern_match(phi_rules[0].pat, "Picture-in-Picture - Edge"));
    CHECK(phi_pattern_match(phi_rules[1].pat, "CalculatorApp.exe"));
    CHECK(!phi_pattern_match(phi_rules[1].pat, "CalculatorApp"));
    CHECK(phi_pattern_match(phi_rules[3].pat, "PowerToys.Settings.exe"));
    CHECK(!phi_pattern_match(phi_rules[3].pat, "PowerToys"));

    /* the glob itself */
    CHECK(phi_pattern_match("a?c", "AbC"));
    CHECK(phi_pattern_match("*", ""));
    CHECK(phi_pattern_match("x|y*|z", "yes"));
    CHECK(!phi_pattern_match("x|y*|z", "no"));
    CHECK(!phi_pattern_match("abc", NULL));

    printf("defaults: %d binds, %d rules\n", phi_nbinds, phi_nrules);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("defaults: all tests passed\n");
    return 0;
}
