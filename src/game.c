#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define JUMP_V   560.0f  /* apex ~90 px, airtime ~0.64 s */
#define GRAVITY 1750.0f

/* per-terrain feel: player acceleration, player top speed, player drag,
 * and drag applied to loose objects (barrels/fragments) resting on it */
static const struct { float accel, maxspd, drag, obj_drag; } TPHYS[TERRAIN_COUNT] = {
    [TERRAIN_GRASS] = { 2600.0f, 430.0f,  7.0f, 2.2f  },
    [TERRAIN_SAND]  = { 1500.0f, 290.0f,  9.0f, 5.0f  },
    [TERRAIN_WATER] = { 1100.0f, 200.0f, 10.0f, 4.5f  },
    [TERRAIN_ICE]   = { 1000.0f, 540.0f,  1.1f, 0.35f },
};

static float frand(Game *g)
{
    uint32_t x = g->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g->rng = x;
    return (float)(x >> 8) / 16777216.0f;
}

/* ------------------------------------------------------------- geometry */

static void poly_finish(Poly *p)
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

static float cross3(float ax, float ay, float bx, float by, float cx, float cy)
{
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

static bool segs_cross(float ax, float ay, float bx, float by,
                       float cx, float cy, float dx, float dy)
{
    float d1 = cross3(cx, cy, dx, dy, ax, ay);
    float d2 = cross3(cx, cy, dx, dy, bx, by);
    float d3 = cross3(ax, ay, bx, by, cx, cy);
    float d4 = cross3(ax, ay, bx, by, dx, dy);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

static bool poly_hits_rect(const Poly *p, const Rect *r)
{
    if (r->x > p->maxx || r->x + r->w < p->minx ||
        r->y > p->maxy || r->y + r->h < p->miny)
        return false;
    if (poly_contains(p, r->x, r->y) ||
        poly_contains(p, r->x + r->w, r->y) ||
        poly_contains(p, r->x, r->y + r->h) ||
        poly_contains(p, r->x + r->w, r->y + r->h))
        return true;
    for (int i = 0; i < p->n; i++)
        if (p->x[i] >= r->x && p->x[i] <= r->x + r->w &&
            p->y[i] >= r->y && p->y[i] <= r->y + r->h)
            return true;
    float rx[4] = {r->x, r->x + r->w, r->x + r->w, r->x};
    float ry[4] = {r->y, r->y, r->y + r->h, r->y + r->h};
    for (int i = 0, j = p->n - 1; i < p->n; j = i++)
        for (int k = 0, l = 3; k < 4; l = k++)
            if (segs_cross(p->x[i], p->y[i], p->x[j], p->y[j],
                           rx[k], ry[k], rx[l], ry[l]))
                return true;
    return false;
}

/* closest point on segment ab to p, returned in (qx,qy); returns dist^2 */
static float seg_closest(float px, float py, float ax, float ay,
                         float bx, float by, float *qx, float *qy)
{
    float dx = bx - ax, dy = by - ay;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    *qx = ax + t * dx;
    *qy = ay + t * dy;
    return (*qx - px) * (*qx - px) + (*qy - py) * (*qy - py);
}

static float seg_dist2(float px, float py, float ax, float ay,
                       float bx, float by)
{
    float qx, qy;
    return seg_closest(px, py, ax, ay, bx, by, &qx, &qy);
}

static bool poly_hits_circle(const Poly *p, float cx, float cy, float r)
{
    if (cx < p->minx - r || cx > p->maxx + r ||
        cy < p->miny - r || cy > p->maxy + r)
        return false;
    if (poly_contains(p, cx, cy))
        return true;
    for (int i = 0, j = p->n - 1; i < p->n; j = i++)
        if (seg_dist2(cx, cy, p->x[i], p->y[i], p->x[j], p->y[j]) <= r * r)
            return true;
    return false;
}

/* ------------------------------------------------------- height queries */

/* highest wall top under a point; 0 = ground */
static int level_at(const Game *g, float x, float y)
{
    int lvl = LEVEL_GROUND;
    for (int i = 0; i < g->nwalls; i++)
        if (g->walls[i].level > lvl && poly_contains(&g->walls[i].p, x, y))
            lvl = g->walls[i].level;
    return lvl;
}

/* a circle body at `level` is blocked by any taller wall; there are no
 * edge fences -- walking off a wall just drops you onto what's below */
static bool circle_blocked(const Game *g, float x, float y, float r, int level)
{
    for (int i = 0; i < g->nwalls; i++)
        if (g->walls[i].level > level &&
            poly_hits_circle(&g->walls[i].p, x, y, r))
            return true;
    return false;
}

/* the player's feet height decides which wall faces are solid; airborne
 * that height follows the jump arc, so "jump one step up" falls out of
 * the physics (the arc tops out below the two-step wall face) */
static float player_feet(const Game *g)
{
    return (g->airborne ? g->pz : g->plevel * LEVEL_STEP) + 1.0f;
}

static bool player_blocked(const Game *g, float x, float y)
{
    float feet = player_feet(g);
    for (int i = 0; i < g->nwalls; i++)
        if (g->walls[i].level * LEVEL_STEP > feet &&
            poly_hits_circle(&g->walls[i].p, x, y, PLAYER_R))
            return true;
    return false;
}

/* outward normal at the nearest blocking wall edge, for sliding */
static bool contact_normal(const Game *g, float x, float y,
                           float *nx, float *ny)
{
    float feet = player_feet(g);
    float best = 1e30f, bqx = 0, bqy = 0;
    float reach = PLAYER_R + 4.0f;
    for (int i = 0; i < g->nwalls; i++) {
        const Poly *p = &g->walls[i].p;
        if (g->walls[i].level * LEVEL_STEP <= feet)
            continue;
        if (x < p->minx - reach || x > p->maxx + reach ||
            y < p->miny - reach || y > p->maxy + reach)
            continue;
        for (int a = 0, b = p->n - 1; a < p->n; b = a++) {
            float qx, qy;
            float d2 = seg_closest(x, y, p->x[a], p->y[a], p->x[b], p->y[b],
                                   &qx, &qy);
            if (d2 < best) { best = d2; bqx = qx; bqy = qy; }
        }
    }
    if (best >= reach * reach || best < 0.0001f)
        return false;
    float d = sqrtf(best);
    *nx = (x - bqx) / d;
    *ny = (y - bqy) / d;
    return true;
}

/* bullets fly at the shooter's level and clear anything at or below it */
static bool point_in_wall(const Game *g, float x, float y, int level)
{
    for (int i = 0; i < g->nwalls; i++)
        if (g->walls[i].level > level &&
            poly_contains(&g->walls[i].p, x, y))
            return true;
    return false;
}

static bool rect_hits_wall(const Game *g, const Rect *r, int level)
{
    for (int i = 0; i < g->nwalls; i++)
        if (g->walls[i].level > level &&
            poly_hits_rect(&g->walls[i].p, r))
            return true;
    return false;
}

static TerrainType terrain_at(const Game *g, float x, float y)
{
    TerrainType t = TERRAIN_GRASS;
    for (int i = 0; i < g->nterrain; i++)
        if (poly_contains(&g->terrain[i].p, x, y))
            t = g->terrain[i].type; /* last patch in the file wins */
    return t;
}

/* ---------------------------------------------------------------- map */

static TerrainType terrain_by_name(const char *s)
{
    if (!strcmp(s, "sand"))  return TERRAIN_SAND;
    if (!strcmp(s, "water")) return TERRAIN_WATER;
    if (!strcmp(s, "ice"))   return TERRAIN_ICE;
    return TERRAIN_GRASS;
}

static int wall_level_by_name(const char *s)
{
    if (!strcmp(s, "low"))    return LEVEL_LOW;
    if (!strcmp(s, "medium")) return LEVEL_MEDIUM;
    if (!strcmp(s, "high"))   return LEVEL_HIGH;
    return 0; /* not a wall kind */
}

static void rect_poly(Poly *p, float x, float y, float w, float h)
{
    p->n = 4;
    p->x[0] = x;     p->y[0] = y;
    p->x[1] = x + w; p->y[1] = y;
    p->x[2] = x + w; p->y[2] = y + h;
    p->x[3] = x;     p->y[3] = y + h;
    poly_finish(p);
}

static void add_wall(Game *g, int level, const Poly *p)
{
    if (g->nwalls < MAX_WALLS)
        g->walls[g->nwalls++] = (Wall){level, *p};
}

static void add_shape(Game *g, const char *name, const Poly *p)
{
    int lvl = wall_level_by_name(name);
    if (lvl > 0) {
        add_wall(g, lvl, p);
    } else if (!strcmp(name, "wall")) {
        add_wall(g, LEVEL_HIGH, p); /* legacy walls are unjumpable */
    } else {
        if (g->nterrain < MAX_TERRAIN)
            g->terrain[g->nterrain++] =
                (TerrainPatch){terrain_by_name(name), *p};
    }
}

bool game_load_map(Game *g, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "crateblast: cannot open map '%s'\n", path);
        return false;
    }
    memset(g, 0, sizeof(*g));
    g->rng = 0x1234abcdu;
    g->map_w = SCREEN_W;
    g->map_h = SCREEN_H;
    g->px = SCREEN_W / 2.0f;
    g->py = SCREEN_H / 2.0f;
    g->aim_x = 1.0f;
    g->cursor_x = SCREEN_W / 2.0f;
    g->cursor_y = SCREEN_H / 2.0f;

    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        float x, y, w, h;
        char name[32];
        int off;
        Poly p;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
            continue;
        if (sscanf(line, "size %f %f", &w, &h) == 2) {
            g->map_w = w; g->map_h = h;
        } else if (sscanf(line, "spawn %f %f", &x, &y) == 2) {
            g->px = x; g->py = y;
        } else if (sscanf(line, "wall %31s %f %f %f %f",
                          name, &x, &y, &w, &h) == 5 &&
                   wall_level_by_name(name) > 0) {
            rect_poly(&p, x, y, w, h);
            add_wall(g, wall_level_by_name(name), &p);
        } else if (sscanf(line, "wall %f %f %f %f", &x, &y, &w, &h) == 4) {
            rect_poly(&p, x, y, w, h);
            add_wall(g, LEVEL_HIGH, &p); /* legacy: no height keyword */
        } else if (sscanf(line, "terrain %31s %f %f %f %f",
                          name, &x, &y, &w, &h) == 5) {
            rect_poly(&p, x, y, w, h);
            add_shape(g, name, &p);
        } else if (sscanf(line, "poly %31s%n", name, &off) == 1) {
            char *ptr = line + off, *end;
            p.n = 0;
            while (p.n < MAX_POLY_PTS) {
                x = strtof(ptr, &end);
                if (end == ptr) break;
                ptr = end;
                y = strtof(ptr, &end);
                if (end == ptr) break;
                ptr = end;
                p.x[p.n] = x; p.y[p.n] = y; p.n++;
            }
            if (p.n >= 3) {
                poly_finish(&p);
                add_shape(g, name, &p);
            } else {
                fprintf(stderr, "crateblast: %s:%d: poly needs >= 3 points\n",
                        path, lineno);
            }
        } else if (sscanf(line, "crate %f %f", &x, &y) == 2 ||
                   sscanf(line, "barrel %f %f", &x, &y) == 2) {
            if (g->nbarrels < MAX_BARRELS)
                g->barrels[g->nbarrels++] =
                    (Barrel){x, y, 0, 0, 0, 0, 3, 0, true};
        } else {
            fprintf(stderr, "crateblast: %s:%d: skipping unrecognized line\n",
                    path, lineno);
        }
    }
    fclose(f);

    /* everything rests on whatever the map put underneath it */
    g->plevel = level_at(g, g->px, g->py);
    g->pz = g->pfloor = g->plevel * LEVEL_STEP;
    for (int i = 0; i < g->nbarrels; i++) {
        Barrel *b = &g->barrels[i];
        b->level = level_at(g, b->x, b->y);
        b->z = b->level * LEVEL_STEP;
    }

    g->cam_x = g->px - SCREEN_W / 2.0f;
    g->cam_y = g->py - SCREEN_H / 2.0f;
    return true;
}

