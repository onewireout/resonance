#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include <ctype.h>
#include <math.h>
#include <linux/input-event-codes.h>
#include <string.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
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

#include "resonance_buffer.h"
#include "bootintro.h"

#include "resonance.h"
#include "config.h"
#include "decorations.h"
#include "gestures.h"
#include "ipc.h"

static void schedule_frames(struct resonance_server *server);

/* Push-resize (see full implementation and comments near resonance_push_begin) */
static void resonance_push_begin(struct resonance_server *server, enum resonance_floor floor,
		struct resonance_toplevel *exclude_toplevel, struct resonance_xwayland_view *exclude_view,
		uint32_t edges);
static void resonance_push_apply(struct resonance_server *server);
static void resonance_push_release(struct resonance_server *server);



static void deactivate_surface(struct wlr_surface *surface) {
	if (!surface) return;
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	if (!root) {
		root = surface;
	}
	struct wlr_xdg_toplevel *xdg_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(root);
	if (xdg_toplevel) {
		wlr_xdg_toplevel_set_activated(xdg_toplevel, false);
	} else {
		struct wlr_xwayland_surface *xsurface = wlr_xwayland_surface_try_from_wlr_surface(root);
		if (xsurface) {
			wlr_xwayland_surface_activate(xsurface, false);
		}
	}
}

static void focus_toplevel(struct resonance_toplevel *toplevel) {
	/* Note: this function only deals with keyboard focus. */
	if (toplevel == NULL) {
		return;
	}
	struct resonance_server *server = toplevel->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	struct wlr_surface *prev_root = prev_surface ? wlr_surface_get_root_surface(prev_surface) : NULL;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
	if (prev_root == surface) {
		/* Don't re-focus an already focused surface. */
		return;
	}
	deactivate_surface(prev_surface);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	/* Move the toplevel to the front */
	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	/* Activate the new surface */
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	/*
	 * Tell the seat to have the keyboard enter this surface. wlroots will keep
	 * track of this and automatically send key events to the appropriate
	 * clients without additional work on your part.
	 */
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

static void focus_xwayland_view(struct resonance_xwayland_view *view) {
	if (view == NULL) {
		return;
	}
	struct resonance_server *server = view->server;
	struct wlr_seat *seat = server->seat;

	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	struct wlr_surface *prev_root = prev_surface ? wlr_surface_get_root_surface(prev_surface) : NULL;
	if (prev_root == view->xwayland_surface->surface) {
		return;
	}
	deactivate_surface(prev_surface);

	if (view->scene_tree) {
		wlr_scene_node_raise_to_top(&view->scene_tree->node);
	}
	wlr_xwayland_surface_activate(view->xwayland_surface, true);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard != NULL && view->xwayland_surface->surface != NULL) {
		wlr_seat_keyboard_notify_enter(seat, view->xwayland_surface->surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	struct resonance_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	/*
	 * A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same seat. You can swap out the underlying wlr_keyboard like this and
	 * wlr_seat handles this transparently.
	 */
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static void resonance_toplevel_set_fullscreen(struct resonance_toplevel *toplevel, bool fullscreen);
static void resonance_xwayland_view_set_fullscreen(struct resonance_xwayland_view *view, bool fullscreen);
static void resonance_toplevel_set_floor_maximized(struct resonance_toplevel *toplevel, bool maximized);
static void resonance_xwayland_view_set_floor_maximized(struct resonance_xwayland_view *view, bool maximized);
static void toggle_floor_maximized_focused(struct resonance_server *server);
static void xdg_toplevel_apply_floor_maximized(struct resonance_toplevel *toplevel);
static void toplevel_cancel_placement_wait(struct resonance_toplevel *toplevel);

/* Handles a "fullscreen:<id>\t<0|1>" IPC command. */
static void ipc_handle_fullscreen_command(struct resonance_server *server, char *line) {
	char *rest = line + 11; /* skip "fullscreen:" */
	char *saveptr = NULL;
	char *id_s = strtok_r(rest, "\t", &saveptr);
	char *fs_s = strtok_r(NULL, "\t", &saveptr);
	if (!id_s || !fs_s) {
		return;
	}
	uint32_t id = (uint32_t)strtoul(id_s, NULL, 10);
	bool fullscreen = atoi(fs_s) != 0;

	struct resonance_toplevel *toplevel;
	struct resonance_xwayland_view *view;
	ipc_find_window_by_id(server, id, &toplevel, &view);

	if (toplevel) {
		resonance_toplevel_set_fullscreen(toplevel, fullscreen);
	} else if (view) {
		resonance_xwayland_view_set_fullscreen(view, fullscreen);
	}
}

/* Parses a floor name ("basement"/"ground"/"first"/"second") into a
 * resonance_floor. Returns false (leaving *out untouched) if unrecognized, so
 * a caller can fall back to leaving the window's current floor alone. */
static bool floor_from_string(const char *s, enum resonance_floor *out) {
	if (strcmp(s, "basement") == 0) {
		*out = RESONANCE_FLOOR_BASEMENT;
	} else if (strcmp(s, "ground") == 0) {
		*out = RESONANCE_FLOOR_GROUND;
	} else if (strcmp(s, "first") == 0) {
		*out = RESONANCE_FLOOR_FIRST;
	} else if (strcmp(s, "second") == 0) {
		*out = RESONANCE_FLOOR_SECOND;
	} else {
		return false;
	}
	return true;
}

/* Handles a "place:<id>\t<x>\t<y>\t<w>\t<h>[\t<floor>]" IPC command. The
 * trailing floor field is optional so older placement.py builds that don't
 * send one still work - the window just keeps whatever floor it's already
 * on. Floor assignment itself is entirely core/placement.py's call; this
 * only executes it (see floor_trees in resonance_server). */
static void ipc_handle_place_command(struct resonance_server *server, char *line) {
	char *rest = line + 6; /* skip "place:" */
	char *saveptr = NULL;
	char *id_s = strtok_r(rest, "\t", &saveptr);
	char *x_s  = strtok_r(NULL, "\t", &saveptr);
	char *y_s  = strtok_r(NULL, "\t", &saveptr);
	char *w_s  = strtok_r(NULL, "\t", &saveptr);
	char *h_s  = strtok_r(NULL, "\t", &saveptr);
	char *floor_s = strtok_r(NULL, "\t", &saveptr);
	if (!id_s || !x_s || !y_s || !w_s || !h_s) {
		return;
	}
	uint32_t id = (uint32_t)strtoul(id_s, NULL, 10);
	int x = atoi(x_s);
	int y = atoi(y_s);
	int w = atoi(w_s);
	int h = atoi(h_s);
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	enum resonance_floor floor;
	bool have_floor = floor_s && floor_from_string(floor_s, &floor);

	struct resonance_toplevel *toplevel;
	struct resonance_xwayland_view *view;
	ipc_find_window_by_id(server, id, &toplevel, &view);

	if (toplevel) {
		if (toplevel->fullscreen) {
			resonance_toplevel_set_fullscreen(toplevel, false);
		}
		toplevel_cancel_placement_wait(toplevel);
		toplevel->user_positioned = true;
		if (have_floor && floor != toplevel->floor) {
			toplevel->floor = floor;
			wlr_scene_node_reparent(&toplevel->scene_tree->node,
				server->floor_trees[floor]);
		}
		wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, w, h);
		/* wlr_scene_node_set_position() alone never triggers an xdg surface
		 * commit, so if the requested size matches what the client already
		 * has (the common case), xdg_toplevel_commit never fires and brain.py
		 * never hears about the new position - it just keeps re-saving the
		 * stale pre-placement geometry on every tick. Report it ourselves.
		 * Deliberately NOT stamping last_w/last_h here: those must stay at
		 * the pre-placement size so that when the client's real resize
		 * commit lands, xdg_toplevel_commit's change-detection still fires
		 * and redraws the border/decoration to match. */
		ipc_send_window_geo(server, "geometry",
			toplevel->xdg_toplevel->title, toplevel->xdg_toplevel->app_id,
			x, y, w, h, toplevel->id, toplevel->fullscreen);
	} else if (view) {
		if (view->fullscreen) {
			resonance_xwayland_view_set_fullscreen(view, false);
		}
		if (have_floor && floor != view->floor) {
			view->floor = floor;
			wlr_scene_node_reparent(&view->scene_tree->node,
				server->floor_trees[floor]);
		}
		wlr_scene_node_set_position(&view->scene_tree->node, x, y);
		wlr_xwayland_surface_configure(view->xwayland_surface, x, y, w, h);
		ipc_send_window_geo(server, "geometry",
			view->xwayland_surface->title, view->xwayland_surface->class,
			x, y, w, h, view->id, view->fullscreen);
	}
}

/* IPC transport (connect, send, read loop) lives in ipc.c. The read loop
 * dispatches inbound lines here — the command handlers below drive the
 * scene graph and stay in the core. */

/* Handles a single inbound IPC line, e.g. "place:<id>\t<x>\t<y>\t<w>\t<h>"
 *  or "fullscreen:<id>\t<0|1>". */
void core_handle_ipc_command(struct resonance_server *server, char *line) {
	if (strncmp(line, "fullscreen:", 11) == 0) {
		ipc_handle_fullscreen_command(server, line);
		return;
	}
	if (strncmp(line, "place:", 6) == 0) {
		ipc_handle_place_command(server, line);
		return;
	}
}

/* Keybinding config loading lives in config.c. */

/* Window chrome rendering (gradient borders, dialog glass, resonance_buffer,
 * xdg-decoration handler) lives in decorations.c. */

static void toggle_fullscreen_focused(struct resonance_server *server) {
	struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
	if (!focused) {
		return;
	}
	struct wlr_surface *root = wlr_surface_get_root_surface(focused);

	/* Find matching XWayland view */
	struct wlr_xwayland_surface *xsurface = wlr_xwayland_surface_try_from_wlr_surface(root);
	if (xsurface) {
		while (xsurface->parent) {
			xsurface = xsurface->parent;
		}

		struct resonance_xwayland_view *xview;
		wl_list_for_each(xview, &server->xwayland_views, link) {
			if (xview->xwayland_surface != xsurface) continue;
			resonance_xwayland_view_set_fullscreen(xview, !xview->fullscreen);
			return;
		}
	}

	/* Find matching XDG toplevel */
	struct resonance_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel->xdg_toplevel->base->surface != root) continue;
		resonance_toplevel_set_fullscreen(toplevel, !toplevel->fullscreen);
		return;
	}
}

static bool handle_keybinding(struct resonance_server *server, xkb_keysym_t sym) {
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 *
	 * This function assumes Alt is held down.
	 */
	switch (sym) {
	default:
		return false;
	}
	return true;
}

static bool keysym_is_modifier(xkb_keysym_t sym) {
	switch (sym) {
	case XKB_KEY_Shift_L: case XKB_KEY_Shift_R:
	case XKB_KEY_Control_L: case XKB_KEY_Control_R:
	case XKB_KEY_Caps_Lock:
	case XKB_KEY_Shift_Lock:
	case XKB_KEY_Meta_L: case XKB_KEY_Meta_R:
	case XKB_KEY_Alt_L: case XKB_KEY_Alt_R:
	case XKB_KEY_Super_L: case XKB_KEY_Super_R:
	case XKB_KEY_Hyper_L: case XKB_KEY_Hyper_R:
		return true;
	default:
		return false;
	}
}

/* Seconds to wait for windows to close on their own before terminating
 * anyway. Long enough for a browser to flush its session, short enough that
 * a client which never intends to exit doesn't hold the session hostage. */
#define SHUTDOWN_GRACE_SECS 3

static int count_open_windows(struct resonance_server *server) {
	return wl_list_length(&server->toplevels) +
		wl_list_length(&server->xwayland_views);
}

static void finish_shutdown(struct resonance_server *server, const char *why) {
	wlr_log(WLR_INFO, "shutdown: terminating (%s), %d window(s) still open",
		why, count_open_windows(server));
	if (server->shutdown_timer) {
		wl_event_source_remove(server->shutdown_timer);
		server->shutdown_timer = NULL;
	}
	wl_display_terminate(server->wl_display);
}

static int shutdown_grace_expired(void *data) {
	struct resonance_server *server = data;

	/* Name what's outstanding - a window still here after the full grace
	 * period either ignored the close request or is holding a modal (an
	 * unsaved-changes prompt), and that's what to look for in the log if a
	 * client keeps surviving the restart. */
	struct resonance_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel->xdg_toplevel) {
			wlr_log(WLR_INFO, "shutdown: still open (xdg): app_id=%s title=%s",
				toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "(null)",
				toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "(null)");
		}
	}
	struct resonance_xwayland_view *view;
	wl_list_for_each(view, &server->xwayland_views, link) {
		if (view->xwayland_surface) {
			wlr_log(WLR_INFO, "shutdown: still open (xwayland): class=%s title=%s",
				view->xwayland_surface->class ? view->xwayland_surface->class : "(null)",
				view->xwayland_surface->title ? view->xwayland_surface->title : "(null)");
		}
	}

	finish_shutdown(server, "grace period expired");
	return 0;
}

/* Called from both unmap paths - once the last window is gone there's
 * nothing left to wait for, so don't sit out the rest of the grace period. */
static void resonance_shutdown_check_done(struct resonance_server *server) {
	if (!server->shutting_down) {
		return;
	}
	if (count_open_windows(server) == 0) {
		finish_shutdown(server, "all windows closed");
	}
}

static void resonance_begin_shutdown(struct resonance_server *server) {
	if (server->shutting_down) {
		/* Second super+escape while already winding down - read as "stop
		 * waiting, I mean it", so a client sitting on a modal can't make the
		 * shortcut feel broken. */
		finish_shutdown(server, "second quit request");
		return;
	}
	server->shutting_down = true;

	int asked = 0;
	struct resonance_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel->xdg_toplevel) {
			wlr_log(WLR_INFO, "shutdown: asking to close (xdg): app_id=%s",
				toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "(null)");
			wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
			asked++;
		}
	}
	struct resonance_xwayland_view *view;
	wl_list_for_each(view, &server->xwayland_views, link) {
		if (view->xwayland_surface) {
			wlr_log(WLR_INFO, "shutdown: asking to close (xwayland): class=%s",
				view->xwayland_surface->class ? view->xwayland_surface->class : "(null)");
			wlr_xwayland_surface_close(view->xwayland_surface);
			asked++;
		}
	}

	wlr_log(WLR_INFO, "shutdown: asked %d window(s) to close, waiting up to %ds",
		asked, SHUTDOWN_GRACE_SECS);

	if (asked == 0) {
		finish_shutdown(server, "nothing open");
		return;
	}

	server->shutdown_timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->wl_display),
		shutdown_grace_expired, server);
	if (!server->shutdown_timer) {
		/* No timer means no backstop, and a client that never closes would
		 * hang the session forever - quit now rather than risk that. */
		finish_shutdown(server, "failed to arm grace timer");
		return;
	}
	wl_event_source_timer_update(server->shutdown_timer,
		SHUTDOWN_GRACE_SECS * 1000);
}

static void keyboard_handle_key(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a key is pressed or released. */
	struct resonance_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct resonance_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);

	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			if (syms[i] >= XKB_KEY_XF86Switch_VT_1 &&
					syms[i] <= XKB_KEY_XF86Switch_VT_12) {
				if (server->session) {
					struct wlr_session *session = server->session;
					unsigned vt = syms[i] - XKB_KEY_XF86Switch_VT_1 + 1;
					wlr_session_change_vt(session, vt);
				}
				handled = true;
			}
		}
	}

	if (!handled && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			if (syms[i] == XKB_KEY_XF86AudioRaiseVolume) {
				ipc_send(server, "volume:up\n");
				handled = true; break;
			}
			if (syms[i] == XKB_KEY_XF86AudioLowerVolume) {
				ipc_send(server, "volume:down\n");
				handled = true; break;
			}
			if (syms[i] == XKB_KEY_XF86AudioMute) {
				ipc_send(server, "volume:mute\n");
				handled = true; break;
			}
		}
	}

	if (!handled && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < nsyms; i++) {
			/* Super+Escape: fallback exit shortcut */
			if (syms[i] == XKB_KEY_Escape && (modifiers & WLR_MODIFIER_LOGO)) {
				resonance_begin_shutdown(server);
				handled = true;
				break;
			}

			/* Super+C: close focused window (supports both XDG and XWayland, handling subsurfaces & popups) */
			if (syms[i] == XKB_KEY_c && (modifiers & WLR_MODIFIER_LOGO)) {
				struct wlr_surface *focused =
					server->seat->keyboard_state.focused_surface;
				if (focused) {
					struct wlr_surface *root_surface = wlr_surface_get_root_surface(focused);
					struct wlr_xdg_surface *xdg_surface =
						wlr_xdg_surface_try_from_wlr_surface(root_surface);
					if (xdg_surface) {
						while (xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP && xdg_surface->popup) {
							struct wlr_surface *parent_surf = xdg_surface->popup->parent;
							if (!parent_surf) break;
							root_surface = wlr_surface_get_root_surface(parent_surf);
							xdg_surface = wlr_xdg_surface_try_from_wlr_surface(root_surface);
						}
						if (xdg_surface && xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xdg_surface->toplevel) {
							wlr_xdg_toplevel_send_close(xdg_surface->toplevel);
						}
					} else {
						struct wlr_xwayland_surface *xsurface =
							wlr_xwayland_surface_try_from_wlr_surface(root_surface);
						if (xsurface) {
							while (xsurface->parent) {
								xsurface = xsurface->parent;
							}
							wlr_xwayland_surface_close(xsurface);
						}
					}
				}
				handled = true;
				break;
			}

			/* Check dynamic keybindings config */
			for (size_t j = 0; j < server->keybindings_len; j++) {
				struct resonance_keybinding *kb = &server->keybindings[j];
				uint32_t mask = WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT;
				if ((modifiers & mask) == kb->modifiers && syms[i] == kb->keysym) {
					if (kb->action && strcmp(kb->action, "fullscreen") == 0) {
						toggle_floor_maximized_focused(server);
					} else if (kb->action && strcmp(kb->action, "true_fullscreen") == 0) {
						toggle_fullscreen_focused(server);
					} else {
						char msg[384];
						snprintf(msg, sizeof(msg), "keybind:%s\n", kb->name);
						ipc_send(server, msg);
					}
					handled = true;
					break;
				}
			}
			if (handled) break;
		}
	}

	if (!handled && (modifiers & WLR_MODIFIER_LOGO)) {
		bool all_modifiers = true;
		for (int i = 0; i < nsyms; i++) {
			if (!keysym_is_modifier(syms[i])) {
				all_modifiers = false;
				break;
			}
		}
		if (!all_modifiers) {
			handled = true;
		}
	}

	if (!handled) {
		/* Otherwise, we pass it along to the client. */
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	/* This event is raised by the keyboard base wlr_input_device to signal
	 * the destruction of the wlr_keyboard. It will no longer receive events
	 * and should be destroyed.
	 */
	struct resonance_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void server_new_keyboard(struct resonance_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct resonance_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	if (!keyboard) {
		wlr_log(WLR_ERROR, "Failed to allocate keyboard structure!");
		return;
	}
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	/* We need to prepare an XKB keymap and assign it to the keyboard. This
	 * assumes the defaults (e.g. layout = "us"). */
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap) {
		wlr_log(WLR_ERROR, "Failed to compile XKB keymap!");
		xkb_context_unref(context);
		free(keyboard);
		return;
	}

	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	/* Here we set up listeners for keyboard events. */
	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

	/* And add the keyboard to our list of keyboards */
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct resonance_server *server,
		struct wlr_input_device *device) {
	/* We don't do anything special with pointers. All of our pointer handling
	 * is proxied through wlr_cursor. On another compositor, you might take this
	 * opportunity to do libinput configuration on the device to set
	 * acceleration, etc. */
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct resonance_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;
	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}
	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In Resonance we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct resonance_server *server = wl_container_of(
			listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	if (event->seat_client != focused_client) {
		return;
	}
	wlr_cursor_set_surface(server->cursor, event->surface,
		event->hotspot_x, event->hotspot_y);
}

