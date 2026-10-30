#include "bootintro.h"
#include "resonance_buffer.h"

#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <jpeglib.h>
#include <pango/pangocairo.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#define BOOTINTRO_SYNC_WAIT_SECS 2.5
#define BOOTINTRO_FADE_IN_SECS   2.5
#define BOOTINTRO_HOLD_SECS      2.0
#define BOOTINTRO_FADE_OUT_SECS  2.0

static void box_blur_pass(uint32_t *src, uint32_t *dst,
		int width, int height, int radius, bool horizontal) {
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint64_t a = 0, r = 0, g = 0, b = 0;
			int count = 0;
			for (int d = -radius; d <= radius; d++) {
				int nx = horizontal ? x + d : x;
				int ny = horizontal ? y : y + d;
				if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
				uint32_t p = src[ny * width + nx];
				a += (p >> 24) & 0xFF;
				r += (p >> 16) & 0xFF;
				g += (p >>  8) & 0xFF;
				b +=  p        & 0xFF;
				count++;
			}
			dst[y * width + x] = ((a/count) << 24) | ((r/count) << 16) |
				((g/count) << 8) | (b/count);
		}
	}
}

static void blur_pixels(uint32_t *pixels, int width, int height, int radius) {
	uint32_t *tmp = malloc(width * height * 4);
	if (!tmp) return;
	for (int pass = 0; pass < 3; pass++) {
		box_blur_pass(pixels, tmp, width, height, radius, true);
		box_blur_pass(tmp, pixels, width, height, radius, false);
	}
	free(tmp);
}

static struct wlr_buffer *render_boot_text_shadow(const char *text, int *out_w, int *out_h) {
	cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *probe_cr = cairo_create(probe);
	PangoLayout *layout = pango_cairo_create_layout(probe_cr);
	PangoFontDescription *desc = pango_font_description_from_string("Plus Jakarta Sans Light 96");
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	PangoAttrList *probe_attrs = pango_attr_list_new();
	pango_attr_list_insert(probe_attrs, pango_attr_letter_spacing_new(5 * PANGO_SCALE));
	pango_layout_set_attributes(layout, probe_attrs);
	pango_attr_list_unref(probe_attrs);
	int text_w, text_h;
	pango_layout_get_pixel_size(layout, &text_w, &text_h);
	g_object_unref(layout);
	cairo_destroy(probe_cr);
	cairo_surface_destroy(probe);

	int pad = 56;
	int buf_w = text_w + pad * 2;
	int buf_h = text_h + pad * 2;

	struct wlr_buffer *buf = resonance_buffer_create(buf_w, buf_h);
	if (!buf) return NULL;

	void *data = resonance_buffer_data(buf);
	size_t stride = resonance_buffer_stride(buf);
	uint32_t *shadow_pixels = calloc(buf_w * buf_h, 4);
	if (!shadow_pixels) { wlr_buffer_drop(buf); return NULL; }

	cairo_surface_t *shadow_surf = cairo_image_surface_create_for_data(
		(unsigned char *)shadow_pixels, CAIRO_FORMAT_ARGB32, buf_w, buf_h, buf_w * 4);
	cairo_t *shadow_cr = cairo_create(shadow_surf);
	layout = pango_cairo_create_layout(shadow_cr);
	desc = pango_font_description_from_string("Plus Jakarta Sans Light 96");
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	PangoAttrList *sattrs = pango_attr_list_new();
	pango_attr_list_insert(sattrs, pango_attr_letter_spacing_new(5 * PANGO_SCALE));
	pango_layout_set_attributes(layout, sattrs);
	pango_attr_list_unref(sattrs);
	cairo_set_source_rgba(shadow_cr, 0.0, 0.0, 0.0, 0.90);
	cairo_move_to(shadow_cr, pad, pad);
	pango_cairo_show_layout(shadow_cr, layout);
	g_object_unref(layout);
	cairo_surface_flush(shadow_surf);
	cairo_destroy(shadow_cr);
	cairo_surface_destroy(shadow_surf);

	blur_pixels(shadow_pixels, buf_w, buf_h, 24);

