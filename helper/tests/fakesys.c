/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The test build's system access (vitrine-helper-fake): the attribute
 * files live in a fake tree under $VITRINE_HELPER_TEST_ROOT, where this
 * file plays the kernel's part - the fair and ext servers' runtime <= period
 * check at each write and their -EBUSY for a CPU that /sys/devices/system/
 * cpu/online leaves out, amdgpu's overdrive table that takes edits in
 * manual only and a commit that fails while the maximum is 0 - so that the
 * order of the writes is tested, not just their values.  A file with a
 * sibling <file>.stuck takes writes without keeping them (for
 * pp_od_clk_voltage, its commits), for the read back; a server's file with
 * <file>.writes N takes N more writes, then refuses them.  Other files (the
 * udmabuf module's parameters) take any value, as the kernel's int
 * parameters do.  Every write, scheduler and nice
 * change and log line is appended to <root>/journal.  Schedulers and nice
 * values are kept in <root>/sched (an unprivileged test cannot make threads
 * real-time nor lower their nice), where the next helper finds them, and
 * real-time time limits in <root>/rttime-<pid> (see sys_getrttime); the
 * processes and /proc are real.  The group database is <root>/etc/group
 * (name:x:gid:members lines); the users are the real ones.  This build
 * refuses to run as root, and the installed helper has no test root at all.
 *
 * VITRINE_HELPER_TEST_KILL_AT=N kills it (SIGKILL) right after its Nth
 * write, as a crash or a kill in the middle of a change would.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <unistd.h>

#include "../src/helper.h"

static char root[256];
static int writes;      /* for VITRINE_HELPER_TEST_KILL_AT */

bool sys_init(void)
{
    const char *r = getenv("VITRINE_HELPER_TEST_ROOT");

    if (geteuid() == 0) {
        fprintf(stderr, "vitrine-helper-fake: the test build does not run as root\n");
        return false;
    }
    if (!r || r[0] != '/' || strlen(r) >= sizeof(root)) {
        fprintf(stderr, "vitrine-helper-fake: VITRINE_HELPER_TEST_ROOT must name the fake tree\n");
        return false;
    }
    snprintf(root, sizeof(root), "%s", r);
    return true;
}

const char *sys_root(void)
{
    return root;
}

static void journal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void journal(const char *fmt, ...)
{
    char path[PATH_MAX];
    va_list ap;
    int fd;

    snprintf(path, sizeof(path), "%s/journal", root);
    fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return;
    }
    va_start(ap, fmt);
    vdprintf(fd, fmt, ap);
    va_end(ap);
    dprintf(fd, "\n");
    close(fd);
}

int sys_read(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t n;

    if (fd < 0) {
        return -errno;
    }
    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) {
        return -errno;
    }
    buf[n] = '\0';
    return (int)n;
}

static int put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_TRUNC | O_CREAT | O_CLOEXEC, 0644);
    size_t len = strlen(text);
    bool ok;

    if (fd < 0) {
        return -errno;
    }
    ok = write(fd, text, len) == (ssize_t)len;
    close(fd);
    return ok ? 0 : -EIO;
}

/* @path's sibling @name */
static void sibling(char *out, size_t size, const char *path, const char *name)
{
    const char *slash = strrchr(path, '/');

    snprintf(out, size, "%.*s/%s", (int)(slash - path), path, name);
}

static bool get_u64(const char *path, unsigned long long *v)
{
    char buf[32];

    if (sys_read(path, buf, sizeof(buf)) <= 0) {
        return false;
    }
    buf[strcspn(buf, "\n")] = '\0';
    return parse_uint(buf, ~0ULL, v);
}

/* The CPU of a server file's path ".../cpuN/period" is offline: the
   fake tree's /sys/devices/system/cpu/online leaves it out */
