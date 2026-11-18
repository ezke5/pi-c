// SPDX-License-Identifier: GPL-3.0-or-later
#include "hooks.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <stdio.h>
#include <string.h>

bool pi_hook_valid_package_name(const char *pkgname)
{
    if (pkgname == NULL || *pkgname == '\0') {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)pkgname; *cursor != '\0';
         cursor++) {
        if (!(('a' <= *cursor && *cursor <= 'z') || ('A' <= *cursor && *cursor <= 'Z') ||
              ('0' <= *cursor && *cursor <= '9') || strchr("._+-", *cursor) != NULL)) {
            return false;
        }
    }
    return true;
}

int pi_hook_run(const char *root, const char *pkgname, const char *function)
{
    char path[4096];
    pid_t child;
    int status;
    const char *allowed[] = {"pre_install", "post_install", "pre_upgrade", "post_upgrade",
                             "pre_remove", "post_remove"};
    bool valid_function = false;

    if (root == NULL || !pi_hook_valid_package_name(pkgname) || function == NULL) {
        return EINVAL;
    }
    for (size_t index = 0U; index < sizeof(allowed) / sizeof(allowed[0]); index++) {
        if (strcmp(function, allowed[index]) == 0) {
            valid_function = true;
            break;
        }
    }
    if (!valid_function || snprintf(path, sizeof(path), "%s/var/db/pi/installed/%s/hooks",
                                     root, pkgname) < 0 ||
        strlen(path) >= sizeof(path)) {
        return EINVAL;
    }
    if (access(path, R_OK) != 0) {
        return errno == ENOENT ? 0 : errno;
    }
    child = fork();
    if (child < 0) {
        return errno;
    }
    if (child == 0) {
        char root_env[4096];
        char package_env[256];
        char version_env[256] = "PI_PKGVER=";
        char meta_path[4096];
        char line[512];
        FILE *meta;
        char *environment[4] = {root_env, package_env, version_env, NULL};
        (void)snprintf(root_env, sizeof(root_env), "PI_ROOT=%s", root);
        (void)snprintf(package_env, sizeof(package_env), "PI_PKGNAME=%s", pkgname);
        (void)snprintf(meta_path, sizeof(meta_path), "%s/var/db/pi/installed/%s/meta", root,
                       pkgname);
        meta = fopen(meta_path, "r");
        while (meta != NULL && fgets(line, sizeof(line), meta) != NULL) {
            if (strncmp(line, "pkgver=", 7U) == 0) {
                line[strcspn(line, "\r\n")] = '\0';
                (void)snprintf(version_env, sizeof(version_env), "PI_PKGVER=%s", line + 7U);
                break;
            }
        }
        if (meta != NULL) {
            (void)fclose(meta);
        }
        if (strlen(root_env) >= sizeof(root_env) - 1U ||
            strlen(package_env) >= sizeof(package_env) - 1U ||
            strlen(version_env) >= sizeof(version_env) - 1U) {
            _exit(126);
        }
        execle("/bin/sh", "sh", "-c",
               "hook=$1; . \"$2\"; command -v \"$hook\" >/dev/null 2>&1 && \"$hook\"",
               "pi-hook", function, path, environment);
        _exit(127);
    }
    if (waitpid(child, &status, 0) < 0) {
        return errno;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
