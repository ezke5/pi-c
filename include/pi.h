// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PI_H
#define PI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PI_USE_HASH_LENGTH 12U

struct pi_string_list {
    char **items;
    size_t count;
};

void pi_string_list_free(struct pi_string_list *list);
bool pi_string_list_contains(const struct pi_string_list *list, const char *value);

#endif
