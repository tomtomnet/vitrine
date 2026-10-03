/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * System access on the real system: the only place the installed helper
 * touches files, schedulers and capabilities.  The test build replaces this
 * file with tests/fakesys.c.
 */
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/xattr.h>
#include <syslog.h>
#include <unistd.h>

#include "helper.h"

bool sys_init(void)
{
    openlog("vitrine-helper", LOG_PID, LOG_DAEMON);
    return true;
}

const char *sys_root(void)
{
    return "";
}

int sys_read(const char *path, char *buf, size_t size)
{
    /* the attributes are files of their own: never follow a link to one */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY);
    ssize_t n;

    if (fd < 0) {
        return -errno;
    }
    n = read(fd, buf, size - 1);
    if (n < 0) {
        n = -errno;
    } else {
        buf[n] = '\0';
    }
    close(fd);
    return (int)n;
}

int sys_write(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY);
    size_t len = strlen(value);
    ssize_t n;
    int ret = 0;

    if (fd < 0) {
        return -errno;
    }
    /* sysfs and debugfs take a value in a single write */
    n = write(fd, value, len);
    if (n < 0) {
        ret = -errno;
    } else if ((size_t)n != len) {
        ret = -EIO;
    }
    if (close(fd) < 0 && ret == 0) {
        ret = -errno;
    }
    return ret;
}

int sys_getsched(pid_t tid, int *policy, int *priority)
{
    struct sched_param sp;
    int p = sched_getscheduler(tid);

    if (p < 0 || sched_getparam(tid, &sp) < 0) {
        return -errno;
    }
    /* without SCHED_RESET_ON_FORK's bit */
    *policy = p & ~SCHED_RESET_ON_FORK;
    *priority = sp.sched_priority;
    return 0;
}

int sys_setsched(pid_t tid, int policy, int priority)
{
    struct sched_param sp = {.sched_priority = priority};

    return sched_setscheduler(tid, policy, &sp) < 0 ? -errno : 0;
}

int sys_set_file_cap(int fd, const char *path)
{
    /* what setcap cap_sys_nice=ep writes: revision 2, effective, permitted
       (little-endian on disk) */
    struct vfs_cap_data cap = {
        .magic_etc = htole32(VFS_CAP_REVISION_2 | VFS_CAP_FLAGS_EFFECTIVE),
        .data = {{.permitted = htole32(1u << CAP_SYS_NICE), .inheritable = 0}, {0, 0}},
    };
    struct vfs_cap_data back;
    ssize_t n;

    (void)path;
    if (fsetxattr(fd, "security.capability", &cap, XATTR_CAPS_SZ_2, 0) < 0) {
        return -errno;
    }
    /* read back: an LSM or file system may drop it silently */
    n = fgetxattr(fd, "security.capability", &back, sizeof(back));
    if (n != XATTR_CAPS_SZ_2 || memcmp(&back, &cap, XATTR_CAPS_SZ_2) != 0) {
        return -EIO;
    }
    return 0;
}

void sys_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsyslog(LOG_NOTICE, fmt, ap);
    va_end(ap);
}
