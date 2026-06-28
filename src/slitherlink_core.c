#include "slitherlink_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define UNKNOWN SL_UNKNOWN
#define LINE SL_LINE
#define BLANK SL_BLANK

typedef struct {
    int type; // 0: edge_state, 1: uf_parent, 2: uf_size, 4: loop_closed
    int id;
    int old_val;
} TrailEntry;

static int R, C;
static int E, V;
static int *grid;

static int *edge_u, *edge_v;
static int *edge_c1, *edge_c2;
static int *v_edges, *v_deg;
static int *c_edges;

static int *edge_state;
static int loop_closed;

static TrailEntry *trail;
static int trail_ptr;
static int trail_cap;

static int *prop_queue;
static int pq_head, pq_tail;

static int *uf_parent, *uf_size;
static SlStats *active_stats;
static int *active_clue_usage;
static int active_edge_one_shape_repairs;

static long debug_growth_attempts;
static long debug_growth_connect_failures;
static long debug_growth_checkerboard_failures;
static long debug_growth_loop_failures;
static long debug_growth_successes;
static double debug_growth_grow_ms;
static double debug_growth_connect_ms;
static double debug_growth_repair_ms;
static double debug_growth_validate_ms;
static int debug_dumped_checkerboard;
static int debug_dumped_loop;

static int debug_enabled(void) {
    return getenv("SL_GENERATE_DEBUG") != NULL;
}

static void debug_print_cells(const char *label, int rows, int cols, const int *cells) {
    fprintf(stderr, "%s\n", label);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            fputc(cells[r * cols + c] ? '#' : '.', stderr);
        }
        fputc('\n', stderr);
    }
}

static void reset_stats(SlStats *stats) {
    if (!stats) return;
    stats->solution_count = 0;
    stats->dfs_branches = 0;
    stats->probe_calls = 0;
    stats->max_depth = 0;
    stats->elapsed_ms = 0.0;
}

static void free_state(void) {
    free(grid); grid = NULL;
    free(edge_u); edge_u = NULL;
    free(edge_v); edge_v = NULL;
    free(edge_c1); edge_c1 = NULL;
    free(edge_c2); edge_c2 = NULL;
    free(v_edges); v_edges = NULL;
    free(v_deg); v_deg = NULL;
    free(c_edges); c_edges = NULL;
    free(edge_state); edge_state = NULL;
    free(trail); trail = NULL;
    free(prop_queue); prop_queue = NULL;
    free(uf_parent); uf_parent = NULL;
    free(uf_size); uf_size = NULL;
    R = C = E = V = 0;
    loop_closed = 0;
    trail_ptr = trail_cap = 0;
    pq_head = pq_tail = 0;
    active_clue_usage = NULL;
}

static int checked_alloc(void **ptr, size_t count, size_t size) {
    *ptr = calloc(count, size);
    return *ptr != NULL;
}

static int uf_find(int i) {
    while (i != uf_parent[i]) i = uf_parent[i];
    return i;
}

static int push_trail(int type, int id, int old_val) {
    if (trail_ptr >= trail_cap) return 0;
    trail[trail_ptr++] = (TrailEntry){type, id, old_val};
    return 1;
}

static void rollback(int target) {
    while (trail_ptr > target) {
        trail_ptr--;
        TrailEntry t = trail[trail_ptr];
        if (t.type == 0) edge_state[t.id] = t.old_val;
        else if (t.type == 1) uf_parent[t.id] = t.old_val;
        else if (t.type == 2) uf_size[t.id] = t.old_val;
        else if (t.type == 4) loop_closed = t.old_val;
    }
    pq_head = pq_tail = 0;
}

static int enqueue_edge(int e) {
    if (pq_tail >= E) return 0;
    prop_queue[pq_tail++] = e;
    return 1;
}

static int assign_edge(int e, int val) {
    if (edge_state[e] == val) return 1;
    if (edge_state[e] != UNKNOWN) return 0;

    if (!push_trail(0, e, UNKNOWN)) return 0;
    edge_state[e] = val;
    if (!enqueue_edge(e)) return 0;

    if (val == LINE) {
        int u = edge_u[e], v = edge_v[e];
        int root_u = uf_find(u), root_v = uf_find(v);
        if (root_u == root_v) {
            if (!push_trail(4, 0, loop_closed)) return 0;
            loop_closed = 1;
        } else {
            if (uf_size[root_u] < uf_size[root_v]) {
                int tmp = root_u; root_u = root_v; root_v = tmp;
            }
            if (!push_trail(1, root_v, uf_parent[root_v])) return 0;
            uf_parent[root_v] = root_u;
            if (!push_trail(2, root_u, uf_size[root_u])) return 0;
            uf_size[root_u] += uf_size[root_v];
        }
    }
    return 1;
}

static int propagate(void) {
    while (pq_head < pq_tail) {
        int e = prop_queue[pq_head++];

        for (int i = 0; i < 2; i++) {
            int c = (i == 0) ? edge_c1[e] : edge_c2[e];
            if (c == -1 || grid[c] == -1) continue;

            int req = grid[c];
            int lines = 0, blanks = 0, unk = 0;
            for (int j = 0; j < 4; j++) {
                int ce = c_edges[c * 4 + j];
                if (edge_state[ce] == LINE) lines++;
                else if (edge_state[ce] == BLANK) blanks++;
                else unk++;
            }

            if (lines > req || blanks > 4 - req) {
                if (active_clue_usage) active_clue_usage[c] = 1;
                return 0;
            }
            if (lines == req && unk > 0) {
                if (active_clue_usage) active_clue_usage[c] = 1;
                for (int j = 0; j < 4; j++)
                    if (edge_state[c_edges[c * 4 + j]] == UNKNOWN)
                        if (!assign_edge(c_edges[c * 4 + j], BLANK)) return 0;
            }
            if (blanks == 4 - req && unk > 0) {
                if (active_clue_usage) active_clue_usage[c] = 1;
                for (int j = 0; j < 4; j++)
                    if (edge_state[c_edges[c * 4 + j]] == UNKNOWN)
                        if (!assign_edge(c_edges[c * 4 + j], LINE)) return 0;
            }
        }

        for (int i = 0; i < 2; i++) {
            int v = (i == 0) ? edge_u[e] : edge_v[e];
            int deg = v_deg[v];
            int lines = 0, unk = 0;
            for (int j = 0; j < deg; j++) {
                int ve = v_edges[v * 4 + j];
                if (edge_state[ve] == LINE) lines++;
                else if (edge_state[ve] == UNKNOWN) unk++;
            }

            if (lines > 2) return 0;
            if (lines == 1 && unk == 0) return 0;
            if (lines == 2 && unk > 0) {
                for (int j = 0; j < deg; j++)
                    if (edge_state[v_edges[v * 4 + j]] == UNKNOWN)
                        if (!assign_edge(v_edges[v * 4 + j], BLANK)) return 0;
            }
            if (lines == 1 && unk == 1) {
                for (int j = 0; j < deg; j++)
                    if (edge_state[v_edges[v * 4 + j]] == UNKNOWN)
                        if (!assign_edge(v_edges[v * 4 + j], LINE)) return 0;
            }
            if (lines == 0 && unk == 1) {
                for (int j = 0; j < deg; j++)
                    if (edge_state[v_edges[v * 4 + j]] == UNKNOWN)
                        if (!assign_edge(v_edges[v * 4 + j], BLANK)) return 0;
            }
        }

        if (loop_closed) {
            for (int i = 0; i < E; i++) {
                if (edge_state[i] == UNKNOWN) {
                    if (!assign_edge(i, BLANK)) return 0;
                }
            }
        }
    }
    return 1;
}

static int probe_and_deduce(void) {
    if (active_stats) active_stats->probe_calls++;

    int changed = 0;
    int current_trail = trail_ptr;
    int *line_state = (int*)malloc(E * sizeof(int));
    int *common_vals = (int*)malloc(E * sizeof(int));
    if (!line_state || !common_vals) {
        free(line_state);
        free(common_vals);
        return -1;
    }

    for (int i = 0; i < E; i++) common_vals[i] = UNKNOWN;

    for (int e = 0; e < E; e++) {
        if (edge_state[e] != UNKNOWN) continue;

        int ok_line = assign_edge(e, LINE) && propagate();
        if (ok_line) memcpy(line_state, edge_state, E * sizeof(int));
        rollback(current_trail);

        int ok_blank = assign_edge(e, BLANK) && propagate();
        if (ok_line && ok_blank) {
            for (int i = 0; i < E; i++) {
                if (line_state[i] != UNKNOWN && line_state[i] == edge_state[i]) {
                    if (common_vals[i] == UNKNOWN) common_vals[i] = line_state[i];
                }
            }
        }
        rollback(current_trail);

        if (!ok_line && !ok_blank) {
            free(line_state); free(common_vals); return -1;
        } else if (ok_line && !ok_blank) {
            if (!assign_edge(e, LINE) || !propagate()) {
                free(line_state); free(common_vals); return -1;
            }
            changed++;
            current_trail = trail_ptr;
        } else if (!ok_line && ok_blank) {
            if (!assign_edge(e, BLANK) || !propagate()) {
                free(line_state); free(common_vals); return -1;
            }
            changed++;
            current_trail = trail_ptr;
        }
    }

    for (int i = 0; i < E; i++) {
        if (common_vals[i] != UNKNOWN && edge_state[i] == UNKNOWN) {
            if (!assign_edge(i, common_vals[i]) || !propagate()) {
                free(line_state); free(common_vals); return -1;
            }
            changed++;
        }
    }

    free(line_state);
    free(common_vals);
    return changed;
}

static int deduce_until_stable(void) {
    if (!propagate()) return 0;

    int changed;
    do {
        changed = probe_and_deduce();
        if (changed == -1) return 0;
    } while (changed > 0);

    return 1;
}

static int first_unknown_edge(void) {
    for (int i = 0; i < E; i++) {
        if (edge_state[i] == UNKNOWN) return i;
    }
    return -1;
}

static int solve_one(int depth) {
    if (active_stats && depth > active_stats->max_depth) active_stats->max_depth = depth;
    if (!deduce_until_stable()) return 0;

    int target = first_unknown_edge();
    if (target == -1) return 1;

    int saved_trail = trail_ptr;
    if (active_stats) active_stats->dfs_branches++;
    if (assign_edge(target, LINE) && solve_one(depth + 1)) return 1;
    rollback(saved_trail);

    if (active_stats) active_stats->dfs_branches++;
    if (assign_edge(target, BLANK) && solve_one(depth + 1)) return 1;
    rollback(saved_trail);

    return 0;
}

static int solve_count(int limit, int depth, int *first_solution) {
    if (active_stats && depth > active_stats->max_depth) active_stats->max_depth = depth;
    if (!deduce_until_stable()) return 0;

    int target = first_unknown_edge();
    if (target == -1) {
        if (first_solution && active_stats && active_stats->solution_count == 0) {
            memcpy(first_solution, edge_state, E * sizeof(int));
        }
        if (active_stats) active_stats->solution_count++;
        return 1;
    }

    int found = 0;
    int saved_trail = trail_ptr;

    if (active_stats) active_stats->dfs_branches++;
    if (assign_edge(target, LINE)) {
        found += solve_count(limit, depth + 1, first_solution);
        if (active_stats && active_stats->solution_count >= limit) {
            rollback(saved_trail);
            return found;
        }
    }
    rollback(saved_trail);

    if (active_stats) active_stats->dfs_branches++;
    if (assign_edge(target, BLANK)) {
        found += solve_count(limit, depth + 1, first_solution);
    }
    rollback(saved_trail);

    return found;
}

