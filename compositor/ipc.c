#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/util/log.h>

#include "resonance.h"
#include "ipc.h"

void ipc_disconnect(struct resonance_server *server) {
	if (server->ipc_event_source) {
		wl_event_source_remove(server->ipc_event_source);
		server->ipc_event_source = NULL;
	}
	if (server->ipc_fd >= 0) {
		close(server->ipc_fd);
		server->ipc_fd = -1;
	}
	server->ipc_inbuf_len = 0;
	server->ipc_outbuf_len = 0;
}

/* Pushes the display names brain.py needs for spawning X11/Wayland clients
 * (e.g. via Tide/Sill). brain.py starts before Xwayland/the Wayland socket
 * exist, so it has no other way to learn these — and since it can restart
 * independently of resonance (its own watchdog), this must be resent on every
 * (re)connect, not just once at boot, or a restarted brain.py is left
 * without DISPLAY/WAYLAND_DISPLAY until the whole session reboots. */
static void ipc_send_env_vars(struct resonance_server *server) {
	if (server->xwayland && server->xwayland->display_name) {
		char env_msg[256];
		snprintf(env_msg, sizeof(env_msg), "env:DISPLAY=%s\n", server->xwayland->display_name);
		ipc_send(server, env_msg);
	}
	if (server->socket) {
		char env_msg[256];
		snprintf(env_msg, sizeof(env_msg), "env:WAYLAND_DISPLAY=%s\n", server->socket);
		ipc_send(server, env_msg);
	}
}

static int ipc_read_cb(int fd, uint32_t mask, void *data);

void ipc_connect(struct resonance_server *server) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return;
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, "/tmp/resonance.sock", sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return;
	}
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags != -1) {
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}
	server->ipc_fd = fd;
	server->ipc_event_source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->wl_display),
		fd, WL_EVENT_READABLE, ipc_read_cb, server);
	if (!server->ipc_event_source) {
		close(fd);
		server->ipc_fd = -1;
		return;
	}
	ipc_send_env_vars(server);
}

/* Flushes any buffered output data. Returns true if fully drained. */
static bool ipc_flush_outbuf(struct resonance_server *server) {
	while (server->ipc_outbuf_len > 0) {
		ssize_t ret = write(server->ipc_fd, server->ipc_outbuf,
				server->ipc_outbuf_len);
		if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return false;
			ipc_disconnect(server);
			return false;
		}
		memmove(server->ipc_outbuf, server->ipc_outbuf + ret,
				server->ipc_outbuf_len - (size_t)ret);
		server->ipc_outbuf_len -= (size_t)ret;
	}
	if (server->ipc_event_source)
		wl_event_source_fd_update(server->ipc_event_source, WL_EVENT_READABLE);
	return true;
}

static void ipc_buffer_msg(struct resonance_server *server, const char *data,
		size_t len) {
	size_t space = sizeof(server->ipc_outbuf) - server->ipc_outbuf_len;
	if (len > space)
		return;
	memcpy(server->ipc_outbuf + server->ipc_outbuf_len, data, len);
	server->ipc_outbuf_len += len;
	if (server->ipc_event_source)
		wl_event_source_fd_update(server->ipc_event_source,
				WL_EVENT_READABLE | WL_EVENT_WRITABLE);
}

void ipc_send(struct resonance_server *server, const char *msg) {
	if (server->ipc_fd < 0) ipc_connect(server);
	if (server->ipc_fd < 0) return;

	if (server->ipc_outbuf_len > 0 && !ipc_flush_outbuf(server)) {
		ipc_buffer_msg(server, msg, strlen(msg));
		return;
	}

	const char *p = msg;
	size_t remaining = strlen(msg);
	while (remaining > 0) {
		ssize_t ret = write(server->ipc_fd, p, remaining);
		if (ret < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				ipc_buffer_msg(server, p, remaining);
				return;
			}
			ipc_disconnect(server);
			return;
		}
		p += ret;
		remaining -= (size_t)ret;
	}
}

