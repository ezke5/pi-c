// SPDX-License-Identifier: GPL-3.0-or-later
#include "db.h"
#include "hooks.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    char root[] = "/tmp/pi-phase2-XXXXXX";
    struct pi_package_metadata metadata = {
        .pkgname = "sample", .pkgver = "1", .pkgrel = "1", .arch = "x86_64"
    };
    bool valid = false;
    char directory[256];
    FILE *file;

    assert(mkdtemp(root) != NULL);
    assert(pi_hook_valid_package_name("sample+pkg_1"));
    assert(!pi_hook_valid_package_name("../sample"));
    assert(pi_db_write_metadata(root, &metadata) == 0);
    assert(snprintf(directory, sizeof(directory), "%s/var/db/pi/installed/sample/meta", root) > 0);
    file = fopen(directory, "r");
    assert(file != NULL);
    assert(fclose(file) == 0);
    assert(pi_db_verify(root, "sample", &valid) == ENOENT);
    assert(pi_db_remove(root, "sample", false) == 0);
    (void)rmdir(root);
    return 0;
}
