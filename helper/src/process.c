/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The caller's QEMU processes: watching them (pidfd), and their threads'
 * scheduling - real-time for the VM in front, ordinary for those behind -
 * set here, as root, so that QEMU needs no capability of its own.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
 * What a pass over a process's threads works with: the threads to leave as
 * they are (real-time before this helper's first rt: QEMU's or a library's
 * own choice), its counts, and the threads it collects
 */
struct pass {
    const pid_t *kept;
    int nkept;
    int counts[3];
    pid_t *found;
    int nfound, maxfound;
};

static bool kept(const struct pass *p, pid_t tid)
{
    for (int i = 0; i < p->nkept; i++) {
        if (p->kept[i] == tid) {
            return true;
        }
    }
    return false;
}

/*
 * Calls @fn on each thread of the process of /proc/<pid> @procfd until a
 * pass changes nothing: a thread started during a pass by a thread not yet
 * changed is caught by the next.  Returns the number of passes, or -errno
 * when the process is gone.
 */
static int each_thread(int procfd, bool (*fn)(pid_t tid, int taskfd, const char *name,
                                               struct pass *p),
                       struct pass *p)
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
                changed |= fn((pid_t)tid, dirfd(d), e->d_name, p);
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

/* The threads at SCHED_FIFO 1 now, into p->found: before this helper's
   first rt, they are not its doing */
static bool find_fifo1(pid_t tid, int taskfd, const char *name, struct pass *p)
{
    int policy, priority;

    (void)taskfd;
    (void)name;
    if (p->nfound < p->maxfound && sys_getsched(tid, &policy, &priority) == 0 &&
        policy == SCHED_FIFO && priority == 1) {
        p->found[p->nfound++] = tid;
    }
    return false;
}

/* counts: [0] made real-time, [1] failed, [2] last error */
static bool rt_thread(pid_t tid, int taskfd, const char *name, struct pass *p)
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
        p->counts[0]++;
        return true;
    }
    if (err != -ESRCH) {
        p->counts[1]++;
        p->counts[2] = -err;
    }
    return false;
}

/* counts: [0] threads, [1] real-time ones */
static bool count_thread(pid_t tid, int taskfd, const char *name, struct pass *p)
{
    int policy, priority;

    (void)taskfd;
    (void)name;
    if (sys_getsched(tid, &policy, &priority) == 0) {
        p->counts[0]++;
        p->counts[1] += policy == SCHED_FIFO || policy == SCHED_RR || policy == SCHED_DEADLINE;
    }
    return false;
}

/* counts: [0] back to SCHED_OTHER; only what rt set: FIFO 1, not kept */
static bool other_thread(pid_t tid, int taskfd, const char *name, struct pass *p)
{
    int policy, priority;

    if (!kept(p, tid) && sys_getsched(tid, &policy, &priority) == 0 && policy == SCHED_FIFO &&
        priority == 1 && set_thread(tid, taskfd, name, SCHED_OTHER, 0) == 0) {
        p->counts[0]++;
        return true;
    }
    return false;
}

/* counts: [0] vCPUs at BEHIND_NICE now */
static bool behind_vcpu(pid_t tid, int taskfd, const char *name, struct pass *p)
{
    int nice;

    if (!is_vcpu(taskfd, name) || sys_getnice(tid, &nice) < 0 || nice == BEHIND_NICE) {
        return false;
    }
    /* a nice the user or QEMU chose (other than the default) stays */
    if (nice == 0 && set_thread(tid, taskfd, name, -1, BEHIND_NICE) == 0) {
        p->counts[0]++;
        return true;
    }
    return false;
}

