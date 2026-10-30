#define _GNU_SOURCE

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "resonance.h"
#include "resonance_buffer.h"
#include "decorations.h"

struct resonance_buffer {
	struct wlr_buffer base;
	int fd;
	void *data;
	size_t size;
	int width, height;
	size_t stride;
	uint32_t format;
};

static void resonance_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct resonance_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	munmap(buffer->data, buffer->size);
	close(buffer->fd);
	free(buffer);
}

static bool resonance_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct resonance_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	*data = buffer->data;
	*format = buffer->format;
	*stride = buffer->stride;
	return true;
}

static void resonance_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	// No-op
}

static bool resonance_buffer_get_shm(struct wlr_buffer *wlr_buffer,
		struct wlr_shm_attributes *attribs) {
	struct resonance_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	attribs->fd = buffer->fd;
	attribs->format = buffer->format;
	attribs->width = buffer->width;
	attribs->height = buffer->height;
	attribs->stride = buffer->stride;
	attribs->offset = 0;
	return true;
}

static const struct wlr_buffer_impl resonance_buffer_impl = {
	.destroy = resonance_buffer_destroy,
	.begin_data_ptr_access = resonance_buffer_begin_data_ptr_access,
	.end_data_ptr_access = resonance_buffer_end_data_ptr_access,
	.get_shm = resonance_buffer_get_shm,
};

struct wlr_buffer *resonance_buffer_create(int width, int height) {
	int stride = width * sizeof(uint32_t);
	size_t size = stride * height;

	int fd = memfd_create("resonance-border", MFD_CLOEXEC);
	if (fd < 0) return NULL;

	if (ftruncate(fd, size) < 0) {
		close(fd);
		return NULL;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	struct resonance_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer) {
		munmap(data, size);
		close(fd);
		return NULL;
	}

	buffer->fd = fd;
	buffer->data = data;
	buffer->size = size;
	buffer->width = width;
	buffer->height = height;
	buffer->stride = stride;
	buffer->format = DRM_FORMAT_ARGB8888;

	wlr_buffer_init(&buffer->base, &resonance_buffer_impl, width, height);
	return &buffer->base;
}

void *resonance_buffer_data(struct wlr_buffer *buf) {
	struct resonance_buffer *buffer = wl_container_of(buf, buffer, base);
	return buffer->data;
}

size_t resonance_buffer_stride(struct wlr_buffer *buf) {
	struct resonance_buffer *buffer = wl_container_of(buf, buffer, base);
	return buffer->stride;
}

#define BORDER_COLOR_A_R 70
#define BORDER_COLOR_A_G 120
#define BORDER_COLOR_A_B 123

#define BORDER_COLOR_B_R 212
#define BORDER_COLOR_B_G 185
#define BORDER_COLOR_B_B 168

/* BORDER_WIDTH lives in decorations.h - the core's frame-geometry math
 * (resonance_*_absolute_box, get_floor_maximized_box, floor-maximize) needs it
 * too, to offset windows by the frame this module draws around them. */
#define BORDER_RADIUS 12

/* Dialog glass background — gradient border ring + dark glass fill, matching the
 * Resonance window aesthetic. Used for CSD dialog toplevels whose own surfaces
 * are transparent (e.g. Ghostty close-confirm, GTK4/libadwaita dialogs). */
#define DIALOG_GLASS_R  18
#define DIALOG_GLASS_G  18
#define DIALOG_GLASS_B  24
#define DIALOG_GLASS_A  199   /* ~0.78 * 255 */
#define DIALOG_BORDER   4

void get_window_radius(const char *app_id, const char *class, int *radius_top, int *radius_bottom) {
	// Default values
	*radius_top = 12;
	*radius_bottom = 12;

	const char *name = NULL;
	if (app_id && strlen(app_id) > 0) {
		name = app_id;
	} else if (class && strlen(class) > 0) {
		name = class;
	}

	if (!name) {
		return;
	}

	// Create a lowercase copy for case-insensitive comparison
	char *lower = strdup(name);
	if (lower) {
		for (int i = 0; lower[i]; i++) {
			lower[i] = tolower((unsigned char)lower[i]);
		}

		if (strstr(lower, "steam") || strstr(lower, "gamescope") || strstr(lower, "kitty") || strstr(lower, "alacritty")) {
			// Fully square windows
			*radius_top = 0;
			*radius_bottom = 0;
		}

		free(lower);
	}
}