/* ---------------------------------------------------------- collision */

/* Advance one coordinate of a circle body by v*dt. On contact, bisect to
 * the contact point and reflect the velocity scaled by -rest. A body that
 * starts blocked moves freely so it can drift out. */
static void move_circle_axis(const Game *g, float *cx, float *cy, bool xaxis,
                             float *v, float dt, float rest, float r,
                             int level)
{
    float *coord = xaxis ? cx : cy;
    float delta = *v * dt;
    if (delta == 0)
        return;
    float old = *coord;
    bool was_stuck = circle_blocked(g, *cx, *cy, r, level);
    *coord = old + delta;
    if (was_stuck || !circle_blocked(g, *cx, *cy, r, level))
        return;
    float lo = 0, hi = 1;
    for (int i = 0; i < 8; i++) {
        float mid = (lo + hi) * 0.5f;
        *coord = old + delta * mid;
        if (circle_blocked(g, *cx, *cy, r, level)) hi = mid; else lo = mid;
    }
    *coord = old + delta * lo;
    *v *= -rest;
}

static void move_circle(const Game *g, float *x, float *y, float *vx,
                        float *vy, float dt, float rest, float r, int level)
{
    move_circle_axis(g, x, y, true, vx, dt, rest, r, level);
    if (*x < r) { *x = r; *vx *= -rest; }
    if (*x > g->map_w - r) { *x = g->map_w - r; *vx *= -rest; }

    move_circle_axis(g, x, y, false, vy, dt, rest, r, level);
    if (*y < r) { *y = r; *vy *= -rest; }
    if (*y > g->map_h - r) { *y = g->map_h - r; *vy *= -rest; }
}

