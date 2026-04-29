#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "pim_runtime.h"
#include "bench.h"
#include <unistd.h>
#include "time.h"

#define BEGIN_BENCH
/* #define MINI_BENCH */
/* #define KMEAN_BENCH */
/* #define MATMUL_BENCH */
/* #define POLY_EVAL_BENCH */

enum BenchTyp{
    MINI_BENCH,
    KMEAN_BENCH,
    MATMUL_BENCH,
    POLY_EVAL_BENCH
};

extern struct bench_t kmean_bench;
extern struct bench_t matmul_bench;
extern struct bench_t poly_eval_bench;

void run_base_bench(struct bench_t *this_bench, struct bench_arg_t args){
    this_bench->init(this_bench, args);
    this_bench->prepare(this_bench);
    this_bench->calc_base(this_bench);
    this_bench->clean(this_bench);
    return;
}

void run_share_bench(struct bench_t *this_bench, struct bench_arg_t args){
    this_bench->init(this_bench, args);
    this_bench->prepare(this_bench);
    this_bench->calc_share(this_bench);
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
    enum BenchTyp b_type;
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

    enum BenchTyp btype = ((struct task_args*)arg)->b_type;

    if(btype == MINI_BENCH){
        printf("Running MINI_BENCH\n");
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
    } else if(btype == KMEAN_BENCH){
        printf("Running KMEAN_BENCH\n");
#define NUM_POINTS 1000
#define K_CLUSTERS 32
        struct bench_arg_t kmean_args = {
        .obj_cnt0 = NUM_POINTS,\
                    .obj_cnt1 = K_CLUSTERS,\
                    .obj_cnt2 = NUM_POINTS,\
                    .user = user,\
        };
        memcpy(kmean_args.avail_cores, avail_cores, sizeof(int) * 16);
        run_share_bench(&kmean_bench, kmean_args);
        pim_print_traces();


        // run_bench(&kmean_bench, (struct bench_arg_t){\
        //         .obj_cnt0 = NUM_POINTS,\
        //         .obj_cnt1 = K_CLUSTERS,\
        //         .obj_cnt2 = NUM_POINTS\
        //         });

    } else if(btype == MATMUL_BENCH){
        printf("Running MATMUL_BENCH\n");
#define MAT_N 128
#define TILE_SIZE 64

        struct bench_arg_t matmul_args = {
        .obj_cnt0 = MAT_N,\
                    .obj_cnt1 = 0,\ 
                    .obj_cnt2 = 0,\ 
                    .config_const0 = TILE_SIZE,\
                    .user = user,\
        };
        memcpy(matmul_args.avail_cores, avail_cores, sizeof(int) * 16);
        run_share_bench(&matmul_bench, matmul_args);
        pim_print_traces();


    } else if(btype == POLY_EVAL_BENCH){
        printf("Running POLY_EVAL_BENCH\n");
#define ARRAY_SIZE 10000 // Tune for cache size
        struct bench_arg_t poly_eval_args = {
        .obj_cnt0 = ARRAY_SIZE,\
                    .obj_cnt1 = 0,\ 
                    .obj_cnt2 = 0,\ 
                    .config_const0 = 0, // not in use
                    .user = user,\
        };
        memcpy(poly_eval_args.avail_cores, avail_cores, sizeof(int) * 16);
        run_share_bench(&poly_eval_bench, poly_eval_args);
        pim_print_traces();

    } else {
        fprintf(stderr, "Unrecognizable bench type %d\n", btype);
    }
#endif

done:
    pim_free_cores(user);
    pim_user_destroy(user);
    return NULL;
}

int main()
{
    /* int bench_run_id = atoi(argv[1]); */
    srand(time(NULL));
    /* Use a low watermark so the two-user demo flushes quickly */
    if (pim_lib_init((1 << 12)) < 0) {
        perror("pim_lib_init");
        return EXIT_FAILURE;
    }

    //Lets do single threaded test for class project due tomorrow reason :(
    struct task_args a = { .task_id = 0, .b_type = MINI_BENCH, .core_ids = {0, 1, 2, 3} };
    struct task_args b = { .task_id = 1, .b_type = KMEAN_BENCH, .core_ids = {4, 5, 6, 7} };
    struct task_args c = { .task_id = 2, .b_type = MATMUL_BENCH, .core_ids = {8, 9, 10, 11} };
    struct task_args d = { .task_id = 3, .b_type = POLY_EVAL_BENCH, .core_ids = {12, 13, 14, 15} };

    pthread_t ta, tb, tc, td;

    /* if(bench_run_id == 1){ */
        pthread_create(&ta, NULL, user_task, &a);
        pthread_join(ta, NULL);
    /* } else if(bench_run_id == 2){ */
        pthread_create(&tb, NULL, user_task, &b);
        pthread_join(tb, NULL);
    /* } else if(bench_run_id == 4){ */
        pthread_create(&td, NULL, user_task, &d);
        pthread_join(td, NULL);
    /* } else if(bench_run_id == 3){ */
        pthread_create(&tc, NULL, user_task, &c);
        pthread_join(tc, NULL);
    /* } else { */
        /* fprintf(stderr, "bench run id %d DNE\n", bench_run_id); */
    /* } */

    pim_lib_fini();

    printf("Bench finished\n");
    return EXIT_SUCCESS;
}
