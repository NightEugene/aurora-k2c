// Wayland-клиент: fullscreen + RGBA кадры через wl_shm (XRGB8888).
#include "k2c_display.h"
#include "xdg_shell_min.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <poll.h>
#include <pthread.h>

static K2CFrameSink g_host_sink;
static void *g_host_context;
static pthread_mutex_t g_host_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct wl_display *g_dpy;
static struct wl_compositor *g_comp;
static struct wl_shm *g_shm;
static struct wl_proxy *g_wm_base, *g_xdg_surface, *g_toplevel;
static struct wl_shell *g_shell;
static struct wl_shell_surface *g_shell_surface;
static struct wl_surface *g_surface;
static struct wl_output *g_output;
static int g_output_portrait = 1;
static struct wl_proxy *g_qt_surface_ext, *g_qt_extended_surface;
static const struct wl_interface g_qt_extended_iface;
static const struct wl_interface *g_qt_get_types[] = { &g_qt_extended_iface, &wl_surface_interface };
static const struct wl_message g_qt_ext_requests[] = { { "get_extended_surface", "no", g_qt_get_types } };
static const struct wl_interface g_qt_ext_iface = { "qt_surface_extension", 1, 1, g_qt_ext_requests, 0, NULL };
static const struct wl_message g_qt_surface_requests[] = {
    { "update_generic_property", "sa", NULL }, { "set_content_orientation_mask", "i", NULL },
    { "set_window_flags", "i", NULL }, { "raise", "", NULL }, { "lower", "", NULL }
};
static const struct wl_message g_qt_surface_events[] = {
    { "onscreen_visibility", "i", NULL }, { "set_generic_property", "sa", NULL }, { "close", "", NULL }
};
static const struct wl_interface g_qt_extended_iface = {
    "qt_extended_surface", 1, 5, g_qt_surface_requests, 3, g_qt_surface_events
};
static void qt_visible(void *data, struct wl_proxy *surface, int32_t visible)
{ (void)data; (void)surface; (void)visible; }
static void qt_property(void *data, struct wl_proxy *surface, const char *name, struct wl_array *value)
{ (void)data; (void)surface; (void)name; (void)value; }
static void qt_close(void *data, struct wl_proxy *surface)
{ (void)data; (void)surface; fprintf(stderr, "[wl] extended surface close\n"); }
static void (*g_qt_surface_listener[])(void) = {
    (void (*)(void))qt_visible, (void (*)(void))qt_property, (void (*)(void))qt_close
};
static struct wl_seat *g_seat;
static struct wl_touch *g_touch;
static struct wl_pointer *g_pointer;
static int32_t g_touch_id = -1;
static float g_touch_x, g_touch_y, g_pointer_x, g_pointer_y;
static int g_pointer_pressed;

#define INPUT_CAPACITY 128
static K2CInput g_input[INPUT_CAPACITY];
static unsigned g_input_head, g_input_tail;
static pthread_mutex_t g_input_mutex = PTHREAD_MUTEX_INITIALIZER;

static int g_w = 2000, g_h = 1200;
static int g_configured;

void k2c_display_set_host(int width, int height, K2CFrameSink sink, void *context)
{
    pthread_mutex_lock(&g_host_mutex);
    if (width > 0 && height > 0) { g_w = width; g_h = height; }
    g_host_sink = sink;
    g_host_context = context;
    if (sink) g_output_portrait = 0;
    pthread_mutex_unlock(&g_host_mutex);
}

static void queue_touch(int action, float x, float y)
{
    pthread_mutex_lock(&g_input_mutex);
    unsigned next = (g_input_head + 1) % INPUT_CAPACITY;
    if (next == g_input_tail) {
        if (action == 2) { pthread_mutex_unlock(&g_input_mutex); return; }
        // Never leave Unity with a stuck contact after queue overflow.
        g_input_head = g_input_tail = 0;
        action = 3;
        next = 1;
    }
    // Aurora's shm input follows the portrait buffer coordinates. Undo the
    // clockwise pixel rotation used when presenting our landscape image.
    g_input[g_input_head] = g_output_portrait ?
        (K2CInput){ action, y / g_w, 1.f - x / g_h } :
        (K2CInput){ action, x / g_w, y / g_h };
    g_input_head = next;
    pthread_mutex_unlock(&g_input_mutex);
}

void k2c_display_host_input(int action, float x, float y)
{
    queue_touch(action, x * g_w, y * g_h);
}

