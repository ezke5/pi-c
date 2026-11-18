// SPDX-License-Identifier: GPL-3.0-or-later
#include "net.h"

#include <dlfcn.h>
#include <openssl/evp.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void CURL;
typedef int CURLcode;
typedef void CURLSLIST;
typedef CURL *(*curl_init_fn)(void);
typedef CURLcode (*curl_perform_fn)(CURL *);
typedef void (*curl_cleanup_fn)(CURL *);
typedef CURLcode (*curl_setopt_fn)(CURL *, int, ...);
typedef CURLSLIST *(*curl_slist_append_fn)(CURLSLIST *, const char *);
typedef void (*curl_slist_free_all_fn)(CURLSLIST *);
typedef CURLcode (*curl_getinfo_fn)(CURL *, int, ...);

struct curl_api {
    void *handle;
    curl_init_fn init;
    curl_perform_fn perform;
    curl_cleanup_fn cleanup;
    curl_setopt_fn setopt;
    curl_slist_append_fn slist_append;
    curl_slist_free_all_fn slist_free_all;
    curl_getinfo_fn getinfo;
};

static int curl_load(struct curl_api *api)
{
    api->handle = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
    if (api->handle == NULL) {
        return ENOSYS;
    }
#define LOAD(name) *(void **)(&api->name) = dlsym(api->handle, "curl_" #name)
    LOAD(init);
    LOAD(perform);
    LOAD(cleanup);
    LOAD(setopt);
    LOAD(slist_append);
    LOAD(slist_free_all);
#undef LOAD
    *(void **)(&api->getinfo) = dlsym(api->handle, "curl_easy_getinfo");
    if (api->init == NULL || api->perform == NULL || api->cleanup == NULL ||
        api->setopt == NULL || api->slist_append == NULL ||
        api->slist_free_all == NULL || api->getinfo == NULL) {
        (void)dlclose(api->handle);
        return ENOSYS;
    }
    return 0;
}

int pi_net_sha256_file(const char *path, char output[65])
{
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    unsigned char input[8192];
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0U;
    FILE *file;
    size_t read_count;

    if (context == NULL) {
        return ENOMEM;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        EVP_MD_CTX_free(context);
        return errno;
    }
    if (EVP_DigestInit_ex(context, EVP_sha256(), NULL) != 1) {
        (void)fclose(file);
        EVP_MD_CTX_free(context);
        return EIO;
    }
    while ((read_count = fread(input, 1U, sizeof(input), file)) > 0U) {
        if (EVP_DigestUpdate(context, input, read_count) != 1) {
            (void)fclose(file);
            EVP_MD_CTX_free(context);
            return EIO;
        }
    }
    if (ferror(file) != 0 || fclose(file) != 0 ||
        EVP_DigestFinal_ex(context, digest, &length) != 1) {
        EVP_MD_CTX_free(context);
        return EIO;
    }
    EVP_MD_CTX_free(context);
    for (unsigned int index = 0U; index < length; index++) {
        (void)snprintf(output + index * 2U, 3U, "%02x", digest[index]);
    }
    output[64] = '\0';
    return 0;
}

static const char *url_host(const char *url)
{
    const char *host = strstr(url, "://");
    host = host == NULL ? url : host + 3U;
    if (*host == '[') {
        return host + 1U;
    }
    return host;
}

static int extract_url_host(const char *url, char host[256])
{
    const char *start = url_host(url);
    const char *end = start;
    size_t length;

    while (*end != '\0' && *end != '/' && *end != ':' && *end != '@') {
        end++;
    }
    if (*start == '@') {
        start = strchr(start, '@');
        if (start == NULL) return EINVAL;
        start++;
        end = start;
        while (*end != '\0' && *end != '/' && *end != ':' ) end++;
    }
    length = (size_t)(end - start);
    if (length == 0U || length >= 256U) return EINVAL;
    memcpy(host, start, length);
    host[length] = '\0';
    return 0;
}

