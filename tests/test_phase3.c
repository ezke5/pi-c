// SPDX-License-Identifier: GPL-3.0-or-later
#include "dag.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

int main(void)
{
    char name[64];
    char condition[64];
    struct pi_dependency parsed = {0};
    struct pi_dependency a_deps[] = {{.name = "b"}};
    struct pi_dependency b_deps[] = {{.name = "c"}};
    struct pi_dag_node nodes[] = {
        {.name = "a", .dependencies = a_deps, .dependency_count = 1U},
        {.name = "b", .dependencies = b_deps, .dependency_count = 1U},
        {.name = "c", .dependencies = NULL, .dependency_count = 0U}
    };
    const char *roots[] = {"a"};
    const char *order[3];
    size_t count = 0U;
    char cycle[128];

    assert(pi_dag_parse_dependency("wayland?wayland-protocols", &parsed, name, sizeof(name),
                                   condition, sizeof(condition)) == 0);
    assert(strcmp(name, "wayland-protocols") == 0);
    assert(strcmp(condition, "wayland") == 0);
    assert(pi_dag_sort(nodes, 3U, roots, 1U, order, 3U, &count, cycle, sizeof(cycle)) == 0);
    assert(count == 3U && strcmp(order[0], "c") == 0 && strcmp(order[2], "a") == 0);
    a_deps[0].name = "a";
    assert(pi_dag_sort(nodes, 3U, roots, 1U, order, 3U, &count, cycle, sizeof(cycle)) == ELOOP);
    return 0;
}
