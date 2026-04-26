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


#define DEVICE_PATH  "/dev/PIM_controller"
#define IOCTL_MAGIC  114514

/* Mirror of kernel ABI — must stay in sync with PIM_control_cmd.h */
struct pim_req_t {
    int  event_fd;
    char req_list[MAX_PIM_UNIT];
};

// Internal structs

struct pim_req_handle {
    atomic_int            done; /* set to 1 by completer_loop when kernel signals completion */
    struct pim_req_handle *next; /* links handles within the same batch */
};

struct batch_tracker {
    int                   efd;      /* eventfd the kernel writes to on completion */
    struct pim_req_handle *req_head; /* first handle in the batch, used to mark all done */
    struct batch_tracker  *next;     /* next in lib.inflight linked list */
};

struct pending_batch {
    char                  cmd_list[MAX_PIM_UNIT]; /* one command slot per core, PIM_NOP if unused */
    int                   count;                  /* number of submissions accumulated so far */
    struct pim_req_handle *head;                  /* first handle in this batch */
    struct pim_req_handle *tail;                  /* last handle, for O(1) append */
    struct timeval         first_submit_ts;        /* timestamp of first submission, for timeout flush */
};


 
struct pim_user {
    int  user_id;
    bool owns_core[MAX_PIM_UNIT];

    int   core2chunk[MAX_PIM_UNIT];     /* -1 when core not owned */
    bool  core_mode[MAX_PIM_UNIT];  /* false = HOST_OWNED */ // need to verify
};

 // Global library state (singleton)

static struct {
    int dev_fd;           
    int epoll_fd;         
    int watermark;       /* flush when pending count reaches this threshold */
    int flush_timeout_ms; /* flush pending batch after this many ms even if watermark not hit */

    pthread_mutex_t    lock; 

    struct pending_batch  pending;   /* requests waiting to be sent to kernel */
    struct batch_tracker *inflight;  /* linked list of batches sent but not yet acked */

    int next_user_id;              
    int core_owner[MAX_PIM_UNIT];  /* user_id that owns each core, -1 if free */

    pthread_t  bg_completer_tid; 
    pthread_t  bg_check_tid;    
    void  *mem_base;   
    size_t chunk_size; 
    int    num_chunks; /* total chunks available = PIM_MEM_MAX_PAGES * PAGE_SIZE / chunk_size */
    bool  *chunk_free; /* chunk_free[i] = true means chunk slot i is available for allocation */

    int pfn2core_id[MAX_PIM_UNIT]; 
    struct timeval last_pause[MAX_PIM_UNIT]; 
    struct timeval last_resume[MAX_PIM_UNIT]; 
    bool is_penalty[MAX_PIM_UNIT]; // 1: pim owned; 0: cpu owned
    size_t pause_penalty; // the minimum interval between resume and pause
    size_t resume_watermark; // the minimum interval between pause and resume

} lib;

// Forward declarations
static void  flush_pending_locked(void);
static void *completer_loop(void *arg);
static void *bg_check_loop(void *arg);

// Library lifecycle


