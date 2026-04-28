#ifndef PIM_RUNTIME_H
#define PIM_RUNTIME_H

#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <semaphore.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIM_NOP        0
#define PIM_START      1
#define MEM_PAUSE      2
#define MEM_RESUME     3
#define PIM_QUERY      4

#define MAX_PIM_UNIT   128
#define PIM_MEM_MAX_PAGES 64

struct pim_user {
    int  user_id;
    bool owns_core[MAX_PIM_UNIT];
    int  core2chunk[MAX_PIM_UNIT];
    bool core_mode[MAX_PIM_UNIT];
};

struct pim_timing {
    struct timeval last_pause [MAX_PIM_UNIT];
    struct timeval last_resume[MAX_PIM_UNIT];
    bool           is_penalty [MAX_PIM_UNIT]; /* 1: pim owned; 0: cpu owned */
};

/* Mirror of kernel ABI */
struct pim_req_t {
    int  event_fd;
    char req_list[MAX_PIM_UNIT];
};

struct pim_req_handle {
    atomic_int            done;
    sem_t                 sem;
    struct pim_req_handle *next;
};

struct batch_tracker {
    int                   efd;
    struct pim_req_handle *req_head;
    struct batch_tracker  *next;
};

struct pending_batch {
    char                  cmd_list[MAX_PIM_UNIT];
    int                   count;
    struct pim_req_handle *head;
    struct pim_req_handle *tail;
};

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

/* Initialize library with the determined memory chunk size */
int  pim_lib_init(size_t chunk_size);
int  pim_lib_fini(void);

pim_user_t *pim_user_create(void);
void        pim_user_destroy(pim_user_t *user);
int  pim_alloc_cores(pim_user_t *user, int *core_ids, int count);
void pim_free_cores(pim_user_t *user);

pim_req_handle_t *pim_submit(pim_user_t *user, const char cmd_list[MAX_PIM_UNIT]);
int pim_poll(pim_req_handle_t *req);
int pim_wait(pim_req_handle_t *req, int timeout_ms);
void pim_req_free(pim_req_handle_t *req);

size_t pim_chunk_size(void);

void pause_core(pim_user_t *user, int core_id);
void resume_core(pim_user_t *user, int core_id);

struct rw_ret read_64(struct pim_user *user, void *addr);
struct rw_ret write_64(struct pim_user *user, void *addr, uint64_t val);
void *get_lib_base();

#ifdef __cplusplus
}
#endif

#endif /* PIM_RUNTIME_H */
