// Минимальная клиентская реализация xdg-shell (v1) — только нужное.
#ifndef XDG_SHELL_MIN_H
#define XDG_SHELL_MIN_H

#include <wayland-client.h>

extern struct wl_interface xdg_wm_base_interface;
extern struct wl_interface xdg_surface_interface;
extern struct wl_interface xdg_toplevel_interface;

struct xdg_wm_base_listener {
    void (*ping)(void *data, struct wl_proxy *xdg_wm_base, uint32_t serial);
};
struct xdg_surface_listener {
    void (*configure)(void *data, struct wl_proxy *xdg_surface, uint32_t serial);
};
struct xdg_toplevel_listener {
    void (*configure)(void *data, struct wl_proxy *xdg_toplevel,
                      int32_t width, int32_t height, struct wl_array *states);
    void (*close)(void *data, struct wl_proxy *xdg_toplevel);
};

static inline struct wl_proxy *
xdg_wm_base_get_xdg_surface(struct wl_proxy *wm, struct wl_surface *surface)
{
    return wl_proxy_marshal_constructor(wm, 2, &xdg_surface_interface, NULL, surface);
}
static inline void
xdg_wm_base_pong(struct wl_proxy *wm, uint32_t serial)
{
    wl_proxy_marshal(wm, 3, serial);
}
static inline struct wl_proxy *
xdg_surface_get_toplevel(struct wl_proxy *xs)
{
    return wl_proxy_marshal_constructor(xs, 1, &xdg_toplevel_interface, NULL);
}
static inline void
xdg_surface_ack_configure(struct wl_proxy *xs, uint32_t serial)
{
    wl_proxy_marshal(xs, 3, serial);
}
static inline void
xdg_toplevel_set_title(struct wl_proxy *tl, const char *title)
{
    wl_proxy_marshal(tl, 2, title);
}
static inline void
xdg_toplevel_set_app_id(struct wl_proxy *tl, const char *app_id)
{
    wl_proxy_marshal(tl, 3, app_id);
}
static inline void
xdg_toplevel_set_fullscreen(struct wl_proxy *tl, struct wl_proxy *output)
{
    wl_proxy_marshal(tl, 11, output);
}

#endif