static void handle_pointer_constraint_destroy(struct wl_listener *listener, void *data) {
	struct resonance_server *server = wl_container_of(listener, server, active_constraint_destroy);
	wl_list_remove(&server->active_constraint_destroy.link);
	server->active_constraint = NULL;
}

static void activate_constraint(struct resonance_server *server,
		struct wlr_pointer_constraint_v1 *constraint) {
	if (server->active_constraint) {
		struct wlr_pointer_constraint_v1 *old = server->active_constraint;
		/* Remove the destroy listener BEFORE send_deactivated — a ONESHOT constraint
		 * may be destroyed immediately inside send_deactivated, which would fire
		 * handle_pointer_constraint_destroy and double-remove the listener. */
		wl_list_remove(&server->active_constraint_destroy.link);
		server->active_constraint = NULL;
		wlr_pointer_constraint_v1_send_deactivated(old);
	}
	server->active_constraint = constraint;
	if (constraint) {
		server->active_constraint_destroy.notify = handle_pointer_constraint_destroy;
		wl_signal_add(&constraint->events.destroy, &server->active_constraint_destroy);
		wlr_pointer_constraint_v1_send_activated(constraint);
		if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
			wlr_cursor_set_surface(server->cursor, NULL, 0, 0);
		}
	} else {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
}

static struct resonance_toplevel *desktop_toplevel_at(struct resonance_server *server,
		double lx, double ly, struct wlr_surface **surface,
		double *sx, double *sy);

static void handle_new_pointer_constraint(struct wl_listener *listener, void *data) {
	struct resonance_server *server = wl_container_of(listener, server, new_pointer_constraint);
	struct wlr_pointer_constraint_v1 *constraint = data;
	struct wlr_surface *focused = server->seat->pointer_state.focused_surface;
	if (focused == constraint->surface) {
		activate_constraint(server, constraint);
	}
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct resonance_server *server = wl_container_of(
			listener, server, pointer_focus_change);
	struct wlr_seat_pointer_focus_change_event *event = data;

	if (server->active_constraint &&
			server->active_constraint->surface != event->new_surface) {
		bool to_override_redirect = false;
		if (event->new_surface) {
			struct wlr_surface *root = wlr_surface_get_root_surface(event->new_surface);
			struct wlr_xwayland_surface *xsurface = wlr_xwayland_surface_try_from_wlr_surface(root);
			to_override_redirect = xsurface && xsurface->override_redirect;
		}
		if (!to_override_redirect) {
			activate_constraint(server, NULL);
		}
	}

	if (event->new_surface && !server->active_constraint && server->pointer_constraints) {
		struct wlr_pointer_constraint_v1 *constraint =
			wlr_pointer_constraints_v1_constraint_for_surface(
				server->pointer_constraints, event->new_surface, server->seat);
		if (constraint) {
			activate_constraint(server, constraint);
		}
	}

	if (event->new_surface == NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in resonance we always honor
	 */
	struct resonance_server *server = wl_container_of(
			listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void seat_request_start_drag(struct wl_listener *listener, void *data) {
	struct resonance_server *server = wl_container_of(
			listener, server, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;

	if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin, event->serial)) {
		wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
		return;
	}

	struct wlr_touch_point *point;
	if (wlr_seat_validate_touch_grab_serial(server->seat, event->origin, event->serial, &point)) {
		wlr_seat_start_touch_drag(server->seat, event->drag, event->serial, point);
		return;
	}

	wlr_data_source_destroy(event->drag->source);
}

static void update_drag_icon_position(struct resonance_server *server) {
	if (server->drag_icon_tree == NULL || server->drag_icon == NULL ||
			server->drag_icon->surface == NULL) {
		return;
	}

	wlr_scene_node_set_position(&server->drag_icon_tree->node,
		server->cursor->x, server->cursor->y);
}

static void drag_icon_destroy(struct wl_listener *listener, void *data) {
	struct resonance_server *server = wl_container_of(
			listener, server, drag_icon_destroy);

	server->drag_icon_tree = NULL;
	server->drag_icon = NULL;
	wl_list_remove(&server->drag_icon_destroy.link);
	wl_list_init(&server->drag_icon_destroy.link);
	server->drag_icon_destroy_connected = false;
	schedule_frames(server);
}

static void seat_start_drag(struct wl_listener *listener, void *data) {
	struct resonance_server *server = wl_container_of(
			listener, server, start_drag);
	struct wlr_drag *drag = data;

	if (drag->icon != NULL) {
		if (server->drag_icon_destroy_connected) {
			wl_list_remove(&server->drag_icon_destroy.link);
			wl_list_init(&server->drag_icon_destroy.link);
			server->drag_icon_destroy_connected = false;
		}
		server->drag_icon_tree =
			wlr_scene_drag_icon_create(server->layer_trees[3], drag->icon);
		server->drag_icon = drag->icon;
		update_drag_icon_position(server);
		server->drag_icon_destroy.notify = drag_icon_destroy;
		wl_signal_add(&drag->icon->events.destroy, &server->drag_icon_destroy);
		server->drag_icon_destroy_connected = true;
		schedule_frames(server);
	}
}

static struct resonance_toplevel *desktop_toplevel_at(
		struct resonance_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	/* This returns the topmost node in the scene at the given layout coords.
	 * We only care about surface nodes as we are specifically looking for a
	 * surface in the surface tree of a resonance_toplevel. */
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}
	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) {
		return NULL;
	}

	*surface = scene_surface->surface;
	/* Find the node corresponding to the resonance_toplevel at the root of this
	 * surface tree, it is the only one for which we set the data field. */
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	if (tree == NULL) {
		return NULL;
	}
	return tree->node.data;
}

static void reset_cursor_mode(struct resonance_server *server) {
	if (server->grabbed_xwayland_view != NULL &&
			server->grabbed_xwayland_view->scene_tree != NULL) {
		struct resonance_xwayland_view *view = server->grabbed_xwayland_view;
		if (server->cursor_mode == RESONANCE_CURSOR_MOVE) {
			/* view->scene_tree->node.x/y is parent-relative when this view is
			 * nested inside its parent's own scene tree (see
			 * xwayland_view_associate) - wlr_xwayland_surface_configure wants
			 * absolute (root-window) coordinates, so add the parent's
			 * on-screen position back in before telling the X11 surface where
			 * it is. Sending the raw relative value here previously desynced
			 * xwayland_surface->x/y from the true screen position, causing
			 * the dialog to jump on the next commit. */
			int abs_x = view->scene_tree->node.x;
			int abs_y = view->scene_tree->node.y;
			if (view->scene_tree->node.parent) {
				int px = 0, py = 0;
				wlr_scene_node_coords(&view->scene_tree->node.parent->node, &px, &py);
				abs_x += px;
				abs_y += py;
			}
			wlr_xwayland_surface_configure(view->xwayland_surface,
				abs_x, abs_y,
				view->xwayland_surface->width, view->xwayland_surface->height);
		}
		if (!view->xwayland_surface->override_redirect && !view->xwayland_surface->parent) {
			ipc_send_window_geo(server, "geometry",
				view->xwayland_surface->title, view->xwayland_surface->class,
				view->scene_tree->node.x, view->scene_tree->node.y,
				view->xwayland_surface->width, view->xwayland_surface->height, view->id,
				view->fullscreen);
			char release_buf[32];
			snprintf(release_buf, sizeof(release_buf), "release:%u\n", view->id);
			ipc_send(server, release_buf);
		}
	} else if (server->grabbed_toplevel != NULL) {
		struct resonance_toplevel *toplevel = server->grabbed_toplevel;
		if (server->cursor_mode == RESONANCE_CURSOR_RESIZE) {
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->pending_w, toplevel->pending_h);
		}
		struct wlr_xdg_surface *xdg_surf = toplevel->xdg_toplevel->base;
		struct wlr_box geo = xdg_surf->geometry;
		struct wlr_surface *surf = xdg_surf->surface;
		int out_x, out_y, out_w, out_h;
		if (geo.width > 0 && geo.height > 0) {
			out_x = toplevel->scene_tree->node.x;
			out_y = toplevel->scene_tree->node.y;
			out_w = geo.width;
			out_h = geo.height;
		} else {
			out_x = toplevel->scene_tree->node.x;
			out_y = toplevel->scene_tree->node.y;
			out_w = surf->current.width;
			out_h = surf->current.height;
		}
		ipc_send_window_geo(server, "geometry",
			toplevel->xdg_toplevel->title, toplevel->xdg_toplevel->app_id,
			out_x, out_y, out_w, out_h, toplevel->id, toplevel->fullscreen);
		char release_buf[32];
		snprintf(release_buf, sizeof(release_buf), "release:%u\n", toplevel->id);
		ipc_send(server, release_buf);
	}
	resonance_push_release(server);
	server->cursor_mode = RESONANCE_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
	server->grabbed_xwayland_view = NULL;
}

static void process_cursor_move(struct resonance_server *server) {
	if (server->grabbed_xwayland_view) {
		struct resonance_xwayland_view *view = server->grabbed_xwayland_view;
		int new_x = (int)(server->cursor->x - server->grab_x);
		int new_y = (int)(server->cursor->y - server->grab_y);
		wlr_scene_node_set_position(&view->scene_tree->node, new_x, new_y);
		return;
	}
	struct resonance_toplevel *toplevel = server->grabbed_toplevel;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		server->cursor->x - server->grab_x,
		server->cursor->y - server->grab_y);
}

static void process_cursor_resize(struct resonance_server *server) {
	/*
	 * Resizing the grabbed toplevel can be a little bit complicated, because we
	 * could be resizing from any corner or edge. This not only resizes the
	 * toplevel on one or two axes, but can also move the toplevel if you resize
	 * from the top or left edges (or top-left corner).
	 *
	 * Note that some shortcuts are taken here. In a more fleshed-out
	 * compositor, you'd wait for the client to prepare a buffer at the new
	 * size, then commit any movement that was prepared.
	 */
	struct resonance_toplevel *toplevel = server->grabbed_toplevel;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	int new_width = new_right - new_left;
	int new_height = new_bottom - new_top;

	resonance_push_apply(server);

	if (server->grabbed_xwayland_view) {
		struct resonance_xwayland_view *view = server->grabbed_xwayland_view;
		wlr_scene_node_set_position(&view->scene_tree->node, new_left, new_top);
		/* new_left/new_top are parent-relative (see reset_cursor_mode) - convert
		 * to absolute before telling the X11 surface where it is. */
		int abs_x = new_left, abs_y = new_top;
		if (view->scene_tree->node.parent) {
			int px = 0, py = 0;
			wlr_scene_node_coords(&view->scene_tree->node.parent->node, &px, &py);
			abs_x += px;
			abs_y += py;
		}
		wlr_xwayland_surface_configure(view->xwayland_surface,
			abs_x, abs_y, new_width, new_height);
		return;
	}

	struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
	toplevel->pending_x = new_left - geo_box->x;
	toplevel->pending_y = new_top - geo_box->y;
	toplevel->pending_w = new_width;
	toplevel->pending_h = new_height;
	toplevel->resize_dirty = true;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	if (now - toplevel->last_configure_time >= 16) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
		toplevel->last_configure_time = now;
	}
}

static void schedule_frames(struct resonance_server *server) {
	struct resonance_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output->enabled) {
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
}

static void set_buffer_opacity(struct wlr_scene_buffer *buffer, int sx, int sy, void *user_data) {
	float *opacity = user_data;
	wlr_scene_buffer_set_opacity(buffer, *opacity);
}

static void set_view_opacity(struct resonance_xwayland_view *view, float opacity) {
	if (view->scene_tree) {
		wlr_scene_node_for_each_buffer(&view->scene_tree->node,
			set_buffer_opacity, &opacity);
	}
}



static const char *get_toplevel_app_id(struct resonance_toplevel *toplevel) {
	if (!toplevel) return NULL;
	if (toplevel->is_xwayland) {
		struct resonance_xwayland_view *view = (struct resonance_xwayland_view *)toplevel;
		if (view->xwayland_surface && view->xwayland_surface->class) {
			return view->xwayland_surface->class;
		}
	} else {
		if (toplevel->xdg_toplevel && toplevel->xdg_toplevel->app_id) {
			return toplevel->xdg_toplevel->app_id;
		}
	}
	return NULL;
}

/* Super+flick gesture recognition lives in gestures.c. */
/* If there is an active focused override-redirect XWayland window, hold
 * pointer focus on it and forward motion events to it.
 * Returns true if the caller should return early (handled). */
static bool process_cursor_override_redirect_focus(struct resonance_server *server, uint32_t time) {
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *focused_surf = seat->pointer_state.focused_surface;
	if (!focused_surf) {
		return false;
	}
	struct wlr_surface *root_surf = wlr_surface_get_root_surface(focused_surf);
	struct resonance_xwayland_view *v;
	wl_list_for_each(v, &server->xwayland_views, link) {
		if (v->xwayland_surface->override_redirect && v->xwayland_surface->surface == root_surf) {
			double sx = server->cursor->x - v->xwayland_surface->x;
			double sy = server->cursor->y - v->xwayland_surface->y;
			int w = v->xwayland_surface->width;
			int h = v->xwayland_surface->height;
			if (sx >= 0 && sx < w && sy >= 0 && sy < h) {
				wlr_seat_pointer_notify_motion(seat, time, sx, sy);
				return true;
			}
		}
	}
	return false;
}

/* Hover-based IPC events (hover-enter / hover-leave by app_id) and
 * focus-follows-hover keyboard activation, with layer-shell
 * keyboard-interactive exceptions. */
static void process_cursor_hover_focus(struct resonance_server *server,
		struct resonance_toplevel *toplevel, bool sync_active) {
	struct wlr_seat *seat = server->seat;

	if (toplevel != server->hovered_toplevel) {
		const char *old_app_id = get_toplevel_app_id(server->hovered_toplevel);
		const char *new_app_id = get_toplevel_app_id(toplevel);

		if (old_app_id && (!new_app_id || strcmp(old_app_id, new_app_id) != 0)) {
			char buf[320];
			snprintf(buf, sizeof(buf), "hover-leave:%s\n", old_app_id);
			ipc_send(server, buf);
		}
		if (new_app_id && (!old_app_id || strcmp(old_app_id, new_app_id) != 0)) {
			char buf[320];
			snprintf(buf, sizeof(buf), "hover-enter:%s\n", new_app_id);
			ipc_send(server, buf);
		}
		server->hovered_toplevel = toplevel;
	}

	if (!toplevel) {
		/* If there's no toplevel under the cursor, set the cursor image to a
		 * default. This is what makes the cursor image appear when you move it
		 * around the screen, not over any toplevels. */
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");

		/* Focus dismiss layer / logic:
		 * If the mouse leaves all windows (goes to wallpaper), deactivate the focused surface
		 * and clear keyboard focus. Do not clear focus if the focused surface is an interactive layer shell surface (like Tide). */
		struct wlr_surface *focused = seat->keyboard_state.focused_surface;
		if (focused && !sync_active) {
			struct wlr_layer_surface_v1 *layer_surface =
				wlr_layer_surface_v1_try_from_wlr_surface(focused);
			if (layer_surface && layer_surface->current.keyboard_interactive !=
					ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
				/* Keep focus on the interactive layer shell surface */
			} else {
				deactivate_surface(focused);
				wlr_seat_keyboard_notify_clear_focus(seat);
			}
		}
	}
	if (toplevel) {
		/* Give keyboard focus on hover without raising the window. */
		struct wlr_surface *focused = seat->keyboard_state.focused_surface;
		struct wlr_surface *focused_root = focused ? wlr_surface_get_root_surface(focused) : NULL;
		struct wlr_surface *hovered = toplevel->is_xwayland ?
			((struct resonance_xwayland_view *)toplevel)->xwayland_surface->surface :
			toplevel->xdg_toplevel->base->surface;

		bool is_override_redirect = toplevel->is_xwayland &&
			((struct resonance_xwayland_view *)toplevel)->xwayland_surface->override_redirect;

		bool keep_layer_focus = false;
		if (focused) {
			struct wlr_layer_surface_v1 *layer_surface =
				wlr_layer_surface_v1_try_from_wlr_surface(focused);
			if (layer_surface && layer_surface->current.keyboard_interactive !=
					ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
				keep_layer_focus = true;
			}
		}

		if (hovered && focused_root != hovered && !is_override_redirect && !sync_active && !keep_layer_focus) {
			deactivate_surface(focused);

			if (toplevel->is_xwayland) {
				wlr_xwayland_surface_activate(
					((struct resonance_xwayland_view *)toplevel)->xwayland_surface, true);
			} else {
				wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
			}

			struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
			if (keyboard) {
				wlr_seat_keyboard_notify_enter(seat, hovered,
					keyboard->keycodes, keyboard->num_keycodes,
					&keyboard->modifiers);
			}
		}
	}
}

/* Track which output the cursor is on; when it crosses to a new output,
 * write the output's center point to /tmp/resonance-active-output. */
static void process_cursor_track_active_output(struct resonance_server *server) {
	struct wlr_output *now = wlr_output_layout_output_at(
			server->output_layout, server->cursor->x, server->cursor->y);
	if (now && now != server->cursor_output) {
		server->cursor_output = now;
		FILE *f = fopen("/tmp/resonance-active-output.tmp", "w");
		if (f) {
			/* Write a point safely inside the resolved output's box (its
			 * center), not the raw cursor position. Rounding the raw
			 * cursor coordinate with %.0f can push a value that wlroots
			 * just resolved to this output exactly onto the seam with
			 * the next output, where a consumer re-deriving the monitor
			 * from the coordinate (e.g. GTK's get_monitor_at_point) may
			 * disagree about which side of the boundary it's on. */
			struct wlr_box box;
			wlr_output_layout_get_box(server->output_layout, now, &box);
			fprintf(f, "%.0f %.0f", box.x + box.width / 2.0, box.y + box.height / 2.0);
			fclose(f);
			rename("/tmp/resonance-active-output.tmp", "/tmp/resonance-active-output");
		}
	}
}