static int init_from_clues(int rows, int cols, const int *clues) {
    if (rows <= 0 || cols <= 0 || !clues) return 0;
    free_state();

    R = rows;
    C = cols;
    V = (R + 1) * (C + 1);
    E = (R + 1) * C + R * (C + 1);
    trail_cap = E * 64 + V * 8 + R * C * 8;

    if (!checked_alloc((void**)&grid, R * C, sizeof(int))) return 0;
    memcpy(grid, clues, R * C * sizeof(int));

    if (!checked_alloc((void**)&edge_u, E, sizeof(int))) return 0;
    if (!checked_alloc((void**)&edge_v, E, sizeof(int))) return 0;
    if (!checked_alloc((void**)&edge_c1, E, sizeof(int))) return 0;
    if (!checked_alloc((void**)&edge_c2, E, sizeof(int))) return 0;
    if (!checked_alloc((void**)&v_edges, V * 4, sizeof(int))) return 0;
    if (!checked_alloc((void**)&v_deg, V, sizeof(int))) return 0;
    if (!checked_alloc((void**)&c_edges, R * C * 4, sizeof(int))) return 0;
    if (!checked_alloc((void**)&edge_state, E, sizeof(int))) return 0;
    if (!checked_alloc((void**)&trail, trail_cap, sizeof(TrailEntry))) return 0;
    if (!checked_alloc((void**)&prop_queue, E, sizeof(int))) return 0;
    if (!checked_alloc((void**)&uf_parent, V, sizeof(int))) return 0;
    if (!checked_alloc((void**)&uf_size, V, sizeof(int))) return 0;

    for (int i = 0; i < V; i++) {
        uf_parent[i] = i;
        uf_size[i] = 1;
    }

    for (int r = 0; r <= R; r++) {
        for (int c = 0; c < C; c++) {
            int e = r * C + c;
            edge_u[e] = r * (C + 1) + c;
            edge_v[e] = r * (C + 1) + c + 1;
            edge_c1[e] = (r > 0) ? (r - 1) * C + c : -1;
            edge_c2[e] = (r < R) ? r * C + c : -1;
            v_edges[edge_u[e] * 4 + v_deg[edge_u[e]]++] = e;
            v_edges[edge_v[e] * 4 + v_deg[edge_v[e]]++] = e;
            if (r > 0) c_edges[((r - 1) * C + c) * 4 + 2] = e;
            if (r < R) c_edges[(r * C + c) * 4 + 0] = e;
        }
    }

    int offset = (R + 1) * C;
    for (int r = 0; r < R; r++) {
        for (int c = 0; c <= C; c++) {
            int e = offset + r * (C + 1) + c;
            edge_u[e] = r * (C + 1) + c;
            edge_v[e] = (r + 1) * (C + 1) + c;
            edge_c1[e] = (c > 0) ? r * C + (c - 1) : -1;
            edge_c2[e] = (c < C) ? r * C + c : -1;
            v_edges[edge_u[e] * 4 + v_deg[edge_u[e]]++] = e;
            v_edges[edge_v[e] * 4 + v_deg[edge_v[e]]++] = e;
            if (c > 0) c_edges[(r * C + (c - 1)) * 4 + 1] = e;
            if (c < C) c_edges[(r * C + c) * 4 + 3] = e;
        }
    }

    return 1;
}

static unsigned int rng_next(unsigned int *state) {
    *state = (*state * 1664525u) + 1013904223u;
    return *state;
}

static int rng_range(unsigned int *state, int limit) {
    if (limit <= 0) return 0;
    return (int)(rng_next(state) % (unsigned int)limit);
}

static void shuffle_ints(int *values, int count, unsigned int *rng) {
    for (int i = count - 1; i > 0; i--) {
        int j = rng_range(rng, i + 1);
        int tmp = values[i];
        values[i] = values[j];
        values[j] = tmp;
    }
}

static void shuffle_removal_indices(int rows, int cols, int *values, int count,
                                    const int *clues, int edge_one_priority, unsigned int *rng) {
    (void)rows;
    (void)cols;
    (void)edge_one_priority;
    shuffle_ints(values, count, rng);
    int write = 0;
    int clue_order[4] = {0, 3, 1, 2};
    for (int pass = 0; pass < 4; pass++) {
        for (int i = write; i < count; i++) {
            if (clues[values[i]] == clue_order[pass]) {
                int tmp = values[write];
                values[write] = values[i];
                values[i] = tmp;
                write++;
            }
        }
    }
}

static void prioritize_removal_indices(int rows, int cols, int *values, int count, const int *clues,
                                       const int *usage, int edge_one_priority, unsigned int *rng) {
    (void)rows;
    (void)cols;
    (void)edge_one_priority;
    shuffle_ints(values, count, rng);
    int write = 0;
    int clue_order[4] = {0, 3, 1, 2};
    for (int used_pass = 0; used_pass < 2; used_pass++) {
        for (int clue_pass = 0; clue_pass < 4; clue_pass++) {
            for (int i = write; i < count; i++) {
                int idx = values[i];
                int unused = usage && clues[idx] >= 0 && usage[idx] == 0;
                int used_bucket = unused ? 0 : 1;
                if (used_bucket == used_pass && clues[idx] == clue_order[clue_pass]) {
                    int tmp = values[write];
                    values[write] = values[i];
                    values[i] = tmp;
                    write++;
                }
            }
        }
    }
}

static int h_edge_index_for(int cols, int r, int c) {
    return r * cols + c;
}

static int v_edge_index_for(int rows, int cols, int r, int c) {
    return (rows + 1) * cols + r * (cols + 1) + c;
}

static int edge_endpoint_u(int rows, int cols, int e) {
    int horizontal_count = (rows + 1) * cols;
    if (e < horizontal_count) {
        int r = e / cols;
        int c = e % cols;
        return r * (cols + 1) + c;
    }
    int ve = e - horizontal_count;
    int r = ve / (cols + 1);
    int c = ve % (cols + 1);
    return r * (cols + 1) + c;
}

static int edge_endpoint_v(int rows, int cols, int e) {
    int horizontal_count = (rows + 1) * cols;
    if (e < horizontal_count) {
        int r = e / cols;
        int c = e % cols;
        return r * (cols + 1) + c + 1;
    }
    int ve = e - horizontal_count;
    int r = ve / (cols + 1);
    int c = ve % (cols + 1);
    return (r + 1) * (cols + 1) + c;
}

static int validate_single_loop(int rows, int cols, const int *edges) {
    int edge_count = (rows + 1) * cols + rows * (cols + 1);
    int vertex_count = (rows + 1) * (cols + 1);
    int *deg = (int*)calloc(vertex_count, sizeof(int));
    int *neighbors = (int*)malloc(vertex_count * 2 * sizeof(int));
    int *visited = (int*)calloc(vertex_count, sizeof(int));
    int *stack = (int*)malloc(vertex_count * sizeof(int));
    if (!deg || !neighbors || !visited || !stack) {
        free(deg); free(neighbors); free(visited); free(stack);
        return 0;
    }

    int start = -1;
    int line_vertices = 0;
    for (int e = 0; e < edge_count; e++) {
        if (edges[e] != LINE) continue;
        int u = edge_endpoint_u(rows, cols, e);
        int v = edge_endpoint_v(rows, cols, e);
        if (deg[u] >= 2 || deg[v] >= 2) {
            free(deg); free(neighbors); free(visited); free(stack);
            return 0;
        }
        neighbors[u * 2 + deg[u]] = v;
        neighbors[v * 2 + deg[v]] = u;
        deg[u]++;
        deg[v]++;
    }

    for (int v = 0; v < vertex_count; v++) {
        if (deg[v] == 0) continue;
        if (deg[v] != 2) {
            free(deg); free(neighbors); free(visited); free(stack);
            return 0;
        }
        if (start == -1) start = v;
        line_vertices++;
    }
    if (start == -1) {
        free(deg); free(neighbors); free(visited); free(stack);
        return 0;
    }

    int top = 0, seen = 0;
    stack[top++] = start;
    visited[start] = 1;
    while (top > 0) {
        int v = stack[--top];
        seen++;
        for (int i = 0; i < deg[v]; i++) {
            int next = neighbors[v * 2 + i];
            if (!visited[next]) {
                visited[next] = 1;
                stack[top++] = next;
            }
        }
    }

    free(deg);
    free(neighbors);
    free(visited);
    free(stack);
    return seen == line_vertices;
}

static int validate_loop_coverage(int rows, int cols, const int *edges) {
    int edge_count = (rows + 1) * cols + rows * (cols + 1);
    int min_r = rows + 1, max_r = -1;
    int min_c = cols + 1, max_c = -1;
    int *row_lines = (int*)calloc(rows, sizeof(int));
    int *col_lines = (int*)calloc(cols, sizeof(int));
    if (!row_lines || !col_lines) {
        free(row_lines);
        free(col_lines);
        return 0;
    }

    for (int e = 0; e < edge_count; e++) {
        if (edges[e] != LINE) continue;
        int vertices[2] = {
            edge_endpoint_u(rows, cols, e),
            edge_endpoint_v(rows, cols, e)
        };
        for (int i = 0; i < 2; i++) {
            int r = vertices[i] / (cols + 1);
            int c = vertices[i] % (cols + 1);
            if (r < min_r) min_r = r;
            if (r > max_r) max_r = r;
            if (c < min_c) min_c = c;
            if (c > max_c) max_c = c;
        }

        int horizontal_count = (rows + 1) * cols;
        if (e < horizontal_count) {
            int r = e / cols;
            int c = e % cols;
            if (r > 0) {
                row_lines[r - 1]++;
                col_lines[c]++;
            }
            if (r < rows) {
                row_lines[r]++;
                col_lines[c]++;
            }
        } else {
            int ve = e - horizontal_count;
            int r = ve / (cols + 1);
            int c = ve % (cols + 1);
            row_lines[r]++;
            if (c > 0) col_lines[c - 1]++;
            if (c < cols) col_lines[c]++;
        }
    }

    if (max_r < 0 || max_c < 0) {
        free(row_lines);
        free(col_lines);
        return 0;
    }

    int height = max_r - min_r + 1;
    int width = max_c - min_c + 1;
    int min_height = ((rows + 1) * 7 + 9) / 10;
    int min_width = ((cols + 1) * 7 + 9) / 10;
    if (height < min_height || width < min_width) {
        free(row_lines);
        free(col_lines);
        return 0;
    }

    for (int r = 0; r < rows; r++) {
        if (row_lines[r] == 0) {
            free(row_lines);
            free(col_lines);
            return 0;
        }
    }
    for (int c = 0; c < cols; c++) {
        if (col_lines[c] == 0) {
            free(row_lines);
            free(col_lines);
            return 0;
        }
    }

    if (rows >= 4 && cols >= 4) {
        for (int r0 = 0; r0 + 3 < rows; r0++) {
            for (int c0 = 0; c0 + 3 < cols; c0++) {
                int near_line = 0;
                for (int r = r0; r < r0 + 4; r++) {
                    for (int c = c0; c < c0 + 4; c++) {
                        if (edges[h_edge_index_for(cols, r, c)] == LINE ||
                            edges[h_edge_index_for(cols, r + 1, c)] == LINE ||
                            edges[v_edge_index_for(rows, cols, r, c)] == LINE ||
                            edges[v_edge_index_for(rows, cols, r, c + 1)] == LINE) {
                            near_line++;
                        }
                    }
                }
                if (near_line < 3) {
                    free(row_lines);
                    free(col_lines);
                    return 0;
                }
            }
        }
    }

    free(row_lines);
    free(col_lines);
    return 1;
}

