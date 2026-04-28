#include "bench.h"
#include "stdlib.h"

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
void calc_kmeans(struct bench_t *this_bench) {
    /* Setup kmean variables */
    Point3D *points = (Point3D*)(this_bench->args.mem0);
    Point3D *centroids = (Point3D*)(this_bench->args.mem1);
    int *labels = (int*)(this_bench->args.mem2);

    int NUM_POINTS = this_bench->args.obj_cnt0;
    int K_CLUSTERS = this_bench->args.obj_cnt1;

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
   .calc = calc_kmeans,
   .clean = clean_kmean
};