static bool cpu_offline(const char *path)
{
    char online[PATH_MAX], text[256], *save = NULL;
    const char *c = strstr(path, "/cpu");
    unsigned cpu;

    snprintf(online, sizeof(online), "%s" CPUS_ONLINE, root);
    if (!c || sscanf(c, "/cpu%u/", &cpu) != 1 || sys_read(online, text, sizeof(text)) <= 0) {
        return false;
    }
    for (char *t = strtok_r(text, ",\n", &save); t; t = strtok_r(NULL, ",\n", &save)) {
        unsigned a, b;
        int got = sscanf(t, "%u-%u", &a, &b);

        if (got >= 1 && cpu >= a && cpu <= (got == 2 ? b : a)) {
            return false;
        }
    }
    return true;
}

/* debugfs sched/{fair,ext}_server/cpuN/{period,runtime}: sched_server_write_common() */
static int fair_write(const char *path, const char *value, bool is_period)
{
    char other[PATH_MAX], text[32], stuck[PATH_MAX];
    unsigned long long v, p, r;

    if (!parse_uint(value, ~0ULL, &v)) {
        return -EINVAL;
    }
    sibling(other, sizeof(other), path, is_period ? "runtime" : "period");
    if (!get_u64(other, is_period ? &r : &p)) {
        return -EIO;
    }
    if (is_period) {
        p = v;
    } else {
        r = v;
    }
    if (r > p || p < 100000ULL || p > (1ULL << 22) * 1000ULL) {
        return -EINVAL;
    }
    if (cpu_offline(path)) {
        return -EBUSY;
    }
    /* <file>.writes N: N more writes taken, then refused */
    snprintf(stuck, sizeof(stuck), "%s.writes", path);
    if (access(stuck, F_OK) == 0) {
        unsigned long long left;

        if (!get_u64(stuck, &left) || left == 0) {
            return -EACCES;
        }
        snprintf(text, sizeof(text), "%llu\n", left - 1);
        put(stuck, text);
    }
    snprintf(stuck, sizeof(stuck), "%s.stuck", path);
    if (access(stuck, F_OK) == 0) {
        /* taken, not kept */
        return 0;
    }
    snprintf(text, sizeof(text), "%llu\n", v);
    return put(path, text);
}

/* amdgpu's soft range as pp_od_clk_voltage shows it */
static int od_put(const char *path, unsigned min, unsigned max, unsigned lo, unsigned hi)
{
    char text[256];

    snprintf(text, sizeof(text),
             "OD_SCLK:\n0:        %uMhz\n1:       %uMhz\nOD_RANGE:\nSCLK:     %uMhz       %uMhz\n",
             min, max, lo, hi);
    return put(path, text);
}

static bool od_get(const char *dev_path, struct od_table *od)
{
    char path[PATH_MAX], text[1024];

    sibling(path, sizeof(path), dev_path, "pp_od_clk_voltage");
    return sys_read(path, text, sizeof(text)) > 0 && od_parse(text, od);
}

/* The edits not committed yet: pp_od_clk_voltage.pending, "min max" */
static bool pending_get(const char *dev_path, unsigned *min, unsigned *max)
{
    char path[PATH_MAX], text[64];

    sibling(path, sizeof(path), dev_path, "pp_od_clk_voltage.pending");
    return sys_read(path, text, sizeof(text)) > 0 && sscanf(text, "%u %u", min, max) == 2;
}

static void pending_put(const char *dev_path, unsigned min, unsigned max)
{
    char path[PATH_MAX], text[64];

    sibling(path, sizeof(path), dev_path, "pp_od_clk_voltage.pending");
    snprintf(text, sizeof(text), "%u %u\n", min, max);
    put(path, text);
}

static const char *const levels[] = {"auto", "low", "high", "manual", "profile_standard",
                                     "profile_min_sclk", "profile_min_mclk", "profile_peak",
                                     NULL};