/* Collide-and-slide for the player: move to the contact point, project
 * the velocity onto the wall tangent, and spend the leftover time along
 * it. Hitting a wall face-on nearly stops you; a glancing angle keeps
 * most of your speed. */
static void move_player(Game *g, float dt)
{
    if (player_blocked(g, g->px, g->py)) { /* stuck escape: move freely */
        g->px += g->pvx * dt;
        g->py += g->pvy * dt;
    } else {
        float frac = 1.0f;
        for (int iter = 0; iter < 3 && frac > 0.001f; iter++) {
            float dx = g->pvx * dt * frac, dy = g->pvy * dt * frac;
            if (fabsf(dx) < 0.0001f && fabsf(dy) < 0.0001f)
                break;
            if (!player_blocked(g, g->px + dx, g->py + dy)) {
                g->px += dx;
                g->py += dy;
                break;
            }
            float lo = 0, hi = 1;
            for (int i = 0; i < 8; i++) {
                float mid = (lo + hi) * 0.5f;
                if (player_blocked(g, g->px + dx * mid, g->py + dy * mid))
                    hi = mid;
                else
                    lo = mid;
            }
            g->px += dx * lo;
            g->py += dy * lo;
            frac *= 1.0f - lo;
            float nx, ny;
            if (!contact_normal(g, g->px, g->py, &nx, &ny)) {
                g->pvx = g->pvy = 0;
                break;
            }
            float vn = g->pvx * nx + g->pvy * ny;
            if (vn < 0) { /* drop the into-wall component, keep tangent */
                g->pvx -= vn * nx;
                g->pvy -= vn * ny;
            }
        }
    }
    if (g->px < PLAYER_R) { g->px = PLAYER_R; if (g->pvx < 0) g->pvx = 0; }
    if (g->px > g->map_w - PLAYER_R) {
        g->px = g->map_w - PLAYER_R;
        if (g->pvx > 0) g->pvx = 0;
    }
    if (g->py < PLAYER_R) { g->py = PLAYER_R; if (g->pvy < 0) g->pvy = 0; }
    if (g->py > g->map_h - PLAYER_R) {
        g->py = g->map_h - PLAYER_R;
        if (g->pvy > 0) g->pvy = 0;
    }
}

