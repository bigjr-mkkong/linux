#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "pim_runtime.h"

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

done:
    pim_free_cores(user);
    pim_user_destroy(user);
    return NULL;
}

int main(void)
{
    /* Use a low watermark so the two-user demo flushes quickly */
    if (pim_lib_init(2, 50, (1 << 12)) < 0) {
        perror("pim_lib_init");
        return EXIT_FAILURE;
    }

    struct task_args a = { .task_id = 0, .core_ids = {0, 1, 2, 3} };
    struct task_args b = { .task_id = 1, .core_ids = {4, 5, 6, 7} };

    pthread_t ta, tb;
    pthread_create(&ta, NULL, user_task, &a);
    pthread_create(&tb, NULL, user_task, &b);

    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    pim_lib_fini();
    return EXIT_SUCCESS;
}