void ipc_find_window_by_id(struct resonance_server *server, uint32_t id,
		struct resonance_toplevel **out_toplevel, struct resonance_xwayland_view **out_view) {
	*out_toplevel = NULL;
	*out_view = NULL;
	struct resonance_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel->id == id) {
			*out_toplevel = toplevel;
			return;
		}
	}
	struct resonance_xwayland_view *view;
	wl_list_for_each(view, &server->xwayland_views, link) {
		if (view->id == id) {
			*out_view = view;
			return;
		}
	}
}

static int ipc_read_cb(int fd, uint32_t mask, void *data) {
	struct resonance_server *server = data;
	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		ipc_disconnect(server);
		return 0;
	}

	if (mask & WL_EVENT_WRITABLE)
		ipc_flush_outbuf(server);
	if (!(mask & WL_EVENT_READABLE))
		return 0;

	char buf[512];
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n <= 0) {
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return 0;
		}
		ipc_disconnect(server);
		return 0;
	}

	size_t space = sizeof(server->ipc_inbuf) - server->ipc_inbuf_len - 1;
	if ((size_t)n > space) {
		/* A line is too long for the buffer; drop what we have and resync
		 * on the next newline instead of growing unboundedly. */
		server->ipc_inbuf_len = 0;
		space = sizeof(server->ipc_inbuf) - 1;
	}
	memcpy(server->ipc_inbuf + server->ipc_inbuf_len, buf, (size_t)n);
	server->ipc_inbuf_len += (size_t)n;

	char *start = server->ipc_inbuf;
	size_t len = server->ipc_inbuf_len;
	size_t consumed = 0;
	for (size_t i = 0; i < len; i++) {
		if (start[i] == '\n') {
			start[i] = '\0';
			core_handle_ipc_command(server, start + consumed);
			consumed = i + 1;
		}
	}
	if (consumed > 0) {
		memmove(server->ipc_inbuf, server->ipc_inbuf + consumed, len - consumed);
		server->ipc_inbuf_len = len - consumed;
	}
	return 0;
}

static void ipc_clean_field(const char *in, char *out, size_t out_size) {
	const char *s = (in && *in) ? in : "unknown";
	snprintf(out, out_size, "%s", s);
	for (int i = 0; out[i]; i++) {
		if (out[i] == '\t' || out[i] == '\n' || out[i] == '\r') {
			out[i] = ' ';
		}
	}
}

/* app_id and title are sent as separate fields (not collapsed into one) so
 * consumers like placement memory can group windows by app_id — which is
 * stable across an app's windows (e.g. a splash screen and its main window
 * share app_id but not title) — while still having title for display. */
void ipc_send_window_geo(struct resonance_server *server, const char *type,
		const char *title, const char *app_id, int x, int y, int w, int h, uint32_t id,
		bool fullscreen) {
	struct resonance_toplevel *toplevel = NULL;
	struct resonance_xwayland_view *view = NULL;
	ipc_find_window_by_id(server, id, &toplevel, &view);
	bool floor_maximized = false;
	if (toplevel) {
		floor_maximized = toplevel->floor_maximized;
	} else if (view) {
		floor_maximized = view->floor_maximized;
	}

	char buf[768];
	char clean_app_id[256], clean_title[256];
	ipc_clean_field(app_id, clean_app_id, sizeof(clean_app_id));
	ipc_clean_field(title, clean_title, sizeof(clean_title));
	snprintf(buf, sizeof(buf), "window-%s:%s\t%s\t%d\t%d\t%d\t%d\t%u\t%d\t%d\n",
		type, clean_app_id, clean_title, x, y, w, h, id, fullscreen ? 1 : 0, floor_maximized ? 1 : 0);
	ipc_send(server, buf);
}

void ipc_send_window_unmap(struct resonance_server *server, uint32_t id) {
	char buf[64];
	snprintf(buf, sizeof(buf), "window-unmap:%u\n", id);
	ipc_send(server, buf);
}