static void build_edges_from_cells(int rows, int cols, const int *cells, int *edges) {
    int edge_count = (rows + 1) * cols + rows * (cols + 1);
    for (int i = 0; i < edge_count; i++) edges[i] = BLANK;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (!cells[r * cols + c]) continue;
            if (r == 0 || !cells[(r - 1) * cols + c])
                edges[h_edge_index_for(cols, r, c)] = LINE;
            if (r == rows - 1 || !cells[(r + 1) * cols + c])
                edges[h_edge_index_for(cols, r + 1, c)] = LINE;
            if (c == 0 || !cells[r * cols + c - 1])
                edges[v_edge_index_for(rows, cols, r, c)] = LINE;
            if (c == cols - 1 || !cells[r * cols + c + 1])
                edges[v_edge_index_for(rows, cols, r, c + 1)] = LINE;
        }
    }
}

static int cell_neighbor_count(int rows, int cols, const int *cells, int r, int c, int want_filled) {
    int count = 0;
    if (r > 0 && cells[(r - 1) * cols + c] == want_filled) count++;
    if (r + 1 < rows && cells[(r + 1) * cols + c] == want_filled) count++;
    if (c > 0 && cells[r * cols + c - 1] == want_filled) count++;
    if (c + 1 < cols && cells[r * cols + c + 1] == want_filled) count++;
    return count;
}

static int count_filled_cells(int rows, int cols, const int *cells) {
    int count = 0;
    for (int i = 0; i < rows * cols; i++) {
        if (cells[i]) count++;
    }
    return count;
}

static int cells_connected(int rows, int cols, const int *cells) {
    int cell_count = rows * cols;
    int start = -1;
    int filled = 0;
    for (int i = 0; i < cell_count; i++) {
        if (cells[i]) {
            if (start == -1) start = i;
            filled++;
        }
    }
    if (filled == 0) return 0;

    int *visited = (int*)calloc(cell_count, sizeof(int));
    int *stack = (int*)malloc(cell_count * sizeof(int));
    if (!visited || !stack) {
        free(visited);
        free(stack);
        return 0;
    }

    int top = 0;
    int seen = 0;
    stack[top++] = start;
    visited[start] = 1;
    while (top > 0) {
        int idx = stack[--top];
        int r = idx / cols;
        int c = idx % cols;
        seen++;
        int nbs[4][2] = {
            {r - 1, c}, {r + 1, c}, {r, c - 1}, {r, c + 1}
        };
        for (int i = 0; i < 4; i++) {
            int nr = nbs[i][0];
            int nc = nbs[i][1];
            if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
            int nidx = nr * cols + nc;
            if (cells[nidx] && !visited[nidx]) {
                visited[nidx] = 1;
                stack[top++] = nidx;
            }
        }
    }

    free(visited);
    free(stack);
    return seen == filled;
}

static int try_remove_boundary_cell(int rows, int cols, int *cells, int min_filled,
                                    int *candidates, unsigned int *rng) {
    int filled = count_filled_cells(rows, cols, cells);
    if (filled <= min_filled) return 0;

    int candidate_count = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            if (!cells[idx]) continue;
            int outside = 4 - cell_neighbor_count(rows, cols, cells, r, c, 1);
            if (r == 0) outside++;
            if (r + 1 == rows) outside++;
            if (c == 0) outside++;
            if (c + 1 == cols) outside++;
            if (outside >= 2) candidates[candidate_count++] = idx;
        }
    }
    if (candidate_count == 0) return 0;
    shuffle_ints(candidates, candidate_count, rng);

    for (int i = 0; i < candidate_count; i++) {
        int idx = candidates[i];
        cells[idx] = 0;
        if (cells_connected(rows, cols, cells)) return 1;
        cells[idx] = 1;
    }
    return 0;
}

static int try_add_protrusion_cell(int rows, int cols, int *cells, int max_filled,
                                   int *candidates, unsigned int *rng) {
    int filled = count_filled_cells(rows, cols, cells);
    if (filled >= max_filled) return 0;

    int candidate_count = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            if (cells[idx]) continue;
            int touching = cell_neighbor_count(rows, cols, cells, r, c, 1);
            if (touching == 1 || touching == 2) candidates[candidate_count++] = idx;
        }
    }
    if (candidate_count == 0) return 0;
    shuffle_ints(candidates, candidate_count, rng);

    cells[candidates[0]] = 1;
    return 1;
}

static void perturb_cells(int rows, int cols, int *cells, unsigned int *rng, int *candidates) {
    int cell_count = rows * cols;
    int min_filled = cell_count / 5;
    int max_filled = (cell_count * 4) / 5;
    if (min_filled < 2) min_filled = 2;
    if (max_filled < min_filled + 1) max_filled = min_filled + 1;

    int iterations = cell_count / 2;
    if (rows * cols >= 100) iterations = cell_count;
    if (rows * cols >= 225) iterations = cell_count * 3 / 2;

    for (int i = 0; i < iterations; i++) {
        if (rng_range(rng, 100) < 55) {
            if (!try_remove_boundary_cell(rows, cols, cells, min_filled, candidates, rng)) {
                try_add_protrusion_cell(rows, cols, cells, max_filled, candidates, rng);
            }
        } else {
            if (!try_add_protrusion_cell(rows, cols, cells, max_filled, candidates, rng)) {
                try_remove_boundary_cell(rows, cols, cells, min_filled, candidates, rng);
            }
        }
    }
}

static void repair_sparse_zero_patterns(int rows, int cols, int *cells, int *edges,
                                        int *clues, unsigned int *rng);

static int generate_random_loop(int rows, int cols, unsigned int *rng, int *out_solution) {
    int cell_count = rows * cols;
    int *cells = (int*)calloc(cell_count, sizeof(int));
    int *frontier = (int*)malloc(cell_count * sizeof(int));
    int *repair_clues = (int*)malloc(cell_count * sizeof(int));
    if (!cells || !frontier || !repair_clues) {
        free(cells); free(frontier); free(repair_clues);
        return 0;
    }

    for (int attempt = 0; attempt < 2000; attempt++) {
        memset(cells, 0, cell_count * sizeof(int));
        int min_cells = cell_count / 4;
        if (min_cells < 2) min_cells = 2;
        int span = cell_count / 2;
        if (span < 1) span = 1;
        int target = min_cells + rng_range(rng, span + 1);
        if (target > cell_count) target = cell_count;

        int start = rng_range(rng, cell_count);
        cells[start] = 1;
        int filled = 1;

        while (filled < target) {
            int frontier_count = 0;
            int slender_count = 0;
            for (int r = 0; r < rows; r++) {
                for (int c = 0; c < cols; c++) {
                    int idx = r * cols + c;
                    if (cells[idx]) continue;
                    int touches = cell_neighbor_count(rows, cols, cells, r, c, 1);
                    if (touches > 0) {
                        frontier[frontier_count++] = idx;
                        if (touches == 1) slender_count++;
                    }
                }
            }
            if (frontier_count == 0) break;
            int next;
            if (slender_count > 0 && rng_range(rng, 100) < 75) {
                int wanted = rng_range(rng, slender_count);
                next = frontier[0];
                for (int i = 0; i < frontier_count; i++) {
                    int idx = frontier[i];
                    int r = idx / cols;
                    int c = idx % cols;
                    if (cell_neighbor_count(rows, cols, cells, r, c, 1) == 1) {
                        if (wanted == 0) {
                            next = idx;
                            break;
                        }
                        wanted--;
                    }
                }
            } else {
                next = frontier[rng_range(rng, frontier_count)];
            }
            cells[next] = 1;
            filled++;
        }

        perturb_cells(rows, cols, cells, rng, frontier);
        repair_sparse_zero_patterns(rows, cols, cells, out_solution, repair_clues, rng);
        build_edges_from_cells(rows, cols, cells, out_solution);
        if (validate_single_loop(rows, cols, out_solution) &&
            validate_loop_coverage(rows, cols, out_solution)) {
            free(cells);
            free(frontier);
            free(repair_clues);
            return 1;
        }
    }

    free(cells);
    free(frontier);
    free(repair_clues);
    return 0;
}

static int add_random_walk_wall(int rows, int cols, int *cells, int *blocked,
                                int r, int c, int pr, int pc, unsigned int *rng) {
    int dirs[2][2];
    if (r != pr || c == pc) {
        dirs[0][0] = 0; dirs[0][1] = -1;
        dirs[1][0] = 0; dirs[1][1] = 1;
    } else {
        dirs[0][0] = -1; dirs[0][1] = 0;
        dirs[1][0] = 1; dirs[1][1] = 0;
    }

    int changed = 0;
    for (int i = 0; i < 2; i++) {
        if (rng_range(rng, 100) >= 35) continue;
        int wr = r + dirs[i][0];
        int wc = c + dirs[i][1];
        if (wr < 0 || wr >= rows || wc < 0 || wc >= cols) continue;
        int idx = wr * cols + wc;
        if (!cells[idx]) {
            blocked[idx] = 1;
            changed = 1;
        }
    }
    return changed;
}

static int choose_frontier_jump(int rows, int cols, const int *cells, const int *blocked,
                                int *out_r, int *out_c, int *candidates, unsigned int *rng) {
    int count = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            if (cells[idx] || blocked[idx]) continue;
            int touches = cell_neighbor_count(rows, cols, cells, r, c, 1);
            if (touches == 1 || touches == 2) candidates[count++] = idx;
        }
    }
    if (count == 0) return 0;
    int chosen = candidates[rng_range(rng, count)];
    *out_r = chosen / cols;
    *out_c = chosen % cols;
    return 1;
}

