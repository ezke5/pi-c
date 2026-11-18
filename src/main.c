// SPDX-License-Identifier: GPL-3.0-or-later
#include "pi_archive.h"
#include "build.h"
#include "dag.h"
#include "db.h"
#include "net.h"
#include "index.h"

#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(const char *program)
{
    (void)fprintf(stderr, "usage: %s [--root PATH] <build|sync|install|remove|upgrade|"
                         "autoremove|search|verify|info|index> [--nodeps] ARG\n", program);
}


static int ensure_dir(const char *path)
{
    char copy[PATH_MAX];
    if (snprintf(copy, sizeof(copy), "%s", path) < 0 || strlen(path) >= sizeof(copy)) {
        return ENAMETOOLONG;
    }
    for (char *cursor = copy + 1U; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) return errno;
            *cursor = '/';
        }
    }
    if (mkdir(copy, 0755) != 0 && errno != EEXIST) return errno;
    return 0;
}

static int install_archive(const char *root, const char *archive)
{
    char staging[PATH_MAX];
    char manifest[PATH_MAX];
    char package[PATH_MAX];
    const char *base = strrchr(archive, '/');
    struct pi_package_metadata metadata = {0};
    struct pi_db_package operation;
    int result;

    base = base == NULL ? archive : base + 1U;
    if (snprintf(package, sizeof(package), "%s", base) < 0 ||
        strlen(package) >= sizeof(package)) {
        return ENAMETOOLONG;
    }
    char *suffix = strstr(package, ".pkg.tar");
    if (suffix != NULL) {
        *suffix = '\0';
    }
    if (snprintf(staging, sizeof(staging), "%s/var/cache/pi/staging/pi-%ld", root,
                 (long)getpid()) < 0 ||
        strlen(staging) >= sizeof(staging) ||
        snprintf(manifest, sizeof(manifest), "%s/.MANIFEST", staging) < 0 ||
        strlen(manifest) >= sizeof(manifest)) {
        return ENAMETOOLONG;
    }
    result = ensure_dir(staging);
    if (result != 0) {
        return result;
    }
    result = pi_archive_extract(archive, staging);
    if (result != 0) {
        return result;
    }
    metadata.pkgname = package;
    metadata.arch = "x86_64";
    operation = (struct pi_db_package){
        .root = root, .pkgname = package, .metadata = &metadata, .manifest = manifest
    };
    result = pi_db_install_commit(&operation, staging);
    (void)pi_db_verify(root, package, &(bool){false});
    if (result == 0) {
        result = pi_db_world_add(root, package);
    }
    return result;
}

static int search_repository(const char *root, const char *query)
{
    char path[PATH_MAX];
    DIR *directory;
    struct dirent *entry;
    bool found = false;

    if (snprintf(path, sizeof(path), "%s/var/db/pi/sync", root) < 0 ||
        strlen(path) >= sizeof(path)) {
        return ENAMETOOLONG;
    }
    directory = opendir(path);
    if (directory == NULL) {
        return errno == ENOENT ? 0 : errno;
    }
    while ((entry = readdir(directory)) != NULL) {
        char index_path[PATH_MAX];
        FILE *file;
        char line[4096];
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (snprintf(index_path, sizeof(index_path), "%s/%s", path, entry->d_name) < 0 ||
            strlen(index_path) >= sizeof(index_path)) {
            continue;
        }
        file = fopen(index_path, "r");
        if (file == NULL) {
            continue;
        }
        while (fgets(line, sizeof(line), file) != NULL) {
            if (strstr(line, query) != NULL) {
                (void)fputs(line, stdout);
                found = true;
            }
        }
        (void)fclose(file);
    }
    (void)closedir(directory);
    return found ? 0 : ENOENT;
}

