#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SCREEN_W 1920
#define SCREEN_H 1080

#define MAX_WALLS 256
#define MAX_TERRAIN 128
#define MAX_CRATES 128
#define MAX_BULLETS 128
#define MAX_FRAGMENTS 512

#define PLAYER_SIZE 36.0f
#define CRATE_SIZE 44.0f

typedef struct { float x, y, w, h; } Rect;

#define MAX_POLY_PTS 32

typedef struct {
    int n;
    float x[MAX_POLY_PTS], y[MAX_POLY_PTS];
    float minx, miny, maxx, maxy; /* bbox, computed at load */
} Poly;

typedef enum {
    TERRAIN_GRASS,
    TERRAIN_SAND,
    TERRAIN_WATER,
    TERRAIN_ICE,
    TERRAIN_COUNT
} TerrainType;

typedef struct { TerrainType type; Poly p; } TerrainPatch;

typedef struct { Rect r; float vx, vy; int hp; bool alive; } Crate;

typedef struct { float x, y, vx, vy, ttl; bool alive; } Bullet;

typedef struct { Rect r; float vx, vy, ttl; uint32_t color; bool alive; } Fragment;

typedef struct {
    /* map */
    float map_w, map_h;
    Poly walls[MAX_WALLS];
    int nwalls;
    TerrainPatch terrain[MAX_TERRAIN];
    int nterrain;

    /* entities */
    Crate crates[MAX_CRATES];
    int ncrates;
    Bullet bullets[MAX_BULLETS];
    Fragment frags[MAX_FRAGMENTS];

    /* player (center position) */
    float px, py, pvx, pvy;
    float aim_x, aim_y;      /* unit vector toward cursor */
    float fire_cooldown;

    /* camera (top-left corner, world coords) */
    float cam_x, cam_y;

    /* input, written by the wayland layer */
    bool key_w, key_a, key_s, key_d;
    bool mouse_down;
    float cursor_x, cursor_y; /* surface-local pixels */

    uint32_t rng;
    int score;
} Game;

bool game_load_map(Game *g, const char *path);
void game_update(Game *g, float dt);
void game_render(const Game *g, uint32_t *pix);
