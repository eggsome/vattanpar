#include "game.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>

/* 2.5D oblique projection: a point at height z draws at screen y - z.
 * Walls are extruded volumes (south faces visible), the player and
 * barrels are cylinders, and everything is painter-sorted by its ground
 * footprint so tall things occlude what stands behind them. */

#define COL_GRASS  0xFF3B5E3B
#define COL_SAND   0xFFC0A45E
#define COL_WATER  0xFF2A5E86
#define COL_ICE    0xFFB9D4DE
#define COL_BULLET 0xFFFFE066
#define COL_GUN    0xFF565C68
#define COL_SHADOW 0xFF14161A

#define COL_PLAYER_TOP  0xFFEDEFF4
#define COL_PLAYER_RIM  0xFF9AA0AC
#define COL_PLAYER_SIDE 0xFFAEB4C4
#define COL_PLAYER_HI   0xFFC9CDDA

#define COL_BARREL_RIM  0xFF5C3A17
#define COL_BARREL_SIDE 0xFF6B4420
#define COL_BARREL_HI   0xFF7F5329

#define PLAYER_H 34 /* cylinder body heights, world px */
#define BARREL_H 30
#define BULLET_Z 18 /* bullets fly at waist height above their level */

static const uint32_t TERRAIN_COL[TERRAIN_COUNT] = {
    [TERRAIN_GRASS] = COL_GRASS,
    [TERRAIN_SAND]  = COL_SAND,
    [TERRAIN_WATER] = COL_WATER,
    [TERRAIN_ICE]   = COL_ICE,
};

/* indexed by wall level 1..3: lit top, darker extruded south face.
 * The steps between levels are deliberately wide so the three heights
 * read at a glance. */
static const uint32_t WALL_COL[4]  = {0, 0xFF282C34, 0xFF414857, 0xFF5D667B};
static const uint32_t WALL_EDGE[4] = {0, 0xFF3E4450, 0xFF5A6274, 0xFF7C86A0};
static const uint32_t WALL_FACE[4] = {0, 0xFF1A1D23, 0xFF262B36, 0xFF333A49};
static const uint32_t WALL_MM[4]   = {0, 0xFF3F444E, 0xFF565E70, 0xFF7A8299};
#define COL_SEAM 0xFF12151B /* storey seams and ground contact lines */

static const uint32_t BARREL_COL[3] = {0xFF83501F, 0xFF9C5F27, 0xFFB4712F};

static void fill_rect(uint32_t *pix, int x, int y, int w, int h, uint32_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    for (int j = y; j < y + h; j++) {
        uint32_t *row = pix + (size_t)j * SCREEN_W + x;
        for (int i = 0; i < w; i++)
            row[i] = c;
    }
}

static void fill_circle(uint32_t *pix, int cx, int cy, int r, uint32_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= SCREEN_H) continue;
        int hw = (int)sqrtf((float)(r * r - dy * dy));
        int xa = cx - hw, xb = cx + hw + 1;
        if (xa < 0) xa = 0;
        if (xb > SCREEN_W) xb = SCREEN_W;
        uint32_t *row = pix + (size_t)y * SCREEN_W;
        for (int x = xa; x < xb; x++)
            row[x] = c;
    }
}

static void fill_ring(uint32_t *pix, int cx, int cy, int r, int t, uint32_t c)
{
    int ir = r - t;
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= SCREEN_H) continue;
        int ohw = (int)sqrtf((float)(r * r - dy * dy));
        int ihw = (dy > -ir && dy < ir)
                      ? (int)sqrtf((float)(ir * ir - dy * dy)) : 0;
        uint32_t *row = pix + (size_t)y * SCREEN_W;
        for (int x = cx - ohw; x <= cx + ohw; x++) {
            if (x < 0 || x >= SCREEN_W) continue;
            if (x > cx - ihw && x < cx + ihw) continue;
            row[x] = c;
        }
    }
}

