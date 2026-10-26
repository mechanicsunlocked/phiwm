/* phiwm — dwindle layout engine (platform independent, plain C99)
 *
 * Each workspace owns a binary tree. Leaves hold windows, inner nodes
 * hold a split (side-by-side or stacked) and the share of the first child.
 * New windows split the focused leaf, exactly like Hyprland's dwindle.
 */
#ifndef PHI_LAYOUT_H
#define PHI_LAYOUT_H

typedef struct { int x, y, w, h; } PhiRect;

typedef struct PhiNode PhiNode;
struct PhiNode {
    PhiNode *parent;
    PhiNode *child[2];  /* both NULL on a leaf */
    void    *win;       /* payload, only on leaves */
    int      horiz;     /* inner node: 1 = side by side, 0 = stacked */
    int      locked;    /* inner node: direction fixed (preserve_split / togglesplit) */
    double   ratio;     /* inner node: share of child[0], 0.05 .. 0.95 */
    PhiRect  box;       /* box from the last layout pass, gaps not applied */
    int      min_w, min_h; /* leaf: the window's minimum size; inner node: what its children need */
};

typedef struct {
    PhiNode *root;
    double   last;   /* clockwise position (0..4) of the window split last; see phi_split_target */
} PhiTree;

typedef struct {
    double ratio;          /* share the existing window keeps when it is split (0.5 = half) */
    int    preserve_split; /* keep each split's direction once it is made */
    double width_mult;     /* split side by side when w >= h * width_mult */
} PhiDwindle;

enum { PHI_EDGE_LEFT, PHI_EDGE_RIGHT, PHI_EDGE_TOP, PHI_EDGE_BOTTOM };

/* The leaf a new window splits when none is given: the least-split one, and among
 * those the next one clockwise after the window that was split last. So the screen
 * is halved left|right, then the right half top/bottom, then the left half, then
 * the four quarters in turn. NULL on an empty tree. Boxes must be laid out. */
PhiNode *phi_split_target(const PhiTree *t, PhiRect area);

/* Insert `win` by splitting `target` (NULL = phi_split_target). The new window goes
 * right/bottom; `force` >= 0 overrides that: 0 = the side of point (px, py),
 * 1 = new window left/top, 2 = right/bottom. Returns the new leaf, or NULL on
 * allocation failure. */
PhiNode *phi_insert(PhiTree *t, PhiNode *target, void *win, PhiRect area,
                    const PhiDwindle *cfg, int force, int px, int py);

/* Remove a leaf; its sibling takes over the parent's box. Frees the leaf. */
void     phi_remove(PhiTree *t, PhiNode *leaf);

/* The smallest box a leaf accepts (0 = no minimum). A split that cannot hold both
 * children's minimums side by side is stacked instead, and vice versa. The ratio
 * is never changed for it: a leaf that still does not fit gets a box that is too
 * small, and the caller decides what to do with the window. */
void     phi_set_min(PhiNode *leaf, int w, int h);

/* Whether `target`'s box, split at the ratio, has room for its own minimum and a
 * new window needing (min_w, min_h), side by side or stacked. NULL target: yes. */
int      phi_can_split(const PhiNode *target, const PhiDwindle *cfg, int min_w, int min_h);

/* Recompute every node's box inside `area`. */
void     phi_layout(PhiTree *t, PhiRect area, const PhiDwindle *cfg);

/* Window rectangle for a leaf: its box minus gap_in on every edge that
 * touches another window (outer edges are already inset by gaps_out). */
PhiRect  phi_tile_rect(const PhiNode *leaf, PhiRect area, int gap_in);

void     phi_toggle_split(PhiNode *leaf);
void     phi_swap(PhiNode *a, PhiNode *b);           /* swaps payloads only */
int      phi_resize(PhiNode *leaf, int dx, int dy);  /* grow/shrink by pixels */
int      phi_move_edge(PhiNode *leaf, int edge, int coord);
int      phi_split_ratio(PhiNode *leaf, double value, int exact);

PhiNode *phi_first(const PhiTree *t);
PhiNode *phi_last(const PhiTree *t);
PhiNode *phi_next(const PhiNode *leaf);
int      phi_count(const PhiTree *t);
void     phi_free(PhiTree *t);

#endif