static void touch_down(void *data, struct wl_touch *touch, uint32_t serial,
                       uint32_t time, struct wl_surface *surface, int32_t id,
                       wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)touch; (void)serial; (void)time;
    if (surface != g_surface || g_touch_id != -1) return;
    g_touch_id = id;
    g_touch_x = wl_fixed_to_double(x); g_touch_y = wl_fixed_to_double(y);
    queue_touch(0, g_touch_x, g_touch_y);
}
static void touch_up(void *data, struct wl_touch *touch, uint32_t serial,
                     uint32_t time, int32_t id)
{
    (void)data; (void)touch; (void)serial; (void)time;
    if (id != g_touch_id) return;
    queue_touch(1, g_touch_x, g_touch_y);
    g_touch_id = -1;
}
static void touch_motion(void *data, struct wl_touch *touch, uint32_t time,
                         int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)touch; (void)time;
    if (id != g_touch_id) return;
    g_touch_x = wl_fixed_to_double(x); g_touch_y = wl_fixed_to_double(y);
    queue_touch(2, g_touch_x, g_touch_y);
}
static void touch_frame(void *data, struct wl_touch *touch) { (void)data; (void)touch; }
static void touch_cancel(void *data, struct wl_touch *touch)
{
    (void)data; (void)touch;
    if (g_touch_id != -1) queue_touch(3, g_touch_x, g_touch_y);
    g_touch_id = -1;
}
static const struct wl_touch_listener g_touch_listener = {
    .down = touch_down, .up = touch_up, .motion = touch_motion,
    .frame = touch_frame, .cancel = touch_cancel
};

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)pointer; (void)serial; (void)surface;
    g_pointer_x = wl_fixed_to_double(x); g_pointer_y = wl_fixed_to_double(y);
}
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface)
{
    (void)data; (void)pointer; (void)serial; (void)surface;
    if (g_pointer_pressed) queue_touch(3, g_pointer_x, g_pointer_y);
    g_pointer_pressed = 0;
}
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                           wl_fixed_t x, wl_fixed_t y)
{
    (void)data; (void)pointer; (void)time;
    g_pointer_x = wl_fixed_to_double(x); g_pointer_y = wl_fixed_to_double(y);
    if (g_pointer_pressed) queue_touch(2, g_pointer_x, g_pointer_y);
}
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state)
{
    (void)data; (void)pointer; (void)serial; (void)time;
    if (button != 0x110) return; // BTN_LEFT
    g_pointer_pressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
    queue_touch(g_pointer_pressed ? 0 : 1, g_pointer_x, g_pointer_y);
}
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                         uint32_t axis, wl_fixed_t value)
{ (void)data; (void)pointer; (void)time; (void)axis; (void)value; }
static const struct wl_pointer_listener g_pointer_listener = {
    .enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
    .button = pointer_button, .axis = pointer_axis
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !g_touch) {
        g_touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(g_touch, &g_touch_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && g_touch) {
        touch_cancel(NULL, g_touch);
        wl_touch_release(g_touch); g_touch = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g_pointer) {
        g_pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g_pointer, &g_pointer_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && g_pointer) {
        pointer_leave(NULL, g_pointer, 0, NULL);
        wl_pointer_release(g_pointer); g_pointer = NULL;
    }
    fprintf(stderr, "[wl] input capabilities: 0x%x\n", caps);
}
static void seat_name(void *data, struct wl_seat *seat, const char *name)
{ (void)data; (void)seat; (void)name; }
static const struct wl_seat_listener g_seat_listener = { seat_capabilities, seat_name };

static void output_geometry(void *data, struct wl_output *output, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t subpixel,
                            const char *make, const char *model, int32_t transform)
{ (void)data; (void)output; (void)x; (void)y; (void)pw; (void)ph; (void)subpixel;
  (void)make; (void)model; (void)transform; }
static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    (void)data; (void)output; (void)refresh;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        if (g_surface) return;
        g_output_portrait = width < height;
        if (!getenv("K2C_WIDTH")) g_w = width > height ? width : height;
        if (!getenv("K2C_HEIGHT")) g_h = width > height ? height : width;
        fprintf(stderr, "[wl] output mode %dx%d, portrait=%d\n", width, height, g_output_portrait);
    }
}
static void output_done(void *data, struct wl_output *output) { (void)data; (void)output; }
static void output_scale(void *data, struct wl_output *output, int32_t scale)
{ (void)data; (void)output; (void)scale; }
static const struct wl_output_listener g_output_listener = {
    .geometry = output_geometry, .mode = output_mode, .done = output_done, .scale = output_scale
};

#define NBUF 2
static struct {
    struct wl_buffer *buf;
    void *data;
    size_t size;
    int busy;
} g_bufs[NBUF];
static int g_cur;

