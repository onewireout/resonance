#define _GNU_SOURCE

#include <linux/input-event-codes.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland/xwayland.h>

#include "resonance.h"
#include "gestures.h"
#include "ipc.h"

static void inject_combo_without_super(struct resonance_server *server, uint32_t *keys, int num_keys) {
	struct wlr_seat *seat = server->seat;
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (!keyboard) return;

	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

	wlr_seat_set_keyboard(seat, keyboard);

	// 1. Send release for Super keys (just in case they are tracked by application raw state)
	wlr_seat_keyboard_notify_key(seat, ms, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED);
	wlr_seat_keyboard_notify_key(seat, ms, KEY_RIGHTMETA, WL_KEYBOARD_KEY_STATE_RELEASED);

	// 2. Temporarily notify seat of empty modifiers (clears Active Super Modifier in Wayland client state)
	struct wlr_keyboard_modifiers empty_mods = {0};
	wlr_seat_keyboard_notify_modifiers(seat, &empty_mods);

	// 3. Press combo keys
	for (int i = 0; i < num_keys; i++) {
		wlr_seat_keyboard_notify_key(seat, ms, keys[i], WL_KEYBOARD_KEY_STATE_PRESSED);
	}

	// 4. Release combo keys in reverse order
	for (int i = num_keys - 1; i >= 0; i--) {
		wlr_seat_keyboard_notify_key(seat, ms, keys[i], WL_KEYBOARD_KEY_STATE_RELEASED);
	}

	// 5. Restore Super modifiers in seat keyboard state
	wlr_seat_keyboard_notify_modifiers(seat, &keyboard->modifiers);

	// 6. Restore Super key pressed state
	wlr_seat_keyboard_notify_key(seat, ms, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void trigger_home_key(struct resonance_server *server) {
	uint32_t keys[] = { KEY_HOME };
	inject_combo_without_super(server, keys, 1);
}

static void trigger_back_key(struct resonance_server *server) {
	uint32_t keys[] = { KEY_BACK };
	inject_combo_without_super(server, keys, 1);
}

static bool is_firefox_focused(struct resonance_server *server) {
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *focused_surface = seat->keyboard_state.focused_surface;
	if (!focused_surface) return false;

	struct wlr_surface *root = wlr_surface_get_root_surface(focused_surface);
	if (!root) return false;

	struct wlr_xdg_toplevel *xdg_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(root);
	if (xdg_toplevel && xdg_toplevel->app_id) {
		if (strcasecmp(xdg_toplevel->app_id, "firefox") == 0 || strcasestr(xdg_toplevel->app_id, "firefox")) {
			return true;
		}
	}

	struct wlr_xwayland_surface *xsurface = wlr_xwayland_surface_try_from_wlr_surface(root);
	if (xsurface && xsurface->class) {
		if (strcasecmp(xsurface->class, "firefox") == 0 || strcasestr(xsurface->class, "firefox")) {
			return true;
		}
	}

	return false;
}

/* Super+flick gesture: accumulate displacement while Super is held
 * and trigger gesture actions when threshold is reached. Swipe-up
 * dispatches via IPC (gesture:swipe-up → resonance.toml [gestures]);
 * swipe-down/right inject keys directly through the seat because
 * resonance doesn't expose wlr_virtual_keyboard for external tools. */
void gesture_handle_motion(struct resonance_server *server, uint32_t time) {
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(server->seat);
	uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;
	double dy = server->cursor->y - server->gesture_last_y;
	double dx = server->cursor->x - server->gesture_last_x;
	double adx = dx < 0 ? -dx : dx;
	double ady = dy < 0 ? -dy : dy;

	if (mods & WLR_MODIFIER_LOGO) {
		uint32_t dt = time - server->gesture_last_time;
		if (dt > 80) {
			server->gesture_up_accum = 0;
			server->gesture_up_drift = 0;
			server->gesture_down_accum = 0;
			server->gesture_down_drift = 0;
			server->gesture_right_accum = 0;
			server->gesture_right_drift = 0;
		} else {
			// Swipe Up: moving up (dy < 0)
			if (dy < 0 && (dt == 0 || (-dy / (double)dt) > 6.0)) {
				server->gesture_up_accum += -dy;
				server->gesture_up_drift += adx;
			}
			// Swipe Down: moving down (dy > 0)
			if (dy > 0 && (dt == 0 || (dy / (double)dt) > 7.0)) {
				server->gesture_down_accum += dy;
				server->gesture_down_drift += adx;
			}
			// Swipe Right: moving right (dx > 0)
			if (dx > 0 && (dt == 0 || (dx / (double)dt) > 7.0)) {
				server->gesture_right_accum += dx;
				server->gesture_right_drift += ady;
			}

			if (dy > 20) {
				server->gesture_up_accum = 0;
				server->gesture_up_drift = 0;
			}
			if (dy < -20) {
				server->gesture_down_accum = 0;
				server->gesture_down_drift = 0;
			}
			if (dx < -20) {
				server->gesture_right_accum = 0;
				server->gesture_right_drift = 0;
			}
		}

		if (!server->gesture_fired) {
			// Swipe Up
			if (server->gesture_up_accum > 600 &&
				server->gesture_up_accum > server->gesture_up_drift * 1.5) {
				server->gesture_fired = true;
				wlr_log(WLR_INFO, "[DEBUG] Gesture Swipe Up fired");
				ipc_send(server, "gesture:swipe-up\n");
			}
			// Swipe Down
			else if (server->gesture_down_accum > 500 &&
				server->gesture_down_accum > server->gesture_down_drift * 1.5) {
				server->gesture_fired = true;
				wlr_log(WLR_INFO, "[DEBUG] Gesture Swipe Down fired -> injecting Home");
				trigger_home_key(server);
			}
			// Swipe Right
			else if (server->gesture_right_accum > 500 &&
				server->gesture_right_accum > server->gesture_right_drift * 1.5) {
				server->gesture_fired = true;
				wlr_log(WLR_INFO, "[DEBUG] Gesture Swipe Right fired -> checking Firefox");
				if (is_firefox_focused(server)) {
					wlr_log(WLR_INFO, "[DEBUG] Firefox focused -> injecting Back key");
					trigger_back_key(server);
				} else {
					wlr_log(WLR_INFO, "[DEBUG] Firefox not focused");
				}
			}
		}
	} else {
		server->gesture_up_accum = 0;
		server->gesture_up_drift = 0;
		server->gesture_down_accum = 0;
		server->gesture_down_drift = 0;
		server->gesture_right_accum = 0;
		server->gesture_right_drift = 0;
		server->gesture_fired = false;
	}

	server->gesture_last_x = server->cursor->x;
	server->gesture_last_y = server->cursor->y;
	server->gesture_last_time = time;
}
