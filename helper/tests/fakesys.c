/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The test build's system access (vitrine-helper-fake): the attribute
 * files live in a fake tree under $VITRINE_HELPER_TEST_ROOT, where this
 * file plays the kernel's part - the fair server's runtime <= period check
 * at each write, amdgpu's overdrive table that takes edits in manual only
 * and a commit that fails while the maximum is 0 - so that the order of the
 * writes is tested, not just their values.  Other files (the udmabuf
 * module's parameters) take any value, as the kernel's int parameters do.  Every write, scheduler change,
 * capability and log line is appended to <root>/journal.  Schedulers are
 * kept in memory (an unprivileged test cannot make threads real-time); the
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

/* debugfs sched/fair_server/cpuN/{period,runtime}: sched_fair_server_write() */
static int fair_write(const char *path, const char *value, bool is_period)
{
    char other[PATH_MAX], text[32];
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
    char before[32] = "", od_path[PATH_MAX], pending[PATH_MAX], text[40];
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
        /* "minimum sclk ... greater than the setting maximum" */
        if (max == 0 || min > max) {
            return -EINVAL;
        }
        od_put(path, min, max, od.range_lo, od.range_hi);
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
    if (strncmp(rel, FAIR_SERVER_DIR "/", sizeof(FAIR_SERVER_DIR)) == 0 &&
        (strcmp(base, "period") == 0 || strcmp(base, "runtime") == 0)) {
        err = fair_write(path, v, strcmp(base, "period") == 0);
    } else if (strcmp(base, "power_dpm_force_performance_level") == 0) {
        err = level_write(path, v);
    } else if (strcmp(base, "pp_od_clk_voltage") == 0) {
        err = od_write(path, v);
    } else {
        err = put(path, value);
    }
    journal("write %s %s%s", rel, v, err ? " FAILED" : "");
    if (getenv("VITRINE_HELPER_TEST_KILL_AT") &&
        ++writes == atoi(getenv("VITRINE_HELPER_TEST_KILL_AT"))) {
        journal("killed");
        raise(SIGKILL);
    }
    return err;
}

/* --- schedulers, in memory --- */

static struct {
    pid_t tid;
    int policy, priority;
} sched[4096];
static int nsched;

int sys_getsched(pid_t tid, int *policy, int *priority)
{
    struct sched_param sp;
    int p;

    for (int i = 0; i < nsched; i++) {
        if (sched[i].tid == tid) {
            *policy = sched[i].policy;
            *priority = sched[i].priority;
            return 0;
        }
    }
    if ((p = sched_getscheduler(tid)) < 0 || sched_getparam(tid, &sp) < 0) {
        return -errno;
    }
    *policy = p & ~SCHED_RESET_ON_FORK;
    *priority = sp.sched_priority;
    return 0;
}

int sys_setsched(pid_t tid, int policy, int priority)
{
    int i;

    if (kill(tid, 0) < 0 && errno == ESRCH) {
        return -ESRCH;
    }
    for (i = 0; i < nsched && sched[i].tid != tid; i++) {
    }
    if (i == nsched) {
        if (nsched == (int)(sizeof(sched) / sizeof(sched[0]))) {
            return -ENOMEM;
        }
        nsched++;
    }
    sched[i].tid = tid;
    sched[i].policy = policy;
    sched[i].priority = priority;
    journal("sched %d %s %d", (int)tid, policy == SCHED_FIFO ? "fifo" : "other", priority);
    return 0;
}

int sys_set_file_cap(int fd, const char *path)
{
    (void)fd;
    journal("setcap %s", path);
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
