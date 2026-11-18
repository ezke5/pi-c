// SPDX-License-Identifier: GPL-3.0-or-later
#include "build.h"
#include "conf.h"
#include "net.h"
#include "pi_archive.h"
#include "sandbox.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct build_metadata {
    char *pkgname;
    char *pkgver;
    char *pkgrel;
    char *pkgdesc;
    char *arch;
    char *license;
    char *depends;
    char *makedepends;
    char *use_flags;
    char **sources;
    size_t source_count;
    char **checksums;
    size_t checksum_count;
    bool prepare;
    bool build;
    bool check;
    bool package;
};

struct build_settings {
    char *ports_dir;
    char *use;
    char *cflags;
    char *cxxflags;
    char *ldflags;
    char *makeflags;
    char *max_jobs;
};

static int parse_values(char *value, char ***items, size_t *count);

static void metadata_free(struct build_metadata *metadata)
{
    free(metadata->pkgname);
    free(metadata->pkgver);
    free(metadata->pkgrel);
    free(metadata->pkgdesc);
    free(metadata->arch);
    free(metadata->license);
    free(metadata->depends);
    free(metadata->makedepends);
    free(metadata->use_flags);
    for (size_t index = 0U; index < metadata->source_count; index++) free(metadata->sources[index]);
    for (size_t index = 0U; index < metadata->checksum_count; index++) free(metadata->checksums[index]);
    free(metadata->sources);
    free(metadata->checksums);
    *metadata = (struct build_metadata){0};
}

static void settings_free(struct build_settings *settings)
{
    free(settings->ports_dir);
    free(settings->use);
    free(settings->cflags);
    free(settings->cxxflags);
    free(settings->ldflags);
    free(settings->makeflags);
    free(settings->max_jobs);
    *settings = (struct build_settings){0};
}

static char *copy_text(const char *text)
{
    size_t length = text == NULL ? 0U : strlen(text);
    char *copy = calloc(length + 1U, 1U);
    if (copy != NULL && length > 0U) memcpy(copy, text, length);
    return copy;
}

static void trim(char *text)
{
    char *start = text;
    size_t length;
    while (*start == ' ' || *start == '\t') start++;
    if (start != text) memmove(text, start, strlen(start) + 1U);
    length = strlen(text);
    while (length > 0U && (text[length - 1U] == ' ' || text[length - 1U] == '\t' ||
                           text[length - 1U] == '\r' || text[length - 1U] == '\n')) {
        text[--length] = '\0';
    }
}

static int set_text(char **destination, const char *value)
{
    char *copy = copy_text(value);
    if (copy == NULL) return ENOMEM;
    free(*destination);
    *destination = copy;
    return 0;
}

static int set_scalar(char **destination, char *value)
{
    char *items = NULL;
    char **values = NULL;
    size_t count = 0U;
    int result = parse_values(value, &values, &count);
    if (result == 0) {
        items = count > 0U ? copy_text(values[0]) : copy_text("");
        if (items == NULL) result = ENOMEM;
        else {
            free(*destination);
            *destination = items;
        }
    }
    for (size_t index = 0U; index < count; index++) free(values[index]);
    free(values);
    return result;
}

static int set_list(char **destination, char *value)
{
    char **items = NULL;
    size_t count = 0U;
    size_t length = 0U;
    char *text;
    int result = parse_values(value, &items, &count);

    if (result != 0) return result;
    for (size_t index = 0U; index < count; index++) {
        length += strlen(items[index]) + (index + 1U < count ? 1U : 0U);
    }
    text = calloc(length + 1U, 1U);
    if (text == NULL) result = ENOMEM;
    else {
        for (size_t index = 0U; index < count; index++) {
            (void)strcat(text, items[index]);
            if (index + 1U < count) (void)strcat(text, " ");
        }
        free(*destination);
        *destination = text;
    }
    for (size_t index = 0U; index < count; index++) free(items[index]);
    free(items);
    return result;
}

