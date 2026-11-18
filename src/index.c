// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "index.h"

#include <openssl/evp.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define INDEX_HEADER "PI-INDEX 1\n"
#define PACKAGE_SUFFIX ".pkg.tar.zst"

static int safe_filename(const char *name)
{
    size_t length;

    if (name == NULL || *name == '\0' || name[0] == '.' || strchr(name, '/') != NULL ||
        strchr(name, '\\') != NULL || strstr(name, "..") != NULL) {
        return 0;
    }
    length = strlen(name);
    if (length <= sizeof(PACKAGE_SUFFIX) - 1U ||
        strcmp(name + length - (sizeof(PACKAGE_SUFFIX) - 1U), PACKAGE_SUFFIX) != 0) {
        return 0;
    }
    for (size_t index = 0U; index < length; index++) {
        unsigned char character = (unsigned char)name[index];
        if (character < 0x20U || character == 0x7fU || character == ' ' ||
            character == '\t') {
            return 0;
        }
    }
    return 1;
}

static int hash_fd(int fd, size_t *size, char digest[65])
{
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    unsigned char input[65536];
    unsigned char output[EVP_MAX_MD_SIZE];
    unsigned int length = 0U;
    ssize_t count;
    size_t total = 0U;

    if (context == NULL || EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(context);
        return ENOMEM;
    }
    while ((count = read(fd, input, sizeof(input))) > 0) {
        if (EVP_DigestUpdate(context, input, (size_t)count) != 1 ||
            SIZE_MAX - total < (size_t)count) {
            EVP_MD_CTX_free(context);
            return EOVERFLOW;
        }
        total += (size_t)count;
    }
    if (count < 0 || EVP_DigestFinal_ex(context, output, &length) != 1 || length != 32U) {
        EVP_MD_CTX_free(context);
        return count < 0 ? errno : EIO;
    }
    EVP_MD_CTX_free(context);
    for (unsigned int index = 0U; index < length; index++) {
        (void)snprintf(digest + index * 2U, 3U, "%02x", output[index]);
    }
    digest[64] = '\0';
    *size = total;
    return 0;
}

static int entry_compare(const void *left, const void *right)
{
    const struct pi_index_entry *a = left;
    const struct pi_index_entry *b = right;
    return strcmp(a->filename, b->filename);
}

void pi_index_free(struct pi_index *index)
{
    if (index == NULL) {
        return;
    }
    for (size_t item = 0U; item < index->count; item++) {
        free(index->entries[item].filename);
    }
    free(index->entries);
    *index = (struct pi_index){0};
}

static int add_entry(struct pi_index *index, const char *filename, int fd)
{
    struct stat status;
    struct pi_index_entry *entries;
    int result;

    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
        return errno == 0 ? EINVAL : errno;
    }
    entries = realloc(index->entries, (index->count + 1U) * sizeof(*entries));
    if (entries == NULL) {
        return ENOMEM;
    }
    index->entries = entries;
    index->entries[index->count].filename = strdup(filename);
    if (index->entries[index->count].filename == NULL) {
        return ENOMEM;
    }
    result = hash_fd(fd, &index->entries[index->count].size_bytes,
                     index->entries[index->count].sha256);
    if (result != 0) {
        free(index->entries[index->count].filename);
        return result;
    }
    index->count++;
    return 0;
}

int pi_index_build(const char *repository, const char *index_path)
{
    struct pi_index index = {0};
    DIR *directory = NULL;
    int directory_fd = -1;
    int result = 0;
    struct dirent *entry;
    char temporary[PATH_MAX];
    const char *basename = strrchr(index_path == NULL ? "" : index_path, '/');

    if (repository == NULL || index_path == NULL || *index_path == '\0') {
        return EINVAL;
    }
    basename = basename == NULL ? index_path : basename + 1U;
    directory_fd = open(repository, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_fd < 0) {
        return errno;
    }
    directory = fdopendir(directory_fd);
    if (directory == NULL) {
        result = errno;
        (void)close(directory_fd);
        return result;
    }
    while ((entry = readdir(directory)) != NULL && result == 0) {
        int file;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, basename) == 0) {
            continue;
        }
        if (!safe_filename(entry->d_name)) {
            continue;
        }
        file = openat(dirfd(directory), entry->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (file < 0) {
            result = errno;
        } else {
            result = add_entry(&index, entry->d_name, file);
            (void)close(file);
        }
    }
    if (closedir(directory) != 0 && result == 0) {
        result = errno;
    }
    if (result != 0) {
        pi_index_free(&index);
        return result;
    }
    qsort(index.entries, index.count, sizeof(*index.entries), entry_compare);
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", index_path, (long)getpid()) < 0 ||
        strlen(temporary) >= sizeof(temporary)) {
        pi_index_free(&index);
        return ENAMETOOLONG;
    }
    {
        FILE *output = fopen(temporary, "wx");
        if (output == NULL) {
            result = errno;
        } else {
            (void)fputs(INDEX_HEADER, output);
            for (size_t item = 0U; result == 0 && item < index.count; item++) {
                if (fprintf(output, "%s\t%zu\t%s\n", index.entries[item].filename,
                            index.entries[item].size_bytes, index.entries[item].sha256) < 0) {
                    result = EIO;
                }
            }
            if (fclose(output) != 0 && result == 0) {
                result = errno;
            }
            if (result == 0 && rename(temporary, index_path) != 0) {
                result = errno;
            }
        }
    }
    if (result != 0) {
        (void)unlink(temporary);
    }
    pi_index_free(&index);
    return result;
}