	cairo_surface_t *surface = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_ARGB32, buf_w, buf_h, stride);
	cairo_t *cr = cairo_create(surface);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_surface_t *shadow_src = cairo_image_surface_create_for_data(
		(unsigned char *)shadow_pixels, CAIRO_FORMAT_ARGB32, buf_w, buf_h, buf_w * 4);
	cairo_set_source_surface(cr, shadow_src, 0, 0);
	cairo_paint(cr);
	cairo_surface_destroy(shadow_src);
	free(shadow_pixels);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	*out_w = buf_w;
	*out_h = buf_h;
	return buf;
}

static struct wlr_buffer *render_boot_text(const char *text, int *out_w, int *out_h) {
	cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *probe_cr = cairo_create(probe);
	PangoLayout *layout = pango_cairo_create_layout(probe_cr);
	PangoFontDescription *desc = pango_font_description_from_string("Plus Jakarta Sans Light 96");
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	PangoAttrList *probe_attrs = pango_attr_list_new();
	pango_attr_list_insert(probe_attrs, pango_attr_letter_spacing_new(5 * PANGO_SCALE));
	pango_layout_set_attributes(layout, probe_attrs);
	pango_attr_list_unref(probe_attrs);
	int text_w, text_h;
	pango_layout_get_pixel_size(layout, &text_w, &text_h);
	g_object_unref(layout);
	cairo_destroy(probe_cr);
	cairo_surface_destroy(probe);

	int pad = 56;
	int buf_w = text_w + pad * 2;
	int buf_h = text_h + pad * 2;

	struct wlr_buffer *buf = resonance_buffer_create(buf_w, buf_h);
	if (!buf) return NULL;

	void *data = resonance_buffer_data(buf);
	size_t stride = resonance_buffer_stride(buf);
	cairo_surface_t *surface = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_ARGB32, buf_w, buf_h, stride);
	cairo_t *cr = cairo_create(surface);

	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);

	/* Tide gradient: teal rgb(70,120,123) → warm sand rgb(212,185,168) */
	layout = pango_cairo_create_layout(cr);
	desc = pango_font_description_from_string("Plus Jakarta Sans Light 96");
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	PangoAttrList *text_attrs = pango_attr_list_new();
	pango_attr_list_insert(text_attrs, pango_attr_letter_spacing_new(5 * PANGO_SCALE));
	pango_layout_set_attributes(layout, text_attrs);
	pango_attr_list_unref(text_attrs);
	int text_h_px = 0;
	pango_layout_get_pixel_size(layout, NULL, &text_h_px);
	cairo_pattern_t *grad = cairo_pattern_create_linear(0, pad, 0, pad + text_h_px);
	cairo_pattern_add_color_stop_rgba(grad, 0.0, 100/255.0, 184/255.0, 194/255.0, 1.0);
	cairo_pattern_add_color_stop_rgba(grad, 1.0, 194/255.0, 149/255.0, 114/255.0, 1.0);
	cairo_move_to(cr, pad, pad);
	pango_cairo_layout_path(cr, layout);
	cairo_set_source(cr, grad);
	cairo_fill(cr);
	cairo_pattern_destroy(grad);
	g_object_unref(layout);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	*out_w = buf_w;
	*out_h = buf_h;
	return buf;
}

static struct wlr_buffer *render_boot_subtitle_shadow(int *out_w, int *out_h) {
	const char *text = "held in momentum";
	int pad = 24;

	cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *probe_cr = cairo_create(probe);
	PangoLayout *layout = pango_cairo_create_layout(probe_cr);
	PangoFontDescription *desc = pango_font_description_from_string("Plus Jakarta Sans Light 26");
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	PangoAttrList *probe_attrs = pango_attr_list_new();
	pango_attr_list_insert(probe_attrs, pango_attr_letter_spacing_new(6 * PANGO_SCALE));
	pango_layout_set_attributes(layout, probe_attrs);
	pango_attr_list_unref(probe_attrs);
	int text_w, text_h;
	pango_layout_get_pixel_size(layout, &text_w, &text_h);
	g_object_unref(layout);
	cairo_destroy(probe_cr);
	cairo_surface_destroy(probe);

	int buf_w = text_w + pad * 2;
	int buf_h = text_h + pad * 2;

	struct wlr_buffer *buf = resonance_buffer_create(buf_w, buf_h);
	if (!buf) return NULL;

	void *data = resonance_buffer_data(buf);
	size_t stride = resonance_buffer_stride(buf);
	uint32_t *shadow_pixels = calloc(buf_w * buf_h, 4);
	if (!shadow_pixels) { wlr_buffer_drop(buf); return NULL; }