static int generate_random_loop_walk(int rows, int cols, unsigned int *rng, int *out_solution) {
    int cell_count = rows * cols;
    int *cells = (int*)calloc(cell_count, sizeof(int));
    int *blocked = (int*)calloc(cell_count, sizeof(int));
    int *candidates = (int*)malloc(cell_count * 8 * sizeof(int));
    int *repair_clues = (int*)malloc(cell_count * sizeof(int));
    if (!cells || !blocked || !candidates || !repair_clues) {
        free(cells);
        free(blocked);
        free(candidates);
        free(repair_clues);
        return 0;
    }

    for (int attempt = 0; attempt < 3000; attempt++) {
        memset(cells, 0, cell_count * sizeof(int));
        memset(blocked, 0, cell_count * sizeof(int));

        int corner = rng_range(rng, 4);
        int r = (corner >= 2) ? rows - 1 : 0;
        int c = (corner % 2) ? cols - 1 : 0;
        cells[r * cols + c] = 1;
        int filled = 1;

        int min_target = (cell_count * 35) / 100;
        int span = (cell_count * 35) / 100;
        if (min_target < 2) min_target = 2;
        if (span < 1) span = 1;
        int target = min_target + rng_range(rng, span + 1);
        if (target > (cell_count * 4) / 5) target = (cell_count * 4) / 5;
        if (target < min_target) target = min_target;

        int max_steps = cell_count * 12;
        for (int step = 0; step < max_steps && filled < target; step++) {
            int count = 0;
            int dirs[4][2] = {
                {-1, 0}, {1, 0}, {0, -1}, {0, 1}
            };
            for (int d = 0; d < 4; d++) {
                int nr = r + dirs[d][0];
                int nc = c + dirs[d][1];
                if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
                int idx = nr * cols + nc;
                if (blocked[idx]) continue;

                if (!cells[idx]) {
                    int touches = cell_neighbor_count(rows, cols, cells, nr, nc, 1);
                    if (touches > 2) continue;
                    candidates[count++] = idx;
                    if (touches == 1) {
                        candidates[count++] = idx;
                        candidates[count++] = idx;
                    }
                } else if (rng_range(rng, 100) < 18) {
                    candidates[count++] = idx;
                }
            }

            if (count == 0) {
                if (!choose_frontier_jump(rows, cols, cells, blocked, &r, &c, candidates, rng)) break;
            } else {
                int chosen = candidates[rng_range(rng, count)];
                int nr = chosen / cols;
                int nc = chosen % cols;
                int was_new = !cells[chosen];
                int pr = r, pc = c;
                r = nr;
                c = nc;
                if (was_new) {
                    cells[chosen] = 1;
                    filled++;
                    add_random_walk_wall(rows, cols, cells, blocked, r, c, pr, pc, rng);
                }
            }

            if (rng_range(rng, 100) < 8) {
                int wr = rng_range(rng, rows);
                int wc = rng_range(rng, cols);
                int idx = wr * cols + wc;
                if (!cells[idx]) blocked[idx] = 1;
            }
        }

        if (filled < min_target) continue;

        repair_sparse_zero_patterns(rows, cols, cells, out_solution, repair_clues, rng);
        build_edges_from_cells(rows, cols, cells, out_solution);
        if (validate_single_loop(rows, cols, out_solution) &&
            validate_loop_coverage(rows, cols, out_solution)) {
            free(cells);
            free(blocked);
            free(candidates);
            free(repair_clues);
            return 1;
        }
    }

    free(cells);
    free(blocked);
    free(candidates);
    free(repair_clues);
    return 0;
}

static int count_distinct_neighbor_islands(int rows, int cols, const int *island_id,
                                           int r, int c, int *ids, int max_ids) {
    int count = 0;
    int dirs[4][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1}
    };
    for (int d = 0; d < 4; d++) {
        int nr = r + dirs[d][0];
        int nc = c + dirs[d][1];
        if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
        int id = island_id[nr * cols + nc];
        if (id < 0) continue;
        int seen = 0;
        for (int i = 0; i < count; i++) {
            if (ids[i] == id) {
                seen = 1;
                break;
            }
        }
        if (!seen && count < max_ids) ids[count++] = id;
    }
    return count;
}

static int binary_cell_value_after_add(int rows, int cols, const int *island_id,
                                       int add_r, int add_c, int r, int c) {
    if (r < 0 || r >= rows || c < 0 || c >= cols) return 0;
    if (r == add_r && c == add_c) return 1;
    return island_id[r * cols + c] >= 0;
}

static int has_checkerboard_2x2_values(int a, int b, int c, int d) {
    return a == d && b == c && a != b;
}

static int would_create_checkerboard_growth(int rows, int cols, const int *island_id, int r, int c) {
    for (int dr = -1; dr <= 0; dr++) {
        for (int dc = -1; dc <= 0; dc++) {
            int r0 = r + dr;
            int c0 = c + dc;
            if (r0 < 0 || c0 < 0 || r0 + 1 >= rows || c0 + 1 >= cols) continue;
            int a = binary_cell_value_after_add(rows, cols, island_id, r, c, r0, c0);
            int b = binary_cell_value_after_add(rows, cols, island_id, r, c, r0, c0 + 1);
            int cc = binary_cell_value_after_add(rows, cols, island_id, r, c, r0 + 1, c0);
            int d = binary_cell_value_after_add(rows, cols, island_id, r, c, r0 + 1, c0 + 1);
            if (has_checkerboard_2x2_values(a, b, cc, d)) return 1;
        }
    }
    return 0;
}

static void break_checkerboard_around_fill(int rows, int cols, int *cells, int r, int c) {
    for (int dr = -1; dr <= 0; dr++) {
        for (int dc = -1; dc <= 0; dc++) {
            int r0 = r + dr;
            int c0 = c + dc;
            if (r0 < 0 || c0 < 0 || r0 + 1 >= rows || c0 + 1 >= cols) continue;
            int pos[4][2] = {
                {r0, c0}, {r0, c0 + 1}, {r0 + 1, c0}, {r0 + 1, c0 + 1}
            };
            int v[4];
            for (int i = 0; i < 4; i++) v[i] = cells[pos[i][0] * cols + pos[i][1]] != 0;
            if (!has_checkerboard_2x2_values(v[0], v[1], v[2], v[3])) continue;
            for (int i = 0; i < 4; i++) {
                int rr = pos[i][0];
                int cc = pos[i][1];
                if (cells[rr * cols + cc] == 0) {
                    cells[rr * cols + cc] = 1;
                    return;
                }
            }
        }
    }
}

static int has_checkerboard_cells(int rows, int cols, const int *cells) {
    for (int r = 0; r + 1 < rows; r++) {
        for (int c = 0; c + 1 < cols; c++) {
            int a = cells[r * cols + c] != 0;
            int b = cells[r * cols + c + 1] != 0;
            int cc = cells[(r + 1) * cols + c] != 0;
            int d = cells[(r + 1) * cols + c + 1] != 0;
            if (has_checkerboard_2x2_values(a, b, cc, d)) return 1;
        }
    }
    return 0;
}

static int growth_probability(int rows, int cols, int r, int c, int source_id,
                              const int *state, const int *island_id, int growth_bias) {
    int ids[4];
    int touching = count_distinct_neighbor_islands(rows, cols, island_id, r, c, ids, 4);
    if (touching == 0) return 0;
    if (touching > 1) return 0;
    if (would_create_checkerboard_growth(rows, cols, island_id, r, c)) return 0;

    int same_neighbors = 0;
    int dirs[4][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1}
    };
    for (int d = 0; d < 4; d++) {
        int nr = r + dirs[d][0];
        int nc = c + dirs[d][1];
        if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
        if (island_id[nr * cols + nc] == source_id) same_neighbors++;
    }
    if (same_neighbors > 2) return 0;

    int blocky_risk = 0;
    for (int dr = -1; dr <= 0; dr++) {
        for (int dc = -1; dc <= 0; dc++) {
            int r0 = r + dr;
            int c0 = c + dc;
            if (r0 < 0 || c0 < 0 || r0 + 1 >= rows || c0 + 1 >= cols) continue;
            int same = 0;
            for (int rr = r0; rr <= r0 + 1; rr++) {
                for (int cc = c0; cc <= c0 + 1; cc++) {
                    if (rr == r && cc == c) continue;
                    if (island_id[rr * cols + cc] == source_id) same++;
                }
            }
            if (same >= 2) blocky_risk++;
        }
    }

    int dist_top = r;
    int dist_bottom = rows - 1 - r;
    int dist_left = c;
    int dist_right = cols - 1 - c;
    int edge_dist = dist_top;
    if (dist_bottom < edge_dist) edge_dist = dist_bottom;
    if (dist_left < edge_dist) edge_dist = dist_left;
    if (dist_right < edge_dist) edge_dist = dist_right;
    int center_r2 = rows - 1;
    int center_c2 = cols - 1;
    int from_center = abs(2 * r - center_r2) + abs(2 * c - center_c2);

    int p = 30;
    if (same_neighbors == 1) p += 30;
    if (same_neighbors == 2) p -= 32;
    p -= blocky_risk * 18;
    if (edge_dist == 0) p -= 35;
    else if (edge_dist == 1) p -= 18;
    else if (edge_dist >= 3) p += 10;
    p -= from_center / 6;

    if (edge_dist == 0) {
        int edge_neighbors = 0;
        if (r == 0 || r + 1 == rows) {
            if (c > 0 && island_id[r * cols + c - 1] == source_id) edge_neighbors++;
            if (c + 1 < cols && island_id[r * cols + c + 1] == source_id) edge_neighbors++;
        }
        if (c == 0 || c + 1 == cols) {
            if (r > 0 && island_id[(r - 1) * cols + c] == source_id) edge_neighbors++;
            if (r + 1 < rows && island_id[(r + 1) * cols + c] == source_id) edge_neighbors++;
        }
        p -= edge_neighbors * 28;
    }

    if (state[r * cols + c] == 3) p = 0;
    if (growth_bias < 0) growth_bias = 0;
    if (growth_bias > 100) growth_bias = 100;
    p = (p * (50 + growth_bias)) / 100;
    if (p < 0) p = 0;
    if (p > 90) p = 90;
    return p;
}

static int place_growth_seeds(int rows, int cols, int *state, int *island_id,
                              int seed_count, unsigned int *rng) {
    int placed = 0;
    int attempts = rows * cols * 4;
    while (placed < seed_count && attempts-- > 0) {
        int margin_r = rows > 6 ? 1 : 0;
        int margin_c = cols > 6 ? 1 : 0;
        int r = margin_r + rng_range(rng, rows - margin_r * 2);
        int c = margin_c + rng_range(rng, cols - margin_c * 2);
        int idx = r * cols + c;
        if (state[idx] != 0) continue;

        int too_close = 0;
        for (int rr = r - 2; rr <= r + 2 && !too_close; rr++) {
            for (int cc = c - 2; cc <= c + 2; cc++) {
                if (rr < 0 || rr >= rows || cc < 0 || cc >= cols) continue;
                if (island_id[rr * cols + cc] >= 0) {
                    too_close = 1;
                    break;
                }
            }
        }
        if (too_close && placed + 1 < seed_count) continue;

        state[idx] = 1;
        island_id[idx] = placed;
        placed++;
    }
    return placed;
}

