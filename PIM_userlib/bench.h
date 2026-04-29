#ifndef __BENCH_H__
#define __BENCH_H__

#include "stdint.h"
#include "unistd.h"
#include "pim_runtime.h"

struct bench_arg_t{
    //set by benchmark
    size_t obj_cnt0, obj_cnt1, obj_cnt2;
    int config_const0;

    //calculated by initialization code
    void *mem0, *mem1, *mem2;
    size_t size0, size1, size2;

    struct pim_user *user;
    int avail_cores[16];
};

struct bench_t{
    struct bench_arg_t args;
    void (*init)(struct bench_t *this_bench, struct bench_arg_t args);
    int (*prepare)(struct bench_t *this_bench);
    void (*calc_base)(struct bench_t *this_bench);
    void (*calc_share)(struct bench_t *this_bench);
    void (*clean)(struct bench_t *this_bench);
};

#endif
