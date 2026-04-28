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

void calc_matmul(struct bench_t *this_bench) {
    size_t N = this_bench->args.obj_cnt0;
    double *A = (double*)this_bench->args.mem0;
    double *B = (double*)this_bench->args.mem1;
    double *C = (double*)this_bench->args.mem2;
    int TILE_SIZE = this_bench->args.config_const0;

    for (size_t i = 0; i < N; i += TILE_SIZE) {
        for (size_t j = 0; j < N; j += TILE_SIZE) {
            for (size_t k = 0; k < N; k += TILE_SIZE) {
                for (size_t ii = i; ii < i + TILE_SIZE && ii < N; ii++) {
                    for (size_t jj = j; jj < j + TILE_SIZE && jj < N; jj++) {
                        double sum = 0;
                        for (size_t kk = k; kk < k + TILE_SIZE && kk < N; kk++) {
                            sum += A[ii * N + kk] * B[kk * N + jj];
                        }
                        C[ii * N + jj] += sum;
                    }
                }
            }
        }
    }
}

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
   .calc = calc_matmul,
   .clean = clean_matmul
};