static int parse_doh_answer(const char *json, char ip_out[64])
{
    const char *answer = json;
    while ((answer = strstr(answer, "\"type\"")) != NULL) {
        const char *colon = strchr(answer, ':');
        const char *data;
        char value[64];
        size_t length;

        if (colon == NULL) return EBADMSG;
        colon++;
        while (*colon == ' ' || *colon == '\t') colon++;
        if (*colon != '1') {
            answer++;
            continue;
        }
        data = strstr(answer, "\"data\"");
        if (data == NULL) return EBADMSG;
        data = strchr(data, ':');
        if (data == NULL) return EBADMSG;
        data++;
        while (*data == ' ' || *data == '\t') data++;
        if (*data != '"') return EBADMSG;
        data++;
        length = 0U;
        while (*data != '"' && *data != '\0' && length + 1U < sizeof(value)) {
            value[length++] = *data++;
        }
        if (*data != '"' || length == 0U) return EBADMSG;
        value[length] = '\0';
        if (snprintf(ip_out, 64U, "%s", value) < 0 || strlen(value) >= 64U) return ENAMETOOLONG;
        return 0;
    }
    return ENOENT;
}

static int doh_query(struct curl_api *api, const char *url,
                     char ip_out[64])
{
    char temporary[256];
    char line[65536];
    char *json;
    FILE *file;
    CURL *curl;
    CURLSLIST *headers = NULL;
    CURLcode code;
    int result;
    size_t length;

    if (snprintf(temporary, sizeof(temporary), "/tmp/pi-doh-%ld-%ld",
                 (long)getpid(), (long)time(NULL)) < 0 ||
        strlen(temporary) >= sizeof(temporary)) return ENAMETOOLONG;
    file = fopen(temporary, "wb+");
    if (file == NULL) return errno;
    curl = api->init();
    if (curl == NULL) {
        (void)fclose(file);
        (void)unlink(temporary);
        return EIO;
    }
    headers = api->slist_append(headers, "Accept: application/dns-json");
    if (headers == NULL) {
        api->cleanup(curl);
        (void)fclose(file);
        (void)unlink(temporary);
        return ENOMEM;
    }
    CURLcode setopt_result = 0;
    if ((setopt_result = api->setopt(curl, 10002, url)) == 0)
        setopt_result = api->setopt(curl, 10001, file);
    if (setopt_result == 0) setopt_result = api->setopt(curl, 10023, headers);
    if (setopt_result == 0) setopt_result = api->setopt(curl, 64L, 1L);
    if (setopt_result == 0) setopt_result = api->setopt(curl, 81L, 2L);
    if (setopt_result == 0) setopt_result = api->setopt(curl, 78L, 10L);
    code = setopt_result == 0 ? api->perform(curl) : setopt_result;
    api->slist_free_all(headers);
    api->cleanup(curl);
    if (code != 0 || fflush(file) != 0) {
        (void)fclose(file);
        (void)unlink(temporary);
        return EIO;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        (void)fclose(file);
        (void)unlink(temporary);
        return EIO;
    }
    length = (size_t)ftell(file);
    if (length >= sizeof(line)) length = sizeof(line) - 1U;
    (void)fseek(file, 0L, SEEK_SET);
    length = fread(line, 1U, length, file);
    line[length] = '\0';
    (void)fclose(file);
    json = line;
    result = parse_doh_answer(json, ip_out);
    (void)unlink(temporary);
    return result;
}

static int pi_net_doh_resolve(const char *host, char ip_out[64])
{
    struct curl_api api = {0};
    char url[512];
    int result;

    result = curl_load(&api);
    if (result != 0) return result;
    if (snprintf(url, sizeof(url),
                 "https://1.1.1.1/dns-query?name=%s&type=A", host) < 0 ||
        strlen(url) >= sizeof(url)) {
        (void)dlclose(api.handle);
        return ENAMETOOLONG;
    }
    result = doh_query(&api, url, ip_out);
    if (result != 0) {
        if (snprintf(url, sizeof(url),
                     "https://8.8.8.8/resolve?name=%s&type=A", host) < 0 ||
            strlen(url) >= sizeof(url)) {
            (void)dlclose(api.handle);
            return ENAMETOOLONG;
        }
        result = doh_query(&api, url, ip_out);
    }
    (void)dlclose(api.handle);
    return result;
}