/* rect variant, for fragments */
static void move_rect_axis(const Game *g, Rect *r, float *coord, float *v,
                           float dt, float rest, int level)
{
    float delta = *v * dt;
    if (delta == 0)
        return;
    float old = *coord;
    bool was_stuck = rect_hits_wall(g, r, level);
    *coord = old + delta;
    if (was_stuck || !rect_hits_wall(g, r, level))
        return;
    float lo = 0, hi = 1;
    for (int i = 0; i < 8; i++) {
        float mid = (lo + hi) * 0.5f;
        *coord = old + delta * mid;
        if (rect_hits_wall(g, r, level)) hi = mid; else lo = mid;
    }
    *coord = old + delta * lo;
    *v *= -rest;
}

static void move_rect(const Game *g, Rect *r, float *vx, float *vy,
                      float dt, float rest, int level)
{
    move_rect_axis(g, r, &r->x, vx, dt, rest, level);
    if (r->x < 0) { r->x = 0; *vx *= -rest; }
    if (r->x + r->w > g->map_w) { r->x = g->map_w - r->w; *vx *= -rest; }

    move_rect_axis(g, r, &r->y, vy, dt, rest, level);
    if (r->y < 0) { r->y = 0; *vy *= -rest; }
    if (r->y + r->h > g->map_h) { r->y = g->map_h - r->h; *vy *= -rest; }
}

