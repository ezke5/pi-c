// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PI_SANDBOX_H
#define PI_SANDBOX_H

#include <stdbool.h>

struct pi_sandbox_options {
    const char *srcdir;
    const char *pkgdir;
    const char *script;
    bool network;
    const char *use;
    const char *cflags;
    const char *cxxflags;
    const char *ldflags;
    const char *makeflags;
    const char *max_jobs;
};

int pi_sandbox_run(const struct pi_sandbox_options *options);

#endif
