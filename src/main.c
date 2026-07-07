#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "game.h"

struct buffer {
    struct wl_buffer *wl;
    uint32_t *pixels;
    bool busy;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static struct wl_keyboard *keyboard;
static struct wl_pointer *pointer;
static struct wl_surface *surface;
static struct xdg_surface *xsurface;
static struct xdg_toplevel *toplevel;
static struct buffer buffers[2];
static bool configured;
static bool running = true;
static uint32_t last_frame_time;
static Game game;

static void redraw(void);

/* ------------------------------------------------------------- buffers */

static void buffer_release(void *data, struct wl_buffer *wl)
{
    (void)wl;
    ((struct buffer *)data)->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {buffer_release};

static bool create_buffers(void)
{
    const int stride = SCREEN_W * 4;
    const int size = stride * SCREEN_H;

    int fd = memfd_create("crateblast-shm", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size * 2) < 0) {
        perror("crateblast: shm");
        return false;
    }
    void *mem = mmap(NULL, size * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        perror("crateblast: mmap");
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size * 2);
    for (int i = 0; i < 2; i++) {
        buffers[i].wl = wl_shm_pool_create_buffer(
            pool, i * size, SCREEN_W, SCREEN_H, stride, WL_SHM_FORMAT_XRGB8888);
        buffers[i].pixels = (uint32_t *)((char *)mem + i * size);
        wl_buffer_add_listener(buffers[i].wl, &buffer_listener, &buffers[i]);
    }
    wl_shm_pool_destroy(pool);
    close(fd);
    return true;
}

/* --------------------------------------------------------------- input */

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt,
                      int32_t fd, uint32_t sz)
{
    (void)d; (void)k; (void)fmt; (void)sz;
    close(fd); /* WASD works off raw evdev codes; no xkb needed */
}

static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial,
                     struct wl_surface *s, struct wl_array *keys)
{
    (void)d; (void)k; (void)serial; (void)s; (void)keys;
}

static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial,
                     struct wl_surface *s)
{
    (void)d; (void)k; (void)serial; (void)s;
    game.key_w = game.key_a = game.key_s = game.key_d = false;
}

static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial,
                   uint32_t time, uint32_t key, uint32_t state)
{
    (void)d; (void)k; (void)serial; (void)time;
    bool down = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    switch (key) {
    case KEY_W: game.key_w = down; break;
    case KEY_A: game.key_a = down; break;
    case KEY_S: game.key_s = down; break;
    case KEY_D: game.key_d = down; break;
    case KEY_ESC: if (down) running = false; break;
    }
}

static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t serial,
                         uint32_t dep, uint32_t lat, uint32_t lock,
                         uint32_t group)
{
    (void)d; (void)k; (void)serial; (void)dep; (void)lat; (void)lock;
    (void)group;
}

static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate,
                      int32_t delay)
{
    (void)d; (void)k; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat,
};

static void pt_enter(void *d, struct wl_pointer *p, uint32_t serial,
                     struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)s;
    wl_pointer_set_cursor(p, serial, NULL, 0, 0); /* we draw a crosshair */
    game.cursor_x = (float)wl_fixed_to_double(x);
    game.cursor_y = (float)wl_fixed_to_double(y);
}

static void pt_leave(void *d, struct wl_pointer *p, uint32_t serial,
                     struct wl_surface *s)
{
    (void)d; (void)p; (void)serial; (void)s;
    game.mouse_down = false;
}

static void pt_motion(void *d, struct wl_pointer *p, uint32_t time,
                      wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)time;
    game.cursor_x = (float)wl_fixed_to_double(x);
    game.cursor_y = (float)wl_fixed_to_double(y);
}

static void pt_button(void *d, struct wl_pointer *p, uint32_t serial,
                      uint32_t time, uint32_t button, uint32_t state)
{
    (void)d; (void)p; (void)serial; (void)time;
    if (button == BTN_LEFT)
        game.mouse_down = state == WL_POINTER_BUTTON_STATE_PRESSED;
}

static void pt_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a,
                    wl_fixed_t v)
{ (void)d; (void)p; (void)t; (void)a; (void)v; }
static void pt_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void pt_axis_source(void *d, struct wl_pointer *p, uint32_t s)
{ (void)d; (void)p; (void)s; }
static void pt_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{ (void)d; (void)p; (void)t; (void)a; }
static void pt_axis_discrete(void *d, struct wl_pointer *p, uint32_t a,
                             int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }
