// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PI_ARCHIVE_H
#define PI_ARCHIVE_H

#include <stddef.h>

/*
 * Archives are reproducible tar files compressed with gzip.  The manifest is
 * stored in the archive as ".PKG-MANIFEST" and, when manifest_path is not
 * NULL, is also written to that path.
 */
struct pi_archive_options {
    const char *source_dir;
    const char *archive_path;
    const char *manifest_path;
};

int pi_archive_create(const struct pi_archive_options *options);
int pi_archive_extract(const char *archive_path, const char *destination_dir);
int pi_archive_extract_staged(const char *archive_path, const char *destination_dir);
int pi_archive_write_manifest(const char *source_dir, const char *manifest_path);
int pi_archive_verify_manifest(const char *root, const char *manifest_path);

#endif
