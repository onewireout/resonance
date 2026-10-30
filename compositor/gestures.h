#ifndef GESTURES_H
#define GESTURES_H

/* Super+flick gesture recognition for the Resonance compositor.
 *
 * Extracted from resonance.c (see Documentation/Resonance-Refactor-Plan.md).
 * A fixed policy layer sitting on top of pointer motion events — the
 * core calls one entry point from process_cursor_motion. Gesture
 * actions are dispatched as IPC messages (gesture:swipe-*) so the
 * hub/brain.py can map them to commands via resonance.toml. */

#include <stdint.h>

struct resonance_server;

void gesture_handle_motion(struct resonance_server *server, uint32_t time);

#endif /* GESTURES_H */