static void pt_axis_value120(void *d, struct wl_pointer *p, uint32_t a,
                             int32_t v)
{ (void)d; (void)p; (void)a; (void)v; }
static void pt_axis_rel_dir(void *d, struct wl_pointer *p, uint32_t a,
                            uint32_t dir)
{ (void)d; (void)p; (void)a; (void)dir; }

static const struct wl_pointer_listener pointer_listener = {
    pt_enter, pt_leave, pt_motion, pt_button, pt_axis, pt_frame,
    pt_axis_source, pt_axis_stop, pt_axis_discrete, pt_axis_value120,
    pt_axis_rel_dir,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
}

static void seat_name(void *d, struct wl_seat *s, const char *n)
{ (void)d; (void)s; (void)n; }

static const struct wl_seat_listener seat_listener = {seat_caps, seat_name};

/* ----------------------------------------------------------- xdg-shell */

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)d;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {wm_ping};

static void xsurf_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
    (void)d;
    xdg_surface_ack_configure(xs, serial);
    configured = true;
}

static const struct xdg_surface_listener xsurface_listener = {xsurf_configure};

static void top_configure(void *d, struct xdg_toplevel *t, int32_t w,
                          int32_t h, struct wl_array *states)
{
    /* fixed 1920x1080 buffer for v1; the suggested size is ignored */
    (void)d; (void)t; (void)w; (void)h; (void)states;
}

static void top_close(void *d, struct xdg_toplevel *t)
{
    (void)d; (void)t;
    running = false;
}

static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h)
{ (void)d; (void)t; (void)w; (void)h; }

static void top_wm_caps(void *d, struct xdg_toplevel *t, struct wl_array *c)
{ (void)d; (void)t; (void)c; }

static const struct xdg_toplevel_listener toplevel_listener = {
    top_configure, top_close, top_bounds, top_wm_caps,
};

/* ------------------------------------------------------------ registry */

static void reg_global(void *d, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version)
{
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
                                      version < 4 ? version : 4);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        seat = wl_registry_bind(reg, name, &wl_seat_interface,
                                version < 5 ? version : 5);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}

static void reg_remove(void *d, struct wl_registry *reg, uint32_t name)
{ (void)d; (void)reg; (void)name; }

static const struct wl_registry_listener registry_listener = {
    reg_global, reg_remove,
};

/* ------------------------------------------------------------ rendering */

static void frame_done(void *d, struct wl_callback *cb, uint32_t time)
{
    (void)d;
    wl_callback_destroy(cb);
    float dt = last_frame_time ? (time - last_frame_time) / 1000.0f : 0.0f;
    last_frame_time = time;
    if (dt > 0.05f) dt = 0.05f;
    game_update(&game, dt);
    redraw();
}

static const struct wl_callback_listener frame_listener = {frame_done};

static void redraw(void)
{
    struct buffer *buf = NULL;
    for (int i = 0; i < 2; i++)
        if (!buffers[i].busy)
            buf = &buffers[i];

    struct wl_callback *cb = wl_surface_frame(surface);
    wl_callback_add_listener(cb, &frame_listener, NULL);

    if (buf) { /* both busy is rare; skip the frame, callback still fires */
        game_render(&game, buf->pixels);
        wl_surface_attach(surface, buf->wl, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, SCREEN_W, SCREEN_H);
        buf->busy = true;
    }
    wl_surface_commit(surface);
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    const char *map_path = argc > 1 ? argv[1] : "map.txt";
    if (!game_load_map(&game, map_path))
        return 1;

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "crateblast: cannot connect to Wayland display\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);

    if (!compositor || !shm || !wm_base || !seat) {
        fprintf(stderr, "crateblast: missing globals (compositor %d shm %d "
                        "xdg_wm_base %d seat %d)\n",
                !!compositor, !!shm, !!wm_base, !!seat);
        return 1;
    }
    if (!create_buffers())
        return 1;

    surface = wl_compositor_create_surface(compositor);
    xsurface = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xsurface, &xsurface_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xsurface);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
    xdg_toplevel_set_title(toplevel, "crateblast");
    xdg_toplevel_set_app_id(toplevel, "crateblast");
    wl_surface_commit(surface);

    while (!configured && wl_display_dispatch(display) != -1)
        ;
    redraw(); /* first frame; frame callbacks take over from here */

    while (running && wl_display_dispatch(display) != -1)
        ;

    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xsurface);
    wl_surface_destroy(surface);
    wl_display_disconnect(display);
    return 0;
}
