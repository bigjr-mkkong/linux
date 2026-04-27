#ifndef PIM_RUNTIME_H
#define PIM_RUNTIME_H

#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIM_NOP        0
#define PIM_START      1
#define MEM_PAUSE      2
#define MEM_RESUME     3
#define PIM_QUERY      4

#define MAX_PIM_UNIT          128
#define PIM_DEFAULT_WATERMARK   8
#define PIM_DEFAULT_FLUSH_MS   10

/*
 * The driver's non-cacheable mempool is fixed at 64 pages (PIM_MAX_MEM in
 * PIM_controller.c).  Each PIM core that is allocated gets one chunk of
 * chunk_size bytes carved from this pool, so at most
 * floor(64 * PAGE_SIZE / chunk_size) cores can hold chunks simultaneously.
 * chunk_size must be a multiple of the system page size.
 */
#define PIM_MEM_MAX_PAGES      64

struct pim_user {
    int  user_id;
    bool owns_core[MAX_PIM_UNIT];

    int   core2chunk[MAX_PIM_UNIT];     /* -1 when core not owned */
    bool  core_mode[MAX_PIM_UNIT];  /* false = HOST_OWNED */ // need to verify
};
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


 

 // Global library state (singleton)


enum rw_state_t{
    SUCC,
    FAIL
};

struct rw_ret{
    enum rw_state_t state;
    uint64_t data;
};
typedef struct pim_user       pim_user_t;
typedef struct pim_req_handle pim_req_handle_t;

/*
 * Initialize the library — call once per process before anything else.
 *
 * watermark:        flush accumulated requests to kernel once this many
 *                   submissions are pending (0 = use default of 8).
 * flush_timeout_ms: flush to kernel after this many ms even if watermark
 *                   has not been reached (0 = use default of 10 ms).
 * chunk_size:       size in bytes of each per-core non-cacheable memory
 *                   chunk.  Must be a multiple of the system page size.
 *                   All cores share the same chunk size.
 *
 * The library mmaps PIM_MEM_MAX_PAGES * PAGE_SIZE bytes from
 * /dev/PIM_controller in one call and slices it into
 * floor(PIM_MEM_MAX_PAGES * PAGE_SIZE / chunk_size) equal chunks, one
 * per allocated PIM core.  max_users is no longer a parameter because the
 * pool size is determined entirely by the driver's hard cap and chunk_size.
 */
int  pim_lib_init(int watermark, int flush_timeout_ms, size_t chunk_size);
void pim_lib_fini(void);

/*
 * Per-user registration.
 * Each user thread (or logical task) creates its own handle.
 * pim_user_create no longer claims a memory chunk; chunks are claimed
 * per-core inside pim_alloc_cores.
 */
pim_user_t *pim_user_create(void);
void        pim_user_destroy(pim_user_t *user);

/*
 * Allocate `count` free PIM cores for this user.
 * core_ids[] is filled with the indices of the allocated cores.
 *
 * Each allocated core is also assigned one chunk from the non-cacheable
 * pool.  The chunks are independent: starting a PIM operation on one core
 * does not affect the ownership state of any other core's chunk.
 *
 * Returns 0 on success, -1 if not enough cores or pool slots are free
 * (errno = ENOSPC).
 */
int  pim_alloc_cores(pim_user_t *user, int *core_ids, int count);

/*
 * Free all PIM cores owned by this user and return their chunk slots to
 * the pool.
 */
void pim_free_cores(pim_user_t *user);

/*
 * Non-blocking submission.
 *
 * cmd_list[i] is the command for PIM unit i.  The library verifies that
 * the user only targets cores it owns; violating this returns NULL
 * (errno=EACCES).
 *
 * pim_submit automatically updates the per-core chunk ownership state
 * based on the command sent to each core:
 *   PIM_START / MEM_RESUME on core i  →  core i's chunk becomes PIM_OWNED
 *   MEM_PAUSE              on core i  →  core i's chunk becomes HOST_OWNED
 * Only the cores named in cmd_list are affected; other cores' states are
 * unchanged.
 */
pim_req_handle_t *pim_submit(pim_user_t *user, const char cmd_list[MAX_PIM_UNIT]);

/*
 * Non-blocking completion check.
 * Returns 1 if done, 0 if still pending, -1 on error.
 */
int pim_poll(pim_req_handle_t *req);

/*
 * Blocking completion wait.
 * timeout_ms: -1 = wait forever, 0 = check once without sleeping.
 * Returns 0 on completion, -1 on timeout (errno=ETIMEDOUT) or error.
 */
int pim_wait(pim_req_handle_t *req, int timeout_ms);

/* Release a handle.  Only call after pim_poll()/pim_wait() confirm completion. */
void pim_req_free(pim_req_handle_t *req);

/*
 * Return a pointer to the non-cacheable chunk belonging to core_id.
 * The user writes input data here before handing the core to PIM, and
 * reads results from here (via pim_read) after PIM work is done.
 * Returns NULL if user does not own core_id.
 */
void *pim_user_mem(pim_user_t *user, int core_id);

/*
 * Return the size in bytes of every core's chunk (= chunk_size passed to
 * pim_lib_init).  All chunks are equal in size.
 */
size_t pim_chunk_size(void);


void pause_core(int core_id);
void resume_core(int core_id);
struct rw_ret read_64(struct pim_user *user, void *addr);
struct rw_ret write_64(struct pim_user *user, void *addr, uint64_t val);
void *get_lib_base();
#ifdef __cplusplus
}
#endif

#endif /* PIM_RUNTIME_H */
