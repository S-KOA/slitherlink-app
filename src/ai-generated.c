#include "slitherlink_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_usage(const char *program) {
    fprintf(stderr, "Usage: %s [--count [limit] | --usage] <puzzle file>\n", program);
    fprintf(stderr, "       %s --generate <rows> <cols> [seed] [difficulty: 0=easy, 1=normal, 2=hard]\n", program);
}

static void print_stats(const SlStats *stats) {
    printf("solutions=%d\n", stats->solution_count);
    printf("dfs_branches=%d\n", stats->dfs_branches);
    printf("probe_calls=%d\n", stats->probe_calls);
    printf("max_depth=%d\n", stats->max_depth);
    printf("elapsed_ms=%.3f\n", stats->elapsed_ms);
}

static void print_puzzle_csv(int rows, int cols, const int *clues) {
    printf("%d,%d\n", rows, cols);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int clue = clues[r * cols + c];
            if (clue >= 0) printf("%d", clue);
            if (c + 1 < cols) printf(",");
        }
        printf("\n");
    }
}

static void print_usage_csv(int rows, int cols, const int *clues, const int *usage) {
    printf("%d,%d\n", rows, cols);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            if (clues[idx] >= 0) printf("%d", usage[idx] ? 1 : 0);
            if (c + 1 < cols) printf(",");
        }
        printf("\n");
    }
}

int main(int argc, char **argv) {
    int count_mode = 0;
    int usage_mode = 0;
    int limit = 2;
    const char *filename = NULL;

    if (argc >= 4 && strcmp(argv[1], "--generate") == 0) {
        int rows = atoi(argv[2]);
        int cols = atoi(argv[3]);
        unsigned int seed = (argc >= 5) ? (unsigned int)strtoul(argv[4], NULL, 10) : (unsigned int)time(NULL);
        int difficulty = (argc >= 6) ? atoi(argv[5]) : 0;
        if (rows <= 1 || cols <= 1) {
            print_usage(argv[0]);
            return 1;
        }

        int cell_count = rows * cols;
        int edge_count = (rows + 1) * cols + rows * (cols + 1);
        int *clues = (int*)malloc(cell_count * sizeof(int));
        int *solution = (int*)malloc(edge_count * sizeof(int));
        if (!clues || !solution) {
            fprintf(stderr, "error: out of memory\n");
            sl_free(clues);
            sl_free(solution);
            return 1;
        }

        SlStats stats;
        if (!sl_generate(rows, cols, seed, difficulty, clues, solution, &stats)) {
            fprintf(stderr, "error: failed to generate puzzle\n");
            sl_free(clues);
            sl_free(solution);
            return 1;
        }

        print_puzzle_csv(rows, cols, clues);
        printf("\nSolved:\n");
        sl_print_board(rows, cols, clues, solution);
        printf("\n");
        print_stats(&stats);
        sl_free(clues);
        sl_free(solution);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--count") == 0) {
            count_mode = 1;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                limit = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "--usage") == 0) {
            usage_mode = 1;
        } else {
            filename = argv[i];
        }
    }

    if (!filename) {
        print_usage(argv[0]);
        return 1;
    }

    int rows = 0, cols = 0;
    int *clues = NULL;
    if (!sl_load_puzzle_file(filename, &rows, &cols, &clues)) {
        fprintf(stderr, "error: cannot load %s\n", filename);
        return 1;
    }

    SlStats stats;
    if (count_mode) {
        int count = sl_count_solutions(rows, cols, clues, limit, &stats);
        printf("Solution count%s: %d\n", count >= limit ? " (limited)" : "", count);
        print_stats(&stats);
    } else if (usage_mode) {
        int cell_count = rows * cols;
        int edge_count = (rows + 1) * cols + rows * (cols + 1);
        int *usage = (int*)calloc(cell_count, sizeof(int));
        int *edges = (int*)calloc(edge_count, sizeof(int));
        if (!usage || !edges) {
            fprintf(stderr, "error: out of memory\n");
            sl_free(usage);
            sl_free(edges);
            sl_free(clues);
            return 1;
        }

        if (sl_analyze_clue_usage(rows, cols, clues, usage, edges, &stats)) {
            print_usage_csv(rows, cols, clues, usage);
            print_stats(&stats);
        } else {
            printf("No solution found.\n");
            print_stats(&stats);
        }
        sl_free(usage);
        sl_free(edges);
    } else {
        int edge_count = (rows + 1) * cols + rows * (cols + 1);
        int *edges = (int*)calloc(edge_count, sizeof(int));
        if (!edges) {
            fprintf(stderr, "error: out of memory\n");
            sl_free(clues);
            return 1;
        }

        if (sl_solve(rows, cols, clues, edges, &stats)) {
            printf("Solved:\n");
            sl_print_board(rows, cols, clues, edges);
            print_stats(&stats);
        } else {
            printf("No solution found.\n");
            print_stats(&stats);
        }
        sl_free(edges);
    }

    sl_free(clues);
    return 0;
}