int pi_net_fetch(const struct pi_net_options *options)
{
    struct curl_api api = {0};
    char temporary[4096];
    char digest[65];
    char host[256];
    char ip[64];
    CURL *curl;
    CURLSLIST *resolve = NULL;
    CURLSLIST *headers = NULL;
    FILE *file;
    int result;
    CURLcode code;

    if (options == NULL || options->url == NULL || options->destination == NULL) {
        return EINVAL;
    }
    if (snprintf(temporary, sizeof(temporary), "%s.part.%ld", options->destination,
                 (long)getpid()) < 0 ||
        strlen(temporary) >= sizeof(temporary)) {
        return ENAMETOOLONG;
    }
    file = fopen(temporary, "wb");
    if (file == NULL) {
        return errno;
    }
    result = curl_load(&api);
    if (result != 0) {
        (void)fclose(file);
        (void)unlink(temporary);
        return result;
    }
    curl = api.init();
    if (curl == NULL) {
        (void)dlclose(api.handle);
        (void)fclose(file);
        (void)unlink(temporary);
        return EIO;
    }
    headers = api.slist_append(headers, "Accept: */*");
    if (headers != NULL) {
        CURLSLIST *next = api.slist_append(headers,
            "User-Agent: git/2.47.0 (ANRIX-Linux pi)");
        if (next == NULL) {
            api.slist_free_all(headers);
            headers = NULL;
        } else {
            headers = next;
        }
    }
    if (headers == NULL) {
        api.cleanup(curl);
        (void)dlclose(api.handle);
        (void)fclose(file);
        (void)unlink(temporary);
        return ENOMEM;
    }
#define SETOPT(option, value) api_setopt_result = api.setopt(curl, option, value)
    CURLcode api_setopt_result = 0;
    SETOPT(10002, options->url);
    if (api_setopt_result == 0) SETOPT(10001, file);
    if (api_setopt_result == 0) SETOPT(10023, headers);
    if (api_setopt_result == 0) SETOPT(52L, 1L);
    if (api_setopt_result == 0) SETOPT(68L, 5L);
    if (api_setopt_result == 0) SETOPT(64L, 1L);
    if (api_setopt_result == 0) SETOPT(81L, 2L);
    if (api_setopt_result == 0) SETOPT(84L, 2L);
    if (api_setopt_result == 0) SETOPT(10102, "");
    if (api_setopt_result == 0) SETOPT(213L, 1L);
    if (api_setopt_result == 0) SETOPT(19L, 1024L);
    if (api_setopt_result == 0) SETOPT(20L, 15L);
    if (api_setopt_result == 0) SETOPT(78L, 20L);
    if (api_setopt_result == 0) SETOPT(13L, options->timeout_seconds > 0L ?
                                       options->timeout_seconds : 300L);
#undef SETOPT
    code = api_setopt_result == 0 ? api.perform(curl) : api_setopt_result;
    if (code == 6 && extract_url_host(options->url, host) == 0 &&
        pi_net_doh_resolve(host, ip) == 0) {
        char resolve443[512];
        char resolve80[512];
        if (snprintf(resolve443, sizeof(resolve443), "%s:443:%s", host, ip) >=
                (int)sizeof(resolve443) ||
            snprintf(resolve80, sizeof(resolve80), "%s:80:%s", host, ip) >=
                (int)sizeof(resolve80)) {
            code = 26;
        } else {
            resolve = api.slist_append(resolve, resolve443);
            if (resolve != NULL) {
                CURLSLIST *next = api.slist_append(resolve, resolve80);
                if (next == NULL) {
                    api.slist_free_all(resolve);
                    resolve = NULL;
                } else {
                    resolve = next;
                }
            }
            if (resolve != NULL) {
                (void)fflush(file);
                (void)ftruncate(fileno(file), 0L);
                rewind(file);
                if (api.setopt(curl, 10203, resolve) == 0) code = api.perform(curl);
            } else {
                code = 26;
            }
        }
    }
    if (code == 16 || code == 17) {
        (void)fflush(file);
        (void)ftruncate(fileno(file), 0L);
        rewind(file);
        if (api.setopt(curl, 84L, 1L) == 0) code = api.perform(curl);
    }
    char *content_type = NULL;
    CURLcode getinfo_code = api.getinfo(curl, 1048594, &content_type);
    api.slist_free_all(headers);
    if (resolve != NULL) api.slist_free_all(resolve);
    api.cleanup(curl);
    (void)dlclose(api.handle);
    if (fclose(file) != 0) {
        (void)unlink(temporary);
        return EIO;
    }
    if (code != 0) {
        (void)fprintf(stderr, "pi: network transfer failed for %s",
                      options->url);
        if (code == 28) (void)fprintf(stderr, ": connection stalled");
        (void)fputc('\n', stderr);
        (void)unlink(temporary);
        return code == 28 ? ETIMEDOUT : EIO;
    }
    if (getinfo_code == 0 && content_type != NULL &&
        strncmp(content_type, "text/html", 9U) == 0 &&
        (strstr(options->destination, ".tar.gz") != NULL ||
         strstr(options->destination, ".tar.xz") != NULL ||
         strstr(options->destination, ".tar.zst") != NULL ||
         strstr(options->destination, ".tgz") != NULL ||
         strstr(options->destination, ".zip") != NULL)) {
        (void)fprintf(stderr, "pi: server returned an HTML challenge instead of a binary archive: %s\n",
                      options->url);
        (void)unlink(temporary);
        return EIO;
    }
    if (options->sha256 != NULL) {
        result = pi_net_sha256_file(temporary, digest);
        if (result != 0 || strcmp(digest, options->sha256) != 0) {
            (void)unlink(temporary);
            return EBADMSG;
        }
    }
    if (rename(temporary, options->destination) != 0) {
        result = errno;
        (void)unlink(temporary);
        return result;
    }
    return 0;
}