static void process_cursor_motion(struct resonance_server *server, uint32_t time) {
	if (server->drag_icon_tree != NULL) {
		update_drag_icon_position(server);
	}

	/* Global cursor position feed - Wayland deliberately doesn't let
	 * arbitrary clients query this (unlike X11), so this is our own
	 * general-purpose escape hatch over the resonance IPC socket, in the
	 * same spirit as Hyprland's `hyprctl cursorpos`. Throttled to ~30ms
	 * so it doesn't flood the socket on every raw motion event. */
	if (time - server->last_cursor_broadcast_ms >= 30) {
		server->last_cursor_broadcast_ms = time;
		char buf[64];
		snprintf(buf, sizeof(buf), "cursor-pos:%d\t%d\n",
			(int)server->cursor->x, (int)server->cursor->y);
		ipc_send(server, buf);
	}

	/* If the mode is non-passthrough, delegate to those functions. */
	if (server->cursor_mode == RESONANCE_CURSOR_MOVE) {
		process_cursor_move(server);
		return;
	} else if (server->cursor_mode == RESONANCE_CURSOR_RESIZE) {
		process_cursor_resize(server);
		return;
	}

	gesture_handle_motion(server, time);

	if (process_cursor_override_redirect_focus(server, time)) {
		return;
	}

	/* Find the toplevel under the pointer and send the event along. */
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct resonance_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);

	bool sync_active = false;
	struct resonance_xwayland_view *sync_view;
	wl_list_for_each(sync_view, &server->xwayland_views, link) {
		if (sync_view->hidden_for_sync) {
			sync_active = true;
			break;
		}
	}

	process_cursor_hover_focus(server, toplevel, sync_active);

	if (surface) {
		/*
		 * Send pointer enter and motion events.
		 *
		 * The enter event gives the surface "pointer focus", which is distinct
		 * from keyboard focus. You get pointer focus by moving the pointer over
		 * a window.
		 *
		 * Note that wlroots will avoid sending duplicate enter/motion events if
		 * the surface has already has pointer focus or if the client is already
		 * aware of the coordinates passed.
		 */
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
	} else {
		/* Clear pointer focus so future button events and such are not sent to
		 * the last client to have the cursor over it. */
		wlr_seat_pointer_clear_focus(server->seat);
	}

	process_cursor_track_active_output(server);
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct resonance_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;

	if (server->active_constraint &&
			server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
		wlr_relative_pointer_manager_v1_send_relative_motion(
			server->relative_pointer_manager, server->seat,
			(uint64_t)event->time_msec * 1000,
			event->delta_x, event->delta_y,
			event->unaccel_dx, event->unaccel_dy);
		process_cursor_motion(server, event->time_msec);
		schedule_frames(server);
		return;
	}

	double old_x = server->cursor->x;
	double old_y = server->cursor->y;
	wlr_cursor_move(server->cursor, &event->pointer->base,
			event->delta_x, event->delta_y);

	if (server->active_constraint &&
			server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_CONFINED) {
		double sx, sy;
		struct wlr_surface *surface = NULL;
		struct resonance_toplevel *tl = desktop_toplevel_at(server,
				server->cursor->x, server->cursor->y, &surface, &sx, &sy);
		bool over_override_redirect = tl && tl->is_xwayland &&
				((struct resonance_xwayland_view *)tl)->xwayland_surface->override_redirect;
		if (!over_override_redirect && surface != server->active_constraint->surface) {
			wlr_cursor_warp(server->cursor, NULL, old_x, old_y);
		}
	}

	process_cursor_motion(server, event->time_msec);
	schedule_frames(server);
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	struct resonance_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	if (server->active_constraint &&
			server->active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
		process_cursor_motion(server, event->time_msec);
		schedule_frames(server);
		return;
	}
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x,
		event->y);
	process_cursor_motion(server, event->time_msec);
	schedule_frames(server);
}

static void begin_interactive(struct resonance_toplevel *toplevel,
		enum resonance_cursor_mode mode, uint32_t edges);
static void begin_interactive_xwayland(struct resonance_xwayland_view *view,
		enum resonance_cursor_mode mode, uint32_t edges);

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct resonance_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct resonance_toplevel *toplevel = desktop_toplevel_at(server,
			server->cursor->x, server->cursor->y, &surface, &sx, &sy);

	/* Super+LMB: move window. Super+RMB: resize from nearest corner. */
	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED && toplevel) {
		struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
		uint32_t modifiers = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		if (modifiers & WLR_MODIFIER_LOGO) {
			wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
			wl_list_remove(&toplevel->link);
			if (toplevel->is_xwayland) {
				wl_list_insert(&server->xwayland_views, &toplevel->link);
			} else {
				wl_list_insert(&server->toplevels, &toplevel->link);
			}
			if (toplevel->is_xwayland) {
				struct resonance_xwayland_view *xw =
					(struct resonance_xwayland_view *)toplevel;
				if (event->button == BTN_LEFT) {
					begin_interactive_xwayland(xw, RESONANCE_CURSOR_MOVE, 0);
					return;
				} else if (event->button == BTN_RIGHT) {
					int cx = xw->scene_tree->node.x + xw->xwayland_surface->width / 2;
					int cy = xw->scene_tree->node.y + xw->xwayland_surface->height / 2;
					uint32_t edges =
						(server->cursor->x < cx ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
						(server->cursor->y < cy ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
					begin_interactive_xwayland(xw, RESONANCE_CURSOR_RESIZE, edges);
					return;
				}
			} else {
				if (event->button == BTN_LEFT) {
					begin_interactive(toplevel, RESONANCE_CURSOR_MOVE, 0);
					return;
				} else if (event->button == BTN_RIGHT) {
					struct wlr_box geo = toplevel->xdg_toplevel->base->geometry;
					int cx = toplevel->scene_tree->node.x + geo.x + geo.width / 2;
					int cy = toplevel->scene_tree->node.y + geo.y + geo.height / 2;
					uint32_t edges =
						(server->cursor->x < cx ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
						(server->cursor->y < cy ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
					begin_interactive(toplevel, RESONANCE_CURSOR_RESIZE, edges);
					return;
				}
			}
		}
	}

	wlr_seat_pointer_notify_button(server->seat,
			event->time_msec, event->button, event->state);
	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		reset_cursor_mode(server);
	} else {
		if (toplevel && toplevel->is_xwayland) {
			struct resonance_xwayland_view *xw_view = (struct resonance_xwayland_view *)toplevel;
			if (!xw_view->xwayland_surface->override_redirect) {
				if (xw_view->scene_tree) {
					wlr_scene_node_raise_to_top(&xw_view->scene_tree->node);
				}
				focus_xwayland_view(xw_view);
			}
		} else if (toplevel) {
			wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
			wl_list_remove(&toplevel->link);
			wl_list_insert(&server->toplevels, &toplevel->link);
			focus_toplevel(toplevel);
		}
		schedule_frames(server);
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct resonance_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	uint32_t modifiers = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;

	if ((modifiers & WLR_MODIFIER_LOGO) &&
			event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL) {
		/* Super+Scroll: cycle focus to next/prev output and release any pointer constraint */
		if (server->active_constraint) {
			activate_constraint(server, NULL);
		}

		/* Find current output, pick next or previous in list */
		struct resonance_output *current = NULL;
		struct resonance_output *output;
		wl_list_for_each(output, &server->outputs, link) {
			if (output->wlr_output == server->cursor_output) {
				current = output;
				break;
			}
		}

		struct resonance_output *target = NULL;
		if (current) {
			if (event->delta > 0) {
				/* scroll down → next output */
				struct wl_list *next = current->link.next;
				if (next == &server->outputs) {
					next = server->outputs.next;
				}
				target = wl_container_of(next, target, link);
			} else {
				/* scroll up → previous output */
				struct wl_list *prev = current->link.prev;
				if (prev == &server->outputs) {
					prev = server->outputs.prev;
				}
				target = wl_container_of(prev, target, link);
			}
		}

		if (target && target != current) {
			struct wlr_box box;
			wlr_output_layout_get_box(server->output_layout, target->wlr_output, &box);
			double cx = box.x + box.width / 2.0;
			double cy = box.y + box.height / 2.0;
			wlr_cursor_warp(server->cursor, NULL, cx, cy);
			process_cursor_motion(server, event->time_msec);
			schedule_frames(server);
		}
		return;
	}

	wlr_seat_pointer_notify_axis(server->seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
	schedule_frames(server);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	/* This event is forwarded by the cursor when a pointer emits an frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	struct resonance_server *server =
		wl_container_of(listener, server, cursor_frame);
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(server->seat);
}

static void handle_session_active(struct wl_listener *listener, void *data) {
	struct resonance_server *server =
		wl_container_of(listener, server, session_active);
	if (server->session->active) {
		server->backend_failed = false;
		struct resonance_output *output;
		wl_list_for_each(output, &server->outputs, link) {
			wlr_output_schedule_frame(output->wlr_output);
		}
	}
}

static void output_frame(struct wl_listener *listener, void *data) {
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	struct resonance_output *output = wl_container_of(listener, output, frame);
	struct wlr_scene *scene = output->server->scene;

	if (!output->wlr_output->enabled) {
		return;
	}
	if (output->server->session && !output->server->session->active) {
		return;
	}

	struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(
		scene, output->wlr_output);
	if (!scene_output) {
		return;
	}

	/* backend_failed gates layer-shell surface commits (see
	 * layer_surface_commit) so a genuinely dead backend doesn't spin.
	 * Reset it optimistically every frame and only re-latch it if this
	 * frame's own commit fails -- a single transient DRM error (an
	 * atomic commit racing a monitor's link flapping, say) must not
	 * permanently block backgrounds/panels for the rest of the session. */
	output->server->backend_failed = false;

	bool needs_frame = wlr_scene_output_needs_frame(scene_output);
	if (needs_frame) {
		/* Render the scene if needed and commit the output */
		if (!wlr_scene_output_commit(scene_output, NULL)) {
			output->server->backend_failed = true;
			return;
		}
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);

	struct resonance_server *srv = output->server;
	if (bootintro_frame_tick(&srv->boot, now)) {
		schedule_frames(srv);
	}
}



static void output_request_state(struct wl_listener *listener, void *data) {
	/* This function is called when the backend requests a new state for
	 * the output. For example, Wayland and X11 backends request a new mode
	 * when the output window is resized. */
	struct resonance_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct resonance_output *output = wl_container_of(listener, output, destroy);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

/* ---- adaptive sync vs. screen capture ----
 * ext-image-copy-capture forces compositing (disables direct scanout) so
 * there's a buffer to blit from. With adaptive sync on, the compositor's
 * slightly irregular frame timing makes the monitor's refresh rate wobble,
 * which reads as stutter. Disable VRR while any capture session is active
 * on an output; re-enable when the last session ends. */

static void set_adaptive_sync(struct wlr_output *wlr_output, bool enabled) {
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_adaptive_sync_enabled(&state, enabled);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
}

static void capture_session_destroy(struct wl_listener *listener, void *data) {
	struct capture_session_tracker *tracker =
		wl_container_of(listener, tracker, destroy);
	struct resonance_output *output = tracker->output;
	output->capture_count--;
	if (output->capture_count == 0 &&
			output->wlr_output->adaptive_sync_supported) {
		set_adaptive_sync(output->wlr_output, true);
		wlr_log(WLR_INFO, "capture ended on %s: re-enabled adaptive sync",
			output->wlr_output->name);
	}
	wl_list_remove(&tracker->destroy.link);
	free(tracker);
}

static void new_capture_session(struct wl_listener *listener, void *data) {
	struct resonance_server *server =
		wl_container_of(listener, server, new_capture_session);
	struct wlr_ext_image_copy_capture_session_v1 *session = data;

	struct wlr_output *wlr_output =
		wlr_output_try_from_ext_image_capture_source_v1(session->source);
	if (!wlr_output)
		return;

	struct resonance_output *output = NULL;
	struct resonance_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output == wlr_output) {
			output = o;
			break;
		}
	}
	if (!output)
		return;

	output->capture_count++;
	if (output->capture_count == 1 &&
			wlr_output->adaptive_sync_supported) {
		set_adaptive_sync(wlr_output, false);
		wlr_log(WLR_INFO, "capture started on %s: disabled adaptive sync",
			wlr_output->name);
	}

	struct capture_session_tracker *tracker = calloc(1, sizeof(*tracker));
	tracker->output = output;
	tracker->destroy.notify = capture_session_destroy;
	wl_signal_add(&session->events.destroy, &tracker->destroy);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct resonance_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* Configures the output created by the backend to use our allocator
	 * and our renderer. Must be done once, before committing the output */
	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	/* The output may be disabled, switch it on. */
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_output_mode *mode = NULL;
	int layout_x = 0, layout_y = 0;

	if (strcmp(wlr_output->name, "DP-1") == 0) {
		layout_x = 0;
		layout_y = 0;
		struct wlr_output_mode *m;
		wl_list_for_each(m, &wlr_output->modes, link) {
			if (m->width == 2560 && m->height == 1440) {
				if (mode == NULL || m->refresh > mode->refresh) {
					mode = m;
				}
			}
		}
	} else if (strcmp(wlr_output->name, "DP-2") == 0) {
		layout_x = 2560;
		layout_y = 180;
		struct wlr_output_mode *m;
		wl_list_for_each(m, &wlr_output->modes, link) {
			if (m->width == 1920 && m->height == 1080) {
				if (mode == NULL || m->refresh > mode->refresh) {
					mode = m;
				}
			}
		}
	}

	if (mode == NULL) {
		mode = wlr_output_preferred_mode(wlr_output);
	}

	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}

	/* Ask for a variable refresh rate (FreeSync) when the display supports
	 * it, so frames are shown as they finish instead of waiting for the next
	 * fixed vblank. */
	bool want_adaptive_sync = wlr_output->adaptive_sync_supported;
	if (want_adaptive_sync) {
		wlr_output_state_set_adaptive_sync_enabled(&state, true);
	}

	/* Atomically applies the new output state. Adaptive sync can turn an
	 * otherwise valid commit into a failing one, so fall back to a fixed
	 * refresh rate rather than leaving the output unconfigured. */
	if (!wlr_output_commit_state(wlr_output, &state) && want_adaptive_sync) {
		wlr_log(WLR_INFO, "%s: commit with adaptive sync failed, "
			"retrying without it", wlr_output->name);
		wlr_output_state_set_adaptive_sync_enabled(&state, false);
		wlr_output_commit_state(wlr_output, &state);
	}
	wlr_output_state_finish(&state);

	wlr_log(WLR_INFO, "%s: %dx%d@%.3fHz, adaptive sync %s (hardware: %s)",
		wlr_output->name,
		mode ? mode->width : 0, mode ? mode->height : 0,
		mode ? mode->refresh / 1000.0 : 0.0,
		wlr_output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED
			? "on" : "off",
		wlr_output->adaptive_sync_supported ? "supported" : "unsupported");

	/* Allocates and configures our state for this output */
	struct resonance_output *output = calloc(1, sizeof(*output));
	if (!output) {
		wlr_log(WLR_ERROR, "Failed to allocate output structure!");
		return;
	}
	output->wlr_output = wlr_output;
	output->server = server;

	/* Sets up a listener for the frame event. */
	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	/* Sets up a listener for the state request event. */
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);

	/* Sets up a listener for the destroy event. */
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	/* Adds this to the output layout. The add_auto function arranges outputs
	 * from left-to-right in the order they appear. A more sophisticated
	 * compositor would let the user configure the arrangement of outputs in the
	 * layout.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	struct wlr_output_layout_output *l_output = wlr_output_layout_add(
		server->output_layout, wlr_output, layout_x, layout_y);
	struct wlr_scene_output *scene_output = wlr_scene_output_create(server->scene, wlr_output);
	if (!l_output || !scene_output) {
		wlr_log(WLR_ERROR, "Failed to add output to layout/scene");
		return;
	}
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);

	bootintro_handle_new_output(&server->boot, server->layer_trees[3],
		server->output_layout, wlr_output);
}

static struct resonance_output *resonance_server_get_primary_output(struct resonance_server *server);

/* Finds the output box under (x, y) — used to fullscreen a window onto
 * whichever monitor it currently sits on, rather than a fixed output.
 * Falls back to the primary output if the point isn't over any output. */
static bool resonance_output_box_at(struct resonance_server *server, int x, int y, struct wlr_box *box) {
	struct wlr_output *output = wlr_output_layout_output_at(server->output_layout, x, y);
	if (!output) {
		struct resonance_output *out = resonance_server_get_primary_output(server);
		if (!out) {
			return false;
		}
		output = out->wlr_output;
	}
	wlr_output_layout_get_box(server->output_layout, output, box);
	return true;
}

/* Push-resize: dragging an edge shoves the one nearest same-floor window
 * in that direction, rather than letting windows overlap or requiring them
 * to be manually shuffled apart. The neighbor is picked once at grab time
 * (whichever same-floor window is nearest in the drag direction, no matter
 * how far away), but whether it's actually moving is re-evaluated live on
 * every step of the drag: it stays put until the dragged edge comes within
 * RESONANCE_PUSH_GAP_PX of it, then tracks the drag to keep that gap, and
 * retracts back toward its original spot if the drag backs off. See the
 * push_h_ and push_v_ fields on resonance_server for the state this
 * maintains across a single interactive resize. */

/* Resting gap the push mechanic keeps between the two windows' drawn
 * frames (border included), once contact is made. This is a frame-to-frame
 * measurement, not a content measurement - no per-app padding/shadow
 * knowledge involved anywhere in push. This is NOT an activation
 * threshold: the neighbor is picked at grab time regardless of how far
 * away it starts, and starts moving the moment the dragged frame comes
 * within this gap of it, however far into the drag that happens. */
#define RESONANCE_PUSH_GAP_PX 7
/* Never push/shrink a neighbor smaller than this on the pushed axis. */
#define RESONANCE_PUSH_MIN_SIZE_PX 100

/* Mirrors core/placement.py's FLOOR_INSETS - kept in sync manually, same
 * pattern as the OUTPUTS layout comment near ipc_handle_place_command.
 * Used only to know how far a pushed neighbor is allowed to travel before
 * it must start shrinking instead of translating further. */
static const int resonance_floor_inset_px[RESONANCE_FLOOR_COUNT] = {
	[RESONANCE_FLOOR_BASEMENT] = 0,
	[RESONANCE_FLOOR_GROUND] = 30,
	[RESONANCE_FLOOR_FIRST] = 60,
	[RESONANCE_FLOOR_SECOND] = 90,
};

/* Frame box: the window's drawn border, exactly as update_window_border
 * renders it - position minus BORDER_WIDTH, size plus 2*BORDER_WIDTH.
 * Nothing here knows or cares about any app's declared geometry offset;
 * this is the same simple formula the border itself uses, so push always
 * agrees with what's actually on screen. */
static void resonance_toplevel_absolute_box(struct resonance_toplevel *t, struct wlr_box *box) {
	struct wlr_box *geo = &t->xdg_toplevel->base->geometry;
	int w = t->last_w > 0 ? t->last_w : geo->width;
	int h = t->last_h > 0 ? t->last_h : geo->height;
	box->x = t->scene_tree->node.x - BORDER_WIDTH;
	box->y = t->scene_tree->node.y - BORDER_WIDTH;
	box->width = w + 2 * BORDER_WIDTH;
	box->height = h + 2 * BORDER_WIDTH;
}

static void resonance_xwayland_view_absolute_box(struct resonance_xwayland_view *v, struct wlr_box *box) {
	box->x = v->scene_tree->node.x - BORDER_WIDTH;
	box->y = v->scene_tree->node.y - BORDER_WIDTH;
	box->width = v->xwayland_surface->width + 2 * BORDER_WIDTH;
	box->height = v->xwayland_surface->height + 2 * BORDER_WIDTH;
}

/* Inverse of the frame box above: given a desired frame box, place the
 * window so its frame lands exactly there. */
static void resonance_set_toplevel_absolute_box(struct resonance_toplevel *t, int x, int y, int w, int h) {
	wlr_scene_node_set_position(&t->scene_tree->node, x + BORDER_WIDTH, y + BORDER_WIDTH);
	wlr_xdg_toplevel_set_size(t->xdg_toplevel, w - 2 * BORDER_WIDTH, h - 2 * BORDER_WIDTH);
}

static void resonance_set_xwayland_view_absolute_box(struct resonance_xwayland_view *v, int x, int y, int w, int h) {
	wlr_scene_node_set_position(&v->scene_tree->node, x + BORDER_WIDTH, y + BORDER_WIDTH);
	wlr_xwayland_surface_configure(v->xwayland_surface, x + BORDER_WIDTH, y + BORDER_WIDTH,
		w - 2 * BORDER_WIDTH, h - 2 * BORDER_WIDTH);
}

/* Distance (in the push direction) from the dragged edge to this box's
 * facing edge, or INT_MIN if the box doesn't overlap on the perpendicular
 * axis (i.e. it's not actually alongside the edge being dragged, just
 * coincidentally at a similar coordinate elsewhere) or sits behind the
 * dragged window's own trailing edge (own_far_edge - the edge on the
 * OPPOSITE side from the one being dragged, which doesn't move during this
 * resize) rather than ahead of it in the push direction. A candidate
 * behind that line is on the wrong side entirely, not a push target, no
 * matter how small its raw distance to edge_pos comes out. Smaller
 * (more negative, including already-overlapping) distances among
 * qualifying candidates are closer. There's no upper bound - a neighbor
 * far away is still "the" neighbor for this drag, it just won't start
 * moving until the dragged edge reaches it. See resonance_push_apply_axis for
 * where that distance actually gates movement. */
static int resonance_push_box_distance(struct wlr_box *box, bool horizontal, uint32_t edge,
		int edge_pos, int own_far_edge, int perp_lo, int perp_hi) {
	int lo = horizontal ? box->y : box->x;
	int hi = horizontal ? box->y + box->height : box->x + box->width;
	if (lo >= perp_hi || hi <= perp_lo) {
		return INT_MIN;
	}
	bool dir_positive = (edge == WLR_EDGE_RIGHT || edge == WLR_EDGE_BOTTOM);
	int facing_edge = horizontal
		? (dir_positive ? box->x : box->x + box->width)
		: (dir_positive ? box->y : box->y + box->height);
	if (dir_positive ? (facing_edge <= own_far_edge) : (facing_edge >= own_far_edge)) {
		return INT_MIN;
	}
	return dir_positive ? (facing_edge - edge_pos) : (edge_pos - facing_edge);
}

/* Scans same-floor windows (excluding the grabbed one) for whichever one is
 * nearest in the push direction - deliberately only one, since push doesn't
 * chain. Nearest is picked regardless of how far away it is; whether it
 * actually starts moving this drag is decided later, live, in
 * resonance_push_apply_axis as the dragged edge approaches it. */
static void resonance_push_find_neighbor(struct resonance_server *server, enum resonance_floor floor,
		struct resonance_toplevel *exclude_toplevel, struct resonance_xwayland_view *exclude_view,
		bool horizontal, uint32_t edge, int edge_pos, int own_far_edge, int perp_lo, int perp_hi,
		struct resonance_toplevel **out_toplevel, struct resonance_xwayland_view **out_view,
		struct wlr_box *out_box) {
	*out_toplevel = NULL;
	*out_view = NULL;
	int best_dist = INT_MAX;

	struct resonance_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t == exclude_toplevel || t->floor != floor || t->fullscreen) {
			continue;
		}
		struct wlr_box box;
		resonance_toplevel_absolute_box(t, &box);
		int dist = resonance_push_box_distance(&box, horizontal, edge, edge_pos, own_far_edge, perp_lo, perp_hi);
		if (dist != INT_MIN && dist < best_dist) {
			best_dist = dist;
			*out_toplevel = t;
			*out_view = NULL;
			*out_box = box;
		}
	}
	struct resonance_xwayland_view *v;
	wl_list_for_each(v, &server->xwayland_views, link) {
		if (v == exclude_view || v->floor != floor || v->xwayland_surface->override_redirect || v->fullscreen) {
			continue;
		}
		struct wlr_box box;
		resonance_xwayland_view_absolute_box(v, &box);
		int dist = resonance_push_box_distance(&box, horizontal, edge, edge_pos, own_far_edge, perp_lo, perp_hi);
		if (dist != INT_MIN && dist < best_dist) {
			best_dist = dist;
			*out_toplevel = NULL;
			*out_view = v;
			*out_box = box;
		}
	}
}

