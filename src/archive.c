// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "pi_archive.h"

#include <archive.h>
#include <archive_entry.h>
#include <openssl/evp.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PI_MANIFEST_NAME ".MANIFEST"

struct manifest {
    char **lines;
    size_t count;
    size_t length;
};

static int safe_name(const char *name)
{
    const char *part = name;

    if (name == NULL || *name == '\0' || *name == '/' || strchr(name, '\\') != NULL) {
        return 0;
    }
    while (*part != '\0') {
        const char *end = strchr(part, '/');
        size_t length = end == NULL ? strlen(part) : (size_t)(end - part);

        if (length == 0U || (length == 1U && part[0] == '.') ||
            (length == 2U && part[0] == '.' && part[1] == '.')) {
            return 0;
        }
        if (end == NULL) {
            break;
        }
        part = end + 1;
    }
    return 1;
}

static int join_path(char *out, size_t size, const char *left, const char *right)
{
    int written = snprintf(out, size, "%s/%s", left, right);
    return written < 0 ? EIO : ((size_t)written >= size ? ENAMETOOLONG : 0);
}

static int make_parent_directories(const char *path)
{
    char copy[PATH_MAX];
    char *slash;

    if (snprintf(copy, sizeof(copy), "%s", path) < 0 || strlen(path) >= sizeof(copy)) {
        return ENAMETOOLONG;
    }
    slash = strrchr(copy, '/');
    if (slash == NULL) return 0;
    *slash = '\0';
    for (char *cursor = copy + 1U; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) return errno;
            *cursor = '/';
        }
    }
    if (*copy != '\0' && mkdir(copy, 0755) != 0 && errno != EEXIST) return errno;
    return 0;
}

static int manifest_add(struct manifest *manifest, const char *line)
{
    size_t length = strlen(line);
    char *copy = malloc(length + 1U);
    char **lines;

    if (copy == NULL) {
        return ENOMEM;
    }
    memcpy(copy, line, length + 1U);
    lines = realloc(manifest->lines, (manifest->count + 1U) * sizeof(*lines));
    if (lines == NULL) {
        free(copy);
        return ENOMEM;
    }
    manifest->lines = lines;
    manifest->lines[manifest->count++] = copy;
    manifest->length += length;
    return 0;
}

static void manifest_free(struct manifest *manifest)
{
    for (size_t index = 0U; index < manifest->count; index++) {
        free(manifest->lines[index]);
    }
    free(manifest->lines);
    *manifest = (struct manifest){0};
}

static int compare_lines(const void *left, const void *right)
{
    const char * const *a = left;
    const char * const *b = right;
    return strcmp(*a, *b);
}

static int hash_file(const char *path, char digest[65])
{
    unsigned char bytes[65536];
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_length = 0U;
    EVP_MD_CTX *context = NULL;
    FILE *file = NULL;
    size_t length;
    int result = -1;

    file = fopen(path, "rb");
    if (file == NULL) {
        return errno;
    }
    context = EVP_MD_CTX_new();
    if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        goto done;
    }
    while ((length = fread(bytes, 1U, sizeof(bytes), file)) != 0U) {
        if (EVP_DigestUpdate(context, bytes, length) != 1) {
            goto done;
        }
    }
    if (ferror(file) != 0 || EVP_DigestFinal_ex(context, hash, &hash_length) != 1 ||
        hash_length != 32U) {
        goto done;
    }
    for (unsigned int index = 0U; index < hash_length; index++) {
        (void)snprintf(digest + index * 2U, 3U, "%02x", hash[index]);
    }
    digest[64] = '\0';
    result = 0;
done:
    EVP_MD_CTX_free(context);
    if (fclose(file) != 0 && result == 0) {
        result = errno;
    }
    return result;
}

static int collect_manifest(const char *root, const char *relative,
                            struct manifest *manifest)
{
    char path[PATH_MAX];
    DIR *directory;
    struct dirent *entry;
    int result;

