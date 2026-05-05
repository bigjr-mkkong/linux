#include "bench.h"
#include "pim_runtime.h"
#include "time.h"
#include <bits/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>


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
    int TILE_SIZE = this_bench->args.config_const0;

    // Calculate total number of tiles in each dimension (handling remainders)
    int total_i_tiles = (N + TILE_SIZE - 1) / TILE_SIZE;
    int total_j_tiles = (N + TILE_SIZE - 1) / TILE_SIZE;
    int total_k_tiles = (N + TILE_SIZE - 1) / TILE_SIZE;

    int core_id_a = this_bench->args.avail_cores[0];
    int core_id_b = this_bench->args.avail_cores[1];

    // The FSM needs to run once for every block combination of (i, j, k)
    int total_slots = total_i_tiles * total_j_tiles * total_k_tiles;

    // Dynamically allocate imaginary caches (prevents large VLA stack overflow)
    double *A_cache = (double*)malloc(TILE_SIZE * TILE_SIZE * sizeof(double));
    double *B_cache = (double*)malloc(TILE_SIZE * TILE_SIZE * sizeof(double));
    double *C_cache = (double*)malloc(TILE_SIZE * TILE_SIZE * sizeof(double));

    struct timespec begin, end;
    long long elapsed_ns;

    int mode = 0; // 0 -> READ, 1 -> CALC, 2 -> WRITE
    int cache_cnt = 0; // Track which tile combination we are processing

    clock_gettime(CLOCK_MONOTONIC, &begin);

    while (cache_cnt < total_slots) {

        // Unflatten the 1D cache_cnt back into 3D tile coordinates
        int k_tile = cache_cnt % total_k_tiles;
        int j_tile = (cache_cnt / total_k_tiles) % total_j_tiles;
        int i_tile = cache_cnt / (total_k_tiles * total_j_tiles);

        size_t i_start = i_tile * TILE_SIZE;
        size_t j_start = j_tile * TILE_SIZE;
        size_t k_start = k_tile * TILE_SIZE;

        // Calculate lengths safely to handle the non-divisible tail edges of the matrices
        size_t i_len = (i_start + TILE_SIZE > N) ? (N - i_start) : TILE_SIZE;
        size_t j_len = (j_start + TILE_SIZE > N) ? (N - j_start) : TILE_SIZE;
        size_t k_len = (k_start + TILE_SIZE > N) ? (N - k_start) : TILE_SIZE;

        if (mode == 0) {
            // STAGE 0: READ - Load tiles for A, B, and the current partial results of C
            pause_core(this_bench->args.user, core_id_a);
            for (size_t ii = 0; ii < i_len; ii++) {
                for (size_t kk = 0; kk < k_len; kk++) {
                    A_cache[ii * TILE_SIZE + kk] = A[(i_start + ii) * N + (k_start + kk)];
                }
            }
            for (size_t kk = 0; kk < k_len; kk++) {
                for (size_t jj = 0; jj < j_len; jj++) {
                    B_cache[kk * TILE_SIZE + jj] = B[(k_start + kk) * N + (j_start + jj)];
                }
            }
            for (size_t ii = 0; ii < i_len; ii++) {
                for (size_t jj = 0; jj < j_len; jj++) {
                    C_cache[ii * TILE_SIZE + jj] = C[(i_start + ii) * N + (j_start + jj)];
                }
            }
            resume_core(this_bench->args.user, core_id_a);
            mode = 1;

        } else if (mode == 1) {
            // STAGE 1: CALC - Perform matrix multiplication strictly on the local cache
            for (size_t ii = 0; ii < i_len; ii++) {
                for (size_t jj = 0; jj < j_len; jj++) {
                    double sum = 0;
                    for (size_t kk = 0; kk < k_len; kk++) {
                        sum += A_cache[ii * TILE_SIZE + kk] * B_cache[kk * TILE_SIZE + jj];
                    }
                    // Accumulate the computed dot product sum to C's cache
                    C_cache[ii * TILE_SIZE + jj] += sum;
                }
            }
            mode = 2;

        } else if (mode == 2) {
            // STAGE 2: WRITE - Write the updated partial C tile back out to main memory
            pause_core(this_bench->args.user, core_id_b);
            for (size_t ii = 0; ii < i_len; ii++) {
                for (size_t jj = 0; jj < j_len; jj++) {
                    C[(i_start + ii) * N + (j_start + jj)] = C_cache[ii * TILE_SIZE + jj];
                }
            }
            resume_core(this_bench->args.user, core_id_b);
            mode = 0;

            // Advance to the next 3D tile combination
            cache_cnt++;

        } else {
            fprintf(stderr, "calc_matmul_share(): Invalid state: %d\n", mode);
            exit(-1);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    // Clean up caches
    free(A_cache);
    free(B_cache);
    free(C_cache);

    elapsed_ns = (end.tv_sec - begin.tv_sec) * 1000000000LL + (end.tv_nsec - begin.tv_nsec);
    printf("Matmul Share Case time: %lld\n", elapsed_ns);
}

void calc_matmul_base(struct bench_t *this_bench) {
    size_t N = this_bench->args.obj_cnt0;
    double *A = (double*)this_bench->args.mem0;
    double *B = (double*)this_bench->args.mem1;
    double *C = (double*)this_bench->args.mem2;
    int TILE_SIZE = this_bench->args.config_const0;

    // Calculate total number of tiles in each dimension (handling remainders)
    int total_i_tiles = (N + TILE_SIZE - 1) / TILE_SIZE;
    int total_j_tiles = (N + TILE_SIZE - 1) / TILE_SIZE;
    int total_k_tiles = (N + TILE_SIZE - 1) / TILE_SIZE;

    // The FSM needs to run once for every block combination of (i, j, k)
    int total_slots = total_i_tiles * total_j_tiles * total_k_tiles;

    // Dynamically allocate imaginary caches (prevents large VLA stack overflow)
    double *A_cache = (double*)malloc(TILE_SIZE * TILE_SIZE * sizeof(double));
    double *B_cache = (double*)malloc(TILE_SIZE * TILE_SIZE * sizeof(double));
    double *C_cache = (double*)malloc(TILE_SIZE * TILE_SIZE * sizeof(double));

    struct timespec begin, end;
    long long elapsed_ns;

    int mode = 0; // 0 -> READ, 1 -> CALC, 2 -> WRITE
    int cache_cnt = 0; // Track which tile combination we are processing

    clock_gettime(CLOCK_MONOTONIC, &begin);

    while (cache_cnt < total_slots) {

        // Unflatten the 1D cache_cnt back into 3D tile coordinates
        int k_tile = cache_cnt % total_k_tiles;
        int j_tile = (cache_cnt / total_k_tiles) % total_j_tiles;
        int i_tile = cache_cnt / (total_k_tiles * total_j_tiles);

        size_t i_start = i_tile * TILE_SIZE;
        size_t j_start = j_tile * TILE_SIZE;
        size_t k_start = k_tile * TILE_SIZE;

        // Calculate lengths safely to handle the non-divisible tail edges of the matrices
        size_t i_len = (i_start + TILE_SIZE > N) ? (N - i_start) : TILE_SIZE;
        size_t j_len = (j_start + TILE_SIZE > N) ? (N - j_start) : TILE_SIZE;
        size_t k_len = (k_start + TILE_SIZE > N) ? (N - k_start) : TILE_SIZE;

        if (mode == 0) {
            // STAGE 0: READ - Load tiles for A, B, and the current partial results of C
            for (size_t ii = 0; ii < i_len; ii++) {
                for (size_t kk = 0; kk < k_len; kk++) {
                    A_cache[ii * TILE_SIZE + kk] = A[(i_start + ii) * N + (k_start + kk)];
                }
            }
            for (size_t kk = 0; kk < k_len; kk++) {
                for (size_t jj = 0; jj < j_len; jj++) {
                    B_cache[kk * TILE_SIZE + jj] = B[(k_start + kk) * N + (j_start + jj)];
                }
            }
            for (size_t ii = 0; ii < i_len; ii++) {
                for (size_t jj = 0; jj < j_len; jj++) {
                    C_cache[ii * TILE_SIZE + jj] = C[(i_start + ii) * N + (j_start + jj)];
                }
            }
            mode = 1;

        } else if (mode == 1) {
            // STAGE 1: CALC - Perform matrix multiplication strictly on the local cache
            for (size_t ii = 0; ii < i_len; ii++) {
                for (size_t jj = 0; jj < j_len; jj++) {
                    double sum = 0;
                    for (size_t kk = 0; kk < k_len; kk++) {
                        sum += A_cache[ii * TILE_SIZE + kk] * B_cache[kk * TILE_SIZE + jj];
                    }
                    // Accumulate the computed dot product sum to C's cache
                    C_cache[ii * TILE_SIZE + jj] += sum;
                }
            }
            mode = 2;

        } else if (mode == 2) {
            // STAGE 2: WRITE - Write the updated partial C tile back out to main memory
            for (size_t ii = 0; ii < i_len; ii++) {
                for (size_t jj = 0; jj < j_len; jj++) {
                    C[(i_start + ii) * N + (j_start + jj)] = C_cache[ii * TILE_SIZE + jj];
                }
            }
            mode = 0;

            // Advance to the next 3D tile combination
            cache_cnt++;

        } else {
            fprintf(stderr, "calc_matmul_share(): Invalid state: %d\n", mode);
            exit(-1);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    // Clean up caches
    free(A_cache);
    free(B_cache);
    free(C_cache);

    elapsed_ns = (end.tv_sec - begin.tv_sec) * 1000000000LL + (end.tv_nsec - begin.tv_nsec);
    printf("Matmul Base Case time: %lld\n", elapsed_ns);
}
/* void calc_matmul_base(struct bench_t *this_bench) { */
/*     size_t N = this_bench->args.obj_cnt0; */
/*     double *A = (double*)this_bench->args.mem0; */
/*     double *B = (double*)this_bench->args.mem1; */
/*     double *C = (double*)this_bench->args.mem2; */
/*     int TILE_SIZE = this_bench->args.config_const0; */

/*     for (size_t i = 0; i < N; i += TILE_SIZE) { */
/*         for (size_t j = 0; j < N; j += TILE_SIZE) { */
/*             for (size_t k = 0; k < N; k += TILE_SIZE) { */
/*                 for (size_t ii = i; ii < i + TILE_SIZE && ii < N; ii++) { */
/*                     for (size_t jj = j; jj < j + TILE_SIZE && jj < N; jj++) { */
/*                         double sum = 0; */
/*                         for (size_t kk = k; kk < k + TILE_SIZE && kk < N; kk++) { */
/*                             sum += A[ii * N + kk] * B[kk * N + jj]; */
/*                         } */
/*                         C[ii * N + jj] += sum; */
/*                     } */
/*                 } */
/*             } */
/*         } */
/*     } */
/* } */


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
