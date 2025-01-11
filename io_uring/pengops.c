#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/io_uring.h>

#include <uapi/linux/io_uring.h>

#include "io_uring.h"
#include "pengops.h"

int io_prep_pengpush(struct io_kiocb *req, const struct io_uring_sqe *sqe){
	return 0;
}

int io_pengpush(struct io_kiocb *req, unsigned int issue_flags){
	return IOU_OK;
}
