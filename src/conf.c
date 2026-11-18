// SPDX-License-Identifier: GPL-3.0-or-later
#include "conf.h"

#include <openssl/evp.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int list_add(struct pi_string_list *list, const char *value, bool enabled)
{
    size_t length;
    char *copy;
    char **items;

    if (value == NULL || *value == '\0') {
        return 0;
    }
    length = strlen(value);
    copy = malloc(length + 1U);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, value, length + 1U);
    items = realloc(list->items, (list->count + 1U) * sizeof(*items));
    if (items == NULL) {
        free(copy);
        return -1;
    }
    list->items = items;
    if (enabled) {
        list->items[list->count] = copy;
    } else {
        copy[0] = '-';
        memmove(copy + 1U, copy, length);
        list->items[list->count] = copy;
    }
    list->count++;
    return 0;
}

static void trim(char *text)
{
    char *start = text;
    size_t length;

    while (isspace((unsigned char)*start) != 0) {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1U);
    }
    length = strlen(text);
    while (length > 0U && isspace((unsigned char)text[length - 1U]) != 0) {
        text[--length] = '\0';
    }
}

static int parse_line(char *line, struct pi_string_list *flags)
{
    char *cursor = strchr(line, '=');

    if (cursor == NULL) {
        return 0;
    }
    cursor++;
    trim(cursor);
    if (*cursor == '"' || *cursor == '\'') {
        cursor++;
    }
    while (*cursor != '\0') {
        char *end;
        bool enabled = true;
        char saved;

        while (isspace((unsigned char)*cursor) != 0) {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        if (*cursor == '-') {
            enabled = false;
            cursor++;
        }
        end = cursor;
        while (*end != '\0' && isspace((unsigned char)*end) == 0 &&
               *end != '"' && *end != '\'') {
            end++;
        }
        saved = *end;
        *end = '\0';
        if (list_add(flags, cursor, enabled) != 0) {
            return -1;
        }
        *end = saved;
        cursor = end;
        if (*cursor == '"' || *cursor == '\'') {
            cursor++;
        }
    }
    return 0;
}

static int parse_file(const char *path, struct pi_string_list *flags)
{
    FILE *file;
    char *line = NULL;
    size_t capacity = 0U;
    ssize_t length;

    if (path == NULL) {
        return 0;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return errno == ENOENT ? 0 : -1;
    }
    while ((length = getline(&line, &capacity, file)) >= 0) {
        (void)length;
        trim(line);
        if (*line != '\0' && *line != '#') {
            if (parse_line(line, flags) != 0) {
                free(line);
                fclose(file);
                return -1;
            }
        }
    }
    free(line);
    if (ferror(file) != 0) {
        (void)fclose(file);
        return -1;
    }
    return fclose(file) == 0 ? 0 : -1;
}

static int compare_flags(const void *left, const void *right)
{
    const char * const *a = left;
    const char * const *b = right;
    return strcmp(*a, *b);
}

void pi_string_list_free(struct pi_string_list *list)
{
    if (list == NULL) {
        return;
    }
    for (size_t index = 0U; index < list->count; index++) {
        free(list->items[index]);
    }
    free(list->items);
    *list = (struct pi_string_list){0};
}

bool pi_string_list_contains(const struct pi_string_list *list, const char *value)
{
    if (list == NULL || value == NULL) {
        return false;
    }
    for (size_t index = 0U; index < list->count; index++) {
        if (strcmp(list->items[index], value) == 0) {
            return true;
        }
    }
    return false;
}

bool pi_conf_use_enabled(const struct pi_string_list *flags, const char *name)
{
    if (flags == NULL || name == NULL) {
        return false;
    }
    return pi_string_list_contains(flags, name);
}

int pi_conf_load_use_flags(const char *global_path, const char *package_path,
                           struct pi_string_list *flags)
{
    if (flags == NULL) {
        return EINVAL;
    }
    *flags = (struct pi_string_list){0};
    if (parse_file(global_path, flags) != 0 || parse_file(package_path, flags) != 0) {
        pi_string_list_free(flags);
        return -1;
    }
    return 0;
}

int pi_conf_use_hash(const struct pi_string_list *flags,
                     char output[PI_USE_HASH_LENGTH + 1U])
{
    EVP_MD_CTX *context;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0U;
    char **sorted;
    size_t text_length = 0U;
    char *text;
    int result = -1;

    if (flags == NULL || output == NULL) {
        return EINVAL;
    }
    sorted = malloc(flags->count * sizeof(*sorted));
    if (flags->count > 0U && sorted == NULL) {
        return ENOMEM;
    }
    for (size_t index = 0U; index < flags->count; index++) {
        sorted[index] = flags->items[index];
        text_length += strlen(flags->items[index]) + 1U;
    }
    qsort(sorted, flags->count, sizeof(*sorted), compare_flags);
    text = calloc(text_length + 1U, sizeof(*text));
    if (text == NULL) {
        free(sorted);
        return ENOMEM;
    }
    for (size_t index = 0U; index < flags->count; index++) {
        (void)strcat(text, sorted[index]);
        (void)strcat(text, "\n");
    }
    context = EVP_MD_CTX_new();
    if (context != NULL && EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1 &&
        EVP_DigestUpdate(context, text, strlen(text)) == 1 &&
        EVP_DigestFinal_ex(context, digest, &digest_length) == 1 &&
        digest_length >= 6U) {
        for (size_t index = 0U; index < 6U; index++) {
            (void)snprintf(output + index * 2U, 3U, "%02x", digest[index]);
        }
        output[PI_USE_HASH_LENGTH] = '\0';
        result = 0;
    }
    EVP_MD_CTX_free(context);
    free(text);
    free(sorted);
    return result;
}