static void grow_islands(int rows, int cols, int *state, int *island_id,
                         int seed_count, int *candidates, unsigned int *rng, int growth_bias) {
    int cell_count = rows * cols;
    int target = (cell_count * (45 + rng_range(rng, 16))) / 100;
    int filled = 0;
    for (int i = 0; i < cell_count; i++) {
        if (island_id[i] >= 0) filled++;
    }

    int stable_steps = 0;
    int max_steps = cell_count * 20;
    for (int step = 0; step < max_steps && filled < target && stable_steps < cell_count; step++) {
        int candidate_count = 0;
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int idx = r * cols + c;
                if (state[idx] != 0) continue;
                int ids[4];
                int touching = count_distinct_neighbor_islands(rows, cols, island_id, r, c, ids, 4);
                if (touching == 0) continue;
                if (touching > 1) {
                    state[idx] = 3;
                    continue;
                }
                if (would_create_checkerboard_growth(rows, cols, island_id, r, c)) {
                    state[idx] = 3;
                    continue;
                }
                candidates[candidate_count++] = idx;
                int same_neighbors = 0;
                int dirs[4][2] = {
                    {-1, 0}, {1, 0}, {0, -1}, {0, 1}
                };
                for (int d = 0; d < 4; d++) {
                    int nr = r + dirs[d][0];
                    int nc = c + dirs[d][1];
                    if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
                    if (island_id[nr * cols + nc] == ids[0]) same_neighbors++;
                }
                int edge_dist = r;
                if (rows - 1 - r < edge_dist) edge_dist = rows - 1 - r;
                if (c < edge_dist) edge_dist = c;
                if (cols - 1 - c < edge_dist) edge_dist = cols - 1 - c;
                if (same_neighbors == 1 && edge_dist > 0) {
                    candidates[candidate_count++] = idx;
                    if (edge_dist >= 3) candidates[candidate_count++] = idx;
                }
            }
        }
        if (candidate_count == 0) break;

        shuffle_ints(candidates, candidate_count, rng);
        int changed = 0;
        int batch = candidate_count / 3 + 1;
        for (int i = 0; i < candidate_count && i < batch && filled < target; i++) {
            int idx = candidates[i];
            if (state[idx] != 0) continue;
            int r = idx / cols;
            int c = idx % cols;
            int ids[4];
            int touching = count_distinct_neighbor_islands(rows, cols, island_id, r, c, ids, 4);
            if (touching != 1) {
                state[idx] = 3;
                continue;
            }
            if (would_create_checkerboard_growth(rows, cols, island_id, r, c)) {
                state[idx] = 3;
                continue;
            }
            int p = growth_probability(rows, cols, r, c, ids[0], state, island_id, growth_bias);
            if (rng_range(rng, 100) < p) {
                state[idx] = rng_range(rng, 100) < 70 ? 1 : 2;
                island_id[idx] = ids[0];
                filled++;
                changed = 1;
            }
        }

        for (int i = 0; i < cell_count; i++) {
            if (state[i] == 1 && rng_range(rng, 100) < 18) state[i] = 2;
        }
        stable_steps = changed ? 0 : stable_steps + 1;
    }

    (void)seed_count;
}

static void merge_island_ids(int rows, int cols, int *island_id, int from_id, int to_id) {
    for (int i = 0; i < rows * cols; i++) {
        if (island_id[i] == from_id) island_id[i] = to_id;
    }
}

static int connect_growth_islands(int rows, int cols, int *state, int *island_id,
                                  int island_count, unsigned int *rng) {
    int cell_count = rows * cols;
    int *candidates = (int*)malloc(cell_count * sizeof(int));
    if (!candidates) return 0;

    int changed = 1;
    while (changed) {
        changed = 0;
        int candidate_count = 0;
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int idx = r * cols + c;
                if (island_id[idx] >= 0) continue;
                int ids[4];
                int touching = count_distinct_neighbor_islands(rows, cols, island_id, r, c, ids, 4);
                if (touching >= 2) candidates[candidate_count++] = idx;
            }
        }
        if (candidate_count == 0) break;

        shuffle_ints(candidates, candidate_count, rng);
        for (int i = 0; i < candidate_count; i++) {
            int idx = candidates[i];
            if (island_id[idx] >= 0) continue;
            int r = idx / cols;
            int c = idx % cols;
            int ids[4];
            int touching = count_distinct_neighbor_islands(rows, cols, island_id, r, c, ids, 4);
            if (touching < 2) continue;

            int target = ids[0];
            state[idx] = 2;
            island_id[idx] = target;
            if (would_create_checkerboard_growth(rows, cols, island_id, r, c)) {
                int dirs[4][2] = {
                    {-1, 0}, {1, 0}, {0, -1}, {0, 1}
                };
                for (int d = 0; d < 4; d++) {
                    int nr = r + dirs[d][0];
                    int nc = c + dirs[d][1];
                    if (nr < 0 || nr >= rows || nc < 0 || nc >= cols) continue;
                    int nidx = nr * cols + nc;
                    if (island_id[nidx] < 0) {
                        state[nidx] = 2;
                        island_id[nidx] = target;
                        break;
                    }
                }
            }

            for (int j = 1; j < touching; j++) {
                merge_island_ids(rows, cols, island_id, ids[j], target);
            }
            changed = 1;
        }
    }
    free(candidates);

    (void)island_count;
    int *cells = (int*)malloc(cell_count * sizeof(int));
    if (!cells) return 0;
    for (int i = 0; i < cell_count; i++) cells[i] = island_id[i] >= 0 ? 1 : 0;
    int ok = cells_connected(rows, cols, cells);
    free(cells);
    return ok;
}

static int generate_random_loop_growth(int rows, int cols, unsigned int *rng, int *out_solution, int strict, int growth_bias) {
    int cell_count = rows * cols;
    int *state = (int*)calloc(cell_count, sizeof(int));
    int *island_id = (int*)malloc(cell_count * sizeof(int));
    int *cells = (int*)calloc(cell_count, sizeof(int));
    int *candidates = (int*)malloc(cell_count * 4 * sizeof(int));
    int *repair_clues = (int*)malloc(cell_count * sizeof(int));
    if (!state || !island_id || !cells || !candidates || !repair_clues) {
        free(state); free(island_id); free(cells); free(candidates); free(repair_clues);
        return 0;
    }

    for (int attempt = 0; attempt < 1000; attempt++) {
        debug_growth_attempts++;
        memset(state, 0, cell_count * sizeof(int));
        for (int i = 0; i < cell_count; i++) island_id[i] = -1;

        int seed_count = cell_count / 28;
        if (seed_count < 3) seed_count = 3;
        if (seed_count > 18) seed_count = 18;
        int placed = place_growth_seeds(rows, cols, state, island_id, seed_count, rng);
        if (placed < 2) continue;

        clock_t grow_start = clock();
        grow_islands(rows, cols, state, island_id, placed, candidates, rng, growth_bias);
        debug_growth_grow_ms += (double)(clock() - grow_start) * 1000.0 / CLOCKS_PER_SEC;
        clock_t connect_start = clock();
        if (!connect_growth_islands(rows, cols, state, island_id, placed, rng)) {
            debug_growth_connect_ms += (double)(clock() - connect_start) * 1000.0 / CLOCKS_PER_SEC;
            debug_growth_connect_failures++;
            continue;
        }
        debug_growth_connect_ms += (double)(clock() - connect_start) * 1000.0 / CLOCKS_PER_SEC;

        for (int i = 0; i < cell_count; i++) cells[i] = island_id[i] >= 0 ? 1 : 0;
        clock_t repair_start = clock();
        repair_sparse_zero_patterns(rows, cols, cells, out_solution, repair_clues, rng);
        debug_growth_repair_ms += (double)(clock() - repair_start) * 1000.0 / CLOCKS_PER_SEC;
        if (has_checkerboard_cells(rows, cols, cells)) {
            if (debug_enabled() && !debug_dumped_checkerboard) {
                debug_print_cells("growth_failure_sample reason=checkerboard", rows, cols, cells);
                debug_dumped_checkerboard = 1;
            }
            debug_growth_checkerboard_failures++;
            continue;
        }
        build_edges_from_cells(rows, cols, cells, out_solution);
        if (!strict) {
            debug_growth_successes++;
            free(state); free(island_id); free(cells); free(candidates); free(repair_clues);
            return 1;
        }
        clock_t validate_start = clock();
        if (validate_single_loop(rows, cols, out_solution)) {
            debug_growth_validate_ms += (double)(clock() - validate_start) * 1000.0 / CLOCKS_PER_SEC;
            debug_growth_successes++;
            free(state); free(island_id); free(cells); free(candidates); free(repair_clues);
            return 1;
        }
        debug_growth_validate_ms += (double)(clock() - validate_start) * 1000.0 / CLOCKS_PER_SEC;
        if (debug_enabled() && !debug_dumped_loop) {
            debug_print_cells("growth_failure_sample reason=not_single_loop", rows, cols, cells);
            debug_dumped_loop = 1;
        }
        debug_growth_loop_failures++;
    }

    free(state);
    free(island_id);
    free(cells);
    free(candidates);
    free(repair_clues);
    return 0;
}

int sl_generate_growth_islands_preview(int rows, int cols, unsigned int seed, int growth_bias,
                                       int *out_island_ids, int *out_states, int *out_island_count) {
    if (rows <= 1 || cols <= 1 || !out_island_ids || !out_states) return 0;

    int cell_count = rows * cols;
    unsigned int rng = seed ? seed : 1u;
    int *state = (int*)calloc(cell_count, sizeof(int));
    int *island_id = (int*)malloc(cell_count * sizeof(int));
    int *candidates = (int*)malloc(cell_count * 4 * sizeof(int));
    if (!state || !island_id || !candidates) {
        free(state);
        free(island_id);
        free(candidates);
        return 0;
    }

    for (int i = 0; i < cell_count; i++) island_id[i] = -1;

    int seed_count = cell_count / 28;
    if (seed_count < 3) seed_count = 3;
    if (seed_count > 18) seed_count = 18;
    int placed = place_growth_seeds(rows, cols, state, island_id, seed_count, &rng);
    if (placed < 1) {
        free(state);
        free(island_id);
        free(candidates);
        return 0;
    }

    grow_islands(rows, cols, state, island_id, placed, candidates, &rng, growth_bias);
    memcpy(out_island_ids, island_id, cell_count * sizeof(int));
    memcpy(out_states, state, cell_count * sizeof(int));
    if (out_island_count) *out_island_count = placed;

    free(state);
    free(island_id);
    free(candidates);
    return 1;
}

static void clues_from_solution(int rows, int cols, const int *solution, int *clues) {
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int n = 0;
            if (solution[h_edge_index_for(cols, r, c)] == LINE) n++;
            if (solution[h_edge_index_for(cols, r + 1, c)] == LINE) n++;
            if (solution[v_edge_index_for(rows, cols, r, c)] == LINE) n++;
            if (solution[v_edge_index_for(rows, cols, r, c + 1)] == LINE) n++;
            clues[r * cols + c] = n;
        }
    }
}

static int is_edge_band_cell(int rows, int cols, int r, int c) {
    return r == 0 || c == 0 || r + 1 == rows || c + 1 == cols;
}

static int count_edge_band_cells(int rows, int cols) {
    int count = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (is_edge_band_cell(rows, cols, r, c)) count++;
        }
    }
    return count;
}

static int shape_limit_zero_cross(int rows, int cols, int strict) {
    int cell_count = rows * cols;
    if (cell_count < 400) return 0;
    int div = strict ? 220 : 320;
    int limit = cell_count / div;
    return limit < 1 ? 1 : limit;
}

