// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PI_DAG_H
#define PI_DAG_H

#include <stddef.h>

struct pi_dependency {
    const char *name;
    const char *use_flag;
};

struct pi_dag_node {
    const char *name;
    const struct pi_dependency *dependencies;
    size_t dependency_count;
};

int pi_dag_parse_dependency(const char *text, struct pi_dependency *dependency,
                            char *name, size_t name_size, char *condition,
                            size_t condition_size);
int pi_dag_sort(const struct pi_dag_node *nodes, size_t node_count,
                const char * const *requested, size_t requested_count,
                const char **order, size_t order_capacity, size_t *order_count,
                char *cycle, size_t cycle_size);

int pi_dag_resolve(const struct pi_dependency *dependencies, size_t count,
                   const char * const *enabled_flags, size_t enabled_count,
                   const char **resolved, size_t resolved_capacity,
                   size_t *resolved_count);

#endif
