#include "slitherlink_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_usage(const char *program) {
    fprintf(stderr, "Usage: %s <rows> <cols> [difficulty: 0=easy, 1=normal, 2=hard] [seed] [mode: classic|walk|growth|growth-preview|growth-islands] [growth_bias: 0..100] [edge_shape_priority: 0..100]\n", program);
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

static char island_char(int id) {
    const char *chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    if (id < 0) return '.';
    return chars[id % 62];
}

static void print_islands(int rows, int cols, const int *island_ids, const int *states) {
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            if (island_ids[idx] >= 0) putchar(island_char(island_ids[idx]));
            else if (states[idx] == 3) putchar('#');
            else putchar('.');
        }
        putchar('\n');
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    int rows = atoi(argv[1]);
    int cols = atoi(argv[2]);
    int difficulty = argc >= 4 ? atoi(argv[3]) : 0;
    unsigned int seed = argc >= 5 ? (unsigned int)strtoul(argv[4], NULL, 10) : (unsigned int)time(NULL);
    int mode = 0;
    if (argc >= 6 && strcmp(argv[5], "walk") == 0) mode = 1;
    if (argc >= 6 && strcmp(argv[5], "growth") == 0) mode = 2;
    if (argc >= 6 && strcmp(argv[5], "growth-preview") == 0) mode = 3;
    if (argc >= 6 && strcmp(argv[5], "growth-islands") == 0) mode = 4;
    int growth_bias = argc >= 7 ? atoi(argv[6]) : 50;
    if (growth_bias < 0) growth_bias = 0;
    if (growth_bias > 100) growth_bias = 100;
    int edge_one_priority = argc >= 8 ? atoi(argv[7]) : 35;
    if (edge_one_priority < 0) edge_one_priority = 0;
    if (edge_one_priority > 100) edge_one_priority = 100;

    if (rows <= 1 || cols <= 1) {
        print_usage(argv[0]);
        return 1;
    }

    int cell_count = rows * cols;
    int edge_count = (rows + 1) * cols + rows * (cols + 1);
    if (mode == 4) {
        int *island_ids = (int*)malloc(cell_count * sizeof(int));
        int *states = (int*)malloc(cell_count * sizeof(int));
        int island_count = 0;
        if (!island_ids || !states) {
            fprintf(stderr, "error: out of memory\n");
            sl_free(island_ids);
            sl_free(states);
            return 1;
        }
        if (!sl_generate_growth_islands_preview(rows, cols, seed, growth_bias, island_ids, states, &island_count)) {
            fprintf(stderr, "error: failed to generate island preview\n");
            sl_free(island_ids);
            sl_free(states);
            return 1;
        }
        print_islands(rows, cols, island_ids, states);
        fprintf(stderr, "seed=%u mode=growth-islands growth_bias=%d islands=%d\n", seed, growth_bias, island_count);
        sl_free(island_ids);
        sl_free(states);
        return 0;
    }

    int *clues = (int*)malloc(cell_count * sizeof(int));
    int *solution = (int*)malloc(edge_count * sizeof(int));
    if (!clues || !solution) {
        fprintf(stderr, "error: out of memory\n");
        sl_free(clues);
        sl_free(solution);
        return 1;
    }

    SlStats stats;
    int ok;
    if (mode == 3) {
        ok = sl_generate_growth_preview_with_bias(rows, cols, seed, growth_bias, clues, solution);
        if (ok) {
            stats.solution_count = 0;
            stats.dfs_branches = 0;
            stats.probe_calls = 0;
            stats.max_depth = 0;
            stats.elapsed_ms = 0.0;
        }
    } else if (mode == 2) {
        ok = sl_generate_growth_with_options(rows, cols, seed, difficulty, growth_bias, edge_one_priority, clues, solution, &stats);
    } else if (mode == 1) {
        ok = sl_generate_walk(rows, cols, seed, difficulty, clues, solution, &stats);
    } else {
        ok = sl_generate(rows, cols, seed, difficulty, clues, solution, &stats);
    }
    if (!ok) {
        fprintf(stderr, "error: failed to generate puzzle\n");
        sl_free(clues);
        sl_free(solution);
        return 1;
    }

    print_puzzle_csv(rows, cols, clues);
    const char *mode_name = mode == 3 ? "growth-preview" : (mode == 2 ? "growth" : (mode == 1 ? "walk" : "classic"));
    if (mode == 3) {
        fprintf(stderr, "Preview boundary:\n");
        sl_print_board(rows, cols, clues, solution);
    }
    fprintf(stderr, "seed=%u difficulty=%d mode=%s growth_bias=%d edge_shape_priority=%d solutions=%d dfs_branches=%d probe_calls=%d max_depth=%d elapsed_ms=%.3f\n",
            seed, difficulty, mode_name, growth_bias, edge_one_priority,
            stats.solution_count, stats.dfs_branches,
            stats.probe_calls, stats.max_depth, stats.elapsed_ms);

    sl_free(clues);
    sl_free(solution);
    return 0;
}