static int level_write(const char *path, const char *value)
{
    char before[32] = "", od_path[PATH_MAX], pending[PATH_MAX], text[160];
    struct od_table od;
    bool known = false;

    for (int i = 0; levels[i]; i++) {
        known |= strcmp(value, levels[i]) == 0;
    }
    if (!known || !od_get(path, &od)) {
        return -EINVAL;
    }
    sys_read(path, before, sizeof(before));
    before[strcspn(before, "\n")] = '\0';
    if (strcmp(value, "manual") == 0) {
        /* the driver's maximum is 0 after the switch to manual */
        if (strcmp(before, "manual") != 0) {
            pending_put(path, od.min, 0);
        }
    } else {
        /* any other level: the driver's own range again */
        sibling(od_path, sizeof(od_path), path, "pp_od_clk_voltage");
        od_put(od_path, od.range_lo, od.range_hi, od.range_lo, od.range_hi);
        sibling(pending, sizeof(pending), path, "pp_od_clk_voltage.pending");
        unlink(pending);
    }
    snprintf(text, sizeof(text), "%s\n", value);
    return put(path, text);
}

static int od_write(const char *path, const char *value)
{
    char level_path[PATH_MAX], level[32] = "";
    struct od_table od;
    unsigned min, max, v;

    sibling(level_path, sizeof(level_path), path, "power_dpm_force_performance_level");
    sys_read(level_path, level, sizeof(level));
    level[strcspn(level, "\n")] = '\0';
    if (strcmp(level, "manual") != 0 || !od_get(path, &od)) {
        return -EINVAL;
    }
    if (!pending_get(path, &min, &max)) {
        min = od.min;
        max = od.max;
    }
    if (sscanf(value, "s 0 %u", &v) == 1) {
        if (v < od.range_lo || v > od.range_hi) {
            return -EINVAL;
        }
        min = v;
    } else if (sscanf(value, "s 1 %u", &v) == 1) {
        if (v < od.range_lo || v > od.range_hi) {
            return -EINVAL;
        }
        max = v;
    } else if (strcmp(value, "r") == 0) {
        min = od.range_lo;
        max = od.range_hi;
    } else if (strcmp(value, "c") == 0) {
        char stuck[PATH_MAX];

        /* "minimum sclk ... greater than the setting maximum" */
        if (max == 0 || min > max) {
            return -EINVAL;
        }
        snprintf(stuck, sizeof(stuck), "%s.stuck", path);
        if (access(stuck, F_OK) != 0) {
            od_put(path, min, max, od.range_lo, od.range_hi);
        }
    } else {
        return -EINVAL;
    }
    pending_put(path, min, max);
    return 0;
}

int sys_write(const char *path, const char *value)
{
    const char *rel = path + strlen(root);
    const char *base = strrchr(path, '/') + 1;
    char v[128];
    int err;

    snprintf(v, sizeof(v), "%s", value);
    v[strcspn(v, "\n")] = '\0';
    if ((strncmp(rel, FAIR_SERVER_DIR "/", sizeof(FAIR_SERVER_DIR)) == 0 ||
         strncmp(rel, EXT_SERVER_DIR "/", sizeof(EXT_SERVER_DIR)) == 0) &&
        (strcmp(base, "period") == 0 || strcmp(base, "runtime") == 0)) {
        err = fair_write(path, v, strcmp(base, "period") == 0);
    } else if (strcmp(base, "power_dpm_force_performance_level") == 0) {
        err = level_write(path, v);
    } else if (strcmp(base, "pp_od_clk_voltage") == 0) {
        err = od_write(path, v);
    } else {
        char stuck[PATH_MAX];

        snprintf(stuck, sizeof(stuck), "%s.stuck", path);
        /* taken, not kept */
        err = access(stuck, F_OK) == 0 ? 0 : put(path, value);
    }
    journal("write %s %s%s", rel, v, err ? " FAILED" : "");
    if (getenv("VITRINE_HELPER_TEST_KILL_AT") &&
        ++writes == atoi(getenv("VITRINE_HELPER_TEST_KILL_AT"))) {
        journal("killed");
        raise(SIGKILL);
    }
    return err;
}

/* --- schedulers and nice values, in <root>/sched --- */

/*
 * "tid policy priority nice" lines: what the kernel would have, for the
 * threads changed here - kept in the fake tree, so that the next helper (a
 * crash recovery) sees what an earlier one set.  flock(LOCK_EX) around each
 * change, helpers running side by side in some tests.
 */
