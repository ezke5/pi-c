// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "index.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    char directory[] = ".pi-index-test-XXXXXX";
    char package[256];
    char index_path[256];
    FILE *file;
    struct pi_index index = {0};

    assert(mkdtemp(directory) != NULL);
    assert(snprintf(package, sizeof(package), "%s/demo-1-1-x86_64.pkg.tar.zst", directory) > 0);
    assert(snprintf(index_path, sizeof(index_path), "%s/index", directory) > 0);
    file = fopen(package, "wb");
    assert(file != NULL);
    assert(fputs("package", file) >= 0);
    assert(fclose(file) == 0);
    assert(pi_index_build(directory, index_path) == 0);
    assert(pi_index_load(index_path, &index) == 0);
    assert(index.count == 1U);
    assert(strcmp(index.entries[0].filename, "demo-1-1-x86_64.pkg.tar.zst") == 0);
    assert(pi_index_verify(directory, &index) == 0);
    file = fopen(package, "ab");
    assert(file != NULL);
    assert(fputs("changed", file) >= 0);
    assert(fclose(file) == 0);
    assert(pi_index_verify(directory, &index) == EBADMSG);
    pi_index_free(&index);
    (void)unlink(package);
    (void)unlink(index_path);
    assert(rmdir(directory) == 0);
    return 0;
}
