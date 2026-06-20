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
	char *read = line;
	size_t argc = 0;

	if (!line || !argv || argv_max == 0) {
		return -EINVAL;
	}

	while (*read) {
		char *write;
		char quote = '\0';

		while (*read && wt_is_cmd_space(*read)) {
			*read++ = '\0';
		}

		if (!*read) {
			break;
		}

		if (argc >= argv_max) {
			return -E2BIG;
		}

		argv[argc++] = read;
		write = read;

		while (*read) {
			char c = *read++;

			if (quote) {
				if (c == '\\' && *read) {
					*write++ = wt_unescape_char(*read++);
					continue;
				}
				if (c == quote) {
					quote = '\0';
					continue;
				}
				*write++ = c;
				continue;
			}

			if (c == '\'' || c == '"') {
				quote = c;
				continue;
			}

			if (wt_is_cmd_space(c)) {
				break;
			}

			if (c == '\\' && *read) {
				*write++ = wt_unescape_char(*read++);
				continue;
			}

			*write++ = c;
		}

		if (quote) {
			return -EINVAL;
		}

		*write = '\0';
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
