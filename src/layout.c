/* phiwm — dwindle layout engine. See layout.h. */
#include "layout.h"
#include <stdlib.h>

#define RATIO_MIN 0.05
#define RATIO_MAX 0.95

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static int is_leaf(const PhiNode *n) { return n->child[0] == NULL; }

static void replace_in_parent(PhiTree *t, PhiNode *old, PhiNode *nu)
{
    PhiNode *p = old->parent;
    nu->parent = p;
    if (!p) t->root = nu;
    else if (p->child[0] == old) p->child[0] = nu;
    else p->child[1] = nu;
}

static int want_horiz(PhiRect b, const PhiDwindle *cfg)
{
    double m = cfg && cfg->width_mult > 0 ? cfg->width_mult : 1.0;
    return (double)b.w >= (double)b.h * m;
}

static int imax(int a, int b) { return a > b ? a : b; }

/* Bottom-up: an inner node needs what its children need, side by side or stacked. */
static void measure(PhiNode *n)
{
    if (is_leaf(n)) return;
    PhiNode *a = n->child[0], *b = n->child[1];
    measure(a);
    measure(b);
    if (n->horiz) { n->min_w = a->min_w + b->min_w;       n->min_h = imax(a->min_h, b->min_h); }
    else          { n->min_w = imax(a->min_w, b->min_w);  n->min_h = a->min_h + b->min_h; }
}

static int fits(const PhiNode *n, int horiz, PhiRect box)
{
    const PhiNode *a = n->child[0], *b = n->child[1];
    if (horiz) return a->min_w + b->min_w <= box.w && imax(a->min_h, b->min_h) <= box.h;
    return imax(a->min_w, b->min_w) <= box.w && a->min_h + b->min_h <= box.h;
}

static int flipped;

static void layout_node(PhiNode *n, PhiRect box, const PhiDwindle *cfg)
{
    n->box = box;
    if (is_leaf(n)) return;
    int horiz = n->locked ? n->horiz : want_horiz(box, cfg);
    /* A direction that cannot hold both children's minimums gives way to the other
     * one when that fits: two wide windows end up stacked instead of overlapping.
     * The ratio itself is never bent: like Hyprland, every split keeps its share no
     * matter what sits inside, and a window that still does not fit overflows. */
    if (!fits(n, horiz, box) && fits(n, !horiz, box)) { horiz = !horiz; flipped++; }
    n->horiz = horiz;
    int len = horiz ? box.w : box.h;
    int first = (int)(len * n->ratio + 0.5);
    PhiRect a = box, b = box;
    if (horiz) { a.w = first; b.x = box.x + first; b.w = box.w - first; }
    else       { a.h = first; b.y = box.y + first; b.h = box.h - first; }
    layout_node(n->child[0], a, cfg);
    layout_node(n->child[1], b, cfg);
}

void phi_layout(PhiTree *t, PhiRect area, const PhiDwindle *cfg)
{
    if (!t->root) return;
    /* a flip changes what the parents need; one more pass settles it */
    for (int pass = 0; pass < 2; pass++) {
        measure(t->root);
        flipped = 0;
        layout_node(t->root, area, cfg);
        if (!flipped) break;
    }
}

void phi_set_min(PhiNode *leaf, int w, int h)
{
    if (!leaf) return;
    leaf->min_w = w;
    leaf->min_h = h;
}

int phi_can_split(const PhiNode *target, const PhiDwindle *cfg, int min_w, int min_h)
{
    if (!target) return 1;
    PhiRect b = target->box;
    double keep = clampd(cfg ? cfg->ratio : 0.5, RATIO_MIN, RATIO_MAX);
    double small = keep < 0.5 ? keep : 1.0 - keep;   /* the smaller share of the two */
    int w = imax(target->min_w, min_w), h = imax(target->min_h, min_h);
    if (w <= (int)(b.w * small) && h <= b.h) return 1;   /* side by side */
    if (w <= b.w && h <= (int)(b.h * small)) return 1;   /* stacked */
    return 0;
}

