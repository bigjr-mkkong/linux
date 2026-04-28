/*
 * TODO
 * Rewrite following function into benchmark style
 */

#include "bench.h"
#include <stdlib.h>

#define ARRAY_SIZE 1000000 // Tune for cache size

void init_poly_eval(struct bench_t *this_bench, struct bench_arg_t args){
    this_bench->args = args;
    size_t array_bytes = ARRAY_SIZE * sizeof(float);

    this_bench->args.size0 = array_bytes; // X array
    this_bench->args.size1 = array_bytes; // Y array
}

void prepare_poly_eval(struct bench_t *this_bench){
    this_bench->args.mem0 = calloc(1, this_bench->args.size0);
    this_bench->args.mem1 = calloc(1, this_bench->args.size1);

    float *X = (float*)this_bench->args.mem0;
    for(size_t i = 0; i < ARRAY_SIZE; i++){
        X[i] = (float)rand() / RAND_MAX; // Random input values
    }
}

void calc_poly_eval(struct bench_t *this_bench) {
    float *X = (float*)this_bench->args.mem0;
    float *Y = (float*)this_bench->args.mem1;

    // Coefficients for a random 5th degree polynomial
    float c5 = 2.5f, c4 = -1.2f, c3 = 3.4f, c2 = -0.5f, c1 = 1.1f, c0 = 4.0f;

    for (int i = 0; i < ARRAY_SIZE; i++) {
        float x = X[i];
        
        // Horner's method for calculating: c5*x^5 + c4*x^4 + c3*x^3 + c2*x^2 + c1*x + c0
        // This creates a tight dependency chain of Multiply-Accumulate (MAC) operations
        float result = ((((c5 * x + c4) * x + c3) * x + c2) * x + c1) * x + c0;
        
        Y[i] = result;
    }
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
   .calc = calc_poly_eval,
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
