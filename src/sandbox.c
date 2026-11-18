// SPDX-License-Identifier: GPL-3.0-or-later
#include "sandbox.h"

#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static int bind_ro_if_exists(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) != 0) {
        return 0;
    }
    if (S_ISLNK(st.st_mode)) {
        char target[256];
        ssize_t len = readlink(src, target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = '\0';
            (void)symlink(target, dst);
        }
        return 0;
    }
    (void)mkdir(dst, 0755);
    if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) != 0) {
        return -1;
    }
    (void)mount(NULL, dst, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | MS_REC, NULL);
    return 0;
}

static int mount_sandbox_fs(const struct pi_sandbox_options *options)
{
    const char *root = "/tmp/pi-sandbox";

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        perror("sandbox: MS_PRIVATE /");
        return -1;
    }
    (void)mkdir(root, 0755);
    if (mount("tmpfs", root, "tmpfs", MS_NOSUID | MS_NODEV, NULL) != 0) {
        perror("sandbox: mount tmpfs root");
        return -1;
    }

    const char *ro_dirs[] = { "/usr", "/bin", "/sbin", "/lib", "/lib64", "/etc" };
    for (size_t i = 0; i < sizeof(ro_dirs) / sizeof(ro_dirs[0]); i++) {
        char dst[256];
        snprintf(dst, sizeof(dst), "%s%s", root, ro_dirs[i]);
        if (bind_ro_if_exists(ro_dirs[i], dst) != 0) {
            perror("sandbox: bind ro dir");
            return -1;
        }
    }

    (void)mkdir("/tmp/pi-sandbox/proc", 0555);
    if (mount("proc", "/tmp/pi-sandbox/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
        perror("sandbox: mount /proc");
        return -1;
    }

    (void)mkdir("/tmp/pi-sandbox/dev", 0755);
    if (mount("/dev", "/tmp/pi-sandbox/dev", NULL, MS_BIND | MS_REC, NULL) != 0) {
        perror("sandbox: bind /dev");
        return -1;
    }

    (void)mkdir("/tmp/pi-sandbox/tmp", 01777);
    if (mount("tmpfs", "/tmp/pi-sandbox/tmp", "tmpfs", MS_NOSUID | MS_NODEV, NULL) != 0) {
        perror("sandbox: mount /tmp");
        return -1;
    }

    (void)mkdir("/tmp/pi-sandbox/src", 0755);
    if (mount(options->srcdir, "/tmp/pi-sandbox/src", NULL, MS_BIND | MS_REC, NULL) != 0) {
        perror("sandbox: bind srcdir");
        return -1;
    }

    (void)mkdir("/tmp/pi-sandbox/pkg", 0755);
    if (mount(options->pkgdir, "/tmp/pi-sandbox/pkg", NULL, MS_BIND | MS_REC, NULL) != 0) {
        perror("sandbox: bind pkgdir");
        return -1;
    }

    if (chroot(root) != 0 || chdir("/src") != 0) {
        perror("sandbox: chroot/chdir");
        return -1;
    }

    (void)setenv("srcdir", "/src", 1);
    (void)setenv("pkgdir", "/pkg", 1);
    if (options->use != NULL) (void)setenv("USE", options->use, 1);
    if (options->cflags != NULL) (void)setenv("CFLAGS", options->cflags, 1);
    if (options->cxxflags != NULL) (void)setenv("CXXFLAGS", options->cxxflags, 1);
    if (options->ldflags != NULL) (void)setenv("LDFLAGS", options->ldflags, 1);
    if (options->makeflags != NULL) (void)setenv("MAKEFLAGS", options->makeflags, 1);
    if (options->max_jobs != NULL) (void)setenv("MAX_JOBS", options->max_jobs, 1);
    return 0;
}

static int sandbox_child(void *argument)
{
    const struct pi_sandbox_options *options = argument;

    if (mount_sandbox_fs(options) != 0) {
        return EXIT_FAILURE;
    }
    execl("/bin/bash", "bash", "-c", options->script, (char *)NULL);
    perror("sandbox: execl");
    return EXIT_FAILURE;
}

int pi_sandbox_run(const struct pi_sandbox_options *options)
{
    pid_t child;
    int status;
    void *stack;
    const size_t stack_size = 1024U * 1024U;
    int flags = CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWIPC | CLONE_NEWUTS | SIGCHLD;

    if (options == NULL || options->srcdir == NULL || options->pkgdir == NULL ||
        options->script == NULL) {
        return EINVAL;
    }
    if (!options->network) {
        flags |= CLONE_NEWNET;
    }
    stack = malloc(stack_size);
    if (stack == NULL) {
        return ENOMEM;
    }
    child = clone(sandbox_child, (char *)stack + stack_size, flags, (void *)options);
    if (child < 0) {
        free(stack);
        return errno;
    }
    if (waitpid(child, &status, 0) < 0) {
        free(stack);
        return errno;
    }
    free(stack);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}