struct sched_entry {
    pid_t tid;
    int policy, priority, nice;
};
static struct sched_entry sched[4096];
static int nsched;
static int sched_fd = -1;

static void sched_lock(void)
{
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/sched", root);
    sched_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (sched_fd >= 0) {
        flock(sched_fd, LOCK_EX);
    }
}

static void sched_load(void)
{
    char text[65536], *save = NULL;
    ssize_t n;

    nsched = 0;
    if (sched_fd < 0 || lseek(sched_fd, 0, SEEK_SET) < 0 ||
        (n = read(sched_fd, text, sizeof(text) - 1)) <= 0) {
        return;
    }
    text[n] = '\0';
    for (char *l = strtok_r(text, "\n", &save); l && nsched < 4096;
         l = strtok_r(NULL, "\n", &save)) {
        struct sched_entry e;

        if (sscanf(l, "%d %d %d %d", &e.tid, &e.policy, &e.priority, &e.nice) == 4) {
            sched[nsched++] = e;
        }
    }
}

static void sched_save(void)
{
    char line[64];

    if (sched_fd < 0 || ftruncate(sched_fd, 0) < 0 || lseek(sched_fd, 0, SEEK_SET) < 0) {
        return;
    }
    for (int i = 0; i < nsched; i++) {
        int len = snprintf(line, sizeof(line), "%d %d %d %d\n", (int)sched[i].tid,
                           sched[i].policy, sched[i].priority, sched[i].nice);
        if (write(sched_fd, line, (size_t)len) != len) {
            return;
        }
    }
}

static void sched_unlock(void)
{
    if (sched_fd >= 0) {
        close(sched_fd);
        sched_fd = -1;
    }
}

/* The entry of @tid, made from what the kernel has; -1 when full */
static int sched_find(pid_t tid)
{
    struct sched_param sp;
    int i, p;

    for (i = 0; i < nsched && sched[i].tid != tid; i++) {
    }
    if (i < nsched) {
        return i;
    }
    if (nsched == (int)(sizeof(sched) / sizeof(sched[0]))) {
        return -1;
    }
    p = sched_getscheduler(tid);
    sched[i].tid = tid;
    sched[i].policy = p < 0 ? SCHED_OTHER : p & ~SCHED_RESET_ON_FORK;
    sched[i].priority = p >= 0 && sched_getparam(tid, &sp) == 0 ? sp.sched_priority : 0;
    errno = 0;
    sched[i].nice = getpriority(PRIO_PROCESS, (id_t)tid);
    if (errno) {
        sched[i].nice = 0;
    }
    nsched++;
    return i;
}

/* @tid's entry, read (@change: changed through @fn, saved) under the lock */
static int sched_with(pid_t tid, bool change, void (*fn)(struct sched_entry *e, void *data),
                      void *data)
{
    int i, err = 0;

    if (kill(tid, 0) < 0 && errno == ESRCH) {
        return -ESRCH;
    }
    sched_lock();
    sched_load();
    if ((i = sched_find(tid)) < 0) {
        err = -ENOMEM;
    } else {
        fn(&sched[i], data);
        if (change) {
            sched_save();
        }
    }
    sched_unlock();
    return err;
}

struct sched_args {
    int policy, priority, nice;
};

static void get_entry(struct sched_entry *e, void *data)
{
    struct sched_args *a = data;

    a->policy = e->policy;
    a->priority = e->priority;
    a->nice = e->nice;
}

static void set_policy(struct sched_entry *e, void *data)
{
    const struct sched_args *a = data;

    e->policy = a->policy;
    e->priority = a->priority;
}

static void set_nice(struct sched_entry *e, void *data)
{
    e->nice = ((const struct sched_args *)data)->nice;
}

int sys_getsched(pid_t tid, int *policy, int *priority)
{
    struct sched_args a;
    int err = sched_with(tid, false, get_entry, &a);

    if (!err) {
        *policy = a.policy;
        *priority = a.priority;
    }
    return err;
}

