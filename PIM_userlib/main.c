#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "pim_runtime.h"
#include "bench.h"
#include <unistd.h>
#include "time.h"

#define BEGIN_BENCH
#define MINI_BENCH
#define KMEAN_BENCH
#define MATMUL_BENCH

extern struct bench_t kmean_bench;
extern struct bench_t matmul_bench;

void run_bench(struct bench_t *this_bench, struct bench_arg_t args){
    this_bench->init(this_bench, args);
    this_bench->prepare(this_bench);
    this_bench->calc(this_bench);
    this_bench->clean(this_bench);

    return;
}

/*
 * Simulates two independent user tasks each owning a disjoint set of PIM
 * cores and submitting commands concurrently. The library batches their
 * requests and delivers a single ioctl to the kernel when either the
 * watermark or the flush timeout fires.
 */

struct task_args {
    int task_id;
    int core_ids[4];
};

static void *user_task(void *arg)
{
    struct task_args *a = arg;

    pim_user_t *user = pim_user_create();
    if (!user) { perror("pim_user_create"); return NULL; }

    if (pim_alloc_cores(user, a->core_ids, 4) < 0) {
        perror("pim_alloc_cores");
        pim_user_destroy(user);
        return NULL;
    }
    printf("Task %d: allocated cores %d %d %d %d\n",
           a->task_id,
           a->core_ids[0], a->core_ids[1], a->core_ids[2], a->core_ids[3]);

    /* Build a cmd_list targeting only owned cores */
    char cmd_list[MAX_PIM_UNIT];
    memset(cmd_list, PIM_NOP, sizeof(cmd_list));
    for (int i = 0; i < 4; i++)
        cmd_list[a->core_ids[i]] = PIM_START;

    pim_req_handle_t *req = pim_submit(user, cmd_list);
    if (!req) { perror("pim_submit"); goto done; }

    printf("Task %d: submitted PIM_START, waiting...\n", a->task_id);

    /* Block until the kernel acks (up to 2 seconds) */
    if (pim_wait(req, 2000) < 0) {
        perror("pim_wait");
    } else {
        printf("Task %d: PIM_START completed\n", a->task_id);
    }

    pim_req_free(req);

#if defined(BEGIN_BENCH)

    void* begin = get_lib_base();
    struct rw_ret ret0;
    int avail_cores[16] = {0}, avail_ptr = 0;
    for(int i=0; i<MAX_PIM_UNIT; i++){
        if(user->owns_core[i]){
            avail_cores[avail_ptr] = i;
            avail_ptr++;
        }
    }

    /*actual workload begin*/
    /* Here is mem-intensive task */

#if defined(MINI_BENCH)
    for(int i=0 ;i < avail_ptr; i++){
        pause_core(user, avail_cores[i]);
    }

    for(size_t i=0; i<16; i++) {
        ret0 = read_64(user, begin + avail_cores[0] * (1<<12) + sizeof(uint64_t) * i);
        if(ret0.state != SUCC){
            fprintf(stderr, "Failed to read from core %d offset %ld\n", avail_cores[0], i);
            goto done;
        }
    }

    for(int i=0; i<avail_ptr; i++){
        resume_core(user, avail_cores[i]);
    }


    /* Here is CPU intensive work */
    volatile double workhorse = 1.0001;
    for(int i=0; i<16; i++){
        workhorse *= 1.14514;
        workhorse += 1.1919810;
    }
#endif

#if defined(KMEAN_BENCH)

#define NUM_POINTS 500000
#define K_CLUSTERS 32

    run_bench(&kmean_bench, (struct bench_arg_t){\
            .obj_cnt0 = NUM_POINTS,\
            .obj_cnt1 = K_CLUSTERS,\
            .obj_cnt2 = NUM_POINTS\
            });
#endif

#if defined(MATMUL_BENCH)

#define MAT_N 1024
#define TILE_SIZE 32

    run_bench(&matmul_bench, (struct bench_arg_t){
            .obj_cnt0 = MAT_N,
            .obj_cnt1 = 0, //Not in use
            .obj_cnt2 = 0,// Not in use
            .config_const0 = TILE_SIZE
            });
#endif

#if defined(POLY_EVAL_BENCH)

#define ARRAY_SIZE 1000000 // Tune for cache size

    run_bench(&poly_eval_bench, (struct bench_arg_t){
            .obj_cnt0 = ARRAY_SIZE, // Number of terms in the polynomial
            .obj_cnt1 = 0, // Not in use
            .obj_cnt2 = 0, // Not in use
            .config_const0 = 3.14159 // Point of evaluation
            });
#endif

#endif

done:
    pim_free_cores(user);
    pim_user_destroy(user);
    return NULL;
}

int main(void)
{
    srand(time(NULL));
    /* Use a low watermark so the two-user demo flushes quickly */
    if (pim_lib_init((1 << 12)) < 0) {
        perror("pim_lib_init");
        return EXIT_FAILURE;
    }

    //Lets do single threaded test for class project due tomorrow reason :(
    struct task_args a = { .task_id = 0, .core_ids = {0, 1, 2, 3} };
    /* struct task_args b = { .task_id = 1, .core_ids = {4, 5, 6, 7} }; */

    pthread_t ta, tb;
    pthread_create(&ta, NULL, user_task, &a);
    /* pthread_create(&tb, NULL, user_task, &b); */

    pthread_join(ta, NULL);
    /* pthread_join(tb, NULL); */

    pim_lib_fini();
    return EXIT_SUCCESS;
}