	cairo_surface_t *shadow_surf = cairo_image_surface_create_for_data(
		(unsigned char *)shadow_pixels, CAIRO_FORMAT_ARGB32, buf_w, buf_h, buf_w * 4);
	cairo_t *shadow_cr = cairo_create(shadow_surf);
	layout = pango_cairo_create_layout(shadow_cr);
	desc = pango_font_description_from_string("Plus Jakarta Sans Light 26");
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	PangoAttrList *sattrs = pango_attr_list_new();
	pango_attr_list_insert(sattrs, pango_attr_letter_spacing_new(6 * PANGO_SCALE));
	pango_layout_set_attributes(layout, sattrs);
	pango_attr_list_unref(sattrs);
	cairo_set_source_rgba(shadow_cr, 0.0, 0.0, 0.0, 0.90);
	cairo_move_to(shadow_cr, pad, pad);
	pango_cairo_show_layout(shadow_cr, layout);
	g_object_unref(layout);
	cairo_surface_flush(shadow_surf);
	cairo_destroy(shadow_cr);
	cairo_surface_destroy(shadow_surf);

	blur_pixels(shadow_pixels, buf_w, buf_h, 10);

	cairo_surface_t *surface = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_ARGB32, buf_w, buf_h, stride);
	cairo_t *cr = cairo_create(surface);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_surface_t *shadow_src = cairo_image_surface_create_for_data(
		(unsigned char *)shadow_pixels, CAIRO_FORMAT_ARGB32, buf_w, buf_h, buf_w * 4);
	cairo_set_source_surface(cr, shadow_src, 0, 0);
	cairo_paint(cr);
	cairo_surface_destroy(shadow_src);
	free(shadow_pixels);
	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	*out_w = buf_w;
	*out_h = buf_h;
	return buf;
}

static struct wlr_buffer *render_boot_subtitle(int *out_w, int *out_h) {
	const char *text = "held in momentum";
	int pad = 24;

	cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *probe_cr = cairo_create(probe);
	PangoLayout *layout = pango_cairo_create_layout(probe_cr);
	PangoFontDescription *desc = pango_font_description_from_string("Plus Jakarta Sans Light 26");
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	PangoAttrList *probe_attrs = pango_attr_list_new();
	pango_attr_list_insert(probe_attrs, pango_attr_letter_spacing_new(6 * PANGO_SCALE));
	pango_layout_set_attributes(layout, probe_attrs);
	pango_attr_list_unref(probe_attrs);
	int text_w, text_h;
	pango_layout_get_pixel_size(layout, &text_w, &text_h);
	g_object_unref(layout);
	cairo_destroy(probe_cr);
	cairo_surface_destroy(probe);

	int buf_w = text_w + pad * 2;
	int buf_h = text_h + pad * 2;

	struct wlr_buffer *buf = resonance_buffer_create(buf_w, buf_h);
	if (!buf) return NULL;

	void *data = resonance_buffer_data(buf);
	size_t stride = resonance_buffer_stride(buf);
	cairo_surface_t *surface = cairo_image_surface_create_for_data(
		data, CAIRO_FORMAT_ARGB32, buf_w, buf_h, stride);
	cairo_t *cr = cairo_create(surface);

	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);

	layout = pango_cairo_create_layout(cr);
	desc = pango_font_description_from_string("Plus Jakarta Sans Light 26");
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	PangoAttrList *attrs = pango_attr_list_new();
	pango_attr_list_insert(attrs, pango_attr_letter_spacing_new(6 * PANGO_SCALE));
	pango_layout_set_attributes(layout, attrs);
	pango_attr_list_unref(attrs);

	cairo_move_to(cr, pad, pad);
	pango_cairo_layout_path(cr, layout);
	cairo_set_source_rgba(cr, 0.918, 0.918, 0.922, 1.0);
	cairo_fill(cr);
	g_object_unref(layout);

	cairo_destroy(cr);
	cairo_surface_destroy(surface);

	*out_w = buf_w;
	*out_h = buf_h;
	return buf;
}