/* subtle multiply-darken, used for the grid so it reads on any terrain */
static void darken_rect(uint32_t *pix, int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    for (int j = y; j < y + h; j++) {
        uint32_t *row = pix + (size_t)j * SCREEN_W + x;
        for (int i = 0; i < w; i++)
            row[i] -= (row[i] >> 3) & 0x001F1F1F;
    }
}

static void outline_rect(uint32_t *pix, int x, int y, int w, int h, uint32_t c)
{
    fill_rect(pix, x, y, w, 1, c);
    fill_rect(pix, x, y + h - 1, w, 1, c);
    fill_rect(pix, x, y, 1, h, c);
    fill_rect(pix, x + w - 1, y, 1, h, c);
}

/* scanline even-odd fill of screen-space poly (world coords * scale + off) */
static void fill_poly(uint32_t *pix, const Poly *p, float scale,
                      float offx, float offy, uint32_t c)
{
    int y0 = (int)floorf(p->miny * scale + offy);
    int y1 = (int)ceilf(p->maxy * scale + offy);
    if (y0 < 0) y0 = 0;
    if (y1 > SCREEN_H) y1 = SCREEN_H;
    for (int y = y0; y < y1; y++) {
        float fy = y + 0.5f;
        float xs[MAX_POLY_PTS];
        int nxs = 0;
        for (int i = 0, j = p->n - 1; i < p->n; j = i++) {
            float yi = p->y[i] * scale + offy, yj = p->y[j] * scale + offy;
            if ((yi > fy) == (yj > fy)) continue;
            float t = (fy - yi) / (yj - yi);
            float xi = p->x[i] * scale + offx, xj = p->x[j] * scale + offx;
            xs[nxs++] = xi + t * (xj - xi);
        }
        for (int i = 1; i < nxs; i++) { /* insertion sort, nxs is tiny */
            float v = xs[i];
            int k = i - 1;
            while (k >= 0 && xs[k] > v) { xs[k + 1] = xs[k]; k--; }
            xs[k + 1] = v;
        }
        for (int i = 0; i + 1 < nxs; i += 2) {
            int xa = (int)floorf(xs[i] + 0.5f);
            int xb = (int)floorf(xs[i + 1] + 0.5f);
            if (xa < 0) xa = 0;
            if (xb > SCREEN_W) xb = SCREEN_W;
            uint32_t *row = pix + (size_t)y * SCREEN_W;
            for (int x = xa; x < xb; x++)
                row[x] = c;
        }
    }
}

/* stamp small squares along a line */
static void stamp_line(uint32_t *pix, float x1, float y1, float x2, float y2,
                       int thick, uint32_t c)
{
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    int steps = (int)(len / (thick * 0.5f)) + 1;
    for (int s = 0; s <= steps; s++) {
        float t = (float)s / steps;
        fill_rect(pix, (int)(x1 + dx * t) - thick / 2,
                  (int)(y1 + dy * t) - thick / 2, thick, thick, c);
    }
}

/* outline for polys */
static void poly_edges(uint32_t *pix, const Poly *p, float offx, float offy,
                       int thick, uint32_t c)
{
    for (int i = 0, j = p->n - 1; i < p->n; j = i++)
        stamp_line(pix, p->x[j] + offx, p->y[j] + offy,
                   p->x[i] + offx, p->y[i] + offy, thick, c);
}

static bool poly_contains(const Poly *p, float x, float y)
{
    if (x < p->minx || x > p->maxx || y < p->miny || y > p->maxy)
        return false;
    bool in = false;
    for (int i = 0, j = p->n - 1; i < p->n; j = i++) {
        if ((p->y[i] > y) != (p->y[j] > y)) {
            float t = (y - p->y[i]) / (p->y[j] - p->y[i]);
            if (x < p->x[i] + t * (p->x[j] - p->x[i]))
                in = !in;
        }
    }
    return in;
}

