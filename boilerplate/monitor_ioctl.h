#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define MONITOR_MAGIC 0xAB
#define MONITOR_DEVICE "/dev/container_monitor"
#define CONTAINER_ID_LEN 32

struct monitor_request {
    int pid;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    char container_id[CONTAINER_ID_LEN];
};

#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request)

#endif /* MONITOR_IOCTL_H */
