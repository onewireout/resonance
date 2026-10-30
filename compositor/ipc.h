#ifndef IPC_H
#define IPC_H

/* IPC transport for the Resonance compositor.
 *
 * Extracted from resonance.c (see Documentation/Resonance-Refactor-Plan.md).
 * This module owns the socket transport, message framing, and the
 * outbound send helpers. Inbound command handlers (place, fullscreen)
 * stay in the core because they drive the scene graph directly;
 * ipc_read_cb dispatches to them via core_handle_ipc_command()
 * declared in resonance.h. */

#include <stdbool.h>
#include <stdint.h>

struct resonance_server;
struct resonance_toplevel;
struct resonance_xwayland_view;

void ipc_connect(struct resonance_server *server);
void ipc_disconnect(struct resonance_server *server);
void ipc_send(struct resonance_server *server, const char *msg);

void ipc_send_window_geo(struct resonance_server *server, const char *type,
		const char *title, const char *app_id,
		int x, int y, int w, int h, uint32_t id, bool fullscreen);
void ipc_send_window_unmap(struct resonance_server *server, uint32_t id);

void ipc_find_window_by_id(struct resonance_server *server, uint32_t id,
		struct resonance_toplevel **out_toplevel,
		struct resonance_xwayland_view **out_view);

#endif /* IPC_H */