static int valid_digest(const char *digest)
{
    if (strlen(digest) != 64U) {
        return 0;
    }
    for (size_t index = 0U; index < 64U; index++) {
        if (!((digest[index] >= '0' && digest[index] <= '9') ||
              (digest[index] >= 'a' && digest[index] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

int pi_index_load(const char *index_path, struct pi_index *index)
{
    FILE *input;
    char line[PATH_MAX + 100U];
    unsigned long long size;

    if (index_path == NULL || index == NULL) {
        return EINVAL;
    }
    *index = (struct pi_index){0};
    input = fopen(index_path, "r");
    if (input == NULL) {
        return errno;
    }
    if (fgets(line, sizeof(line), input) == NULL || strcmp(line, INDEX_HEADER) != 0) {
        (void)fclose(input);
        return EBADMSG;
    }
    while (fgets(line, sizeof(line), input) != NULL) {
        char filename[PATH_MAX];
        char digest[65];
        char *first_tab;
        char *second_tab;
        char *end;
        unsigned long long parsed_size;

        if (strchr(line, '\n') == NULL) {
            pi_index_free(index);
            (void)fclose(input);
            return EBADMSG;
        }
        line[strlen(line) - 1U] = '\0';
        first_tab = strchr(line, '\t');
        second_tab = first_tab == NULL ? NULL : strchr(first_tab + 1U, '\t');
        if (first_tab == NULL || second_tab == NULL || strchr(second_tab + 1U, '\t') != NULL) {
            pi_index_free(index);
            (void)fclose(input);
            return EBADMSG;
        }
        *first_tab = '\0';
        *second_tab = '\0';
        if (first_tab[1] == '\0') {
            pi_index_free(index);
            (void)fclose(input);
            return EBADMSG;
        }
        for (char *cursor = first_tab + 1U; *cursor != '\0'; cursor++) {
            if (*cursor < '0' || *cursor > '9') {
                pi_index_free(index);
                (void)fclose(input);
                return EBADMSG;
            }
        }
        errno = 0;
        parsed_size = strtoull(first_tab + 1U, &end, 10);
        if (errno != 0 || end == first_tab + 1U || *end != '\0' ||
            parsed_size > SIZE_MAX || snprintf(filename, sizeof(filename), "%s", line) < 0 ||
            strlen(line) >= sizeof(filename) ||
            snprintf(digest, sizeof(digest), "%s", second_tab + 1U) < 0 ||
            strlen(second_tab + 1U) >= sizeof(digest) || !safe_filename(filename) ||
            !valid_digest(digest)) {
            pi_index_free(index);
            (void)fclose(input);
            return EBADMSG;
        }
        if (index->count > 0U &&
            strcmp(index->entries[index->count - 1U].filename, filename) >= 0) {
            pi_index_free(index);
            (void)fclose(input);
            return EBADMSG;
        }
        {
            struct pi_index_entry *entries = realloc(
                index->entries, (index->count + 1U) * sizeof(*entries));
            if (entries == NULL) {
                pi_index_free(index);
                (void)fclose(input);
                return ENOMEM;
            }
            index->entries = entries;
            index->entries[index->count].filename = strdup(filename);
            if (index->entries[index->count].filename == NULL) {
                pi_index_free(index);
                (void)fclose(input);
                return ENOMEM;
            }
            size = parsed_size;
            index->entries[index->count].size_bytes = (size_t)size;
            (void)snprintf(index->entries[index->count].sha256, sizeof(digest), "%s", digest);
            index->count++;
        }
    }
    if (ferror(input) != 0 || fclose(input) != 0) {
        pi_index_free(index);
        return EIO;
    }
    return 0;
}

int pi_index_verify(const char *repository, const struct pi_index *index)
{
    int directory;

    if (repository == NULL || index == NULL) {
        return EINVAL;
    }
    directory = open(repository, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) {
        return errno;
    }
    for (size_t item = 0U; item < index->count; item++) {
        struct stat status;
        size_t size;
        char digest[65];
        int file = openat(directory, index->entries[item].filename,
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        int result;

        if (file < 0) {
            result = errno;
        } else {
            result = fstat(file, &status) == 0 && S_ISREG(status.st_mode)
                          ? hash_fd(file, &size, digest)
                          : EINVAL;
            if (result == 0 && (size != index->entries[item].size_bytes ||
                                strcmp(digest, index->entries[item].sha256) != 0)) {
                result = EBADMSG;
            }
            (void)close(file);
        }
        if (result != 0) {
            (void)close(directory);
            return result;
        }
    }
    (void)close(directory);
    return 0;
}