    result = relative == NULL ? snprintf(path, sizeof(path), "%s", root) < 0 ? EIO :
        ((strlen(root) >= sizeof(path)) ? ENAMETOOLONG : 0) :
        join_path(path, sizeof(path), root, relative);
    if (result != 0) {
        return result;
    }
    directory = opendir(path);
    if (directory == NULL) {
        return errno;
    }
    while ((entry = readdir(directory)) != NULL) {
        struct stat status;
        char child[PATH_MAX];
        char child_relative[PATH_MAX];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (relative == NULL) {
            result = snprintf(child_relative, sizeof(child_relative), "%s", entry->d_name);
        } else {
            result = snprintf(child_relative, sizeof(child_relative), "%s/%s", relative,
                              entry->d_name);
        }
        if (result < 0 || (size_t)result >= sizeof(child_relative) ||
            !safe_name(child_relative) || join_path(child, sizeof(child), root, child_relative) != 0) {
            result = ENAMETOOLONG;
            break;
        }
        if (lstat(child, &status) != 0) {
            result = errno;
            break;
        }
        if (S_ISDIR(status.st_mode)) {
            result = collect_manifest(root, child_relative, manifest);
        } else if (S_ISREG(status.st_mode)) {
            char digest[65];
            char line[PATH_MAX + 70U];

            result = hash_file(child, digest);
            if (result == 0) {
                int written = snprintf(line, sizeof(line), "%s  %s\n", digest, child_relative);
                result = written < 0 || (size_t)written >= sizeof(line) ? ENAMETOOLONG :
                    manifest_add(manifest, line);
            }
        } else {
            result = EINVAL; /* Device files and symlinks are never packaged. */
        }
        if (result != 0) {
            break;
        }
    }
    if (closedir(directory) != 0 && result == 0) {
        result = errno;
    }
    return result;
}

static int manifest_load(const char *root, struct manifest *manifest)
{
    int result = collect_manifest(root, NULL, manifest);
    if (result == 0) {
        qsort(manifest->lines, manifest->count, sizeof(*manifest->lines), compare_lines);
    }
    return result;
}

static int manifest_write(const struct manifest *manifest, const char *path)
{
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return errno;
    }
    for (size_t index = 0U; index < manifest->count; index++) {
        if (fputs(manifest->lines[index], file) == EOF) {
            (void)fclose(file);
            return EIO;
        }
    }
    return fclose(file) == 0 ? 0 : errno;
}

static int archive_add_file(struct archive *output, const char *root, const char *relative)
{
    char path[PATH_MAX];
    struct stat status;
    struct archive_entry *entry;
    FILE *file = NULL;
    unsigned char buffer[65536];
    size_t length;
    int result;

    result = join_path(path, sizeof(path), root, relative);
    if (result != 0) {
        return result;
    }
    int descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 || fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        int error = errno;
        if (descriptor >= 0) (void)close(descriptor);
        return error == 0 ? EINVAL : error;
    }
    entry = archive_entry_new();
    if (entry == NULL) {
        return ENOMEM;
    }
    archive_entry_set_pathname(entry, relative);
    archive_entry_set_mode(entry, status.st_mode);
    archive_entry_set_size(entry, status.st_size);
    archive_entry_set_uid(entry, 0);
    archive_entry_set_gid(entry, 0);
    archive_entry_set_mtime(entry, 0, 0);
    file = fdopen(descriptor, "rb");
    if (file == NULL) {
        (void)close(descriptor);
        archive_entry_free(entry);
        return errno;
    }
    result = archive_write_header(output, entry) == ARCHIVE_OK ? 0 : EIO;
    while (result == 0 && (length = fread(buffer, 1U, sizeof(buffer), file)) != 0U) {
        if (archive_write_data(output, buffer, length) < 0) {
            result = EIO;
        }
    }
    if (ferror(file) != 0 && result == 0) {
        result = EIO;
    }
    (void)fclose(file);
    archive_entry_free(entry);
    return result;
}

