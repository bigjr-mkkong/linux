#ifndef PIM_RUNTIME_H
#define PIM_RUNTIME_H

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

typedef struct pim_user       pim_user_t;
typedef struct pim_req_handle pim_req_handle_t;

/*
 * Initialize the library — call once per process before anything else.
 *
 * watermark:        flush accumulated requests to kernel once this many
 *                   submissions are pending (0 = use default of 8).
 * flush_timeout_ms: flush to kernel after this many ms even if watermark
 *                   has not been reached (0 = use default of 10 ms).
 *
 * Internally this opens /dev/PIM_controller, creates the shared epoll
 * instance, and starts the background completer thread.
 */
int  pim_lib_init(int watermark, int flush_timeout_ms);
void pim_lib_fini(void);

/*
 * Per-user registration.
 * Each user thread (or logical task) creates its own handle.
 */
pim_user_t *pim_user_create(void);
void        pim_user_destroy(pim_user_t *user);

/*
 * Allocate `count` free PIM cores for this user.
 * core_ids[] is filled with the indices of the allocated cores.
 * Returns 0 on success, -1 if not enough cores are free (errno = ENOSPC).
 */
int  pim_alloc_cores(pim_user_t *user, int *core_ids, int count);
void pim_free_cores(pim_user_t *user);

/*
 * Non-blocking submission.
 *
 * cmd_list[i] is the command for PIM unit i. The library verifies that the
 * user only targets cores it owns; violating this returns NULL (errno=EACCES).
 *
 * The request is accumulated into the current pending batch. The batch is
 * flushed to the kernel when the watermark is reached or the flush timeout
 * fires. Multiple users' requests may be combined into a single ioctl().
 *
 * Returns an opaque handle the caller uses to track completion; the caller
 * must eventually call pim_req_free() after the request completes.
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

/* Release a handle. Only call after pim_poll()/pim_wait() confirm completion. */
void pim_req_free(pim_req_handle_t *req);

#ifdef __cplusplus
}
#endif

#endif /* PIM_RUNTIME_H */
