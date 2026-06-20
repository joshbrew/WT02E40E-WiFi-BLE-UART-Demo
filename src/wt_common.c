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

static bool wt_is_cmd_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char wt_unescape_char(char c)
{
	switch (c) {
	case 'n':
		return '\n';
	case 'r':
		return '\r';
	case 't':
		return '\t';
	default:
		return c;
	}
}

int wt_split_args_quoted(char *line, char **argv, size_t argv_max)
{
	char *p = line;
	size_t argc = 0;

	if (!line || !argv || argv_max == 0) {
		return -EINVAL;
	}

	while (*p) {
		char *out;
		bool in_token = false;

		while (*p && wt_is_cmd_space(*p)) {
			*p++ = '\0';
		}

		if (!*p) {
			break;
		}

		if (argc >= argv_max) {
			return -E2BIG;
		}

		argv[argc++] = p;
		out = p;
		in_token = true;

		while (*p && in_token) {
			if (wt_is_cmd_space(*p)) {
				break;
			}

			if (*p == '\'' || *p == '"') {
				char quote = *p++;

				while (*p && *p != quote) {
					if (*p == '\\' && p[1]) {
						p++;
						*out++ = wt_unescape_char(*p++);
					} else {
						*out++ = *p++;
					}
				}

				if (*p != quote) {
					return -EINVAL;
				}

				p++;
				continue;
			}

			if (*p == '\\' && p[1]) {
				p++;
				*out++ = wt_unescape_char(*p++);
				continue;
			}

			*out++ = *p++;
		}

		*out = '\0';

		while (*p && wt_is_cmd_space(*p)) {
			*p++ = '\0';
		}
	}

	return (int)argc;
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