int pim_lib_init(int watermark, int flush_timeout_ms, size_t chunk_size)
/* Initializes the library: opens the PIM device, mmaps the non-cacheable memory pool,
 * starts the bg_completer and bg_check background threads.
 * Must be called once before any other pim_* function. Returns 0 on success, -1 on error. */
{
    memset(&lib, 0, sizeof(lib));
    memset(lib.core_owner, -1, sizeof(lib.core_owner));
    memset(lib.pfn2core_id, -1, sizeof(lib.pfn2core_id));
    memset(lib.is_penalty,   0, sizeof(lib.is_penalty));   // all start CPU_OWNED

    lib.watermark        = watermark        > 0 ? watermark        : PIM_DEFAULT_WATERMARK;
    lib.flush_timeout_ms = flush_timeout_ms > 0 ? flush_timeout_ms : PIM_DEFAULT_FLUSH_MS;

    long page_size = sysconf(_SC_PAGE_SIZE);
    if (page_size <= 0 || chunk_size == 0) {
        errno = EINVAL;
        return -1;
    }

    /* chunk_size must be page-aligned: the driver maps in PAGE_SIZE steps */
    if (chunk_size % (size_t)page_size != 0) {
        errno = EINVAL;
        return -1;
    }

    size_t total_pool = (size_t)PIM_MEM_MAX_PAGES * (size_t)page_size;

    /* Derive how many per-core chunks fit in the driver's fixed pool */
    int num_chunks = (int)(total_pool / chunk_size);
    if (num_chunks == 0) {
        errno = EINVAL;
        return -1;
    }

    lib.dev_fd = open(DEVICE_PATH, O_RDWR);
    if (lib.dev_fd < 0)
        return -1;

    lib.epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (lib.epoll_fd < 0)
        goto err_dev;

    lib.chunk_free = malloc((size_t)num_chunks * sizeof(bool));
    if (!lib.chunk_free)
        goto err_epoll;
    for (int i = 0; i < num_chunks; i++)
        lib.chunk_free[i] = true;

    /*
     * mmap the full 64-page non-cacheable slab in one call.
     * The driver ignores the offset argument and always maps from
     * pim_mempool + 0; MAP_SHARED is required for the non-cacheable
     * pgprot set by the driver to take effect.
     */
    lib.mem_base = mmap(NULL, total_pool, PROT_READ | PROT_WRITE, MAP_SHARED,
                        lib.dev_fd, 0);
    if (lib.mem_base == MAP_FAILED)
        goto err_chunk_free;

    lib.chunk_size = chunk_size;
    lib.num_chunks = num_chunks;

    pthread_mutex_init(&lib.lock, NULL);
    atomic_store(&lib.running, 1);

    pthread_create(&lib.bg_check_tid, NULL, bg_check_loop, NULL);

    if (pthread_create(&lib.bg_completer_tid, NULL, completer_loop, NULL) != 0)
        goto err_mmap;

    return 0;

err_mmap:
    munmap(lib.mem_base, total_pool);
err_chunk_free:
    free(lib.chunk_free);
err_epoll:
    close(lib.epoll_fd);
err_dev:
    close(lib.dev_fd);
    return -1;
}

void pim_lib_fini(void)
/* Shuts down the library: stops background threads, flushes any remaining pending
 * requests, unmaps memory, and closes all file descriptors. */
{
    atomic_store(&lib.running, 0);
    pthread_join(lib.bg_completer_tid, NULL);
    pthread_join(lib.bg_check_tid, NULL);


    pthread_mutex_lock(&lib.lock);
    if (lib.pending.count > 0)
        flush_pending_locked();
    pthread_mutex_unlock(&lib.lock);

    munmap(lib.mem_base, lib.chunk_size * (size_t)lib.num_chunks);
    free(lib.chunk_free);

    close(lib.epoll_fd);
    close(lib.dev_fd);
    pthread_mutex_destroy(&lib.lock);
}

 // User management

pim_user_t *pim_user_create(void)
/* Allocates and registers a new user handle. Each user thread should create its own
 * handle before calling pim_alloc_cores. Returns NULL on allocation failure. */
{
    pim_user_t *u = calloc(1, sizeof(*u));
    if (!u) return NULL;

    /* Mark all per-core chunk indices as unassigned */
    memset(u->core2chunk, -1, sizeof(u->core2chunk));

    pthread_mutex_lock(&lib.lock);
    u->user_id = lib.next_user_id++;
    pthread_mutex_unlock(&lib.lock);
    return u;
}

void pim_user_destroy(pim_user_t *user)
/* Releases all cores owned by this user and frees the user handle. */
{
    if (!user) return;
    pim_free_cores(user);
    free(user);
}



/* -------------------------------------------------------------------------
 * Core allocation
 * ---------------------------------------------------------------------- */


