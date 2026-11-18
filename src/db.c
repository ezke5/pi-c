// SPDX-License-Identifier: GPL-3.0-or-later
#include "db.h"
#include "hooks.h"

#include <openssl/evp.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool safe_relative(const char *path)
{
    return path != NULL && path[0] != '/' && strstr(path, "..") == NULL && *path != '\0';
}

static int mkdir_parents(const char *path, mode_t mode)
{
    char copy[PATH_MAX];
    if (snprintf(copy, sizeof(copy), "%s", path) < 0 || strlen(path) >= sizeof(copy)) return ENAMETOOLONG;
    for (char *cursor = copy + 1U; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(copy, mode) != 0 && errno != EEXIST) return errno;
            *cursor = '/';
        }
    }
    if (mkdir(copy, mode) != 0 && errno != EEXIST) return errno;
    return 0;
}

static int package_dir(char *out, size_t size, const char *root, const char *pkgname)
{
    if (pkgname == NULL || !pi_hook_valid_package_name(pkgname)) return EINVAL;
    int written = snprintf(out, size, "%s/var/db/pi/installed/%s", root, pkgname);
    return written < 0 || (size_t)written >= size ? ENAMETOOLONG : 0;
}

static int hash_path(const char *path, bool symlink, char output[65])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char digest[EVP_MAX_MD_SIZE], buffer[8192];
    unsigned int length = 0U;
    int fd = -1;
    ssize_t count;
    char link_target[PATH_MAX];
    ssize_t link_length;
    if (ctx == NULL || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx); return ENOMEM;
    }
    if (symlink) {
        link_length = readlink(path, link_target, sizeof(link_target) - 1U);
        if (link_length < 0) { EVP_MD_CTX_free(ctx); return errno; }
        if (EVP_DigestUpdate(ctx, link_target, (size_t)link_length) != 1) {
            EVP_MD_CTX_free(ctx); return EIO;
        }
    } else {
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) { EVP_MD_CTX_free(ctx); return errno; }
        while ((count = read(fd, buffer, sizeof(buffer))) > 0) {
            if (EVP_DigestUpdate(ctx, buffer, (size_t)count) != 1) {
                (void)close(fd); EVP_MD_CTX_free(ctx); return EIO;
            }
        }
        if (count < 0) { (void)close(fd); EVP_MD_CTX_free(ctx); return errno; }
    }
    if (EVP_DigestFinal_ex(ctx, digest, &length) != 1 || length != 32U) {
        if (fd >= 0) (void)close(fd); EVP_MD_CTX_free(ctx); return EIO;
    }
    if (fd >= 0) (void)close(fd);
    EVP_MD_CTX_free(ctx);
    for (unsigned int index = 0U; index < length; index++) (void)snprintf(output + index * 2U, 3U, "%02x", digest[index]);
    output[64] = '\0';
    return 0;
}

static int atomic_write(const char *path, const char *content)
{
    char temporary[PATH_MAX];
    int fd;
    size_t length = strlen(content);
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid()) < 0 ||
        strlen(temporary) >= sizeof(temporary)) return ENAMETOOLONG;
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return errno;
    if (write(fd, content, length) != (ssize_t)length || fsync(fd) != 0 ||
        close(fd) != 0 || rename(temporary, path) != 0) {
        int error = errno; (void)unlink(temporary); return error;
    }
    return 0;
}

void pi_package_metadata_free(struct pi_package_metadata *metadata)
{
    if (metadata == NULL) return;
    free(metadata->pkgname); free(metadata->pkgver); free(metadata->pkgrel);
    free(metadata->arch); free(metadata->use_flags); free(metadata->use_hash);
    free(metadata->build_date); free(metadata->install_reason);
    *metadata = (struct pi_package_metadata){0};
}