static int shape_limit_weak_zero(int rows, int cols, int strict) {
    int cell_count = rows * cols;
    int div = strict ? 7 : 14;
    int limit = cell_count / div;
    return limit < 1 ? 1 : limit;
}

static int shape_limit_edge_low(int rows, int cols, int strict) {
    int edge_band_count = count_edge_band_cells(rows, cols);
    if (rows * cols >= 900) {
        int limit = strict ? 50 : 45;
        if (edge_band_count < limit) limit = edge_band_count / 3;
        return limit < 1 ? 1 : limit;
    }
    int div = strict ? 4 : 8;
    int limit = edge_band_count / div;
    return limit < 1 ? 1 : limit;
}

static int is_edge_low_clue(int rows, int cols, int clue) {
    if (rows * cols >= 900) return clue == 0;
    return clue <= 1;
}

static int find_sparse_zero_pattern(int rows, int cols, const int *clues, int strict,
                                    int *out_r, int *out_c) {
    int weak_zero_count = 0;
    int zero_cross_count = 0;
    int weak_zero_limit = shape_limit_weak_zero(rows, cols, strict);
    int zero_cross_limit = shape_limit_zero_cross(rows, cols, strict);
    int edge_low_count = 0;
    int edge_low_limit = shape_limit_edge_low(rows, cols, strict);

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (!is_edge_band_cell(rows, cols, r, c)) continue;
            if (is_edge_low_clue(rows, cols, clues[r * cols + c])) {
                edge_low_count++;
                if (edge_low_count > edge_low_limit) {
                    *out_r = r;
                    *out_c = c;
                    return 1;
                }
            }
        }
    }

    for (int r = 1; r + 1 < rows; r++) {
        for (int c = 1; c + 1 < cols; c++) {
            int center = clues[r * cols + c];
            if (center != 0) continue;

            int up = clues[(r - 1) * cols + c];
            int down = clues[(r + 1) * cols + c];
            int left = clues[r * cols + c - 1];
            int right = clues[r * cols + c + 1];

            if (up == 0 && down == 0 && left == 0 && right == 0) {
                zero_cross_count++;
                if (zero_cross_count > zero_cross_limit) {
                    *out_r = r;
                    *out_c = c;
                    return 1;
                }
            }
            if (up <= 1 && down <= 1 && left <= 1 && right <= 1) {
                weak_zero_count++;
                if (weak_zero_count > weak_zero_limit) {
                    *out_r = r;
                    *out_c = c;
                    return 1;
                }
            }
        }
    }

    return 0;
}

static void count_sparse_zero_patterns(int rows, int cols, const int *clues,
                                       int *out_zero_cross, int *out_weak_zero, int *out_edge_low) {
    int zero_cross = 0;
    int weak_zero = 0;
    int edge_low = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (is_edge_band_cell(rows, cols, r, c) && is_edge_low_clue(rows, cols, clues[r * cols + c])) edge_low++;
        }
    }
    for (int r = 1; r + 1 < rows; r++) {
        for (int c = 1; c + 1 < cols; c++) {
            if (clues[r * cols + c] != 0) continue;
            int up = clues[(r - 1) * cols + c];
            int down = clues[(r + 1) * cols + c];
            int left = clues[r * cols + c - 1];
            int right = clues[r * cols + c + 1];
            if (up == 0 && down == 0 && left == 0 && right == 0) zero_cross++;
            if (up <= 1 && down <= 1 && left <= 1 && right <= 1) weak_zero++;
        }
    }
    if (out_zero_cross) *out_zero_cross = zero_cross;
    if (out_weak_zero) *out_weak_zero = weak_zero;
    if (out_edge_low) *out_edge_low = edge_low;
}

static int nearest_cell_with_value(int rows, int cols, const int *cells, int tr, int tc, int value,
                                   int *out_r, int *out_c) {
    int best_dist = rows + cols + 1;
    int found = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            if (cells[r * cols + c] != value) continue;
            int dist = abs(r - tr) + abs(c - tc);
            if (dist < best_dist) {
                best_dist = dist;
                *out_r = r;
                *out_c = c;
                found = 1;
            }
        }
    }
    return found;
}

static void fill_path_between(int rows, int cols, int *cells, int r, int c, int tr, int tc,
                              unsigned int *rng);

static int is_corner_cell(int rows, int cols, int r, int c) {
    return (r == 0 || r + 1 == rows) && (c == 0 || c + 1 == cols);
}

static int find_edge_low_repair_target(int rows, int cols, const int *clues, const int *cells,
                                       int *out_r, int *out_c) {
    for (int target_clue = 0; target_clue <= 1; target_clue++) {
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                if (!is_edge_band_cell(rows, cols, r, c)) continue;
                if (is_corner_cell(rows, cols, r, c)) continue;
                if (cells[r * cols + c] != 0) continue;
                if (clues[r * cols + c] != target_clue) continue;
                *out_r = r;
                *out_c = c;
                return 1;
            }
        }
    }
    return 0;
}

static int extend_edge_cell_inward(int rows, int cols, int *cells, int r, int c) {
    int dr = 0, dc = 0;
    if (r == 0) dr = 1;
    else if (r + 1 == rows) dr = -1;
    else if (c == 0) dc = 1;
    else if (c + 1 == cols) dc = -1;
    else return 0;

    int min_dim = rows < cols ? rows : cols;
    int max_len = min_dim / 12 + 2;
    if (max_len < 3) max_len = 3;
    if (max_len > 6) max_len = 6;

    int hit_step = -1;
    int rr = r + dr;
    int cc = c + dc;
    for (int step = 1; step <= max_len; step++) {
        if (rr < 0 || rr >= rows || cc < 0 || cc >= cols) break;
        if (cells[rr * cols + cc] != 0) {
            hit_step = step;
            break;
        }
        rr += dr;
        cc += dc;
    }
    if (hit_step < 0) return 0;

    rr = r;
    cc = c;
    for (int step = 0; step <= hit_step; step++) {
        cells[rr * cols + cc] = 1;
        break_checkerboard_around_fill(rows, cols, cells, rr, cc);
        rr += dr;
        cc += dc;
    }
    return 1;
}

static int try_edge_shape_edit(int rows, int cols, int *cells, int r1, int c1, int r2, int c2) {
    int cell_count = rows * cols;
    int *backup = (int*)malloc(cell_count * sizeof(int));
    if (!backup) return 0;
    memcpy(backup, cells, cell_count * sizeof(int));

    cells[r1 * cols + c1] = 1;
    break_checkerboard_around_fill(rows, cols, cells, r1, c1);
    if (r2 >= 0 && c2 >= 0) {
        cells[r2 * cols + c2] = 1;
        break_checkerboard_around_fill(rows, cols, cells, r2, c2);
    }

    int ok = !has_checkerboard_cells(rows, cols, cells) && cells_connected(rows, cols, cells);
    if (!ok) memcpy(cells, backup, cell_count * sizeof(int));
    free(backup);
    return ok;
}

static int repair_edge_outer_outer_inner_outer_outer_line(int rows, int cols, int *cells,
                                                         int start_r, int start_c,
                                                         int step_r, int step_c,
                                                         int length, unsigned int *rng) {
    for (int pos = 2; pos + 2 < length; pos++) {
        int r0 = start_r + step_r * (pos - 2);
        int c0 = start_c + step_c * (pos - 2);
        int r1 = start_r + step_r * (pos - 1);
        int c1 = start_c + step_c * (pos - 1);
        int r2 = start_r + step_r * pos;
        int c2 = start_c + step_c * pos;
        int r3 = start_r + step_r * (pos + 1);
        int c3 = start_c + step_c * (pos + 1);
        int r4 = start_r + step_r * (pos + 2);
        int c4 = start_c + step_c * (pos + 2);
        if (is_corner_cell(rows, cols, r1, c1) || is_corner_cell(rows, cols, r3, c3)) continue;
        if (cells[r0 * cols + c0] != 0) continue;
        if (cells[r1 * cols + c1] != 0) continue;
        if (cells[r2 * cols + c2] == 0) continue;
        if (cells[r3 * cols + c3] != 0) continue;
        if (cells[r4 * cols + c4] != 0) continue;

        int mode = rng_range(rng, 3);
        if (mode == 0 && try_edge_shape_edit(rows, cols, cells, r1, c1, -1, -1)) return 1;
        if (mode == 1 && try_edge_shape_edit(rows, cols, cells, r3, c3, -1, -1)) return 1;
        if (try_edge_shape_edit(rows, cols, cells, r1, c1, r3, c3)) return 1;
        if (try_edge_shape_edit(rows, cols, cells, r1, c1, -1, -1)) return 1;
        if (try_edge_shape_edit(rows, cols, cells, r3, c3, -1, -1)) return 1;
    }
    return 0;
}

static int repair_edge_outer_outer_inner_outer_outer(int rows, int cols, int *cells,
                                                     unsigned int *rng) {
    if (repair_edge_outer_outer_inner_outer_outer_line(rows, cols, cells, 0, 0, 0, 1, cols, rng)) return 1;
    if (repair_edge_outer_outer_inner_outer_outer_line(rows, cols, cells, rows - 1, 0, 0, 1, cols, rng)) return 1;
    if (repair_edge_outer_outer_inner_outer_outer_line(rows, cols, cells, 0, 0, 1, 0, rows, rng)) return 1;
    if (repair_edge_outer_outer_inner_outer_outer_line(rows, cols, cells, 0, cols - 1, 1, 0, rows, rng)) return 1;
    return 0;
}

static void fill_path_between(int rows, int cols, int *cells, int r, int c, int tr, int tc,
                              unsigned int *rng) {
    while (r != tr || c != tc) {
        if (r != tr && c != tc) {
            if (rng_range(rng, 2) == 0) r += (tr > r) ? 1 : -1;
            else c += (tc > c) ? 1 : -1;
        } else if (r != tr) {
            r += (tr > r) ? 1 : -1;
        } else {
            c += (tc > c) ? 1 : -1;
        }
        if (r >= 0 && r < rows && c >= 0 && c < cols) {
            cells[r * cols + c] = 1;
            break_checkerboard_around_fill(rows, cols, cells, r, c);
        }
    }
}

static int carve_path_to_outside(int rows, int cols, int *cells, int r, int c, unsigned int *rng) {
    int target_r = (r < rows - 1 - r) ? 0 : rows - 1;
    int target_c = (c < cols - 1 - c) ? 0 : cols - 1;
    int prefer_row = rng_range(rng, 2);

    while (r != target_r || c != target_c) {
        cells[r * cols + c] = 0;
        if (prefer_row && r != target_r) {
            r += (target_r > r) ? 1 : -1;
        } else if (!prefer_row && c != target_c) {
            c += (target_c > c) ? 1 : -1;
        } else if (r != target_r) {
            r += (target_r > r) ? 1 : -1;
        } else {
            c += (target_c > c) ? 1 : -1;
        }
    }
    cells[r * cols + c] = 0;
    return cells_connected(rows, cols, cells);
}