static void scene_buffer_fill(struct wlr_scene_buffer *buf,
		int img_w, int img_h, int out_w, int out_h) {
	double scale_x = (double)out_w / img_w;
	double scale_y = (double)out_h / img_h;
	double scale = scale_x > scale_y ? scale_x : scale_y;
	double src_x = ((img_w * scale) - out_w) / 2.0 / scale;
	double src_y = ((img_h * scale) - out_h) / 2.0 / scale;
	struct wlr_fbox src_box = {
		.x = src_x, .y = src_y,
		.width = out_w / scale, .height = out_h / scale,
	};
	wlr_scene_buffer_set_source_box(buf, &src_box);
	wlr_scene_buffer_set_dest_size(buf, out_w, out_h);
}

struct resonance_jpeg_error_mgr {
	struct jpeg_error_mgr pub;
	jmp_buf setjmp_buffer;
	char message[JMSG_LENGTH_MAX];
};

static void resonance_jpeg_error_exit(j_common_ptr cinfo) {
	struct resonance_jpeg_error_mgr *err =
		(struct resonance_jpeg_error_mgr *)cinfo->err;
	(*cinfo->err->format_message)(cinfo, err->message);
	longjmp(err->setjmp_buffer, 1);
}

static struct wlr_buffer *load_boot_image(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		wlr_log(WLR_ERROR, "Boot image not found: %s", path);
		return NULL;
	}

	struct jpeg_decompress_struct cinfo = {0};
	struct resonance_jpeg_error_mgr jerr = {0};
	struct wlr_buffer *buf = NULL;
	uint8_t *row = NULL;
	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = resonance_jpeg_error_exit;
	if (setjmp(jerr.setjmp_buffer)) {
		wlr_log(WLR_ERROR, "Failed to decode boot image %s: %s", path, jerr.message);
		free(row);
		if (buf) {
			wlr_buffer_drop(buf);
		}
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return NULL;
	}
	jpeg_create_decompress(&cinfo);
	jpeg_stdio_src(&cinfo, f);
	if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
		wlr_log(WLR_ERROR, "Failed to read boot image header: %s", path);
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return NULL;
	}
	cinfo.out_color_space = JCS_RGB;
	jpeg_start_decompress(&cinfo);

	int w = cinfo.output_width;
	int h = cinfo.output_height;

	buf = resonance_buffer_create(w, h);
	if (!buf) {
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return NULL;
	}

	uint32_t *pixels = resonance_buffer_data(buf);
	row = malloc(w * 3);
	if (!row) {
		wlr_buffer_drop(buf);
		jpeg_destroy_decompress(&cinfo);
		fclose(f);
		return NULL;
	}

	int y = 0;
	while (cinfo.output_scanline < cinfo.output_height) {
		if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) {
			wlr_log(WLR_ERROR, "Failed to read boot image scanline: %s", path);
			free(row);
			wlr_buffer_drop(buf);
			jpeg_destroy_decompress(&cinfo);
			fclose(f);
			return NULL;
		}
		for (int x = 0; x < w; x++) {
			uint8_t r = row[x * 3 + 0];
			uint8_t g = row[x * 3 + 1];
			uint8_t b = row[x * 3 + 2];
			pixels[y * w + x] = (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
		}
		y++;
	}

	free(row);
	row = NULL;
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	fclose(f);
	return buf;
}

void bootintro_init(struct bootintro_state *st, const char *image_path) {
	memset(st, 0, sizeof(*st));
	st->image = load_boot_image(image_path);
	st->phase = st->image ? BOOTINTRO_SYNC_WAIT : BOOTINTRO_DONE;
}

