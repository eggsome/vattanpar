/* headless check of the height/jump/fall/slide rules against the frozen
 * geometry in tests/testmap.txt (run via `make test`)
 * staircase blocks: low 2700,1850 300x300  medium 3000,1850  high 3300,1850
 * diagonal medium wall: 3500,1450 -> 3800,1750 band */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "game.h"

static Game g;

static void steps(int n)
{
    for (int i = 0; i < n; i++)
        game_update(&g, 1.0f / 60.0f);
}

static void place(float x, float y, int level)
{
    g.px = x; g.py = y; g.pvx = g.pvy = 0;
    g.pz = level * LEVEL_STEP; g.pvz = 0;
    g.airborne = false; g.plevel = level;
    g.ev_jump = g.ev_fall = false;
    g.jump_buffer = 0;
    g.key_w = g.key_a = g.key_s = g.key_d = g.key_space = false;
    g.space_latch = false;
}

static void jump_press(void)
{
    g.key_space = true;
    game_update(&g, 1.0f / 60.0f);
    g.key_space = false;
}

static int fails;

static void expect(const char *what, int got, int want)
{
    printf("%-46s %d (want %d) pos=%.0f,%.0f %s\n",
           what, got, want, g.px, g.py, got == want ? "ok" : "FAIL");
    if (got != want) fails++;
}