static void resonance_push_begin(struct resonance_server *server, enum resonance_floor floor,
		struct resonance_toplevel *exclude_toplevel, struct resonance_xwayland_view *exclude_view,
		uint32_t edges) {
	server->push_h_toplevel = NULL;
	server->push_h_view = NULL;
	server->push_v_toplevel = NULL;
	server->push_v_view = NULL;

	/* grabbed_box is the DRAGGED window's own current frame box - read the
	 * same way as any other window's frame box, no special-casing. */
	struct wlr_box grabbed_box;
	if (exclude_toplevel) {
		resonance_toplevel_absolute_box(exclude_toplevel, &grabbed_box);
	} else {
		resonance_xwayland_view_absolute_box(exclude_view, &grabbed_box);
	}

	if (edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
		uint32_t edge = (edges & WLR_EDGE_RIGHT) ? WLR_EDGE_RIGHT : WLR_EDGE_LEFT;
		int edge_pos = (edge == WLR_EDGE_RIGHT) ? grabbed_box.x + grabbed_box.width : grabbed_box.x;
		int own_far_edge = (edge == WLR_EDGE_RIGHT) ? grabbed_box.x : grabbed_box.x + grabbed_box.width;
		server->push_h_edge = edge;
		resonance_push_find_neighbor(server, floor, exclude_toplevel, exclude_view,
			true, edge, edge_pos, own_far_edge, grabbed_box.y, grabbed_box.y + grabbed_box.height,
			&server->push_h_toplevel, &server->push_h_view, &server->push_h_box);
	}
	if (edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) {
		uint32_t edge = (edges & WLR_EDGE_BOTTOM) ? WLR_EDGE_BOTTOM : WLR_EDGE_TOP;
		int edge_pos = (edge == WLR_EDGE_BOTTOM) ? grabbed_box.y + grabbed_box.height : grabbed_box.y;
		int own_far_edge = (edge == WLR_EDGE_BOTTOM) ? grabbed_box.y : grabbed_box.y + grabbed_box.height;
		server->push_v_edge = edge;
		resonance_push_find_neighbor(server, floor, exclude_toplevel, exclude_view,
			false, edge, edge_pos, own_far_edge, grabbed_box.x, grabbed_box.x + grabbed_box.width,
			&server->push_v_toplevel, &server->push_v_view, &server->push_v_box);
	}
}

/* Moves/shrinks one pushed neighbor along one axis. This is re-run on every
 * step of the drag and is live in one direction only: the neighbor's near
 * edge (the one facing the dragged edge) stays put until the dragged edge
 * comes within RESONANCE_PUSH_GAP_PX of it - however far into the drag that
 * takes - then tracks the dragged edge to hold that gap. It does NOT slide
 * back if the drag retreats afterward; once pushed, a neighbor stays
 * pushed for the rest of this drag (no magnetism/snap-back yet - may add
 * later). This works because start_box is the server's persistent
 * push_h_box/push_v_box, which this function updates in place at the end,
 * so "start" on the next call means "wherever it currently is," not
 * "where it was at grab time." The far edge trails along with whatever
 * shift the near edge underwent, unless doing so would cross the floor's
 * padded bound, in which case the far edge holds there and the window
 * shrinks instead of continuing to translate. dir is +1 if the neighbor
 * sits on the increasing-coordinate side of the dragged edge (push edge
 * RIGHT/BOTTOM), -1 otherwise. dragged_edge_pos is the dragged edge's
 * current absolute position. */
static void resonance_push_apply_axis(struct resonance_server *server,
		struct resonance_toplevel *t, struct resonance_xwayland_view *v,
		struct wlr_box *start_box, bool horizontal, uint32_t edge, int dragged_edge_pos) {
	if (!t && !v) {
		return;
	}
	int dir = (edge == WLR_EDGE_RIGHT || edge == WLR_EDGE_BOTTOM) ? 1 : -1;

	int start_near = horizontal
		? (dir == 1 ? start_box->x : start_box->x + start_box->width)
		: (dir == 1 ? start_box->y : start_box->y + start_box->height);
	int start_far = horizontal
		? (dir == 1 ? start_box->x + start_box->width : start_box->x)
		: (dir == 1 ? start_box->y + start_box->height : start_box->y);

	int contact = dragged_edge_pos + (dir == 1 ? RESONANCE_PUSH_GAP_PX : -RESONANCE_PUSH_GAP_PX);
	int near_edge = dir == 1 ? (start_near > contact ? start_near : contact)
	                         : (start_near < contact ? start_near : contact);
	int shift = near_edge - start_near;
	int far_edge = start_far + shift;

	int cx = horizontal ? near_edge : (start_box->x + start_box->width / 2);
	int cy = horizontal ? (start_box->y + start_box->height / 2) : near_edge;
	struct wlr_box output_box;
	if (resonance_output_box_at(server, cx, cy, &output_box)) {
		enum resonance_floor floor = t ? t->floor : v->floor;
		int inset = resonance_floor_inset_px[floor];
		int bound_min = horizontal ? output_box.x + inset : output_box.y + inset;
		int bound_max = horizontal
			? output_box.x + output_box.width - inset
			: output_box.y + output_box.height - inset;
		if (dir == 1 && far_edge > bound_max) {
			far_edge = bound_max;
		} else if (dir == -1 && far_edge < bound_min) {
			far_edge = bound_min;
		}
	}

	int size = dir == 1 ? far_edge - near_edge : near_edge - far_edge;
	if (size < RESONANCE_PUSH_MIN_SIZE_PX) {
		size = RESONANCE_PUSH_MIN_SIZE_PX;
		far_edge = dir == 1 ? near_edge + size : near_edge - size;
	}

	int new_x = horizontal ? (dir == 1 ? near_edge : far_edge) : start_box->x;
	int new_y = horizontal ? start_box->y : (dir == 1 ? near_edge : far_edge);
	int new_w = horizontal ? size : start_box->width;
	int new_h = horizontal ? start_box->height : size;

	/* A pushed window no longer fills its floor slot, so it stops being
	 * floor-maximized the moment it's moved or shrunk. The flag is cleared
	 * directly rather than through set_floor_maximized(false), which would
	 * restore the pre-maximize geometry and fight the push mid-drag. */
	if (t) {
		t->floor_maximized = false;
		t->maxed_w = 0;
		t->maxed_h = 0;
		resonance_set_toplevel_absolute_box(t, new_x, new_y, new_w, new_h);
	} else {
		v->floor_maximized = false;
		v->maxed_w = 0;
		v->maxed_h = 0;
		resonance_set_xwayland_view_absolute_box(v, new_x, new_y, new_w, new_h);
	}

	/* Persist as the new "start" so a later retreat of the dragged edge
	 * can't pull this neighbor back past where it's already been pushed
	 * to - see the no-snap-back note above. */
	start_box->x = new_x;
	start_box->y = new_y;
	start_box->width = new_w;
	start_box->height = new_h;
}

static void get_floor_maximized_box(struct resonance_server *server,
		struct resonance_toplevel *toplevel_self,
		struct resonance_xwayland_view *view_self,
		enum resonance_floor floor,
		struct wlr_box *out_box) {
	struct wlr_box cur_box;
	if (toplevel_self) {
		if (toplevel_self->floor_maximized) {
			cur_box.x = toplevel_self->saved_x - BORDER_WIDTH;
			cur_box.y = toplevel_self->saved_y - BORDER_WIDTH;
			cur_box.width = toplevel_self->saved_w + 2 * BORDER_WIDTH;
			cur_box.height = toplevel_self->saved_h + 2 * BORDER_WIDTH;
		} else {
			resonance_toplevel_absolute_box(toplevel_self, &cur_box);
		}
	} else {
		if (view_self->floor_maximized) {
			cur_box.x = view_self->saved_x - BORDER_WIDTH;
			cur_box.y = view_self->saved_y - BORDER_WIDTH;
			cur_box.width = view_self->saved_w + 2 * BORDER_WIDTH;
			cur_box.height = view_self->saved_h + 2 * BORDER_WIDTH;
		} else {
			resonance_xwayland_view_absolute_box(view_self, &cur_box);
		}
	}

	struct wlr_box output_box;
	int cx = cur_box.x + cur_box.width / 2;
	int cy = cur_box.y + cur_box.height / 2;
	if (!resonance_output_box_at(server, cx, cy, &output_box)) {
		output_box.x = 0;
		output_box.y = 0;
		output_box.width = 1920;
		output_box.height = 1080;
	}

	int inset = resonance_floor_inset_px[floor];
	int left_limit = output_box.x + inset;
	int right_limit = output_box.x + output_box.width - inset;
	int top_limit = output_box.y + inset;
	int bottom_limit = output_box.y + output_box.height - inset;

	struct resonance_toplevel *t;
	wl_list_for_each(t, &server->toplevels, link) {
		if (t == toplevel_self || t->floor != floor || t->fullscreen) {
			continue;
		}
		struct wlr_box other;
		if (t->floor_maximized) {
			other.x = t->saved_x - BORDER_WIDTH;
			other.y = t->saved_y - BORDER_WIDTH;
			other.width = t->saved_w + 2 * BORDER_WIDTH;
			other.height = t->saved_h + 2 * BORDER_WIDTH;
		} else {
			resonance_toplevel_absolute_box(t, &other);
		}

		if (other.y < cur_box.y + cur_box.height && other.y + other.height > cur_box.y) {
			int other_cx = other.x + other.width / 2;
			int cur_cx = cur_box.x + cur_box.width / 2;
			if (other_cx <= cur_cx) {
				if (other.x + other.width + RESONANCE_PUSH_GAP_PX > left_limit) {
					left_limit = other.x + other.width + RESONANCE_PUSH_GAP_PX;
				}
			} else {
				if (other.x - RESONANCE_PUSH_GAP_PX < right_limit) {
					right_limit = other.x - RESONANCE_PUSH_GAP_PX;
				}
			}
		}
	}

	struct resonance_xwayland_view *v;
	wl_list_for_each(v, &server->xwayland_views, link) {
		if (v == view_self || v->floor != floor || v->xwayland_surface->override_redirect || v->fullscreen) {
			continue;
		}
		struct wlr_box other;
		if (v->floor_maximized) {
			other.x = v->saved_x - BORDER_WIDTH;
			other.y = v->saved_y - BORDER_WIDTH;
			other.width = v->saved_w + 2 * BORDER_WIDTH;
			other.height = v->saved_h + 2 * BORDER_WIDTH;
		} else {
			resonance_xwayland_view_absolute_box(v, &other);
		}

		if (other.y < cur_box.y + cur_box.height && other.y + other.height > cur_box.y) {
			int other_cx = other.x + other.width / 2;
			int cur_cx = cur_box.x + cur_box.width / 2;
			if (other_cx <= cur_cx) {
				if (other.x + other.width + RESONANCE_PUSH_GAP_PX > left_limit) {
					left_limit = other.x + other.width + RESONANCE_PUSH_GAP_PX;
				}
			} else {
				if (other.x - RESONANCE_PUSH_GAP_PX < right_limit) {
					right_limit = other.x - RESONANCE_PUSH_GAP_PX;
				}
			}
		}
	}

	wl_list_for_each(t, &server->toplevels, link) {
		if (t == toplevel_self || t->floor != floor || t->fullscreen) {
			continue;
		}
		struct wlr_box other;
		if (t->floor_maximized) {
			other.x = t->saved_x - BORDER_WIDTH;
			other.y = t->saved_y - BORDER_WIDTH;
			other.width = t->saved_w + 2 * BORDER_WIDTH;
			other.height = t->saved_h + 2 * BORDER_WIDTH;
		} else {
			resonance_toplevel_absolute_box(t, &other);
		}

		if (other.x < cur_box.x + cur_box.width && other.x + other.width > cur_box.x) {
			int other_cy = other.y + other.height / 2;
			int cur_cy = cur_box.y + cur_box.height / 2;
			if (other_cy <= cur_cy) {
				if (other.y + other.height + RESONANCE_PUSH_GAP_PX > top_limit) {
					top_limit = other.y + other.height + RESONANCE_PUSH_GAP_PX;
				}
			} else {
				if (other.y - RESONANCE_PUSH_GAP_PX < bottom_limit) {
					bottom_limit = other.y - RESONANCE_PUSH_GAP_PX;
				}
			}
		}
	}

	wl_list_for_each(v, &server->xwayland_views, link) {
		if (v == view_self || v->floor != floor || v->xwayland_surface->override_redirect || v->fullscreen) {
			continue;
		}
		struct wlr_box other;
		if (v->floor_maximized) {
			other.x = v->saved_x - BORDER_WIDTH;
			other.y = v->saved_y - BORDER_WIDTH;
			other.width = v->saved_w + 2 * BORDER_WIDTH;
			other.height = v->saved_h + 2 * BORDER_WIDTH;
		} else {
			resonance_xwayland_view_absolute_box(v, &other);
		}

		if (other.x < cur_box.x + cur_box.width && other.x + other.width > cur_box.x) {
			int other_cy = other.y + other.height / 2;
			int cur_cy = cur_box.y + cur_box.height / 2;
			if (other_cy <= cur_cy) {
				if (other.y + other.height + RESONANCE_PUSH_GAP_PX > top_limit) {
					top_limit = other.y + other.height + RESONANCE_PUSH_GAP_PX;
				}
			} else {
				if (other.y - RESONANCE_PUSH_GAP_PX < bottom_limit) {
					bottom_limit = other.y - RESONANCE_PUSH_GAP_PX;
				}
			}
		}
	}

	out_box->x = left_limit;
	out_box->y = top_limit;
	out_box->width = (right_limit > left_limit) ? (right_limit - left_limit) : 100;
	out_box->height = (bottom_limit > top_limit) ? (bottom_limit - top_limit) : 100;
}



