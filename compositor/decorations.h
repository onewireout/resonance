#ifndef DECORATIONS_H
#define DECORATIONS_H

/* Window chrome rendering for the Resonance compositor.
 *
 * Extracted from resonance.c (see Documentation/Resonance-Refactor-Plan.md). This
 * module owns everything the compositor draws *around* a client's surface:
 * the gradient window border, the dialog glass background, the per-app corner
 * radius, the memfd-backed resonance_buffer those render into (public API in
 * resonance_buffer.h, implemented here), and the xdg-decoration handler that
 * forces server-side decorations.
 *
 * The compositor core calls into these; they never reach back into core state
 * beyond the scene tree and dimensions they're handed. */

struct resonance_server;
struct wlr_scene_tree;
struct wlr_scene_buffer;
struct wlr_buffer;
struct wl_listener;

/* Width of the drawn window border, in pixels. Shared with the compositor
 * core's frame-geometry math (resonance_*_absolute_box, get_floor_maximized_box,
 * floor-maximize), which offsets windows by this much to account for the
 * frame the decorations module draws around them. */
#define BORDER_WIDTH 4

void get_window_radius(const char *app_id, const char *class,
		int *radius_top, int *radius_bottom);

void update_window_border(struct resonance_server *server,
		struct wlr_scene_tree *scene_tree,
		struct wlr_scene_buffer **border_node_ptr,
		struct wlr_buffer **border_buffer_ptr,
		int width, int height,
		int radius_top, int radius_bottom,
		int offset_x, int offset_y);

void update_dialog_background(struct wlr_scene_tree *scene_tree,
		struct wlr_scene_buffer **bg_node_ptr,
		struct wlr_buffer **bg_buffer_ptr,
		int width, int height,
		int radius_top, int radius_bottom,
		int geo_x, int geo_y);

void handle_new_toplevel_decoration(struct wl_listener *listener, void *data);

#endif /* DECORATIONS_H */