static int append_item(char ***items, size_t *count, const char *value)
{
    char *copy = copy_text(value);
    char **next;
    if (copy == NULL) return ENOMEM;
    next = realloc(*items, (*count + 1U) * sizeof(*next));
    if (next == NULL) {
        free(copy);
        return ENOMEM;
    }
    next[*count] = copy;
    *items = next;
    (*count)++;
    return 0;
}

static int parse_values(char *value, char ***items, size_t *count)
{
    char token[PATH_MAX];
    size_t length = 0U;
    char quote = '\0';
    bool array = false;

    trim(value);
    if (*value == '(') {
        array = true;
        value++;
    }
    for (char *cursor = value;; cursor++) {
        char current = *cursor;
        if (quote != '\0') {
            if (current == quote) quote = '\0';
            else if (length + 1U < sizeof(token)) token[length++] = current;
        } else if (current == '\'' || current == '"') {
            quote = current;
        } else if (current == '\0' || current == ')' || current == ' ' || current == '\t') {
            if (length > 0U) {
                token[length] = '\0';
                if (append_item(items, count, token) != 0) return ENOMEM;
                length = 0U;
            }
            if (current == '\0' || (current == ')' && array)) break;
        } else if (current == '#') {
            break;
        } else if (length + 1U < sizeof(token)) {
            token[length++] = current;
        } else {
            return ENAMETOOLONG;
        }
    }
    return 0;
}

static int parse_pkgbuild(const char *path, struct build_metadata *metadata)
{
    FILE *file = fopen(path, "r");
    char *line = NULL;
    size_t capacity = 0U;
    ssize_t length;
    int result = 0;

    if (file == NULL) return errno;
    while (result == 0 && (length = getline(&line, &capacity, file)) >= 0) {
        char *equals;
        char *value;
        trim(line);
        if (*line == '\0' || *line == '#') continue;
        if (strstr(line, "prepare()") != NULL) metadata->prepare = true;
        if (strstr(line, "build()") != NULL) metadata->build = true;
        if (strstr(line, "check()") != NULL) metadata->check = true;
        if (strstr(line, "package()") != NULL) metadata->package = true;
        equals = strchr(line, '=');
        if (equals == NULL ||
            (strchr(line, '(') != NULL && equals > strchr(line, '('))) continue;
        *equals = '\0';
        value = equals + 1;
        trim(line);
        if (strcmp(line, "pkgname") == 0) {
            result = set_scalar(&metadata->pkgname, value);
        } else if (strcmp(line, "pkgver") == 0) result = set_scalar(&metadata->pkgver, value);
        else if (strcmp(line, "pkgrel") == 0) result = set_scalar(&metadata->pkgrel, value);
        else if (strcmp(line, "pkgdesc") == 0) result = set_scalar(&metadata->pkgdesc, value);
        else if (strcmp(line, "arch") == 0) result = set_scalar(&metadata->arch, value);
        else if (strcmp(line, "license") == 0) result = set_scalar(&metadata->license, value);
        else if (strcmp(line, "depends") == 0) result = set_list(&metadata->depends, value);
        else if (strcmp(line, "makedepends") == 0) result = set_list(&metadata->makedepends, value);
        else if (strcmp(line, "use_flags") == 0) result = set_list(&metadata->use_flags, value);
        else if (strcmp(line, "source") == 0) result = parse_values(value, &metadata->sources,
                                                                    &metadata->source_count);
        else if (strcmp(line, "sha256sums") == 0) result = parse_values(value, &metadata->checksums,
                                                                         &metadata->checksum_count);
    }
    free(line);
    if (ferror(file) != 0 && result == 0) result = EIO;
    if (fclose(file) != 0 && result == 0) result = errno;
    if (result == 0 && (metadata->pkgname == NULL || metadata->pkgver == NULL ||
                        metadata->pkgrel == NULL || metadata->package == false)) {
        result = EINVAL;
    }
    return result;
}

