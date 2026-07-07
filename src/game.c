#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* per-terrain feel: player acceleration, player top speed, player drag,
 * and drag applied to loose objects (crates/fragments) resting on it */
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
    /* rect corner inside polygon */
    if (poly_contains(p, r->x, r->y) ||
        poly_contains(p, r->x + r->w, r->y) ||
        poly_contains(p, r->x, r->y + r->h) ||
        poly_contains(p, r->x + r->w, r->y + r->h))
        return true;
    /* polygon vertex inside rect (also covers polygon inside rect) */
    for (int i = 0; i < p->n; i++)
        if (p->x[i] >= r->x && p->x[i] <= r->x + r->w &&
            p->y[i] >= r->y && p->y[i] <= r->y + r->h)
            return true;
    /* edge crossings */
    float rx[4] = {r->x, r->x + r->w, r->x + r->w, r->x};
    float ry[4] = {r->y, r->y, r->y + r->h, r->y + r->h};
    for (int i = 0, j = p->n - 1; i < p->n; j = i++)
        for (int k = 0, l = 3; k < 4; l = k++)
            if (segs_cross(p->x[i], p->y[i], p->x[j], p->y[j],
                           rx[k], ry[k], rx[l], ry[l]))
                return true;
    return false;
}

static bool point_in_wall(const Game *g, float x, float y)
{
    for (int i = 0; i < g->nwalls; i++)
        if (poly_contains(&g->walls[i], x, y))
            return true;
    return false;
}

static bool hits_wall(const Game *g, const Rect *r)
{
    for (int i = 0; i < g->nwalls; i++)
        if (poly_hits_rect(&g->walls[i], r))
            return true;
    return false;
}

static bool overlap(const Rect *a, const Rect *b)
{
    return a->x < b->x + b->w && a->x + a->w > b->x &&
           a->y < b->y + b->h && a->y + a->h > b->y;
}

static bool point_in(float x, float y, const Rect *r, float pad)
{
    return x >= r->x - pad && x <= r->x + r->w + pad &&
           y >= r->y - pad && y <= r->y + r->h + pad;
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

static void rect_poly(Poly *p, float x, float y, float w, float h)
{
    p->n = 4;
    p->x[0] = x;     p->y[0] = y;
    p->x[1] = x + w; p->y[1] = y;
    p->x[2] = x + w; p->y[2] = y + h;
    p->x[3] = x;     p->y[3] = y + h;
    poly_finish(p);
}

static void add_shape(Game *g, const char *name, const Poly *p)
{
    if (!strcmp(name, "wall")) {
        if (g->nwalls < MAX_WALLS)
            g->walls[g->nwalls++] = *p;
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
        } else if (sscanf(line, "wall %f %f %f %f", &x, &y, &w, &h) == 4) {
            rect_poly(&p, x, y, w, h);
            add_shape(g, "wall", &p);
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
        } else if (sscanf(line, "crate %f %f", &x, &y) == 2) {
            if (g->ncrates < MAX_CRATES)
                g->crates[g->ncrates++] = (Crate){
                    {x - CRATE_SIZE / 2, y - CRATE_SIZE / 2, CRATE_SIZE, CRATE_SIZE},
                    0, 0, 3, true};
        } else {
            fprintf(stderr, "crateblast: %s:%d: skipping unrecognized line\n",
                    path, lineno);
        }
    }
    fclose(f);

    g->cam_x = g->px - SCREEN_W / 2.0f;
    g->cam_y = g->py - SCREEN_H / 2.0f;
    return true;
}

/* ---------------------------------------------------------- collision */

/* Advance one coordinate by v*dt. On wall contact, bisect to the contact
 * point and reflect the velocity scaled by -rest (rest=0 -> stop/slide).
 * A body that starts inside a wall moves freely so it can drift out. */
static void move_axis(const Game *g, Rect *r, float *coord, float *v,
                      float dt, float rest)
{
    float delta = *v * dt;
    if (delta == 0)
        return;
    float old = *coord;
    bool was_stuck = hits_wall(g, r);
    *coord = old + delta;
    if (was_stuck || !hits_wall(g, r))
        return;
    float lo = 0, hi = 1;
    for (int i = 0; i < 8; i++) {
        float mid = (lo + hi) * 0.5f;
        *coord = old + delta * mid;
        if (hits_wall(g, r)) hi = mid; else lo = mid;
    }
    *coord = old + delta * lo;
    *v *= -rest;
}

static void move_rect(const Game *g, Rect *r, float *vx, float *vy,
                      float dt, float rest)
{
    move_axis(g, r, &r->x, vx, dt, rest);
    if (r->x < 0) { r->x = 0; *vx *= -rest; }
    if (r->x + r->w > g->map_w) { r->x = g->map_w - r->w; *vx *= -rest; }

    move_axis(g, r, &r->y, vy, dt, rest);
    if (r->y < 0) { r->y = 0; *vy *= -rest; }
    if (r->y + r->h > g->map_h) { r->y = g->map_h - r->h; *vy *= -rest; }
}

/* -------------------------------------------------------------- spawns */

static void spawn_fragment(Game *g, float x, float y, float speed_min,
                           float speed_max, float size_min, float size_max,
                           float ttl, uint32_t color)
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
            ttl * (0.6f + 0.8f * frand(g)), color, true};
        return;
    }
}

