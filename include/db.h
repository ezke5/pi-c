// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PI_DB_H
#define PI_DB_H

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

struct pi_package_metadata {
    char *pkgname;
    char *pkgver;
    char *pkgrel;
    char *arch;
    char *use_flags;
    char *use_hash;
    char *build_date;
    char *install_reason;
    size_t size_bytes;
};

struct pi_db_package {
    const char *root;
    const char *pkgname;
    const struct pi_package_metadata *metadata;
    const char *manifest;
    const char *depends;
    const char *backup;
    const char *hooks;
};

void pi_package_metadata_free(struct pi_package_metadata *metadata);
int pi_db_write_metadata(const char *root, const struct pi_package_metadata *metadata);
int pi_db_check_conflicts(const char *root, const char *pkgname, const char *manifest,
                          const char *backup);
int pi_db_install_commit(const struct pi_db_package *package, const char *staging_root);
int pi_db_verify(const char *root, const char *pkgname, bool *ok);
int pi_db_remove(const char *root, const char *pkgname, bool force);
int pi_db_info(const char *root, const char *pkgname, FILE *output);
int pi_db_lock(const char *root, int *lock_fd);
void pi_db_unlock(int lock_fd);
int pi_db_world_add(const char *root, const char *pkgname);
int pi_db_world_remove(const char *root, const char *pkgname);
int pi_db_clean_cache(const char *root, unsigned int keep);

#endif
