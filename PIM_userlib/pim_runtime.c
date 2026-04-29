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
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <semaphore.h>

#define DEVICE_PATH  "/dev/PIM_controller"
#define IOCTL_MAGIC  114514

// Global library state (singleton)

static struct {
    int dev_fd;
    int epoll_fd;
    atomic_int running;

    pthread_mutex_t    lock;
    sem_t              submit_sem;  /* Triggers the flush logic immediately */

    struct pending_batch  pending;
    struct batch_tracker *inflight;

    int next_user_id;
    int core_owner[MAX_PIM_UNIT];
    pim_user_t *core_user[MAX_PIM_UNIT]; /* Track owner explicitly for bg_check_loop */

    pthread_t  bg_submitter_tid;
    pthread_t  bg_completer_tid;
    pthread_t  bg_check_tid;

    void  *mem_base;
    size_t chunk_size, total_pool;
    int    num_chunks;
    bool  *chunk_free;

    int pfn2core_id[MAX_PIM_UNIT];
    struct pim_timing timing[2];
    atomic_int timing_idx;

    long int pause_penalty;
    long int resume_watermark;

} lib;

static void  flush_pending(void);
static void *submitter_loop(void *arg);
static void *completer_loop(void *arg);
static void *bg_check_loop(void *arg);

static inline bool is_active_pimcmd(char pimcmd){
    switch (pimcmd){
        case PIM_START:
        case MEM_PAUSE:
        case MEM_RESUME:
        case PIM_QUERY:
            return true;
        default:
            return false;
    }
}

int pim_lib_init(size_t chunk_size) {
    memset(&lib, 0, sizeof(lib));
    memset(lib.core_owner, -1, sizeof(lib.core_owner));
    memset(lib.pfn2core_id, -1, sizeof(lib.pfn2core_id));
    memset(lib.core_user, 0, sizeof(lib.core_user));

    memset(&lib.timing[0], 0, sizeof(lib.timing[0]));
    memset(&lib.timing[1], 0, sizeof(lib.timing[1]));
    atomic_store(&lib.timing_idx, 0);
    lib.next_user_id = 0;

    long page_size = sysconf(_SC_PAGE_SIZE);
    if (page_size <= 0 || chunk_size == 0 || (chunk_size % (size_t)page_size != 0)) {
        fprintf(stderr, "pagesz wrong\n");
        errno = EINVAL;
        return -1;
    }

    lib.total_pool = (size_t)PIM_MEM_MAX_PAGES * (size_t)page_size;
    int num_chunks = (int)(lib.total_pool / chunk_size);
    if (num_chunks == 0) {
        fprintf(stderr, "num_chunks wrong\n");
        errno = EINVAL;
        return -1;
    }

    lib.dev_fd = open(DEVICE_PATH, O_RDWR);
    if (lib.dev_fd < 0) return -1;

    lib.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (lib.epoll_fd < 0) goto err_dev;

    lib.chunk_free = malloc((size_t)num_chunks * sizeof(bool));
    if (!lib.chunk_free) goto err_epoll;
    for (int i = 0; i < num_chunks; i++)
        lib.chunk_free[i] = true;

    lib.mem_base = mmap(NULL, lib.total_pool, PROT_READ | PROT_WRITE, MAP_SHARED, lib.dev_fd, 0);
    if (lib.mem_base == MAP_FAILED) goto err_chunk_free;

    lib.chunk_size = chunk_size;
    lib.num_chunks = num_chunks;

    pthread_mutex_init(&lib.lock, NULL);
    sem_init(&lib.submit_sem, 0, 0);
    atomic_store(&lib.running, 1);

    if (pthread_create(&lib.bg_check_tid, NULL, bg_check_loop, NULL) != 0) {
        atomic_store(&lib.running, 0);
        goto err_mmap;
    }
    if (pthread_create(&lib.bg_submitter_tid, NULL, submitter_loop, NULL) != 0) {
        atomic_store(&lib.running, 0);
        pthread_join(lib.bg_check_tid, NULL);
        goto err_mmap;
    }
    if (pthread_create(&lib.bg_completer_tid, NULL, completer_loop, NULL) != 0) {
        atomic_store(&lib.running, 0);
        sem_post(&lib.submit_sem);
        pthread_join(lib.bg_check_tid, NULL);
        pthread_join(lib.bg_submitter_tid, NULL);
        goto err_mmap;
    }

    lib.resume_watermark = 500;
    lib.pause_penalty = 100;

    return 0;

err_mmap:
    munmap(lib.mem_base, lib.total_pool);
err_chunk_free:
    free(lib.chunk_free);
err_epoll:
    close(lib.epoll_fd);
err_dev:
    close(lib.dev_fd);
    return -1;
}