int pim_alloc_cores(pim_user_t *user, int *allocated_core_ids, int count)
/* User can allocate cores, they need to pass in the number of cores to allocate(count)
 * and it will return a list of allocated core IDs 
 * User should pass in the array to store the allocated core IDs, it cannot be NULL
 */
{
    if (!user || !allocated_core_ids || count <= 0) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&lib.lock);

    /* Pre-check: enough free PIM cores */
    int avail_cores = 0;
    for (int i = 0; i < MAX_PIM_UNIT; i++)
        if (lib.core_owner[i] == -1) avail_cores++;

    /* Pre-check: enough free chunk slots (one chunk needed per core) */
    int avail_chunks = 0;
    for (int i = 0; i < lib.num_chunks; i++)
        if (lib.chunk_free[i]) avail_chunks++;

    if (avail_cores < count || avail_chunks < count) {
        pthread_mutex_unlock(&lib.lock);
        errno = ENOSPC;
        return -1;
    }

    int found = 0;
    for (int i = 0; i < MAX_PIM_UNIT && found < count; i++) {
        if (lib.core_owner[i] != -1)
            continue;

        /* Find a free chunk slot for this core */
        int chunk_idx = -1;
        for (int j = 0; j < lib.num_chunks; j++) {
            if (lib.chunk_free[j]) {
                chunk_idx = j;
                lib.chunk_free[j] = false;
                break;
            }
        }
        /* chunk_idx != -1 guaranteed by the pre-check above */

        lib.core_owner[i]          = user->user_id;
        user->owns_core[i]         = true;
        user->core2chunk[i]    = chunk_idx;
        
        user->core_mode[i] = false;  /* starts HOST_OWNED */
        allocated_core_ids[found++]          = i;

        // CHANGE:
        lib.pfn2core_id[chunk_idx] = i; // record the mapping from chunk slot to core id
        gettimeofday(&lib.last_pause[i], NULL);  // have to set this otherwise in check() the 
                                                 // time_since_last_pause will be very large and cause 
                                                 // incorrect resume when user first try to access the core
        lib.is_penalty[i] = false;  // starts HOST_OWNED
    }

    pthread_mutex_unlock(&lib.lock);
    return 0;
}


void pim_free_cores(pim_user_t *user)
/* Releases all cores owned by this user: clears pfn2core_id, returns chunk slots to
 * the pool, and resets all per-core ownership state. */
{
    if (!user) return;
    pthread_mutex_lock(&lib.lock);
    for (int i = 0; i < MAX_PIM_UNIT; i++) {
        if (!user->owns_core[i])
            continue;
        lib.pfn2core_id[user->core2chunk[i]] = -1; // clear the pfn to core id mapping
        lib.core_owner[i]                      = -1;
        lib.chunk_free[user->core2chunk[i]] = true;
        user->owns_core[i]                     = false;
        user->core2chunk[i]                = -1;
        user->core_mode[i]             = false;
    }
    pthread_mutex_unlock(&lib.lock);
}

/* -------------------------------------------------------------------------
 * Submission (non-blocking)
 * ---------------------------------------------------------------------- */

pim_req_handle_t *pim_submit(pim_user_t *user, const char cmd_list[MAX_PIM_UNIT])

/* User can submit a list of commands to the to the pim core he has
 * allocated. This function will also modify the core mode between PIM mode and HOST mode
 * according to commands.
 * Returns a handle that can be used to query or wait for
 * completion, or NULL on error. */

