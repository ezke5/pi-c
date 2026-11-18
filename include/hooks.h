// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PI_HOOKS_H
#define PI_HOOKS_H

#include <stdbool.h>

bool pi_hook_valid_package_name(const char *pkgname);
int pi_hook_run(const char *root, const char *pkgname, const char *function);

#endif
