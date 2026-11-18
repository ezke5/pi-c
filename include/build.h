// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PI_BUILD_H
#define PI_BUILD_H

#include <stdbool.h>

struct pi_build_spec {
    const char *name;
    const char *source_dir;
    const char *package_dir;
    const char *archive_path;
    const char *manifest_path;
    const char *script;
    bool network;
    const char *root;
    const char *pkgbuild_path;
    bool nodeps;
};

int pi_build_run(const struct pi_build_spec *spec);

#endif
