// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PI_NET_H
#define PI_NET_H

#include <stddef.h>

struct pi_net_options {
    const char *url;
    const char *destination;
    const char *sha256;
    long timeout_seconds;
};

int pi_net_fetch(const struct pi_net_options *options);
int pi_net_download(const struct pi_net_options *options);
int pi_net_clone(const char *spec, const char *destination);
int pi_net_sync_index(const char *url, const char *destination);
int pi_net_sha256_file(const char *path, char output[65]);

int pi_net_clone(const char *spec, const char *dest_dir);

#endif