int pi_net_sync_index(const char *url, const char *destination)
{
    return pi_net_fetch(&(struct pi_net_options){.url = url, .destination = destination,
                                                 .timeout_seconds = 30L});
}

int pi_net_download(const struct pi_net_options *options)
{
    return pi_net_fetch(options);
}

int pi_net_clone(const char *spec, const char *destination)
{
    char url[4096];
    char host[256];
    char ip[64];
    char *fragment;
    char *ref = NULL;
    char ref_storage[1024];
    const char *branch = NULL;
    bool tag = false;
    struct addrinfo hints = {0};
    struct addrinfo *resolved = NULL;
    pid_t child;
    int status;
    int gai_result;
    bool doh_ready = false;

    if (spec == NULL || destination == NULL) return EINVAL;
    if (strncmp(spec, "git+", 4U) == 0) spec += 4U;
    if (strncmp(spec, "http://", 7U) != 0 &&
        strncmp(spec, "https://", 8U) != 0 &&
        strncmp(spec, "file://", 7U) != 0) return EINVAL;
    if (snprintf(url, sizeof(url), "%s", spec) < 0 ||
        strlen(url) >= sizeof(url)) return ENAMETOOLONG;
    fragment = strpbrk(url, "#");
    if (fragment != NULL) {
        *fragment = '\0';
        fragment++;
        if (strncmp(fragment, "tag=", 4U) == 0) {
            tag = true;
            ref = fragment + 4U;
        } else if (strncmp(fragment, "branch=", 7U) == 0) {
            ref = fragment + 7U;
        }
        if (ref != NULL) {
            if (snprintf(ref_storage, sizeof(ref_storage), "%s", ref) < 0 ||
                strlen(ref_storage) >= sizeof(ref_storage)) return ENAMETOOLONG;
            branch = ref_storage;
        }
    }
    (void)tag;
    if (extract_url_host(url, host) == 0) {
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        gai_result = getaddrinfo(host, NULL, &hints, &resolved);
        if (gai_result == 0) {
            freeaddrinfo(resolved);
        } else if (gai_result == EAI_NONAME || gai_result == EAI_AGAIN) {
            if (pi_net_doh_resolve(host, ip) == 0) {
                doh_ready = true;
            }
        }
    }
    child = fork();
    if (child < 0) return errno;
    if (child == 0) {
        char *argv[10];
        size_t argc = 0U;
        (void)setenv("GIT_TERMINAL_PROMPT", "0", 1);
        if (doh_ready) {
            char resolve_value[512];
            (void)setenv("GIT_CONFIG_COUNT", "1", 1);
            (void)setenv("GIT_CONFIG_KEY_0", "http.curloptResolve", 1);
            if (snprintf(resolve_value, sizeof(resolve_value), "%s:443:%s", host, ip) >=
                    (int)sizeof(resolve_value)) _exit(127);
            (void)setenv("GIT_CONFIG_VALUE_0", resolve_value, 1);
        }
        argv[argc++] = "git";
        argv[argc++] = "clone";
        argv[argc++] = "--depth";
        argv[argc++] = "1";
        if (branch != NULL) {
            argv[argc++] = "--branch";
            argv[argc++] = (char *)branch;
        }
        argv[argc++] = "--";
        argv[argc++] = url;
        argv[argc++] = (char *)destination;
        argv[argc] = NULL;
        (void)execvp("git", argv);
        (void)fprintf(stderr, "pi: git clone failed: %s\n", strerror(errno));
        _exit(127);
    }
    if (waitpid(child, &status, 0) < 0) return errno;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    if (WIFEXITED(status)) {
        (void)fprintf(stderr, "pi: git clone exited with status %d\n", WEXITSTATUS(status));
    } else {
        (void)fprintf(stderr, "pi: git clone terminated abnormally\n");
    }
    return EIO;
}