int sys_setsched(pid_t tid, int policy, int priority)
{
    struct sched_args a = {policy, priority, 0};
    int err = sched_with(tid, true, set_policy, &a);

    if (!err) {
        journal("sched %d %s %d", (int)tid, policy == SCHED_FIFO ? "fifo" : "other", priority);
    }
    return err;
}

int sys_getnice(pid_t tid, int *nice)
{
    struct sched_args a;
    int err = sched_with(tid, false, get_entry, &a);

    if (!err) {
        *nice = a.nice;
    }
    return err;
}

int sys_setnice(pid_t tid, int nice)
{
    struct sched_args a = {0, 0, nice};
    int err = sched_with(tid, true, set_nice, &a);

    if (!err) {
        journal("nice %d %d", (int)tid, nice);
    }
    return err;
}

/*
 * Real-time time limits: an unprivileged test cannot raise a hard limit, so
 * a process's, once set here, is kept in <root>/rttime-<pid> ("soft hard",
 * "unlimited" for none); before that, the process's own.  With a file
 * <root>/rttime.refuse, setting one fails with EPERM.
 */
static void rttime_path(char *path, size_t size, pid_t pid)
{
    snprintf(path, size, "%s/rttime-%d", root, (int)pid);
}

static bool rttime_parse(const char *word, rlim_t *v)
{
    unsigned long long n;
    char *end;

    if (strcmp(word, "unlimited") == 0) {
        *v = RLIM_INFINITY;
        return true;
    }
    errno = 0;
    n = strtoull(word, &end, 10);
    if (errno || end == word || *end) {
        return false;
    }
    *v = (rlim_t)n;
    return true;
}

static const char *rttime_word(rlim_t v, char *buf, size_t size)
{
    if (v == RLIM_INFINITY) {
        return "unlimited";
    }
    snprintf(buf, size, "%llu", (unsigned long long)v);
    return buf;
}

int sys_getrttime(pid_t pid, rlim_t *soft, rlim_t *hard)
{
    char path[PATH_MAX], text[64], a[32], b[32];
    struct rlimit own;
    int fd;

    rttime_path(path, sizeof(path), pid);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) {
        ssize_t n = read(fd, text, sizeof(text) - 1);

        close(fd);
        text[n > 0 ? n : 0] = '\0';
        if (sscanf(text, "%31s %31s", a, b) == 2 && rttime_parse(a, soft) &&
            rttime_parse(b, hard)) {
            return 0;
        }
        return -EIO;
    }
    if (prlimit(pid, RLIMIT_RTTIME, NULL, &own) < 0) {
        return -errno;
    }
    *soft = own.rlim_cur;
    *hard = own.rlim_max;
    return 0;
}

int sys_setrttime(pid_t pid, rlim_t soft, rlim_t hard)
{
    char path[PATH_MAX], a[32], b[32];
    const char *sw = rttime_word(soft, a, sizeof(a)), *hw = rttime_word(hard, b, sizeof(b));
    int fd;

    if (kill(pid, 0) < 0 && errno == ESRCH) {
        return -ESRCH;
    }
    snprintf(path, sizeof(path), "%s/rttime.refuse", root);
    if (access(path, F_OK) == 0) {
        return -EPERM;
    }
    rttime_path(path, sizeof(path), pid);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -errno;
    }
    dprintf(fd, "%s %s\n", sw, hw);
    close(fd);
    journal("rttime %d %s %s", (int)pid, sw, hw);
    return 0;
}

