#include "asm-generic/errno-base.h"
#include "linux/jiffies.h"
#include "linux/printk.h"
#include "linux/spinlock.h"
#include "linux/spinlock_types.h"
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/kfifo.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/eventfd.h>
#include "PIM_control_cmd.h"

#define DRV_NAME "PIM_controller"
#define FIFO_MAX_ELEMENTS 1024

/*
 * How to use it:
 * Only one user thread can call ioctl()
 * ioctl() will spawn wthread(working thread), which actually start to process user requests
 * user use epoll() to check if their requests being fulfilled
 */

struct fifo_elem_t{
    struct PIM_req_t user_req;
    struct eventfd_ctx *ctx;
};

static struct kfifo req_fifo;
static DEFINE_SPINLOCK(req_fifo_lock);

static struct workqueue_struct *wthread_wq;
static struct work_struct wthread;


static void wthread_func(struct work_struct *work) {
    struct fifo_elem_t cmd;

    // I need to replace all kfifo in/out with spinlock version

    if(kfifo_is_empty_spinlocked(&req_fifo, &req_fifo_lock)){
        return;
    }

    if(kfifo_out_locked(&req_fifo, &cmd, 1, &req_fifo_lock) != 1) {
        pr_warn("[PIM wthread] kfifo failed to provide command\n");
        return;
    }

    for(int i=0; i<MAX_PIM_UNIT; i++) {
        int sub_cmd = cmd.user_req.req_list[i];
        switch(sub_cmd) {
            case PIM_NOP:
            {
                break;
            }
            case PIM_START:
            {
                pr_info(DRV_NAME "PIM_START @ %lld\n", get_jiffies_64());
                eventfd_signal(cmd.ctx);
                break;
            }
            case MEM_PAUSE:
            {
                pr_info(DRV_NAME "MEM_PAUSE @ %lld\n", get_jiffies_64());
                eventfd_signal(cmd.ctx);
                break;
            }
            case MEM_RESUME:
            {
                pr_info(DRV_NAME "MEM_RESUME @ %lld\n", get_jiffies_64());
                eventfd_signal(cmd.ctx);
                break;
            }
            case PIM_QUERY:
            {
                pr_info(DRV_NAME "PIM_QUERY @ %lld\n", get_jiffies_64());
                eventfd_signal(cmd.ctx);
                break;
            }
            default:
                pr_warn(DRV_NAME "Unidentified command for pim unit %d: %d\n", \
                        i, sub_cmd);
        }
    }

    eventfd_ctx_put(cmd.ctx);

    return;
}

static long pim_controller_ioctl(struct file *file, unsigned int cmd_type,\
    unsigned long arg) {

    struct fifo_elem_t payload;

    if (cmd_type != MAGIC)
        return -ENOTTY;

    if (copy_from_user(&payload.user_req, (struct PIM_req_t __user *)arg, \
                sizeof(struct PIM_req_t)))
        return -EFAULT;

    payload.ctx = eventfd_ctx_fdget(payload.user_req.event_fd);
    if (IS_ERR(payload.ctx)) {
        pr_err(DRV_NAME ": Invalid eventfd provided.\n");
        return PTR_ERR(payload.ctx);
    }

    if (kfifo_avail(&req_fifo) < sizeof(payload)) {
        eventfd_ctx_put(payload.ctx);
        return -ENOSPC;
    }
    kfifo_in_spinlocked(&req_fifo, &payload, sizeof(payload), &req_fifo_lock);

    queue_work(wthread_wq, &wthread);

    return 0;
}

static const struct file_operations my_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = pim_controller_ioctl,
};

static struct miscdevice pim_cntr = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DRV_NAME,
    .fops = &my_fops,
};

static int __init pim_controller_init(void)
{
    int ret;

    ret = kfifo_alloc(&req_fifo, FIFO_MAX_ELEMENTS * sizeof(int), GFP_KERNEL);
    if (ret) return ret;

    INIT_WORK(&wthread, wthread_func);

    wthread_wq = alloc_ordered_workqueue("epoll_wq", WQ_UNBOUND);
    if(!wthread_wq) {
        pr_warn(DRV_NAME "Failed to acquire working thread\n");
        return -EINVAL;
    }

    ret = misc_register(&pim_cntr);
    if (ret) {
        kfifo_free(&req_fifo);
        pr_warn(DRV_NAME "Failed to register PIM driver\n");
        return ret;
    }

    pr_info(DRV_NAME ": Module loaded. /dev/%s created.\n", DRV_NAME);
    return 0;
}

static void __exit pim_controller_exit(void)
{
    misc_deregister(&pim_cntr);
    flush_work(&wthread);
    kfifo_free(&req_fifo);
    pr_info(DRV_NAME ": Module unloaded.\n");
}

module_init(pim_controller_init);
module_exit(pim_controller_exit);

MODULE_LICENSE("GPL");
