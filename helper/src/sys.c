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
#include <grp.h>
#include <linux/capability.h>
#include <sched.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

int sys_group_find(const char *name, gid_t *gid)
{
    struct group *gr = getgrnam(name);

    if (!gr) {
        return 0;
    }
    *gid = gr->gr_gid;
    return 1;
}

bool sys_group_has(const char *user, gid_t primary, gid_t gid)
{
    gid_t some[64], *groups = some;
    int n = 64;
    bool member = primary == gid;

    /* the user database's list, as polkit reads it: not this process's */
    if (!member && getgrouplist(user, primary, groups, &n) < 0) {
        groups = n > 0 ? calloc((size_t)n, sizeof(gid_t)) : NULL;
        if (!groups || getgrouplist(user, primary, groups, &n) < 0) {
            n = 0;
        }
    }
    for (int i = 0; !member && i < n; i++) {
        member = groups[i] == gid;
    }
    if (groups != some) {
        free(groups);
    }
    return member;
}

/*
 * Runs a tool of the shadow suite, which keeps /etc/group and /etc/gshadow
 * consistent and locked while it edits them, and tells caches (nscd, sssd)
 * about the change.  It is looked for in the usual places only, run with a
 * fixed environment, and the first line of its error output goes to @err
 * (its output says what it is doing - gpasswd's "Adding user ..." - and is
 * dropped).
 */
static int run_tool(const char *const places[], char *const argv[], char *err, size_t size)
{
    static char *const env[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", NULL};
    posix_spawn_file_actions_t actions;
    int out[2], status = -1, rc = ENOENT;
    const char *tool = NULL;
    size_t len = 0;
    ssize_t n;
    pid_t pid;

    err[0] = '\0';
    for (int i = 0; places[i] && !tool; i++) {
        if (access(places[i], X_OK) == 0) {
            tool = places[i];
        }
    }
    if (!tool) {
        snprintf(err, size, "%s is not installed", argv[0]);
        return -1;
    }
    if (pipe2(out, O_CLOEXEC) < 0) {
        snprintf(err, size, "%s", strerror(errno));
        return -1;
    }
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDERR_FILENO);
    rc = posix_spawn(&pid, tool, &actions, NULL, argv, env);
    posix_spawn_file_actions_destroy(&actions);
    close(out[1]);
    if (rc != 0) {
        close(out[0]);
        snprintf(err, size, "%s: %s", tool, strerror(rc));
        return -1;
    }
    while (len < size - 1 && ((n = read(out[0], err + len, size - 1 - len)) > 0 ||
                              (n < 0 && errno == EINTR))) {
        len += n > 0 ? (size_t)n : 0;
    }
    err[len] = '\0';
    close(out[0]);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            /* its status unknown: not a success (main() lets no SIGCHLD
               ignored have the kernel reap it) */
            snprintf(err, size, "cannot wait for %s: %s", argv[0], strerror(errno));
            return -1;
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        err[0] = '\0';
        return 0;
    }
    /* its first line */
    err[strcspn(err, "\n")] = '\0';
    if (!err[0]) {
        snprintf(err, size, "%s failed (status %d)", argv[0],
                 WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
    }
    return -1;
}

int sys_group_create(const char *name, char *err, size_t size)
{
    static const char *const places[] = {"/usr/sbin/groupadd", "/usr/bin/groupadd",
                                         "/sbin/groupadd", NULL};
    char *const argv[] = {"groupadd", "--system", (char *)name, NULL};

    return run_tool(places, argv, err, size);
}

int sys_group_add_user(const char *name, const char *user, char *err, size_t size)
{
    static const char *const places[] = {"/usr/bin/gpasswd", "/usr/sbin/gpasswd",
                                         "/bin/gpasswd", NULL};
    char *const argv[] = {"gpasswd", "-a", (char *)user, (char *)name, NULL};

    return run_tool(places, argv, err, size);
}