static void resonance_toplevel_set_floor_maximized(struct resonance_toplevel *toplevel, bool maximized) {
	if (maximized == toplevel->floor_maximized) {
		return;
	}
	if (toplevel->fullscreen) {
		resonance_toplevel_set_fullscreen(toplevel, false);
	}

	if (maximized) {
		toplevel->saved_x = toplevel->scene_tree->node.x;
		toplevel->saved_y = toplevel->scene_tree->node.y;
		toplevel->saved_w = toplevel->last_w > 0 ? toplevel->last_w : toplevel->xdg_toplevel->base->geometry.width;
		toplevel->saved_h = toplevel->last_h > 0 ? toplevel->last_h : toplevel->xdg_toplevel->base->geometry.height;
		toplevel->floor_maximized = true;

		struct wlr_box box;
		get_floor_maximized_box(toplevel->server, toplevel, NULL, toplevel->floor, &box);

		toplevel->maxed_w = box.width;
		toplevel->maxed_h = box.height;
		toplevel->pending_x = box.x;
		toplevel->pending_y = box.y;
		toplevel->resize_dirty = true;
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, box.width, box.height);
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, true);
	} else {
		toplevel->floor_maximized = false;
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
		if (toplevel->saved_w > 0 && toplevel->saved_h > 0) {
			toplevel->pending_x = toplevel->saved_x;
			toplevel->pending_y = toplevel->saved_y;
			toplevel->resize_dirty = true;
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->saved_w, toplevel->saved_h);
		}
	}
}

static void resonance_xwayland_view_set_floor_maximized(struct resonance_xwayland_view *view, bool maximized) {
	if (maximized == view->floor_maximized) {
		return;
	}
	if (view->fullscreen) {
		resonance_xwayland_view_set_fullscreen(view, false);
	}

	if (maximized) {
		view->saved_x = view->scene_tree->node.x;
		view->saved_y = view->scene_tree->node.y;
		view->saved_w = view->last_w > 0 ? view->last_w : view->xwayland_surface->width;
		view->saved_h = view->last_h > 0 ? view->last_h : view->xwayland_surface->height;
		view->floor_maximized = true;

		struct wlr_box box;
		get_floor_maximized_box(view->server, NULL, view, view->floor, &box);

		view->maxed_w = box.width;
		view->maxed_h = box.height;
		wlr_scene_node_set_position(&view->scene_tree->node, box.x, box.y);
		wlr_xwayland_surface_configure(view->xwayland_surface, box.x, box.y, box.width, box.height);
		wlr_xwayland_surface_set_maximized(view->xwayland_surface, true, true);
	} else {
		view->floor_maximized = false;
		wlr_xwayland_surface_set_maximized(view->xwayland_surface, false, false);
		if (view->saved_w > 0 && view->saved_h > 0) {
			resonance_set_xwayland_view_absolute_box(view, view->saved_x - BORDER_WIDTH, view->saved_y - BORDER_WIDTH, view->saved_w + 2 * BORDER_WIDTH, view->saved_h + 2 * BORDER_WIDTH);
		}
	}
}

static void toggle_floor_maximized_focused(struct resonance_server *server) {
	struct wlr_surface *focused = server->seat->keyboard_state.focused_surface;
	if (!focused) {
		return;
	}
	struct wlr_surface *root = wlr_surface_get_root_surface(focused);

	struct wlr_xwayland_surface *xsurface = wlr_xwayland_surface_try_from_wlr_surface(root);
	if (xsurface) {
		while (xsurface->parent) {
			xsurface = xsurface->parent;
		}

		struct resonance_xwayland_view *xview;
		wl_list_for_each(xview, &server->xwayland_views, link) {
			if (xview->xwayland_surface != xsurface) continue;
			resonance_xwayland_view_set_floor_maximized(xview, !xview->floor_maximized);
			return;
		}
	}

	struct resonance_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel->xdg_toplevel->base->surface != root) continue;
		resonance_toplevel_set_floor_maximized(toplevel, !toplevel->floor_maximized);
		return;
	}
}

static void xdg_toplevel_apply_floor_maximized(struct resonance_toplevel *toplevel) {
	struct wlr_box box;
	get_floor_maximized_box(toplevel->server, toplevel, NULL, toplevel->floor, &box);

	toplevel->maxed_w = box.width;
	toplevel->maxed_h = box.height;
	if (toplevel->scene_tree) {
		wlr_scene_node_set_position(&toplevel->scene_tree->node, box.x, box.y);
	}
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, box.width, box.height);
}

/* Reads the dragged window's own CURRENT frame box - same function, same
 * rules as everything else - rather than predicting it from the mouse.
 * This means push reacts once the client actually commits the new size,
 * same as its border does, instead of chasing an intended-but-not-yet-
 * rendered position. No args needed: it just looks at whichever window is
 * currently grabbed. */
static void resonance_push_apply(struct resonance_server *server) {
	if (!server->push_h_toplevel && !server->push_h_view &&
			!server->push_v_toplevel && !server->push_v_view) {
		return;
	}
	struct wlr_box dragged_box;
	if (server->grabbed_toplevel) {
		resonance_toplevel_absolute_box(server->grabbed_toplevel, &dragged_box);
	} else if (server->grabbed_xwayland_view) {
		resonance_xwayland_view_absolute_box(server->grabbed_xwayland_view, &dragged_box);
	} else {
		return;
	}
	if (server->push_h_toplevel || server->push_h_view) {
		int dragged_edge_pos = (server->push_h_edge == WLR_EDGE_RIGHT)
			? dragged_box.x + dragged_box.width : dragged_box.x;
		resonance_push_apply_axis(server, server->push_h_toplevel, server->push_h_view,
			&server->push_h_box, true, server->push_h_edge, dragged_edge_pos);
	}
	if (server->push_v_toplevel || server->push_v_view) {
		int dragged_edge_pos = (server->push_v_edge == WLR_EDGE_BOTTOM)
			? dragged_box.y + dragged_box.height : dragged_box.y;
		resonance_push_apply_axis(server, server->push_v_toplevel, server->push_v_view,
			&server->push_v_box, false, server->push_v_edge, dragged_edge_pos);
	}
}

/* Reports the final settled position of any pushed neighbor(s) back to
 * core/placement.py (mirroring what reset_cursor_mode already does for the
 * grabbed window itself), so a push during this drag is remembered instead
 * of being silently forgotten and reverted next time that window reopens.
 * Then clears push state for the next interactive resize. */
static void resonance_push_release(struct resonance_server *server) {
	if (server->push_h_toplevel) {
		struct resonance_toplevel *t = server->push_h_toplevel;
		struct wlr_box box;
		resonance_toplevel_absolute_box(t, &box);
		ipc_send_window_geo(server, "geometry", t->xdg_toplevel->title, t->xdg_toplevel->app_id,
			box.x, box.y, box.width, box.height, t->id, t->fullscreen);
	} else if (server->push_h_view) {
		struct resonance_xwayland_view *v = server->push_h_view;
		struct wlr_box box;
		resonance_xwayland_view_absolute_box(v, &box);
		ipc_send_window_geo(server, "geometry", v->xwayland_surface->title, v->xwayland_surface->class,
			box.x, box.y, box.width, box.height, v->id, v->fullscreen);
	}
	if (server->push_v_toplevel) {
		struct resonance_toplevel *t = server->push_v_toplevel;
		struct wlr_box box;
		resonance_toplevel_absolute_box(t, &box);
		ipc_send_window_geo(server, "geometry", t->xdg_toplevel->title, t->xdg_toplevel->app_id,
			box.x, box.y, box.width, box.height, t->id, t->fullscreen);
	} else if (server->push_v_view) {
		struct resonance_xwayland_view *v = server->push_v_view;
		struct wlr_box box;
		resonance_xwayland_view_absolute_box(v, &box);
		ipc_send_window_geo(server, "geometry", v->xwayland_surface->title, v->xwayland_surface->class,
			box.x, box.y, box.width, box.height, v->id, v->fullscreen);
	}
	server->push_h_toplevel = NULL;
	server->push_h_view = NULL;
	server->push_v_toplevel = NULL;
	server->push_v_view = NULL;
}

static bool xdg_toplevel_fullscreen_box(struct resonance_toplevel *toplevel, struct wlr_box *box) {
	struct resonance_server *server = toplevel->server;
	int w = toplevel->last_w > 0 ? toplevel->last_w : toplevel->xdg_toplevel->base->geometry.width;
	int h = toplevel->last_h > 0 ? toplevel->last_h : toplevel->xdg_toplevel->base->geometry.height;
	int cx = toplevel->scene_tree->node.x + w / 2;
	int cy = toplevel->scene_tree->node.y + h / 2;
	return resonance_output_box_at(server, cx, cy, box);
}

static void xdg_toplevel_apply_fullscreen(struct resonance_toplevel *toplevel) {
	struct wlr_box box;
	if (!xdg_toplevel_fullscreen_box(toplevel, &box)) {
		return;
	}

	if (toplevel->scene_tree) {
		wlr_scene_node_set_position(&toplevel->scene_tree->node, box.x, box.y);
	}
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, box.width, box.height);
}

static void xdg_toplevel_sync_fullscreen_flag(struct resonance_toplevel *toplevel) {
	if (!toplevel->fullscreen && toplevel->xdg_toplevel->requested.fullscreen) {
		toplevel->saved_x = toplevel->scene_tree->node.x;
		toplevel->saved_y = toplevel->scene_tree->node.y;
		toplevel->saved_w = toplevel->xdg_toplevel->base->geometry.width;
		toplevel->saved_h = toplevel->xdg_toplevel->base->geometry.height;
		toplevel->fullscreen = true;
		wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, true);
	}
}

/* Centers a toplevel of size w×h on its parent (if it has one and the
 * parent is mapped), clamped to the parent's output; falls back to
 * centering on the cursor's output otherwise. Used both at map time and
 * from the commit handler, since some clients (e.g. GTK dialogs) map at
 * a placeholder size and commit their real size a moment later. */
static void center_toplevel(struct resonance_toplevel *toplevel, int w, int h) {
	struct resonance_server *server = toplevel->server;
	int x, y;
	bool centered_on_parent = false;

	if (toplevel->xdg_toplevel->parent) {
		struct resonance_toplevel *parent = NULL;
		struct resonance_toplevel *pt;
		wl_list_for_each(pt, &server->toplevels, link) {
			if (pt->xdg_toplevel == toplevel->xdg_toplevel->parent) {
				parent = pt;
				break;
			}
		}
		if (parent && parent->scene_tree) {
			int px = 0, py = 0;
			wlr_scene_node_coords(&parent->scene_tree->node, &px, &py);
			struct wlr_box parent_geo = parent->xdg_toplevel->base->geometry;
			struct wlr_surface *parent_surf = parent->xdg_toplevel->base->surface;
			int parent_w = parent_geo.width > 0 ? parent_geo.width : parent_surf->current.width;
			int parent_h = parent_geo.height > 0 ? parent_geo.height : parent_surf->current.height;

			x = px + (parent_w - w) / 2;
			y = py + (parent_h - h) / 2;

			/* Clamp to the parent's output so an edge-hugging parent
			 * doesn't push the dialog partly off-screen. */
			double center_x = px + parent_w / 2.0;
			double center_y = py + parent_h / 2.0;
			struct wlr_output *output = wlr_output_layout_output_at(
				server->output_layout, center_x, center_y);
			if (output) {
				struct wlr_box output_box;
				wlr_output_layout_get_box(server->output_layout, output, &output_box);
				if (x < output_box.x) x = output_box.x;
				if (y < output_box.y) y = output_box.y;
				if (x + w > output_box.x + output_box.width) {
					x = output_box.x + output_box.width - w;
				}
				if (y + h > output_box.y + output_box.height) {
					y = output_box.y + output_box.height - h;
				}
			}
			wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
			centered_on_parent = true;
		}
	}

	if (!centered_on_parent) {
		struct wlr_output *output = wlr_output_layout_output_at(
			server->output_layout, server->cursor->x, server->cursor->y);
		if (output) {
			struct wlr_box output_box;
			wlr_output_layout_get_box(server->output_layout, output, &output_box);
			x = output_box.x + (output_box.width - w) / 2;
			y = output_box.y + (output_box.height - h) / 2;
			wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
		}
	}
}

/* How long to hold off the commit handler's auto-recenter fallback after
 * map, giving an external IPC placement decision (see ipc_handle_place_command)
 * a chance to land first. Chosen to comfortably cover a local socket
 * round-trip to brain.py, not client render time. */
#define PLACEMENT_GRACE_MS 250

static void toplevel_cancel_placement_wait(struct resonance_toplevel *toplevel) {
	bool was_awaiting = toplevel->awaiting_placement;
	toplevel->awaiting_placement = false;
	if (toplevel->placement_timeout) {
		wl_event_source_remove(toplevel->placement_timeout);
		toplevel->placement_timeout = NULL;
	}
	if (was_awaiting && toplevel->scene_tree) {
		wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
	}
}

static int toplevel_placement_timeout(void *data) {
	struct resonance_toplevel *toplevel = data;
	struct wl_event_source *source = toplevel->placement_timeout;
	toplevel->placement_timeout = NULL;
	if (toplevel->awaiting_placement) {
		toplevel->awaiting_placement = false;
		if (!toplevel->user_positioned) {
			center_toplevel(toplevel, toplevel->last_w, toplevel->last_h);
		}
		if (toplevel->scene_tree) {
			wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
		}
	}
	if (source) {
		wl_event_source_remove(source);
	}
	return 0;
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	/* Called when the surface is mapped, or ready to display on-screen. */
	struct resonance_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	struct resonance_server *server = toplevel->server;

	if (toplevel->id == 0) {
		toplevel->id = server->next_window_id++;
	}
	wl_list_insert(&server->toplevels, &toplevel->link);
	xdg_toplevel_sync_fullscreen_flag(toplevel);

	/* Dialogs (e.g. Ghostty's close-confirm) inherit their parent's floor
	 * here rather than at toplevel creation, since xdg_toplevel->parent is
	 * only guaranteed to be set by map time - the client's set_parent
	 * request lands after get_toplevel, so it isn't populated yet when
	 * server_new_xdg_toplevel runs. Without this, a dialog spawned from a
	 * window promoted to a higher floor would render underneath it. */
	if (toplevel->xdg_toplevel->parent) {
		struct wlr_scene_tree *parent_scene_tree = toplevel->xdg_toplevel->parent->base->data;
		if (parent_scene_tree) {
			struct resonance_toplevel *parent_toplevel = parent_scene_tree->node.data;
			if (parent_toplevel && parent_toplevel->floor != toplevel->floor) {
				toplevel->floor = parent_toplevel->floor;
				wlr_scene_node_reparent(&toplevel->scene_tree->node,
					server->floor_trees[toplevel->floor]);
			}
		}
	}

	struct wlr_surface *surf = toplevel->xdg_toplevel->base->surface;
	if (toplevel->fullscreen) {
		xdg_toplevel_apply_fullscreen(toplevel);
	} else {
		/* Center using full surface size — xdg_surface geometry may be 0 at map time. */
		center_toplevel(toplevel, surf->current.width, surf->current.height);

		if (!toplevel->xdg_toplevel->parent) {
			/* Only real top-level windows (not dialogs/popups) are ever
			 * repositioned by external placement memory (see
			 * core/placement.py) - hold off the commit handler's recenter
			 * fallback until we know whether a place: command is coming,
			 * so it can't race that IPC round-trip. */
			toplevel->awaiting_placement = true;
			/* Stay hidden until the placement decision lands (or the grace
			 * timer gives up waiting for one) - otherwise this cursor-
			 * centered fallback position renders for a frame or two before
			 * snapping to the real remembered spot, a visible flash. */
			wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
			struct wl_event_loop *loop = wl_display_get_event_loop(server->wl_display);
			toplevel->placement_timeout =
				wl_event_loop_add_timer(loop, toplevel_placement_timeout, toplevel);
			if (toplevel->placement_timeout)
				wl_event_source_timer_update(toplevel->placement_timeout, PLACEMENT_GRACE_MS);
		}
	}

	/* Parented toplevels (dialogs, e.g. Thunar's Properties window) don't get
	 * a foreign-toplevel handle - mirrors how XWayland already gates
	 * xwayland_view_register_toplevel on !parent && !override_redirect, so
	 * they don't clutter the taskbar/alt-tab list as their own entry. */
	if (!toplevel->xdg_toplevel->parent) {
		toplevel->foreign_handle = wlr_foreign_toplevel_handle_v1_create(
			server->foreign_toplevel_manager);
		if (toplevel->xdg_toplevel->title)
			wlr_foreign_toplevel_handle_v1_set_title(toplevel->foreign_handle,
				toplevel->xdg_toplevel->title);
		if (toplevel->xdg_toplevel->app_id)
			wlr_foreign_toplevel_handle_v1_set_app_id(toplevel->foreign_handle,
				toplevel->xdg_toplevel->app_id);
	}

	{
		toplevel->last_w = surf->current.width;
		toplevel->last_h = surf->current.height;
		if (!toplevel->xdg_toplevel->parent) {
			/* Dialogs/popups already skip the foreign-toplevel handle above
			 * so they don't clutter the taskbar - for the same reason, don't
			 * report them to placement memory (core/placement.py) either.
			 * It has no signal to tell a dialog apart from a real window
			 * beyond app_id collision, which fails whenever the dialog's
			 * app_id differs from its parent's (e.g. generic GTK dialogs
			 * reporting app_id "GTK Application") - it would otherwise
			 * treat the dialog as its own independent window and replay
			 * unrelated remembered geometry onto it. */
			ipc_send_window_geo(server, "map",
				toplevel->xdg_toplevel->title,
				toplevel->xdg_toplevel->app_id,
				toplevel->scene_tree->node.x,
				toplevel->scene_tree->node.y,
				surf->current.width, surf->current.height, toplevel->id, toplevel->fullscreen);
		}
		int radius_top = 12;
		int radius_bottom = 12;
		get_window_radius(toplevel->xdg_toplevel->app_id, NULL, &radius_top, &radius_bottom);
		if (!toplevel->fullscreen) {
			struct wlr_box geo = toplevel->xdg_toplevel->base->geometry;
			int vis_w = (geo.width  > 0) ? geo.width  : surf->current.width;
			int vis_h = (geo.height > 0) ? geo.height : surf->current.height;
			update_window_border(server, toplevel->scene_tree,
				&toplevel->border_node, &toplevel->border_buffer, vis_w, vis_h,
				radius_top, radius_bottom, 0, 0);
			/* Draw glass frame behind CSD dialog toplevels whose surfaces are
			 * transparent (e.g. Ghostty close-confirm, GTK4/libadwaita dialogs).
			 * Use the xdg geometry to strip shadow padding so the frame sits
			 * flush with the visible client content. */
			if (toplevel->xdg_toplevel->parent) {
				update_dialog_background(toplevel->scene_tree,
					&toplevel->dialog_bg_node, &toplevel->dialog_bg_buffer,
					vis_w, vis_h, radius_top, radius_bottom,
					0, 0);
			}
		}
	}

	focus_toplevel(toplevel);
	schedule_frames(server);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	/* Called when the surface is unmapped, and should no longer be shown. */
	struct resonance_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);

	ipc_send_window_unmap(toplevel->server, toplevel->id);

	/* Reset the cursor mode if the grabbed toplevel was unmapped. */
	if (toplevel == toplevel->server->grabbed_toplevel) {
		reset_cursor_mode(toplevel->server);
	}

	if (toplevel == toplevel->server->hovered_toplevel) {
		const char *old_app_id = get_toplevel_app_id(toplevel);
		if (old_app_id) {
			char buf[320];
			snprintf(buf, sizeof(buf), "hover-leave:%s\n", old_app_id);
			ipc_send(toplevel->server, buf);
		}
		toplevel->server->hovered_toplevel = NULL;
	}

	wl_list_remove(&toplevel->link);
	wl_list_init(&toplevel->link);
	schedule_frames(toplevel->server);
	resonance_shutdown_check_done(toplevel->server);
}

