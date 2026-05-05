#include "bench.h"
#include "stdlib.h"
#include <complex.h>
#include "stdio.h"

#define POINT_BATCH 16

typedef struct {
    float x, y, z;
} Point3D;

/* 
 * Calculate memory size based on obj_cnt
 */
void init_kmean(struct bench_t *this_bench, struct bench_arg_t args){
    this_bench->args = args;
    this_bench->args.size0 = sizeof(Point3D) * this_bench->args.obj_cnt0;
    this_bench->args.size1 = sizeof(Point3D) * this_bench->args.obj_cnt1;
    this_bench->args.size2 = sizeof(int) * this_bench->args.obj_cnt2;
}


/*
 * Allocate memory and generate random number for bench
 */
int prepare_kmean(struct bench_t *this_bench){
    size_t mem0_sz = this_bench->args.size0;
    size_t mem1_sz = this_bench->args.size1;
    size_t mem2_sz = this_bench->args.size2;
    this_bench->args.mem0 = calloc(1, mem0_sz);
    this_bench->args.mem1 = calloc(1, mem1_sz);
    this_bench->args.mem2 = calloc(1, mem2_sz);

    Point3D *obj0_arr = (Point3D*)this_bench->args.mem0;
    Point3D *obj1_arr = (Point3D*)this_bench->args.mem1;
    // Generate random points
    for(size_t i=0; i<this_bench->args.obj_cnt0; i++){
        obj0_arr[i] = (Point3D){
            .x = (float)rand() / RAND_MAX,
            .y = (float)rand() / RAND_MAX,
            .z = (float)rand() / RAND_MAX
        };
    }

    // Generate random initial centroids
    for(size_t i=0; i<this_bench->args.obj_cnt1; i++){
        obj1_arr[i] = (Point3D){
            .x = (float)rand() / RAND_MAX,
            .y = (float)rand() / RAND_MAX,
            .z = (float)rand() / RAND_MAX
        };
    }
    return 0;
}

/*
 * Main body for benchmark
 */
void calc_kmeans_base(struct bench_t *this_bench) {
    /* Setup kmean variables */
    Point3D *points = (Point3D*)(this_bench->args.mem0);
    Point3D *centroids = (Point3D*)(this_bench->args.mem1);
    int *labels = (int*)(this_bench->args.mem2);

    int NUM_POINTS = this_bench->args.obj_cnt0;
    int K_CLUSTERS = this_bench->args.obj_cnt1;

    struct timespec begin, end;
    long long elapsed_ns;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    for (int p = 0; p < NUM_POINTS; p++) {
        float min_dist = 1e9;
        int best_cluster = 0;

        for (int c = 0; c < K_CLUSTERS; c++) {
            float dx = points[p].x - centroids[c].x;
            float dy = points[p].y - centroids[c].y;
            float dz = points[p].z - centroids[c].z;

            float dist = (dx * dx) + (dy * dy) + (dz * dz);

            if (dist < min_dist) {
                min_dist = dist;
                best_cluster = c;
            }
        }
        labels[p] = best_cluster;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    elapsed_ns = (end.tv_sec - begin.tv_sec) * 1000000000LL + (end.tv_nsec - begin.tv_nsec);
    printf("kmean Base Case time: %lld\n", elapsed_ns);
}

void calc_kmeans_share(struct bench_t *this_bench) {
    /* Setup kmean variables */
    Point3D *points = (Point3D*)(this_bench->args.mem0);
    Point3D *centroids = (Point3D*)(this_bench->args.mem1);
    int *labels = (int*)(this_bench->args.mem2);

    int NUM_POINTS = this_bench->args.obj_cnt0;
    int K_CLUSTERS = this_bench->args.obj_cnt1;

    // Calculate total number of chunks based on POINT_BATCH
    int total_slots = (NUM_POINTS + POINT_BATCH - 1) / POINT_BATCH;

    // Imaginary caches for the current chunk
    Point3D points_cache[POINT_BATCH];
    int labels_cache[POINT_BATCH];

    struct timespec begin, end;
    long long elapsed_ns;

    int mode = 0; // 0 -> READ, 1 -> CALC, 2 -> WRITE
    int cache_cnt = 0; // Track which chunk we are processing

    clock_gettime(CLOCK_MONOTONIC, &begin);

    while (cache_cnt < total_slots) {
        int elems_to_process = (cache_cnt == total_slots - 1) ? (NUM_POINTS - cache_cnt * POINT_BATCH) : POINT_BATCH;

        if (mode == 0) {
            for(int i = 0; i < elems_to_process; i++){
                points_cache[i] = points[cache_cnt * POINT_BATCH + i];
            }
            mode = 1;

        } else if (mode == 1) {
            for(int i = 0; i < elems_to_process; i++){
                float min_dist = 1e9;
                int best_cluster = 0;

                for (int c = 0; c < K_CLUSTERS; c++) {
                    float dx = points_cache[i].x - centroids[c].x;
                    float dy = points_cache[i].y - centroids[c].y;
                    float dz = points_cache[i].z - centroids[c].z;

                    float dist = (dx * dx) + (dy * dy) + (dz * dz);

                    if (dist < min_dist) {
                        min_dist = dist;
                        best_cluster = c;
                    }
                }
                labels_cache[i] = best_cluster;
            }
            mode = 2;

        } else if (mode == 2) {
            for(int i = 0; i < elems_to_process; i++){
                labels[cache_cnt * POINT_BATCH + i] = labels_cache[i];
            }
            mode = 0;
            cache_cnt++;

        } else {
            fprintf(stderr, "calc_kmeans_share(): Invalid state: %d\n", mode);
            exit(-1);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    elapsed_ns = (end.tv_sec - begin.tv_sec) * 1000000000LL + (end.tv_nsec - begin.tv_nsec);
    printf("kmean Share Case time: %lld\n", elapsed_ns);
}

/* 
 * Clean up for benchmark
 */
void clean_kmean(struct bench_t *this_bench){
    free(this_bench->args.mem0);
    free(this_bench->args.mem1);
    free(this_bench->args.mem2);

    return;
}

struct bench_t kmean_bench \
= {
   .args = (struct bench_arg_t){0},
   .init = init_kmean,
   .prepare = prepare_kmean,
   .calc_share = calc_kmeans_share,
   .calc_base = calc_kmeans_base,
   .clean = clean_kmean
};
