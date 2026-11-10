/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * System access on the real system: the only place the installed helper
 * touches files, schedulers, limits, device nodes, modules and the group
 * database.  The test build replaces this file with tests/fakesys.c.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
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

int sys_getnice(pid_t tid, int *nice)
{
    int n;

    /* -1 is a nice value too: errno tells */
    errno = 0;
    n = getpriority(PRIO_PROCESS, (id_t)tid);
    if (n == -1 && errno) {
        return -errno;
    }
    *nice = n;
    return 0;
}

int sys_setnice(pid_t tid, int nice)
{
    return setpriority(PRIO_PROCESS, (id_t)tid, nice) < 0 ? -errno : 0;
}

int sys_getrttime(pid_t pid, rlim_t *soft, rlim_t *hard)
{
    struct rlimit old;

    if (prlimit(pid, RLIMIT_RTTIME, NULL, &old) < 0) {
        return -errno;
    }
    *soft = old.rlim_cur;
    *hard = old.rlim_max;
    return 0;
}

int sys_setrttime(pid_t pid, rlim_t soft, rlim_t hard)
{
    const struct rlimit limit = {.rlim_cur = soft, .rlim_max = hard};

    return prlimit(pid, RLIMIT_RTTIME, &limit, NULL) < 0 ? -errno : 0;
}

int sys_latency_request(const char *path, int us)
{
    const int32_t value = us;
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY);
    struct stat st;
    ssize_t n;

    if (fd < 0) {
        return -errno;
    }
    /* the kernel's device, never a file in its place: a write there would
       request nothing */
    if (fstat(fd, &st) < 0 || !S_ISCHR(st.st_mode)) {
        close(fd);
        return -ENODEV;
    }
    /* four bytes, the kernel's s32 (another length is read as hex text) */
    n = write(fd, &value, sizeof(value));
    if (n != (ssize_t)sizeof(value)) {
        const int err = n < 0 ? -errno : -EIO;

        close(fd);
        return err;
    }
    return fd;
}

void sys_latency_release(int fd)
{
    close(fd);
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

int sys_getmemlock(pid_t pid, rlim_t *soft, rlim_t *hard)
{
    struct rlimit old;

    if (prlimit(pid, RLIMIT_MEMLOCK, NULL, &old) < 0) {
        return -errno;
    }
    *soft = old.rlim_cur;
    *hard = old.rlim_max;
    return 0;
}

int sys_setmemlock(pid_t pid, rlim_t soft, rlim_t hard)
{
    const struct rlimit limit = {.rlim_cur = soft, .rlim_max = hard};

    return prlimit(pid, RLIMIT_MEMLOCK, &limit, NULL) < 0 ? -errno : 0;
}

int sys_load_module(const char *name, char *err, size_t size)
{
    static const char *const places[] = {"/usr/sbin/modprobe", "/sbin/modprobe",
                                         "/usr/bin/modprobe", NULL};
    char *const argv[] = {"modprobe", "--", (char *)name, NULL};

    return run_tool(places, argv, err, size);
}

/* The character device node @path itself, never what a link names: an
   O_PATH file of it, or -errno (-ENODEV: not a character device) */
static int node_open(const char *path, struct stat *st)
{
    int fd = open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC);

    if (fd < 0) {
        return -errno;
    }
    if (fstat(fd, st) < 0 || !S_ISCHR(st->st_mode)) {
        close(fd);
        return -ENODEV;
    }
    return fd;
}

int sys_node_get(const char *path, uid_t *uid, gid_t *gid, mode_t *mode)
{
    struct stat st;
    const int fd = node_open(path, &st);

    if (fd < 0) {
        return fd;
    }
    close(fd);
    *uid = st.st_uid;
    *gid = st.st_gid;
    *mode = st.st_mode & 07777;
    return 0;
}

int sys_node_set(const char *path, uid_t uid, gid_t gid, mode_t mode)
{
    char self[64];
    struct stat st;
    const int fd = node_open(path, &st);
    int err = 0;

    /*
     * The kernel's node (devtmpfs, a folder only root writes), changed
     * through the file that was checked: owner by it, mode through its
     * /proc/self/fd link (an O_PATH file takes no fchmod)
     */
    if (fd < 0) {
        return fd;
    }
    snprintf(self, sizeof(self), "/proc/self/fd/%d", fd);
    if (fchownat(fd, "", uid, gid, AT_EMPTY_PATH) < 0 || chmod(self, mode & 07777) < 0) {
        err = -errno;
    }
    close(fd);
    return err;
}