static int parse_config(const char *path, struct build_settings *settings)
{
    FILE *file;
    char *line = NULL;
    size_t capacity = 0U;
    int result = 0;

    file = fopen(path, "r");
    if (file == NULL) return errno == ENOENT ? 0 : errno;
    while (result == 0 && getline(&line, &capacity, file) >= 0) {
        char *equals;
        char *value;
        trim(line);
        if (*line == '\0' || *line == '#') continue;
        equals = strchr(line, '=');
        if (equals == NULL) continue;
        *equals = '\0';
        value = equals + 1;
        trim(line);
        trim(value);
        if (*value == '"' || *value == '\'') {
            size_t length = strlen(value);
            if (length > 1U && value[length - 1U] == value[0]) {
                value[length - 1U] = '\0';
                value++;
            }
        }
        if (strcmp(line, "PORTS_DIR") == 0) result = set_text(&settings->ports_dir, value);
        else if (strcmp(line, "USE") == 0) result = set_text(&settings->use, value);
        else if (strcmp(line, "CFLAGS") == 0) result = set_text(&settings->cflags, value);
        else if (strcmp(line, "CXXFLAGS") == 0) result = set_text(&settings->cxxflags, value);
        else if (strcmp(line, "LDFLAGS") == 0) result = set_text(&settings->ldflags, value);
        else if (strcmp(line, "MAKEFLAGS") == 0) result = set_text(&settings->makeflags, value);
        else if (strcmp(line, "MAX_JOBS") == 0) result = set_text(&settings->max_jobs, value);
    }
    free(line);
    if (ferror(file) != 0 && result == 0) result = EIO;
    if (fclose(file) != 0 && result == 0) result = errno;
    return result;
}

static int remove_tree(const char *path)
{
    struct stat status;
    DIR *dir;
    struct dirent *entry;
    char child[PATH_MAX];

    if (lstat(path, &status) != 0) {
        return errno == ENOENT ? 0 : errno;
    }
    if (!S_ISDIR(status.st_mode)) {
        return unlink(path) == 0 ? 0 : errno;
    }
    dir = opendir(path);
    if (dir == NULL) {
        return errno;
    }
    while ((entry = readdir(dir)) != NULL) {
        int result;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) < 0 ||
            strlen(child) >= sizeof(child)) {
            (void)closedir(dir);
            return ENAMETOOLONG;
        }
        result = remove_tree(child);
        if (result != 0) {
            (void)closedir(dir);
            return result;
        }
    }
    if (closedir(dir) != 0) {
        return errno;
    }
    return rmdir(path) == 0 ? 0 : errno;
}

static int mkdir_parents(const char *path)
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

static int copy_file(const char *source, const char *destination)
{
    FILE *input = fopen(source, "rb");
    FILE *output;
    unsigned char buffer[65536];
    size_t length;
    int result = 0;
    if (input == NULL) return errno;
    output = fopen(destination, "wb");
    if (output == NULL) {
        result = errno;
    } else {
        while ((length = fread(buffer, 1U, sizeof(buffer), input)) > 0U &&
               fwrite(buffer, 1U, length, output) == length) {}
        if (ferror(input) != 0 || ferror(output) != 0 || fclose(output) != 0) result = EIO;
    }
    (void)fclose(input);
    return result;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1U;
}

static bool is_url(const char *value)
{
    return strncmp(value, "http://", 7U) == 0 ||
           strncmp(value, "https://", 8U) == 0 ||
           strncmp(value, "git+http://", 12U) == 0 ||
           strncmp(value, "git+https://", 13U) == 0 ||
           strncmp(value, "git+file://", 12U) == 0;
}

static bool is_git_url(const char *value)
{
    return strncmp(value, "git+http://", 11U) == 0 ||
           strncmp(value, "git+https://", 12U) == 0 ||
           strncmp(value, "git+file://", 12U) == 0;
}