void bootintro_handle_new_output(struct bootintro_state *st,
		struct wlr_scene_tree *overlay_tree, struct wlr_output_layout *layout,
		struct wlr_output *wlr_output) {
	if (st->phase == BOOTINTRO_DONE || !st->image ||
			st->overlay_count >= BOOTINTRO_MAX_OVERLAYS) {
		return;
	}

	struct wlr_box layout_box;
	wlr_output_layout_get_box(layout, wlr_output, &layout_box);

	/* The black rect exists to hide the desktop until the logo is up; the
	 * hold-phase cleanup below tears every rect down and never runs again.
	 * An output that appears after that (a monitor whose DP link flaps
	 * during boot, say) must not get one, or it keeps an opaque black
	 * sheet over it for the rest of the session. */
	struct wlr_scene_rect *rect = NULL;
	if (st->phase == BOOTINTRO_SYNC_WAIT || st->phase == BOOTINTRO_FADE_IN ||
			st->phase == BOOTINTRO_HOLD) {
		float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		rect = wlr_scene_rect_create(
			overlay_tree, layout_box.width, layout_box.height, black);
		if (rect) {
			wlr_scene_node_set_position(&rect->node, layout_box.x, layout_box.y);
		}
	}

	struct wlr_scene_buffer *overlay = wlr_scene_buffer_create(
		overlay_tree, st->image);
	if (overlay) {
		wlr_scene_node_set_position(&overlay->node, layout_box.x, layout_box.y);
		scene_buffer_fill(overlay,
			st->image->width, st->image->height,
			layout_box.width, layout_box.height);
		wlr_scene_buffer_set_opacity(overlay, 0.0f);
	}

	st->rects[st->overlay_count] = rect;
	st->overlays[st->overlay_count] = overlay;
	st->overlay_count++;

	if (st->overlay_count == 1) {
		clock_gettime(CLOCK_MONOTONIC, &st->phase_start);

		int text_w, text_h;
		struct wlr_buffer *text_buf = render_boot_text("Resonance", &text_w, &text_h);
		if (text_buf) {
			int tx = layout_box.x + (layout_box.width - text_w) / 2;
			int ty = layout_box.y + (layout_box.height - text_h) / 2;

			/* Shadow node created first so it sits below the text node */
			int shadow_w, shadow_h;
			struct wlr_buffer *tshadow_buf = render_boot_text_shadow("Resonance", &shadow_w, &shadow_h);
			if (tshadow_buf) {
				st->text_shadow = wlr_scene_buffer_create(
					overlay_tree, tshadow_buf);
				wlr_buffer_drop(tshadow_buf);
				if (st->text_shadow) {
					wlr_scene_node_set_position(&st->text_shadow->node, tx, ty);
					wlr_scene_buffer_set_opacity(st->text_shadow, 0.0f);
				}
			}

			st->text = wlr_scene_buffer_create(
				overlay_tree, text_buf);
			wlr_buffer_drop(text_buf);
			if (st->text) {
				wlr_scene_node_set_position(&st->text->node, tx, ty);
				wlr_scene_buffer_set_opacity(st->text, 0.0f);

				int sub_w, sub_h;
				struct wlr_buffer *sshadow_buf = render_boot_subtitle_shadow(&sub_w, &sub_h);
				if (sshadow_buf) {
					st->subtitle_shadow = wlr_scene_buffer_create(
						overlay_tree, sshadow_buf);
					wlr_buffer_drop(sshadow_buf);
					if (st->subtitle_shadow) {
						int sx = layout_box.x + (layout_box.width - sub_w) / 2;
						int sy = ty + text_h - 72;
						wlr_scene_node_set_position(&st->subtitle_shadow->node, sx, sy);
						wlr_scene_buffer_set_opacity(st->subtitle_shadow, 0.0f);
					}
				}

				struct wlr_buffer *sub_buf = render_boot_subtitle(&sub_w, &sub_h);
				if (sub_buf) {
					st->subtitle = wlr_scene_buffer_create(
						overlay_tree, sub_buf);
					wlr_buffer_drop(sub_buf);
					if (st->subtitle) {
						int sx = layout_box.x + (layout_box.width - sub_w) / 2;
						int sy = ty + text_h - 72;
						wlr_scene_node_set_position(&st->subtitle->node, sx, sy);
						wlr_scene_buffer_set_opacity(st->subtitle, 0.0f);
					}
				}
			}
		}
	}
}

