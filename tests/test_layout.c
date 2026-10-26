/* Native unit tests for the dwindle engine: cc -I../src test_layout.c ../src/layout.c */
#include "layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static PhiDwindle half = { 0.5, 1, 1.0 };

static PhiNode *find(PhiTree *t, void *w)
{
    for (PhiNode *n = phi_first(t); n; n = phi_next(n)) if (n->win == w) return n;
    return NULL;
}

static void dump(PhiTree *t, PhiRect area, int gin)
{
    for (PhiNode *n = phi_first(t); n; n = phi_next(n)) {
        PhiRect r = phi_tile_rect(n, area, gin);
        printf("   win %ld: %4d,%4d %4dx%4d\n", (long)(size_t)n->win, r.x, r.y, r.w, r.h);
    }
}

int main(void)
{
    PhiRect area = { 10, 10, 2540, 1420 }; /* 2560x1440 work area, gaps_out 10 */
    PhiTree t = { 0 };

    /* one window fills the area */
    PhiNode *focus = phi_insert(&t, NULL, (void *)1, area, &half, -1, 0, 0);
    phi_layout(&t, area, &half);
    CHECK(focus->box.w == area.w && focus->box.h == area.h);

    /* spiral: always split the newest window */
    for (long i = 2; i <= 5; i++) {
        focus = phi_insert(&t, focus, (void *)(size_t)i, area, &half, -1, 0, 0);
        CHECK(focus != NULL);
    }
    phi_layout(&t, area, &half);
    CHECK(phi_count(&t) == 5);
    printf("spiral, 5 windows:\n");
    dump(&t, area, 5);

    PhiNode *w1 = find(&t, (void *)1), *w2 = find(&t, (void *)2), *w3 = find(&t, (void *)3);
    PhiNode *w4 = find(&t, (void *)4), *w5 = find(&t, (void *)5);
    /* halves, clockwise: 2 right of 1, 3 below 2, 4 left of 3, 5 above 4 */
    CHECK(w1->box.w == 1270 && w1->box.h == 1420);
    CHECK(w2->box.x == w1->box.x + w1->box.w && w2->box.w == 1270 && w2->box.h == 710);
    CHECK(w3->box.y == w2->box.y + w2->box.h && w3->box.h == 710 && w3->box.w == 635);
    CHECK(w4->box.x + w4->box.w == w3->box.x && w4->box.w == 635);
    CHECK(w5->box.x == w4->box.x && w5->box.y == w3->box.y && w5->box.h == 355);
    CHECK(w4->box.y == w5->box.y + w5->box.h && w4->box.h == 355);

    /* gaps: 2 * gap_in between neighbours, none added on outer edges */
    PhiRect r1 = phi_tile_rect(w1, area, 5), r2 = phi_tile_rect(w2, area, 5);
    CHECK(r1.x == area.x && r1.y == area.y && r1.h == area.h);
    CHECK(r2.x - (r1.x + r1.w) == 10);

    /* every leaf box tiles the area exactly: total area matches */
    long sum = 0;
    for (PhiNode *n = phi_first(&t); n; n = phi_next(n)) sum += (long)n->box.w * n->box.h;
    CHECK(sum == (long)area.w * area.h);

    /* togglesplit flips the parent of window 3 */
    int before = w3->parent->horiz;
    phi_toggle_split(w3);
    CHECK(w3->parent->horiz == !before);
    phi_toggle_split(w3);

    /* keyboard resize grows window 1 to the right */
    int w_before = w1->box.w;
    CHECK(phi_resize(w1, 100, 0));
    phi_layout(&t, area, &half);
    CHECK(abs(w1->box.w - (w_before + 100)) <= 1);

    /* rightmost window grows leftwards */
    int x5 = w5->box.x;
    CHECK(phi_resize(w5, 50, 0) || phi_resize(w5, 0, 0) == 0);
    phi_layout(&t, area, &half);
    printf("after resizes:\n");
    dump(&t, area, 5);
    (void)x5;

    /* mouse edge drag: move window 1's right edge to x = 1200 */
    CHECK(phi_move_edge(w1, PHI_EDGE_RIGHT, 1200));
    phi_layout(&t, area, &half);
    CHECK(abs((w1->box.x + w1->box.w) - 1200) <= 1);
    CHECK(!phi_move_edge(w1, PHI_EDGE_LEFT, 100)); /* no split on its left */

    /* swap keeps boxes, exchanges payloads */
    PhiRect b1 = w1->box;
    phi_swap(w1, w2);
    CHECK(w1->win == (void *)2 && w2->win == (void *)1);
    CHECK(w1->box.x == b1.x);
    phi_swap(w1, w2);

    /* removing a middle window gives its space to the sibling */
    phi_remove(&t, find(&t, (void *)3));
    phi_layout(&t, area, &half);
    CHECK(phi_count(&t) == 4);
    sum = 0;
    for (PhiNode *n = phi_first(&t); n; n = phi_next(n)) sum += (long)n->box.w * n->box.h;
    CHECK(sum == (long)area.w * area.h);

    /* remove everything */
    while (t.root) phi_remove(&t, phi_first(&t));
    CHECK(phi_count(&t) == 0);

    /* force_split 0 follows the point; force 1 puts the new window first */
    PhiNode *a = phi_insert(&t, NULL, (void *)1, area, &half, -1, 0, 0);
    PhiNode *b = phi_insert(&t, a, (void *)2, area, &half, 0, 50, 700);  /* point on the left half */
    phi_layout(&t, area, &half);
    CHECK(b->box.x == area.x);              /* new window went left */
    CHECK(a->box.w == b->box.w);            /* halves */
    PhiNode *c = phi_insert(&t, a, (void *)3, area, &half, 1, 0, 0);
    phi_layout(&t, area, &half);
    CHECK(c->parent->child[0] == c);

    /* stacked split on a portrait area */
    phi_free(&t);
    PhiRect portrait = { 0, 0, 1000, 1800 };
    a = phi_insert(&t, NULL, (void *)1, portrait, &half, -1, 0, 0);
    b = phi_insert(&t, a, (void *)2, portrait, &half, -1, 0, 0);
    phi_layout(&t, portrait, &half);
    CHECK(a->parent->horiz == 0 && b->box.y > a->box.y);

    /* split ratio clamps */
    phi_split_ratio(b, 5.0, 1);
    CHECK(a->parent->ratio <= 0.95);
    phi_free(&t);

    /* minimum sizes */
    PhiRect wide = { 0, 0, 1000, 800 };
    a = phi_insert(&t, NULL, (void *)1, wide, &half, -1, 0, 0);
    b = phi_insert(&t, a, (void *)2, wide, &half, -1, 0, 0);
    phi_layout(&t, wide, &half);
    CHECK(a->parent->horiz == 1);
    /* two windows too wide to sit side by side are stacked */
    phi_set_min(a, 600, 100);
    phi_set_min(b, 600, 100);
    phi_layout(&t, wide, &half);
    CHECK(a->parent->horiz == 0 && a->box.w == 1000 && b->box.w == 1000 && b->box.y == a->box.h);
    /* the split stays (locked) and the ratio is never bent: b is short of its minimum */
    phi_set_min(a, 0, 0);
    phi_set_min(b, 0, 700);
    phi_layout(&t, wide, &half);
    CHECK(a->parent->horiz == 0 && a->box.h == 400 && b->box.h < 700);
    /* no direction fits: the ratio is left alone (the app overflows, phiwm does not jitter) */
    phi_set_min(a, 600, 500);
    phi_set_min(b, 600, 500);
    phi_layout(&t, wide, &half);
    CHECK(a->parent->horiz == 0 && a->box.h == 400);
    /* stacked does not fit but side by side does: flips back */
    phi_set_min(a, 0, 500);
    phi_set_min(b, 0, 500);
    phi_layout(&t, wide, &half);
    CHECK(a->parent->horiz == 1 && a->box.w == 500 && a->box.h == 800);
    /* a column deeper in the spiral: its needs reach the root through measure() */
    PhiNode *d = phi_insert(&t, b, (void *)3, wide, &half, -1, 0, 0);
    phi_set_min(a, 0, 0);
    phi_set_min(b, 300, 0);
    phi_set_min(d, 300, 0);
    phi_layout(&t, wide, &half);
    CHECK(b->parent->horiz == 0);                 /* b over d in the 382 wide column */
    CHECK(t.root->min_w == 300 && a->box.w + b->box.w == 1000 && b->box.w >= 300 && d->box.w >= 300);
    phi_set_min(b, 600, 0);                       /* too wide for the column: the root keeps its ratio */
    phi_layout(&t, wide, &half);
    CHECK(b->parent->horiz == 0 && a->box.w == 500 && b->box.w == 500);
    phi_free(&t);

    /* can the focused window's half take a newcomer? */
    a = phi_insert(&t, NULL, (void *)1, wide, &half, -1, 0, 0);
    phi_layout(&t, wide, &half);
    CHECK(phi_can_split(a, &half, 0, 0));
    CHECK(phi_can_split(a, &half, 600, 0));       /* too wide for a half: stacked works */
    CHECK(!phi_can_split(a, &half, 600, 500));    /* neither way */
    phi_set_min(a, 0, 500);
    CHECK(!phi_can_split(a, &half, 600, 0));      /* a itself is too tall to stack */
    CHECK(phi_can_split(NULL, &half, 9999, 9999));
    phi_free(&t);

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("layout: all tests passed\n");
    return 0;
}
