// SPDX-License-Identifier: GPL-3.0-or-later
#include "conf.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct pi_string_list flags = {0};
    char hash[PI_USE_HASH_LENGTH + 1U];
    FILE *file = fopen("/tmp/pi-test.conf", "w");

    assert(file != NULL);
    fputs("USE=\"wayland -X clang\"\n", file);
    assert(fclose(file) == 0);
    assert(pi_conf_load_use_flags("/tmp/pi-test.conf", NULL, &flags) == 0);
    assert(pi_conf_use_enabled(&flags, "wayland"));
    assert(!pi_conf_use_enabled(&flags, "X"));
    assert(pi_conf_use_hash(&flags, hash) == 0);
    assert(hash[PI_USE_HASH_LENGTH] == '\0');
    pi_string_list_free(&flags);
    remove("/tmp/pi-test.conf");
    return 0;
}