static void buffer_release_cb(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    *(int *)data = 0;
}
static const struct wl_buffer_listener g_buf_listener = { buffer_release_cb };

static int memfd(size_t size)
{
    int fd = (int)syscall(SYS_memfd_create, "k2c", 0);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return -1; }
    return fd;
}

static int alloc_shm_buffers(void)
{
    int buffer_w = g_output_portrait ? g_h : g_w;
    int buffer_h = g_output_portrait ? g_w : g_h;
    size_t stride = (size_t)buffer_w * 4;
    size_t size = stride * (size_t)buffer_h;
    int fd = memfd(size * NBUF);
    if (fd < 0) { fprintf(stderr, "[wl] memfd failed\n"); return -1; }
    void *map = mmap(NULL, size * NBUF, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return -1; }
    struct wl_shm_pool *pool = wl_shm_create_pool(g_shm, fd, (int)(size * NBUF));
    for (int i = 0; i < NBUF; i++) {
        g_bufs[i].data = (char *)map + size * (size_t)i;
        g_bufs[i].size = size;
        g_bufs[i].buf = wl_shm_pool_create_buffer(pool, (int)(size * (size_t)i),
                                                buffer_w, buffer_h, (int)stride, WL_SHM_FORMAT_XRGB8888);
        wl_buffer_add_listener(g_bufs[i].buf, &g_buf_listener, &g_bufs[i].busy);
    }
    wl_shm_pool_destroy(pool);
    close(fd);
    return 0;
}

static void registry_cb(void *data, struct wl_registry *reg, uint32_t id,
                        const char *iface, uint32_t version)
{
    (void)data; (void)version;
    fprintf(stderr, "[wl] global: %s v%u\n", iface, version);
    if (strcmp(iface, "wl_compositor") == 0)
        g_comp = wl_registry_bind(reg, id, &wl_compositor_interface, 4);
    else if (strcmp(iface, "wl_shm") == 0)
        g_shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
    else if (strcmp(iface, "xdg_wm_base") == 0)
        g_wm_base = wl_registry_bind(reg, id, &xdg_wm_base_interface, 1);
    else if (strcmp(iface, "wl_shell") == 0)
        g_shell = wl_registry_bind(reg, id, &wl_shell_interface, 1);
    else if (strcmp(iface, "qt_surface_extension") == 0)
        g_qt_surface_ext = wl_registry_bind(reg, id, &g_qt_ext_iface, 1);
    else if (strcmp(iface, "wl_output") == 0 && !g_output) {
        g_output = wl_registry_bind(reg, id, &wl_output_interface, version < 2 ? version : 2);
        wl_output_add_listener(g_output, &g_output_listener, NULL);
    }
    else if (strcmp(iface, "wl_seat") == 0) {
        g_seat = wl_registry_bind(reg, id, &wl_seat_interface, version < 4 ? version : 4);
        wl_seat_add_listener(g_seat, &g_seat_listener, NULL);
    }
}

static void registry_rm_cb(void *data, struct wl_registry *reg, uint32_t id)
{
    (void)data; (void)reg; (void)id;
}

static const struct wl_registry_listener g_reg_listener = { registry_cb, registry_rm_cb };

