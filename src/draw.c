#include "game.h"
#include <math.h>
#include <stddef.h>

#define COL_GRASS  0xFF3B5E3B
#define COL_SAND   0xFFC0A45E
#define COL_WATER  0xFF2A5E86
#define COL_ICE    0xFFB9D4DE
#define COL_WALL   0xFF262931
#define COL_WALL_HI 0xFF3C4150
#define COL_BULLET 0xFFFFE066
#define COL_PLAYER 0xFFEDEFF4
#define COL_PLAYER_EDGE 0xFF9AA0AC
#define COL_BARREL 0xFF565C68

static const uint32_t TERRAIN_COL[TERRAIN_COUNT] = {
    [TERRAIN_GRASS] = COL_GRASS,
    [TERRAIN_SAND]  = COL_SAND,
    [TERRAIN_WATER] = COL_WATER,
    [TERRAIN_ICE]   = COL_ICE,
};

static const uint32_t CRATE_COL[3] = {0xFF83501F, 0xFF9C5F27, 0xFFB4712F};

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

/* stamp small squares along each edge -- outline for wall polys */
static void poly_edges(uint32_t *pix, const Poly *p, float offx, float offy,
                       int thick, uint32_t c)
{
    for (int i = 0, j = p->n - 1; i < p->n; j = i++) {
        float x1 = p->x[j] + offx, y1 = p->y[j] + offy;
        float x2 = p->x[i] + offx, y2 = p->y[i] + offy;
        float dx = x2 - x1, dy = y2 - y1;
        float len = sqrtf(dx * dx + dy * dy);
        int steps = (int)(len / (thick * 0.5f)) + 1;
        for (int s = 0; s <= steps; s++) {
            float t = (float)s / steps;
            fill_rect(pix, (int)(x1 + dx * t) - thick / 2,
                      (int)(y1 + dy * t) - thick / 2, thick, thick, c);
        }
    }
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
        /* halve brightness so the minimap stays quiet */
        c = 0xFF000000 | ((c >> 1) & 0x007F7F7F);
        fill_poly(pix, &g->terrain[i].p, s, ox, oy, c);
    }
    for (int i = 0; i < g->nwalls; i++)
        fill_poly(pix, &g->walls[i], s, ox, oy, 0xFF5A5F6A);
    for (int i = 0; i < g->ncrates; i++) {
        if (!g->crates[i].alive) continue;
        fill_rect(pix, ox + (int)(g->crates[i].r.x * s) - 1,
                  oy + (int)(g->crates[i].r.y * s) - 1, 3, 3, 0xFFD08A45);
    }
    outline_rect(pix, ox + (int)(g->cam_x * s), oy + (int)(g->cam_y * s),
                 (int)(SCREEN_W * s), (int)(SCREEN_H * s), 0xFF787E8C);
    fill_rect(pix, ox + (int)(g->px * s) - 2, oy + (int)(g->py * s) - 2,
              4, 4, 0xFFFFFFFF);
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

    /* walls: dark fill, lighter edge */
    for (int i = 0; i < g->nwalls; i++) {
        const Poly *p = &g->walls[i];
        if (p->maxx < g->cam_x || p->minx > g->cam_x + SCREEN_W ||
            p->maxy < g->cam_y || p->miny > g->cam_y + SCREEN_H)
            continue;
        fill_poly(pix, p, 1.0f, -g->cam_x, -g->cam_y, COL_WALL);
        poly_edges(pix, p, -g->cam_x, -g->cam_y, 3, COL_WALL_HI);
    }

    /* crates, tinted darker as they take damage */
    for (int i = 0; i < g->ncrates; i++) {
        const Crate *c = &g->crates[i];
        if (!c->alive) continue;
        int x, y, w, h;
        world_rect(g, &c->r, &x, &y, &w, &h);
        int hp = c->hp < 1 ? 1 : (c->hp > 3 ? 3 : c->hp);
        fill_rect(pix, x, y, w, h, 0xFF5C3A17);
        fill_rect(pix, x + 3, y + 3, w - 6, h - 6, CRATE_COL[hp - 1]);
        darken_rect(pix, x + w / 2 - 2, y + 3, 4, h - 6);
        darken_rect(pix, x + 3, y + h / 2 - 2, w - 6, 4);
    }

    /* fragments */
    for (int i = 0; i < MAX_FRAGMENTS; i++) {
        const Fragment *fr = &g->frags[i];
        if (!fr->alive) continue;
        int x, y, w, h;
        world_rect(g, &fr->r, &x, &y, &w, &h);
        fill_rect(pix, x, y, w, h, fr->color);
    }

    /* bullets */
    for (int i = 0; i < MAX_BULLETS; i++) {
        const Bullet *b = &g->bullets[i];
        if (!b->alive) continue;
        fill_rect(pix, (int)(b->x - g->cam_x) - 3, (int)(b->y - g->cam_y) - 3,
                  6, 6, COL_BULLET);
    }

    /* player: barrel first, body on top */
    {
        int cx = (int)(g->px - g->cam_x), cy = (int)(g->py - g->cam_y);
        for (float t = 8; t <= 30; t += 2)
            fill_rect(pix, cx + (int)(g->aim_x * t) - 4,
                      cy + (int)(g->aim_y * t) - 4, 8, 8, COL_BARREL);
        int hs = (int)(PLAYER_SIZE / 2);
        fill_rect(pix, cx - hs, cy - hs, (int)PLAYER_SIZE, (int)PLAYER_SIZE,
                  COL_PLAYER_EDGE);
        fill_rect(pix, cx - hs + 3, cy - hs + 3, (int)PLAYER_SIZE - 6,
                  (int)PLAYER_SIZE - 6, COL_PLAYER);
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
