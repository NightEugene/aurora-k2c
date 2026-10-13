// Минимальная xdg-shell: wl_interface-описания (xdg_wm_base/xdg_surface/xdg_toplevel v1).
#include "xdg_shell_min.h"

static struct wl_interface xdg_positioner_interface_placeholder = {
    "xdg_positioner", 1, 0, NULL, 0, NULL
};

static const struct wl_interface *xdg_wm_base_get_xdg_surface_types[] = {
    &xdg_surface_interface, &wl_surface_interface
};
static const struct wl_message xdg_wm_base_requests[] = {
    { "destroy", "", NULL },
    { "create_positioner", "n", (const struct wl_interface *[]){ &xdg_positioner_interface_placeholder } },
    { "get_xdg_surface", "no", xdg_wm_base_get_xdg_surface_types },
    { "pong", "u", NULL },
};
static const struct wl_message xdg_wm_base_events[] = {
    { "ping", "u", NULL },
};
struct wl_interface xdg_wm_base_interface = {
    "xdg_wm_base", 1, 4, xdg_wm_base_requests, 1, xdg_wm_base_events
};

static const struct wl_message xdg_surface_requests[] = {
    { "destroy", "", NULL },
    { "get_toplevel", "n", (const struct wl_interface *[]){ &xdg_toplevel_interface } },
    { "set_window_geometry", "4i", NULL },
    { "ack_configure", "u", NULL },
};
static const struct wl_message xdg_surface_events[] = {
    { "configure", "u", NULL },
};
struct wl_interface xdg_surface_interface = {
    "xdg_surface", 1, 4, xdg_surface_requests, 1, xdg_surface_events
};

static const struct wl_message xdg_toplevel_requests[] = {
    { "destroy", "", NULL },
    { "set_parent", "o", (const struct wl_interface *[]){ &xdg_toplevel_interface } },
    { "set_title", "s", NULL },
    { "set_app_id", "s", NULL },
    { "show_window_menu", "ou2i", NULL },
    { "move", "ou", NULL },
    { "resize", "ou", NULL },
    { "set_max_size", "2i", NULL },
    { "set_min_size", "2i", NULL },
    { "set_maximized", "", NULL },
    { "unset_maximized", "", NULL },
    { "set_fullscreen", "o", NULL },
    { "unset_fullscreen", "", NULL },
    { "set_minimized", "", NULL },
};
static const struct wl_message xdg_toplevel_events[] = {
    { "configure", "iia", NULL },
    { "close", "", NULL },
};
struct wl_interface xdg_toplevel_interface = {
    "xdg_toplevel", 1, 14, xdg_toplevel_requests, 2, xdg_toplevel_events
};