int pi_db_write_metadata(const char *root, const struct pi_package_metadata *metadata)
{
    char directory[PATH_MAX], path[PATH_MAX], content[4096];
    if (root == NULL || metadata == NULL || metadata->pkgname == NULL ||
        package_dir(directory, sizeof(directory), root, metadata->pkgname) != 0 ||
        mkdir_parents(directory, 0755) != 0 ||
        snprintf(path, sizeof(path), "%s/meta", directory) < 0 ||
        strlen(path) >= sizeof(path) ||
        snprintf(content, sizeof(content), "pkgname=%s\npkgver=%s\npkgrel=%s\narch=%s\nuse_flags=%s\nuse_hash=%s\nbuild_date=%s\ninstall_reason=%s\nsize_bytes=%zu\n",
                 metadata->pkgname, metadata->pkgver ? metadata->pkgver : "",
                 metadata->pkgrel ? metadata->pkgrel : "", metadata->arch ? metadata->arch : "",
                 metadata->use_flags ? metadata->use_flags : "", metadata->use_hash ? metadata->use_hash : "",
                 metadata->build_date ? metadata->build_date : "",
                 metadata->install_reason ? metadata->install_reason : "", metadata->size_bytes) < 0 ||
        strlen(content) >= sizeof(content)) return EINVAL;
    return atomic_write(path, content);
}

int pi_db_check_conflicts(const char *root, const char *pkgname, const char *manifest, const char *backup)
{
    FILE *file;
    char line[PATH_MAX + 96U];
    (void)backup;
    if (root == NULL || pkgname == NULL || manifest == NULL || !pi_hook_valid_package_name(pkgname)) return EINVAL;
    file = fopen(manifest, "r");
    if (file == NULL) return errno;
    while (fgets(line, sizeof(line), file) != NULL) {
        char mode[32], digest[65], relative[PATH_MAX], target[PATH_MAX];
        struct stat status;
        if (sscanf(line, "%31s %64s %4095[^\n]", mode, digest, relative) != 3 ||
            !safe_relative(relative) || snprintf(target, sizeof(target), "%s/%s", root, relative) < 0 ||
            strlen(target) >= sizeof(target)) { (void)fclose(file); return EINVAL; }
        if (lstat(target, &status) == 0) { (void)fclose(file); return EEXIST; }
        if (errno != ENOENT) { int error = errno; (void)fclose(file); return error; }
    }
    (void)fclose(file);
    return 0;
}

static int copy_file(const char *source, const char *destination)
{
    FILE *input = fopen(source, "rb"), *output;
    char buffer[8192];
    size_t length;
    if (input == NULL) return errno;
    output = fopen(destination, "wb");
    if (output == NULL) { int error = errno; (void)fclose(input); return error; }
    while ((length = fread(buffer, 1U, sizeof(buffer), input)) > 0U) (void)fwrite(buffer, 1U, length, output);
    (void)fclose(input);
    return fclose(output) == 0 ? 0 : EIO;
}

int pi_db_install_commit(const struct pi_db_package *package, const char *staging_root)
{
    char database[PATH_MAX], source[PATH_MAX], destination[PATH_MAX];
    DIR *directory;
    struct dirent *entry;
    int result;
    if (package == NULL || package->root == NULL || package->pkgname == NULL ||
        package->metadata == NULL || staging_root == NULL) return EINVAL;
    result = pi_db_check_conflicts(package->root, package->pkgname, package->manifest, package->backup);
    if (result != 0) return result;
    if (package_dir(database, sizeof(database), package->root, package->pkgname) != 0 ||
        mkdir_parents(database, 0755) != 0 || snprintf(source, sizeof(source), "%s", staging_root) < 0 ||
        strlen(source) >= sizeof(source)) return EINVAL;
    directory = opendir(source);
    if (directory == NULL) return errno;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (snprintf(destination, sizeof(destination), "%s/%s", package->root, entry->d_name) < 0 ||
            strlen(destination) >= sizeof(destination)) { (void)closedir(directory); return ENAMETOOLONG; }
        if (snprintf(source, sizeof(source), "%s/%s", staging_root, entry->d_name) < 0 ||
            strlen(source) >= sizeof(source)) { (void)closedir(directory); return ENAMETOOLONG; }
        (void)unlink(destination);
        if (rename(source, destination) != 0) { (void)closedir(directory); return errno; }
    }
    (void)closedir(directory);
    result = pi_db_write_metadata(package->root, package->metadata);
    if (result != 0) return result;
    const char *files[] = {package->manifest, package->depends, package->backup, package->hooks};
    const char *names[] = {"manifest", "depends", "backup", "hooks"};
    for (size_t index = 0U; index < 4U; index++) {
        if (files[index] == NULL) continue;
        if (snprintf(destination, sizeof(destination), "%s/%s", database, names[index]) < 0 ||
            strlen(destination) >= sizeof(destination)) return ENAMETOOLONG;
        result = copy_file(files[index], destination);
        if (result != 0) return result;
    }
    return 0;
}