static void repair_sparse_zero_patterns(int rows, int cols, int *cells, int *edges,
                                        int *clues, unsigned int *rng) {
    int cell_count = rows * cols;
    int repair_limit = cell_count / 2;
    if (cell_count >= 900) repair_limit = cell_count / 10;
    if (repair_limit < 16) repair_limit = 16;
    int first_edge_one_repairs = 0;
    int second_edge_one_repairs = 0;
    for (int i = 0; i < repair_limit; i++) {
        build_edges_from_cells(rows, cols, cells, edges);
        clues_from_solution(rows, cols, edges, clues);

        if (rows * cols >= 900 && first_edge_one_repairs < active_edge_one_shape_repairs &&
            repair_edge_outer_outer_inner_outer_outer(rows, cols, cells, rng)) {
            first_edge_one_repairs++;
            continue;
        }

        int zr = 0, zc = 0;
        if (!find_sparse_zero_pattern(rows, cols, clues, 0, &zr, &zc)) {
            if (rows * cols >= 900 && second_edge_one_repairs < active_edge_one_shape_repairs &&
                repair_edge_outer_outer_inner_outer_outer(rows, cols, cells, rng)) {
                second_edge_one_repairs++;
                continue;
            }
            return;
        }

        if (is_edge_band_cell(rows, cols, zr, zc)) {
            int er = zr, ec = zc;
            if (!find_edge_low_repair_target(rows, cols, clues, cells, &er, &ec)) return;
            if (!extend_edge_cell_inward(rows, cols, cells, er, ec)) return;
            if (rows * cols >= 900 && second_edge_one_repairs < active_edge_one_shape_repairs &&
                repair_edge_outer_outer_inner_outer_outer(rows, cols, cells, rng)) {
                second_edge_one_repairs++;
            }
            continue;
        }

        int idx = zr * cols + zc;
        if (cells[idx] == 0) {
            int sr = 0, sc = 0;
            if (!nearest_cell_with_value(rows, cols, cells, zr, zc, 1, &sr, &sc)) return;
            fill_path_between(rows, cols, cells, sr, sc, zr, zc, rng);
        } else {
            int *backup = (int*)malloc(cell_count * sizeof(int));
            if (!backup) return;
            memcpy(backup, cells, cell_count * sizeof(int));
            if (!carve_path_to_outside(rows, cols, cells, zr, zc, rng)) {
                memcpy(cells, backup, cell_count * sizeof(int));
                int er = 0, ec = 0;
                if (nearest_cell_with_value(rows, cols, cells, zr, zc, 0, &er, &ec)) {
                    fill_path_between(rows, cols, cells, zr, zc, er, ec, rng);
                }
            }
            free(backup);
        }
        if (rows * cols >= 900 && second_edge_one_repairs < active_edge_one_shape_repairs &&
            repair_edge_outer_outer_inner_outer_outer(rows, cols, cells, rng)) {
            second_edge_one_repairs++;
        }
    }
}

static int visible_clue_indices(const int *clues, int cell_count, int *indices) {
    int count = 0;
    for (int i = 0; i < cell_count; i++) {
        if (clues[i] >= 0) indices[count++] = i;
    }
    return count;
}

static int validate_full_clue_shape(int rows, int cols, const int *clues) {
    int r = 0, c = 0;
    return !find_sparse_zero_pattern(rows, cols, clues, 1, &r, &c);
}

static int validate_clue_distribution(int rows, int cols, const int *clues, int difficulty) {
    int cell_count = rows * cols;
    int visible = 0;
    for (int i = 0; i < cell_count; i++) {
        if (clues[i] >= 0) visible++;
    }

    int density_percent = difficulty >= 2 ? 40 : (difficulty == 1 ? 43 : 46);
    int min_visible = (cell_count * density_percent + 99) / 100;
    if (visible < min_visible) return 0;

    int row_min = cols >= 6 ? 2 : 1;
    int col_min = rows >= 6 ? 2 : 1;

    for (int r = 0; r < rows; r++) {
        int row_visible = 0;
        for (int c = 0; c < cols; c++) {
            if (clues[r * cols + c] >= 0) row_visible++;
        }
        if (row_visible < row_min) return 0;
    }

    for (int c = 0; c < cols; c++) {
        int col_visible = 0;
        for (int r = 0; r < rows; r++) {
            if (clues[r * cols + c] >= 0) col_visible++;
        }
        if (col_visible < col_min) return 0;
    }

    if (rows >= 3 && cols >= 3) {
        for (int r = 0; r + 2 < rows; r++) {
            for (int c = 0; c + 2 < cols; c++) {
                int window_visible = 0;
                for (int dr = 0; dr < 3; dr++) {
                    for (int dc = 0; dc < 3; dc++) {
                        if (clues[(r + dr) * cols + c + dc] >= 0) window_visible++;
                    }
                }
                if (window_visible < 2) return 0;
            }
        }
    }

    if (rows >= 5 && cols >= 5) {
        for (int r = 0; r + 3 < rows; r++) {
            for (int c = 0; c + 3 < cols; c++) {
                int window_visible = 0;
                for (int dr = 0; dr < 4; dr++) {
                    for (int dc = 0; dc < 4; dc++) {
                        if (clues[(r + dr) * cols + c + dc] >= 0) window_visible++;
                    }
                }
                if (window_visible < 4) return 0;
            }
        }
    }

    return 1;
}

static int difficulty_target(int difficulty) {
    if (difficulty <= 0) return 0;
    if (difficulty == 1) return 30;
    return 50;
}

static int difficulty_score(const SlStats *stats) {
    if (!stats) return 0;
    return stats->dfs_branches + stats->max_depth * 8 + stats->probe_calls / 4;
}

static int evaluate_unique_puzzle(int rows, int cols, const int *clues, int difficulty, int *solution, SlStats *stats) {
    if (!validate_clue_distribution(rows, cols, clues, difficulty)) return 0;
    SlStats count_stats;
    if (sl_count_solutions(rows, cols, clues, 2, &count_stats) != 1) return 0;
    return sl_solve(rows, cols, clues, solution, stats);
}

static int improve_by_small_deletions(int rows, int cols, int difficulty, int target_score,
                                      int edge_one_priority, unsigned int *rng,
                                      int *clues, int *solution, SlStats *stats) {
    int cell_count = rows * cols;
    int edge_count = (rows + 1) * cols + rows * (cols + 1);
    int *indices = (int*)malloc(cell_count * sizeof(int));
    int *usage = (int*)malloc(cell_count * sizeof(int));
    int *trial_clues = (int*)malloc(cell_count * sizeof(int));
    int *trial_solution = (int*)malloc(edge_count * sizeof(int));
    if (!indices || !usage || !trial_clues || !trial_solution) {
        free(indices);
        free(usage);
        free(trial_clues);
        free(trial_solution);
        return 0;
    }

    int best_score = difficulty_score(stats);
    int passes = target_score > 0 ? 8 : 2;
    int small_batches[] = {4, 2, 1};

    for (int pass = 0; pass < passes && best_score < target_score; pass++) {
        for (int b = 0; b < 3 && best_score < target_score; b++) {
            int batch = small_batches[b];
            int visible = visible_clue_indices(clues, cell_count, indices);
            if (visible == 0) break;
            SlStats usage_stats;
            if (sl_analyze_clue_usage(rows, cols, clues, usage, NULL, &usage_stats)) {
                prioritize_removal_indices(rows, cols, indices, visible, clues, usage, edge_one_priority, rng);
            } else {
                shuffle_removal_indices(rows, cols, indices, visible, clues, edge_one_priority, rng);
            }

            for (int pos = 0; pos < visible && best_score < target_score; pos += batch) {
                memcpy(trial_clues, clues, cell_count * sizeof(int));
                int remove_count = batch;
                if (pos + remove_count > visible) remove_count = visible - pos;
                for (int i = 0; i < remove_count; i++) {
                    trial_clues[indices[pos + i]] = -1;
                }

                SlStats trial_stats;
                if (!evaluate_unique_puzzle(rows, cols, trial_clues, difficulty, trial_solution, &trial_stats)) {
                    continue;
                }

                int trial_score = difficulty_score(&trial_stats);
                if (trial_score >= best_score || target_score == 0) {
                    memcpy(clues, trial_clues, cell_count * sizeof(int));
                    memcpy(solution, trial_solution, edge_count * sizeof(int));
                    *stats = trial_stats;
                    best_score = trial_score;
                }
            }
        }
    }

    free(indices);
    free(usage);
    free(trial_clues);
    free(trial_solution);
    return best_score >= target_score;
}

