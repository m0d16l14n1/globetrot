/* SPDX-License-Identifier: MIT
 * Configuration loader and validator for globetrot (/etc/globetrot.conf).
 */
#ifndef GLOBETROT_CONFIG_H
#define GLOBETROT_CONFIG_H

#include "globe_sm.h"

#include <stdbool.h>
#include <stddef.h>

#define GT_DEFAULT_CONFIG_PATH "/etc/globetrot.conf"
#define GT_DEFAULT_SOCKET_PATH "/run/globetrot/events.sock"
#define GT_DEFAULT_STATUS_PATH "/run/globetrot/status.json"

bool globe_config_parse_string(const char *text, globe_config_t *cfg, bool overlay,
                               char *errbuf, size_t errbuf_len);

bool globe_config_load_file(const char *path, globe_config_t *cfg, bool missing_ok,
                            char *errbuf, size_t errbuf_len);

bool globe_tap_mode_from_name(const char *name, globe_tap_mode_t *out_mode);
const char *globe_tap_mode_to_name(globe_tap_mode_t mode);

#endif /* GLOBETROT_CONFIG_H */