PhiNode *phi_first(const PhiTree *t)
{
    PhiNode *n = t->root;
    while (n && !is_leaf(n)) n = n->child[0];
    return n;
}

PhiNode *phi_last(const PhiTree *t)
{
    PhiNode *n = t->root;
    while (n && !is_leaf(n)) n = n->child[1];
    return n;
}

PhiNode *phi_next(const PhiNode *leaf)
{
    const PhiNode *n = leaf;
    while (n->parent && n->parent->child[1] == n) n = n->parent;
    if (!n->parent) return NULL;
    PhiNode *m = n->parent->child[1];
    while (!is_leaf(m)) m = m->child[0];
    return m;
}

int phi_count(const PhiTree *t)
{
    int c = 0;
    for (PhiNode *n = phi_first(t); n; n = phi_next(n)) c++;
    return c;
}

/* Where a box sits, clockwise from 3 o'clock: right 0, bottom 1, left 2, top 3
 * (y grows downwards). The whole area counts as left: that is where its half goes. */
static double clock_pos(PhiRect b, PhiRect area)
{
    double dx = (b.x + b.w / 2.0) - (area.x + area.w / 2.0);
    double dy = (b.y + b.h / 2.0) - (area.y + area.h / 2.0);
    double a = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    if (a == 0) return 2.0;
    double p = dx / a;
    return dy >= 0 ? 1 - p : 3 + p;
}

static int depth_of(const PhiNode *n)
{
    int d = 0;
    while (n->parent) { n = n->parent; d++; }
    return d;
}

PhiNode *phi_split_target(const PhiTree *t, PhiRect area)
{
    int min_d = -1;
    for (PhiNode *n = phi_first(t); n; n = phi_next(n)) {
        int d = depth_of(n);
        if (min_d < 0 || d < min_d) min_d = d;
    }
    PhiNode *next = NULL, *wrap = NULL;
    double na = 0, wa = 0;
    for (PhiNode *n = phi_first(t); n; n = phi_next(n)) {
        if (depth_of(n) != min_d) continue;
        double a = clock_pos(n->box, area);
        if (a > t->last + 1e-9 && (!next || a < na)) { next = n; na = a; }
        if (!wrap || a < wa) { wrap = n; wa = a; }
    }
    return next ? next : wrap;
}

PhiNode *phi_insert(PhiTree *t, PhiNode *target, void *win, PhiRect area,
                    const PhiDwindle *cfg, int force, int px, int py)
{
    PhiNode *leaf = calloc(1, sizeof *leaf);
    if (!leaf) return NULL;
    leaf->win = win;
    if (!t->root) { t->root = leaf; leaf->box = area; return leaf; }

    phi_layout(t, area, cfg);
    if (!target || !is_leaf(target)) target = phi_split_target(t, area);

    PhiNode *split = calloc(1, sizeof *split);
    if (!split) { free(leaf); return NULL; }
    PhiRect b = target->box;
    replace_in_parent(t, target, split);
    split->box = b;
    split->horiz = want_horiz(b, cfg);
    split->locked = cfg ? cfg->preserve_split : 1;

    int new_first = 0;   /* the new window goes right/bottom */
    if (force == 1) new_first = 1;
    else if (force == 0) {
        int inside = px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h;
        if (!inside) new_first = 0;
        else if (split->horiz) new_first = px < b.x + b.w / 2;
        else new_first = py < b.y + b.h / 2;
    }

    double keep = clampd(cfg ? cfg->ratio : 0.5, RATIO_MIN, RATIO_MAX);
    split->child[new_first ? 0 : 1] = leaf;
    split->child[new_first ? 1 : 0] = target;
    split->ratio = new_first ? 1.0 - keep : keep;
    leaf->parent = split;
    target->parent = split;
    phi_layout(t, area, cfg);
    t->last = clock_pos(target->box, area);   /* the next split continues clockwise from here */
    return leaf;
}