{
    if (!cmd_list) {
        errno = EINVAL;
        return NULL;
    }

    for (int i = 0; i < MAX_PIM_UNIT; i++) {
        // ownership check:
        if (!user->owns_core[i]) {
            fprintf(stderr, "pim_submit: user %d does not own core %d\n",
                    user->user_id, i);
            errno = EACCES;
            return NULL;
        }
        // if there are already pending command for this core, we should not allow user to submit 
        // new command before the previous one is fulfilled
        if (lib.pending.cmd_list[i] != -1) {
            fprintf(stderr, "pim_submit: user %d has pending command for core %d\n",
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

    for (int i = 0; i < MAX_PIM_UNIT; i++) {
        lib.pending.cmd_list[i] = cmd_list[i];
    }

    if (lib.pending.count == 0)
        gettimeofday(&lib.pending.first_submit_ts, NULL);

    if (lib.pending.tail)
        lib.pending.tail->next = req;
    else
        lib.pending.head = req;
    lib.pending.tail = req;
    lib.pending.count++;

    // Per-core ownership update.  
    
    for (int i = 0; i < MAX_PIM_UNIT; i++) {
        if (!user->owns_core[i])
            continue;
        if (cmd_list[i] == PIM_START || cmd_list[i] == MEM_RESUME)
            user->core_mode[i] = true;   /* core i: PIM takes ownership */
        else if (cmd_list[i] == MEM_PAUSE)
            user->core_mode[i] = false;  /* core i: host takes ownership */
    }

    if (lib.pending.count >= lib.watermark)
        flush_pending_locked();

    pthread_mutex_unlock(&lib.lock);
    return req;
}

// Completion  

int pim_poll(pim_req_handle_t *req)

/* User can check if the request is complete.
 * Returns 1 if req is complete, 0 if still pending, or -1 on error. */

{
    if (!req) return -1;
    return atomic_load(&req->done) ? 1 : 0;
}

int pim_wait(pim_req_handle_t *req, int timeout_ms)
/* Blocks until req is complete or timeout_ms milliseconds elapse.
 * Pass timeout_ms = -1 to wait forever. Returns 0 on completion, -1 on timeout (errno=ETIMEDOUT). */
{
    if (!req) {
        errno = EINVAL;
        return -1;
    }

    struct timeval deadline = {0};
    if (timeout_ms >= 0) {
        gettimeofday(&deadline, NULL);
        deadline.tv_sec  += timeout_ms / 1000;
        deadline.tv_usec += (timeout_ms % 1000) * 1000;
    }

    while (!atomic_load(&req->done)) {
        if (timeout_ms >= 0) {
            struct timeval now;
            gettimeofday(&now, NULL);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_usec >= deadline.tv_usec)) {
                errno = ETIMEDOUT;
                return -1;
            }
        }
        usleep(100);
    }
    return 0;
}

void pim_req_free(pim_req_handle_t *req)
/* Frees a request handle. Only call after pim_poll or pim_wait confirms completion. */
{
    free(req);
}


// CPU-PIM Access Management


enum acc_state_t { SUCC, FAIL};

// Do we need this function?
static inline void pause_core(int core_id)
/* Internal: sends MEM_PAUSE to core_id and waits for ack. Called by bg_check_loop only.
 * Updates last_pause and clears is_penalty so the core is marked CPU_OWNED. */
{
    char cmd_list[MAX_PIM_UNIT];
    memset(cmd_list, -1, sizeof(cmd_list));
    cmd_list[core_id] = MEM_PAUSE;

    pim_req_handle_t *req = pim_submit(NULL, cmd_list);
    if (!req) {
        fprintf(stderr, "pause_core: failed to submit pause for core %d\n", core_id);
        return;
    }

    pthread_mutex_lock(&lib.lock);
    flush_pending_locked();
    pthread_mutex_unlock(&lib.lock);

    if (pim_wait(req, -1) != 0) {
        fprintf(stderr, "pause_core: failed to wait for pause completion on core %d\n", core_id);
    }
    pim_req_free(req);

    gettimeofday(&lib.last_pause[core_id], NULL); 
}

static inline void resume_core(int core_id)
/* Internal: sends MEM_RESUME to core_id and waits for ack. Called by bg_check_loop only
 * when CPU has overstayed resume_watermark. Updates last_resume and sets is_penalty. */
{
    char cmd_list[MAX_PIM_UNIT];
    memset(cmd_list, -1, sizeof(cmd_list));
    cmd_list[core_id] = MEM_RESUME;

    pim_req_handle_t *req = pim_submit(NULL, cmd_list);
    if (!req) {
        fprintf(stderr, "resume_core: failed to submit resume for core %d\n", core_id);
        return;
    }

    pthread_mutex_lock(&lib.lock);
    flush_pending_locked();
    pthread_mutex_unlock(&lib.lock);

    if (pim_wait(req, -1) != 0) {
        fprintf(stderr, "resume_core: failed to wait for resume completion on core %d\n", core_id);
    }
    pim_req_free(req);

    gettimeofday(&lib.last_resume[core_id], NULL);
}


static inline acc_state_t check(int core_id) 
{
    struct timeval now;
    gettimeofday(&now, NULL);
    long time_since_last_pause = (now.tv_sec - lib.last_pause[core_id].tv_sec) * 1000L +
                                (now.tv_usec - lib.last_pause[core_id].tv_usec) / 1000L;
    long time_since_last_resume = (now.tv_sec - lib.last_resume[core_id].tv_sec) * 1000L +
                           (now.tv_usec - lib.last_resume[core_id].tv_usec) / 1000L;
    
    if (lib.is_penalty[core_id]) {
        // pim owned the core/memory chunk
        if (time_since_last_resume < lib.pause_penalty) {
            // try to access it within penalty time
            return FAIL;
        }
    } else {
        // cpu owned the core/memory chunk
        if (time_since_last_pause > lib.resume_watermark) {
            resume_core(core_id);
            return FAIL;
        }
    }
    return SUCC;
}

acc_ret read_64(size_t addr) {
    int pfn = (addr - (size_t)lib.mem_base) / lib.chunk_size;
    int core_id = lib.pfn2core_id[pfn];

    if (check(core_id) == FAIL) {
        return (acc_ret){ .ret_state = FAIL, .val = 0};
    }
    uint64_t val = *addr;
    return (acc_ret){ .ret_state = SUCC, .val = val };
}

acc_ret write_64(size_t addr, uint64_t val) {
    int pfn     = (addr - (size_t)lib.mem_base) / lib.chunk_size;
    int core_id = lib.pfn2core_id[pfn];

    if (check(core_id) == FAIL)
        return (acc_ret){ .ret_state = FAIL, .val = 0 };

    *(volatile uint64_t *)addr = val;
    return (acc_ret){ .ret_state = SUCC, .val = 0 };
}

static void flush_pending_locked(void)
/* flush all the pending requests to the kernel*/
{
    if (lib.pending.count == 0)
        return;

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
    if (ioctl(lib.dev_fd, IOCTL_MAGIC, &kreq) < 0)
        perror("pim flush: ioctl");

    // update the last pause/resume time and penalty state for each core according to the command list
    // might not be too accurate since this is not the point it start execution,
    // we cannot guarantee the order of command execution for different cores in the kernel, 
    // but it still probably be good for our current design
    for (int i = 0; i < MAX_PIM_UNIT; i++) {
        if (lib.pending.cmd_list[i] == MEM_PAUSE) {
            gettimeofday(&lib.last_pause[i], NULL);
            lib.is_penalty[i] = false;
        } else if (lib.pending.cmd_list[i] == MEM_RESUME) {
            gettimeofday(&lib.last_resume[i], NULL);
            lib.is_penalty[i] = true;
    }
}

    memset(lib.pending.cmd_list, -1, sizeof(lib.pending.cmd_list));
    lib.pending.count = 0;
    lib.pending.head  = NULL;
    lib.pending.tail  = NULL;
}

/*  Background  thread */

static void *bg_check_loop(void *arg)
/* Background watchdog thread. Scans all allocated cores every 500µs and calls check().
 * If a core is CPU_OWNED and has exceeded resume_watermark, forces a resume so PIM
 * is not blocked indefinitely. Acts as a safety net for when no read_64/write_64 is active. */
{
    (void)arg;
    while (atomic_load(&lib.running)) {
        for (int i = 0; i < MAX_PIM_UNIT; i++) {
            if (lib.pfn2core_id[i] == -1) continue;
            if (check(i) == FAIL && !lib.is_penalty[i])
                resume_core(i);    // CPU overstayed, force resume
        }
        usleep(500);
    }
    return NULL;
}

static void *completer_loop(void *arg)
{
    (void)arg;
    struct epoll_event events[64];

    while (atomic_load(&lib.running)) {

        int n = epoll_wait(lib.epoll_fd, events, 64, 0);

        for (int i = 0; i < n; i++) {
            struct batch_tracker *bt = events[i].data.ptr;

            uint64_t val;
            read(bt->efd, &val, sizeof(val));

            struct pim_req_handle *r = bt->req_head;
            while (r) {
                atomic_store(&r->done, 1);
                r = r->next;
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
        // force to flush the pending request if the pending request has been waiting
        //  for too long
        pthread_mutex_lock(&lib.lock);
        if (lib.pending.count > 0) {
            struct timeval now;
            gettimeofday(&now, NULL);
            long elapsed_ms =
                (now.tv_sec  - lib.pending.first_submit_ts.tv_sec)  * 1000L +
                (now.tv_usec - lib.pending.first_submit_ts.tv_usec) / 1000L;
            if (elapsed_ms >= lib.flush_timeout_ms)
                flush_pending_locked();
        }
        pthread_mutex_unlock(&lib.lock);

        usleep(500);
    }

    return NULL;
}
