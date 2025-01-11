#ifndef __PENG_OPS__
#define __PENG_OPS__

#include <linux/pagemap.h>
int io_prep_pengpush(struct io_kiocb *req, const struct io_uring_sqe *sqe);
int io_pengpush(struct io_kiocb *req, unsigned int issue_flags);

#endif
