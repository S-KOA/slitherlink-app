#ifndef SLITHERLINK_CORE_H
#define SLITHERLINK_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

#define SL_UNKNOWN 0
#define SL_LINE 1
#define SL_BLANK -1

typedef struct {
    int solution_count;
    int dfs_branches;
    int probe_calls;
    int max_depth;
    double elapsed_ms;
} SlStats;

int sl_solve(int rows, int cols, const int *clues, int *out_edges, SlStats *stats);
int sl_count_solutions(int rows, int cols, const int *clues, int limit, SlStats *stats);
int sl_analyze_clue_usage(int rows, int cols, const int *clues, int *out_usage, int *out_edges, SlStats *stats);
int sl_generate(int rows, int cols, unsigned int seed, int difficulty, int *out_clues, int *out_solution, SlStats *stats);
int sl_generate_walk(int rows, int cols, unsigned int seed, int difficulty, int *out_clues, int *out_solution, SlStats *stats);
int sl_generate_growth(int rows, int cols, unsigned int seed, int difficulty, int *out_clues, int *out_solution, SlStats *stats);
int sl_generate_growth_with_bias(int rows, int cols, unsigned int seed, int difficulty, int growth_bias, int *out_clues, int *out_solution, SlStats *stats);
int sl_generate_growth_with_options(int rows, int cols, unsigned int seed, int difficulty, int growth_bias, int edge_one_priority, int *out_clues, int *out_solution, SlStats *stats);
int sl_generate_growth_preview(int rows, int cols, unsigned int seed, int *out_clues, int *out_solution);
int sl_generate_growth_preview_with_bias(int rows, int cols, unsigned int seed, int growth_bias, int *out_clues, int *out_solution);
int sl_generate_growth_islands_preview(int rows, int cols, unsigned int seed, int growth_bias, int *out_island_ids, int *out_states, int *out_island_count);
int sl_load_puzzle_file(const char *filename, int *out_rows, int *out_cols, int **out_clues);
void sl_free(void *ptr);
void sl_print_board(int rows, int cols, const int *clues, const int *edges);

#ifdef __cplusplus
}
#endif

#endif