/* -------------------------------------------------------------- spawns */

static void spawn_fragment(Game *g, float x, float y, int level,
                           float speed_min, float speed_max, float size_min,
                           float size_max, float ttl, uint32_t color)
{
    for (int i = 0; i < MAX_FRAGMENTS; i++) {
        Fragment *fr = &g->frags[i];
        if (fr->alive) continue;
        float ang = frand(g) * 6.2831853f;
        float spd = speed_min + frand(g) * (speed_max - speed_min);
        float sz = size_min + frand(g) * (size_max - size_min);
        *fr = (Fragment){
            {x - sz / 2, y - sz / 2, sz, sz},
            cosf(ang) * spd, sinf(ang) * spd,
            ttl * (0.6f + 0.8f * frand(g)), level, color, true};
        return;
    }
}

static void explode_barrel(Game *g, const Barrel *b)
{
    static const uint32_t colors[3] = {0xFFB4712F, 0xFF8F5722, 0xFFD08A45};
    for (int i = 0; i < 10; i++)
        spawn_fragment(g, b->x, b->y, b->level, 120, 480, 6, 14, 2.0f,
                       colors[i % 3]);
}

static void fire_bullet(Game *g)
{
    for (int i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &g->bullets[i];
        if (b->alive) continue;
        /* small spread */
        float a = atan2f(g->aim_y, g->aim_x) + (frand(g) - 0.5f) * 0.05f;
        *b = (Bullet){
            g->px + g->aim_x * 26.0f, g->py + g->aim_y * 26.0f,
            cosf(a) * 1300.0f, sinf(a) * 1300.0f, 1.2f, g->plevel, true};
        return;
    }
}

/* -------------------------------------------------------------- update */

