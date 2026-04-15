#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <stdint.h>
#include <string.h>

#include "../PIM_control_cmd.h"

#define DEVICE_PATH "/dev/PIM_controller"

/*
 * The user space library I am thing of rn is:
 * 1. User task will claim one(or more) PIM core from it during the initialization
 * 2. User task can submit command to their requested PIM core
 * 3. Library side command receiver should check if user is trying to  control its own allocated core and stop if user want to stop other people's core
 * 4. Library need to submit once the number of stacked requests reach the water mark or time is up
 * 5. User task will also trying to ask library if their requests are fulfilled, libarary should be prepared to do it
 * 6. Library should use unblocked epoll(as many other database system) to ask kernel if it complete certain task
 *
 * Below is an example of submit one requests and wait for response. Notice it's using blocking version for demo purpose
 */

int main() {
    int dev_fd, efd, epoll_fd;
    struct PIM_req_t req;
    struct epoll_event ev, events[1];

    /*
     * Open the PIM device for future operation
     * This code only runs once during the initialization
     */
    dev_fd = open(DEVICE_PATH, O_RDWR);
    if (dev_fd < 0) {
        perror("Failed to open " DEVICE_PATH " (Are you root?)");
        return EXIT_FAILURE;
    }

    /*
     * eventfd is a file descriptor used only for kernel signaling
     * To be clear, efd(event fd) still works under poll/epoll.
     * kernel only need to increment a counter of the context behind eventfd, then it can be detected by poll/epoll for synchronization
     * For different submission, they should hold different efd
     * initval can remain 0
     */
    efd = eventfd(0, EFD_NONBLOCK);
    if (efd < 0) {
        perror("Failed to create eventfd");
        close(dev_fd);
        return EXIT_FAILURE;
    }


    /*
     * epoll_event contain the fd we want to "monitor" as well as what change are we going to report back
     * Here we use previous created efd as the fd we are going to monitor, and EPOLLIN as the option
     * epoll_event struct is the infomation struct epoll used to know what fd and how are data going to be monitored
     */
    ev.events = EPOLLIN;
    ev.data.fd = efd;
    /*
     * epoll_fd is the id for a specific "epoll instance"
     * Normally in DBMS we need to have different epoll_fd for different event.
     * Here we only control one device, so we only need one epoll_fd as the whole library
     */
    epoll_fd = epoll_create1(0);

    /* 
     * epoll_ctl() will relate our monitoring target(efd) with the monitor instance(epoll_fd). This need to be done for every requests
     */
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, efd, &ev) < 0) {
        perror("Failed to add eventfd to epoll");
        goto cleanup;
    }

    /*
     * For multi requests, the code would looks like this:
     * efd1 = eventfd(0, EFD_NONEBLOCK);
     * ev.events = EPOLLIN;
     * ev.data.fd = efd1;
     * epoll_ctl(epoll_fd, EPOLL_CTL_ADD, efd1, &ev);
     *
     * efd2 = eventfd(0, EFD_NONEBLOCK);
     * ev.data.fd  = efd2;
     * epoll_ctl(epoll_fd, EPOLL_CTL_ADD, efd2, &ev);
     */


    /*
     * Here we setup the final payload we need to send to driver
     */
    memset(&req, 0, sizeof(struct PIM_req_t)); 
    req.event_fd = efd;
    req.req_list[0] = PIM_START;

    if (ioctl(dev_fd, MAGIC, &req) < 0) {
        perror("IOCTL failed");
        goto cleanup;
    } else {
        printf("Request submitted\n");
    }

    /*
     * events is the output buffer for epoll wait
     * For now I am only allowing one piece of report being written back from kernel
     * for multi requests, this number should be 64
     *
     * Another thing need to be aware is here epoll_wait is blocking wait
     * This is not what we want for final version
     * Remember to design a meta-loop for userlib-consumer side to constantly check epoll result non-blocking
     */
    int num_events = epoll_wait(epoll_fd, events, 1, -1);
    
    if (num_events > 0) {
        uint64_t completed_count;
        if (read(efd, &completed_count, sizeof(uint64_t)) > 0) {
            printf("Kernel signaled completion.\n");
        }
    } else {
        perror("epoll_wait failed:");
    }

cleanup:
    close(epoll_fd);
    close(efd);
    close(dev_fd);
    return EXIT_SUCCESS;
}