/* multiply-darken inside a screen-space poly -- used for cast shadows */
static void darken_poly(uint32_t *pix, const Poly *p, float offx, float offy)
{
    int y0 = (int)floorf(p->miny + offy);
    int y1 = (int)ceilf(p->maxy + offy);
    if (y0 < 0) y0 = 0;
    if (y1 > SCREEN_H) y1 = SCREEN_H;
    for (int y = y0; y < y1; y++) {
        float fy = y + 0.5f;
        float xs[MAX_POLY_PTS];
        int nxs = 0;
        for (int i = 0, j = p->n - 1; i < p->n; j = i++) {
            float yi = p->y[i] + offy, yj = p->y[j] + offy;
            if ((yi > fy) == (yj > fy)) continue;
            float t = (fy - yi) / (yj - yi);
            xs[nxs++] = p->x[i] + offx + t * (p->x[j] - p->x[i]);
        }
        for (int i = 1; i < nxs; i++) {
            float v = xs[i];
            int k = i - 1;
            while (k >= 0 && xs[k] > v) { xs[k + 1] = xs[k]; k--; }
            xs[k + 1] = v;
        }
        for (int i = 0; i + 1 < nxs; i += 2) {
            int xa = (int)floorf(xs[i] + 0.5f);
            int xb = (int)floorf(xs[i + 1] + 0.5f);
            if (xa < 0) xa = 0;
            if (xb > SCREEN_W) xb = SCREEN_W;
            uint32_t *row = pix + (size_t)y * SCREEN_W;
            for (int x = xa; x < xb; x++)
                row[x] -= (row[x] >> 2) & 0x003F3F3F;
        }
    }
}

/* outward normal of edge j->i; false for degenerate edges */
static bool edge_normal(const Poly *p, int j, int i, float *nx, float *ny)
{
    float dx = p->x[i] - p->x[j], dy = p->y[i] - p->y[j];
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f)
        return false;
    *nx = dy / len;
    *ny = -dx / len;
    float mx = (p->x[i] + p->x[j]) * 0.5f + *nx * 1.5f;
    float my = (p->y[i] + p->y[j]) * 0.5f + *ny * 1.5f;
    if (poly_contains(p, mx, my)) { *nx = -*nx; *ny = -*ny; }
    return true;
}

/* multiply-darken n steps, ~12.5% each -- storey shading on tall faces */
static uint32_t shade_down(uint32_t c, int n)
{
    while (n-- > 0)
        c -= (c >> 3) & 0x001F1F1F;
    return c;
}

/* world rect -> screen */
static void world_rect(const Game *g, const Rect *r,
                       int *x, int *y, int *w, int *h)
{
    *x = (int)floorf(r->x - g->cam_x);
    *y = (int)floorf(r->y - g->cam_y);
    *w = (int)ceilf(r->w);
    *h = (int)ceilf(r->h);
}

/* 3x5 digits for the score */
static const uint8_t FONT[10][5] = {
    {7,5,5,5,7}, {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1},
    {7,4,7,1,7}, {7,4,7,5,7}, {7,1,1,1,1}, {7,5,7,5,7}, {7,5,7,1,7},
};

static void draw_number(uint32_t *pix, int x, int y, int scale, int n,
                        uint32_t c)
{
    char buf[12];
    int len = 0;
    if (n < 0) n = 0;
    do { buf[len++] = '0' + n % 10; n /= 10; } while (n && len < 11);
    for (int d = len - 1; d >= 0; d--) {
        const uint8_t *glyph = FONT[buf[d] - '0'];
        for (int r = 0; r < 5; r++)
            for (int b = 0; b < 3; b++)
                if (glyph[r] & (4 >> b))
                    fill_rect(pix, x + b * scale, y + r * scale,
                              scale, scale, c);
        x += 4 * scale;
    }
}