void update_window_border(struct resonance_server *server,
		struct wlr_scene_tree *scene_tree,
		struct wlr_scene_buffer **border_node_ptr,
		struct wlr_buffer **border_buffer_ptr,
		int width, int height,
		int radius_top, int radius_bottom,
		int offset_x, int offset_y) {
	if (width <= 0 || height <= 0) return;

	int bw = BORDER_WIDTH;
	int border_w = width + 2 * bw;
	int border_h = height + 2 * bw;

	struct wlr_buffer *new_buffer = resonance_buffer_create(border_w, border_h);
	if (!new_buffer) return;

	struct resonance_buffer *res_buf = wl_container_of(new_buffer, res_buf, base);
	uint32_t *pixels = res_buf->data;

	int rt = radius_top;
	int rb = radius_bottom;
	int max_r = (border_w < border_h ? border_w : border_h) / 2;
	if (rt > max_r) rt = max_r;
	if (rb > max_r) rb = max_r;

	for (int y = 0; y < border_h; y++) {
		float t = (float)y / (float)(border_h > 1 ? border_h - 1 : 1);
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;

		uint8_t r = (uint8_t)(BORDER_COLOR_A_R * (1.0f - t) + BORDER_COLOR_B_R * t);
		uint8_t g = (uint8_t)(BORDER_COLOR_A_G * (1.0f - t) + BORDER_COLOR_B_G * t);
		uint8_t b = (uint8_t)(BORDER_COLOR_A_B * (1.0f - t) + BORDER_COLOR_B_B * t);

		for (int x = 0; x < border_w; x++) {
			uint8_t a = 255;

			/* Distances are measured from the arc centre to the PIXEL
			 * CENTRE, which is why every term carries a half pixel. With
			 * integer indices the outermost row and column of each corner
			 * always land outside the radius and get dropped, leaving the
			 * border a pixel thinner around the corners than along the
			 * straight edges. */
			float dx = 0.0f, dy = 0.0f;
			bool in_corner = false;
			int r_curr = 0;

			if (y < rt && x < rt) { // Top-Left
				r_curr = rt;
				dx = (r_curr - 0.5f) - x;
				dy = (r_curr - 0.5f) - y;
				in_corner = true;
			} else if (y < rt && x >= border_w - rt) { // Top-Right
				r_curr = rt;
				dx = x - (border_w - r_curr - 0.5f);
				dy = (r_curr - 0.5f) - y;
				in_corner = true;
			} else if (y >= border_h - rb && x < rb) { // Bottom-Left
				r_curr = rb;
				dx = (r_curr - 0.5f) - x;
				dy = y - (border_h - r_curr - 0.5f);
				in_corner = true;
			} else if (y >= border_h - rb && x >= border_w - rb) { // Bottom-Right
				r_curr = rb;
				dx = x - (border_w - r_curr - 0.5f);
				dy = y - (border_h - r_curr - 0.5f);
				in_corner = true;
			}

			if (in_corner) {
				/* Coverage, not a threshold: a pixel whose centre sits
				 * half a pixel inside the radius is fully covered, which
				 * is what makes the arc meet the straight edge at the
				 * same thickness. */
				float dist = sqrtf(dx * dx + dy * dy);
				float cover = (float)r_curr - dist + 0.5f;
				if (cover <= 0.0f) {
					a = 0;
				} else if (cover < 1.0f) {
					a = (uint8_t)(255.0f * cover);
				}
			}

			uint8_t pr = (uint8_t)((r * a) / 255);
			uint8_t pg = (uint8_t)((g * a) / 255);
			uint8_t pb = (uint8_t)((b * a) / 255);

			pixels[y * border_w + x] = ((uint32_t)a << 24) | ((uint32_t)pr << 16) | ((uint32_t)pg << 8) | (uint32_t)pb;
		}
	}

	if (*border_node_ptr) {
		wlr_scene_node_destroy(&(*border_node_ptr)->node);
		*border_node_ptr = NULL;
	}

	if (*border_buffer_ptr) {
		wlr_buffer_drop(*border_buffer_ptr);
		*border_buffer_ptr = NULL;
	}

	*border_buffer_ptr = new_buffer;
	*border_node_ptr = wlr_scene_buffer_create(scene_tree, new_buffer);

	if (*border_node_ptr) {
		wlr_scene_node_set_position(&(*border_node_ptr)->node, offset_x - bw, offset_y - bw);
		wlr_scene_node_lower_to_bottom(&(*border_node_ptr)->node);
	}
}

