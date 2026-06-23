#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "wt_stream.h"

uint16_t wt_stream_next_id(uint16_t *next_id)
{
	if (!next_id) {
		return 1;
	}

	(*next_id)++;
	if (*next_id == 0 || *next_id > 999) {
		*next_id = 1;
	}

	return *next_id;
}

static size_t wt_stream_frame_limit(const struct wt_stream_ctx *ctx)
{
	size_t limit;

	if (!ctx || !ctx->frame || ctx->frame_size == 0) {
		return 0;
	}

	limit = ctx->frame_size - 1;
	if (ctx->frame_max > 0) {
		limit = MIN(limit, ctx->frame_max);
	}

	return limit;
}

int wt_stream_begin(struct wt_stream_ctx *ctx, char *frame, size_t frame_size,
			    size_t frame_max, uint32_t inter_frame_delay_ms,
			    wt_stream_emit_frame_fn emit, void *user, uint16_t id)
{
	int len;

	if (!ctx || !frame || frame_size == 0 || !emit || id == 0 || id > 999) {
		return -EINVAL;
	}

	ctx->id = id;
	ctx->seq = 0;
	ctx->frame = frame;
	ctx->frame_size = frame_size;
	ctx->frame_max = frame_max;
	ctx->inter_frame_delay_ms = inter_frame_delay_ms;
	ctx->emit = emit;
	ctx->user = user;

	len = snprintk(ctx->frame, ctx->frame_size, "~S%03u", ctx->id);
	if (len < 0) {
		return len;
	}
	if ((size_t)len > wt_stream_frame_limit(ctx)) {
		return -EMSGSIZE;
	}

	return ctx->emit(ctx->frame, (size_t)len, ctx->user);
}

int wt_stream_write(struct wt_stream_ctx *ctx, const char *data, size_t len)
{
	size_t offset = 0;
	int ret;

	if (!ctx || !ctx->frame || !ctx->emit || (!data && len > 0)) {
		return -EINVAL;
	}

	while (offset < len) {
		const size_t frame_limit = wt_stream_frame_limit(ctx);
		int header_len;
		size_t payload_max;
		size_t payload_len;

		header_len = snprintk(ctx->frame, ctx->frame_size,
				       "~C%03u%03u", ctx->id, ctx->seq % 1000);
		if (header_len < 0) {
			return header_len;
		}
		if (frame_limit <= (size_t)header_len) {
			return -EMSGSIZE;
		}

		payload_max = frame_limit - (size_t)header_len;
		payload_len = MIN(len - offset, payload_max);
		memcpy(&ctx->frame[header_len], &data[offset], payload_len);
		ctx->frame[header_len + payload_len] = '\0';

		ret = ctx->emit(ctx->frame, (size_t)header_len + payload_len, ctx->user);
		if (ret) {
			return ret;
		}

		offset += payload_len;
		ctx->seq++;

		if (offset < len && ctx->inter_frame_delay_ms > 0) {
			k_msleep(ctx->inter_frame_delay_ms);
		}
	}

	return 0;
}

int wt_stream_write_cstr(struct wt_stream_ctx *ctx, const char *text)
{
	if (!text) {
		return -EINVAL;
	}

	return wt_stream_write(ctx, text, strlen(text));
}

int wt_stream_write_request_id_prefix(struct wt_stream_ctx *ctx, const char *request_id)
{
	char prefix[32];
	int len;

	if (!request_id || request_id[0] == '\0') {
		return 0;
	}

	len = snprintk(prefix, sizeof(prefix), "#%s ", request_id);
	if (len < 0) {
		return len;
	}
	if ((size_t)len >= sizeof(prefix)) {
		return -EMSGSIZE;
	}

	return wt_stream_write(ctx, prefix, (size_t)len);
}

int wt_stream_end(struct wt_stream_ctx *ctx)
{
	int len;

	if (!ctx || !ctx->frame || !ctx->emit) {
		return -EINVAL;
	}

	len = snprintk(ctx->frame, ctx->frame_size,
		       "~E%03u%03u", ctx->id, ctx->seq % 1000);
	if (len < 0) {
		return len;
	}
	if ((size_t)len > wt_stream_frame_limit(ctx)) {
		return -EMSGSIZE;
	}

	return ctx->emit(ctx->frame, (size_t)len, ctx->user);
}