void *get_lib_base(){
    return lib.mem_base;
}

int pim_lib_fini(void) {
    pthread_mutex_lock(&lib.lock);
    int pending = lib.pending.count;
    pthread_mutex_unlock(&lib.lock);

    if (pending > 0) {
        fprintf(stderr, "pim_lib_fini: %d pending request(s) not yet flushed\n", pending);
        errno = EBUSY;
        return -1;
    }

    atomic_store(&lib.running, 0);
    sem_post(&lib.submit_sem); // Unblock submitter

    pthread_join(lib.bg_submitter_tid, NULL);
    pthread_join(lib.bg_completer_tid, NULL);
    pthread_join(lib.bg_check_tid, NULL);

    munmap(lib.mem_base, lib.chunk_size * (size_t)lib.num_chunks);
    free(lib.chunk_free);

    close(lib.epoll_fd);
    close(lib.dev_fd);
    pthread_mutex_destroy(&lib.lock);
    sem_destroy(&lib.submit_sem);
    return 0;
}

pim_user_t *pim_user_create(void) {
    pim_user_t *u = calloc(1, sizeof(*u));
    if (!u) return NULL;

    memset(u->core2chunk, -1, sizeof(u->core2chunk));

    pthread_mutex_lock(&lib.lock);
    u->user_id = lib.next_user_id++;
    pthread_mutex_unlock(&lib.lock);
    return u;
}

void pim_user_destroy(pim_user_t *user) {
    if (!user) return;
    pim_free_cores(user);
    free(user);
}

int pim_alloc_cores(pim_user_t *user, int *allocated_core_ids, int count) {
    if (!user || !allocated_core_ids || count <= 0) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&lib.lock);
    int avail_cores = 0, avail_chunks = 0;

    for (int i = 0; i < MAX_PIM_UNIT; i++)
        if (lib.core_owner[i] == -1) avail_cores++;

    for (int i = 0; i < lib.num_chunks; i++)
        if (lib.chunk_free[i]) avail_chunks++;

    if (avail_cores < count || avail_chunks < count) {
        pthread_mutex_unlock(&lib.lock);
        errno = ENOSPC;
        return -1;
    }

    int found = 0;
    for (int i = 0; i < MAX_PIM_UNIT && found < count; i++) {
        if (lib.core_owner[i] != -1) continue;

        int chunk_idx = -1;
        for (int j = 0; j < lib.num_chunks; j++) {
            if (lib.chunk_free[j]) {
                chunk_idx = j;
                lib.chunk_free[j] = false;
                break;
            }
        }

        lib.core_owner[i]          = user->user_id;
        lib.core_user[i]           = user;  // Store user context for background loop
        user->owns_core[i]         = true;
        user->core2chunk[i]        = chunk_idx;
        user->core_mode[i]         = false;
        allocated_core_ids[found++] = i;

        lib.pfn2core_id[chunk_idx] = i;

        struct timeval ts = {0};
        gettimeofday(&ts, NULL);
        lib.timing[0].last_pause[i] = ts;
        lib.timing[1].last_pause[i] = ts;
        lib.timing[0].is_penalty[i] = false;
        lib.timing[1].is_penalty[i] = false;
    }

    pthread_mutex_unlock(&lib.lock);
    return 0;
}

