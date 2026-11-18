// SPDX-License-Identifier: GPL-3.0-or-later
#include "db.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    char root[128];
    int first = -1;
    int second = -1;
    FILE *world;
    char line[128];

    assert(snprintf(root, sizeof(root), "/tmp/pi-v1-%ld", (long)getpid()) > 0);
    assert(mkdir(root, 0755) == 0);
    assert(pi_db_lock(root, &first) == 0);
    assert(pi_db_lock(root, &second) == EWOULDBLOCK || second == EAGAIN);
    assert(pi_db_world_add(root, "zulu") == 0);
    assert(pi_db_world_add(root, "alpha") == 0);
    world = fopen("/tmp/pi-v1-world", "w");
    assert(world != NULL);
    fclose(world);
    pi_db_unlock(first);
    assert(pi_db_world_remove(root, "zulu") == 0);
    world = fopen("/tmp/pi-v1-world-check", "w");
    assert(world != NULL);
    fclose(world);
    (void)line;
    (void)unlink("/tmp/pi-v1-world");
    (void)unlink("/tmp/pi-v1-world-check");
    (void)rmdir(root);
    return 0;
}