/* counts: [0] vCPUs back from BEHIND_NICE to 0 */
static bool unnice_vcpu(pid_t tid, int taskfd, const char *name, struct pass *p)
{
    int nice;

    if (is_vcpu(taskfd, name) && sys_getnice(tid, &nice) == 0 && nice == BEHIND_NICE &&
        set_thread(tid, taskfd, name, -1, 0) == 0) {
        p->counts[0]++;
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

/* A limit as the log says it */
static const char *rttime_text(rlim_t v, char *buf, size_t size)
{
    if (v == RLIM_INFINITY) {
        return "none";
    }
    snprintf(buf, size, "%llu us", (unsigned long long)v);
    return buf;
}

/*
 * No real-time time limit (RLIMIT_RTTIME) on the process, before any of its
 * threads is made real-time: with one, the kernel SIGKILLs the whole process
 * once a real-time thread of it runs that long without sleeping - a vCPU
 * does whenever the guest is busy - and QEMU ends without a word.  Libraries
 * set one for a real-time thread of their own: PipeWire's module-rt, for
 * its audio thread when it gets real-time from RTKit or the realtime portal
 * (QEMU has no CAP_SYS_NICE), sets RTKit's RTTimeUSecMax, 200 ms, for all of
 * QEMU.  What keeps real-time threads from starving the host is the fair
 * server's bound.  Lifted for the process's lifetime, never put back: the
 * real-time threads kept (a library's own) then run as they did in a QEMU
 * with CAP_SYS_NICE, where PipeWire sets no limit.  Read back.  0 or -errno.
 */
static int rttime_lift(struct watched *w)
{
    char was_soft[32], was_hard[32];
    rlim_t soft, hard, s, h;
    int err;

    if ((err = sys_getrttime(w->pid, &soft, &hard)) < 0) {
        return err;
    }
    if (soft == RLIM_INFINITY && hard == RLIM_INFINITY) {
        return 0;
    }
    if ((err = sys_setrttime(w->pid, RLIM_INFINITY, RLIM_INFINITY)) < 0 ||
        (err = sys_getrttime(w->pid, &s, &h)) < 0) {
        return err;
    }
    if (s != RLIM_INFINITY || h != RLIM_INFINITY) {
        return -EIO;
    }
    sys_log("uid %u: real-time time limit of pid %d lifted (soft %s, hard %s): its threads "
            "are made real-time",
            (unsigned)caller_uid, (int)w->pid, rttime_text(soft, was_soft, sizeof(was_soft)),
            rttime_text(hard, was_hard, sizeof(was_hard)));
    return 0;
}

/*
 * SCHED_FIFO 1 on every thread: the VM in front.  Threads started later
 * inherit it: Linux gives a new thread its creator's policy, and
 * pthread_create (QEMU's threads, GLib's, PipeWire's) inherits by default.
 * So no polling: once a pass finds every thread real-time, every thread to
 * come will be.  Only the QEMU's own threads: the processes it started
 * (passt, at its start, before any of this) keep theirs; and threads
 * real-time before (QEMU's or a library's choice) stay as they are, now
 * and when put back.  And only with the fair server's bound in place,
 * without which a real-time vCPU that spins keeps kernel workers off its
 * CPU for up to 950 ms, and without a real-time time limit on the process
 * (rttime_lift), which would end QEMU the first time the guest is busy.
 */
void rt_on(const char *arg)
{
    struct watched *w = find_watched(arg);
    struct pass changes = {0}, total = {0};
    const char *why;
    int passes, err;

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
    if ((err = rttime_lift(w)) < 0) {
        reply("error rt %d: its real-time time limit cannot be lifted, which would end it: %s",
              (int)w->pid, strerror(-err));
        return;
    }
    if (!w->rt_once) {
        struct pass found = {.found = w->kept, .maxfound = MAX_KEPT};
        bool niced = false;

        /*
         * Another helper's record of it (vitrine restarted while it runs):
         * what was real-time before is what that one kept - the rest of
         * its real-time threads are that helper's rt, to be put back.
         * Else, before any change of this helper's: what is real-time
         * already (FIFO 1, the priority rt sets and puts back) is not its
         * own.
         */
        if (sched_record(w, w->kept, &w->nkept, &niced)) {
            w->behind |= niced;
        } else {
            each_thread(w->procfd, find_fifo1, &found);
            w->nkept = found.nfound;
        }
    }
    /* recorded first: a helper that dies leaves them to the next one */
    w->rt = true;
    if (!sched_hold(w)) {
        w->rt = false;
        reply("error rt %d: cannot record it: %s", (int)w->pid, strerror(errno));
        return;
    }
    w->rt_once = true;
    passes = each_thread(w->procfd, rt_thread, &changes);
    if (passes < 0) {
        reply("error rt %d: %s", (int)w->pid, strerror(-passes));
        return;
    }
    each_thread(w->procfd, count_thread, &total);
    if (changes.counts[1] && total.counts[1] < total.counts[0]) {
        reply("error rt %d: %d of %d threads real-time: %s", (int)w->pid, total.counts[1],
              total.counts[0], strerror(changes.counts[2]));
    } else {
        reply("ok rt %d: %d of %d threads real-time", (int)w->pid, total.counts[1],
              total.counts[0]);
    }
    if (changes.counts[0]) {
        sys_log("uid %u: SCHED_FIFO 1 on %d threads of pid %d", (unsigned)caller_uid,
                changes.counts[0], (int)w->pid);
    }
}

/*
 * A VM behind the one in front: the threads rt made real-time back to
 * SCHED_OTHER - every one of them, not only the vCPUs, so that VMs busy in
 * the background cannot take every CPU at real-time priority - and, while
 * real-time threads run (the bound in place), its vCPUs at nice
 * BEHIND_NICE, a little ahead of ordinary tasks still.  The nice needs a
 * record (it is put back at the end); taking privilege away needs none.
 */
void behind(const char *arg)
{
    struct watched *w = find_watched(arg);
    struct pass others = {.kept = NULL}, niced = {0};
    const char *why;
    bool nice = false;

    if (!w) {
        reply("error behind: watch the process first");
        return;
    }
    if (!still_qemu(w)) {
        reply("error behind %d: no longer a QEMU of uid %u", (int)w->pid, (unsigned)caller_uid);
        return;
    }
    w->rt = false;
    /*
     * Another helper's record of it (vitrine restarted while it runs, the
     * old helper going on): the threads that one made real-time are known,
     * and go back as if this one had - else a VM that was in front stayed
     * real-time beside the one in front now
     */
    if (!w->rt_once) {
        bool niced = false;

        if (sched_record(w, w->kept, &w->nkept, &niced)) {
            w->rt_once = true;
            w->behind |= niced;
        }
    }
    if (settings_bound(&why)) {
        const bool was = w->behind;

        w->behind = true;
        nice = sched_hold(w);
        w->behind = was || nice;
    } else if (w->held) {
        sched_hold(w);
    }
    /* only what this helper's rt made real-time: none before its first */
    others.kept = w->kept;
    others.nkept = w->nkept;
    if (w->rt_once) {
        each_thread(w->procfd, other_thread, &others);
    }
    if (nice) {
        each_thread(w->procfd, behind_vcpu, &niced);
    }
    reply("ok behind %d: %d threads ordinary, %d vCPUs at nice %d", (int)w->pid,
          others.counts[0], niced.counts[0], BEHIND_NICE);
    /* the VM in front gone behind: its view's threads with it */
    view_check();
}

void sched_restore(pid_t pid, unsigned long long start, bool niced, const pid_t *keep,
                   int nkeep, const char *verb)
{
    struct pass others = {.kept = keep, .nkept = nkeep}, unniced = {0};
    unsigned long long now;
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
    each_thread(procfd, other_thread, &others);
    if (niced) {
        each_thread(procfd, unnice_vcpu, &unniced);
    }
    close(procfd);
    /* a QEMU's always; the view's when there were some (view_check() put
       them back already as no VM was in front) */
    if (verb && (strcmp(verb, "rt") == 0 || others.counts[0] || unniced.counts[0])) {
        reply("restored %s %d: %d threads back to SCHED_OTHER%s", verb, (int)pid,
              others.counts[0], unniced.counts[0] ? ", vCPUs back to nice 0" : "");
    }
    if (others.counts[0] || unniced.counts[0]) {
        sys_log("pid %d: %d threads back to SCHED_OTHER, %d vCPUs back to nice 0", (int)pid,
                others.counts[0], unniced.counts[0]);
    }
}

/*
 * The process that started this helper (parent_init()): the threads view-rt
 * made real-time are recorded as a watched QEMU's are (sched-<pid>, kept[]
 * the threads at SCHED_FIFO 1 before its first change), but it is not
 * watched: its end ends nothing.
 */
static struct watched parent = {.pidfd = -1, .procfd = -1, .view = true};
/* why there is none, while parent.procfd < 0 */
static const char *parent_why = "the process that started the helper is not known";

void rt_off_all(void)
{
    for (int i = 0; i < nwatched; i++) {
        /* the last holder of its record puts the threads back */
        sched_drop(&watched[i]);
        watched[i].rt = watched[i].behind = false;
    }
    /* no VM in front, so no view either; then its record goes */
    view_check();
    sched_drop(&parent);
}

void parent_init(void)
{
    const pid_t ppid = getppid();
    unsigned long long start;
    char path[64];

    /* 1: it ended already, init (or a subreaper) took the helper over */
    if (ppid <= 1) {
        parent_why = "the process that started the helper has ended";
        return;
    }
    parent.pidfd = pidfd_open_(ppid);
    snprintf(path, sizeof(path), "/proc/%d", (int)ppid);
    parent.procfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    /* still the parent once both are open: they stay bound to that process
       (one that got the pid of a parent ended before is not the parent) */
    if (parent.pidfd < 0 || parent.procfd < 0 || getppid() != ppid ||
        !start_time(parent.procfd, &start) || !pidfd_alive(parent.pidfd)) {
        if (parent.pidfd >= 0) {
            close(parent.pidfd);
        }
        if (parent.procfd >= 0) {
            close(parent.procfd);
        }
        parent.pidfd = parent.procfd = -1;
        parent_why = "the process that started the helper has ended";
        return;
    }
    parent.pid = ppid;
    parent.start = start;
}

/* That process can have threads made real-time: it runs, all four uids
   the caller's; else why not */
static bool parent_usable(const char **why)
{
    static char text[96];

    if (parent.procfd < 0) {
        *why = parent_why;
        return false;
    }
    if (!pidfd_alive(parent.pidfd)) {
        *why = "the process that started the helper has ended";
        return false;
    }
    /* sudo, say: root's */
    if (!owned_by(parent.procfd, caller_uid)) {
        snprintf(text, sizeof(text), "pid %d, which started the helper, is not a process of uid %u",
                 (int)parent.pid, (unsigned)caller_uid);
        *why = text;
        return false;
    }
    return true;
}

/* A watched QEMU is in front: its rt was asked for */
static bool any_front(void)
{
    for (int i = 0; i < nwatched; i++) {
        if (watched[i].rt) {
            return true;
        }
    }
    return false;
}

/* The threads of that process at SCHED_FIFO 1 - those view-rt made so and
   those they started since - but the ones that were before its first
   view-rt, back to SCHED_OTHER: how many */
static int view_lower(void)
{
    struct pass others = {.kept = parent.kept, .nkept = parent.nkept};

    if (!parent.rt) {
        return 0;
    }
    parent.rt = false;
    if (pidfd_alive(parent.pidfd)) {
        each_thread(parent.procfd, other_thread, &others);
    }
    if (others.counts[0]) {
        sys_log("pid %d: %d threads of its VM view back to SCHED_OTHER", (int)parent.pid,
                others.counts[0]);
    }
    return others.counts[0];
}

/*
 * SCHED_FIFO 1 on one thread of the process that started the helper - in
 * vitrine, a thread of its view of the VM in front: the render thread, its
 * Wayland reader, the D-Bus listener, GLib's D-Bus worker - as that VM's
 * QEMU threads are.  The threads it starts inherit it.  Only a thread of
 * that process, all of whose uids are the caller's, only with the fair
 * server's bound in place and a watched QEMU in front (its rt), and back to
 * SCHED_OTHER as soon as none is (view_check()).  Threads at SCHED_FIFO 1
 * before the first view-rt are not this helper's doing: kept as they are,
 * now and when put back; real-time ones are left alone (RTKit's are
 * SCHED_RR), as are SCHED_IDLE ones.
 */
void view_rt(const char *arg)
{
    unsigned long long tid;
    const bool was = parent.rt;
    char name[24];
    const char *why;
    int taskfd, policy, priority, err;

    if (!parse_uint(arg, PID_LIMIT, &tid) || tid < 1) {
        reply("error view-rt: give a thread id");
        return;
    }
    if (!parent_usable(&why)) {
        reply("error view-rt %llu: %s", tid, why);
        return;
    }
    if (!settings_bound(&why)) {
        reply("skip view-rt %llu: the fair server is not set: %s", tid, why);
        return;
    }
    if (!any_front()) {
        reply("skip view-rt %llu: no VM in front", tid);
        return;
    }
    /* one of its threads: its task folder lists those only */
    snprintf(name, sizeof(name), "%llu", tid);
    taskfd = openat(parent.procfd, "task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (taskfd < 0 || faccessat(taskfd, name, F_OK, 0) < 0) {
        reply("error view-rt %llu: not a thread of pid %d", tid, (int)parent.pid);
        if (taskfd >= 0) {
            close(taskfd);
        }
        return;
    }
    if (!parent.rt_once) {
        /* before any change of this helper's: what runs at FIFO 1 already
           (the priority it sets and puts back) is not its own */
        struct pass found = {.found = parent.kept, .maxfound = MAX_KEPT};

        each_thread(parent.procfd, find_fifo1, &found);
        parent.nkept = found.nfound;
    }
    if (parent.nkept == MAX_KEPT) {
        /* kept[] is full, and may have left some out: a put-back would
           take them too */
        reply("skip view-rt %llu: pid %d runs real-time already (%d threads or more at "
              "SCHED_FIFO 1)", tid, (int)parent.pid, MAX_KEPT);
        close(taskfd);
        return;
    }
    if ((err = sys_getsched((pid_t)tid, &policy, &priority)) < 0) {
        reply("error view-rt %llu: %s", tid, strerror(-err));
        close(taskfd);
        return;
    }
    if (policy == SCHED_FIFO || policy == SCHED_RR || policy == SCHED_DEADLINE) {
        /* this helper's (a thread started by one it made real-time), or
           another's choice: as it is */
        reply("ok view-rt %llu: real-time already", tid);
        close(taskfd);
        return;
    }
    if (policy == SCHED_IDLE) {
        reply("skip view-rt %llu: SCHED_IDLE, its own choice", tid);
        close(taskfd);
        return;
    }
    /* recorded first: a helper that dies leaves them to the next one */
    parent.rt = true;
    if (!sched_hold(&parent)) {
        parent.rt = was;
        reply("error view-rt %llu: cannot record it: %s", tid, strerror(errno));
        close(taskfd);
        return;
    }
    parent.rt_once = true;
    err = set_thread((pid_t)tid, taskfd, name, SCHED_FIFO, 1);
    close(taskfd);
    if (err < 0) {
        reply("error view-rt %llu: %s", tid, strerror(-err));
        return;
    }
    reply("ok view-rt %llu: real-time", tid);
    sys_log("uid %u: SCHED_FIFO 1 on thread %llu of pid %d (its view of the VM in front)",
            (unsigned)caller_uid, tid, (int)parent.pid);
}

void view_behind(void)
{
    const int n = view_lower();

    reply("ok view-behind: %d threads ordinary", n);
}

void view_check(void)
{
    int n;

    if (!any_front() && (n = view_lower()) > 0) {
        reply("restored view-rt %d: %d threads back to SCHED_OTHER", (int)parent.pid, n);
    }
}