void phi_remove(PhiTree *t, PhiNode *leaf)
{
    if (!leaf) return;
    PhiNode *p = leaf->parent;
    if (!p) {
        if (t->root == leaf) t->root = NULL;
        free(leaf);
        return;
    }
    PhiNode *sib = p->child[0] == leaf ? p->child[1] : p->child[0];
    replace_in_parent(t, p, sib);
    free(p);
    free(leaf);
}

PhiRect phi_tile_rect(const PhiNode *leaf, PhiRect area, int gap_in)
{
    PhiRect r = leaf->box;
    int l = r.x, tp = r.y, rt = r.x + r.w, bt = r.y + r.h;
    if (l > area.x) l += gap_in;
    if (tp > area.y) tp += gap_in;
    if (rt < area.x + area.w) rt -= gap_in;
    if (bt < area.y + area.h) bt -= gap_in;
    r.x = l; r.y = tp;
    r.w = rt - l < 1 ? 1 : rt - l;
    r.h = bt - tp < 1 ? 1 : bt - tp;
    return r;
}

void phi_toggle_split(PhiNode *leaf)
{
    if (!leaf || !leaf->parent) return;
    leaf->parent->horiz = !leaf->parent->horiz;
    leaf->parent->locked = 1;
}

void phi_swap(PhiNode *a, PhiNode *b)
{
    if (!a || !b || a == b) return;
    void *w = a->win; a->win = b->win; b->win = w;
}

/* Closest ancestor split with the given orientation where the leaf sits on
 * side `side` (0 = first child, 1 = second child). */
static PhiNode *ancestor(PhiNode *leaf, int horiz, int side)
{
    PhiNode *n = leaf;
    while (n->parent) {
        PhiNode *p = n->parent;
        int s = p->child[0] == n ? 0 : 1;
        if (p->horiz == horiz && s == side) return p;
        n = p;
    }
    return NULL;
}

static int nudge(PhiNode *leaf, int horiz, int delta)
{
    if (!delta) return 0;
    /* Prefer moving the edge on the far side (right / bottom), like Hyprland. */
    PhiNode *p = ancestor(leaf, horiz, 0);
    int sign = 1;
    if (!p) { p = ancestor(leaf, horiz, 1); sign = -1; }
    if (!p) return 0;
    int len = horiz ? p->box.w : p->box.h;
    if (len <= 0) return 0;
    double nr = clampd(p->ratio + sign * (double)delta / len, RATIO_MIN, RATIO_MAX);
    if (nr == p->ratio) return 0;
    p->ratio = nr;
    return 1;
}

int phi_resize(PhiNode *leaf, int dx, int dy)
{
    if (!leaf) return 0;
    int a = nudge(leaf, 1, dx);
    int b = nudge(leaf, 0, dy);
    return a || b;
}

int phi_move_edge(PhiNode *leaf, int edge, int coord)
{
    if (!leaf) return 0;
    int horiz = edge == PHI_EDGE_LEFT || edge == PHI_EDGE_RIGHT;
    int side = (edge == PHI_EDGE_LEFT || edge == PHI_EDGE_TOP) ? 1 : 0;
    PhiNode *p = ancestor(leaf, horiz, side);
    if (!p) return 0;
    int start = horiz ? p->box.x : p->box.y;
    int len = horiz ? p->box.w : p->box.h;
    if (len <= 0) return 0;
    p->ratio = clampd((double)(coord - start) / len, RATIO_MIN, RATIO_MAX);
    return 1;
}

int phi_split_ratio(PhiNode *leaf, double value, int exact)
{
    if (!leaf || !leaf->parent) return 0;
    PhiNode *p = leaf->parent;
    p->ratio = clampd(exact ? value : p->ratio + value, RATIO_MIN, RATIO_MAX);
    return 1;
}

static void free_node(PhiNode *n)
{
    if (!n) return;
    free_node(n->child[0]);
    free_node(n->child[1]);
    free(n);
}

void phi_free(PhiTree *t)
{
    free_node(t->root);
    t->root = NULL;
}
