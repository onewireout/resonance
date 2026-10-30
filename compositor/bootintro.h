#ifndef BOOTINTRO_H
#define BOOTINTRO_H

#include <stdbool.h>
#include <time.h>

struct wlr_buffer;
struct wlr_scene_tree;
struct wlr_scene_buffer;
struct wlr_scene_rect;
struct wlr_output;
struct wlr_output_layout;

#define BOOTINTRO_MAX_OVERLAYS 4

enum bootintro_phase {
	BOOTINTRO_SYNC_WAIT,
	BOOTINTRO_FADE_IN,
	BOOTINTRO_HOLD,
	BOOTINTRO_FADE_OUT,
	BOOTINTRO_DONE,
};

/* All state for the "Resonance" boot splash: fullscreen image fade-in/out
 * plus centered title/subtitle text, one overlay per connected output. */
struct bootintro_state {
	enum bootintro_phase phase;
	struct timespec phase_start;
	struct wlr_buffer *image;
	struct wlr_scene_buffer *overlays[BOOTINTRO_MAX_OVERLAYS];
	struct wlr_scene_rect *rects[BOOTINTRO_MAX_OVERLAYS];
	int overlay_count;
	struct wlr_scene_buffer *text;
	struct wlr_scene_buffer *text_shadow;
	struct wlr_scene_buffer *subtitle;
	struct wlr_scene_buffer *subtitle_shadow;
};

/* Loads the splash image and arms the state machine. On failure to load,
 * st->phase is left at BOOTINTRO_DONE so callers never need to branch on
 * whether the intro is actually running. */
void bootintro_init(struct bootintro_state *st, const char *image_path);

/* Called from the new-output handler. If the intro is still running, adds a
 * fullscreen overlay (and, on the first output, the centered title/subtitle)
 * to overlay_tree for the newly connected output. */
void bootintro_handle_new_output(struct bootintro_state *st,
	struct wlr_scene_tree *overlay_tree, struct wlr_output_layout *layout,
	struct wlr_output *wlr_output);

/* Called once per output frame with the current monotonic time. Advances
 * the fade state machine and applies opacity to the overlay nodes. Returns
 * true if another frame should be scheduled to continue the animation. */
bool bootintro_frame_tick(struct bootintro_state *st, struct timespec now);

#endif