static void update_player(Game *g, float dt)
{
    TerrainType t = terrain_at(g, g->px, g->py);
    float ix = (g->key_d ? 1.0f : 0.0f) - (g->key_a ? 1.0f : 0.0f);
    float iy = (g->key_s ? 1.0f : 0.0f) - (g->key_w ? 1.0f : 0.0f);
    if (ix != 0 && iy != 0) { ix *= 0.70710678f; iy *= 0.70710678f; }

    g->pvx += ix * TPHYS[t].accel * dt;
    g->pvy += iy * TPHYS[t].accel * dt;

    float drag = expf(-TPHYS[t].drag * dt);
    g->pvx *= drag;
    g->pvy *= drag;

    float spd = sqrtf(g->pvx * g->pvx + g->pvy * g->pvy);
    if (spd > TPHYS[t].maxspd) {
        g->pvx *= TPHYS[t].maxspd / spd;
        g->pvy *= TPHYS[t].maxspd / spd;
    }

    /* jump: edge-triggered, only while standing */
    if (g->key_space && !g->space_latch && !g->airborne) {
        g->airborne = true;
        g->pvz = JUMP_V;
        g->fall_from = g->plevel;
        g->fall_jumped = true;
        g->ev_jump = true;
    }
    g->space_latch = g->key_space;

    move_player(g, dt);

    if (!g->airborne) {
        int lvl = level_at(g, g->px, g->py);
        if (lvl < g->plevel) { /* walked off an edge: start falling */
            g->airborne = true;
            g->pvz = 0;
            g->fall_from = g->plevel;
            g->fall_jumped = false;
        } else {
            g->plevel = lvl;
            g->pz = lvl * LEVEL_STEP;
        }
    }
    if (g->airborne) {
        g->pz += g->pvz * dt;
        g->pvz -= GRAVITY * dt;
        float floor = level_at(g, g->px, g->py) * LEVEL_STEP;
        if (g->pvz <= 0 && g->pz <= floor) {
            g->pz = floor;
            g->pvz = 0;
            g->airborne = false;
            g->plevel = level_at(g, g->px, g->py);
            /* jumping adds one level to the height fallen from */
            int fell = g->fall_from + (g->fall_jumped ? 1 : 0) - g->plevel;
            if (fell > 1)
                g->ev_fall = true;
        }
    }
    g->pfloor = level_at(g, g->px, g->py) * LEVEL_STEP;

    /* aim toward cursor (cursor is screen-local; camera converts to world) */
    float wx = g->cam_x + g->cursor_x, wy = g->cam_y + g->cursor_y;
    float dx = wx - g->px, dy = wy - g->py;
    float len = sqrtf(dx * dx + dy * dy);
    if (len > 1.0f) { g->aim_x = dx / len; g->aim_y = dy / len; }

    g->fire_cooldown -= dt;
    if (g->mouse_down && g->fire_cooldown <= 0) {
        fire_bullet(g);
        g->fire_cooldown = 0.14f;
    }
}

static void update_barrels(Game *g, float dt)
{
    for (int i = 0; i < g->nbarrels; i++) {
        Barrel *b = &g->barrels[i];
        if (!b->alive) continue;
        TerrainType t = terrain_at(g, b->x, b->y);
        float drag = expf(-TPHYS[t].obj_drag * dt);
        b->vx *= drag;
        b->vy *= drag;
        move_circle(g, &b->x, &b->y, &b->vx, &b->vy, dt, 0.55f,
                    BARREL_R, b->level);
        int lvl = level_at(g, b->x, b->y);
        if (lvl < b->level)
            b->level = lvl; /* rolled off an edge */
        float floorz = b->level * LEVEL_STEP;
        if (b->z > floorz) { /* visual drop after rolling off */
            b->vz -= GRAVITY * dt;
            b->z += b->vz * dt;
            if (b->z <= floorz) { b->z = floorz; b->vz = 0; }
        } else {
            b->z = floorz;
        }
    }

    /* barrel vs barrel (same floor only): push apart, trade velocity */
    for (int i = 0; i < g->nbarrels; i++) {
        Barrel *a = &g->barrels[i];
        if (!a->alive) continue;
        for (int j = i + 1; j < g->nbarrels; j++) {
            Barrel *b = &g->barrels[j];
            if (!b->alive || a->level != b->level) continue;
            float dx = b->x - a->x, dy = b->y - a->y;
            float d2 = dx * dx + dy * dy;
            float min = 2 * BARREL_R;
            if (d2 >= min * min || d2 < 0.0001f) continue;
            float d = sqrtf(d2);
            float nx = dx / d, ny = dy / d;
            float push = (min - d) / 2;
            a->x -= nx * push; a->y -= ny * push;
            b->x += nx * push; b->y += ny * push;
            float van = a->vx * nx + a->vy * ny;
            float vbn = b->vx * nx + b->vy * ny;
            if (van - vbn > 0) { /* approaching */
                float ea = vbn * 0.7f - van, eb = van * 0.7f - vbn;
                a->vx += nx * ea; a->vy += ny * ea;
                b->vx += nx * eb; b->vy += ny * eb;
            }
        }
    }

    /* player shoves barrels on its own floor (but not into a wall) */
    if (g->pz - g->pfloor > 40)
        return; /* jumping clears barrels */
    float pspd = sqrtf(g->pvx * g->pvx + g->pvy * g->pvy);
    for (int i = 0; i < g->nbarrels; i++) {
        Barrel *b = &g->barrels[i];
        if (!b->alive || b->level != g->plevel) continue;
        float dx = b->x - g->px, dy = b->y - g->py;
        float d2 = dx * dx + dy * dy;
        float min = PLAYER_R + BARREL_R;
        if (d2 >= min * min || d2 < 0.0001f) continue;
        float d = sqrtf(d2);
        float nx = dx / d, ny = dy / d;
        float sx = b->x, sy = b->y, svx = b->vx, svy = b->vy;
        b->x = g->px + nx * min;
        b->y = g->py + ny * min;
        b->vx = nx * (pspd * 0.8f + 60.0f);
        b->vy = ny * (pspd * 0.8f + 60.0f);
        if (circle_blocked(g, b->x, b->y, BARREL_R, b->level)) {
            b->x = sx; b->y = sy;   /* pinned between player and wall */
            b->vx = svx; b->vy = svy;
        }
    }
}

