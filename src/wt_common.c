#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "wt_common.h"

const char *wt_onoff_txt(bool state)
{
	return state ? "on" : "off";
}

int wt_build_payload_from_argv(size_t start_arg, size_t argc, char **argv,
				       char *buf, size_t buf_len)
{
	size_t offset = 0;

	if (start_arg >= argc) {
		return -EINVAL;
	}

	for (size_t i = start_arg; i < argc; i++) {
		const char *arg = argv[i];
		size_t arg_len = strlen(arg);

		if (i > start_arg) {
			if (offset + 1 >= buf_len) {
				return -EMSGSIZE;
			}
			buf[offset++] = ' ';
		}

		if (offset + arg_len >= buf_len) {
			return -EMSGSIZE;
		}

		memcpy(&buf[offset], arg, arg_len);
		offset += arg_len;
	}

	buf[offset] = '\0';
	return (int)offset;
}

int wt_parse_udp_port(const char *port_text, uint16_t *port)
{
	char *end = NULL;
	long parsed = strtol(port_text, &end, 10);

	if (!end || *end != '\0' || parsed <= 0 || parsed > UINT16_MAX) {
		return -EINVAL;
	}

	*port = (uint16_t)parsed;
	return 0;
}