static void wm_ping_cb(void *data, struct wl_proxy *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener g_wm_listener = { wm_ping_cb };

static void xsurface_configure_cb(void *data, struct wl_proxy *xs, uint32_t serial)
{
    (void)data;
    xdg_surface_ack_configure(xs, serial);
    g_configured = 1;
}
static const struct xdg_surface_listener g_xs_listener = { xsurface_configure_cb };

static void toplevel_configure_cb(void *data, struct wl_proxy *tl,
                                  int32_t w, int32_t h, struct wl_array *states)
{
    (void)data; (void)tl; (void)states;
    if (w > 0 && h > 0 && !g_bufs[0].buf) {
        g_w = g_output_portrait ? h : w; g_h = g_output_portrait ? w : h;
    }
}
static void toplevel_close_cb(void *data, struct wl_proxy *tl)
{
    (void)data; (void)tl;
    fprintf(stderr, "[wl] close\n");
}
static const struct xdg_toplevel_listener g_tl_listener = { toplevel_configure_cb, toplevel_close_cb };

static void ss_ping_cb(void *data, struct wl_shell_surface *ss, uint32_t serial)
{
    (void)data;
    wl_shell_surface_pong(ss, serial);
}
static void ss_configure_cb(void *data, struct wl_shell_surface *ss, uint32_t edges,
                            int32_t w, int32_t h)
{
    (void)data; (void)ss; (void)edges;
    if (w > 0 && h > 0 && !g_bufs[0].buf) {
        g_w = g_output_portrait ? h : w; g_h = g_output_portrait ? w : h;
    }
}
static void ss_popup_done_cb(void *data, struct wl_shell_surface *ss)
{
    (void)data; (void)ss;
}
static const struct wl_shell_surface_listener g_ss_listener = {
    ss_ping_cb, ss_configure_cb, ss_popup_done_cb
};

int k2c_display_init(void)
{
    if (g_host_sink) {
        fprintf(stderr, "[display] embedded Qt view %dx%d; no additional Wayland surface\n", g_w, g_h);
        return 0;
    }
    const char *env = getenv("K2C_WIDTH");
    if (env) g_w = atoi(env);
    env = getenv("K2C_HEIGHT");
    if (env) g_h = atoi(env);

    g_dpy = wl_display_connect(NULL);
    if (!g_dpy) { fprintf(stderr, "[wl] connect failed (WAYLAND_DISPLAY=%s)\n",
                          getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)"); return -1; }
    struct wl_registry *reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(reg, &g_reg_listener, NULL);
    wl_display_roundtrip(g_dpy);
    wl_display_roundtrip(g_dpy); // events from the seat/output bound by registry callbacks
    if (!g_comp || !g_shm || (!g_wm_base && !g_shell)) {
        fprintf(stderr, "[wl] missing globals: comp=%p shm=%p xdg=%p shell=%p\n",
                (void *)g_comp, (void *)g_shm, g_wm_base, (void *)g_shell);
        return -1;
    }

    g_surface = wl_compositor_create_surface(g_comp);
    // A transformed buffer must contain pixels in the inverse orientation.
    // Keep the surface/input landscape and store portrait shm pixels below.
    if (g_output_portrait) wl_surface_set_buffer_transform(g_surface, WL_OUTPUT_TRANSFORM_270);
    if (g_qt_surface_ext) {
        g_qt_extended_surface = wl_proxy_marshal_constructor(g_qt_surface_ext, 0,
                                                            &g_qt_extended_iface, NULL, g_surface);
        wl_proxy_add_listener(g_qt_extended_surface, g_qt_surface_listener, NULL);
        wl_proxy_marshal(g_qt_extended_surface, 1, 2); // Qt::LandscapeOrientation
    }
    if (g_wm_base) {
        wl_proxy_add_listener(g_wm_base, (void (**)(void))&g_wm_listener, NULL);
        g_xdg_surface = xdg_wm_base_get_xdg_surface(g_wm_base, g_surface);
        wl_proxy_add_listener(g_xdg_surface, (void (**)(void))&g_xs_listener, NULL);
        g_toplevel = xdg_surface_get_toplevel(g_xdg_surface);
        wl_proxy_add_listener(g_toplevel, (void (**)(void))&g_tl_listener, NULL);
        xdg_toplevel_set_title(g_toplevel, "Kingdom Two Crowns");
        xdg_toplevel_set_app_id(g_toplevel, "ru.nighteugene.k2clauncher");
        xdg_toplevel_set_fullscreen(g_toplevel, NULL);
    } else {
        g_shell_surface = wl_shell_get_shell_surface(g_shell, g_surface);
        wl_shell_surface_add_listener(g_shell_surface, &g_ss_listener, NULL);
        wl_shell_surface_set_class(g_shell_surface, "ru.nighteugene.k2clauncher");
        wl_shell_surface_set_title(g_shell_surface, "Kingdom: Two Crowns");
        wl_shell_surface_set_toplevel(g_shell_surface);
        wl_shell_surface_set_fullscreen(g_shell_surface, WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT, 0, NULL);
        fprintf(stderr, "[wl] используется wl_shell\n");
    }
    wl_surface_commit(g_surface);
    wl_display_roundtrip(g_dpy);

    if (alloc_shm_buffers() != 0) return -1;
    fprintf(stderr, "[wl] display ready %dx%d configured=%d\n", g_w, g_h, g_configured);
    return 0;
}

void k2c_display_dispatch(void)
{
    if (!g_dpy) return;
    while (wl_display_prepare_read(g_dpy) != 0)
        if (wl_display_dispatch_pending(g_dpy) < 0) return;
    wl_display_flush(g_dpy);
    struct pollfd pfd = { wl_display_get_fd(g_dpy), POLLIN, 0 };
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        if (wl_display_read_events(g_dpy) < 0) return;
        wl_display_dispatch_pending(g_dpy);
    } else {
        wl_display_cancel_read(g_dpy);
    }
}

void k2c_display_present(const void *pixels, int width, int height, int stride_bytes)
{
    if (!pixels || width <= 0 || height <= 0) return;
    pthread_mutex_lock(&g_host_mutex);
    if (g_host_sink) {
        g_host_sink(g_host_context, pixels, width, height, stride_bytes);
        pthread_mutex_unlock(&g_host_mutex);
        return;
    }
    pthread_mutex_unlock(&g_host_mutex);
    if (!g_dpy || wl_display_get_error(g_dpy)) return;
    int slot = -1;
    for (int i = 0; i < NBUF; i++) {
        int candidate = (g_cur + i) % NBUF;
        if (!g_bufs[candidate].busy) { slot = candidate; break; }
    }
    if (slot < 0) return;
    struct wl_buffer *b = g_bufs[slot].buf;
    char *dst = g_bufs[slot].data;
    size_t dst_stride = (size_t)g_w * 4;
    for (int y = 0; y < g_h; y++) {
        uint32_t *row = (uint32_t *)(dst + (size_t)y * dst_stride);
        int sy = (int)((int64_t)y * height / g_h);
        const uint32_t *src = (const uint32_t *)((const char *)pixels + (size_t)sy * (size_t)stride_bytes);
        for (int x = 0; x < g_w; x++) {
            uint32_t rgba = src[(int64_t)x * width / g_w];
            uint32_t color = 0xff000000u | (rgba & 0x0000ff00u) |
                     ((rgba & 0x000000ffu) << 16) | ((rgba & 0x00ff0000u) >> 16);
            if (g_output_portrait)
                ((uint32_t *)dst)[(size_t)x * g_h + (g_h - 1 - y)] = color;
            else
                row[x] = color;
        }
    }
    g_bufs[slot].busy = 1;
    g_cur = (slot + 1) % NBUF;

    wl_surface_attach(g_surface, b, 0, 0);
    wl_surface_damage_buffer(g_surface, 0, 0,
                             g_output_portrait ? g_h : g_w,
                             g_output_portrait ? g_w : g_h);
    wl_surface_commit(g_surface);
    wl_display_flush(g_dpy);
    static unsigned long presented;
    if (++presented == 1 || presented % 300 == 0)
        fprintf(stderr, "[wl] presented frames: %lu\n", presented);
}

int k2c_display_screen_w(void) { return g_w; }
int k2c_display_screen_h(void) { return g_h; }

static void *g_staging;
static size_t g_staging_size;
static volatile int g_staged_ready;
static int g_staged_w, g_staged_h;
static pthread_mutex_t g_staging_mutex = PTHREAD_MUTEX_INITIALIZER;

int k2c_display_take_input(K2CInput *event)
{
    pthread_mutex_lock(&g_input_mutex);
    if (g_input_tail == g_input_head) { pthread_mutex_unlock(&g_input_mutex); return 0; }
    *event = g_input[g_input_tail];
    g_input_tail = (g_input_tail + 1) % INPUT_CAPACITY;
    pthread_mutex_unlock(&g_input_mutex);
    // MotionEvent coordinates are display pixels. Unity applies its own
    // render-resolution scale; using viewport pixels here scales input twice.
    event->x *= g_w;
    event->y *= g_h;
    return 1;
}

void k2c_display_stage(const void *pixels, int width, int height, int stride_bytes)
{
    if (!pixels || width <= 0 || height <= 0) return;
    pthread_mutex_lock(&g_staging_mutex);
    size_t need = (size_t)width * 4 * (size_t)height;
    if (!g_staging || g_staging_size < need) {
        free(g_staging);
        g_staging = malloc(need);
        g_staging_size = need;
    }
    if (!g_staging) { pthread_mutex_unlock(&g_staging_mutex); return; }
    size_t dst_stride = (size_t)width * 4;
    for (int y = 0; y < height; y++)
        memcpy((char *)g_staging + (size_t)y * dst_stride,
               (const char *)pixels + (size_t)y * (size_t)stride_bytes, (size_t)width * 4);
    g_staged_w = width;
    g_staged_h = height;
    __atomic_store_n(&g_staged_ready, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_staging_mutex);
}

int k2c_display_present_staged(void)
{
    pthread_mutex_lock(&g_staging_mutex);
    if (!__atomic_exchange_n(&g_staged_ready, 0, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&g_staging_mutex);
        return 0;
    }
    if (g_staging)
        k2c_display_present(g_staging, g_staged_w, g_staged_h, g_staged_w * 4);
    pthread_mutex_unlock(&g_staging_mutex);
    return 1;
}