void pim_free_cores(pim_user_t *user) {
    if (!user) return;
    pthread_mutex_lock(&lib.lock);
    for (int i = 0; i < MAX_PIM_UNIT; i++) {
        if (!user->owns_core[i]) continue;

        lib.pfn2core_id[user->core2chunk[i]] = -1;
        lib.core_owner[i]                    = -1;
        lib.core_user[i]                     = NULL;
        lib.chunk_free[user->core2chunk[i]]  = true;

        user->owns_core[i]                   = false;
        user->core2chunk[i]                  = -1;
        user->core_mode[i]                   = false;
    }
    pthread_mutex_unlock(&lib.lock);
}

pim_req_handle_t *pim_submit(pim_user_t *user, const char cmd_list[MAX_PIM_UNIT]) {
    if (!cmd_list) {
        errno = EINVAL;
        return NULL;
    }

    pim_req_handle_t *req = calloc(1, sizeof(*req));
    if (!req) return NULL;
    atomic_init(&req->done, 0);
    sem_init(&req->sem, 0, 0);
    req->next = NULL;

    pthread_mutex_lock(&lib.lock);

    /* Validate only the cores this submission is actually trying to command */
    for (int i = 0; i < MAX_PIM_UNIT; i++) {
        if (cmd_list[i] != PIM_NOP) {
            /* Check Ownership */
            if (user->user_id != -1 && !user->owns_core[i]) {
                pthread_mutex_unlock(&lib.lock);
                free(req);
                errno = EACCES;
                return NULL;
            }
            /* Check if this specific core already has a pending command in the current batch */
            if (is_active_pimcmd(lib.pending.cmd_list[i])) {
                pthread_mutex_unlock(&lib.lock);
                free(req);
                errno = EBUSY;
                return NULL;
            }
        }
    }

    for (int i = 0; i < MAX_PIM_UNIT; i++) {
        if (cmd_list[i] != PIM_NOP) {
            lib.pending.cmd_list[i] = cmd_list[i];

            /* Update local state tracker */
            if (cmd_list[i] == PIM_START || cmd_list[i] == MEM_RESUME)
                user->core_mode[i] = true;
            else if (cmd_list[i] == MEM_PAUSE)
                user->core_mode[i] = false;
        }
    }

    if (lib.pending.tail)
        lib.pending.tail->next = req;
    else
        lib.pending.head = req;
    lib.pending.tail = req;
    lib.pending.count++;

    pthread_mutex_unlock(&lib.lock);

    /* Instantly wake up the submission thread */
    sem_post(&lib.submit_sem);
    return req;
}

int pim_poll(pim_req_handle_t *req) {
    if (!req) return -1;
    return atomic_load(&req->done) ? 1 : 0;
}

int pim_wait(pim_req_handle_t *req, int timeout_ms) {
    if (!req) {
        errno = EINVAL;
        return -1;
    }

    /* Fast path: if the background loop already finished it, return immediately */
    if (atomic_load(&req->done)) {
        return 0;
    }

    if (timeout_ms < 0) {
        /* Infinite Wait: block until semaphore is posted */
        while (sem_wait(&req->sem) == -1) {
            /* If interrupted by a signal, resume waiting */
            if (errno != EINTR) {
                return -1;
            }
        }
        return 0;
    } else {
        /* Timed Wait: calculate absolute timeout for sem_timedwait */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);

        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;

        /* Handle nanosecond overflow */
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        while (sem_timedwait(&req->sem, &ts) == -1) {
            if (errno == ETIMEDOUT) {
                return -1;
            }
            if (errno != EINTR) {
                return -1; /* Other critical error */
            }
        }
        return 0;
    }
}

