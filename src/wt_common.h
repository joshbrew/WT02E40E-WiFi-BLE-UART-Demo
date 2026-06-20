#ifndef WT_COMMON_H
#define WT_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

const char *wt_onoff_txt(bool state);
int wt_build_payload_from_argv(size_t start_arg, size_t argc, char **argv,
				       char *buf, size_t buf_len);
int wt_parse_udp_port(const char *port_text, uint16_t *port);

#endif /* WT_COMMON_H */