/*
 * Draw a Resonance glass frame behind a dialog/transient toplevel.
 *
 * Renders a gradient border ring (teal→shore, DIALOG_BORDER px wide) around a
 * dark semi-transparent glass fill — the same aesthetic as the regular window
 * border but as a self-contained background panel.
 *
 * geo_x / geo_y are the xdg_surface geometry offsets (shadow margin).  The
 * node is placed at (geo_x, geo_y) so the glass sits exactly under the
 * client's visible content region, not its shadow padding.
 */
void update_dialog_background(struct wlr_scene_tree *scene_tree,
		struct wlr_scene_buffer **bg_node_ptr,
		struct wlr_buffer **bg_buffer_ptr,
		int width, int height,
		int radius_top, int radius_bottom,
		int geo_x, int geo_y) {
	if (width <= 0 || height <= 0) return;

	int bw = DIALOG_BORDER;
	int total_w = width  + 2 * bw;
	int total_h = height + 2 * bw;

	struct wlr_buffer *new_buffer = resonance_buffer_create(total_w, total_h);
	if (!new_buffer) return;

	struct resonance_buffer *res_buf = wl_container_of(new_buffer, res_buf, base);
	uint32_t *pixels = res_buf->data;

	int rt = radius_top  + bw;
	int rb = radius_bottom + bw;
	int max_r = (total_w < total_h ? total_w : total_h) / 2;
	if (rt > max_r) rt = max_r;
	if (rb > max_r) rb = max_r;

	for (int y = 0; y < total_h; y++) {
		/* Vertical gradient progress for the border colour */
		float t = (float)y / (float)(total_h > 1 ? total_h - 1 : 1);
		if (t < 0.0f) t = 0.0f;
		if (t > 1.0f) t = 1.0f;

		uint8_t border_r = (uint8_t)(BORDER_COLOR_A_R * (1.0f - t) + BORDER_COLOR_B_R * t);
		uint8_t border_g = (uint8_t)(BORDER_COLOR_A_G * (1.0f - t) + BORDER_COLOR_B_G * t);
		uint8_t border_b = (uint8_t)(BORDER_COLOR_A_B * (1.0f - t) + BORDER_COLOR_B_B * t);

		for (int x = 0; x < total_w; x++) {
			/* --- corner rounding (applied to the outer frame) --- */
			uint8_t outer_a = 255;
			/* Pixel centres and coverage, as in update_window_border -
			 * see the comment there for why the half pixels matter. */
			float dx = 0.0f, dy = 0.0f;
			bool in_corner = false;
			int r_curr = 0;

			if (y < rt && x < rt) {
				r_curr = rt; dx = (r_curr - 0.5f) - x; dy = (r_curr - 0.5f) - y; in_corner = true;
			} else if (y < rt && x >= total_w - rt) {
				r_curr = rt; dx = x - (total_w - r_curr - 0.5f); dy = (r_curr - 0.5f) - y; in_corner = true;
			} else if (y >= total_h - rb && x < rb) {
				r_curr = rb; dx = (r_curr - 0.5f) - x; dy = y - (total_h - r_curr - 0.5f); in_corner = true;
			} else if (y >= total_h - rb && x >= total_w - rb) {
				r_curr = rb; dx = x - (total_w - r_curr - 0.5f); dy = y - (total_h - r_curr - 0.5f); in_corner = true;
			}

			if (in_corner) {
				float dist = sqrtf(dx * dx + dy * dy);
				float cover = (float)r_curr - dist + 0.5f;
				if (cover <= 0.0f) {
					outer_a = 0;
				} else if (cover < 1.0f) {
					outer_a = (uint8_t)(255.0f * cover);
				}
			}

			if (outer_a == 0) {
				pixels[y * total_w + x] = 0;
				continue;
			}

			/* --- decide: border ring vs glass interior --- */
			bool in_border = (x < bw || x >= total_w - bw ||
			                  y < bw || y >= total_h - bw);

			uint8_t pr, pg, pb, pa;
			if (in_border) {
				/* Gradient border — fully opaque, pre-multiplied */
				pa = outer_a;
				pr = (uint8_t)((border_r * (uint32_t)outer_a) / 255);
				pg = (uint8_t)((border_g * (uint32_t)outer_a) / 255);
				pb = (uint8_t)((border_b * (uint32_t)outer_a) / 255);
			} else {
				/* Dark glass fill — semi-transparent, pre-multiplied */
				uint32_t eff_a = ((uint32_t)DIALOG_GLASS_A * outer_a) / 255;
				pa = (uint8_t)eff_a;
				pr = (uint8_t)((DIALOG_GLASS_R * eff_a) / 255);
				pg = (uint8_t)((DIALOG_GLASS_G * eff_a) / 255);
				pb = (uint8_t)((DIALOG_GLASS_B * eff_a) / 255);
			}

			pixels[y * total_w + x] = ((uint32_t)pa << 24) | ((uint32_t)pr << 16)
			                        | ((uint32_t)pg <<  8) | (uint32_t)pb;
		}
	}

	if (*bg_node_ptr) {
		wlr_scene_node_destroy(&(*bg_node_ptr)->node);
		*bg_node_ptr = NULL;
	}
	if (*bg_buffer_ptr) {
		wlr_buffer_drop(*bg_buffer_ptr);
		*bg_buffer_ptr = NULL;
	}

	*bg_buffer_ptr = new_buffer;
	*bg_node_ptr = wlr_scene_buffer_create(scene_tree, new_buffer);

	if (*bg_node_ptr) {
		/* Offset by geometry inset minus border width so the border ring sits
		 * flush against the client's visible content edge. */
		wlr_scene_node_set_position(&(*bg_node_ptr)->node,
			geo_x - bw, geo_y - bw);
		wlr_scene_node_lower_to_bottom(&(*bg_node_ptr)->node);
	}
}

static void decoration_handle_destroy(struct wl_listener *listener, void *data) {
	struct resonance_decoration *decor = wl_container_of(listener, decor, destroy);
	wl_list_remove(&decor->destroy.link);
	wl_list_remove(&decor->surface_commit.link);
	free(decor);
}

static void decoration_handle_commit(struct wl_listener *listener, void *data) {
	struct resonance_decoration *decor = wl_container_of(listener, decor, surface_commit);
	if (decor->wlr_decoration->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(decor->wlr_decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		wl_list_remove(&decor->surface_commit.link);
		wl_list_init(&decor->surface_commit.link);
	}
}

void handle_new_toplevel_decoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
	if (decoration->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		return;
	}

	struct resonance_decoration *decor = calloc(1, sizeof(*decor));
	if (!decor) {
		return;
	}
	decor->wlr_decoration = decoration;

	decor->destroy.notify = decoration_handle_destroy;
	wl_signal_add(&decoration->events.destroy, &decor->destroy);

	decor->surface_commit.notify = decoration_handle_commit;
	wl_signal_add(&decoration->toplevel->base->surface->events.commit, &decor->surface_commit);
}