static int stage_git_source(const char *entry, const char *source_dir)
{
    char repo_name[256];
    char dest[PATH_MAX];
    char clean_url[PATH_MAX];
    const char *raw = entry + 4U;
    const char *frag = strchr(raw, '#');
    size_t raw_len = frag != NULL ? (size_t)(frag - raw) : strlen(raw);

    if (raw_len >= sizeof(clean_url)) {
        return ENAMETOOLONG;
    }
    memcpy(clean_url, raw, raw_len);
    clean_url[raw_len] = '\0';

    const char *base = base_name(clean_url);
    if (snprintf(repo_name, sizeof(repo_name), "%s", base) < 0 || strlen(base) >= sizeof(repo_name)) {
        return ENAMETOOLONG;
    }
    char *dot_git = strstr(repo_name, ".git");
    if (dot_git != NULL) {
        *dot_git = '\0';
    }
    if (snprintf(dest, sizeof(dest), "%s/%s", source_dir, repo_name) < 0 ||
        strlen(dest) >= sizeof(dest)) {
        return ENAMETOOLONG;
    }
    return pi_net_clone(entry, dest);
}

static bool is_archive(const char *path)
{
    return strstr(path, ".tar.gz") != NULL || strstr(path, ".tgz") != NULL ||
           strstr(path, ".tar.xz") != NULL || strstr(path, ".tar.zst") != NULL ||
           strstr(path, ".tar.bz2") != NULL || strstr(path, ".zip") != NULL;
}

static int resolve_pkgbuild(const char *argument, const char *ports_dir, char output[PATH_MAX])
{
    struct stat status;
    DIR *ports;
    struct dirent *category;
    char candidate[PATH_MAX];

    if (stat(argument, &status) == 0) {
        if (S_ISREG(status.st_mode) && strcmp(base_name(argument), "PKGBUILD") == 0) {
            return snprintf(output, PATH_MAX, "%s", argument) < 0 ||
                           strlen(argument) >= PATH_MAX ? ENAMETOOLONG : 0;
        }
        if (S_ISDIR(status.st_mode)) {
            if (snprintf(candidate, sizeof(candidate), "%s/PKGBUILD", argument) < 0 ||
                strlen(candidate) >= sizeof(candidate) || stat(candidate, &status) != 0) {
                return errno;
            }
            return snprintf(output, PATH_MAX, "%s", candidate) < 0 ||
                           strlen(candidate) >= PATH_MAX ? ENAMETOOLONG : 0;
        }
    }
    ports = opendir(ports_dir);
    if (ports == NULL) return errno;
    while ((category = readdir(ports)) != NULL) {
        if (category->d_name[0] == '.') continue;
        if (snprintf(candidate, sizeof(candidate), "%s/%s/%s/PKGBUILD", ports_dir,
                     category->d_name, argument) >= (int)sizeof(candidate)) continue;
        if (stat(candidate, &status) == 0 && S_ISREG(status.st_mode)) {
            (void)snprintf(output, PATH_MAX, "%s", candidate);
            (void)closedir(ports);
            return 0;
        }
    }
    (void)closedir(ports);
    return ENOENT;
}


static void expand_pkg_vars(const char *input, const struct build_metadata *metadata,
                            char *output, size_t size)
{
    size_t out = 0U;
    for (size_t i = 0U; input[i] != '\0' && out + 1U < size;) {
        const char *rep = NULL;
        size_t skip = 0U;
        if (strncmp(input + i, "${pkgver}", 9U) == 0) { rep = metadata->pkgver; skip = 9U; }
        else if (strncmp(input + i, "$pkgver", 7U) == 0) { rep = metadata->pkgver; skip = 7U; }
        else if (strncmp(input + i, "${pkgname}", 10U) == 0) { rep = metadata->pkgname; skip = 10U; }
        else if (strncmp(input + i, "$pkgname", 8U) == 0) { rep = metadata->pkgname; skip = 8U; }
        if (rep != NULL) {
            size_t rlen = strlen(rep);
            if (out + rlen >= size) break;
            memcpy(output + out, rep, rlen);
            out += rlen;
            i += skip;
        } else {
            output[out++] = input[i++];
        }
    }
    output[out] = '\0';
}