int sys_group_busy(const char *path)
{
    struct stat st;
    int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | O_NONBLOCK);
    int ret;

    /* a vfio group opens once: busy, someone has it; opened here, it is free
       (and nothing happens to it: no container, no device asked for) */
    if (fd < 0) {
        return errno == EBUSY ? 1 : -errno;
    }
    ret = fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) ? 0 : -ENODEV;
    close(fd);
    return ret;
}

int sys_rdev(const char *path, bool *block, unsigned *maj, unsigned *min)
{
    struct stat st;

    if (stat(path, &st) < 0) {
        return -errno;
    }
    if (!S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode)) {
        return -ENODEV;
    }
    *block = S_ISBLK(st.st_mode);
    *maj = major(st.st_rdev);
    *min = minor(st.st_rdev);
    return 0;
}

/* @comm of /proc/@pid, without its newline */
static void process_comm(unsigned long long pid, char *comm, size_t size)
{
    char path[64];
    ssize_t n = -1;
    int fd;

    snprintf(path, sizeof(path), "/proc/%llu/comm", pid);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) {
        n = read(fd, comm, size - 1);
        close(fd);
    }
    comm[n > 0 ? n : 0] = '\0';
    comm[strcspn(comm, "\n")] = '\0';
}

int sys_device_users(const struct devnum *devs, int ndevs, struct devuser *users, int max)
{
    const unsigned long long self = (unsigned long long)getpid();
    DIR *proc = opendir("/proc");
    struct dirent *e;
    int n = 0;

    if (!proc) {
        return -errno;
    }
    while ((e = readdir(proc))) {
        unsigned long long pid;
        bool other = false, sound = false;
        struct dirent *f;
        char path[64];
        DIR *fds;
        int fd;

        if (!parse_uint(e->d_name, 4194304, &pid) || pid == self) {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%llu/fd", pid);
        if ((fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
            continue;
        }
        if (!(fds = fdopendir(fd))) {
            close(fd);
            continue;
        }
        while ((f = readdir(fds))) {
            char link[256];
            struct statx sx;
            ssize_t len;

            if (f->d_name[0] == '.') {
                continue;
            }
            /*
             * Device nodes are under /dev: sockets, pipes and files elsewhere
             * are not looked at - a stat of a file on a network file system
             * could wait for its server, and the stat of a node does not
             * (AT_STATX_DONT_SYNC besides)
             */
            len = readlinkat(dirfd(fds), f->d_name, link, sizeof(link) - 1);
            if (len < 5 || strncmp(link, "/dev/", 5) != 0 ||
                statx(dirfd(fds), f->d_name, AT_STATX_DONT_SYNC, STATX_TYPE, &sx) < 0) {
                continue;
            }
            for (int i = 0; i < ndevs; i++) {
                const struct devnum *d = &devs[i];

                if ((d->block ? S_ISBLK(sx.stx_mode) : S_ISCHR(sx.stx_mode)) &&
                    sx.stx_rdev_major == d->maj && (d->min == DEVNUM_ANY || sx.stx_rdev_minor == d->min)) {
                    sound |= d->sound;
                    other |= !d->sound;
                }
            }
        }
        closedir(fds);
        if ((other || sound) && n < max) {
            static const char deleted[] = " (deleted)";
            ssize_t len;

            users[n].pid = (pid_t)pid;
            users[n].other = other;
            process_comm(pid, users[n].comm, sizeof(users[n].comm));
            /* the program it runs: what tells a sound server, not its name */
            snprintf(path, sizeof(path), "/proc/%llu/exe", pid);
            len = readlink(path, users[n].exe, sizeof(users[n].exe) - 1);
            users[n].exe[len > 0 ? len : 0] = '\0';
            /* updated while it runs */
            len = (ssize_t)strlen(users[n].exe);
            if (len > (ssize_t)sizeof(deleted) - 1 &&
                strcmp(users[n].exe + len - (sizeof(deleted) - 1), deleted) == 0) {
                users[n].exe[len - (sizeof(deleted) - 1)] = '\0';
            }
            n++;
        }
    }
    closedir(proc);
    return n;
}

char *sys_read_all(const char *path, size_t max)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY);
    size_t len = 0, cap = 4096;
    char *buf;

    if (fd < 0) {
        return NULL;
    }
    if (!(buf = malloc(cap + 1))) {
        close(fd);
        return NULL;
    }
    for (;;) {
        ssize_t n;

        if (len == cap) {
            char *more;

            if (cap >= max || !(more = realloc(buf, cap * 2 + 1))) {
                break;
            }
            buf = more;
            cap *= 2;
        }
        n = read(fd, buf + len, cap - len);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        len += (size_t)n;
    }
    close(fd);
    buf[len] = '\0';
    return buf;
}

int sys_vfio_expiry_ms(void)
{
    return VFIO_EXPIRY_MS;
}
