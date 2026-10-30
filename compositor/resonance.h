#ifndef RESONANCE_H
#define RESONANCE_H

/* Shared type definitions for the Resonance compositor.
 *
 * resonance.c is being modularized incrementally (see
 * Documentation/Resonance-Refactor-Plan.md). This header holds the structs and
 * enums that every translation unit needs - most importantly the god-object
 * `struct resonance_server` and the parallel `resonance_toplevel` /
 * `resonance_xwayland_view` view structs. Satellite modules (decorations, config,
 * gestures, ipc) include this header and reach into the server struct through
 * it; the compositor core stays in resonance.c.
 *
 * It is intentionally self-contained: include this and you get the types,
 * without needing to include the wlroots headers yourself first. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/util/log.h>
#include <wlr/xwayland/xwayland.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <drm_fourcc.h>

#include "bootintro.h"

/* For brevity's sake, struct members are annotated where they are used. */
struct resonance_xwayland_view;

enum resonance_cursor_mode {
	RESONANCE_CURSOR_PASSTHROUGH,
	RESONANCE_CURSOR_MOVE,
	RESONANCE_CURSOR_RESIZE,
};

/* The floor system: windows live on one of three stacked "floors", each its
 * own scene subtree so z-order between floors is automatic from tree order.
 * Which floor a given window belongs on is decided by core/placement.py, not
 * here - see floor_trees in resonance_server and ipc_handle_place_command. */
enum resonance_floor {
	RESONANCE_FLOOR_BASEMENT = 0,
	RESONANCE_FLOOR_GROUND,
	RESONANCE_FLOOR_FIRST,
	RESONANCE_FLOOR_SECOND,
	RESONANCE_FLOOR_COUNT,
};

struct resonance_keybinding {
	uint32_t modifiers;
	xkb_keysym_t keysym;
	char *name;
	char *action;
};

struct resonance_server {
	struct wl_display *wl_display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	struct wlr_scene_tree *layer_trees[4];
	struct wlr_scene_tree *windows_tree;

	/* Floor system: windows_tree's children, in fixed z-order (basement
	 * behind ground behind first) - see floor_tree_for(). Which floor a
	 * window lands on is decided entirely by core/placement.py; resonance
	 * only reparents into whichever tree it's told via the place: IPC
	 * command (see ipc_handle_place_command). */
	struct wlr_scene_tree *floor_trees[RESONANCE_FLOOR_COUNT];

	/* Parentless override-redirect XWayland popups (menus, tooltips,
	 * dropdowns). Kept above every floor so a promoted parent window can't
	 * bury its own transient UI - see xwayland_view_associate. */
	struct wlr_scene_tree *xwayland_popup_tree;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_list toplevels;

	struct wlr_xwayland *xwayland;
	struct wl_listener new_xwayland_surface;
	struct wl_list xwayland_views;

	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener new_layer_surface;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;
	/* Last time (CLOCK_MONOTONIC ms) a cursor-pos: IPC broadcast was sent -
	 * throttles the global-cursor-position feed (see process_cursor_motion)
	 * so it doesn't flood the socket on every raw motion event. */
	uint32_t last_cursor_broadcast_ms;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_listener drag_icon_destroy;
	bool drag_icon_destroy_connected;
	struct wl_list keyboards;
	enum resonance_cursor_mode cursor_mode;
	struct resonance_toplevel *grabbed_toplevel;
	struct resonance_xwayland_view *grabbed_xwayland_view;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	/* Push-resize state (see resonance_push_begin/apply/release): at most one
	 * same-floor neighbor per axis gets nudged while an edge resize is in
	 * progress - whichever window is nearest the edge being dragged, in
	 * the drag direction, at the moment the drag started. Whether it's
	 * actually moving is re-evaluated live every step (see
	 * resonance_push_apply_axis), not decided once at grab time. No chaining
	 * beyond that one neighbor per axis, by design. Both NULL on an axis
	 * means no same-floor neighbor exists in that direction at all. */
	struct resonance_toplevel *push_h_toplevel;
	struct resonance_xwayland_view *push_h_view;
	struct wlr_box push_h_box;
	uint32_t push_h_edge;
	struct resonance_toplevel *push_v_toplevel;
	struct resonance_xwayland_view *push_v_view;
	struct wlr_box push_v_box;
	uint32_t push_v_edge;

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	struct wlr_session *session;
	struct wl_listener session_active;
	struct wlr_presentation *presentation;
	struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;
	struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
	struct wl_listener new_toplevel_decoration;
	struct wl_listener new_capture_session;

	struct wlr_pointer_constraints_v1 *pointer_constraints;
	struct wl_listener new_pointer_constraint;
	struct wlr_pointer_constraint_v1 *active_constraint;
	struct wl_listener active_constraint_destroy;
	struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;

