// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PI_INDEX_H
#define PI_INDEX_H

#include <stddef.h>

struct pi_index_entry {
    char *filename;
    size_t size_bytes;
    char sha256[65];
};

struct pi_index {
    struct pi_index_entry *entries;
    size_t count;
};

/*
 * Build a deterministic index of regular .pkg.tar.zst files in repository.
 * The index is written atomically and contains a SHA-256 digest for every
 * package.  No subdirectories, symlinks, or other file types are indexed.
 */
int pi_index_build(const char *repository, const char *index_path);
int pi_index_load(const char *index_path, struct pi_index *index);
int pi_index_verify(const char *repository, const struct pi_index *index);
void pi_index_free(struct pi_index *index);

#endif