static int archive_add_directories(struct archive *output, const char *root,
                                   const char *relative)
{
    char path[PATH_MAX];
    DIR *directory;
    struct dirent *entry;
    int result = join_path(path, sizeof(path), root, relative);

    if (result != 0) {
        return result;
    }
    directory = opendir(path);
    if (directory == NULL) {
        return errno;
    }
    while ((entry = readdir(directory)) != NULL && result == 0) {
        char child_relative[PATH_MAX];
        char child_path[PATH_MAX];
        struct stat status;
        int written = *relative == '\0'
                          ? snprintf(child_relative, sizeof(child_relative), "%s", entry->d_name)
                          : snprintf(child_relative, sizeof(child_relative), "%s/%s", relative,
                                     entry->d_name);

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (written < 0 || (size_t)written >= sizeof(child_relative) ||
            join_path(child_path, sizeof(child_path), root, child_relative) != 0 ||
            lstat(child_path, &status) != 0) {
            result = EINVAL;
        } else if (S_ISDIR(status.st_mode)) {
            struct archive_entry *archive_dir = archive_entry_new();
            if (archive_dir == NULL) {
                result = ENOMEM;
            } else {
                archive_entry_set_pathname(archive_dir, child_relative);
                archive_entry_set_filetype(archive_dir, AE_IFDIR);
                archive_entry_set_perm(archive_dir, status.st_mode & 0777U);
                archive_entry_set_mtime(archive_dir, 0, 0);
                result = archive_write_header(output, archive_dir) == ARCHIVE_OK ? 0 : EIO;
                archive_entry_free(archive_dir);
                if (result == 0) {
                    result = archive_add_directories(output, root, child_relative);
                }
            }
        } else if (!S_ISREG(status.st_mode)) {
            result = EINVAL;
        }
    }
    if (closedir(directory) != 0 && result == 0) {
        result = errno;
    }
    return result;
}

int pi_archive_write_manifest(const char *source_dir, const char *manifest_path)
{
    struct manifest manifest = {0};
    int result;

    if (source_dir == NULL || manifest_path == NULL) {
        return EINVAL;
    }
    result = manifest_load(source_dir, &manifest);
    if (result == 0) {
        result = manifest_write(&manifest, manifest_path);
    }
    manifest_free(&manifest);
    return result;
}

int pi_archive_create(const struct pi_archive_options *options)
{
    struct manifest manifest = {0};
    struct archive *output = NULL;
    struct archive_entry *entry = NULL;
    int result;

    if (options == NULL || options->source_dir == NULL || options->archive_path == NULL) {
        return EINVAL;
    }
    result = manifest_load(options->source_dir, &manifest);
    if (result != 0) {
        goto done;
    }
    if (options->manifest_path != NULL) {
        result = manifest_write(&manifest, options->manifest_path);
        if (result != 0) {
            goto done;
        }
    }
    output = archive_write_new();
    if (output == NULL || archive_write_add_filter_zstd(output) != ARCHIVE_OK ||
        archive_write_set_format_pax_restricted(output) != ARCHIVE_OK ||
        archive_write_open_filename(output, options->archive_path) != ARCHIVE_OK) {
        result = EIO;
        goto done;
    }
    for (size_t index = 0U; index < manifest.count && result == 0; index++) {
        const char *line = manifest.lines[index] + 66U;
        char relative[PATH_MAX];
        size_t length = strlen(line);

        if (length < 2U || length - 1U >= sizeof(relative)) {
            result = EINVAL;
            break;
        }
        memcpy(relative, line, length - 1U);
        relative[length - 1U] = '\0';
        result = archive_add_file(output, options->source_dir, relative);
    }
    if (result == 0) {
        result = archive_add_directories(output, options->source_dir, "");
    }
    if (result == 0) {
        entry = archive_entry_new();
        if (entry == NULL) {
            result = ENOMEM;
        } else {
            archive_entry_set_pathname(entry, PI_MANIFEST_NAME);
            archive_entry_set_mode(entry, 0644);
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_size(entry, (la_int64_t)manifest.length);
            result = archive_write_header(output, entry) == ARCHIVE_OK ? 0 : EIO;
            for (size_t index = 0U; result == 0 && index < manifest.count; index++) {
                if (archive_write_data(output, manifest.lines[index],
                                       strlen(manifest.lines[index])) < 0) {
                    result = EIO;
                }
            }
        }
    }
done:
    archive_entry_free(entry);
    if (output != NULL) {
        if (archive_write_close(output) != ARCHIVE_OK && result == 0) {
            result = EIO;
        }
        archive_write_free(output);
    }
    manifest_free(&manifest);
    return result;
}

