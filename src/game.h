#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SCREEN_W 1920
#define SCREEN_H 1080

#define MAX_WALLS 256
#define MAX_TERRAIN 128
#define MAX_BARRELS 128
#define MAX_BULLETS 128
#define MAX_FRAGMENTS 512

#define PLAYER_R 18.0f
#define BARREL_R 21.0f

typedef struct { float x, y, w, h; } Rect;

#define MAX_POLY_PTS 32

typedef struct {
    int n;
    float x[MAX_POLY_PTS], y[MAX_POLY_PTS];
    float minx, miny, maxx, maxy; /* bbox, computed at load */
} Poly;

/* wall height levels; ground is level 0 */
enum { LEVEL_GROUND = 0, LEVEL_LOW = 1, LEVEL_MEDIUM = 2, LEVEL_HIGH = 3 };
#define LEVEL_STEP 70.0f /* world px of height per level */

typedef struct { int level; Poly p; } Wall;

typedef enum {
    TERRAIN_GRASS,
    TERRAIN_SAND,
    TERRAIN_WATER,
    TERRAIN_ICE,
    TERRAIN_COUNT
} TerrainType;

typedef struct { TerrainType type; Poly p; } TerrainPatch;

/* z/vz are visual-only: barrels animate falling when their level drops */
typedef struct {
    float x, y, vx, vy, z, vz;
    int hp, level;
    bool alive;
} Barrel;

typedef struct { float x, y, vx, vy, ttl; int level; bool alive; } Bullet;

typedef struct {
    Rect r; float vx, vy, ttl; int level; uint32_t color; bool alive;
} Fragment;

typedef struct {
    /* map */
    float map_w, map_h;
    Wall walls[MAX_WALLS];
    int nwalls;
    TerrainPatch terrain[MAX_TERRAIN];
    int nterrain;

    /* entities */
    Barrel barrels[MAX_BARRELS];
    int nbarrels;
    Bullet bullets[MAX_BULLETS];
    Fragment frags[MAX_FRAGMENTS];

    /* player (center position; pz is absolute height above ground plane) */
    float px, py, pvx, pvy;
    float pz, pvz;
    float pfloor;            /* floor height under the center (for render) */
    bool airborne;
    int plevel;              /* standing level; takeoff level while airborne */
    int fall_from;           /* level when the player left the ground */
    bool fall_jumped;        /* airborne via jump (adds one to fall height) */
    bool ev_jump, ev_fall;   /* one-shot events consumed by the audio layer */
    float aim_x, aim_y;      /* unit vector toward cursor */
    float fire_cooldown;

    /* camera (top-left corner, world coords) */
    float cam_x, cam_y;

    /* input, written by the wayland layer */
    bool key_w, key_a, key_s, key_d, key_space;
    bool space_latch;        /* jump is edge-triggered */
    float jump_buffer;       /* seconds left to honor a buffered press */
    bool mouse_down;
    float cursor_x, cursor_y; /* surface-local pixels */

    uint32_t rng;
    int score;
} Game;

bool game_load_map(Game *g, const char *path);
void game_update(Game *g, float dt);
void game_render(const Game *g, uint32_t *pix);