bool bootintro_frame_tick(struct bootintro_state *st, struct timespec now) {
	if (st->phase == BOOTINTRO_DONE || st->overlay_count == 0) {
		return false;
	}

	double elapsed = (now.tv_sec - st->phase_start.tv_sec) +
		(now.tv_nsec - st->phase_start.tv_nsec) / 1e9;
	float opacity = 1.0f;

	switch (st->phase) {
	case BOOTINTRO_SYNC_WAIT:
		opacity = 0.0f;
		if (elapsed >= BOOTINTRO_SYNC_WAIT_SECS) {
			st->phase = BOOTINTRO_FADE_IN;
			clock_gettime(CLOCK_MONOTONIC, &st->phase_start);
			pid_t pid = fork();
			if (pid == 0) {
				pid_t grandchild = fork();
				if (grandchild == 0) {
					signal(SIGCHLD, SIG_DFL);
					setsid();
					execlp("pw-play", "pw-play",
						"/path/to/Resonance/boot.wav",
						NULL);
					_exit(1);
				}
				_exit(0);
			}
			if (pid > 0) {
				waitpid(pid, NULL, 0);
			}
		}
		break;
	case BOOTINTRO_FADE_IN:
		opacity = fminf((float)(elapsed / BOOTINTRO_FADE_IN_SECS), 1.0f);
		if (opacity >= 1.0f) {
			st->phase = BOOTINTRO_HOLD;
			clock_gettime(CLOCK_MONOTONIC, &st->phase_start);
		}
		break;
	case BOOTINTRO_HOLD:
		opacity = 1.0f;
		if (elapsed >= BOOTINTRO_HOLD_SECS) {
			for (int i = 0; i < st->overlay_count; i++) {
				if (st->rects[i]) {
					wlr_scene_node_destroy(&st->rects[i]->node);
					st->rects[i] = NULL;
				}
			}
			st->phase = BOOTINTRO_FADE_OUT;
			clock_gettime(CLOCK_MONOTONIC, &st->phase_start);
		}
		break;
	case BOOTINTRO_FADE_OUT:
		opacity = fmaxf(1.0f - (float)(elapsed / BOOTINTRO_FADE_OUT_SECS), 0.0f);
		if (opacity <= 0.0f) {
			for (int i = 0; i < st->overlay_count; i++) {
				if (st->overlays[i]) {
					wlr_scene_node_destroy(&st->overlays[i]->node);
					st->overlays[i] = NULL;
				}
				/* Normally already NULL from the hold-phase cleanup —
				 * a safety net so no rect can outlive the intro. */
				if (st->rects[i]) {
					wlr_scene_node_destroy(&st->rects[i]->node);
					st->rects[i] = NULL;
				}
			}
			st->overlay_count = 0;
			if (st->text_shadow) {
				wlr_scene_node_destroy(&st->text_shadow->node);
				st->text_shadow = NULL;
			}
			if (st->text) {
				wlr_scene_node_destroy(&st->text->node);
				st->text = NULL;
			}
			if (st->subtitle_shadow) {
				wlr_scene_node_destroy(&st->subtitle_shadow->node);
				st->subtitle_shadow = NULL;
			}
			if (st->subtitle) {
				wlr_scene_node_destroy(&st->subtitle->node);
				st->subtitle = NULL;
			}
			wlr_buffer_drop(st->image);
			st->image = NULL;
			st->phase = BOOTINTRO_DONE;
			return false;
		}
		break;
	case BOOTINTRO_DONE:
		break;
	}

	if (st->phase == BOOTINTRO_DONE) {
		return false;
	}

	for (int i = 0; i < st->overlay_count; i++) {
		if (st->overlays[i]) {
			wlr_scene_buffer_set_opacity(st->overlays[i], opacity);
		}
	}
	/* shadows track the overlay */
	if (st->text_shadow) {
		wlr_scene_buffer_set_opacity(st->text_shadow, opacity);
	}
	if (st->subtitle_shadow) {
		float sub_shadow_opacity = (st->phase == BOOTINTRO_FADE_IN)
			? fminf(fmaxf((opacity - 0.7f) / 0.15f, 0.0f), 1.0f)
			: opacity;
		wlr_scene_buffer_set_opacity(st->subtitle_shadow, sub_shadow_opacity);
	}
	if (st->text) {
		wlr_scene_node_raise_to_top(&st->text->node);
		/* text locks in at full opacity by the time background is 50% */
		float text_opacity = (st->phase == BOOTINTRO_FADE_IN)
			? fminf(opacity / 0.5f, 1.0f)
			: opacity;
		wlr_scene_buffer_set_opacity(st->text, text_opacity);
	}
	if (st->subtitle) {
		wlr_scene_node_raise_to_top(&st->subtitle->node);
		/* subtitle starts at 70% of background fade, locks in by 85% */
		float sub_opacity = (st->phase == BOOTINTRO_FADE_IN)
			? fminf(fmaxf((opacity - 0.7f) / 0.15f, 0.0f), 1.0f)
			: opacity;
		wlr_scene_buffer_set_opacity(st->subtitle, sub_opacity);
	}
	return true;
}