static int stage_sources(const char *pkgbuild, const char *source_dir,
                         const char *cache_dir, const struct build_metadata *metadata)
{
    char port_dir[PATH_MAX];
    char path[PATH_MAX];
    char cached[PATH_MAX];
    char digest[65];
    const char *slash;
    int result;

    slash = strrchr(pkgbuild, '/');
    if (slash == NULL) {
        (void)snprintf(port_dir, sizeof(port_dir), ".");
    } else if ((size_t)(slash - pkgbuild) >= sizeof(port_dir)) {
        return ENAMETOOLONG;
    } else {
        memcpy(port_dir, pkgbuild, (size_t)(slash - pkgbuild));
        port_dir[slash - pkgbuild] = '\0';
    }
    for (size_t index = 0U; index < metadata->source_count; index++) {
        char expanded_entry[PATH_MAX];
        expand_pkg_vars(metadata->sources[index], metadata, expanded_entry, sizeof(expanded_entry));
        const char *entry = expanded_entry;
        const char *name = base_name(entry);
        const char *checksum = index < metadata->checksum_count ? metadata->checksums[index] : NULL;
        if (snprintf(path, sizeof(path), "%s/%s", source_dir, name) < 0 ||
            strlen(path) >= sizeof(path) || snprintf(cached, sizeof(cached), "%s/%s", cache_dir,
                                                     name) < 0 || strlen(cached) >= sizeof(cached)) {
            return ENAMETOOLONG;
        }
        if (is_git_url(entry)) {
            result = stage_git_source(entry, source_dir);
        } else if (is_url(entry)) {
            bool valid = false;
            struct stat cached_status;
            if (checksum != NULL && strcmp(checksum, "SKIP") != 0 &&
                pi_net_sha256_file(cached, digest) == 0 && strcmp(digest, checksum) == 0) valid = true;
            if (checksum != NULL && strcmp(checksum, "SKIP") == 0 &&
                stat(cached, &cached_status) == 0 && S_ISREG(cached_status.st_mode)) valid = true;
            if (!valid) {
                char endpoints[4096];
                char *endpoint;
                char *separator;
                result = EIO;
                if (snprintf(endpoints, sizeof(endpoints), "%s", entry) < 0 ||
                    strlen(endpoints) >= sizeof(endpoints)) return ENAMETOOLONG;
                endpoint = endpoints;
                for (;;) {
                    separator = strstr(endpoint, "||");
                    if (separator != NULL) *separator = '\0';
                    result = pi_net_download(&(struct pi_net_options){
                        .url = endpoint, .destination = cached,
                        .sha256 = checksum != NULL && strcmp(checksum, "SKIP") != 0 ? checksum : NULL,
                        .timeout_seconds = 300L});
                    if (result == 0) break;
                    if (separator == NULL) break;
                    endpoint = separator + 2U;
                    while (*endpoint == ' ' || *endpoint == '\t') endpoint++;
                }
                if (result != 0) return result;
            }
            result = copy_file(cached, path);
        } else {
            char local[PATH_MAX];
            if (snprintf(local, sizeof(local), "%s/%s", port_dir, entry) < 0 ||
                strlen(local) >= sizeof(local)) return ENAMETOOLONG;
            result = copy_file(local, path);
            if (result == 0 && checksum != NULL && strcmp(checksum, "SKIP") != 0) {
                result = pi_net_sha256_file(path, digest);
                if (result == 0 && strcmp(digest, checksum) != 0) result = EBADMSG;
            }
        }
        if (result != 0) return result;
        if (is_archive(path)) {
            result = pi_archive_extract(path, source_dir);
            if (result != 0) return result;
        }
    }
    return 0;
}