static void draw_minimap(const Game *g, uint32_t *pix)
{
    const int MM_W = 220;
    float s = MM_W / g->map_w;
    int mm_h = (int)(g->map_h * s);
    int ox = SCREEN_W - MM_W - 16, oy = 16;

    fill_rect(pix, ox - 2, oy - 2, MM_W + 4, mm_h + 4, 0xFF0C0D10);
    for (int i = 0; i < g->nterrain; i++) {
        uint32_t c = TERRAIN_COL[g->terrain[i].type];
        c = 0xFF000000 | ((c >> 1) & 0x007F7F7F);
        fill_poly(pix, &g->terrain[i].p, s, ox, oy, c);
    }
    for (int lvl = LEVEL_LOW; lvl <= LEVEL_HIGH; lvl++)
        for (int i = 0; i < g->nwalls; i++)
            if (g->walls[i].level == lvl)
                fill_poly(pix, &g->walls[i].p, s, ox, oy, WALL_MM[lvl]);
    for (int i = 0; i < g->nbarrels; i++) {
        if (!g->barrels[i].alive) continue;
        fill_rect(pix, ox + (int)(g->barrels[i].x * s) - 1,
                  oy + (int)(g->barrels[i].y * s) - 1, 3, 3, 0xFFD08A45);
    }
    outline_rect(pix, ox + (int)(g->cam_x * s), oy + (int)(g->cam_y * s),
                 (int)(SCREEN_W * s), (int)(SCREEN_H * s), 0xFF787E8C);
    fill_rect(pix, ox + (int)(g->px * s) - 2, oy + (int)(g->py * s) - 2,
              4, 4, 0xFFFFFFFF);
}

/* ------------------------------------------------------- 2.5D drawing */

static void set_bbox(Poly *p)
{
    p->minx = p->maxx = p->x[0];
    p->miny = p->maxy = p->y[0];
    for (int i = 1; i < p->n; i++) {
        if (p->x[i] < p->minx) p->minx = p->x[i];
        if (p->x[i] > p->maxx) p->maxx = p->x[i];
        if (p->y[i] < p->miny) p->miny = p->y[i];
        if (p->y[i] > p->maxy) p->maxy = p->y[i];
    }
}

static void draw_wall(const Game *g, const Wall *w, uint32_t *pix)
{
    float h = w->level * LEVEL_STEP;
    const Poly *p = &w->p;
    Poly tmp;

    /* south-facing side faces, extruded one storey band per level so the
     * height is countable; bands darken toward the ground */
    for (int i = 0, j = p->n - 1; i < p->n; j = i++) {
        float nx, ny;
        if (!edge_normal(p, j, i, &nx, &ny) || ny <= 0.01f)
            continue; /* back face or edge-on */
        for (int s = 0; s < w->level; s++) {
            float bot = s * LEVEL_STEP, top = bot + LEVEL_STEP;
            tmp.n = 4;
            tmp.x[0] = p->x[j]; tmp.y[0] = p->y[j] - bot;
            tmp.x[1] = p->x[i]; tmp.y[1] = p->y[i] - bot;
            tmp.x[2] = p->x[i]; tmp.y[2] = p->y[i] - top;
            tmp.x[3] = p->x[j]; tmp.y[3] = p->y[j] - top;
            set_bbox(&tmp);
            fill_poly(pix, &tmp, 1.0f, -g->cam_x, -g->cam_y,
                      shade_down(WALL_FACE[w->level], w->level - 1 - s));
            if (s < w->level - 1) /* seam between storeys */
                stamp_line(pix, p->x[j] - g->cam_x, p->y[j] - top - g->cam_y,
                           p->x[i] - g->cam_x, p->y[i] - top - g->cam_y,
                           2, COL_SEAM);
        }
        /* contact line where the face meets the ground */
        stamp_line(pix, p->x[j] - g->cam_x, p->y[j] - g->cam_y,
                   p->x[i] - g->cam_x, p->y[i] - g->cam_y, 2, COL_SEAM);
    }

    /* lit top, raised by the wall height */
    tmp = *p;
    for (int i = 0; i < tmp.n; i++)
        tmp.y[i] -= h;
    tmp.miny -= h;
    tmp.maxy -= h;
    fill_poly(pix, &tmp, 1.0f, -g->cam_x, -g->cam_y, WALL_COL[w->level]);
    poly_edges(pix, &tmp, -g->cam_x, -g->cam_y, 3, WALL_EDGE[w->level]);
}