void pim_req_free(pim_req_handle_t *req) {
    if (req) {
        sem_destroy(&req->sem);  // <--- Destroy semaphore before free
        free(req);
    }
}

void pause_core(pim_user_t *user, int core_id) {
    char cmd_list[MAX_PIM_UNIT];
    memset(cmd_list, PIM_NOP, sizeof(cmd_list));
    cmd_list[core_id] = MEM_PAUSE;

    pim_req_handle_t *req = pim_submit(user, cmd_list);
    if (!req) {
        fprintf(stderr, "pause_core: failed to submit pause for core %d\n", core_id);
        return;
    }

    if (pim_wait(req, -1) != 0) {
        fprintf(stderr, "pause_core: failed to wait for pause completion on core %d\n", core_id);
    }
    pim_req_free(req);
}

void resume_core(pim_user_t *user, int core_id) {
    char cmd_list[MAX_PIM_UNIT];
    memset(cmd_list, PIM_NOP, sizeof(cmd_list));
    cmd_list[core_id] = MEM_RESUME;

    pim_req_handle_t *req = pim_submit(user, cmd_list);
    if (!req) {
        fprintf(stderr, "resume_core: failed to submit resume for core %d\n", core_id);
        return;
    }

    if (pim_wait(req, -1) != 0) {
        fprintf(stderr, "resume_core: failed to wait for resume completion on core %d\n", core_id);
    }
    pim_req_free(req);
}


static inline enum rw_state_t check(int core_id) {
    int idx = atomic_load(&lib.timing_idx);
    struct pim_timing *t = &lib.timing[idx];

    struct timeval now;
    gettimeofday(&now, NULL);
    long time_since_pause  = (now.tv_sec - t->last_pause[core_id].tv_sec)  * 1000L
                      + (now.tv_usec - t->last_pause[core_id].tv_usec) / 1000L;
    long time_since_resume = (now.tv_sec - t->last_resume[core_id].tv_sec) * 1000L
                      + (now.tv_usec - t->last_resume[core_id].tv_usec) / 1000L;

    if (t->is_penalty[core_id] && time_since_resume < lib.pause_penalty)   return FAIL;
    if (!t->is_penalty[core_id] && time_since_pause > lib.resume_watermark) return FAIL;

    return SUCC;
}

struct rw_ret read_64(struct pim_user *user, void *addr) {
    int pfn = ((uint64_t)addr - (uint64_t)lib.mem_base) / lib.chunk_size;
    int core_id = lib.pfn2core_id[pfn];

    if (check(core_id) == FAIL || !user->owns_core[core_id]) {
        return (struct rw_ret){ .state = FAIL, .data = 0};
    }
    uint64_t val = *(uint64_t*)addr;
    return (struct rw_ret){ .state = SUCC, .data = val };
}

struct rw_ret write_64(struct pim_user *user, void *addr, uint64_t val) {
    int pfn     = ((uint64_t)addr - (uint64_t)lib.mem_base) / lib.chunk_size;
    int core_id = lib.pfn2core_id[pfn];

    if (check(core_id) == FAIL || !user->owns_core[core_id])
        return (struct rw_ret){ .state = FAIL, .data = 0 };

    *(volatile uint64_t *)addr = val;
    return (struct rw_ret){ .state = SUCC, .data = 0 };
}


