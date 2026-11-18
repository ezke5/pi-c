// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PI_CONF_H
#define PI_CONF_H

#include "pi.h"

#include <stdbool.h>

int pi_conf_load_use_flags(const char *global_path, const char *package_path,
                           struct pi_string_list *flags);
int pi_conf_use_hash(const struct pi_string_list *flags,
                     char output[PI_USE_HASH_LENGTH + 1U]);
bool pi_conf_use_enabled(const struct pi_string_list *flags, const char *name);

#endif
