#ifndef WT_STREAM_H
#define WT_STREAM_H

#include <stddef.h>
#include <stdint.h>

typedef int (*wt_stream_emit_frame_fn)(const char *frame, size_t len, void *user);

struct wt_stream_ctx {
	uint16_t id;
	uint16_t seq;
	char *frame;
	size_t frame_size;
	size_t frame_max;
	uint32_t inter_frame_delay_ms;
	wt_stream_emit_frame_fn emit;
	void *user;
};

uint16_t wt_stream_next_id(uint16_t *next_id);
int wt_stream_begin(struct wt_stream_ctx *ctx, char *frame, size_t frame_size,
			    size_t frame_max, uint32_t inter_frame_delay_ms,
			    wt_stream_emit_frame_fn emit, void *user, uint16_t id);
int wt_stream_write(struct wt_stream_ctx *ctx, const char *data, size_t len);
int wt_stream_write_cstr(struct wt_stream_ctx *ctx, const char *text);
int wt_stream_write_request_id_prefix(struct wt_stream_ctx *ctx, const char *request_id);
int wt_stream_end(struct wt_stream_ctx *ctx);

#endif /* WT_STREAM_H */