	const char *socket;

	int ipc_fd;
	struct wl_event_source *ipc_event_source;
	char ipc_inbuf[1024];
	size_t ipc_inbuf_len;
	char ipc_outbuf[4096];
	size_t ipc_outbuf_len;
	uint32_t next_window_id;

	/* Super+flick gesture tracking */
	double gesture_last_x, gesture_last_y;
	double gesture_up_accum, gesture_up_drift;
	double gesture_down_accum, gesture_down_drift;
	double gesture_right_accum, gesture_right_drift;
	uint32_t gesture_last_time;
	bool gesture_fired;

	struct wlr_output *cursor_output;
	bool backend_failed;

	struct resonance_keybinding *keybindings;
	size_t keybindings_len;

	/* Graceful shutdown (see resonance_begin_shutdown): super+escape asks every
	 * window to close and waits for them to unmap rather than tearing the
	 * display down under them - clients that lose the socket unannounced
	 * treat it as a crash (Firefox offers session restore next launch) or
	 * fail to notice at all and linger as orphans (Electron apps). */
	bool shutting_down;
	struct wl_event_source *shutdown_timer;

	struct bootintro_state boot;
	struct resonance_toplevel *hovered_toplevel;
	struct wlr_scene_tree *drag_icon_tree;
	struct wlr_drag_icon *drag_icon;
};

struct resonance_output {
	struct wl_list link;
	struct resonance_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
	int capture_count;
};

/* Tracks one ext-image-copy-capture session so we can re-enable adaptive
 * sync on its output when it ends. */
struct capture_session_tracker {
	struct resonance_output *output;
	struct wl_listener destroy;
};

struct resonance_toplevel {
	bool is_xwayland; /* must be first field; false for xdg toplevels */
	struct wl_list link;
	struct resonance_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	bool floor_maximized;
	/* The floor slot this window was maximized into, snapshotted when
	 * maximize was applied. The border is drawn to this rather than to a
	 * freshly measured slot, so a neighbour moving or shrinking mid-drag
	 * can't grow the border out from under a surface that isn't resizing
	 * to match. */
	int maxed_w, maxed_h;
	struct wlr_foreign_toplevel_handle_v1 *foreign_handle;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener set_parent;
	int last_w, last_h;
	bool user_positioned;
	/* True from map until either a place: command arrives or
	 * placement_timeout fires - suppresses the commit handler's
	 * re-center fallback so it can't race an in-flight external
	 * placement decision (see toplevel_placement_timeout). */
	bool awaiting_placement;
	struct wl_event_source *placement_timeout;
	bool fullscreen;
	int saved_x, saved_y, saved_w, saved_h;
	struct wlr_scene_buffer *border_node;
	struct wlr_buffer *border_buffer;
	/* Compositor-drawn background for dialog/transient windows */
	struct wlr_scene_buffer *dialog_bg_node;
	struct wlr_buffer *dialog_bg_buffer;
	bool resize_dirty;
	int pending_x, pending_y;
	int pending_w, pending_h;
	uint32_t last_configure_time;
	uint32_t id;
	enum resonance_floor floor;
};

struct resonance_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct resonance_layer_surface {
	struct resonance_server *server;
	struct wlr_scene_layer_surface_v1 *scene_layer_surface;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct resonance_decoration {
	struct wlr_xdg_toplevel_decoration_v1 *wlr_decoration;
	struct wl_listener destroy;
	struct wl_listener surface_commit;
};


struct resonance_xwayland_view {
	bool is_xwayland; /* must be first field; true for xwayland surfaces */
	struct wl_list link;
	struct resonance_server *server;
	struct wlr_xwayland_surface *xwayland_surface;
	struct wlr_scene_tree *scene_tree;
	bool floor_maximized;
	/* See the matching field on resonance_toplevel. */
	int maxed_w, maxed_h;
	struct wlr_foreign_toplevel_handle_v1 *foreign_handle;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener destroy;
	struct wl_listener scene_tree_destroy;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener request_activate;
	struct wl_listener request_configure;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_fullscreen;
	int last_w, last_h;
	int x_offset, y_offset;
	bool hidden_for_sync;
	int sync_flicker_frames;
	bool fullscreen;
	int saved_x, saved_y, saved_w, saved_h;
	struct wlr_scene_buffer *border_node;
	struct wlr_buffer *border_buffer;
	uint32_t id;
	enum resonance_floor floor;
};

struct resonance_keyboard {
	struct wl_list link;
	struct resonance_server *server;
	struct wlr_keyboard *wlr_keyboard;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* Inbound IPC command handler — defined in resonance.c (the core), called
 * from ipc.c's read loop when a full line arrives from the hub. */
void core_handle_ipc_command(struct resonance_server *server, char *line);

#endif /* RESONANCE_H */
