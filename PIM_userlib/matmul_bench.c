#include "bench.h"
#include <stdlib.h>


void init_matmul(struct bench_t *this_bench, struct bench_arg_t args){
    this_bench->args = args;
    size_t matrix_bytes = sizeof(double) * args.obj_cnt0 * args.obj_cnt0;

    this_bench->args.size0 = matrix_bytes; // mat A
    this_bench->args.size1 = matrix_bytes; // mat B
    this_bench->args.size2 = matrix_bytes; // mat C
}

int prepare_matmul(struct bench_t *this_bench){
    this_bench->args.mem0 = calloc(1, this_bench->args.size0);
    this_bench->args.mem1 = calloc(1, this_bench->args.size1);
    this_bench->args.mem2 = calloc(1, this_bench->args.size2);

    size_t N = this_bench->args.obj_cnt0;
    double *A = (double*)this_bench->args.mem0;
    double *B = (double*)this_bench->args.mem1;

    for(size_t i = 0; i < N * N; i++){
        A[i] = (double)rand() / RAND_MAX;
        B[i] = (double)rand() / RAND_MAX;
    }
    return 0;
}

void calc_matmul_share(struct bench_t *this_bench) {
    size_t N = this_bench->args.obj_cnt0;
    double *A = (double*)this_bench->args.mem0;
    double *B = (double*)this_bench->args.mem1;
    double *C = (double*)this_bench->args.mem2;
    int T = this_bench->args.config_const0;

    int core_id_a = this_bench->args.avail_cores[0];
    int core_id_b = this_bench->args.avail_cores[1];
    int core_id_c = this_bench->args.avail_cores[2];

    int NT = (int)((N + T - 1) / T);   // tiles per dimension
    int total_tiles = NT * NT * NT;

    double A_tile[T][T];
    double B_tile[T][T];
    double result_cache[T][T];

    // +2 to drain last two tiles through calc and write stages
    for (int slot = 0; slot < total_tiles + 2; slot++) {
        int s_w = slot - 2;
        int s_c = slot - 1;
        int s_r = slot;

        // Write stage (CPU mode): accumulate result_cache into C
        if (s_w >= 0 && s_w < total_tiles) {
            int i_w = (s_w / (NT * NT)) * T;
            int j_w = ((s_w / NT) % NT)  * T;
            int rows = ((i_w + T) < (int)N) ? T : (int)N - i_w;
            int cols = ((j_w + T) < (int)N) ? T : (int)N - j_w;
            pause_core(this_bench->args.user, core_id_c);
            for (int ii = 0; ii < rows; ii++)
                for (int jj = 0; jj < cols; jj++)
                    C[(i_w + ii) * N + (j_w + jj)] += result_cache[ii][jj];
            resume_core(this_bench->args.user, core_id_c);
        }

        // Calc stage (PIM mode): compute A_tile * B_tile into result_cache
        if (s_c >= 0 && s_c < total_tiles) {
            int i_c = (s_c / (NT * NT)) * T;
            int j_c = ((s_c / NT) % NT)  * T;
            int k_c = (s_c % NT)          * T;
            int rows  = ((i_c + T) < (int)N) ? T : (int)N - i_c;
            int cols  = ((j_c + T) < (int)N) ? T : (int)N - j_c;
            int depth = ((k_c + T) < (int)N) ? T : (int)N - k_c;
            for (int ii = 0; ii < rows; ii++) {
                for (int jj = 0; jj < cols; jj++) {
                    double sum = 0;
                    for (int kk = 0; kk < depth; kk++)
                        sum += A_tile[ii][kk] * B_tile[kk][jj];
                    result_cache[ii][jj] = sum;
                }
            }
        }

        // Read stage (CPU mode): load A and B tiles into cache
        if (s_r < total_tiles) {
            int i_r = (s_r / (NT * NT)) * T;
            int j_r = ((s_r / NT) % NT)  * T;
            int k_r = (s_r % NT)          * T;
            int rows  = ((i_r + T) < (int)N) ? T : (int)N - i_r;
            int cols  = ((j_r + T) < (int)N) ? T : (int)N - j_r;
            int depth = ((k_r + T) < (int)N) ? T : (int)N - k_r;
            pause_core(this_bench->args.user, core_id_a);
            pause_core(this_bench->args.user, core_id_b);
            for (int ii = 0; ii < rows; ii++)
                for (int kk = 0; kk < depth; kk++)
                    A_tile[ii][kk] = A[(i_r + ii) * N + (k_r + kk)];
            for (int kk = 0; kk < depth; kk++)
                for (int jj = 0; jj < cols; jj++)
                    B_tile[kk][jj] = B[(k_r + kk) * N + (j_r + jj)];
            resume_core(this_bench->args.user, core_id_a);
            resume_core(this_bench->args.user, core_id_b);
        }
    }
}