/* cylinder standing at screen (cx, base_y): bottom bulge, body, lid */
static void draw_cylinder(uint32_t *pix, int cx, int base_y, int r, int h,
                          uint32_t side, uint32_t side_hi,
                          uint32_t rim, uint32_t lid)
{
    fill_circle(pix, cx, base_y, r, side);
    fill_rect(pix, cx - r, base_y - h, 2 * r + 1, h + 1, side);
    fill_rect(pix, cx - r + 3, base_y - h + 2, 3, h + r / 2, side_hi);
    fill_circle(pix, cx, base_y - h, r, rim);
    fill_circle(pix, cx, base_y - h, r - 3, lid);
}

static void draw_shadow(uint32_t *pix, int cx, int floor_y, float lift,
                        int base_r)
{
    int r = base_r - (int)(lift * 0.05f);
    if (r < 6) r = 6;
    fill_circle(pix, cx, floor_y, r, COL_SHADOW);
}

static void draw_barrel(const Game *g, const Barrel *b, uint32_t *pix)
{
    int cx = (int)(b->x - g->cam_x), gy = (int)(b->y - g->cam_y);
    int hp = b->hp < 1 ? 1 : (b->hp > 3 ? 3 : b->hp);
    float floorz = b->level * LEVEL_STEP;
    draw_shadow(pix, cx, gy - (int)floorz, b->z - floorz, 16);
    int base = gy - (int)b->z;
    draw_cylinder(pix, cx, base, (int)BARREL_R, BARREL_H,
                  COL_BARREL_SIDE, COL_BARREL_HI,
                  COL_BARREL_RIM, BARREL_COL[hp - 1]);
    fill_circle(pix, cx, base - BARREL_H, 11, COL_BARREL_RIM);
    fill_circle(pix, cx, base - BARREL_H, 8, BARREL_COL[hp - 1]);
}

static void draw_player(const Game *g, uint32_t *pix)
{
    int cx = (int)(g->px - g->cam_x), gy = (int)(g->py - g->cam_y);
    draw_shadow(pix, cx, gy - (int)g->pfloor, g->pz - g->pfloor, 14);
    int base = gy - (int)g->pz;
    draw_cylinder(pix, cx, base, (int)PLAYER_R, PLAYER_H,
                  COL_PLAYER_SIDE, COL_PLAYER_HI,
                  COL_PLAYER_RIM, COL_PLAYER_TOP);
    int ty = base - PLAYER_H + 4; /* gun leaves from just below the lid */
    for (float t = 10; t <= 30; t += 2)
        fill_circle(pix, cx + (int)(g->aim_x * t),
                    ty + (int)(g->aim_y * t), 4, COL_GUN);
}

/* does a disc of radius r touch the polygon? (matches game.c support) */
static bool poly_touches_disc(const Poly *p, float cx, float cy, float r)
{
    if (cx < p->minx - r || cx > p->maxx + r ||
        cy < p->miny - r || cy > p->maxy + r)
        return false;
    if (poly_contains(p, cx, cy))
        return true;
    for (int i = 0, j = p->n - 1; i < p->n; j = i++) {
        float ax = p->x[j], ay = p->y[j], bx = p->x[i], by = p->y[i];
        float dx = bx - ax, dy = by - ay;
        float len2 = dx * dx + dy * dy;
        float t = len2 > 0 ? ((cx - ax) * dx + (cy - ay) * dy) / len2 : 0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        float qx = ax + t * dx - cx, qy = ay + t * dy - cy;
        if (qx * qx + qy * qy <= r * r)
            return true;
    }
    return false;
}

/* painter key: the southmost ground line of whatever supports the body;
 * a disc test so bodies overhanging an edge still sort onto their wall */