int main(int argc, char **argv)
{
    const char *root = "/";
    int argument = 1;
    const char *command;
    int lock_fd = -1;
    bool mutating;
    bool nodeps = false;

    if (argument + 1 < argc && strcmp(argv[argument], "--root") == 0) {
        root = argv[argument + 1];
        argument += 2;
    }
    if (argument >= argc) {
        usage(argv[0]);
        return 2;
    }
    command = argv[argument++];
    if (argument < argc && strcmp(argv[argument], "--nodeps") == 0) {
        nodeps = true;
        argument++;
    }
    mutating = strcmp(command, "build") == 0 || strcmp(command, "install") == 0 || strcmp(command, "remove") == 0 ||
               strcmp(command, "upgrade") == 0 || strcmp(command, "sync") == 0 ||
               strcmp(command, "autoremove") == 0 || strcmp(command, "clean") == 0 ||
               strcmp(command, "index") == 0;
    if (mutating) {
        int lock_result = pi_db_lock(root, &lock_fd);
        if (lock_result != 0) {
            (void)fprintf(stderr, "pi: another instance holds the state lock\n");
            return lock_result;
        }
    }
    int result = 0;
    if (strcmp(command, "install") == 0 && argument < argc) {
        for (; argument < argc && result == 0; argument++) {
            result = install_archive(root, argv[argument]);
        }
        goto done;
    }
    if (strcmp(command, "build") == 0 && argument < argc) {
        result = pi_build_run(&(struct pi_build_spec){
            .name = NULL, .source_dir = NULL, .package_dir = NULL,
            .archive_path = NULL, .manifest_path = NULL, .script = NULL,
            .network = false, .root = root, .pkgbuild_path = argv[argument], .nodeps = nodeps
        });
        goto done;
    }
    if (strcmp(command, "sync") == 0 && argument + 1 < argc) {
        result = pi_net_sync_index(argv[argument], argv[argument + 1U]);
        goto done;
    }
    if (strcmp(command, "remove") == 0 && argument < argc) {
        result = pi_db_remove(root, argv[argument], false);
        if (result == 0) result = pi_db_world_remove(root, argv[argument]);
        goto done;
    }
    if (strcmp(command, "verify") == 0) {
        bool valid = true;
        int result = argument < argc ? pi_db_verify(root, argv[argument], &valid) : EINVAL;
        if (result == 0) {
            (void)puts(valid ? "OK" : "CORRUPTED");
        }
        return result != 0 ? result : (valid ? 0 : 1);
    }
    if (strcmp(command, "info") == 0 && argument < argc) {
        result = pi_db_info(root, argv[argument], stdout);
        goto done;
    }
    if (strcmp(command, "search") == 0 && argument < argc) {
        result = search_repository(root, argv[argument]);
        goto done;
    }
    if (strcmp(command, "index") == 0 && argument < argc) {
        const char *output = NULL;
        if (argument + 2 < argc && strcmp(argv[argument + 1U], "--output") == 0) {
            output = argv[argument + 2U];
        }
        char default_output[PATH_MAX];
        if (output == NULL) {
            if (snprintf(default_output, sizeof(default_output), "%s/repo.index",
                         argv[argument]) < 0 ||
                strlen(default_output) >= sizeof(default_output)) {
                result = ENAMETOOLONG;
                goto done;
            }
            output = default_output;
        }
        result = pi_index_build(argv[argument], output);
        goto done;
    }
    if (strcmp(command, "clean") == 0) {
        unsigned int keep = 2U;
        if (argument + 1 < argc && strcmp(argv[argument], "--keep") == 0) {
            char *end;
            unsigned long value = strtoul(argv[argument + 1U], &end, 10);
            if (*end != '\0' || value > UINT_MAX) {
                result = EINVAL;
                goto done;
            }
            keep = (unsigned int)value;
        }
        result = pi_db_clean_cache(root, keep);
        goto done;
    }
    if (strcmp(command, "upgrade") == 0 || strcmp(command, "autoremove") == 0) {
        (void)fprintf(stderr, "%s: repository orchestration is not configured\n", command);
        result = ENOTSUP;
        goto done;
    }
    usage(argv[0]);
    result = 2;
done:
    if (mutating) {
        pi_db_unlock(lock_fd);
    }
    if (result != 0 && (command == NULL || strcmp(command, "verify") != 0)) {
        (void)fprintf(stderr, "pi: %s: %s\n", command, strerror(result));
    }
    return result;
}