static int destination_path(const char *root, const char *name, char path[PATH_MAX])
{
    if (!safe_name(name)) {
        return EINVAL;
    }
    return join_path(path, PATH_MAX, root, name);
}

int pi_archive_extract(const char *archive_path, const char *destination_dir)
{
    struct archive *input;
    struct archive_entry *entry;
    struct stat status;
    int header_result;
    int result = 0;

    if (archive_path == NULL || destination_dir == NULL) {
        return EINVAL;
    }

    if (lstat(destination_dir, &status) != 0) {
        if (mkdir(destination_dir, 0755) != 0) {
            return errno;
        }
    } else if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode)) {
        return ENOTDIR;
    }
    input = archive_read_new();
    if (input == NULL || archive_read_support_filter_all(input) != ARCHIVE_OK ||
        archive_read_support_format_all(input) != ARCHIVE_OK ||
        archive_read_open_filename(input, archive_path, 65536) != ARCHIVE_OK) {
        archive_read_free(input);
        return EIO;
    }
    while (result == 0 &&
           (header_result = archive_read_next_header(input, &entry)) != ARCHIVE_EOF) {
        const char *name = archive_entry_pathname(entry);
        char path[PATH_MAX];
        mode_t mode = archive_entry_mode(entry) & 0777U;

        if (header_result < ARCHIVE_OK) {
            result = EIO;
            break;
        }
        result = destination_path(destination_dir, name, path);
        if (result != 0 || archive_entry_symlink(entry) != NULL ||
            archive_entry_hardlink(entry) != NULL) {
            result = EINVAL;
            break;
        }
        if (archive_entry_filetype(entry) == AE_IFDIR) {
            if (mkdir(path, mode == 0U ? 0755 : mode) != 0 && errno != EEXIST) {
                result = errno;
            }
        } else if (archive_entry_filetype(entry) == AE_IFREG) {
            int file;
            unsigned char buffer[65536];
            la_ssize_t length;

            if (make_parent_directories(path) != 0) {
                result = errno;
            } else if ((file = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                                    mode == 0U ? 0644 : mode)) < 0) {
                result = errno;
            } else {
                while ((length = archive_read_data(input, buffer, sizeof(buffer))) > 0) {
                    if (write(file, buffer, (size_t)length) != length) {
                        result = EIO;
                        break;
                    }
                }
                if (length < 0 && result == 0) {
                    result = EIO;
                }
                if (close(file) != 0 && result == 0) {
                    result = errno;
                }
            }
        } else {
            result = EINVAL;
        }
    }
    archive_read_close(input);
    archive_read_free(input);
    return result;
}

int pi_archive_extract_staged(const char *archive_path, const char *destination_dir)
{
    return pi_archive_extract(archive_path, destination_dir);
}

int pi_archive_verify_manifest(const char *root, const char *manifest_path)
{
    struct manifest expected = {0};
    struct manifest actual = {0};
    FILE *file;
    char *line = NULL;
    size_t capacity = 0U;
    int result;

    if (root == NULL || manifest_path == NULL) {
        return EINVAL;
    }
    file = fopen(manifest_path, "r");
    if (file == NULL) {
        return errno;
    }
    while (getline(&line, &capacity, file) >= 0) {
        if (manifest_add(&expected, line) != 0) {
            free(line);
            fclose(file);
            manifest_free(&expected);
            return ENOMEM;
        }
    }
    free(line);
    result = ferror(file) != 0 || fclose(file) != 0 ? EIO : 0;
    if (result == 0) {
        result = manifest_load(root, &actual);
    }
    if (result == 0) {
        qsort(expected.lines, expected.count, sizeof(*expected.lines), compare_lines);
        result = expected.count == actual.count ? 0 : EBADMSG;
        for (size_t index = 0U; result == 0 && index < expected.count; index++) {
            if (strcmp(expected.lines[index], actual.lines[index]) != 0) {
                result = EBADMSG;
            }
        }
    }
    manifest_free(&expected);
    manifest_free(&actual);
    return result;
}
