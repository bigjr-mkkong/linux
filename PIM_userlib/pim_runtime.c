#include "pim_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define DEVICE_PATH  "/dev/PIM_controller"
#define IOCTL_MAGIC  114514

/* Mirror of kernel ABI — must stay in sync with PIM_control_cmd.h */
struct pim_req_t {
    int  event_fd;
    char req_list[MAX_PIM_UNIT];
};

/* -------------------------------------------------------------------------
 * Internal structs
 * ---------------------------------------------------------------------- */

/*
 * Returned to the user after pim_submit().
 * The completer thread sets `done` to 1 when the kernel acks the batch
 * this request belongs to.
 */
struct pim_req_handle {
    atomic_int            done;
    struct pim_req_handle *next; /* intrusive list: links all handles in a batch */
};

/*
 * One in-flight kernel submission: one ioctl → one eventfd → N req handles.
 * epoll watches the eventfd; data.ptr points to the batch_tracker so the
 * completer thread can find it without a hashtable lookup.
 */
struct batch_tracker {
    int                   efd;
    struct pim_req_handle *req_head; /* list of all handles waiting on this batch */
    struct batch_tracker  *next;     /* next tracker in lib.inflight list */
};

/*
 * Requests accumulate here until the watermark or flush timeout triggers
 * an actual ioctl(). Multiple users' cmd_lists are OR-merged (safe because
 * ownership enforcement guarantees disjoint cores).
 */
struct pending_batch {
    char                  cmd_list[MAX_PIM_UNIT];
    int                   count;
    struct pim_req_handle *head;
    struct pim_req_handle *tail;
    struct timespec        first_submit_ts;
};

struct pim_user {
    int  user_id;
    bool owns_core[MAX_PIM_UNIT];
};

/* -------------------------------------------------------------------------
 * Global library state (singleton)
 * ---------------------------------------------------------------------- */

static struct {
    int dev_fd;
    int epoll_fd;
    int watermark; // how many requests before auto flush
    int flush_timeout_ms; // max time to wait before auto flush

    pthread_mutex_t    lock;

    struct pending_batch pending; // requests batch not yet sent to kernel
    struct batch_tracker *inflight; // requests batch already sent to kernel

    int next_user_id;
    int core_owner[MAX_PIM_UNIT]; // track which user owns each pim core, -1 is free

    pthread_t   completer_tid; // the background thread that watch epoll
    atomic_int  running;
} lib;

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */
static void  flush_pending_locked(void);
static void *completer_loop(void *arg);

/* -------------------------------------------------------------------------
 * Library lifecycle
 * ---------------------------------------------------------------------- */

int pim_lib_init(int watermark, int flush_timeout_ms)
{
    memset(&lib, 0, sizeof(lib)); // initialize all fields to 0/NULL/false  
    memset(lib.core_owner, -1, sizeof(lib.core_owner)); // initilize all cores as free (owner = -1)

    lib.watermark        = watermark        > 0 ? watermark        : PIM_DEFAULT_WATERMARK;
    lib.flush_timeout_ms = flush_timeout_ms > 0 ? flush_timeout_ms : PIM_DEFAULT_FLUSH_MS;

    lib.dev_fd = open(DEVICE_PATH, O_RDWR);
    if (lib.dev_fd < 0)
        return -1;

    lib.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (lib.epoll_fd < 0)
        goto err_dev;

    pthread_mutex_init(&lib.lock, NULL);
    atomic_store(&lib.running, 1);

    // start the background completer thread, which will monitor the shared epoll instance for completions and also handle timeout-based flushing
    if (pthread_create(&lib.completer_tid, NULL, completer_loop, NULL) != 0)
        goto err_epoll;

    return 0;

err_epoll:
    close(lib.epoll_fd);
    // fall through
err_dev:
    close(lib.dev_fd);
    return -1;
}

void pim_lib_fini(void)
{
    atomic_store(&lib.running, 0);
    pthread_join(lib.completer_tid, NULL);

    pthread_mutex_lock(&lib.lock);
    if (lib.pending.count > 0)
        flush_pending_locked();
    pthread_mutex_unlock(&lib.lock);

    close(lib.epoll_fd);
    close(lib.dev_fd);
    pthread_mutex_destroy(&lib.lock);
}

/* -------------------------------------------------------------------------
 * User management
 * ---------------------------------------------------------------------- */

