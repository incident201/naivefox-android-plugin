/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NAIVEFOX_PLUGIN_TRANSPORT_CONFIG_H
#define NAIVEFOX_PLUGIN_TRANSPORT_CONFIG_H

#include <stdbool.h>

#define NAIVEFOX_CONFIG_MAXIMUM_BYTES (1024U * 1024U)

/* Select only the top-level transport; preserve every other input byte.
 * The caller owns *result. NaiveFox still validates all proxy configuration.
 * Reject ambiguous/invalid JSON, duplicate transport keys, or oversized output.
 */
bool SelectTransport(const char* config, const char* transport, char** result);

#endif
