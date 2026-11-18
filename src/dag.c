// SPDX-License-Identifier: GPL-3.0-or-later
#include "dag.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static size_t node_index(const struct pi_dag_node *nodes, size_t count, const char *name)
{
    for (size_t index = 0U; index < count; index++) {
        if (strcmp(nodes[index].name, name) == 0) {
            return index;
        }
    }
    return count;
}

int pi_dag_parse_dependency(const char *text, struct pi_dependency *dependency,
                            char *name, size_t name_size, char *condition,
                            size_t condition_size)
{
    const char *separator;
    size_t length;
    bool negated = false;

    if (text == NULL || dependency == NULL || name == NULL || condition == NULL ||
        name_size == 0U || condition_size == 0U) {
        return EINVAL;
    }
    separator = strchr(text, '?');
    if (separator == NULL) {
        separator = text;
    } else {
        const char *flag = text;
        if (*flag == '!') {
            negated = true;
            flag++;
        }
        length = (size_t)(separator - flag);
        if (length == 0U || length + 1U > condition_size) {
            return ENAMETOOLONG;
        }
        memcpy(condition, flag, length);
        condition[length] = '\0';
        dependency->use_flag = condition;
        if (negated) {
            memmove(condition + 1U, condition, length + 1U);
            condition[0] = '!';
        }
    }
    const char *package = separator == text ? text : separator + 1U;
    const char *constraint = strpbrk(package, ">=<");
    length = constraint == NULL ? strlen(package) : (size_t)(constraint - package);
    if (length == 0U || length + 1U > name_size) {
        return ENAMETOOLONG;
    }
    memcpy(name, separator == text ? text : separator + 1U, length + 1U);
    dependency->name = name;
    if (separator == text) {
        dependency->use_flag = NULL;
    }
    return 0;
}

int pi_dag_sort(const struct pi_dag_node *nodes, size_t node_count,
                const char * const *requested, size_t requested_count,
                const char **order, size_t order_capacity, size_t *order_count,
                char *cycle, size_t cycle_size)
{
    size_t indegree[node_count == 0U ? 1U : node_count];
    bool selected[node_count == 0U ? 1U : node_count];
    size_t output = 0U;

    if (nodes == NULL || order == NULL || order_count == NULL ||
        (requested_count > 0U && requested == NULL) || cycle == NULL || cycle_size == 0U) {
        return EINVAL;
    }
    memset(indegree, 0, sizeof(indegree));
    memset(selected, 0, sizeof(selected));
    for (size_t index = 0U; index < requested_count; index++) {
        size_t current = node_index(nodes, node_count, requested[index]);
        if (current == node_count) {
            return ENOENT;
        }
        selected[current] = true;
    }
    for (size_t index = 0U; index < node_count; index++) {
        if (!selected[index]) {
            continue;
        }
        for (size_t dep = 0U; dep < nodes[index].dependency_count; dep++) {
            size_t dependency = node_index(nodes, node_count, nodes[index].dependencies[dep].name);
            if (dependency == node_count) {
                return ENOENT;
            }
            if (!selected[dependency]) {
                selected[dependency] = true;
            }
            indegree[index]++;
        }
    }
    for (size_t index = 0U; index < node_count; index++) {
        if (!selected[index]) {
            continue;
        }
        for (size_t dep = 0U; dep < nodes[index].dependency_count; dep++) {
            size_t dependency = node_index(nodes, node_count, nodes[index].dependencies[dep].name);
            if (!selected[dependency]) {
                selected[dependency] = true;
            }
        }
    }
    while (output < order_capacity) {
        size_t candidate = node_count;
        for (size_t index = 0U; index < node_count; index++) {
            if (selected[index] && indegree[index] == 0U &&
                (candidate == node_count || strcmp(nodes[index].name, nodes[candidate].name) < 0)) {
                candidate = index;
            }
        }
        if (candidate == node_count) {
            break;
        }
        selected[candidate] = false;
        order[output++] = nodes[candidate].name;
        for (size_t index = 0U; index < node_count; index++) {
            if (selected[index]) {
                for (size_t dep = 0U; dep < nodes[index].dependency_count; dep++) {
                    if (strcmp(nodes[index].dependencies[dep].name, nodes[candidate].name) == 0 &&
                        indegree[index] > 0U) {
                        indegree[index]--;
                    }
                }
            }
        }
    }
    *order_count = output;
    if (output == order_capacity) {
        for (size_t index = 0U; index < node_count; index++) {
            if (selected[index]) {
                return ENOSPC;
            }
        }
    }
    for (size_t index = 0U; index < node_count; index++) {
        if (selected[index]) {
            (void)snprintf(cycle, cycle_size, "dependency cycle involving %s", nodes[index].name);
            return ELOOP;
        }
    }
    return 0;
}

int pi_dag_resolve(const struct pi_dependency *dependencies, size_t count,
                   const char * const *enabled_flags, size_t enabled_count,
                   const char **resolved, size_t resolved_capacity,
                   size_t *resolved_count)
{
    size_t output = 0U;

    if (resolved_count == NULL || (count > 0U && dependencies == NULL) ||
        (resolved_capacity > 0U && resolved == NULL)) {
        return EINVAL;
    }
    for (size_t index = 0U; index < count; index++) {
        const struct pi_dependency *dependency = &dependencies[index];
        bool enabled = dependency->use_flag == NULL;
        for (size_t flag = 0U; !enabled && flag < enabled_count; flag++) {
            enabled = enabled_flags != NULL &&
                      strcmp(dependency->use_flag, enabled_flags[flag]) == 0;
        }
        if (enabled) {
            if (output == resolved_capacity) {
                return ENOSPC;
            }
            resolved[output++] = dependency->name;
        }
    }
    *resolved_count = output;
    return 0;
}