void sys_log(const char *fmt, ...)
{
    char text[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    journal("log %s", text);
}

/* --- the group database: <root>/etc/group --- */

static void group_path(char *path, size_t size)
{
    snprintf(path, size, "%s/etc/group", root);
}

/* The fields of each line of the fake /etc/group: false from @fn stops */
static void each_group(bool (*fn)(const char *name, unsigned gid, char *members, void *data),
                       void *data)
{
    char path[PATH_MAX], text[8192], *save = NULL;

    group_path(path, sizeof(path));
    if (sys_read(path, text, sizeof(text)) <= 0) {
        return;
    }
    for (char *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        char name[64], members[1024] = "";
        unsigned gid;

        if (sscanf(l, "%63[^:]:%*[^:]:%u:%1023[^\n]", name, &gid, members) >= 2 &&
            !fn(name, gid, members, data)) {
            return;
        }
    }
}

struct find {
    const char *name;
    unsigned gid;
    bool found;
};

static bool find_one(const char *name, unsigned gid, char *members, void *data)
{
    struct find *f = data;

    (void)members;
    if (strcmp(name, f->name) == 0) {
        f->gid = gid;
        f->found = true;
        return false;
    }
    return true;
}

int sys_group_find(const char *name, gid_t *gid)
{
    struct find f = {name, 0, false};

    each_group(find_one, &f);
    if (f.found) {
        *gid = (gid_t)f.gid;
    }
    return f.found;
}

struct has {
    const char *user;
    unsigned gid;
    bool member;
};

static bool has_one(const char *name, unsigned gid, char *members, void *data)
{
    struct has *h = data;
    char *save = NULL;

    (void)name;
    if (gid != h->gid) {
        return true;
    }
    for (char *m = strtok_r(members, ",", &save); m; m = strtok_r(NULL, ",", &save)) {
        h->member |= strcmp(m, h->user) == 0;
    }
    return !h->member;
}

bool sys_group_has(const char *user, gid_t primary, gid_t gid)
{
    struct has h = {user, (unsigned)gid, primary == gid};

    if (!h.member) {
        each_group(has_one, &h);
    }
    return h.member;
}

/* Appends @line to the fake /etc/group, or replaces the line of @name with it */
static int group_put(const char *name, const char *line, char *err, size_t size)
{
    char path[PATH_MAX], text[8192] = "", out[8192] = "", *save = NULL;
    size_t len = strlen(name), used = 0;
    bool replaced = false;
    int fd;

    group_path(path, sizeof(path));
    sys_read(path, text, sizeof(text));
    for (char *l = strtok_r(text, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        bool his = strncmp(l, name, len) == 0 && l[len] == ':';
        used += (size_t)snprintf(out + used, sizeof(out) - used, "%s\n", his ? line : l);
        replaced |= his;
    }
    if (!replaced) {
        used += (size_t)snprintf(out + used, sizeof(out) - used, "%s\n", line);
    }
    fd = open(path, O_WRONLY | O_TRUNC | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0 || write(fd, out, used) != (ssize_t)used) {
        snprintf(err, size, "%s", strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }
    close(fd);
    return 0;
}

int sys_group_create(const char *name, char *err, size_t size)
{
    char line[128];
    int ret;

    /* groupadd --system: an id under 1000 */
    snprintf(line, sizeof(line), "%s:x:977:", name);
    ret = group_put(name, line, err, size);
    journal("groupadd --system %s%s", name, ret ? " FAILED" : "");
    return ret;
}

struct add {
    const char *name;
    char line[1100];
    bool found;
};

static bool add_one(const char *name, unsigned gid, char *members, void *data)
{
    struct add *a = data;

    if (strcmp(name, a->name) != 0) {
        return true;
    }
    snprintf(a->line, sizeof(a->line), "%s:x:%u:%s", name, gid, members);
    a->found = true;
    return false;
}

int sys_group_add_user(const char *name, const char *user, char *err, size_t size)
{
    struct add a = {name, "", false};
    size_t len;
    int ret;

    each_group(add_one, &a);
    if (!a.found) {
        snprintf(err, size, "group '%s' does not exist in /etc/group", name);
        ret = -1;
    } else {
        len = strlen(a.line);
        snprintf(a.line + len, sizeof(a.line) - len, "%s%s", a.line[len - 1] == ':' ? "" : ",",
                 user);
        ret = group_put(name, a.line, err, size);
    }
    journal("gpasswd -a %s %s%s", user, name, ret ? " FAILED" : "");
    return ret;
}