pim_user_t *pim_user_create(void)
{
    pim_user_t *u = calloc(1, sizeof(*u));
    if (!u) return NULL;

    pthread_mutex_lock(&lib.lock);
    u->user_id = lib.next_user_id++;
    pthread_mutex_unlock(&lib.lock);
    return u;
}

void pim_user_destroy(pim_user_t *user)
{
    if (!user) return;
    pim_free_cores(user);
    free(user);
}

/* -------------------------------------------------------------------------
 * Core allocation
 * ---------------------------------------------------------------------- */

int pim_alloc_cores(pim_user_t *user, int *core_ids, int count)
{
    if (!user || !core_ids || count <= 0) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&lib.lock);

    /* Verify enough free cores exist before committing anything */
    int avail = 0;
    for (int i = 0; i < MAX_PIM_UNIT; i++)
        if (lib.core_owner[i] == -1) avail++;

    if (avail < count) {
        pthread_mutex_unlock(&lib.lock);
        errno = ENOSPC;
        return -1;
    }

    int found = 0;
    for (int i = 0; i < MAX_PIM_UNIT && found < count; i++) {
        if (lib.core_owner[i] == -1) {
            lib.core_owner[i]  = user->user_id;
            user->owns_core[i] = true;
            core_ids[found++]  = i;
        }
    }

    pthread_mutex_unlock(&lib.lock);
    return 0;
}

void pim_free_cores(pim_user_t *user)
{
    if (!user) return;
    pthread_mutex_lock(&lib.lock);
    for (int i = 0; i < MAX_PIM_UNIT; i++) {
        if (user->owns_core[i]) {
            lib.core_owner[i]  = -1;
            user->owns_core[i] = false;
        }
    }
    pthread_mutex_unlock(&lib.lock);
}

/* -------------------------------------------------------------------------
 * Submission (non-blocking)
 * ---------------------------------------------------------------------- */

pim_req_handle_t *pim_submit(pim_user_t *user, const char cmd_list[MAX_PIM_UNIT])
{
    // cmd_list: the caller's commands for each core, e.g. {PIM_START, PIM_NOP, PIM_PAUSE, ...}
    if (!user || !cmd_list) {
        errno = EINVAL;
        return NULL;
    }

    /* Ownership check: user must only target cores it has allocated */
    for (int i = 0; i < MAX_PIM_UNIT; i++) {
        if (cmd_list[i] != PIM_NOP && !user->owns_core[i]) {
            fprintf(stderr, "pim_submit: user %d does not own core %d\n",
                    user->user_id, i);
            errno = EACCES;
            return NULL;
        }
    }

    pim_req_handle_t *req = calloc(1, sizeof(*req));
    if (!req) return NULL;
    atomic_init(&req->done, 0);
    req->next = NULL;

    pthread_mutex_lock(&lib.lock);

    /* OR-merge into the pending batch (ownership guarantees disjoint cores) */
    for (int i = 0; i < MAX_PIM_UNIT; i++) {
        if (cmd_list[i] != PIM_NOP)
            lib.pending.cmd_list[i] = cmd_list[i];
    }

    /* Record timestamp of the first submission in this batch */
    if (lib.pending.count == 0)
        clock_gettime(CLOCK_MONOTONIC, &lib.pending.first_submit_ts);

    /* Append to pending handle list */
    if (lib.pending.tail)
        lib.pending.tail->next = req;
    else
        lib.pending.head = req;
    lib.pending.tail = req;
    lib.pending.count++;

    /* Watermark check: flush immediately if we've hit the threshold */
    if (lib.pending.count >= lib.watermark)
        flush_pending_locked();

    pthread_mutex_unlock(&lib.lock);
    return req;
}

/* -------------------------------------------------------------------------
 * Completion
 * ---------------------------------------------------------------------- */

int pim_poll(pim_req_handle_t *req)
{
    if (!req) return -1;
    return atomic_load(&req->done) ? 1 : 0;
}

int pim_wait(pim_req_handle_t *req, int timeout_ms)
{
    if (!req) {
        errno = EINVAL;
        return -1;
    }

    struct timespec deadline = {0};
    if (timeout_ms >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec  += timeout_ms / 1000;
        deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    while (!atomic_load(&req->done)) {
        if (timeout_ms >= 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
                errno = ETIMEDOUT;
                return -1;
            }
        }
        usleep(100); /* 0.1 ms poll interval */
    }
    return 0;
}