int pi_db_verify(const char *root, const char *pkgname, bool *ok)
{
    char directory[PATH_MAX], manifest[PATH_MAX], line[PATH_MAX + 96U];
    FILE *file;
    bool valid = true;
    if (ok == NULL || package_dir(directory, sizeof(directory), root, pkgname) != 0 ||
        snprintf(manifest, sizeof(manifest), "%s/manifest", directory) < 0 ||
        strlen(manifest) >= sizeof(manifest)) return EINVAL;
    file = fopen(manifest, "r");
    if (file == NULL) return errno;
    while (fgets(line, sizeof(line), file) != NULL) {
        char mode[32], expected[65], relative[PATH_MAX], actual[65], path[PATH_MAX];
        struct stat status;
        if (sscanf(line, "%31s %64s %4095[^\n]", mode, expected, relative) != 3 ||
            !safe_relative(relative) || snprintf(path, sizeof(path), "%s/%s", root, relative) < 0 ||
            strlen(path) >= sizeof(path) || lstat(path, &status) != 0 ||
            hash_path(path, S_ISLNK(status.st_mode), actual) != 0 || strcmp(expected, actual) != 0) valid = false;
        (void)mode;
    }
    (void)fclose(file);
    *ok = valid;
    return 0;
}

static int remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    if (directory != NULL) {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            char child[PATH_MAX];
            if (entry->d_name[0] == '.') continue;
            if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) < 0 ||
                strlen(child) >= sizeof(child) || remove_tree(child) != 0) { (void)closedir(directory); return -1; }
        }
        (void)closedir(directory);
        return rmdir(path) == 0 ? 0 : errno;
    }
    return unlink(path) == 0 || errno == ENOENT ? 0 : errno;
}

int pi_db_remove(const char *root, const char *pkgname, bool force)
{
    char directory[PATH_MAX];
    (void)force;
    if (package_dir(directory, sizeof(directory), root, pkgname) != 0) return EINVAL;
    if (pi_hook_run(root, pkgname, "pre_remove") != 0) return -1;
    if (pi_hook_run(root, pkgname, "post_remove") != 0) return -1;
    return remove_tree(directory);
}

int pi_db_info(const char *root, const char *pkgname, FILE *output)
{
    char directory[PATH_MAX], path[PATH_MAX], buffer[4096];
    FILE *input;
    if (output == NULL || package_dir(directory, sizeof(directory), root, pkgname) != 0 ||
        snprintf(path, sizeof(path), "%s/meta", directory) < 0 || strlen(path) >= sizeof(path)) return EINVAL;
    input = fopen(path, "r");
    if (input == NULL) return errno;
    while (fgets(buffer, sizeof(buffer), input) != NULL) (void)fputs(buffer, output);
    (void)fclose(input);
    return 0;
}