static int write_build_script(const char *source_dir, const struct build_metadata *metadata,
                              char script_path[PATH_MAX])
{
    FILE *file;
    if (snprintf(script_path, PATH_MAX, "%s/.pi-build.sh", source_dir) < 0 ||
        strlen(script_path) >= PATH_MAX) return ENAMETOOLONG;
    file = fopen(script_path, "w");
    if (file == NULL) return errno;
    if (fputs("set -eu\n. /src/PKGBUILD\n", file) == EOF) {
        (void)fclose(file);
        return EIO;
    }
    if (metadata->prepare && fputs("(cd /src && prepare)\n", file) == EOF) return EIO;
    if (metadata->build && fputs("(cd /src && build)\n", file) == EOF) return EIO;
    if (metadata->check && fputs("(cd /src && check)\n", file) == EOF) return EIO;
    if (fputs("(cd /src && package)\n", file) == EOF || fclose(file) != 0) return EIO;
    return 0;
}

static int write_pkginfo(const char *package_dir, const struct build_metadata *metadata,
                         const char *use)
{
    char path[PATH_MAX];
    FILE *file;
    time_t now = time(NULL);
    if (snprintf(path, sizeof(path), "%s/.PKGINFO", package_dir) < 0 ||
        strlen(path) >= sizeof(path)) return ENAMETOOLONG;
    file = fopen(path, "w");
    if (file == NULL) return errno;
    if (fprintf(file, "pkgname = %s\npkgver = %s\npkgrel = %s\npkgdesc = %s\narch = %s\n"
                "license = %s\nuse = %s\nbuilddate = %ld\n",
                metadata->pkgname, metadata->pkgver, metadata->pkgrel,
                metadata->pkgdesc != NULL ? metadata->pkgdesc : "",
                metadata->arch != NULL ? metadata->arch : "x86_64",
                metadata->license != NULL ? metadata->license : "",
                use != NULL ? use : "", (long)now) < 0) {
        (void)fclose(file);
        return EIO;
    }
    if (metadata->depends != NULL && fprintf(file, "depend = %s\n", metadata->depends) < 0) {
        (void)fclose(file);
        return EIO;
    }
    return fclose(file) == 0 ? 0 : EIO;
}