static void flush_pending(void) {
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

    pthread_mutex_lock(&lib.lock);

    if (lib.pending.count == 0) {
        pthread_mutex_unlock(&lib.lock);
        close(efd);
        free(bt);
        return;
    }

    bt->efd      = efd;
    bt->req_head = lib.pending.head;
    bt->next     = lib.inflight;
    lib.inflight = bt;

    struct epoll_event ev = {
        .events   = EPOLLIN,
        .data.ptr = bt,
    };
    epoll_ctl(lib.epoll_fd, EPOLL_CTL_ADD, efd, &ev);

    struct pim_req_t kreq;
    kreq.event_fd = efd;
    memcpy(kreq.req_list, lib.pending.cmd_list, MAX_PIM_UNIT);
    if (ioctl(lib.dev_fd, IOCTL_MAGIC, &kreq) < 0){
        epoll_ctl(lib.epoll_fd, EPOLL_CTL_DEL, efd, &ev);
        pthread_mutex_unlock(&lib.lock);
        free(bt);
        perror("pim flush: ioctl");
        return;
    }

    char snapshot[MAX_PIM_UNIT];
    memcpy(snapshot, lib.pending.cmd_list, MAX_PIM_UNIT);
    memset(lib.pending.cmd_list, PIM_NOP, sizeof(lib.pending.cmd_list));
    lib.pending.count = 0;
    lib.pending.head  = NULL;
    lib.pending.tail  = NULL;

    pthread_mutex_unlock(&lib.lock);

    int old_idx = atomic_load(&lib.timing_idx);
    int new_idx = 1 - old_idx;
    lib.timing[new_idx] = lib.timing[old_idx];

    struct timeval now;
    gettimeofday(&now, NULL);
    for (int i = 0; i < MAX_PIM_UNIT; i++) {
        if (snapshot[i] == MEM_PAUSE) {
            lib.timing[new_idx].last_pause[i]  = now;
            lib.timing[new_idx].is_penalty[i]  = false;
        } else if (snapshot[i] == MEM_RESUME) {
            lib.timing[new_idx].last_resume[i] = now;
            lib.timing[new_idx].is_penalty[i]  = true;
        }
    }

    pthread_mutex_lock(&lib.lock);
    atomic_store(&lib.timing_idx, new_idx);
    pthread_mutex_unlock(&lib.lock);
}

/* Background Check Watchdog */
static void *bg_check_loop(void *arg) {
    while (atomic_load(&lib.running)) {
        for (int i = 0; i < MAX_PIM_UNIT; i++) {
            if (lib.pfn2core_id[i] == -1) continue;

            pim_user_t *u = lib.core_user[i];
            if (!u) continue;

            /* ONLY force resume if the host currently has it paused */
            if (!u->core_mode[i]) {
                int idx = atomic_load(&lib.timing_idx);
                if (check(i) == FAIL && !lib.timing[idx].is_penalty[i]) {
                    printf("Watchdog awakend\n");
                    resume_core(u, i);
                }
            }
        }
        usleep(500);
    }
    return NULL;
}

/* Thread 1: Wait for Semaphore and Flush Requests to Driver */
static void *submitter_loop(void *arg) {
    while (atomic_load(&lib.running)) {
        sem_wait(&lib.submit_sem);
        if (!atomic_load(&lib.running)) break;
        flush_pending();
    }
    return NULL;
}

/* Thread 2: Poll Kernel Execution and Complete Requests */
static void *completer_loop(void *arg) {
    struct epoll_event events[64];

    while (atomic_load(&lib.running)) {
        // Poll with a 10ms timeout allowing smooth shutdown evaluation
        int n = epoll_wait(lib.epoll_fd, events, 64, 10);

        for (int i = 0; i < n; i++) {
            struct batch_tracker *bt = events[i].data.ptr;

            uint64_t val;
            if (read(bt->efd, &val, sizeof(val)) > 0) {
                struct pim_req_handle *r = bt->req_head;
                while (r) {
                    atomic_store(&r->done, 1);
                    sem_post(&r->sem);
                    r = r->next;
                }
            }

            epoll_ctl(lib.epoll_fd, EPOLL_CTL_DEL, bt->efd, NULL);
            close(bt->efd);

            pthread_mutex_lock(&lib.lock);
            struct batch_tracker **pp = &lib.inflight;
            while (*pp && *pp != bt)
                pp = &(*pp)->next;
            if (*pp)
                *pp = bt->next;
            pthread_mutex_unlock(&lib.lock);

            free(bt);
        }
    }
    return NULL;
}