int pi_db_lock(const char *root, int *lock_fd)
{
    char directory[PATH_MAX], path[PATH_MAX];
    int fd;
    if (root == NULL || lock_fd == NULL || snprintf(directory, sizeof(directory), "%s/var/lock", root) < 0 ||
        strlen(directory) >= sizeof(directory) || mkdir_parents(directory, 0755) != 0 ||
        snprintf(path, sizeof(path), "%s/pi.lock", directory) < 0 || strlen(path) >= sizeof(path)) return EINVAL;
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return errno;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) { int error = errno; (void)close(fd); return error; }
    *lock_fd = fd;
    return 0;
}

void pi_db_unlock(int lock_fd)
{
    if (lock_fd >= 0) { (void)flock(lock_fd, LOCK_UN); (void)close(lock_fd); }
}

static int string_pointer_compare(const void *left, const void *right)
{
    const char * const *a = left;
    const char * const *b = right;
    return strcmp(*a, *b);
}

static int world_update(const char *root, const char *pkgname, bool add)
{
    char path[PATH_MAX], temporary[PATH_MAX], line[PATH_MAX], parent[PATH_MAX];
    char *entries[4096];
    size_t count = 0U;
    bool present = false;
    FILE *input, *output;
    if (root == NULL || pkgname == NULL || !pi_hook_valid_package_name(pkgname) ||
        snprintf(path, sizeof(path), "%s/var/db/pi/world", root) < 0 || strlen(path) >= sizeof(path) ||
        snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid()) < 0 ||
        strlen(temporary) >= sizeof(temporary)) return EINVAL;
    input = fopen(path, "r");
    if (input != NULL) {
        while (count < 4096U && fgets(line, sizeof(line), input) != NULL) {
            line[strcspn(line, "\r\n")] = '\0';
            if (*line == '\0' || !pi_hook_valid_package_name(line)) continue;
            if (strcmp(line, pkgname) == 0) present = true;
            entries[count] = strdup(line);
            if (entries[count] == NULL) { (void)fclose(input); return ENOMEM; }
            count++;
        }
        (void)fclose(input);
    }
    if (add && !present && count < 4096U) entries[count++] = strdup(pkgname);
    if (!add) {
        size_t write = 0U;
        for (size_t index = 0U; index < count; index++) {
            if (strcmp(entries[index], pkgname) == 0) free(entries[index]);
            else entries[write++] = entries[index];
        }
        count = write;
    }
    qsort(entries, count, sizeof(*entries), string_pointer_compare);
    if (snprintf(parent, sizeof(parent), "%s/var/db/pi", root) < 0 ||
        strlen(parent) >= sizeof(parent) || mkdir_parents(parent, 0755) != 0) return EIO;
    output = fopen(temporary, "w");
    if (output == NULL) return errno;
    for (size_t index = 0U; index < count; index++) { (void)fprintf(output, "%s\n", entries[index]); free(entries[index]); }
    if (fclose(output) != 0 || rename(temporary, path) != 0) { (void)unlink(temporary); return EIO; }
    return 0;
}

int pi_db_world_add(const char *root, const char *pkgname) { return world_update(root, pkgname, true); }
int pi_db_world_remove(const char *root, const char *pkgname) { return world_update(root, pkgname, false); }

int pi_db_clean_cache(const char *root, unsigned int keep)
{
    char path[PATH_MAX];
    DIR *directory;
    struct dirent *entry;
    unsigned int preserved = 0U;
    if (root == NULL || snprintf(path, sizeof(path), "%s/var/cache/pi/binpkgs", root) < 0 ||
        strlen(path) >= sizeof(path)) return EINVAL;
    directory = opendir(path);
    if (directory == NULL) return errno == ENOENT ? 0 : errno;
    while ((entry = readdir(directory)) != NULL) {
        char file[PATH_MAX];
        if (entry->d_name[0] == '.' || strstr(entry->d_name, ".pkg.tar") == NULL) continue;
        if (preserved++ >= keep && snprintf(file, sizeof(file), "%s/%s", path, entry->d_name) > 0 &&
            strlen(file) < sizeof(file)) (void)unlink(file);
    }
    (void)closedir(directory);
    return 0;
}
