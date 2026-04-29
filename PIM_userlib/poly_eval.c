#include "bench.h"
#include "stdio.h"
#include <stdlib.h>
#include "pim_runtime.h"

#define CHUNK_SIZE (4*1024) // 4KB chunk size
#define CACHE_ELEMS (CHUNK_SIZE / 4) // floats per chunk

void init_poly_eval(struct bench_t *this_bench, struct bench_arg_t args){
    this_bench->args = args;
    size_t array_bytes = this_bench->args.obj_cnt0 * sizeof(float);

    this_bench->args.size0 = array_bytes; // X array
    this_bench->args.size1 = array_bytes; // Y array
}

int prepare_poly_eval(struct bench_t *this_bench){
    this_bench->args.mem0 = calloc(1, this_bench->args.size0);
    this_bench->args.mem1 = calloc(1, this_bench->args.size1);

    float *X = (float*)this_bench->args.mem0;
    for(size_t i = 0; i < this_bench->args.obj_cnt0; i++){
        X[i] = (float)rand() / RAND_MAX; // Random input values
    }
    return 0;
}


void calc_poly_eval_base(struct bench_t *this_bench) {
    float *X = (float*)this_bench->args.mem0;
    float *Y = (float*)this_bench->args.mem1;
    int N = (int)this_bench->args.obj_cnt0;

    float c5 = 2.5f, c4 = -1.2f, c3 = 3.4f, c2 = -0.5f, c1 = 1.1f, c0 = 4.0f;
    int core_id_x = this_bench->args.avail_cores[0];
    int core_id_y = this_bench->args.avail_cores[1];

    int total_slots = (N + CACHE_ELEMS - 1) / CACHE_ELEMS;
    float X_cache[CACHE_ELEMS];
    float result_cache[CACHE_ELEMS];

    struct timespec begin, end;
    long long elapsed_ns;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    // +2 to drain last two chunks through calc and write stages
    for (int slot = 0; slot < total_slots + 2; slot++) {
        int s_w = slot - 2;
        int s_c = slot - 1;
        int s_r = slot;

        // Write stage (CPU mode): write result_cache to Y
        if (s_w >= 0 && s_w < total_slots) {
            int base_w = s_w * CACHE_ELEMS;
            int count_w = (base_w + CACHE_ELEMS <= N) ? CACHE_ELEMS : N - base_w;
            for (int j = 0; j < count_w; j++)
                Y[base_w + j] = result_cache[j];
        }

        // Calc stage (PIM mode): compute from X_cache into result_cache
        if (s_c >= 0 && s_c < total_slots) {
            int base_c = s_c * CACHE_ELEMS;
            int count_c = (base_c + CACHE_ELEMS <= N) ? CACHE_ELEMS : N - base_c;
            for (int j = 0; j < count_c; j++) {
                float x = X_cache[j];
                result_cache[j] = ((((c5 * x + c4) * x + c3) * x + c2) * x + c1) * x + c0;
            }
        }

        // Read stage (CPU mode): load CACHE_ELEMS X values into X_cache
        if (s_r < total_slots) {
            int base_r = s_r * CACHE_ELEMS;
            int count_r = (base_r + CACHE_ELEMS <= N) ? CACHE_ELEMS : N - base_r;
            for (int j = 0; j < count_r; j++)
                X_cache[j] = X[base_r + j];
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    elapsed_ns = (end.tv_sec - begin.tv_sec) * 1000000000LL + (end.tv_nsec - begin.tv_nsec);
    printf("Poly eval Base Case time: %lld\n", elapsed_ns);
}

void calc_poly_eval_share(struct bench_t *this_bench) {
    float *X = (float*)this_bench->args.mem0;
    float *Y = (float*)this_bench->args.mem1;
    int N = (int)this_bench->args.obj_cnt0;

    float c5 = 2.5f, c4 = -1.2f, c3 = 3.4f, c2 = -0.5f, c1 = 1.1f, c0 = 4.0f;
    int core_id_x = this_bench->args.avail_cores[0];
    int core_id_y = this_bench->args.avail_cores[1];

    int total_slots = (N + CACHE_ELEMS - 1) / CACHE_ELEMS;
    float X_cache[CACHE_ELEMS];
    float result_cache[CACHE_ELEMS];

    // +2 to drain last two chunks through calc and write stages
    struct timespec begin, end;
    long long elapsed_ns;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (int slot = 0; slot < total_slots + 2; slot++) {
        int s_w = slot - 2;
        int s_c = slot - 1;
        int s_r = slot;

        // Write stage (CPU mode): write result_cache to Y
        if (s_w >= 0 && s_w < total_slots) {
            int base_w = s_w * CACHE_ELEMS;
            int count_w = (base_w + CACHE_ELEMS <= N) ? CACHE_ELEMS : N - base_w;
            pause_core(this_bench->args.user, core_id_y);
            for (int j = 0; j < count_w; j++)
                Y[base_w + j] = result_cache[j];
            resume_core(this_bench->args.user, core_id_y);
        }

        // Calc stage (PIM mode): compute from X_cache into result_cache
        if (s_c >= 0 && s_c < total_slots) {
            int base_c = s_c * CACHE_ELEMS;
            int count_c = (base_c + CACHE_ELEMS <= N) ? CACHE_ELEMS : N - base_c;
            for (int j = 0; j < count_c; j++) {
                float x = X_cache[j];
                result_cache[j] = ((((c5 * x + c4) * x + c3) * x + c2) * x + c1) * x + c0;
            }
        }

        // Read stage (CPU mode): load CACHE_ELEMS X values into X_cache
        if (s_r < total_slots) {
            int base_r = s_r * CACHE_ELEMS;
            int count_r = (base_r + CACHE_ELEMS <= N) ? CACHE_ELEMS : N - base_r;
            pause_core(this_bench->args.user, core_id_x);
            for (int j = 0; j < count_r; j++)
                X_cache[j] = X[base_r + j];
            resume_core(this_bench->args.user, core_id_x);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    elapsed_ns = (end.tv_sec - begin.tv_sec) * 1000000000LL + (end.tv_nsec - begin.tv_nsec);
    printf("Poly eval shared Case time: %lld\n", elapsed_ns);
}

void clean_poly_eval(struct bench_t *this_bench){
    free(this_bench->args.mem0);
    free(this_bench->args.mem1);
}

struct bench_t poly_eval_bench\
= {
   .args = (struct bench_arg_t){0},
   .init = init_poly_eval,
   .prepare = prepare_poly_eval,
   .calc_base = calc_poly_eval_base,
   .calc_share = calc_poly_eval_share,
   .clean = clean_poly_eval
};

// void poly_evaluate(float X[ARRAY_SIZE], float Y[ARRAY_SIZE]) {
//     // Coefficients for a random 5th degree polynomial
//     float c5 = 2.5f, c4 = -1.2f, c3 = 3.4f, c2 = -0.5f, c1 = 1.1f, c0 = 4.0f;

//     for (int i = 0; i < ARRAY_SIZE; i++) {
//         float x = X[i];
        
//         // Horner's method for calculating: c5*x^5 + c4*x^4 + c3*x^3 + c2*x^2 + c1*x + c0
//         // This creates a tight dependency chain of Multiply-Accumulate (MAC) operations
//         float result = ((((c5 * x + c4) * x + c3) * x + c2) * x + c1) * x + c0;
        
//         Y[i] = result;
//     }
// }