static void explode_crate(Game *g, const Crate *c)
{
    static const uint32_t colors[3] = {0xFFB4712F, 0xFF8F5722, 0xFFD08A45};
    float cx = c->r.x + c->r.w / 2, cy = c->r.y + c->r.h / 2;
    for (int i = 0; i < 10; i++)
        spawn_fragment(g, cx, cy, 120, 480, 6, 14, 2.0f, colors[i % 3]);
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
            cosf(a) * 1300.0f, sinf(a) * 1300.0f, 1.2f, true};
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

    Rect pr = {g->px - PLAYER_SIZE / 2, g->py - PLAYER_SIZE / 2,
               PLAYER_SIZE, PLAYER_SIZE};
    move_rect(g, &pr, &g->pvx, &g->pvy, dt, 0.0f);
    g->px = pr.x + PLAYER_SIZE / 2;
    g->py = pr.y + PLAYER_SIZE / 2;

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

static void update_crates(Game *g, float dt)
{
    for (int i = 0; i < g->ncrates; i++) {
        Crate *c = &g->crates[i];
        if (!c->alive) continue;
        TerrainType t = terrain_at(g, c->r.x + c->r.w / 2, c->r.y + c->r.h / 2);
        float drag = expf(-TPHYS[t].obj_drag * dt);
        c->vx *= drag;
        c->vy *= drag;
        move_rect(g, &c->r, &c->vx, &c->vy, dt, 0.55f);
    }

    /* crate vs crate: separate along the shallow axis and trade velocity */
    for (int i = 0; i < g->ncrates; i++) {
        Crate *a = &g->crates[i];
        if (!a->alive) continue;
        for (int j = i + 1; j < g->ncrates; j++) {
            Crate *b = &g->crates[j];
            if (!b->alive || !overlap(&a->r, &b->r)) continue;
            float ox = (a->r.x < b->r.x) ? (a->r.x + a->r.w - b->r.x)
                                         : (b->r.x + b->r.w - a->r.x);
            float oy = (a->r.y < b->r.y) ? (a->r.y + a->r.h - b->r.y)
                                         : (b->r.y + b->r.h - a->r.y);
            if (ox < oy) {
                float s = (a->r.x < b->r.x) ? 1.0f : -1.0f;
                a->r.x -= s * ox / 2;
                b->r.x += s * ox / 2;
                float av = a->vx;
                a->vx = b->vx * 0.7f;
                b->vx = av * 0.7f;
            } else {
                float s = (a->r.y < b->r.y) ? 1.0f : -1.0f;
                a->r.y -= s * oy / 2;
                b->r.y += s * oy / 2;
                float av = a->vy;
                a->vy = b->vy * 0.7f;
                b->vy = av * 0.7f;
            }
        }
    }

    /* player shoves crates it walks into (but not into a wall) */
    Rect pr = {g->px - PLAYER_SIZE / 2, g->py - PLAYER_SIZE / 2,
               PLAYER_SIZE, PLAYER_SIZE};
    float pspd = sqrtf(g->pvx * g->pvx + g->pvy * g->pvy);
    for (int i = 0; i < g->ncrates; i++) {
        Crate *c = &g->crates[i];
        if (!c->alive || !overlap(&pr, &c->r)) continue;
        Rect saved = c->r;
        float svx = c->vx, svy = c->vy;
        float ox = (pr.x < c->r.x) ? (pr.x + pr.w - c->r.x)
                                   : (c->r.x + c->r.w - pr.x);
        float oy = (pr.y < c->r.y) ? (pr.y + pr.h - c->r.y)
                                   : (c->r.y + c->r.h - pr.y);
        if (ox < oy) {
            float s = (pr.x < c->r.x) ? 1.0f : -1.0f;
            c->r.x += s * ox;
            c->vx = s * (pspd * 0.8f + 60.0f);
        } else {
            float s = (pr.y < c->r.y) ? 1.0f : -1.0f;
            c->r.y += s * oy;
            c->vy = s * (pspd * 0.8f + 60.0f);
        }
        if (hits_wall(g, &c->r)) { /* pinned between player and wall */
            c->r = saved;
            c->vx = svx;
            c->vy = svy;
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
        if (!dead && (point_in_wall(g, b->x, b->y) || point_in_wall(g, mx, my))) {
            dead = true;
            for (int s = 0; s < 3; s++)
                spawn_fragment(g, b->x, b->y, 60, 260, 3, 6, 0.4f, 0xFFFFE066);
        }
        for (int c = 0; !dead && c < g->ncrates; c++) {
            Crate *cr = &g->crates[c];
            if (!cr->alive || !point_in(b->x, b->y, &cr->r, 3)) continue;
            dead = true;
            float spd = sqrtf(b->vx * b->vx + b->vy * b->vy);
            cr->vx += b->vx / spd * 240.0f;
            cr->vy += b->vy / spd * 240.0f;
            if (--cr->hp <= 0) {
                cr->alive = false;
                g->score++;
                explode_crate(g, cr);
            } else {
                spawn_fragment(g, b->x, b->y, 80, 300, 3, 7, 0.5f, 0xFFD08A45);
                spawn_fragment(g, b->x, b->y, 80, 300, 3, 7, 0.5f, 0xFF8F5722);
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
        move_rect(g, &fr->r, &fr->vx, &fr->vy, dt, 0.5f);
    }
}

void game_update(Game *g, float dt)
{
    if (dt <= 0) return;
    update_player(g, dt);
    update_crates(g, dt);
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