void calc_matmul_base(struct bench_t *this_bench) {
    size_t N = this_bench->args.obj_cnt0;
    double *A = (double*)this_bench->args.mem0;
    double *B = (double*)this_bench->args.mem1;
    double *C = (double*)this_bench->args.mem2;
    int T = this_bench->args.config_const0;

    int core_id_a = this_bench->args.avail_cores[0];
    int core_id_b = this_bench->args.avail_cores[1];
    int core_id_c = this_bench->args.avail_cores[2];

    int NT = (int)((N + T - 1) / T);   // tiles per dimension
    int total_tiles = NT * NT * NT;

    double A_tile[T][T];
    double B_tile[T][T];
    double result_cache[T][T];

    // +2 to drain last two tiles through calc and write stages
    for (int slot = 0; slot < total_tiles + 2; slot++) {
        int s_w = slot - 2;
        int s_c = slot - 1;
        int s_r = slot;

        // Write stage (CPU mode): accumulate result_cache into C
        if (s_w >= 0 && s_w < total_tiles) {
            int i_w = (s_w / (NT * NT)) * T;
            int j_w = ((s_w / NT) % NT)  * T;
            int rows = ((i_w + T) < (int)N) ? T : (int)N - i_w;
            int cols = ((j_w + T) < (int)N) ? T : (int)N - j_w;
            // pause_core(this_bench->args.user, core_id_c);
            for (int ii = 0; ii < rows; ii++)
                for (int jj = 0; jj < cols; jj++)
                    C[(i_w + ii) * N + (j_w + jj)] += result_cache[ii][jj];
            // resume_core(this_bench->args.user, core_id_c);
        }

        // Calc stage (PIM mode): compute A_tile * B_tile into result_cache
        if (s_c >= 0 && s_c < total_tiles) {
            int i_c = (s_c / (NT * NT)) * T;
            int j_c = ((s_c / NT) % NT)  * T;
            int k_c = (s_c % NT)          * T;
            int rows  = ((i_c + T) < (int)N) ? T : (int)N - i_c;
            int cols  = ((j_c + T) < (int)N) ? T : (int)N - j_c;
            int depth = ((k_c + T) < (int)N) ? T : (int)N - k_c;
            for (int ii = 0; ii < rows; ii++) {
                for (int jj = 0; jj < cols; jj++) {
                    double sum = 0;
                    for (int kk = 0; kk < depth; kk++)
                        sum += A_tile[ii][kk] * B_tile[kk][jj];
                    result_cache[ii][jj] = sum;
                }
            }
        }

        // Read stage (CPU mode): load A and B tiles into cache
        if (s_r < total_tiles) {
            int i_r = (s_r / (NT * NT)) * T;
            int j_r = ((s_r / NT) % NT)  * T;
            int k_r = (s_r % NT)          * T;
            int rows  = ((i_r + T) < (int)N) ? T : (int)N - i_r;
            int cols  = ((j_r + T) < (int)N) ? T : (int)N - j_r;
            int depth = ((k_r + T) < (int)N) ? T : (int)N - k_r;
            // pause_core(this_bench->args.user, core_id_a);
            // pause_core(this_bench->args.user, core_id_b);
            for (int ii = 0; ii < rows; ii++)
                for (int kk = 0; kk < depth; kk++)
                    A_tile[ii][kk] = A[(i_r + ii) * N + (k_r + kk)];
            for (int kk = 0; kk < depth; kk++)
                for (int jj = 0; jj < cols; jj++)
                    B_tile[kk][jj] = B[(k_r + kk) * N + (j_r + jj)];
            // resume_core(this_bench->args.user, core_id_a);
            // resume_core(this_bench->args.user, core_id_b);
        }
    }
}


// void calc_matmul(struct bench_t *this_bench) {
//     size_t N = this_bench->args.obj_cnt0;
//     double *A = (double*)this_bench->args.mem0;
//     double *B = (double*)this_bench->args.mem1;
//     double *C = (double*)this_bench->args.mem2;
//     int TILE_SIZE = this_bench->args.config_const0;

//     // assume each memory lcoation match to same core for simplicity
//     int core_id_a = this_bench->args.avail_cores[0];
//     int core_id_b = this_bench->args.avail_cores[1];
//     int core_id_c = this_bench->args.avail_cores[2];

//     for (size_t i = 0; i < N; i += TILE_SIZE) {
//         for (size_t j = 0; j < N; j += TILE_SIZE) {
//             for (size_t k = 0; k < N; k += TILE_SIZE) {
//                 for (size_t ii = i; ii < i + TILE_SIZE && ii < N; ii++) {
//                     for (size_t jj = j; jj < j + TILE_SIZE && jj < N; jj++) {
//                         double sum = 0;
//                         for (size_t kk = k; kk < k + TILE_SIZE && kk < N; kk++) {
//                             sum += A[ii * N + kk] * B[kk * N + jj];
//                         }
//                         C[ii * N + jj] += sum;
//                     }
//                 }
//             }
//         }
//     }
// }



void clean_matmul(struct bench_t *this_bench){
    free(this_bench->args.mem0);
    free(this_bench->args.mem1);
    free(this_bench->args.mem2);
}

struct bench_t matmul_bench\
= {
   .args = (struct bench_arg_t){0},
   .init = init_matmul,
   .prepare = prepare_matmul,
   .calc_base = calc_matmul_base,
   .calc_share = calc_matmul_share,
   .clean = clean_matmul
};