static void update_bullets(Game *g, float dt)
{
    for (int i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &g->bullets[i];
        if (!b->alive) continue;
        b->ttl -= dt;
        float mx = b->x + b->vx * dt * 0.5f, my = b->y + b->vy * dt * 0.5f;
        b->x += b->vx * dt;
        b->y += b->vy * dt;

        bool dead = b->ttl <= 0 ||
                    b->x < 0 || b->x > g->map_w || b->y < 0 || b->y > g->map_h;
        /* midpoint sample too, so fast bullets can't skip thin walls */
        if (!dead && (point_in_wall(g, b->x, b->y, b->level) ||
                      point_in_wall(g, mx, my, b->level))) {
            dead = true;
            for (int s = 0; s < 3; s++)
                spawn_fragment(g, b->x, b->y, b->level, 60, 260, 3, 6, 0.4f,
                               0xFFFFE066);
        }
        for (int c = 0; !dead && c < g->nbarrels; c++) {
            Barrel *br = &g->barrels[c];
            if (!br->alive) continue;
            float dx = b->x - br->x, dy = b->y - br->y;
            float hr = BARREL_R + 3;
            if (dx * dx + dy * dy > hr * hr) continue;
            dead = true;
            float spd = sqrtf(b->vx * b->vx + b->vy * b->vy);
            br->vx += b->vx / spd * 240.0f;
            br->vy += b->vy / spd * 240.0f;
            if (--br->hp <= 0) {
                br->alive = false;
                g->score++;
                explode_barrel(g, br);
            } else {
                spawn_fragment(g, b->x, b->y, br->level, 80, 300, 3, 7, 0.5f,
                               0xFFD08A45);
                spawn_fragment(g, b->x, b->y, br->level, 80, 300, 3, 7, 0.5f,
                               0xFF8F5722);
            }
        }
        b->alive = !dead;
    }
}

static void update_fragments(Game *g, float dt)
{
    for (int i = 0; i < MAX_FRAGMENTS; i++) {
        Fragment *fr = &g->frags[i];
        if (!fr->alive) continue;
        fr->ttl -= dt;
        if (fr->ttl <= 0) { fr->alive = false; continue; }
        float drag = expf(-3.0f * dt);
        fr->vx *= drag;
        fr->vy *= drag;
        move_rect(g, &fr->r, &fr->vx, &fr->vy, dt, 0.5f, fr->level);
    }
}

void game_update(Game *g, float dt)
{
    if (dt <= 0) return;
    update_player(g, dt);
    update_barrels(g, dt);
    update_bullets(g, dt);
    update_fragments(g, dt);

    /* camera eases toward the player, clamped to the map */
    float tx = g->px - SCREEN_W / 2.0f, ty = g->py - SCREEN_H / 2.0f;
    float k = 1.0f - expf(-8.0f * dt);
    g->cam_x += (tx - g->cam_x) * k;
    g->cam_y += (ty - g->cam_y) * k;
    if (g->cam_x < 0) g->cam_x = 0;
    if (g->cam_y < 0) g->cam_y = 0;
    if (g->cam_x > g->map_w - SCREEN_W) g->cam_x = g->map_w - SCREEN_W;
    if (g->cam_y > g->map_h - SCREEN_H) g->cam_y = g->map_h - SCREEN_H;
}