void pim_req_free(pim_req_handle_t *req)
{
    free(req);
}

/* -------------------------------------------------------------------------
 * Internal: flush the pending batch to the kernel (caller must hold lib.lock)
 * ---------------------------------------------------------------------- */

static void flush_pending_locked(void)
{
    if (lib.pending.count == 0)
        return;

    /*
     * Each kernel submission gets its own eventfd. All req handles in this
     * batch share the same efd — when the kernel signals it the completer
     * thread marks all of them done at once.
     */
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) {
        perror("pim flush: eventfd");
        return;
    }

    struct batch_tracker *bt = malloc(sizeof(*bt));
    if (!bt) {
        close(efd);
        return;
    }
    bt->efd      = efd;
    bt->req_head = lib.pending.head;

    /* Prepend to inflight list */
    bt->next     = lib.inflight;
    lib.inflight = bt;

    /*
     * Register efd with the library's shared epoll. Store `bt` in data.ptr
     * so the completer thread can find all associated handles immediately.
     * There is only one epoll_fd for the whole library (as noted in the
     * example comments).
     */
    struct epoll_event ev = {
        .events   = EPOLLIN,
        .data.ptr = bt,
    };
    epoll_ctl(lib.epoll_fd, EPOLL_CTL_ADD, efd, &ev);

    /* Submit to kernel */
    struct pim_req_t kreq;
    kreq.event_fd = efd;
    memcpy(kreq.req_list, lib.pending.cmd_list, MAX_PIM_UNIT);
    if (ioctl(lib.dev_fd, IOCTL_MAGIC, &kreq) < 0)
        perror("pim flush: ioctl");

    /* Reset pending batch for the next round */
    memset(lib.pending.cmd_list, PIM_NOP, sizeof(lib.pending.cmd_list));
    lib.pending.count = 0;
    lib.pending.head  = NULL;
    lib.pending.tail  = NULL;
}

/* -------------------------------------------------------------------------
 * Background completer thread
 *
 * Uses non-blocking epoll_wait (timeout=0) in a meta-loop, as described in
 * the example comments. This avoids blocking the thread indefinitely and
 * allows the timeout-flush logic to run on every iteration.
 * ---------------------------------------------------------------------- */

static void *completer_loop(void *arg)
{
    (void)arg;
    struct epoll_event events[64]; /* up to 64 completions per iteration */

    while (atomic_load(&lib.running)) {

        /* Non-blocking: return immediately with however many are ready */
        int n = epoll_wait(lib.epoll_fd, events, 64, 0);

        for (int i = 0; i < n; i++) {
            struct batch_tracker *bt = events[i].data.ptr;

            /* Drain the eventfd so epoll stops reporting it */
            uint64_t val;
            read(bt->efd, &val, sizeof(val));

            /* Mark every request handle in this batch as complete */
            struct pim_req_handle *r = bt->req_head;
            while (r) {
                atomic_store(&r->done, 1);
                r = r->next;
            }

            /* Remove efd from epoll and close it */
            epoll_ctl(lib.epoll_fd, EPOLL_CTL_DEL, bt->efd, NULL);
            close(bt->efd);

            /* Remove bt from the inflight list and free it */
            pthread_mutex_lock(&lib.lock);
            struct batch_tracker **pp = &lib.inflight;
            while (*pp && *pp != bt)
                pp = &(*pp)->next;
            if (*pp)
                *pp = bt->next;
            pthread_mutex_unlock(&lib.lock);

            free(bt);
        }

        /*
         * Timeout flush: if the pending batch has been sitting longer than
         * flush_timeout_ms, push it to the kernel even if the watermark
         * has not been reached.
         */
        pthread_mutex_lock(&lib.lock);
        if (lib.pending.count > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms =
                (now.tv_sec  - lib.pending.first_submit_ts.tv_sec)  * 1000L +
                (now.tv_nsec - lib.pending.first_submit_ts.tv_nsec) / 1000000L;
            if (elapsed_ms >= lib.flush_timeout_ms)
                flush_pending_locked();
        }
        pthread_mutex_unlock(&lib.lock);

        usleep(500); /* 0.5 ms between iterations — keeps CPU low without
                        adding meaningful latency vs. the flush timeout */
    }

    return NULL;
}