static float entity_key(const Game *g, float x, float y, float z)
{
    float key = y;
    for (int i = 0; i < g->nwalls; i++) {
        const Wall *w = &g->walls[i];
        if (w->level * LEVEL_STEP <= z + 1.0f &&
            poly_touches_disc(&w->p, x, y, 10.0f) && w->p.maxy + 0.5f > key)
            key = w->p.maxy + 0.5f;
    }
    return key;
}

enum { IT_WALL, IT_BARREL, IT_BULLET, IT_FRAG, IT_PLAYER };

typedef struct { float key; int kind, idx; } Item;

static int item_cmp(const void *pa, const void *pb)
{
    const Item *a = pa, *b = pb;
    return (a->key > b->key) - (a->key < b->key);
}

void game_render(const Game *g, uint32_t *pix)
{
    /* terrain: grass base, then patches */
    for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++)
        pix[i] = COL_GRASS;
    for (int i = 0; i < g->nterrain; i++)
        fill_poly(pix, &g->terrain[i].p, 1.0f, -g->cam_x, -g->cam_y,
                  TERRAIN_COL[g->terrain[i].type]);

    /* world-aligned grid for motion feedback */
    const int GRID = 160;
    for (float gx = floorf(g->cam_x / GRID) * GRID; gx < g->cam_x + SCREEN_W;
         gx += GRID)
        darken_rect(pix, (int)(gx - g->cam_x), 0, 2, SCREEN_H);
    for (float gy = floorf(g->cam_y / GRID) * GRID; gy < g->cam_y + SCREEN_H;
         gy += GRID)
        darken_rect(pix, 0, (int)(gy - g->cam_y), SCREEN_W, 2);

    /* walls cast SE ground shadows, longer the taller the wall; the
     * quads from adjacent edges share the same offset so they tile
     * without double-darkening */
    for (int i = 0; i < g->nwalls; i++) {
        const Wall *w = &g->walls[i];
        float h = w->level * LEVEL_STEP;
        float ox = h * 0.25f, oy = h * 0.45f;
        const Poly *p = &w->p;
        if (p->maxx + ox < g->cam_x || p->minx > g->cam_x + SCREEN_W ||
            p->maxy + oy < g->cam_y || p->miny > g->cam_y + SCREEN_H)
            continue;
        for (int a = 0, b = p->n - 1; a < p->n; b = a++) {
            float nx, ny;
            if (!edge_normal(p, b, a, &nx, &ny))
                continue;
            if (nx * ox + ny * oy <= 0.01f)
                continue; /* faces toward the light: casts no shadow */
            Poly q;
            q.n = 4;
            q.x[0] = p->x[b];      q.y[0] = p->y[b];
            q.x[1] = p->x[a];      q.y[1] = p->y[a];
            q.x[2] = p->x[a] + ox; q.y[2] = p->y[a] + oy;
            q.x[3] = p->x[b] + ox; q.y[3] = p->y[b] + oy;
            set_bbox(&q);
            darken_poly(pix, &q, -g->cam_x, -g->cam_y);
        }
    }

    /* collect visible drawables and painter-sort back to front */
    static Item items[1 + MAX_WALLS + MAX_BARRELS + MAX_BULLETS +
                      MAX_FRAGMENTS];
    int n = 0;
    for (int i = 0; i < g->nwalls; i++) {
        const Poly *p = &g->walls[i].p;
        float h = g->walls[i].level * LEVEL_STEP;
        if (p->maxx < g->cam_x || p->minx > g->cam_x + SCREEN_W ||
            p->maxy < g->cam_y || p->miny - h > g->cam_y + SCREEN_H)
            continue;
        items[n++] = (Item){p->maxy + 0.001f * g->walls[i].level, IT_WALL, i};
    }
    for (int i = 0; i < g->nbarrels; i++) {
        const Barrel *b = &g->barrels[i];
        if (!b->alive) continue;
        items[n++] = (Item){entity_key(g, b->x, b->y, b->z), IT_BARREL, i};
    }
    for (int i = 0; i < MAX_BULLETS; i++) {
        const Bullet *b = &g->bullets[i];
        if (!b->alive) continue;
        items[n++] = (Item){
            entity_key(g, b->x, b->y, b->level * LEVEL_STEP + BULLET_Z),
            IT_BULLET, i};
    }
    for (int i = 0; i < MAX_FRAGMENTS; i++) {
        const Fragment *fr = &g->frags[i];
        if (!fr->alive) continue;
        items[n++] = (Item){
            entity_key(g, fr->r.x + fr->r.w / 2, fr->r.y + fr->r.h / 2,
                       fr->level * LEVEL_STEP),
            IT_FRAG, i};
    }
    float pkey = entity_key(g, g->px, g->py, g->pz);
    items[n++] = (Item){pkey, IT_PLAYER, 0};
    qsort(items, n, sizeof(Item), item_cmp);

    for (int it = 0; it < n; it++) {
        switch (items[it].kind) {
        case IT_WALL:
            draw_wall(g, &g->walls[items[it].idx], pix);
            break;
        case IT_BARREL:
            draw_barrel(g, &g->barrels[items[it].idx], pix);
            break;
        case IT_BULLET: {
            const Bullet *b = &g->bullets[items[it].idx];
            fill_circle(pix, (int)(b->x - g->cam_x),
                        (int)(b->y - g->cam_y) -
                            (int)(b->level * LEVEL_STEP + BULLET_Z),
                        3, COL_BULLET);
            break;
        }
        case IT_FRAG: {
            const Fragment *fr = &g->frags[items[it].idx];
            int x, y, w, h;
            world_rect(g, &fr->r, &x, &y, &w, &h);
            fill_rect(pix, x, y - (int)(fr->level * LEVEL_STEP), w, h,
                      fr->color);
            break;
        }
        case IT_PLAYER:
            draw_player(g, pix);
            break;
        }
    }

    /* x-ray ring when a wall drawn after the player covers the body */
    {
        int cx = (int)(g->px - g->cam_x);
        int by = (int)(g->py - g->cam_y) - (int)g->pz;
        float bx0 = g->px - PLAYER_R, bx1 = g->px + PLAYER_R;
        float by0 = g->py - g->pz - PLAYER_H - PLAYER_R;
        float by1 = g->py - g->pz + PLAYER_R;
        for (int i = 0; i < g->nwalls; i++) {
            const Wall *w = &g->walls[i];
            float wkey = w->p.maxy + 0.001f * w->level;
            float h = w->level * LEVEL_STEP;
            if (wkey <= pkey)
                continue;
            if (bx1 < w->p.minx || bx0 > w->p.maxx ||
                by1 < w->p.miny - h || by0 > w->p.maxy)
                continue;
            fill_ring(pix, cx, by - PLAYER_H / 2, (int)PLAYER_R + 3, 3,
                      COL_PLAYER_RIM);
            break;
        }
    }

    /* crosshair: dark backing then light cross, with a center gap */
    {
        int cx = (int)g->cursor_x, cy = (int)g->cursor_y;
        darken_rect(pix, cx - 12, cy - 2, 24, 4);
        darken_rect(pix, cx - 2, cy - 12, 4, 24);
        fill_rect(pix, cx - 10, cy - 1, 6, 2, 0xFFF0F0F0);
        fill_rect(pix, cx + 4, cy - 1, 6, 2, 0xFFF0F0F0);
        fill_rect(pix, cx - 1, cy - 10, 2, 6, 0xFFF0F0F0);
        fill_rect(pix, cx - 1, cy + 4, 2, 6, 0xFFF0F0F0);
    }

    draw_minimap(g, pix);
    draw_number(pix, 18, 18, 4, g->score, 0xFF101216);
    draw_number(pix, 16, 16, 4, g->score, 0xFFF0F0F0);
}
