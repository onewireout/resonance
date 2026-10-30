#ifndef RESONANCE_BUFFER_H
#define RESONANCE_BUFFER_H

#include <stddef.h>

struct wlr_buffer;

/* Memfd-backed shm buffer used for anything the compositor renders into with
 * cairo (borders, dialog backgrounds, boot splash text). Defined in
 * resonance.c; exposed here so other translation units (e.g. bootintro.c) can
 * create and fill one without knowing its internal layout. */
struct wlr_buffer *resonance_buffer_create(int width, int height);
void *resonance_buffer_data(struct wlr_buffer *buf);
size_t resonance_buffer_stride(struct wlr_buffer *buf);

#endif