int main(int argc, char **argv)
{
    if (!game_load_map(&g, argc > 1 ? argv[1] : "tests/testmap.txt"))
        return 1;
    expect("spawn level", g.plevel, 0);

    /* 1. walking into the medium block face still blocks */
    place(3150, 1700, 0);
    g.key_s = true;
    steps(60);
    printf("%-46s y=%.0f (want < 1835) %s\n", "walk south into medium block",
           g.py, g.py < 1835.0f ? "ok" : "FAIL");
    if (g.py >= 1835.0f) fails++;

    /* 2. climbing one step at a time still works */
    place(2650, 2000, 0);
    g.key_d = true;
    jump_press();
    steps(60);
    expect("jump ground -> low", g.plevel, 1);

    place(3150, 1700, 0);
    g.key_s = true;
    jump_press();
    steps(60);
    expect("jump at medium from ground (illegal)", g.plevel, 0);

    /* climbing works when jumping near the wall (jumping from far away
     * hits the face mid-arc and you slide back down -- intended) */
    place(2900, 2000, 1);
    g.key_d = true;
    steps(30); /* run up against the medium face first */
    jump_press();
    steps(60);
    expect("jump low -> medium (at the face)", g.plevel, 2);

    place(3200, 2000, 2);
    g.key_d = true;
    steps(30);
    jump_press();
    steps(60);
    expect("jump medium -> high (at the face)", g.plevel, 3);

    /* 3. no fences: walking off the low block drops you off, silently */
    place(2850, 2000, 1);
    g.key_a = true;
    steps(90);
    expect("walk west off low block: lands on ground", g.plevel, 0);
    printf("%-46s x=%.0f (want < 2690) %s\n", "  and kept moving",
           g.px, g.px < 2690.0f ? "ok" : "FAIL");
    if (g.px >= 2690.0f) fails++;
    expect("  fall sound after 1-level walk-off", g.ev_fall, 0);

    /* 4. walking off the HIGH block is a 3-level fall -> sound
     * (x=3350 avoids stepping onto the adjoining high wall at x=3400) */
    place(3350, 2000, 3);
    g.key_s = true;                 /* south edge y=2150 is open ground */
    steps(120);
    expect("walk south off high block: on ground", g.plevel, 0);
    expect("  fall sound after 3-level walk-off", g.ev_fall, 1);

    /* 4b. walking onto the adjoining high wall is NOT a fall */
    place(3450, 2000, 3);
    g.key_s = true;
    steps(120);
    expect("walk south onto high rampart: stays high", g.plevel, 3);

    /* 5. jumping off the low block = differential 2 -> sound */
    place(2850, 2000, 1);
    g.key_a = true;
    jump_press();
    expect("  jump sound fired", g.ev_jump, 1);
    steps(90);
    expect("jump west off low block: on ground", g.plevel, 0);
    expect("  fall sound after jump off low", g.ev_fall, 1);

    /* 6. flat jump on open ground: no fall sound */
    place(1400, 2000, 0);
    jump_press();
    steps(90);
    expect("flat jump: no fall sound", g.ev_fall, 0);

    /* 7. overhang: standing just past the edge (support disc still on
     * the wall) must NOT drop you */
    place(2706, 2000, 1); /* low block west edge is x=2700 */
    steps(30);
    expect("overhang near edge: still on low block", g.plevel, 1);
    expect("  no fall sound", g.ev_fall, 0);

    /* 8. past the support margin you really fall, and get pushed clear
     * of the wall instead of landing embedded in it */
    place(2688, 2000, 1);
    steps(45);
    expect("past margin: dropped to ground", g.plevel, 0);
    printf("%-46s x=%.0f (want <= 2683) %s\n", "  pushed clear of the face",
           g.px, g.px <= 2683.0f ? "ok" : "FAIL");
    if (g.px > 2683.0f) fails++;

    /* 9. regression: embedded beside the wall at ground level must not
     * silently pop back up on top when moving toward the wall */
    place(2695, 2000, 0); /* circle overlaps the low block footprint */
    g.key_d = true;
    steps(60);
    expect("walk into wall after fall: stays on ground", g.plevel, 0);
    printf("%-46s x=%.0f (want < 2690) %s\n", "  held at the wall face",
           g.px, g.px < 2690.0f ? "ok" : "FAIL");
    if (g.px >= 2690.0f) fails++;

    /* 10. sliding: push straight south against the 45-degree diagonal
     * wall; the old code got stuck, now we should skate south-east */
    place(3650, 1500, 0);
    g.key_s = true;
    steps(90);
    printf("%-46s x=%.0f (want > 3700) %s\n", "slide along diagonal wall",
           g.px, g.px > 3700.0f ? "ok" : "FAIL");
    if (g.px <= 3700.0f) fails++;

    /* 11. jump buffering: a press just before touchdown (~0.08 s early)
     * jumps again on landing; a much earlier press expires */
    place(1400, 2000, 0);
    jump_press();          /* airborne until ~frame 39 */
    steps(33);
    jump_press();          /* buffered press shortly before landing */
    steps(15);
    expect("buffered press jumps on landing", g.airborne, 1);

    place(1400, 2000, 0);
    jump_press();
    steps(15);
    jump_press();          /* far too early: buffer expires mid-air */
    steps(40);
    expect("too-early press does not jump", g.airborne, 0);

    /* 12. terrain only grips you on the ground: airborne over sand you
     * keep grass-level speed; standing in it you're slowed and capped */
    place(1000, 1200, 0); /* over the sand patch */
    g.airborne = true;
    g.pz = 30;
    g.pvz = 400;
    g.pvx = 400;
    g.key_d = true;
    steps(10);
    printf("%-46s vx=%.0f (want > 300) %s\n", "airborne over sand: full speed",
           g.pvx, g.pvx > 300.0f ? "ok" : "FAIL");
    if (g.pvx <= 300.0f) fails++;

    place(1000, 1200, 0);
    g.pvx = 400;
    g.key_d = true;
    steps(10);
    printf("%-46s vx=%.0f (want < 300) %s\n", "grounded in sand: slowed",
           g.pvx, g.pvx < 300.0f ? "ok" : "FAIL");
    if (g.pvx >= 300.0f) fails++;

    /* 13. wall tops behave like grass even above special terrain */
    place(780, 1100, 1); /* on the low wall inside the sand patch */
    g.pvx = 400;
    g.key_d = true;
    steps(10);
    printf("%-46s vx=%.0f (want > 300) %s\n", "on wall over sand: full speed",
           g.pvx, g.pvx > 300.0f ? "ok" : "FAIL");
    if (g.pvx <= 300.0f) fails++;
    expect("  still on the wall", g.plevel, 1);

    printf(fails ? ">>> %d FAILURES\n" : "all passed\n", fails);
    return fails != 0;
}