static int run_full_build(const struct pi_build_spec *spec)
{
    struct build_metadata metadata = {0};
    struct build_settings settings = {.ports_dir = copy_text("/usr/ports")};
    char pkgbuild[PATH_MAX];
    char source_dir[PATH_MAX];
    char package_dir[PATH_MAX];
    char cache_dir[PATH_MAX];
    char script_path[PATH_MAX];
    char config_path[PATH_MAX];
    int result;

    if (spec->pkgbuild_path == NULL || spec->root == NULL) return EINVAL;
    if (snprintf(config_path, sizeof(config_path), "%s/etc/anrix/anrix.conf", spec->root) < 0 ||
        strlen(config_path) >= sizeof(config_path)) return ENAMETOOLONG;
    result = parse_config(config_path, &settings);
    if (result != 0) goto done;
    result = resolve_pkgbuild(spec->pkgbuild_path, settings.ports_dir, pkgbuild);
    if (result != 0) goto done;
    result = parse_pkgbuild(pkgbuild, &metadata);
    if (result != 0) goto done;
    if (snprintf(source_dir, sizeof(source_dir), "%s/var/cache/pi/src/%s", spec->root,
                 metadata.pkgname) < 0 || strlen(source_dir) >= sizeof(source_dir) ||
        snprintf(package_dir, sizeof(package_dir), "%s/var/cache/pi/pkg/%s", spec->root,
                 metadata.pkgname) < 0 || strlen(package_dir) >= sizeof(package_dir) ||
        snprintf(cache_dir, sizeof(cache_dir), "%s/var/cache/pi/sources", spec->root) < 0 ||
        strlen(cache_dir) >= sizeof(cache_dir)) {
        result = ENAMETOOLONG;
        goto done;
    }
    result = remove_tree(source_dir);
    if (result == 0) result = remove_tree(package_dir);
    if (result == 0) result = mkdir_parents(source_dir);
    if (result == 0) result = mkdir_parents(package_dir);
    if (result == 0) result = mkdir_parents(cache_dir);
    if (result != 0) goto done;
    if (result == 0) {
        char destination[PATH_MAX];
        if (snprintf(destination, sizeof(destination), "%s/PKGBUILD", source_dir) < 0 ||
            strlen(destination) >= sizeof(destination)) result = ENAMETOOLONG;
        else result = copy_file(pkgbuild, destination);
    }
    if (result == 0) result = stage_sources(pkgbuild, source_dir, cache_dir, &metadata);
    if (result == 0) result = write_build_script(source_dir, &metadata, script_path);
    if (result == 0) {
        char archive[PATH_MAX];
        char manifest[PATH_MAX];
        if (snprintf(archive, sizeof(archive), "%s/var/cache/pi/binpkgs/%s-%s-%s-x86_64.pkg.tar.zst",
                     spec->root, metadata.pkgname, metadata.pkgver, metadata.pkgrel) < 0 ||
            strlen(archive) >= sizeof(archive) ||
            snprintf(manifest, sizeof(manifest), "%s.manifest", archive) < 0 ||
            strlen(manifest) >= sizeof(manifest)) result = ENAMETOOLONG;
        else {
            char archive_dir[PATH_MAX];
            char *last_slash = strrchr(archive, '/');
            if (last_slash == NULL || (size_t)(last_slash - archive) >= sizeof(archive_dir)) {
                result = ENAMETOOLONG;
            } else {
                memcpy(archive_dir, archive, (size_t)(last_slash - archive));
                archive_dir[last_slash - archive] = '\0';
                result = mkdir_parents(archive_dir);
            }
            if (result == 0) {
                result = pi_sandbox_run(&(struct pi_sandbox_options){
                    .srcdir = source_dir, .pkgdir = package_dir, .script = ". /src/.pi-build.sh",
                    .network = spec->network, .use = settings.use != NULL ? settings.use : metadata.use_flags,
                    .cflags = settings.cflags, .cxxflags = settings.cxxflags,
                    .ldflags = settings.ldflags, .makeflags = settings.makeflags,
                    .max_jobs = settings.max_jobs});
            }
            if (result == 0) result = write_pkginfo(package_dir, &metadata, settings.use);
            if (result == 0) result = pi_archive_create(&(struct pi_archive_options){
                .source_dir = package_dir, .archive_path = archive, .manifest_path = manifest});
        }
    }
done:
    if (result != 0) (void)fprintf(stderr, "pi: build failed: %s\n", strerror(result));
    metadata_free(&metadata);
    settings_free(&settings);
    return result;
}

int pi_build_run(const struct pi_build_spec *spec)
{
    struct pi_sandbox_options sandbox;
    struct pi_archive_options archive;
    int result;

    if (spec == NULL || spec->name == NULL || spec->source_dir == NULL ||
        spec->package_dir == NULL || spec->archive_path == NULL ||
        spec->manifest_path == NULL || spec->script == NULL) {
        return spec != NULL && spec->pkgbuild_path != NULL ? run_full_build(spec) : EINVAL;
    }
    sandbox = (struct pi_sandbox_options){
        .srcdir = spec->source_dir, .pkgdir = spec->package_dir,
        .script = spec->script, .network = spec->network
    };
    result = pi_sandbox_run(&sandbox);
    if (result != 0) return result;
    archive = (struct pi_archive_options){
        .source_dir = spec->package_dir, .archive_path = spec->archive_path,
        .manifest_path = spec->manifest_path
    };
    return pi_archive_create(&archive);
}