/* Max pixels client-declared geometry may differ from the actual committed
 * buffer size before it's treated as stale rather than a legitimate
 * shadow-margin crop (see xdg_toplevel_commit). */
#define GEO_STALE_SLACK 64

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	/* Called when a new surface state is committed. */
	struct resonance_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

	if (toplevel->xdg_toplevel->base->initial_commit) {
		/* When an xdg_surface performs an initial commit, the compositor must
		 * reply with a configure so the client can map the surface. resonance
		 * configures the xdg_toplevel with 0,0 size to let the client pick the
		 * dimensions itself. */
		xdg_toplevel_sync_fullscreen_flag(toplevel);
		if (toplevel->fullscreen) {
			xdg_toplevel_apply_fullscreen(toplevel);
		} else {
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
		}
	} else {
		schedule_frames(toplevel->server);
		struct wlr_xdg_surface *xdg_surf = toplevel->xdg_toplevel->base;
		struct wlr_box geo = xdg_surf->geometry;
		struct wlr_surface *surf = xdg_surf->surface;
		if (toplevel->resize_dirty) {
			wlr_scene_node_set_position(&toplevel->scene_tree->node, toplevel->pending_x, toplevel->pending_y);
			toplevel->resize_dirty = false;
		}
		int out_x, out_y, out_w, out_h;
		out_x = toplevel->scene_tree->node.x;
		out_y = toplevel->scene_tree->node.y;
		/* geo is client-declared, and normally trustworthy - it lets CSD
		 * clients crop invisible shadow margins out of the decoration box.
		 * Some clients (kdenlive, at least) don't keep it in sync with what
		 * they actually render: it sticks at a stale/default size while the
		 * real buffer resizes to something else entirely. Only trust geo
		 * when it's plausibly just a shadow-margin crop of the real buffer;
		 * otherwise the buffer is what's actually on screen, so use that. */
		if (geo.width > 0 && geo.height > 0 &&
				abs(geo.width - surf->current.width) <= GEO_STALE_SLACK &&
				abs(geo.height - surf->current.height) <= GEO_STALE_SLACK) {
			out_w = geo.width;
			out_h = geo.height;
		} else {
			out_w = surf->current.width;
			out_h = surf->current.height;
		}
		if (out_w > 0 && out_h > 0) {
			if ((out_w != toplevel->last_w || out_h != toplevel->last_h || (!toplevel->fullscreen && !toplevel->border_node)) && surf->mapped) {
				toplevel->last_w = out_w;
				toplevel->last_h = out_h;
				if (toplevel->fullscreen) {
					xdg_toplevel_apply_fullscreen(toplevel);
				} else if (!toplevel->user_positioned && !toplevel->awaiting_placement) {
					center_toplevel(toplevel, out_w, out_h);
				}
				if (!toplevel->xdg_toplevel->parent) {
					/* See the matching skip in xdg_toplevel_map - dialogs
					 * never go to placement memory. */
					ipc_send_window_geo(toplevel->server, "geometry",
						toplevel->xdg_toplevel->title,
						toplevel->xdg_toplevel->app_id,
						toplevel->scene_tree->node.x, toplevel->scene_tree->node.y,
						out_w, out_h, toplevel->id, toplevel->fullscreen);
				}
				int radius_top = 12;
				int radius_bottom = 12;
				get_window_radius(toplevel->xdg_toplevel->app_id, NULL, &radius_top, &radius_bottom);
				if (!toplevel->fullscreen) {
					int border_w = out_w;
					int border_h = out_h;
					/* Draw to the slot snapshotted at maximize time, not to a
					 * freshly measured one. Re-measuring here would track the
					 * floor's other windows live, so pushing a neighbour aside
					 * grew this border while its surface stayed put - a wide
					 * band of bare frame beside unchanged content. */
					if (toplevel->floor_maximized &&
							toplevel->maxed_w > 0 && toplevel->maxed_h > 0) {
						border_w = toplevel->maxed_w;
						border_h = toplevel->maxed_h;
					}
					update_window_border(toplevel->server, toplevel->scene_tree,
						&toplevel->border_node, &toplevel->border_buffer, border_w, border_h,
						radius_top, radius_bottom, 0, 0);
					/* Resize the glass frame when geometry changes */
					if (toplevel->xdg_toplevel->parent) {
						update_dialog_background(toplevel->scene_tree,
							&toplevel->dialog_bg_node, &toplevel->dialog_bg_buffer,
							out_w, out_h, radius_top, radius_bottom,
							0, 0);
					}
				}
			}
		}
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	/* Called when the xdg_toplevel is destroyed. */
	struct resonance_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	toplevel_cancel_placement_wait(toplevel);

	if (toplevel->server->grabbed_toplevel == toplevel) {
		reset_cursor_mode(toplevel->server);
	}

	if (toplevel == toplevel->server->hovered_toplevel) {
		const char *old_app_id = get_toplevel_app_id(toplevel);
		if (old_app_id) {
			char buf[320];
			snprintf(buf, sizeof(buf), "hover-leave:%s\n", old_app_id);
			ipc_send(toplevel->server, buf);
		}
		toplevel->server->hovered_toplevel = NULL;
	}

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	wl_list_remove(&toplevel->set_parent.link);

	if (toplevel->foreign_handle)
		wlr_foreign_toplevel_handle_v1_destroy(toplevel->foreign_handle);

	if (toplevel->border_buffer) {
		wlr_buffer_drop(toplevel->border_buffer);
	}
	if (toplevel->dialog_bg_buffer) {
		wlr_buffer_drop(toplevel->dialog_bg_buffer);
	}

	if (toplevel->scene_tree)
		toplevel->scene_tree->node.data = NULL;

	wl_list_remove(&toplevel->link);
	free(toplevel);
}

static void begin_interactive(struct resonance_toplevel *toplevel,
		enum resonance_cursor_mode mode, uint32_t edges) {
	/* This function sets up an interactive move or resize operation, where the
	 * compositor stops propagating pointer events to clients and instead
	 * consumes them itself, to move or resize windows. */
	struct resonance_server *server = toplevel->server;

	server->grabbed_toplevel = toplevel;
	server->grabbed_xwayland_view = NULL;
	server->cursor_mode = mode;

	if (toplevel->floor_maximized) {
		toplevel->floor_maximized = false;
	}

	if (mode == RESONANCE_CURSOR_MOVE) {
		toplevel_cancel_placement_wait(toplevel);
		toplevel->user_positioned = true;
		server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
		server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
	} else {
		toplevel_cancel_placement_wait(toplevel);
		toplevel->user_positioned = true;
		struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
		/* geo_box's width/height is client-declared and can go stale (see
		 * xdg_toplevel_commit) while the client keeps rendering a different
		 * size. last_w/last_h is the size the compositor last actually
		 * trusted and drew, so use that for the grab box instead - otherwise
		 * grabbing a resize edge snaps the window to the stale size before
		 * the drag delta gets applied back on top. Keep geo_box->x/y since
		 * that offset is a legitimate shadow-margin crop. */
		int box_w = toplevel->last_w > 0 ? toplevel->last_w : geo_box->width;
		int box_h = toplevel->last_h > 0 ? toplevel->last_h : geo_box->height;

		double border_x = (toplevel->scene_tree->node.x + geo_box->x) +
			((edges & WLR_EDGE_RIGHT) ? box_w : 0);
		double border_y = (toplevel->scene_tree->node.y + geo_box->y) +
			((edges & WLR_EDGE_BOTTOM) ? box_h : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = *geo_box;
		server->grab_geobox.width = box_w;
		server->grab_geobox.height = box_h;
		server->grab_geobox.x += toplevel->scene_tree->node.x;
		server->grab_geobox.y += toplevel->scene_tree->node.y;

		server->resize_edges = edges;
		resonance_push_begin(server, toplevel->floor, toplevel, NULL, edges);
	}
}

static void xdg_toplevel_request_move(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * move, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct resonance_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
	begin_interactive(toplevel, RESONANCE_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to begin an interactive
	 * resize, typically because the user clicked on their client-side
	 * decorations. Note that a more sophisticated compositor should check the
	 * provided serial against a list of button press serials sent to this
	 * client, to prevent the client from requesting this whenever they want. */
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct resonance_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
	begin_interactive(toplevel, RESONANCE_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(
		struct wl_listener *listener, void *data) {
	/* This event is raised when a client would like to maximize itself,
	 * typically because the user clicked on the maximize button on client-side
	 * decorations. resonance doesn't support maximization, but to conform to
	 * xdg-shell protocol we still must send a configure.
	 * wlr_xdg_surface_schedule_configure() is used to send an empty reply.
	 * However, if the request was sent before an initial commit, we don't do
	 * anything and let the client finish the initial surface setup. */
	struct resonance_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void resonance_toplevel_set_fullscreen(struct resonance_toplevel *toplevel, bool fullscreen) {
	struct resonance_server *server = toplevel->server;

	if (fullscreen == toplevel->fullscreen) {
		return;
	}

	if (fullscreen) {
		if (toplevel->floor_maximized) {
			resonance_toplevel_set_floor_maximized(toplevel, false);
		}
		toplevel->saved_x = toplevel->scene_tree->node.x;
		toplevel->saved_y = toplevel->scene_tree->node.y;
		toplevel->saved_w = toplevel->xdg_toplevel->base->geometry.width;
		toplevel->saved_h = toplevel->xdg_toplevel->base->geometry.height;
		toplevel->fullscreen = true;

		if (toplevel->border_node) {
			wlr_scene_node_destroy(&toplevel->border_node->node);
			toplevel->border_node = NULL;
		}
		if (toplevel->dialog_bg_node) {
			wlr_scene_node_destroy(&toplevel->dialog_bg_node->node);
			toplevel->dialog_bg_node = NULL;
		}

		struct wlr_box box;
		if (xdg_toplevel_fullscreen_box(toplevel, &box)) {
			if (toplevel->scene_tree) {
				wlr_scene_node_set_position(&toplevel->scene_tree->node, box.x, box.y);
			}
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, box.width, box.height);
			toplevel->last_w = box.width;
			toplevel->last_h = box.height;
			/* Send the geometry report here rather than relying on the commit
			 * handler's size-diff gate (xdg_toplevel_commit) - if the window
			 * already happened to be sized/positioned to fill the output before
			 * going fullscreen, out_w/out_h there won't have changed, so that
			 * gate would never fire and placement memory would never learn
			 * fullscreen turned on (see core/placement.py). */
			ipc_send_window_geo(server, "geometry",
				toplevel->xdg_toplevel->title, toplevel->xdg_toplevel->app_id,
				box.x, box.y, box.width, box.height, toplevel->id, true);
		}
	} else {
		toplevel->fullscreen = false;
		if (toplevel->scene_tree) {
			wlr_scene_node_set_position(&toplevel->scene_tree->node, toplevel->saved_x, toplevel->saved_y);
		}
		if (toplevel->saved_w > 0 && toplevel->saved_h > 0) {
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->saved_w, toplevel->saved_h);
		}
		int radius_top = 12;
		int radius_bottom = 12;
		get_window_radius(toplevel->xdg_toplevel->app_id, NULL, &radius_top, &radius_bottom);
		int w = (toplevel->saved_w > 0) ? toplevel->saved_w : toplevel->xdg_toplevel->base->geometry.width;
		int h = (toplevel->saved_h > 0) ? toplevel->saved_h : toplevel->xdg_toplevel->base->geometry.height;
		update_window_border(server, toplevel->scene_tree,
			&toplevel->border_node, &toplevel->border_buffer, w, h,
			radius_top, radius_bottom, 0, 0);
		if (toplevel->xdg_toplevel->parent) {
			update_dialog_background(toplevel->scene_tree,
				&toplevel->dialog_bg_node, &toplevel->dialog_bg_buffer,
				w, h, radius_top, radius_bottom,
				0, 0);
		}
		toplevel->last_w = w;
		toplevel->last_h = h;
		ipc_send_window_geo(server, "geometry",
			toplevel->xdg_toplevel->title, toplevel->xdg_toplevel->app_id,
			toplevel->saved_x, toplevel->saved_y, w, h, toplevel->id, false);
	}

	wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, fullscreen);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void xdg_toplevel_set_parent(struct wl_listener *listener, void *data) {
	struct resonance_toplevel *toplevel = wl_container_of(listener, toplevel, set_parent);
	struct resonance_server *server = toplevel->server;
	struct wlr_xdg_toplevel *parent = toplevel->xdg_toplevel->parent;

	if (!parent || !toplevel->xdg_toplevel->base->surface->mapped) {
		return;
	}

	wlr_log(WLR_INFO, "late parent: id=%u app_id=%s title=%s",
		toplevel->id,
		toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "",
		toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "");

	ipc_send_window_unmap(server, toplevel->id);
	toplevel_cancel_placement_wait(toplevel);

	struct wlr_scene_tree *parent_scene_tree = parent->base->data;
	if (parent_scene_tree) {
		struct resonance_toplevel *parent_toplevel = parent_scene_tree->node.data;
		if (parent_toplevel && parent_toplevel->floor != toplevel->floor) {
			toplevel->floor = parent_toplevel->floor;
			wlr_scene_node_reparent(&toplevel->scene_tree->node,
				server->floor_trees[toplevel->floor]);
		}
	}

	if (!toplevel->fullscreen) {
		toplevel->user_positioned = false;
		center_toplevel(toplevel, toplevel->last_w, toplevel->last_h);
	}
	schedule_frames(server);
}

static void xdg_toplevel_request_fullscreen(
		struct wl_listener *listener, void *data) {
	struct resonance_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	if (!toplevel->xdg_toplevel->base->initialized) {
		/* Surface isn't ready for a configure yet - xdg_toplevel_sync_fullscreen_flag
		 * picks up requested.fullscreen once it maps, so nothing to do here. */
		return;
	}
	resonance_toplevel_set_fullscreen(toplevel, toplevel->xdg_toplevel->requested.fullscreen);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	/* This event is raised when a client creates a new toplevel (application window). */
	struct resonance_server *server = wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	/* Allocate a resonance_toplevel for this surface */
	struct resonance_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	if (!toplevel) {
		wlr_log(WLR_ERROR, "Failed to allocate toplevel structure!");
		return;
	}
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	wl_list_init(&toplevel->link);

	/* xdg_toplevel->parent (e.g. Ghostty's close-confirm dialog) isn't set
	 * yet here - the client sends set_parent as a follow-up request after
	 * get_toplevel, so it lands after this handler runs. Floor inheritance
	 * for dialogs happens in xdg_toplevel_map instead, once parent is
	 * guaranteed to be populated. */
	toplevel->floor = RESONANCE_FLOOR_GROUND;
	toplevel->scene_tree = wlr_scene_xdg_surface_create(
		toplevel->server->floor_trees[RESONANCE_FLOOR_GROUND], xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->scene_tree;

	/* Listen to the various events it can emit */
	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	/* cotd */
	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
	toplevel->set_parent.notify = xdg_toplevel_set_parent;
	wl_signal_add(&xdg_toplevel->events.set_parent, &toplevel->set_parent);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	/* Called when a new surface state is committed. */
	struct resonance_popup *popup = wl_container_of(listener, popup, commit);

	if (popup->xdg_popup->base->initial_commit) {
		/* When an xdg_surface performs an initial commit, the compositor must
		 * reply with a configure so the client can map the surface.
		 * resonance sends an empty configure. A more sophisticated compositor
		 * might change an xdg_popup's geometry to ensure it's not positioned
		 * off-screen, for example. */
		if (popup->xdg_popup->base->initialized) {
			wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
		}
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	/* Called when the xdg_popup is destroyed. */
	struct resonance_popup *popup = wl_container_of(listener, popup, destroy);

	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);

	free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	/* This event is raised when a client creates a new popup. */
	struct wlr_xdg_popup *xdg_popup = data;
	struct resonance_server *server = wl_container_of(listener, server, new_xdg_popup);

	struct resonance_popup *popup = calloc(1, sizeof(*popup));
	if (!popup) {
		wlr_log(WLR_ERROR, "Failed to allocate popup structure!");
		return;
	}
	popup->xdg_popup = xdg_popup;

	/* Find the parent scene tree. For popups from layer shell surfaces,
	 * xdg_popup->parent is NULL or not an xdg_surface — fall back to the
	 * overlay layer so the popup still renders above everything. */
	struct wlr_scene_tree *parent_tree = server->layer_trees[3];
	if (xdg_popup->parent) {
		struct wlr_xdg_surface *parent =
			wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
		if (parent && parent->data) {
			parent_tree = parent->data;
		}
	}
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
	struct resonance_layer_surface *layer =
		wl_container_of(listener, layer, commit);
	struct wlr_layer_surface_v1 *wlr_layer_surface =
		layer->scene_layer_surface->layer_surface;

	if (layer->server->backend_failed) {
		return;
	}

	uint32_t committed = wlr_layer_surface->current.committed;
	if (committed == 0 && !wlr_layer_surface->initial_commit) {
		return;
	}

	struct wlr_output *output = wlr_layer_surface->output;
	if (!output) {
		return;
	}
	struct wlr_box full_area = {0};
	wlr_output_layout_get_box(layer->server->output_layout, output, &full_area);
	struct wlr_box usable_area = full_area;

	wlr_scene_layer_surface_v1_configure(
		layer->scene_layer_surface, &full_area, &usable_area);

	if (wlr_layer_surface->initial_commit) {
		if (wlr_layer_surface->current.keyboard_interactive ==
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
			struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(layer->server->seat);
			if (keyboard) {
				wlr_seat_keyboard_notify_enter(layer->server->seat,
					wlr_layer_surface->surface,
					keyboard->keycodes, keyboard->num_keycodes,
					&keyboard->modifiers);
			}
		}
	}

	schedule_frames(layer->server);
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
	struct resonance_layer_surface *layer =
		wl_container_of(listener, layer, destroy);
	wl_list_remove(&layer->commit.link);
	wl_list_remove(&layer->destroy.link);
	free(layer);
}

static void server_new_layer_surface(struct wl_listener *listener, void *data) {
	struct resonance_server *server =
		wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *wlr_layer_surface = data;

	if (!wlr_layer_surface->output) {
		struct wlr_output *output = wlr_output_layout_get_center_output(
			server->output_layout);
		if (!output) {
			wlr_layer_surface_v1_destroy(wlr_layer_surface);
			return;
		}
		wlr_layer_surface->output = output;
	}

	enum zwlr_layer_shell_v1_layer layer = wlr_layer_surface->pending.layer;
	struct wlr_scene_tree *parent = server->layer_trees[layer];

	struct resonance_layer_surface *surface =
		calloc(1, sizeof(*surface));
	if (!surface) {
		wlr_log(WLR_ERROR, "Failed to allocate layer surface structure!");
		return;
	}
	surface->server = server;
	surface->scene_layer_surface =
		wlr_scene_layer_surface_v1_create(parent, wlr_layer_surface);
	surface->scene_layer_surface->layer_surface->data = surface;

	surface->commit.notify = layer_surface_commit;
	wl_signal_add(&wlr_layer_surface->surface->events.commit, &surface->commit);

	surface->destroy.notify = layer_surface_destroy;
	wl_signal_add(&wlr_layer_surface->events.destroy, &surface->destroy);
}

static void xwayland_view_sync_fullscreen_flag(struct resonance_xwayland_view *view) {
	if (!view->fullscreen && view->xwayland_surface->fullscreen) {
		/* Client already had the X11 fullscreen property set before this map
		 * (no request_fullscreen event fires in that case, since wlroots only
		 * emits it on state *change*) — sync our tracking flag from the actual
		 * property instead of waiting for an event that won't come. */
		view->fullscreen = true;
	}
}

static struct resonance_output *resonance_server_get_primary_output(struct resonance_server *server) {
	struct resonance_output *out = NULL, *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (strcmp(o->wlr_output->name, "DP-1") == 0) {
			out = o;
			break;
		}
	}
	if (!out && !wl_list_empty(&server->outputs)) {
		out = wl_container_of(server->outputs.next, out, link);
	}
	return out;
}

static void resonance_server_get_active_or_primary_box(struct resonance_server *server, struct wlr_box *box) {
	struct wlr_output *active_output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (active_output) {
		wlr_output_layout_get_box(server->output_layout, active_output, box);
		return;
	}

	struct resonance_output *out = resonance_server_get_primary_output(server);
	if (!out) {
		*box = (struct wlr_box){0};
		return;
	}
	wlr_output_layout_get_box(server->output_layout, out->wlr_output, box);
}

static void xwayland_view_map_fullscreen(struct resonance_xwayland_view *view, struct resonance_server *server) {
	/* Remapping while still flagged fullscreen (common during Proton/XWayland
	 * games' startup or resolution-switch remap churn) — reapply the fullscreen
	 * box instead of falling through to the windowed-position logic below, or
	 * the window snaps back to its pre-fullscreen size/position on every remap. */
	struct resonance_output *out = resonance_server_get_primary_output(server);
	if (out) {
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout, out->wlr_output, &box);
		if (view->scene_tree) {
			wlr_scene_node_set_position(&view->scene_tree->node, box.x, box.y);
		}
		wlr_xwayland_surface_configure(view->xwayland_surface, box.x, box.y, box.width, box.height);
	}
}

static void xwayland_view_map_windowed(struct resonance_xwayland_view *view, struct resonance_server *server) {
	/* Top-level window: use the client's remembered position if any part of
	 * the window is on-screen. Center on DP-1 only as a last resort so that
	 * the client's internal geometry always matches the scene node position —
	 * child windows (menus, popups) inherit their position from the parent's
	 * X11 coords, so disagreement here breaks submenu placement. */
	int req_x = view->xwayland_surface->x;
	int req_y = view->xwayland_surface->y;
	int place_x = req_x, place_y = req_y;

	bool on_screen = false;
	if (req_x != 0 || req_y != 0) {
		if (!wl_list_empty(&server->outputs)) {
			struct resonance_output *o;
			wl_list_for_each(o, &server->outputs, link) {
				struct wlr_box box;
				wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
				int w = view->xwayland_surface->width > 0 ? view->xwayland_surface->width : 800;
				int h = view->xwayland_surface->height > 0 ? view->xwayland_surface->height : 600;
				if (req_x + w > box.x && req_x < box.x + box.width &&
						req_y + h > box.y && req_y < box.y + box.height) {
					on_screen = true;
					break;
				}
			}
		}
	}

	if (!on_screen && !wl_list_empty(&server->outputs)) {
		struct wlr_box box;
		resonance_server_get_active_or_primary_box(server, &box);
		int w = view->xwayland_surface->width > 0 ? view->xwayland_surface->width : 800;
		int h = view->xwayland_surface->height > 0 ? view->xwayland_surface->height : 600;
		place_x = box.x + (box.width - w) / 2;
		place_y = box.y + (box.height - h) / 2;
		if (place_x < box.x) place_x = box.x;
		if (place_y < box.y) place_y = box.y;
	}

	view->x_offset = place_x - req_x;
	view->y_offset = place_y - req_y;
	if (view->scene_tree) {
		wlr_scene_node_set_position(&view->scene_tree->node, place_x, place_y);
	}
	wlr_xwayland_surface_configure(view->xwayland_surface, place_x, place_y,
		view->xwayland_surface->width, view->xwayland_surface->height);
}

static void xwayland_view_map_child(struct resonance_xwayland_view *view, struct resonance_server *server) {
	int child_x = view->xwayland_surface->x;
	int child_y = view->xwayland_surface->y;
	if (child_x == 0 && child_y == 0 && view->xwayland_surface->parent) {
		/* Parented window at default (0,0) — app placed it before receiving
		 * the parent's configure, so center it on the active output. */
		struct wlr_box box;
		resonance_server_get_active_or_primary_box(server, &box);
		int w = view->xwayland_surface->width > 0 ? view->xwayland_surface->width : 400;
		int h = view->xwayland_surface->height > 0 ? view->xwayland_surface->height : 300;
		child_x = box.x + (box.width - w) / 2;
		child_y = box.y + (box.height - h) / 2;
		if (child_x < box.x) child_x = box.x;
		if (child_y < box.y) child_y = box.y;
		/* Make the centered position authoritative on the X11 side too,
		 * otherwise the next commit re-places the child from the stale
		 * xwayland_surface->x/y of (0,0). */
		wlr_xwayland_surface_configure(view->xwayland_surface, child_x, child_y, w, h);
	}

	/* child_x/child_y are now absolute (root-window) coordinates, but this
	 * view's scene node may be nested inside its parent window's own scene
	 * tree (see xwayland_view_associate) - subtract the parent tree's
	 * on-screen position before positioning, or the popup drifts further
	 * off the further the parent window sits from (0,0). */
	int rel_x = child_x, rel_y = child_y;
	if (view->scene_tree->node.parent) {
		int px = 0, py = 0;
		wlr_scene_node_coords(&view->scene_tree->node.parent->node, &px, &py);
		rel_x -= px;
		rel_y -= py;
	}
	wlr_scene_node_set_position(&view->scene_tree->node, rel_x, rel_y);

	/* Focus dismiss layer / gatekeeper:
	 * If this is an override-redirect window (popup), check if it's far from the cursor.
	 * If it's far from the cursor, it's likely a sync flicker at the old window position,
	 * so we hide it by setting its opacity to 0.0f (transparent but input-active) until
	 * it receives a configure request or commits a frame near the cursor. */
	view->hidden_for_sync = false;
	if (view->xwayland_surface->override_redirect) {
		double cx = server->cursor->x;
		double cy = server->cursor->y;
		if (abs(child_x - (int)cx) > 400 || abs(child_y - (int)cy) > 400) {
			float opacity = 0.0f;
			set_view_opacity(view, opacity);
			view->hidden_for_sync = true;
			view->sync_flicker_frames = 0;
		}
	}
}

static void xwayland_view_register_toplevel(struct resonance_xwayland_view *view, struct resonance_server *server) {
	view->last_w = view->xwayland_surface->width;
	view->last_h = view->xwayland_surface->height;
	view->foreign_handle = wlr_foreign_toplevel_handle_v1_create(
		server->foreign_toplevel_manager);
	if (view->xwayland_surface->title)
		wlr_foreign_toplevel_handle_v1_set_title(view->foreign_handle,
			view->xwayland_surface->title);
	if (view->xwayland_surface->class)
		wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign_handle,
			view->xwayland_surface->class);
	ipc_send_window_geo(server, "map",
		view->xwayland_surface->title,
		view->xwayland_surface->class,
		view->scene_tree->node.x, view->scene_tree->node.y,
		view->xwayland_surface->width, view->xwayland_surface->height, view->id,
		view->fullscreen);
	int radius_top = 12;
	int radius_bottom = 12;
	get_window_radius(NULL, view->xwayland_surface->class, &radius_top, &radius_bottom);
	if (!view->fullscreen)
		update_window_border(server, view->scene_tree,
			&view->border_node, &view->border_buffer, view->xwayland_surface->width, view->xwayland_surface->height,
			radius_top, radius_bottom, 0, 0);
}

static void xwayland_view_map_focus(struct resonance_xwayland_view *view, struct resonance_server *server) {
	if (!view->xwayland_surface->override_redirect) {
		focus_xwayland_view(view);
	} else if (view->scene_tree) {
		/* Override-redirect windows (dropdown menus, tooltips) are never
		 * focused, but they must be raised above normal windows or they'll
		 * be hidden behind whichever app has keyboard focus. */
		wlr_scene_node_raise_to_top(&view->scene_tree->node);

		/* Give the popup initial pointer focus so the motion lock in
		 * process_cursor_motion engages. Coordinates are unclamped —
		 * X11 grabs naturally report out-of-bounds relative coords. */
		struct wlr_surface *popup_surf = view->xwayland_surface->surface;
		double sx = server->cursor->x - view->xwayland_surface->x;
		double sy = server->cursor->y - view->xwayland_surface->y;
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		uint32_t now_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
		wlr_seat_pointer_notify_enter(server->seat, popup_surf, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, now_ms, sx, sy);
	}
}

static void xwayland_view_map(struct wl_listener *listener, void *data) {
	struct resonance_xwayland_view *view = wl_container_of(listener, view, map);
	struct resonance_server *server = view->server;
	if (view->id == 0) {
		view->id = server->next_window_id++;
	}
	if (wl_list_empty(&view->link)) {
		wl_list_insert(&server->xwayland_views, &view->link);
	}

	if (view->scene_tree) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, true);
	}

	xwayland_view_sync_fullscreen_flag(view);

	if (!view->xwayland_surface->parent && !view->xwayland_surface->override_redirect && view->fullscreen) {
		xwayland_view_map_fullscreen(view, server);
	} else if (!view->xwayland_surface->parent && !view->xwayland_surface->override_redirect) {
		xwayland_view_map_windowed(view, server);
	} else if (view->scene_tree) {
		xwayland_view_map_child(view, server);
	}

	if (!view->xwayland_surface->override_redirect &&
			!view->xwayland_surface->parent) {
		xwayland_view_register_toplevel(view, server);
	}

	xwayland_view_map_focus(view, server);
	schedule_frames(server);
}

