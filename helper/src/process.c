/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The caller's QEMU processes: watching them (pidfd), making their threads
 * real-time, and the file capability on vitrine's own QEMU build.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "helper.h"

uid_t caller_uid;
struct watched watched[MAX_WATCHED];
int nwatched;

/* The largest pid Linux hands out (PID_MAX_LIMIT on 64-bit) */
#define PID_LIMIT 4194304

static int pidfd_open_(pid_t pid)
{
    return (int)syscall(SYS_pidfd_open, pid, 0);
}

/* The process @pidfd refers to has not exited (nor been reaped) */
static bool pidfd_alive(int pidfd)
{
    return syscall(SYS_pidfd_send_signal, pidfd, 0, NULL, 0) == 0;
}

/* All four uids of the process (real, effective, saved, file system) are @uid */
static bool owned_by(int procfd, uid_t uid)
{
    char buf[4096];
    unsigned long r, e, s, f;
    const char *line;
    ssize_t n;
    int fd = openat(procfd, "status", O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
        return false;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    line = strstr(buf, "\nUid:");
    return line && sscanf(line + 5, "%lu %lu %lu %lu", &r, &e, &s, &f) == 4 && r == uid &&
           e == uid && s == uid && f == uid;
}

/* Its start time, field 22 of /proc/<pid>/stat: with the pid, it names the
   process for good.  False for a process that has ended (a zombie). */
static bool start_time(int procfd, unsigned long long *start)
{
    char state;
    char buf[1024];
    const char *end;
    ssize_t n;
    int fd = openat(procfd, "stat", O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
        return false;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return false;
    }
    buf[n] = '\0';
    /* after the command's ")", which may hold spaces and parentheses: the
       state is field 3, the start time field 22 */
    if (!(end = strrchr(buf, ')'))) {
        return false;
    }
    return sscanf(end + 1, " %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d "
                           "%*d %*d %*d %llu", &state, start) == 2 && state != 'Z' && state != 'X';
}

/* The basename is qemu-system-<arch> (or RHEL's qemu-kvm); @exe gets the
   executable's path */
static bool runs_qemu(int procfd, char *exe, size_t size)
{
    static const char deleted[] = " (deleted)";
    const char *base, *arch;
    ssize_t n = readlinkat(procfd, "exe", exe, size - 1);
    size_t len;

    if (n <= 0) {
        return false;
    }
    exe[n] = '\0';
    /* rebuilt while it runs: the file it runs is gone */
    len = (size_t)n;
    if (len > sizeof(deleted) - 1 && strcmp(exe + len - (sizeof(deleted) - 1), deleted) == 0) {
        exe[len - (sizeof(deleted) - 1)] = '\0';
    }
    base = strrchr(exe, '/') ? strrchr(exe, '/') + 1 : exe;
    if (strcmp(base, "qemu-kvm") == 0) {
        return true;
    }
    if (strncmp(base, "qemu-system-", 12) != 0 || !base[12]) {
        return false;
    }
    for (arch = base + 12; *arch; arch++) {
        if (!((*arch >= 'a' && *arch <= 'z') || (*arch >= '0' && *arch <= '9') || *arch == '_')) {
            return false;
        }
    }
    return true;
}

struct watched *find_watched(const char *arg)
{
    unsigned long long pid;

    if (!parse_uint(arg, PID_LIMIT, &pid)) {
        return NULL;
    }
    for (int i = 0; i < nwatched; i++) {
        if (watched[i].pid == (pid_t)pid) {
            return &watched[i];
        }
    }
    return NULL;
}

bool watch(const char *arg)
{
    unsigned long long v, start;
    char path[64], exe[PATH_MAX];
    int pidfd, procfd;
    pid_t pid;

    if (!parse_uint(arg, PID_LIMIT, &v) || v < 2) {
        reply("error watch: give a process id");
        return false;
    }
    pid = (pid_t)v;
    if (find_watched(arg)) {
        reply("ok watch %d: already", (int)pid);
        return true;
    }
    if (nwatched == MAX_WATCHED) {
        reply("error watch %d: too many processes watched", (int)pid);
        return false;
    }
    if ((pidfd = pidfd_open_(pid)) < 0) {
        reply("error watch %d: %s", (int)pid, strerror(errno));
        return false;
    }
    snprintf(path, sizeof(path), "/proc/%d", (int)pid);
    procfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (procfd < 0) {
        reply("error watch %d: %s", (int)pid, strerror(errno));
        close(pidfd);
        return false;
    }
    /* checked through /proc/<pid>, then the pidfd tells that this is still
       the process the pidfd was opened on: pid reuse cannot slip in */
    if (!owned_by(procfd, caller_uid)) {
        reply("error watch %d: not a process of uid %u", (int)pid, (unsigned)caller_uid);
    } else if (!runs_qemu(procfd, exe, sizeof(exe))) {
        reply("error watch %d: not a QEMU (qemu-system-*, qemu-kvm)", (int)pid);
    } else if (!start_time(procfd, &start) || !pidfd_alive(pidfd)) {
        reply("error watch %d: it has exited", (int)pid);
    } else {
        watched[nwatched++] = (struct watched){.pid = pid, .pidfd = pidfd, .procfd = procfd,
                                               .start = start};
        reply("ok watch %d", (int)pid);
        return true;
    }
    close(procfd);
    close(pidfd);
    return false;
}

void unwatch(int i)
{
    /* its record goes: nothing left to put back in a process that ended */
    sched_drop(&watched[i]);
    close(watched[i].pidfd);
    close(watched[i].procfd);
    watched[i] = watched[--nwatched];
}

/*
 * Calls @fn on each thread of the process of /proc/<pid> @procfd until a
 * pass changes nothing: a thread started during a pass by a thread not yet
 * changed is caught by the next.  Returns the number of passes, or -errno
 * when the process is gone.
 */
static int each_thread(int procfd, bool (*fn)(pid_t tid, int taskfd, const char *name,
                                               int *counts),
                       int *counts)
{
    for (int pass = 1; pass <= 8; pass++) {
        int taskfd = openat(procfd, "task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        bool changed = false;
        struct dirent *e;
        DIR *d;

        if (taskfd < 0 || !(d = fdopendir(taskfd))) {
            int err = errno;
            if (taskfd >= 0) {
                close(taskfd);
            }
            return -err;
        }
        while ((e = readdir(d))) {
            unsigned long long tid;

            if (parse_uint(e->d_name, PID_LIMIT, &tid)) {
                changed |= fn((pid_t)tid, dirfd(d), e->d_name, counts);
            }
        }
        closedir(d);
        if (!changed) {
            return pass;
        }
    }
    return 8;
}

/*
 * Sets one thread's policy, or its nice value (@policy < 0).  The thread's
 * /proc folder, opened first, stays bound to that thread: if it can still
 * be read after the change, the tid was that thread's all along.  If the
 * thread ended in between, its tid cannot have gone to another task yet:
 * Linux hands pids out in turn up to pid_max (4194304 here) before it
 * reuses one.
 */
static int set_thread(pid_t tid, int taskfd, const char *name, int policy, int value)
{
    int tidfd = openat(taskfd, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int err;

    if (tidfd < 0) {
        return -ESRCH;
    }
    err = policy < 0 ? sys_setnice(tid, value) : sys_setsched(tid, policy, value);
    if (!err && faccessat(tidfd, "stat", F_OK, 0) < 0) {
        err = -ESRCH;
    }
    close(tidfd);
    return err;
}

/* A vCPU thread: QEMU names them "CPU <n>/KVM" with debug-threads=on (and
   without it, every thread has the process's name) */
static bool is_vcpu(int taskfd, const char *name)
{
    char path[64], comm[32], accel[16];
    unsigned n;
    ssize_t len;
    int fd;

    snprintf(path, sizeof(path), "%s/comm", name);
    if ((fd = openat(taskfd, path, O_RDONLY | O_CLOEXEC)) < 0) {
        return false;
    }
    len = read(fd, comm, sizeof(comm) - 1);
    close(fd);
    if (len <= 0) {
        return false;
    }
    comm[len] = '\0';
    return sscanf(comm, "CPU %u/%15s", &n, accel) == 2;
}

/* counts: [0] made real-time, [1] failed, [2] last error */
static bool rt_thread(pid_t tid, int taskfd, const char *name, int *counts)
{
    int policy, priority, err;

    if (sys_getsched(tid, &policy, &priority) < 0) {
        return false;
    }
    /* real-time or deadline already (QEMU's or a library's own choice): kept */
    if (policy == SCHED_FIFO || policy == SCHED_RR || policy == SCHED_DEADLINE) {
        return false;
    }
    err = set_thread(tid, taskfd, name, SCHED_FIFO, 1);
    if (err == 0) {
        counts[0]++;
        return true;
    }
    if (err != -ESRCH) {
        counts[1]++;
        counts[2] = -err;
    }
    return false;
}

static bool count_thread(pid_t tid, int taskfd, const char *name, int *counts)
{
    int policy, priority;

    (void)taskfd;
    (void)name;
    if (sys_getsched(tid, &policy, &priority) == 0) {
        counts[0]++;
        counts[1] += policy == SCHED_FIFO || policy == SCHED_RR || policy == SCHED_DEADLINE;
    }
    return false;
}

/* counts: [0] back to SCHED_OTHER; only what rt set: FIFO 1 */
static bool other_thread(pid_t tid, int taskfd, const char *name, int *counts)
{
    int policy, priority;

    if (sys_getsched(tid, &policy, &priority) == 0 && policy == SCHED_FIFO && priority == 1 &&
        set_thread(tid, taskfd, name, SCHED_OTHER, 0) == 0) {
        counts[0]++;
        return true;
    }
    return false;
}

/* counts: [0] vCPUs back from BEHIND_NICE to 0 */
static bool unnice_vcpu(pid_t tid, int taskfd, const char *name, int *counts)
{
    int nice;

    if (is_vcpu(taskfd, name) && sys_getnice(tid, &nice) == 0 && nice == BEHIND_NICE &&
        set_thread(tid, taskfd, name, -1, 0) == 0) {
        counts[0]++;
        return true;
    }
    return false;
}

/* Still the caller's QEMU: it may have exec'ed something else since watch,
   a setuid program for one */
static bool still_qemu(struct watched *w)
{
    char exe[PATH_MAX];

    return owned_by(w->procfd, caller_uid) && runs_qemu(w->procfd, exe, sizeof(exe)) &&
           pidfd_alive(w->pidfd);
}

/*
 * SCHED_FIFO 1 on every thread: the VM in front.  Threads started later
 * inherit it: Linux gives a new thread its creator's policy, and
 * pthread_create (QEMU's threads, GLib's, PipeWire's) inherits by default.
 * So no polling: once a pass finds every thread real-time, every thread to
 * come will be.  Only the QEMU's own threads: the processes it started
 * (passt, at its start, before any of this) keep theirs.  And only with the
 * fair server's bound in place, without which a real-time vCPU that spins
 * keeps kernel workers off its CPU for up to 950 ms.
 */
void rt_on(const char *arg)
{
    struct watched *w = find_watched(arg);
    int counts[3] = {0, 0, 0}, total[2] = {0, 0};
    const char *why;
    int passes;

    if (!w) {
        reply("error rt: watch the process first");
        return;
    }
    if (!still_qemu(w)) {
        reply("error rt %d: no longer a QEMU of uid %u", (int)w->pid, (unsigned)caller_uid);
        return;
    }
    if (!settings_bound(&why)) {
        reply("skip rt %d: the fair server is not set: %s", (int)w->pid, why);
        return;
    }
    /* recorded first: a helper that dies leaves them to the next one */
    w->rt = true;
    if (!sched_hold(w)) {
        w->rt = false;
        reply("error rt %d: cannot record it: %s", (int)w->pid, strerror(errno));
        return;
    }
    passes = each_thread(w->procfd, rt_thread, counts);
    if (passes < 0) {
        reply("error rt %d: %s", (int)w->pid, strerror(-passes));
        return;
    }
    each_thread(w->procfd, count_thread, total);
    if (counts[1] && total[1] < total[0]) {
        reply("error rt %d: %d of %d threads real-time: %s", (int)w->pid, total[1], total[0],
              strerror(counts[2]));
    } else {
        reply("ok rt %d: %d of %d threads real-time", (int)w->pid, total[1], total[0]);
    }
    if (counts[0]) {
        sys_log("uid %u: SCHED_FIFO 1 on %d threads of pid %d", (unsigned)caller_uid, counts[0],
                (int)w->pid);
    }
}

void sched_restore(pid_t pid, unsigned long long start, bool niced, bool say)
{
    unsigned long long now;
    int others[1] = {0}, unniced[1] = {0};
    char path[64];
    int procfd;

    snprintf(path, sizeof(path), "/proc/%d", (int)pid);
    /* that process still, not one that got its pid since: the folder stays
       bound to the process it was opened for */
    if ((procfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
        return;
    }
    if (!start_time(procfd, &now) || now != start) {
        close(procfd);
        return;
    }
    each_thread(procfd, other_thread, others);
    if (niced) {
        each_thread(procfd, unnice_vcpu, unniced);
    }
    close(procfd);
    if (say) {
        reply("restored rt %d: %d threads back to SCHED_OTHER%s", (int)pid, others[0],
              unniced[0] ? ", vCPUs back to nice 0" : "");
    }
    if (others[0] || unniced[0]) {
        sys_log("pid %d: %d threads back to SCHED_OTHER, %d vCPUs back to nice 0", (int)pid,
                others[0], unniced[0]);
    }
}

void rt_off_all(void)
{
    for (int i = 0; i < nwatched; i++) {
        /* the last holder of its record puts the threads back */
        sched_drop(&watched[i]);
        watched[i].rt = watched[i].behind = false;
    }
}

/* --- setcap --- */

/* A file name: not empty, not "." nor ".." */
static bool plain_name(const char *s)
{
    return s[0] && strcmp(s, ".") != 0 && strcmp(s, "..") != 0;
}

/* The ELF machine of the binaries this host runs */
#if defined(__x86_64__)
#define HOST_EM EM_X86_64
#elif defined(__aarch64__)
#define HOST_EM EM_AARCH64
#elif defined(__riscv) && __riscv_xlen == 64
#define HOST_EM EM_RISCV
#elif defined(__powerpc64__)
#define HOST_EM EM_PPC64
#elif defined(__s390x__)
#define HOST_EM EM_S390
#elif defined(__loongarch64)
#define HOST_EM EM_LOONGARCH
#else
#error "unknown host architecture"
#endif

static int fail_setcap(const char *path, const char *why)
{
    reply("error setcap %s: %s", path, why);
    return 1;
}

/*
 * cap_sys_nice=ep on vitrine's own QEMU build, so that QEMU may make its
 * vCPU threads real-time (x-vcpu-priority) and create high-priority amdgpu
 * contexts.  The caller builds that QEMU, so its content is the caller's
 * choice whatever is checked here: what the group grants is CAP_SYS_NICE
 * for a program of the caller's.  The checks make sure it stays the
 * caller's own, private file:
 *  - an absolute path without links, ".", ".." or "//", inside the caller's
 *    home (from the user database, not the environment) and shaped like the
 *    stack's layout: .../vitrine/stack/<build>/bin/qemu-system-<arch>;
 *  - walked one component at a time without following links, each folder
 *    from the home down owned by the caller or root and writable by no one
 *    else (group write only for the caller's own primary group), so the file
 *    checked is the file changed;
 *  - a regular file of the caller's, one link, no setuid/setgid bit, an ELF
 *    executable for this machine, on a file system that honours file
 *    capabilities (not nosuid);
 *  - made 0700 before the capability is set: other users cannot run it.
 * Writing to the file afterwards drops the capability (the kernel does), so
 * every build needs this again.
 */
int setcap(const char *path)
{
    static const char *tail[] = {"vitrine", "stack", NULL, "bin"};
    char copy[PATH_MAX], home[PATH_MAX], proc[64];
    const char *comps[PATH_MAX / 2];
    struct stat st, st2;
    struct statvfs vfs;
    struct passwd *pw;
    Elf64_Ehdr eh;
    int ncomps = 0, nhome = 0, fd = -1, rfd, err;
    size_t hlen;
    char *p, *save = NULL;

    if (path[0] != '/' || strlen(path) >= sizeof(copy)) {
        return fail_setcap(path, "give an absolute path");
    }
    if (!(pw = getpwuid(caller_uid)) || pw->pw_dir[0] != '/' ||
        strlen(pw->pw_dir) >= sizeof(home)) {
        return fail_setcap(path, "the caller has no home folder");
    }
    snprintf(home, sizeof(home), "%s", pw->pw_dir);
    hlen = strlen(home);
    while (hlen > 1 && home[hlen - 1] == '/') {
        home[--hlen] = '\0';
    }
    if (strncmp(path, home, hlen) != 0 || path[hlen] != '/') {
        return fail_setcap(path, "not in the caller's home folder");
    }
    /* the components, each a plain name */
    snprintf(copy, sizeof(copy), "%s", path);
    if (strstr(copy, "//") || copy[strlen(copy) - 1] == '/') {
        return fail_setcap(path, "not a plain path");
    }
    for (p = strtok_r(copy, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if (!plain_name(p)) {
            return fail_setcap(path, "not a plain path");
        }
        comps[ncomps++] = p;
    }
    for (const char *h = home; *h; h++) {
        nhome += *h == '/';
    }
    /* .../vitrine/stack/<build>/bin/qemu-system-<arch> */
    if (ncomps < nhome + 5) {
        return fail_setcap(path, "not a QEMU of vitrine's stack");
    }
    for (int i = 0; i < 4; i++) {
        if (tail[i] && strcmp(comps[ncomps - 5 + i], tail[i]) != 0) {
            return fail_setcap(path, "not a QEMU of vitrine's stack");
        }
    }
    {
        const char *base = comps[ncomps - 1];
        bool ok = strncmp(base, "qemu-system-", 12) == 0 && base[12];

        for (const char *a = base + 12; ok && *a; a++) {
            ok = (*a >= 'a' && *a <= 'z') || (*a >= '0' && *a <= '9') || *a == '_';
        }
        if (!ok) {
            return fail_setcap(path, "not a QEMU of vitrine's stack");
        }
    }

    /* the walk: no links followed, nothing writable by others from the home down */
    if ((fd = open("/", O_PATH | O_DIRECTORY | O_CLOEXEC)) < 0) {
        return fail_setcap(path, strerror(errno));
    }
    for (int i = 0; i < ncomps; i++) {
        bool last = i == ncomps - 1;
        int next = openat(fd, comps[i], O_PATH | O_NOFOLLOW | O_CLOEXEC | (last ? 0 : O_DIRECTORY));

        err = errno;
        close(fd);
        if (next < 0) {
            return fail_setcap(path, err == ELOOP || err == ENOTDIR
                                         ? "a link in the path: give the real path"
                                         : strerror(err));
        }
        fd = next;
        if (fstat(fd, &st) < 0) {
            err = errno;
            close(fd);
            return fail_setcap(path, strerror(err));
        }
        if (!last && i >= nhome - 1 &&
            ((st.st_uid != caller_uid && st.st_uid != 0) || (st.st_mode & S_IWOTH) ||
             ((st.st_mode & S_IWGRP) && st.st_gid != pw->pw_gid))) {
            close(fd);
            return fail_setcap(path, "a folder on the way is writable by others");
        }
    }
    if (S_ISLNK(st.st_mode)) {
        close(fd);
        return fail_setcap(path, "a link in the path: give the real path");
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != caller_uid) {
        close(fd);
        return fail_setcap(path, "not a file of the caller's");
    }
    if (st.st_nlink != 1 || (st.st_mode & (S_ISUID | S_ISGID))) {
        close(fd);
        return fail_setcap(path, "a file with other links or a setuid/setgid bit");
    }
    if (fstatvfs(fd, &vfs) == 0 && (vfs.f_flag & ST_NOSUID)) {
        close(fd);
        return fail_setcap(path, "on a nosuid mount, where file capabilities are ignored");
    }
    /* the very file checked, opened for real through its O_PATH descriptor */
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    rfd = open(proc, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    err = errno;
    close(fd);
    if (rfd < 0) {
        return fail_setcap(path, strerror(err));
    }
    if (fstat(rfd, &st2) < 0 || st2.st_dev != st.st_dev || st2.st_ino != st.st_ino) {
        close(rfd);
        return fail_setcap(path, "the file changed while checked");
    }
    if (pread(rfd, &eh, sizeof(eh), 0) != (ssize_t)sizeof(eh) ||
        memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        (eh.e_type != ET_EXEC && eh.e_type != ET_DYN) || eh.e_machine != HOST_EM) {
        close(rfd);
        return fail_setcap(path, "not an executable for this machine");
    }
    if (fchmod(rfd, 0700) < 0 || (err = sys_set_file_cap(rfd, path)) < 0) {
        err = err < 0 ? -err : errno;
        close(rfd);
        return fail_setcap(path, strerror(err));
    }
    close(rfd);
    sys_log("uid %u: cap_sys_nice=ep on %s", (unsigned)caller_uid, path);
    reply("ok setcap %s: cap_sys_nice=ep, mode 0700", path);
    return 0;
}
