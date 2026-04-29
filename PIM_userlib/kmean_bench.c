#include "bench.h"
#include "stdlib.h"
#include <complex.h>
#include "stdio.h"

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
    Point3D *points    = (Point3D*)(this_bench->args.mem0);
    Point3D *centroids = (Point3D*)(this_bench->args.mem1);
    int     *labels    = (int*)(this_bench->args.mem2);

    int NUM_POINTS = this_bench->args.obj_cnt0;
    int K_CLUSTERS = this_bench->args.obj_cnt1;

    int core_id_p = this_bench->args.avail_cores[0];
    int core_id_c = this_bench->args.avail_cores[1];
    int core_id_l = this_bench->args.avail_cores[2];

    Point3D p_cache;
    Point3D cent_cache[K_CLUSTERS];
    int label_cache = 0;

    struct timespec begin, end;
    long long elapsed_ns;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    // +2 to drain the last two points through calc and write stages
    for (int slot = 0; slot < NUM_POINTS + 2; slot++) {
        int p_w = slot - 2;  // point being written
        int p_c = slot - 1;  // point being computed
        int p_r = slot;      // point being read

        // Write stage (CPU mode): write label computed two slots ago
        if (p_w >= 0 && p_w < NUM_POINTS) {
            pause_core(this_bench->args.user, core_id_l);
            labels[p_w] = label_cache;
            resume_core(this_bench->args.user, core_id_l);
        }

        // Calc stage (PIM mode): compute from cache loaded last slot
        if (p_c >= 0 && p_c < NUM_POINTS) {
            float min_dist = 1e9;
            int best_cluster = 0;
            for (int c = 0; c < K_CLUSTERS; c++) {
                float dx = p_cache.x - cent_cache[c].x;
                float dy = p_cache.y - cent_cache[c].y;
                float dz = p_cache.z - cent_cache[c].z;
                float dist = dx*dx + dy*dy + dz*dz;
                if (dist < min_dist) {
                    min_dist = dist;
                    best_cluster = c;
                }
            }
            label_cache = best_cluster;
        }

        // Read stage (CPU mode): load next point and all centroids into cache
        if (p_r < NUM_POINTS) {
            pause_core(this_bench->args.user, core_id_p);
            /* pause_core(this_bench->args.user, core_id_c); */
            p_cache = points[p_r];
            for (int j = 0; j < K_CLUSTERS; j++)
                cent_cache[j] = centroids[j];
            resume_core(this_bench->args.user, core_id_p);
            /* resume_core(this_bench->args.user, core_id_c); */
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    elapsed_ns = (end.tv_sec - begin.tv_sec) * 1000000000LL + (end.tv_nsec - begin.tv_nsec);
    printf("kmean shared Case time: %lld\n", elapsed_ns);
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