static void xwayland_view_unmap(struct wl_listener *listener, void *data) {
	struct resonance_xwayland_view *view = wl_container_of(listener, view, unmap);

	if (view == view->server->grabbed_xwayland_view) {
		reset_cursor_mode(view->server);
	}

	if ((struct resonance_toplevel *)view == view->server->hovered_toplevel) {
		const char *old_app_id = get_toplevel_app_id((struct resonance_toplevel *)view);
		if (old_app_id) {
			char buf[320];
			snprintf(buf, sizeof(buf), "hover-leave:%s\n", old_app_id);
			ipc_send(view->server, buf);
		}
		view->server->hovered_toplevel = NULL;
	}

	if (!view->xwayland_surface->override_redirect && !view->xwayland_surface->parent) {
		ipc_send_window_unmap(view->server, view->id);
	}
	wl_list_remove(&view->link);
	wl_list_init(&view->link);
	if (view->scene_tree) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	}
	if (view->foreign_handle) {
		wlr_foreign_toplevel_handle_v1_destroy(view->foreign_handle);
		view->foreign_handle = NULL;
	}



	schedule_frames(view->server);

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t now_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	process_cursor_motion(view->server, now_ms);

	resonance_shutdown_check_done(view->server);
}

static void xwayland_view_commit(struct wl_listener *listener, void *data) {
	struct resonance_xwayland_view *view = wl_container_of(listener, view, commit);
	if ((view->xwayland_surface->override_redirect || view->xwayland_surface->parent) && view->scene_tree) {
		int child_x = view->xwayland_surface->x;
		int child_y = view->xwayland_surface->y;
		/* See xwayland_view_map_child: subtract the parent tree's on-screen
		 * position since child_x/child_y are absolute but this node may be
		 * nested inside the parent window's own scene tree. */
		int rel_x = child_x, rel_y = child_y;
		if (view->scene_tree->node.parent) {
			int px = 0, py = 0;
			wlr_scene_node_coords(&view->scene_tree->node.parent->node, &px, &py);
			rel_x -= px;
			rel_y -= py;
		}
		wlr_scene_node_set_position(&view->scene_tree->node, rel_x, rel_y);
		if (view->xwayland_surface->override_redirect && view->hidden_for_sync) {
			double cx = view->server->cursor->x;
			double cy = view->server->cursor->y;
			view->sync_flicker_frames++;
			if ((abs(child_x - (int)cx) <= 400 && abs(child_y - (int)cy) <= 400) ||
					view->sync_flicker_frames >= 2) {
				float opacity = 1.0f;
				set_view_opacity(view, opacity);
				view->hidden_for_sync = false;
			} else {
				float opacity = 0.0f;
				set_view_opacity(view, opacity);
			}
		}
	}
	if (!view->xwayland_surface->override_redirect && view->scene_tree) {
		/* Draw the window border for plain toplevels and for parented XWayland
		 * dialogs alike (mirrors xdg_toplevel_commit, which borders dialogs
		 * too) - only genuine override-redirect popups (menus/tooltips) skip
		 * chrome entirely. */
		int w = view->xwayland_surface->width;
		int h = view->xwayland_surface->height;
		if (w > 0 && h > 0 && (w != view->last_w || h != view->last_h || (!view->fullscreen && !view->border_node))) {
			view->last_w = w;
			view->last_h = h;
			int radius_top = 12;
			int radius_bottom = 12;
			get_window_radius(NULL, view->xwayland_surface->class, &radius_top, &radius_bottom);
			if (!view->fullscreen) {
				int border_w = w;
				int border_h = h;
				/* See the matching note in xdg_toplevel_commit. */
				if (view->floor_maximized &&
						view->maxed_w > 0 && view->maxed_h > 0) {
					border_w = view->maxed_w;
					border_h = view->maxed_h;
				}
				update_window_border(view->server, view->scene_tree,
					&view->border_node, &view->border_buffer, border_w, border_h,
					radius_top, radius_bottom, 0, 0);
			}
		}
	}


	schedule_frames(view->server);
}

static void xwayland_view_request_configure(struct wl_listener *listener, void *data) {
	struct resonance_xwayland_view *view = wl_container_of(listener, view, request_configure);
	struct wlr_xwayland_surface_configure_event *event = data;

	if (view->xwayland_surface->parent || view->xwayland_surface->override_redirect) {
		/* Popup/child/override_redirect: honor the position the client requested. */
		wlr_xwayland_surface_configure(view->xwayland_surface,
			event->x, event->y, event->width, event->height);
		if (view->scene_tree) {
			/* event->x/y are absolute (root-window) coordinates, but this view's
			 * scene node may be nested inside its parent window's own scene tree
			 * (see xwayland_view_associate) - subtract the parent tree's on-screen
			 * position first, same as xwayland_view_map_child, or the dialog jumps
			 * off by roughly the parent's own screen position on every move/resize. */
			int rel_x = event->x, rel_y = event->y;
			if (view->scene_tree->node.parent) {
				int px = 0, py = 0;
				wlr_scene_node_coords(&view->scene_tree->node.parent->node, &px, &py);
				rel_x -= px;
				rel_y -= py;
			}
			wlr_scene_node_set_position(&view->scene_tree->node, rel_x, rel_y);
			if (view->hidden_for_sync) {
				double cx = view->server->cursor->x;
				double cy = view->server->cursor->y;
				if (abs(event->x - (int)cx) <= 400 && abs(event->y - (int)cy) <= 400) {
					float opacity = 1.0f;
					set_view_opacity(view, opacity);
					view->hidden_for_sync = false;
				}
			}
		}
	} else {
		/* Top-level: the compositor manages the position. We ignore the client's
		 * requested position (event->x, event->y) to prevent the down-and-right
		 * shift bug, but we configure the surface at the scene node's current
		 * position to keep them in sync. */
		int current_x = event->x;
		int current_y = event->y;
		if (view->scene_tree) {
			current_x = view->scene_tree->node.x;
			current_y = view->scene_tree->node.y;
		}
		wlr_xwayland_surface_configure(view->xwayland_surface,
			current_x, current_y, event->width, event->height);
	}
}

static void resonance_xwayland_view_set_fullscreen(struct resonance_xwayland_view *view, bool fullscreen) {
	struct resonance_server *server = view->server;

	if (fullscreen == view->fullscreen) {
		return;
	}

	if (fullscreen) {
		if (view->floor_maximized) {
			resonance_xwayland_view_set_floor_maximized(view, false);
		}
		view->saved_x = view->scene_tree->node.x;
		view->saved_y = view->scene_tree->node.y;
		view->saved_w = view->xwayland_surface->width;
		view->saved_h = view->xwayland_surface->height;
		view->fullscreen = true;

		if (view->border_node) {
			wlr_scene_node_destroy(&view->border_node->node);
			view->border_node = NULL;
		}

		int cx = view->saved_x + view->saved_w / 2;
		int cy = view->saved_y + view->saved_h / 2;
		struct wlr_box box;
		if (resonance_output_box_at(server, cx, cy, &box)) {
			if (view->scene_tree) {
				wlr_scene_node_set_position(&view->scene_tree->node, box.x, box.y);
			}
			wlr_xwayland_surface_configure(view->xwayland_surface,
				box.x, box.y, box.width, box.height);
			view->last_w = box.width;
			view->last_h = box.height;
			ipc_send_window_geo(server, "geometry",
				view->xwayland_surface->title, view->xwayland_surface->class,
				box.x, box.y, box.width, box.height, view->id, true);
		}
	} else {
		view->fullscreen = false;
		if (view->scene_tree) {
			wlr_scene_node_set_position(&view->scene_tree->node, view->saved_x, view->saved_y);
		}
		if (view->saved_w > 0 && view->saved_h > 0) {
			wlr_xwayland_surface_configure(view->xwayland_surface,
				view->saved_x, view->saved_y, view->saved_w, view->saved_h);
		}
		int radius_top = 12;
		int radius_bottom = 12;
		get_window_radius(NULL, view->xwayland_surface->class, &radius_top, &radius_bottom);
		int w = (view->saved_w > 0) ? view->saved_w : view->xwayland_surface->width;
		int h = (view->saved_h > 0) ? view->saved_h : view->xwayland_surface->height;
		update_window_border(server, view->scene_tree,
			&view->border_node, &view->border_buffer, w, h,
			radius_top, radius_bottom, 0, 0);
		view->last_w = w;
		view->last_h = h;
		ipc_send_window_geo(server, "geometry",
			view->xwayland_surface->title, view->xwayland_surface->class,
			view->saved_x, view->saved_y, w, h, view->id, false);
	}

	wlr_xwayland_surface_set_fullscreen(view->xwayland_surface, fullscreen);
	schedule_frames(server);
}

