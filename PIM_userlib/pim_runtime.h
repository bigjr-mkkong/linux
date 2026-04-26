#ifndef PIM_RUNTIME_H
#define PIM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

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

/*
 * Safe read from the non-cacheable chunk of core_id.
 *
 * offset: byte offset within core_id's chunk (0-based, 0 to chunk_size-1).
 * dst:    destination buffer in normal (cacheable) host memory.
 * len:    number of bytes to read.
 *
 * Only the chunk belonging to core_id is considered.  Other cores' chunks
 * are not paused and are not affected.
 *
 * If core_id's chunk is HOST_OWNED, the read is a direct memcpy.
 * If core_id's chunk is PIM_OWNED, pim_read automatically:
 *   1. Sends MEM_PAUSE to core_id only.
 *   2. Waits for the pause acknowledgement.
 *   3. Copies len bytes from chunk[offset] into dst.
 *   4. Sends MEM_RESUME to core_id.
 *   5. Waits for the resume acknowledgement before returning.
 *
 * Returns 0 on success, -1 on error (errno set).
 */
int pim_read(pim_user_t *user, int core_id, uint64_t offset, void *dst, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PIM_RUNTIME_H */
