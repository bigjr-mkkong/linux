#ifndef PIM_CONTROL_CMD_H
#define PIM_CONTROL_CMD_H

#include <linux/ioctl.h>

#define MAGIC 114514
#define MY_IOCTL_ENQUEUE_CMD _IOW(MY_MAGIC, 1, int)

#define PIM_NOP     0
#define PIM_START   1
#define MEM_PAUSE   2
#define MEM_RESUME  3
#define PIM_QUERY   4

#define MAX_PIM_UNIT    128 

struct PIM_req_t{
    int event_fd;
    char req_list[MAX_PIM_UNIT];
};

#endif