static int sl_generate_with_mode(int rows, int cols, unsigned int seed, int difficulty,
                                 int loop_mode, int growth_bias, int edge_one_priority,
                                 int *out_clues, int *out_solution, SlStats *stats) {
    if (rows <= 1 || cols <= 1 || !out_clues) return 0;

    int cell_count = rows * cols;
    int edge_count = (rows + 1) * cols + rows * (cols + 1);
    unsigned int rng = seed ? seed : 1u;
    if (edge_one_priority < 0) edge_one_priority = 0;
    if (edge_one_priority > 100) edge_one_priority = 100;
    active_edge_one_shape_repairs = edge_one_priority / 35;
    if (active_edge_one_shape_repairs > 3) active_edge_one_shape_repairs = 3;
    int *solution = out_solution ? out_solution : (int*)malloc(edge_count * sizeof(int));
    int *trial_clues = (int*)malloc(cell_count * sizeof(int));
    int *indices = (int*)malloc(cell_count * sizeof(int));
    int *usage = (int*)malloc(cell_count * sizeof(int));
    int *best_clues = (int*)malloc(cell_count * sizeof(int));
    int *best_solution = (int*)malloc(edge_count * sizeof(int));
    if (!solution || !trial_clues || !indices || !usage || !best_clues || !best_solution) {
        if (!out_solution) free(solution);
        free(trial_clues);
        free(indices);
        free(usage);
        free(best_clues);
        free(best_solution);
        return 0;
    }

    int target_score = difficulty_target(difficulty);
    SlStats best_stats;
    reset_stats(&best_stats);
    int best_score = -1;
    int ok = 0;
    int loops_built = 0;
    int full_unique_checks = 0;
    int batch_unique_checks = 0;
    int eval_unique_checks = 0;
    int accepted_batches = 0;
    int full_shape_failures = 0;
    int full_unique_failures = 0;
    int eval_failures = 0;
    int full_shape_debug_prints = 0;
    int generation_attempts = rows * cols >= 144 ? 600 : 200;
    debug_growth_attempts = 0;
    debug_growth_connect_failures = 0;
    debug_growth_checkerboard_failures = 0;
    debug_growth_loop_failures = 0;
    debug_growth_successes = 0;
    debug_growth_grow_ms = 0.0;
    debug_growth_connect_ms = 0.0;
    debug_growth_repair_ms = 0.0;
    debug_growth_validate_ms = 0.0;
    debug_dumped_checkerboard = 0;
    debug_dumped_loop = 0;
    for (int attempt = 0; attempt < generation_attempts; attempt++) {
        if (debug_enabled() && attempt > 0 && attempt % 25 == 0) {
            fprintf(stderr,
                    "generate_progress attempt=%d loops_built=%d full_shape_failures=%d full_unique_failures=%d eval_failures=%d best_score=%d growth_attempts=%ld growth_successes=%ld checkerboard=%ld loop=%ld\n",
                    attempt, loops_built, full_shape_failures, full_unique_failures,
                    eval_failures, best_score, debug_growth_attempts, debug_growth_successes,
                    debug_growth_checkerboard_failures, debug_growth_loop_failures);
        }
        int loop_ok;
        if (loop_mode == 2) {
            loop_ok = generate_random_loop_growth(rows, cols, &rng, solution, 1, growth_bias);
        } else if (loop_mode == 1) {
            loop_ok = generate_random_loop_walk(rows, cols, &rng, solution);
        } else {
            loop_ok = generate_random_loop(rows, cols, &rng, solution);
        }
        if (!loop_ok) continue;
        loops_built++;
        clues_from_solution(rows, cols, solution, out_clues);
        if (!validate_full_clue_shape(rows, cols, out_clues)) {
            full_shape_failures++;
            if (debug_enabled() && full_shape_debug_prints < 5) {
                int zc = 0, wz = 0, el = 0;
                count_sparse_zero_patterns(rows, cols, out_clues, &zc, &wz, &el);
                fprintf(stderr,
                        "full_shape_failure zero_cross=%d/%d weak_zero=%d/%d edge_low=%d/%d\n",
                        zc, shape_limit_zero_cross(rows, cols, 1),
                        wz, shape_limit_weak_zero(rows, cols, 1),
                        el, shape_limit_edge_low(rows, cols, 1));
                full_shape_debug_prints++;
            }
            continue;
        }

        SlStats unique_stats;
        full_unique_checks++;
        if (sl_count_solutions(rows, cols, out_clues, 2, &unique_stats) != 1) {
            full_unique_failures++;
            continue;
        }

        int operation = cell_count / 2;
        if (operation < 1) operation = 1;
        while (operation > 0) {
            memcpy(trial_clues, out_clues, cell_count * sizeof(int));
            int visible = visible_clue_indices(trial_clues, cell_count, indices);
            if (visible == 0) break;
            SlStats usage_stats;
            if (sl_analyze_clue_usage(rows, cols, trial_clues, usage, NULL, &usage_stats)) {
                prioritize_removal_indices(rows, cols, indices, visible, trial_clues, usage, edge_one_priority, &rng);
            } else {
                shuffle_removal_indices(rows, cols, indices, visible, trial_clues, edge_one_priority, &rng);
            }

            int remove_count = operation < visible ? operation : visible;
            for (int i = 0; i < remove_count; i++) {
                trial_clues[indices[i]] = -1;
            }

            SlStats count_stats;
            batch_unique_checks++;
            if (validate_clue_distribution(rows, cols, trial_clues, difficulty) &&
                sl_count_solutions(rows, cols, trial_clues, 2, &count_stats) == 1) {
                memcpy(out_clues, trial_clues, cell_count * sizeof(int));
                accepted_batches++;
            }
            operation /= 2;
        }

        SlStats current_stats;
        eval_unique_checks++;
        ok = evaluate_unique_puzzle(rows, cols, out_clues, difficulty, solution, &current_stats);
        if (!ok) {
            eval_failures++;
            continue;
        }

        improve_by_small_deletions(rows, cols, difficulty, target_score, edge_one_priority, &rng, out_clues, solution, &current_stats);
        int current_score = difficulty_score(&current_stats);
        if (current_score > best_score) {
            memcpy(best_clues, out_clues, cell_count * sizeof(int));
            memcpy(best_solution, solution, edge_count * sizeof(int));
            best_stats = current_stats;
            best_score = current_score;
        }
        if (current_score >= target_score) break;
    }

    if (debug_enabled()) {
        fprintf(stderr,
                "generate_debug loops_built=%d full_shape_failures=%d full_unique_checks=%d full_unique_failures=%d batch_unique_checks=%d accepted_batches=%d eval_unique_checks=%d eval_failures=%d best_score=%d target_score=%d growth_attempts=%ld growth_connect_failures=%ld growth_checkerboard_failures=%ld growth_loop_failures=%ld growth_successes=%ld growth_grow_ms=%.3f growth_connect_ms=%.3f growth_repair_ms=%.3f growth_validate_ms=%.3f\n",
                loops_built, full_shape_failures, full_unique_checks, full_unique_failures,
                batch_unique_checks, accepted_batches, eval_unique_checks, eval_failures,
                best_score, target_score, debug_growth_attempts,
                debug_growth_connect_failures, debug_growth_checkerboard_failures,
                debug_growth_loop_failures, debug_growth_successes, debug_growth_grow_ms,
                debug_growth_connect_ms, debug_growth_repair_ms, debug_growth_validate_ms);
    }

    ok = best_score >= 0;
    if (ok) {
        memcpy(out_clues, best_clues, cell_count * sizeof(int));
        if (out_solution) memcpy(out_solution, best_solution, edge_count * sizeof(int));
        if (stats) *stats = best_stats;
    }

    if (!out_solution) free(solution);
    free(trial_clues);
    free(indices);
    free(usage);
    free(best_clues);
    free(best_solution);
    return ok;
}

int sl_generate(int rows, int cols, unsigned int seed, int difficulty, int *out_clues, int *out_solution, SlStats *stats) {
    return sl_generate_with_mode(rows, cols, seed, difficulty, 0, 50, 35, out_clues, out_solution, stats);
}

int sl_generate_walk(int rows, int cols, unsigned int seed, int difficulty, int *out_clues, int *out_solution, SlStats *stats) {
    return sl_generate_with_mode(rows, cols, seed, difficulty, 1, 50, 35, out_clues, out_solution, stats);
}

int sl_generate_growth(int rows, int cols, unsigned int seed, int difficulty, int *out_clues, int *out_solution, SlStats *stats) {
    return sl_generate_growth_with_bias(rows, cols, seed, difficulty, 50, out_clues, out_solution, stats);
}

int sl_generate_growth_with_bias(int rows, int cols, unsigned int seed, int difficulty, int growth_bias, int *out_clues, int *out_solution, SlStats *stats) {
    return sl_generate_growth_with_options(rows, cols, seed, difficulty, growth_bias, 35, out_clues, out_solution, stats);
}

int sl_generate_growth_with_options(int rows, int cols, unsigned int seed, int difficulty, int growth_bias, int edge_one_priority, int *out_clues, int *out_solution, SlStats *stats) {
    return sl_generate_with_mode(rows, cols, seed, difficulty, 2, growth_bias, edge_one_priority, out_clues, out_solution, stats);
}

int sl_generate_growth_preview(int rows, int cols, unsigned int seed, int *out_clues, int *out_solution) {
    return sl_generate_growth_preview_with_bias(rows, cols, seed, 50, out_clues, out_solution);
}

int sl_generate_growth_preview_with_bias(int rows, int cols, unsigned int seed, int growth_bias, int *out_clues, int *out_solution) {
    if (rows <= 1 || cols <= 1 || !out_clues || !out_solution) return 0;

    unsigned int rng = seed ? seed : 1u;
    int attempts = rows * cols >= 225 ? 20 : 80;
    for (int i = 0; i < attempts; i++) {
        if (!generate_random_loop_growth(rows, cols, &rng, out_solution, 0, growth_bias)) continue;
        clues_from_solution(rows, cols, out_solution, out_clues);
        return 1;
    }
    return 0;
}

int sl_solve(int rows, int cols, const int *clues, int *out_edges, SlStats *stats) {
    reset_stats(stats);
    clock_t start = clock();
    if (!init_from_clues(rows, cols, clues)) {
        free_state();
        return 0;
    }

    active_stats = stats;
    int ok = solve_one(0);
    if (ok && out_edges) memcpy(out_edges, edge_state, E * sizeof(int));
    active_stats = NULL;

    if (stats) {
        stats->solution_count = ok ? 1 : 0;
        stats->elapsed_ms = 1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC;
    }
    free_state();
    return ok;
}

int sl_analyze_clue_usage(int rows, int cols, const int *clues, int *out_usage, int *out_edges, SlStats *stats) {
    if (!out_usage) return 0;
    int cell_count = rows * cols;
    for (int i = 0; i < cell_count; i++) out_usage[i] = 0;

    reset_stats(stats);
    clock_t start = clock();
    if (!init_from_clues(rows, cols, clues)) {
        free_state();
        return 0;
    }

    active_stats = stats;
    active_clue_usage = out_usage;
    int ok = solve_one(0);
    active_clue_usage = NULL;
    if (ok && out_edges) memcpy(out_edges, edge_state, E * sizeof(int));
    if (stats) {
        stats->solution_count = ok ? 1 : 0;
        stats->elapsed_ms = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
    }
    active_stats = NULL;
    free_state();
    return ok;
}

int sl_count_solutions(int rows, int cols, const int *clues, int limit, SlStats *stats) {
    if (limit <= 0) limit = 2;
    reset_stats(stats);
    clock_t start = clock();
    if (!init_from_clues(rows, cols, clues)) {
        free_state();
        return 0;
    }

    active_stats = stats;
    int count = solve_count(limit, 0, NULL);
    active_stats = NULL;

    if (stats) {
        if (stats->solution_count < count) stats->solution_count = count;
        stats->elapsed_ms = 1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC;
    }
    free_state();
    return stats ? stats->solution_count : count;
}

int sl_load_puzzle_file(const char *filename, int *out_rows, int *out_cols, int **out_clues) {
    if (!filename || !out_rows || !out_cols || !out_clues) return 0;

    FILE *fp = fopen(filename, "r");
    if (!fp) return 0;

    char line[4096];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }

    int rows = 0, cols = 0;
    if (sscanf(line, "%d,%d", &rows, &cols) != 2 || rows <= 0 || cols <= 0) {
        fclose(fp);
        return 0;
    }

    int *clues = (int*)malloc(rows * cols * sizeof(int));
    if (!clues) {
        fclose(fp);
        return 0;
    }

    for (int r = 0; r < rows; r++) {
        if (!fgets(line, sizeof(line), fp)) line[0] = '\0';
        int c = 0;
        char *p = line;
        while (c < cols && *p) {
            if (*p == ',') {
                clues[r * cols + c++] = -1;
                p++;
            } else if (*p >= '0' && *p <= '3') {
                clues[r * cols + c++] = *p - '0';
                p++;
                if (*p == ',') p++;
            } else {
                p++;
            }
        }
        while (c < cols) clues[r * cols + c++] = -1;
    }

    fclose(fp);
    *out_rows = rows;
    *out_cols = cols;
    *out_clues = clues;
    return 1;
}

void sl_free(void *ptr) {
    free(ptr);
}

void sl_print_board(int rows, int cols, const int *clues, const int *edges) {
    int horizontal_count = (rows + 1) * cols;
    for (int r = 0; r <= rows; r++) {
        for (int c = 0; c <= cols; c++) {
            printf("+");
            if (c < cols) {
                int e = r * cols + c;
                printf(edges && edges[e] == LINE ? "---" : "   ");
            }
        }
        printf("\n");
        if (r < rows) {
            for (int c = 0; c <= cols; c++) {
                int e = horizontal_count + r * (cols + 1) + c;
                printf(edges && edges[e] == LINE ? "|" : " ");
                if (c < cols) {
                    int clue = clues[r * cols + c];
                    if (clue >= 0) printf(" %d ", clue);
                    else printf("   ");
                }
            }
            printf("\n");
        }
    }
}