static void xwayland_view_request_fullscreen(struct wl_listener *listener, void *data) {
	struct resonance_xwayland_view *view = wl_container_of(listener, view, request_fullscreen);
	resonance_xwayland_view_set_fullscreen(view, view->xwayland_surface->fullscreen);
}

static void begin_interactive_xwayland(struct resonance_xwayland_view *view,
		enum resonance_cursor_mode mode, uint32_t edges) {
	struct resonance_server *server = view->server;
	server->grabbed_toplevel = NULL;
	server->grabbed_xwayland_view = view;
	server->cursor_mode = mode;

	if (view->floor_maximized) {
		view->floor_maximized = false;
	}

	if (mode == RESONANCE_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - view->scene_tree->node.x;
		server->grab_y = server->cursor->y - view->scene_tree->node.y;
	} else {
		int w = view->xwayland_surface->width;
		int h = view->xwayland_surface->height;
		double border_x = view->scene_tree->node.x + ((edges & WLR_EDGE_RIGHT) ? w : 0);
		double border_y = view->scene_tree->node.y + ((edges & WLR_EDGE_BOTTOM) ? h : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;
		server->grab_geobox.x = view->scene_tree->node.x;
		server->grab_geobox.y = view->scene_tree->node.y;
		server->grab_geobox.width = w;
		server->grab_geobox.height = h;
		server->resize_edges = edges;
		resonance_push_begin(server, view->floor, NULL, view, edges);
	}
}

static void xwayland_view_request_move(struct wl_listener *listener, void *data) {
	struct resonance_xwayland_view *view = wl_container_of(listener, view, request_move);
	begin_interactive_xwayland(view, RESONANCE_CURSOR_MOVE, 0);
}

static void xwayland_view_request_resize(struct wl_listener *listener, void *data) {
	struct resonance_xwayland_view *view = wl_container_of(listener, view, request_resize);
	struct wlr_xwayland_resize_event *event = data;
	begin_interactive_xwayland(view, RESONANCE_CURSOR_RESIZE, event->edges);
}

/* Authoritative teardown signal for view->scene_tree: fires exactly when
 * wlroots frees the scene subsurface tree, regardless of which path
 * triggered it (dissociate, surface destruction, etc). Without this,
 * view->scene_tree/border_node could still point at freed memory in cases
 * that don't go through xwayland_view_dissociate's manual null-out. */
static void xwayland_view_scene_tree_destroy(struct wl_listener *listener, void *data) {
	struct resonance_xwayland_view *view = wl_container_of(listener, view, scene_tree_destroy);
	wl_list_remove(&view->scene_tree_destroy.link);
	wl_list_init(&view->scene_tree_destroy.link);
	view->scene_tree = NULL;
	view->border_node = NULL;
}

static void xwayland_view_associate(struct wl_listener *listener, void *data) {
	struct resonance_xwayland_view *view = wl_container_of(listener, view, associate);

	struct wlr_scene_tree *parent_tree = view->server->floor_trees[RESONANCE_FLOOR_GROUND];
	view->floor = RESONANCE_FLOOR_GROUND;
	if (view->xwayland_surface->parent) {
		struct resonance_xwayland_view *pview;
		wl_list_for_each(pview, &view->server->xwayland_views, link) {
			if (pview->xwayland_surface == view->xwayland_surface->parent) {
				if (pview->scene_tree) {
					parent_tree = pview->scene_tree;
					view->floor = pview->floor;
				}
				break;
			}
		}
	} else if (view->xwayland_surface->override_redirect) {
		/* Parentless override-redirect popup (menu/tooltip/dropdown): keep it
		 * above every floor. Otherwise a parent window promoted to a higher
		 * floor buries its own popup - the ground-floor menu renders behind
		 * the whole upper floor and never appears (e.g. Steam submenus). */
		parent_tree = view->server->xwayland_popup_tree;
	}

	view->scene_tree = wlr_scene_subsurface_tree_create(
		parent_tree, view->xwayland_surface->surface);
	view->scene_tree->node.data = view;
	view->scene_tree_destroy.notify = xwayland_view_scene_tree_destroy;
	wl_signal_add(&view->scene_tree->node.events.destroy, &view->scene_tree_destroy);
	if (!view->xwayland_surface->override_redirect) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	}

	view->map.notify = xwayland_view_map;
	wl_signal_add(&view->xwayland_surface->surface->events.map, &view->map);
	view->unmap.notify = xwayland_view_unmap;
	wl_signal_add(&view->xwayland_surface->surface->events.unmap, &view->unmap);
	view->commit.notify = xwayland_view_commit;
	wl_signal_add(&view->xwayland_surface->surface->events.commit, &view->commit);

	view->request_configure.notify = xwayland_view_request_configure;
	wl_signal_add(&view->xwayland_surface->events.request_configure, &view->request_configure);
	view->request_move.notify = xwayland_view_request_move;
	wl_signal_add(&view->xwayland_surface->events.request_move, &view->request_move);
	view->request_resize.notify = xwayland_view_request_resize;
	wl_signal_add(&view->xwayland_surface->events.request_resize, &view->request_resize);
	view->request_fullscreen.notify = xwayland_view_request_fullscreen;
	wl_signal_add(&view->xwayland_surface->events.request_fullscreen, &view->request_fullscreen);

	if (view->xwayland_surface->surface->mapped) {
		xwayland_view_map(&view->map, NULL);
	}
}

static void xwayland_view_dissociate(struct wl_listener *listener, void *data) {
	struct resonance_xwayland_view *view = wl_container_of(listener, view, dissociate);

	// If the view is still in the xwayland_views list, unmap it before removing event listeners
	if (!wl_list_empty(&view->link)) {
		xwayland_view_unmap(&view->unmap, NULL);
	}

	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->request_configure.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->request_fullscreen.link);

	/* Do NOT call wlr_scene_node_destroy here. The scene subsurface tree was
	 * created via wlr_scene_subsurface_tree_create, which registers an addon on
	 * the wl_surface. When the surface is destroyed, wlroots runs wlr_addon_set_finish
	 * which emits the dissociate signal while mid-destruction — calling destroy
	 * again here double-frees and crashes. Just null our references; wlroots owns
	 * the cleanup.
	 *
	 * Also defensively unlink scene_tree_destroy here: if the tree's own
	 * destroy signal hasn't fired yet by the time a re-associate creates a
	 * new scene_tree, re-adding the same listener to the new tree while
	 * still linked to the old one's signal list would corrupt both lists. */
	if (view->scene_tree) {
		wl_list_remove(&view->scene_tree_destroy.link);
		wl_list_init(&view->scene_tree_destroy.link);
	}
	view->scene_tree = NULL;
	view->border_node = NULL;
}

static void xwayland_view_request_activate(struct wl_listener *listener, void *data) {
	struct resonance_xwayland_view *view = wl_container_of(listener, view, request_activate);
	if (view->xwayland_surface->override_redirect) {
		return;
	}
	focus_xwayland_view(view);
}

static void xwayland_view_destroy(struct wl_listener *listener, void *data) {
	struct resonance_xwayland_view *view = wl_container_of(listener, view, destroy);

	if (view->server->grabbed_xwayland_view == view) {
		reset_cursor_mode(view->server);
	}

	if ((struct resonance_toplevel *)view == view->server->hovered_toplevel) {
		const char *old_app_id = get_toplevel_app_id((struct resonance_toplevel *)view);
		if (old_app_id) {
			char buf[320];
			snprintf(buf, sizeof(buf), "hover-leave:%s\n", old_app_id);
			ipc_send(view->server, buf);
		}
		view->server->hovered_toplevel = NULL;
	}

	wl_list_remove(&view->associate.link);
	wl_list_remove(&view->dissociate.link);
	wl_list_remove(&view->destroy.link);
	wl_list_remove(&view->request_activate.link);

	if (view->border_buffer) {
		wlr_buffer_drop(view->border_buffer);
	}

	wl_list_remove(&view->link);
	free(view);
}

static void server_new_xwayland_surface(struct wl_listener *listener, void *data) {
	struct resonance_server *server = wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xwayland_surface = data;

	struct resonance_xwayland_view *view = calloc(1, sizeof(*view));
	if (!view) {
		wlr_log(WLR_ERROR, "Failed to allocate XWayland view structure!");
		return;
	}
	view->is_xwayland = true;
	view->server = server;
	view->xwayland_surface = xwayland_surface;
	wl_list_init(&view->link);

	view->associate.notify = xwayland_view_associate;
	wl_signal_add(&xwayland_surface->events.associate, &view->associate);
	view->dissociate.notify = xwayland_view_dissociate;
	wl_signal_add(&xwayland_surface->events.dissociate, &view->dissociate);
	view->destroy.notify = xwayland_view_destroy;
	wl_signal_add(&xwayland_surface->events.destroy, &view->destroy);
	view->request_activate.notify = xwayland_view_request_activate;
	wl_signal_add(&xwayland_surface->events.request_activate, &view->request_activate);
}



int main(int argc, char *argv[]) {
	signal(SIGPIPE, SIG_IGN);
	wlr_log_init(WLR_INFO, NULL);
	char *startup_cmd = NULL;

	int c;
	while ((c = getopt(argc, argv, "s:h")) != -1) {
		switch (c) {
		case 's':
			startup_cmd = optarg;
			break;
		default:
			printf("Usage: %s [-s startup command]\n", argv[0]);
			return 0;
		}
	}
	if (optind < argc) {
		printf("Usage: %s [-s startup command]\n", argv[0]);
		return 0;
	}

	struct resonance_server server = {0};
	server.ipc_fd = -1;
	server.next_window_id = 1;
	config_load_keybindings(&server);
	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, managing Wayland globals, and so on. */
	server.wl_display = wl_display_create();
	if (server.wl_display == NULL) {
		wlr_log(WLR_ERROR, "failed to create wl_display");
		return 1;
	}
	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.wl_display), &server.session);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_backend");
		return 1;
	}
	if (server.session) {
		server.session_active.notify = handle_session_active;
		wl_signal_add(&server.session->events.active, &server.session_active);
	}

	/* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
	 * can also specify a renderer using the WLR_RENDERER env var.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_renderer");
		return 1;
	}

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);

	/* Autocreates an allocator for us.
	 * The allocator is the bridge between the renderer and the backend. It
	 * handles the buffer creation, allowing wlroots to render onto the
	 * screen */
	server.allocator = wlr_allocator_autocreate(server.backend,
		server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_allocator");
		return 1;
	}

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces, the subcompositor allows to
	 * assign the role of subsurfaces to surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the handling of the request_set_selection event below.*/
	server.compositor = wlr_compositor_create(server.wl_display, 5, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);
	server.presentation = wlr_presentation_create(server.wl_display, server.backend, 1);

	/* Creates an output layout, which a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	server.output_layout = wlr_output_layout_create(server.wl_display);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	/* Create a scene graph. This is a wlroots abstraction that handles all
	 * rendering and damage tracking. All the compositor author needs to do
	 * is add things that should be rendered to the scene graph at the proper
	 * positions and then call wlr_scene_output_commit() to render a frame if
	 * necessary.
	 */
	server.scene = wlr_scene_create();
	struct wlr_linux_dmabuf_v1 *linux_dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(
		server.wl_display, 5, server.renderer);
	wlr_scene_set_linux_dmabuf_v1(server.scene, linux_dmabuf);
	server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);
	wlr_screencopy_manager_v1_create(server.wl_display);
	wlr_ext_output_image_capture_source_manager_v1_create(server.wl_display, 1);
	struct wlr_ext_image_copy_capture_manager_v1 *capture_mgr =
		wlr_ext_image_copy_capture_manager_v1_create(server.wl_display, 1);
	server.new_capture_session.notify = new_capture_session;
	wl_signal_add(&capture_mgr->events.new_session, &server.new_capture_session);
	server.foreign_toplevel_manager =
		wlr_foreign_toplevel_manager_v1_create(server.wl_display);

	server.layer_trees[0] = wlr_scene_tree_create(&server.scene->tree); /* BACKGROUND */
	server.layer_trees[1] = wlr_scene_tree_create(&server.scene->tree); /* BOTTOM */
	server.windows_tree   = wlr_scene_tree_create(&server.scene->tree); /* normal windows */
	/* Floors, in fixed z-order within windows_tree: basement sits behind
	 * ground sits behind first sits behind second floor. New toplevels
	 * start on ground floor (see server_new_xdg_toplevel / xwayland_view
	 * creation) until placement.py's place: command says otherwise. */
	server.floor_trees[RESONANCE_FLOOR_BASEMENT] = wlr_scene_tree_create(server.windows_tree);
	server.floor_trees[RESONANCE_FLOOR_GROUND]   = wlr_scene_tree_create(server.windows_tree);
	server.floor_trees[RESONANCE_FLOOR_FIRST]    = wlr_scene_tree_create(server.windows_tree);
	server.floor_trees[RESONANCE_FLOOR_SECOND]   = wlr_scene_tree_create(server.windows_tree);
	/* Parentless override-redirect popups sit above all floors (but still
	 * below layer-shell TOP/OVERLAY) so promoted windows can't bury them. */
	server.xwayland_popup_tree = wlr_scene_tree_create(server.windows_tree);
	server.layer_trees[2] = wlr_scene_tree_create(&server.scene->tree); /* TOP */
	server.layer_trees[3] = wlr_scene_tree_create(&server.scene->tree); /* OVERLAY */

	/* Set up xdg-shell version 3. The xdg-shell is a Wayland protocol which is
	 * used for application windows. For more detail on shells, refer to
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html.
	 */
	wl_list_init(&server.toplevels);
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	server.xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server.wl_display);
	server.new_toplevel_decoration.notify = handle_new_toplevel_decoration;
	wl_signal_add(&server.xdg_decoration_manager->events.new_toplevel_decoration, &server.new_toplevel_decoration);




	wlr_xdg_output_manager_v1_create(server.wl_display, server.output_layout);

	server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
	server.new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). */
	setenv("XCURSOR_THEME", "Resonance", 1);
	setenv("XCURSOR_SIZE", "24", 1);
	server.cursor_mgr = wlr_xcursor_manager_create("Resonance", 24);
	if (server.cursor_mgr == NULL) {
		wlr_log(WLR_ERROR, "failed to create wlr_xcursor_manager");
		return 1;
	}
	wlr_xcursor_manager_load(server.cursor_mgr, 1);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html.
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	server.cursor_mode = RESONANCE_CURSOR_PASSTHROUGH;
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
			&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
			&server.request_cursor);
	server.pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&server.seat->pointer_state.events.focus_change,
			&server.pointer_focus_change);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
			&server.request_set_selection);
	server.request_start_drag.notify = seat_request_start_drag;
	wl_signal_add(&server.seat->events.request_start_drag,
			&server.request_start_drag);
	server.start_drag.notify = seat_start_drag;
	wl_signal_add(&server.seat->events.start_drag,
			&server.start_drag);

	server.pointer_constraints = wlr_pointer_constraints_v1_create(server.wl_display);
	server.active_constraint = NULL;
	server.new_pointer_constraint.notify = handle_new_pointer_constraint;
	wl_signal_add(&server.pointer_constraints->events.new_constraint,
			&server.new_pointer_constraint);
	server.relative_pointer_manager = wlr_relative_pointer_manager_v1_create(server.wl_display);

	wl_list_init(&server.xwayland_views);
	server.xwayland = wlr_xwayland_create(server.wl_display, server.compositor, false);
	if (server.xwayland) {
		wlr_xwayland_set_seat(server.xwayland, server.seat);
		server.new_xwayland_surface.notify = server_new_xwayland_surface;
		wl_signal_add(&server.xwayland->events.new_surface, &server.new_xwayland_surface);
		setenv("DISPLAY", server.xwayland->display_name, true);
		wlr_log(WLR_INFO, "XWayland started on DISPLAY=%s", server.xwayland->display_name);
	} else {
		wlr_log(WLR_ERROR, "XWayland failed to start — X11 apps will not work");
	}

	/* Add a Unix socket to the Wayland display. */
	server.socket = wl_display_add_socket_auto(server.wl_display);
	if (!server.socket) {
		wlr_backend_destroy(server.backend);
		return 1;
	}

	bootintro_init(&server.boot,
		"/path/to/Resonance/wallpaperblur.jpg");

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(server.backend)) {
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.wl_display);
		return 1;
	}

	/* Set the WAYLAND_DISPLAY environment variable to our socket and run the
	 * startup command if requested. */
	setenv("WAYLAND_DISPLAY", server.socket, true);

	/* Connect to the brain daemon now that DISPLAY/WAYLAND_DISPLAY are known —
	 * ipc_connect() pushes them over IPC immediately, and will do so again on
	 * every future reconnect (see ipc_send_env_vars). */
	ipc_connect(&server);

	if (startup_cmd) {
		pid_t pid = fork();
		if (pid == 0) {
			pid_t grandchild = fork();
			if (grandchild == 0) {
				signal(SIGCHLD, SIG_DFL);
				execl("/bin/sh", "/bin/sh", "-c", startup_cmd, (void *)NULL);
				_exit(1);
			}
			_exit(0);
		}
		if (pid > 0) {
			waitpid(pid, NULL, 0);
		}
	}
	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wlr_log(WLR_INFO, "Running Wayland compositor on WAYLAND_DISPLAY=%s",
			server.socket);
	wl_display_run(server.wl_display);


	/* Once wl_display_run returns, we destroy all clients then shut down the
	 * server. */
	wl_display_destroy_clients(server.wl_display);

	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	wl_list_remove(&server.new_toplevel_decoration.link);

	if (server.xwayland) {
		wl_list_remove(&server.new_xwayland_surface.link);
		wlr_xwayland_destroy(server.xwayland);
	}

	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);

	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.pointer_focus_change.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.request_start_drag.link);
	wl_list_remove(&server.start_drag.link);
	if (server.drag_icon_destroy_connected) {
		wl_list_remove(&server.drag_icon_destroy.link);
	}
	wl_list_remove(&server.new_pointer_constraint.link);
	if (server.active_constraint) {
		wl_list_remove(&server.active_constraint_destroy.link);
	}

	wl_list_remove(&server.new_output.link);

	if (server.session) {
		wl_list_remove(&server.session_active.link);
	}

	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	ipc_disconnect(&server);
	wl_display_destroy(server.wl_display);

	for (size_t i = 0; i < server.keybindings_len; i++) {
		free(server.keybindings[i].name);
		free(server.keybindings[i].action);
	}
	free(server.keybindings);

	return 0;
}
