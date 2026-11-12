/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The global settings: the kernel's fair server (and ext server), an AMD
 * GPU's clock floor, the udmabuf limits, shared memory's transparent huge
 * pages and the CPUs' energy performance preference; and the record of
 * each QEMU whose threads a helper made real-time or ordinary, and of the
 * process that started it, whose threads view-rt made real-time
 * (sched-<pid>).  The
 * settings belong to the whole machine, so several helpers (vitrine
 * restarted while its VMs run, other users' sessions) coordinate through
 * /run/vitrine-helper, a root-only tmpfs folder whose lifetime is the
 * boot's, like the values themselves:
 *
 *   lock           flock(LOCK_EX) around every change of a hold
 *   <key>.hold     flock(LOCK_SH) by every helper holding the setting, for
 *                  as long as it holds it; the kernel drops it when the
 *                  helper dies, whatever the way
 *   <key>.state    what the setting was and what was written, saved before
 *                  the first write
 *
 * The first holder (it gets LOCK_EX on the hold) saves the originals and
 * writes; the others join without writing; the last one (LOCK_EX again
 * when it lets go) restores each value that still equals what was written
 * - another tool (the research side's vm-host-session) may have changed it
 * since, and then it is left alone.  A state file nobody holds is what a
 * crashed helper left: the next helper restores it.
 *
 * The CPU latency request is no hold: it is this helper's open file of
 * /dev/cpu_dma_latency, and the kernel itself takes the lowest of every
 * program's requests and drops each one with its file.
 *
 * vfio.c's records go through the same holds: an IOMMU group handed to a VM
 * (vfio-<group>, one helper at a time) and the locked memory limit of the
 * process that started the helpers (memlock-<pid>, shared).
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "helper.h"

typedef unsigned long long u64;

static int state_fd = -1;
static int lock_fd = -1;

struct hold {
    char key[32];       /* fair-server, ext-server, gpu-floor-cardN, udmabuf,
                           thp, epp, vfio-<group>, memlock-<pid>,
                           sched-<pid>; empty when free */
    int fd;             /* <key>.hold */
    char value[16];     /* what was asked: on, auto or MHz */
};
/* the fair and ext servers, the udmabuf limits, huge pages, the CPUs'
   preference, eight cards, the IOMMU groups of PCI devices handed to VMs
   and the parent's locked memory limit (vfio.c); then the records of the
   QEMUs watched, and of the process that started the helper (view-rt) */
#define MAX_CARDS 8
#define MAX_SETTINGS (5 + MAX_CARDS + MAX_VFIO_GROUPS + 1)
#define MAX_HOLDS (MAX_SETTINGS + MAX_WATCHED + 1)
static struct hold holds[MAX_HOLDS];

void lock_all(void)
{
    flock(lock_fd, LOCK_EX);
}

void unlock_all(void)
{
    flock(lock_fd, LOCK_UN);
}

/* --- state files --- */

bool state_write(const char *key, const char *text)
{
    char tmp[64], name[64];
    size_t len = strlen(text);
    int fd;
    bool ok;

    snprintf(tmp, sizeof(tmp), "%s.state.new", key);
    snprintf(name, sizeof(name), "%s.state", key);
    fd = openat(state_fd, tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return false;
    }
    ok = write(fd, text, len) == (ssize_t)len;
    ok &= close(fd) == 0;
    return ok && renameat(state_fd, tmp, state_fd, name) == 0;
}

/* The state of @key in a malloc'ed string, or NULL when there is none */
char *state_read(const char *key)
{
    char name[64];
    struct stat st;
    char *text;
    ssize_t n;
    int fd;

    snprintf(name, sizeof(name), "%s.state", key);
    fd = openat(state_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return NULL;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size > (1 << 20) ||
        !(text = malloc((size_t)st.st_size + 1))) {
        close(fd);
        return NULL;
    }
    n = read(fd, text, (size_t)st.st_size);
    close(fd);
    if (n < 0) {
        free(text);
        return NULL;
    }
    text[n] = '\0';
    return text;
}

void state_remove(const char *key)
{
    char name[64];

    snprintf(name, sizeof(name), "%s.state", key);
    unlinkat(state_fd, name, 0);
}

/* --- attribute files --- */

static int read_attr(const char *path, char *buf, size_t size)
{
    char full[PATH_MAX];

    if (!rooted(full, sizeof(full), path)) {
        return -ENAMETOOLONG;
    }
    return sys_read(full, buf, size);
}

static int write_attr(const char *path, const char *value)
{
    char full[PATH_MAX];

    if (!rooted(full, sizeof(full), path)) {
        return -ENAMETOOLONG;
    }
    return sys_write(full, value);
}

static bool read_u64(const char *path, u64 *out)
{
    char buf[32];
    int n = read_attr(path, buf, sizeof(buf));

    if (n <= 0) {
        return false;
    }
    buf[strcspn(buf, "\n")] = '\0';
    return parse_uint(buf, ~0ULL, out);
}

static int write_u64(const char *path, u64 value)
{
    char buf[32];

    snprintf(buf, sizeof(buf), "%llu\n", value);
    return write_attr(path, buf);
}

/* The first line of @path without its newline, in @buf */
static bool read_word(const char *path, char *buf, size_t size)
{
    if (read_attr(path, buf, size) <= 0) {
        return false;
    }
    buf[strcspn(buf, "\n")] = '\0';
    return true;
}

/* Why debugfs cannot be written, or NULL */
static const char *lockdown(void)
{
    static char why[64];
    char buf[128];
    char *lb, *rb;

    /* "none [integrity] confidentiality": the level in brackets */
    if (read_attr(LOCKDOWN_FILE, buf, sizeof(buf)) <= 0 || !(lb = strchr(buf, '[')) ||
        !(rb = strchr(lb, ']')) || strncmp(lb, "[none]", 6) == 0) {
        return NULL;
    }
    *rb = '\0';
    snprintf(why, sizeof(why), "kernel lockdown (%.32s)", lb + 1);
    return why;
}

/* --- holds --- */

struct hold *hold_find(const char *key)
{
    for (int i = 0; i < MAX_HOLDS; i++) {
        if (strcmp(holds[i].key, key) == 0) {
            return &holds[i];
        }
    }
    return NULL;
}

static void restore_key(const char *key);
static void recover_pass(bool sched);

/* A free hold for @key: the QEMUs' records apart from the settings, eight
   cards at most among those */
static struct hold *hold_free(const char *key)
{
    const bool sched = strncmp(key, "sched-", 6) == 0;
    int cards = 0;

    for (int i = 0; i < MAX_SETTINGS; i++) {
        cards += strncmp(holds[i].key, "gpu-floor-", 10) == 0;
    }
    if (!sched && strncmp(key, "gpu-floor-", 10) == 0 && cards >= MAX_CARDS) {
        return NULL;
    }
    for (int i = sched ? MAX_SETTINGS : 0; i < (sched ? MAX_HOLDS : MAX_SETTINGS); i++) {
        if (!holds[i].key[0]) {
            return &holds[i];
        }
    }
    return NULL;
}

/*
 * Under the lock: takes the hold of @key.  *@first tells whether no other
 * helper holds it: this helper then changes the setting, keeping LOCK_EX
 * until hold_share().
 */
struct hold *hold_take(const char *key, const char *value, bool *first)
{
    struct hold *h = hold_free(key);
    char name[48];
    int fd;

    if (!h) {
        /* the callers say why from errno */
        errno = ENOSPC;
        return NULL;
    }
    snprintf(name, sizeof(name), "%s.hold", key);
    fd = openat(state_fd, name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return NULL;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        *first = true;
        /* a state nobody holds: a helper died holding it */
        restore_key(key);
    } else if (errno == EWOULDBLOCK && flock(fd, LOCK_SH) == 0) {
        *first = false;
    } else {
        close(fd);
        return NULL;
    }
    snprintf(h->key, sizeof(h->key), "%s", key);
    snprintf(h->value, sizeof(h->value), "%s", value);
    h->fd = fd;
    return h;
}

void hold_share(struct hold *h)
{
    /* no one else holds any lock on it: the conversion cannot lose it */
    flock(h->fd, LOCK_SH);
}

/* Under the lock, by a helper holding <key>.hold with LOCK_EX: no other
   helper has it open, they open it under the lock only */
static void hold_remove(const char *key)
{
    char name[48];

    snprintf(name, sizeof(name), "%s.hold", key);
    unlinkat(state_fd, name, 0);
}

/* @last: this helper held it alone (LOCK_EX), the file goes too */
void hold_close(struct hold *h, bool last)
{
    if (last) {
        hold_remove(h->key);
    }
    close(h->fd);
    h->fd = -1;
    h->key[0] = '\0';
}

void hold_let_go(struct hold *h)
{
    lock_all();
    /* LOCK_EX only when no other helper holds a LOCK_SH: the last one */
    hold_close(h, flock(h->fd, LOCK_EX | LOCK_NB) == 0);
    unlock_all();
}

struct hold *hold_adopt(const char *key, int fd)
{
    struct hold *h = hold_free(key);

    if (!h) {
        return NULL;
    }
    /* nobody else has it (LOCK_EX was free): shared from now on, as a hold taken */
    flock(fd, LOCK_SH);
    snprintf(h->key, sizeof(h->key), "%s", key);
    snprintf(h->value, sizeof(h->value), "on");
    h->fd = fd;
    return h;
}

/* Lets go of @h, restoring the setting when this helper was its last holder */
void hold_drop(struct hold *h)
{
    bool last;

    lock_all();
    /* LOCK_EX only when no other helper holds a LOCK_SH: the last one */
    if ((last = flock(h->fd, LOCK_EX | LOCK_NB) == 0)) {
        restore_key(h->key);
    }
    hold_close(h, last);
    unlock_all();
}

/* --- the kernel's deadline servers: the fair server, the ext server --- */

struct server {
    const char *key;    /* its hold and state */
    const char *dir;
    const char *name;   /* in messages */
};
static const struct server fair = {"fair-server", FAIR_SERVER_DIR, "fair server"};
static const struct server ext = {"ext-server", EXT_SERVER_DIR, "ext server"};

/* fair_server_on(): a sched_ext scheduler ran, its server is part of the
   bound; why the bound is not in place */
static bool ext_needed;
static char bound_why[200] = "the fair server was not asked for";

struct server_cpu {
    unsigned id;
    u64 period, runtime;            /* before, to put back */
    u64 new_period, new_runtime;    /* written, or found there */
    bool adopted;                   /* found at the target without a record */
};

static void server_path(char *out, size_t size, const struct server *srv, unsigned cpu,
                        const char *file)
{
    snprintf(out, size, "%s/cpu%u/%s", srv->dir, cpu, file);
}

static bool server_get(const struct server *srv, unsigned cpu, u64 *period, u64 *runtime)
{
    char p[PATH_MAX], r[PATH_MAX];

    server_path(p, sizeof(p), srv, cpu, "period");
    server_path(r, sizeof(r), srv, cpu, "runtime");
    return read_u64(p, period) && read_u64(r, runtime);
}

/*
 * CPU @cpu's server from (@period, @runtime) to (@to_period, @to_runtime).
 * The kernel checks runtime <= period at each write, so a shorter period
 * goes after the runtime, a longer one before.  If the second write fails
 * the first is undone: 0 or -errno.
 */
static int server_put(const struct server *srv, unsigned cpu, u64 period, u64 runtime,
                      u64 to_period, u64 to_runtime)
{
    char p[PATH_MAX], r[PATH_MAX];
    int err;

    server_path(p, sizeof(p), srv, cpu, "period");
    server_path(r, sizeof(r), srv, cpu, "runtime");
    if (to_period < period) {
        if ((err = write_u64(r, to_runtime)) < 0) {
            return err;
        }
        if ((err = write_u64(p, to_period)) < 0) {
            write_u64(r, runtime);
        }
    } else {
        if ((err = write_u64(p, to_period)) < 0) {
            return err;
        }
        if ((err = write_u64(r, to_runtime)) < 0) {
            write_u64(p, period);
        }
    }
    return err;
}

/* server_put(), then read back: -EIO, and put back as it was, when the
   values read are not those written */
static int server_set(const struct server *srv, unsigned cpu, u64 period, u64 runtime,
                      u64 to_period, u64 to_runtime)
{
    int err = server_put(srv, cpu, period, runtime, to_period, to_runtime);
    u64 got_period, got_runtime;

    if (err < 0) {
        return err;
    }
    if (!server_get(srv, cpu, &got_period, &got_runtime)) {
        /* written, then not readable: back as far as can be */
        server_put(srv, cpu, to_period, to_runtime, period, runtime);
        return -EIO;
    }
    if (got_period != to_period || got_runtime != to_runtime) {
        server_put(srv, cpu, got_period, got_runtime, period, runtime);
        return -EIO;
    }
    return 0;
}

/* The numbers of the cpuN folders of @dir, sorted: their count, or -errno */
static int cpu_dirs(const char *folder, unsigned **out)
{
    char dir[PATH_MAX];
    struct dirent *e;
    unsigned *ids = NULL;
    int n = 0, cap = 0;
    DIR *d;

    *out = NULL;
    if (!rooted(dir, sizeof(dir), folder) || !(d = opendir(dir))) {
        return errno ? -errno : -ENAMETOOLONG;
    }
    while ((e = readdir(d))) {
        u64 id;

        if (strncmp(e->d_name, "cpu", 3) != 0 || !parse_uint(e->d_name + 3, 65535, &id)) {
            continue;
        }
        if (n == cap) {
            unsigned *more = realloc(ids, (cap = cap ? cap * 2 : 64) * sizeof(*ids));
            if (!more) {
                free(ids);
                closedir(d);
                return -ENOMEM;
            }
            ids = more;
        }
        ids[n++] = (unsigned)id;
    }
    closedir(d);
    /* sorted for the state file and the messages: an insertion sort is plenty */
    for (int i = 1; i < n; i++) {
        unsigned v = ids[i];
        int j = i;
        for (; j > 0 && ids[j - 1] > v; j--) {
            ids[j] = ids[j - 1];
        }
        ids[j] = v;
    }
    *out = ids;
    return n;
}

/* The CPUs of @srv, sorted; their count, or -errno */
static int server_cpus(const struct server *srv, unsigned **out)
{
    return cpu_dirs(srv->dir, out);
}

/*
 * The online CPUs, from /sys/devices/system/cpu/online ("0-3,8-11"): a
 * bitmap of the CPU numbers the servers' folders may have (cpu0 to
 * cpu65535); all of them when the list cannot be read, or not whole
 */
struct online {
    bool all;
    unsigned char bits[65536 / 8];
};

static void online_read(struct online *o)
{
    char buf[8192], *save = NULL;
    int n = read_attr(CPUS_ONLINE, buf, sizeof(buf));
    bool any = false;

    memset(o->bits, 0, sizeof(o->bits));
    o->all = n <= 0 || n >= (int)sizeof(buf) - 1;
    for (char *t = o->all ? NULL : strtok_r(buf, ",\n", &save); t;
         t = strtok_r(NULL, ",\n", &save)) {
        unsigned a, b;
        int got = sscanf(t, "%u-%u", &a, &b);

        if (got < 2) {
            b = a;
        }
        if (got < 1 || b < a || b > 65535) {
            /* not a list: all of them, as when unknown */
            o->all = true;
            return;
        }
        for (unsigned c = a; c <= b; c++) {
            o->bits[c / 8] |= (unsigned char)(1u << (c % 8));
        }
        any = true;
    }
    o->all |= !any;
}

static bool online_has(const struct online *o, unsigned cpu)
{
    return o->all || (cpu < 65536 && (o->bits[cpu / 8] >> (cpu % 8) & 1));
}

int online_cpus(void)
{
    static struct online o;
    unsigned *ids = NULL;
    int n = 0;

    online_read(&o);
    if (!o.all) {
        for (size_t i = 0; i < sizeof(o.bits); i++) {
            n += __builtin_popcount(o.bits[i]);
        }
        return n;
    }
    /* the list unknown: every CPU the kernel has a folder for */
    n = cpu_dirs(CPU_DIR, &ids);
    free(ids);
    return n > 0 ? n : 1;
}

/* A sched_ext scheduler runs (or is starting or stopping): its tasks are
   the ext server's, not the fair server's */
static bool sched_ext_active(void)
{
    char state[32];

    return read_word(SCHED_EXT_STATE, state, sizeof(state)) && strcmp(state, "disabled") != 0;
}

static bool dir_exists(const char *dir)
{
    char path[PATH_MAX];
    struct stat st;

    return rooted(path, sizeof(path), dir) && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Why the bound cannot be set here, or NULL */
static const char *fair_unavailable(void)
{
    const char *why = lockdown();

    if (why) {
        return why;
    }
    if (dir_exists(FAIR_SERVER_DIR)) {
        return NULL;
    }
    if (dir_exists("/sys/kernel/debug/sched")) {
        return "this kernel has no fair server";
    }
    return "debugfs is not mounted";
}

/* "cpuN period runtime new_period new_runtime" lines for the CPUs @c records */
static size_t server_state(char *text, const struct server_cpu *c, int n)
{
    size_t len = 0;

    text[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (c[i].new_period) {
            len += (size_t)sprintf(text + len, "cpu%u %llu %llu %llu %llu\n", c[i].id,
                                   c[i].period, c[i].runtime, c[i].new_period,
                                   c[i].new_runtime);
        }
    }
    return len;
}

/*
 * Under the lock, as the last holder or for a dead one: the state back,
 * where it still holds what was written.  A CPU that cannot be put back now
 * (offline, say) stays recorded, for the next helper to try again: the
 * record goes only with what it records.
 */
static void server_restore(const struct server *srv)
{
    char *text = state_read(srv->key), *line, *save = NULL, *keep;
    int restored = 0, already = 0, left = 0, failed = 0;
    size_t kept = 0;
    u64 period = 0, runtime = 0;

    if (!text) {
        return;
    }
    if (!(keep = malloc(strlen(text) + 1))) {
        /* nothing lost: the next helper tries again */
        free(text);
        return;
    }
    keep[0] = '\0';
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        struct server_cpu c;
        u64 cur_p, cur_r;
        bool ok = false;

        if (sscanf(line, "cpu%u %llu %llu %llu %llu", &c.id, &c.period, &c.runtime,
                   &c.new_period, &c.new_runtime) != 5 || c.runtime > c.period) {
            continue;
        }
        period = c.period;
        runtime = c.runtime;
        if (!server_get(srv, c.id, &cur_p, &cur_r)) {
            /* not readable now: tried again later */
        } else if (cur_p == c.period && cur_r == c.runtime) {
            already++;
            continue;
        } else if ((cur_p == c.new_period && cur_r == c.new_runtime) ||
                   ((cur_p == c.period || cur_p == c.new_period) &&
                    (cur_r == c.runtime || cur_r == c.new_runtime))) {
            /*
             * What was written, or half-way, as server_put() leaves it
             * between its two writes: a helper killed there, setting or
             * restoring.  Not another tool's change: theirs would not be
             * one of our pairs.  (The other mixed pair cannot happen: its
             * runtime would exceed its period, which the kernel refuses.)
             */
            ok = server_set(srv, c.id, cur_p, cur_r, c.period, c.runtime) == 0;
            if (ok) {
                restored++;
                continue;
            }
        } else {
            left++;
            continue;
        }
        failed++;
        kept += (size_t)sprintf(keep + kept, "%s\n", line);
    }
    free(text);
    if (kept) {
        state_write(srv->key, keep);
    } else {
        state_remove(srv->key);
    }
    free(keep);
    reply("restored %s: %d cpus back to %llu ms / %llu ms%s", srv->key, restored + already,
          period / 1000000, runtime / 1000000, failed ? " (some failed, kept for later)" : "");
    if (left) {
        reply("left %s: %d cpus changed by someone else since", srv->key, left);
    }
    sys_log("%s: %d cpus back to %llu / %llu ns, %d left as changed since, %d failed", srv->name,
            restored, period, runtime, left, failed);
}

/* The line of CPU @id in the state text @text ("cpuN p r np nr"), parsed */
static bool state_line(const char *text, unsigned id, struct server_cpu *c)
{
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        if (sscanf(l, "cpu%u %llu %llu %llu %llu", &c->id, &c->period, &c->runtime,
                   &c->new_period, &c->new_runtime) == 5 && c->id == id &&
            c->runtime <= c->period) {
            return true;
        }
    }
    return false;
}

/*
 * The state of @srv: the CPUs of @c that are recorded, then the lines of
 * @before for CPUs not among them (offline ones a restore could not put
 * back, kept for later).  Written, or removed when it records nothing.
 */
static bool server_record(const struct server *srv, const struct server_cpu *c, int n,
                          const char *before)
{
    size_t size = (size_t)n * 96 + (before ? strlen(before) : 0) + 1;
    char *text = malloc(size);
    size_t len;
    bool ok;

    if (!text) {
        errno = ENOMEM;
        return false;
    }
    len = server_state(text, c, n);
    for (const char *l = before; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        size_t llen = strcspn(l, "\n");
        bool ours = false;
        unsigned id;

        if (sscanf(l, "cpu%u ", &id) != 1) {
            continue;
        }
        for (int i = 0; i < n && !ours; i++) {
            ours = c[i].id == id;
        }
        if (!ours) {
            len += (size_t)sprintf(text + len, "%.*s\n", (int)llen, l);
        }
    }
    if (len) {
        ok = state_write(srv->key, text);
    } else {
        state_remove(srv->key);
        ok = true;
    }
    free(text);
    return ok;
}

/*
 * Under the lock, by the first holder: @srv at the target on every online
 * CPU, or nothing changed.  Offline CPUs run nothing (and the kernel
 * refuses their writes): left out, and a record a restore kept for them
 * stays.  A CPU at the target while others are not is a leftover - a run
 * cut short, its record lost - recorded with what the others have (the
 * kernel's default if they differ), or with what a kept record says, to be
 * put back with them.  All of them at the target: another tool's, which
 * may still need it, or a leftover nobody can tell from that - left as
 * found.  0 with what was done, or -1 with why in @why.
 */
static int server_apply(const struct server *srv, char *why, size_t size, int *changed,
                        int *adopted, u64 *was_period, u64 *was_runtime)
{
    static struct online online;
    struct server_cpu *cpus = NULL;
    unsigned *ids = NULL;
    /* what a restore could not put back (hold_take() has just tried) */
    char *before = state_read(srv->key);
    u64 base_p = 0, base_r = 0;
    bool base = false, mixed = false;
    int n, m = 0, rc = -1;

    online_read(&online);
    n = server_cpus(srv, &ids);
    if (n <= 0) {
        snprintf(why, size, "%s: %s", srv->name, n < 0 ? strerror(-n) : "no cpus");
        goto out;
    }
    if (!(cpus = calloc((size_t)n, sizeof(*cpus)))) {
        snprintf(why, size, "%s", strerror(ENOMEM));
        goto out;
    }
    /* what each online CPU has, read before anything is written */
    for (int i = 0; i < n; i++) {
        u64 p, r;

        if (!online_has(&online, ids[i])) {
            continue;
        }
        if (!server_get(srv, ids[i], &p, &r)) {
            snprintf(why, size, "%s: cpu%u cannot be read", srv->name, ids[i]);
            goto out;
        }
        cpus[m++] = (struct server_cpu){ids[i], p, r, 0, 0, false};
        if (p != FAIR_PERIOD_NS || r != FAIR_RUNTIME_NS) {
            mixed |= base && (p != base_p || r != base_r);
            base_p = p;
            base_r = r;
            base = true;
        }
    }
    if (mixed) {
        base_p = FAIR_DEFAULT_PERIOD_NS;
        base_r = FAIR_DEFAULT_RUNTIME_NS;
    }
    for (int i = 0; i < m; i++) {
        struct server_cpu *c = &cpus[i], kept;

        if (c->period == FAIR_PERIOD_NS && c->runtime == FAIR_RUNTIME_NS) {
            if (before && state_line(before, c->id, &kept)) {
                /* recorded already, by a helper that could not put it back */
                c->period = kept.period;
                c->runtime = kept.runtime;
            } else if (base) {
                c->period = base_p;
                c->runtime = base_r;
            } else {
                continue;
            }
            c->adopted = true;
            (*adopted)++;
        }
        c->new_period = FAIR_PERIOD_NS;
        c->new_runtime = FAIR_RUNTIME_NS;
    }
    /* recorded before anything is written */
    if (!server_record(srv, cpus, m, before)) {
        snprintf(why, size, "%s: %s", srv->name, strerror(errno));
        goto out;
    }
    for (int i = 0; i < m; i++) {
        struct server_cpu *c = &cpus[i];
        u64 now_p, now_r;
        int err;

        if (!c->new_period || c->adopted) {
            continue;
        }
        err = server_set(srv, c->id, c->period, c->runtime, c->new_period, c->new_runtime);
        if (err == 0) {
            if (!*changed) {
                *was_period = c->period;
                *was_runtime = c->runtime;
            }
            (*changed)++;
            continue;
        }
        if (err == -EIO) {
            snprintf(why, size, "%s: cpu%u does not read back what was written", srv->name, c->id);
        } else if (err == -EBUSY) {
            snprintf(why, size, "%s: cpu%u is busy (offline now?)", srv->name, c->id);
        } else {
            snprintf(why, size, "%s: cpu%u: %s", srv->name, c->id, strerror(-err));
        }
        /*
         * All or nothing: those written before it back.  What is not as it
         * was stays recorded, for a helper to put back - this CPU if its
         * own undo failed, one that could not be put back, the leftovers
         * found (no hold left on the record: the next helper's)
         */
        if (server_get(srv, c->id, &now_p, &now_r) && now_p == c->period &&
            now_r == c->runtime) {
            c->new_period = 0;
        }
        for (int j = 0; j < i; j++) {
            struct server_cpu *b = &cpus[j];

            if (b->new_period && !b->adopted &&
                server_set(srv, b->id, b->new_period, b->new_runtime, b->period, b->runtime) == 0) {
                b->new_period = 0;
            }
        }
        for (int j = i + 1; j < m; j++) {
            if (!cpus[j].adopted) {
                cpus[j].new_period = 0;
            }
        }
        server_record(srv, cpus, m, before);
        *changed = *adopted = 0;
        goto out;
    }
    rc = 0;
out:
    free(before);
    free(ids);
    free(cpus);
    return rc;
}

/* @srv held at the target: true with what was done in @done, false with why
   in @why */
static bool server_on(const struct server *srv, char *done, size_t dsize, char *why,
                      size_t wsize)
{
    int changed = 0, adopted = 0, err;
    u64 was_period = 0, was_runtime = 0;
    struct hold *h;
    bool first;

    lock_all();
    if (!(h = hold_take(srv->key, "on", &first))) {
        err = errno;
        unlock_all();
        snprintf(why, wsize, "%s: cannot take the hold: %s", srv->name, strerror(err));
        return false;
    }
    if (!first) {
        unlock_all();
        snprintf(done, dsize, "set by another vitrine session");
        return true;
    }
    if (server_apply(srv, why, wsize, &changed, &adopted, &was_period, &was_runtime) < 0) {
        /* a record left (not all put back) is a dead holder's for the next helper */
        hold_close(h, true);
        unlock_all();
        return false;
    }
    hold_share(h);
    unlock_all();
    done[0] = '\0';
    if (changed) {
        snprintf(done, dsize, "%d cpus at 10 ms / 1 ms (was %llu ms / %llu ms)", changed,
                 was_period / 1000000, was_runtime / 1000000);
        sys_log("uid %u: %s %llu / %llu ns on %d cpus (was %llu / %llu ns)", (unsigned)caller_uid,
                srv->name, FAIR_PERIOD_NS, FAIR_RUNTIME_NS, changed, was_period, was_runtime);
    } else if (!adopted) {
        snprintf(done, dsize, "already 10 ms / 1 ms");
    }
    if (adopted) {
        size_t len = strlen(done);

        snprintf(done + len, dsize - len, "%s%d cpus found at 10 ms / 1 ms without a record, put "
                 "back after with the others", len ? "; " : "", adopted);
        sys_log("uid %u: %s: %d cpus found at %llu / %llu ns without a record: recorded",
                (unsigned)caller_uid, srv->name, adopted, FAIR_PERIOD_NS, FAIR_RUNTIME_NS);
    }
    return true;
}

void fair_server_on(void)
{
    char done[200] = "", ext_done[200] = "", why[200] = "";
    const char *skip;

    if (hold_find(fair.key)) {
        /* a sched_ext scheduler started since: its server too */
        if (!sched_ext_active() || hold_find(ext.key)) {
            reply("ok fair-server on: already");
            return;
        }
        ext_needed = true;
        if (!dir_exists(EXT_SERVER_DIR)) {
            snprintf(bound_why, sizeof(bound_why),
                     "a sched_ext scheduler runs, and this kernel has no server for its tasks");
            reply("skip fair-server: %s", bound_why);
        } else if (!server_on(&ext, ext_done, sizeof(ext_done), why, sizeof(why))) {
            snprintf(bound_why, sizeof(bound_why), "%s", why);
            reply("error fair-server: %s", why);
        } else {
            bound_why[0] = '\0';
            reply("ok fair-server on: already; ext server: %s", ext_done);
        }
        return;
    }
    if ((skip = fair_unavailable())) {
        snprintf(bound_why, sizeof(bound_why), "%s", skip);
        reply("skip fair-server: %s", skip);
        return;
    }
    /* under sched_ext, ordinary tasks wait for the ext server: no bound
       without it (it came with Linux 7.0) */
    ext_needed = sched_ext_active();
    if (ext_needed && !dir_exists(EXT_SERVER_DIR)) {
        snprintf(bound_why, sizeof(bound_why),
                 "a sched_ext scheduler runs, and this kernel has no server for its tasks");
        reply("skip fair-server: %s", bound_why);
        return;
    }
    if (!server_on(&fair, done, sizeof(done), why, sizeof(why))) {
        snprintf(bound_why, sizeof(bound_why), "%s", why);
        reply("error fair-server: %s", why);
        return;
    }
    if (ext_needed && !server_on(&ext, ext_done, sizeof(ext_done), why, sizeof(why))) {
        /* all or nothing, the bound with it */
        hold_drop(hold_find(fair.key));
        snprintf(bound_why, sizeof(bound_why), "%s", why);
        reply("error fair-server: %s", why);
        return;
    }
    bound_why[0] = '\0';
    if (ext_needed) {
        reply("ok fair-server on: %s; ext server: %s", done, ext_done);
    } else {
        reply("ok fair-server on: %s", done);
    }
}

void fair_server_off(void)
{
    struct hold *h;

    /* no real-time thread without the bound */
    rt_off_all();
    if ((h = hold_find(ext.key))) {
        hold_drop(h);
    }
    if ((h = hold_find(fair.key))) {
        hold_drop(h);
    }
    snprintf(bound_why, sizeof(bound_why), "the fair server was let go");
    reply("ok fair-server off");
}

bool settings_bound(const char **why)
{
    /* a sched_ext scheduler started since the fair server was set needs
       its server too: read again each time */
    const bool scx = ext_needed || sched_ext_active();

    if (hold_find(fair.key) && (!scx || hold_find(ext.key))) {
        return true;
    }
    if (hold_find(fair.key) && !bound_why[0]) {
        *why = "a sched_ext scheduler started after it was set";
    } else {
        *why = bound_why[0] ? bound_why : "the fair server is not set";
    }
    return false;
}

/* --- GPU clock floor (amdgpu) --- */

bool od_parse(const char *text, struct od_table *od)
{
    const char *line = text;
    int section = 0;    /* 1: OD_SCLK, 2: OD_RANGE, 0: another */
    int found = 0;

    while (*line) {
        size_t len = strcspn(line, "\n");
        char buf[128];
        size_t blen;
        unsigned a, b;

        snprintf(buf, sizeof(buf), "%.*s", (int)(len < sizeof(buf) ? len : sizeof(buf) - 1),
                 line);
        blen = strlen(buf);
        if (strcmp(buf, "OD_SCLK:") == 0) {
            section = 1;
        } else if (strcmp(buf, "OD_RANGE:") == 0) {
            section = 2;
        } else if (blen && buf[blen - 1] == ':' && buf[0] >= 'A' && buf[0] <= 'Z') {
            /* OD_MCLK:, OD_VDDGFX_OFFSET:... */
            section = 0;
        } else if (section == 1 && sscanf(buf, "0: %u", &a) == 1) {
            od->min = a;
            found |= 1;
        } else if (section == 1 && sscanf(buf, "1: %u", &a) == 1) {
            od->max = a;
            found |= 2;
        } else if (section == 2 && sscanf(buf, "SCLK: %uMhz %u", &a, &b) == 2) {
            od->range_lo = a;
            od->range_hi = b;
            found |= 4;
        } else if (section == 2 && sscanf(buf, "SCLK: %uMHz %u", &a, &b) == 2) {
            od->range_lo = a;
            od->range_hi = b;
            found |= 4;
        }
        line += len + (line[len] == '\n');
    }
    return found == 7 && od->range_lo <= od->range_hi;
}

static void card_attr(char *out, size_t size, const char *card, const char *attr)
{
    snprintf(out, size, DRM_DIR "/%s/device/%s", card, attr);
}

static bool card_od(const char *card, struct od_table *od)
{
    char path[PATH_MAX], text[1024];

    card_attr(path, sizeof(path), card, "pp_od_clk_voltage");
    return read_attr(path, text, sizeof(text)) > 0 && od_parse(text, od);
}

static bool card_level(const char *card, char *level, size_t size)
{
    char path[PATH_MAX];

    card_attr(path, sizeof(path), card, "power_dpm_force_performance_level");
    return read_word(path, level, size);
}

static int card_write(const char *card, const char *attr, const char *value)
{
    char path[PATH_MAX];

    card_attr(path, sizeof(path), card, attr);
    return write_attr(path, value);
}

/*
 * An APU: amdgpu describes its metrics with the APU layouts
 * (gpu_metrics_v2_x, v3_x), a discrete GPU with v1_x
 */
static bool card_is_apu(const char *card)
{
    char path[PATH_MAX], buf[64];

    card_attr(path, sizeof(path), card, "gpu_metrics");
    /* structure_size (16 bits), format_revision, content_revision */
    return read_attr(path, buf, sizeof(buf)) >= 4 &&
           ((unsigned char)buf[2] == 2 || (unsigned char)buf[2] == 3);
}

/* Why the floor cannot be set on @card, or NULL */
static const char *card_unavailable(const char *card, struct od_table *od)
{
    char path[PATH_MAX], full[PATH_MAX], link[PATH_MAX], vendor[16];
    const char *driver;
    ssize_t n;

    card_attr(path, sizeof(path), card, "vendor");
    if (!read_word(path, vendor, sizeof(vendor))) {
        return "no such card";
    }
    if (strcmp(vendor, "0x1002") != 0) {
        return "not an AMD GPU";
    }
    card_attr(path, sizeof(path), card, "driver");
    if (!rooted(full, sizeof(full), path) ||
        (n = readlink(full, link, sizeof(link) - 1)) <= 0) {
        return "no driver";
    }
    link[n] = '\0';
    driver = strrchr(link, '/') ? strrchr(link, '/') + 1 : link;
    if (strcmp(driver, "amdgpu") != 0) {
        return "not driven by amdgpu";
    }
    if (!card_od(card, od)) {
        return "no overdrive clock table (pp_od_clk_voltage)";
    }
    return NULL;
}

/*
 * @card's overdrive clocks back to @min - @max, committed, then its level
 * @level: what it had, exactly - never "r", which resets the whole
 * overdrive table, a user's undervolt or memory clocks with it.  @min 0:
 * the table is not known (a state of an older helper), "r" then.  0, or
 * the first -errno (the level is written whatever came before).
 */
static int card_put_back(const char *card, unsigned min, unsigned max, const char *level)
{
    char cmd[48];
    int err, lerr;

    if (min) {
        snprintf(cmd, sizeof(cmd), "s 0 %u\n", min);
        err = card_write(card, "pp_od_clk_voltage", cmd);
        if (!err) {
            snprintf(cmd, sizeof(cmd), "s 1 %u\n", max);
            err = card_write(card, "pp_od_clk_voltage", cmd);
        }
    } else {
        err = card_write(card, "pp_od_clk_voltage", "r\n");
    }
    /* committed: edits not committed yet may hold the floor still */
    if (!err) {
        err = card_write(card, "pp_od_clk_voltage", "c\n");
    }
    snprintf(cmd, sizeof(cmd), "%s\n", level);
    lerr = card_write(card, "power_dpm_force_performance_level", cmd);
    return lerr ? lerr : err;
}

/* Under the lock, as the last holder or for a dead one */
static void gpu_restore(const char *card)
{
    char key[32], level[32], saved[32] = "";
    unsigned min = 0, max = 0, was_min = 0, was_max = 0;
    struct od_table od;
    char *text;
    int n;

    snprintf(key, sizeof(key), "gpu-floor-%s", card);
    if (!(text = state_read(key))) {
        return;
    }
    /* "was": the table before; not in the states of an older helper */
    n = sscanf(text, "level %31s min %u max %u was %u %u", saved, &min, &max, &was_min,
               &was_max);
    if (n < 3 || strcmp(saved, "manual") == 0) {
        free(text);
        state_remove(key);
        return;
    }
    free(text);
    /*
     * Ours: level manual with the table as written - or as it was, the
     * table of a helper killed in its own sequence (after "manual", before
     * the commit; or putting it back, after the reset).  The level was auto
     * before (gpu_floor() leaves any other alone), so manual with either
     * table is this helper's doing, and the level stuck at manual would
     * keep every later floor away ("the performance level is manual").
     */
    if (card_level(card, level, sizeof(level)) && strcmp(level, "manual") == 0 &&
        card_od(card, &od) &&
        ((od.min == min && od.max == max) ||
         (n == 5 && od.min == was_min && od.max == was_max))) {
        char now[32];
        int err = card_put_back(card, n == 5 ? was_min : 0, was_max, saved);

        reply("restored gpu-floor %s: level %s%s", card, saved, err ? " (some writes failed)" : "");
        sys_log("%s: clock floor %u MHz off, level %s", card, min, saved);
        if (!card_level(card, now, sizeof(now)) || strcmp(now, "manual") == 0) {
            /* still manual: the next helper tries again */
            return;
        }
    } else if (card_level(card, level, sizeof(level)) && strcmp(level, saved) == 0) {
        reply("restored gpu-floor %s: level %s already", card, saved);
    } else {
        reply("left gpu-floor %s: changed by someone else since", card);
        sys_log("%s: clock floor left as is: changed by someone else since", card);
    }
    state_remove(key);
}

static bool card_name_ok(const char *card)
{
    u64 n;

    return strncmp(card, "card", 4) == 0 && strlen(card) <= 8 && parse_uint(card + 4, 9999, &n);
}

void gpu_floor(const char *card, const char *value)
{
    struct od_table od, now = {0, 0, 0, 0};
    struct hold *h;
    const char *why;
    char key[32], level[32], text[96], cmd[48];
    unsigned mhz = 0;
    bool first;
    u64 v;
    int err;

    if (!card_name_ok(card)) {
        reply("error gpu-floor: give the card as cardN");
        return;
    }
    snprintf(key, sizeof(key), "gpu-floor-%s", card);
    h = hold_find(key);
    if (strcmp(value, "off") == 0) {
        if (h) {
            hold_drop(h);
        }
        reply("ok gpu-floor %s off", card);
        return;
    }
    if (strcmp(value, "auto") != 0 && !parse_uint(value, 100000, &v)) {
        reply("error gpu-floor %s: give a clock in MHz, auto or off", card);
        return;
    }
    if (h) {
        if (strcmp(h->value, value) == 0) {
            reply("ok gpu-floor %s %s: already", card, value);
            return;
        }
        /* another floor: the old one goes first */
        hold_drop(h);
    }
    if ((why = card_unavailable(card, &od))) {
        reply("skip gpu-floor %s: %s", card, why);
        return;
    }
    if (strcmp(value, "auto") == 0) {
        /* measured on a Radeon 780M; discrete GPUs: nothing until measured */
        if (!card_is_apu(card)) {
            reply("skip gpu-floor %s: auto sets a floor on AMD APUs only", card);
            return;
        }
        mhz = GPU_FLOOR_AUTO_MHZ;
        if (mhz > od.range_hi) {
            reply("skip gpu-floor %s: auto: %u MHz is above the GPU's range", card, mhz);
            return;
        }
    } else {
        mhz = (unsigned)v;
        if (mhz < od.range_lo || mhz > od.range_hi) {
            reply("error gpu-floor %s: %u MHz is outside %u-%u MHz", card, mhz, od.range_lo,
                  od.range_hi);
            return;
        }
    }

    lock_all();
    if (!(h = hold_take(key, value, &first))) {
        int err = errno;

        unlock_all();
        reply("error gpu-floor %s: cannot take the hold: %s", card, strerror(err));
        return;
    }
    if (!first) {
        unlock_all();
        reply("ok gpu-floor %s: set by another vitrine session", card);
        return;
    }
    /* the hold's restore may have changed them: read them again */
    why = NULL;
    if (!card_level(card, level, sizeof(level)) || !card_od(card, &od)) {
        why = "cannot read the GPU's power settings";
    } else if (strcmp(level, "auto") != 0) {
        /* a level someone chose (manual: by hand or another tool): left as it is */
        snprintf(text, sizeof(text), "the performance level is %.32s, not auto", level);
        why = text;
    } else if (mhz <= od.min) {
        snprintf(text, sizeof(text), "the minimum is already %u MHz", od.min);
        why = text;
    }
    if (why) {
        hold_close(h, true);
        unlock_all();
        reply("skip gpu-floor %s: %s", card, why);
        return;
    }
    snprintf(text, sizeof(text), "level %s\nmin %u\nmax %u\nwas %u %u\n", level, mhz,
             od.range_hi, od.min, od.max);
    if (!state_write(key, text)) {
        err = errno;
        hold_close(h, true);
        unlock_all();
        reply("error gpu-floor %s: %s", card, strerror(err));
        return;
    }
    /* overdrive edits take in manual only; the maximum too: after the first
       switch to manual the driver's is 0 and the commit fails */
    err = card_write(card, "power_dpm_force_performance_level", "manual\n");
    if (!err) {
        snprintf(cmd, sizeof(cmd), "s 0 %u\n", mhz);
        err = card_write(card, "pp_od_clk_voltage", cmd);
    }
    if (!err) {
        snprintf(cmd, sizeof(cmd), "s 1 %u\n", od.range_hi);
        err = card_write(card, "pp_od_clk_voltage", cmd);
    }
    if (!err) {
        err = card_write(card, "pp_od_clk_voltage", "c\n");
    }
    /* read back: the driver may take a value and keep another */
    if (!err && (!card_od(card, &now) || now.min != mhz)) {
        err = -EIO;
    }
    if (err) {
        card_put_back(card, od.min, od.max, level);
        state_remove(key);
        hold_close(h, true);
        unlock_all();
        if (err == -EIO) {
            reply("error gpu-floor %s: the floor did not take (the lowest clock reads %u MHz)",
                  card, now.min);
        } else {
            reply("error gpu-floor %s: %s", card, strerror(-err));
        }
        return;
    }
    hold_share(h);
    unlock_all();
    sys_log("uid %u: %s clock floor %u MHz (was %u MHz, level %s)", (unsigned)caller_uid, card,
            mhz, od.min, level);
    reply("ok gpu-floor %s %u MHz (was %u MHz, level %s)", card, mhz, od.min, level);
}

/* --- udmabuf limits --- */

/*
 * The module's parameters, read by the kernel at each UDMABUF_CREATE_LIST:
 * a change counts for the next blob, nothing to restart.  Both are ints
 * (mode 0644), and /dev/udmabuf itself is the desktop user's already
 * (systemd's uaccess): raising them only lets that user's udmabufs be
 * bigger, in pages they own anyway.
 */
static const struct {
    const char *name;
    u64 target;
} udmabuf_params[] = {
    {"list_limit", UDMABUF_LIST_LIMIT},
    {"size_limit_mb", UDMABUF_SIZE_LIMIT_MB},
};
#define NUDMABUF (int)(sizeof(udmabuf_params) / sizeof(udmabuf_params[0]))

static void udmabuf_path(char *out, size_t size, const char *name)
{
    snprintf(out, size, UDMABUF_DIR "/%s", name);
}

static int udmabuf_index(const char *name)
{
    for (int i = 0; i < NUDMABUF; i++) {
        if (strcmp(udmabuf_params[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* @what with ", " before it if @text has something already */
static void append(char *text, size_t size, const char *what)
{
    size_t len = strlen(text);

    snprintf(text + len, size - len, "%s%s", len ? ", " : "", what);
}

/* Under the lock, as the last holder or for a dead one: the state back */
static void udmabuf_restore(void)
{
    char *text = state_read("udmabuf"), *line, *save = NULL;
    char back[128] = "", left[64] = "";
    bool failed = false;

    if (!text) {
        return;
    }
    /* "NAME WAS WROTE" per parameter it raised */
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char name[16], path[PATH_MAX], what[48];
        u64 was, wrote, now;

        if (sscanf(line, "%15s %llu %llu", name, &was, &wrote) != 3 || udmabuf_index(name) < 0) {
            continue;
        }
        udmabuf_path(path, sizeof(path), name);
        snprintf(what, sizeof(what), "%s %llu", name, was);
        if (!read_u64(path, &now)) {
            failed = true;
        } else if (now == wrote) {
            if (write_u64(path, was) == 0) {
                append(back, sizeof(back), what);
            } else {
                failed = true;
            }
        } else if (now == was) {
            /* killed before writing it: nothing to undo */
            append(back, sizeof(back), what);
        } else {
            append(left, sizeof(left), name);
        }
    }
    free(text);
    state_remove("udmabuf");
    if (back[0] || failed) {
        reply("restored udmabuf: %s%s", back[0] ? back : "nothing", failed ? " (some failed)" : "");
    }
    if (left[0]) {
        reply("left udmabuf: %s changed by someone else since", left);
    }
    sys_log("udmabuf: back to %s%s%s%s", back[0] ? back : "nothing",
            left[0] ? "; left as changed since: " : "", left, failed ? "; some failed" : "");
}

void udmabuf_on(const char *arg)
{
    struct watched *w = find_watched(arg);
    char path[PATH_MAX], text[128], said[160] = "", state[128] = "";
    u64 was[NUDMABUF], wrote[NUDMABUF], now;
    const char *why = NULL;
    struct stat st;
    struct hold *h;
    bool first;
    int changed = 0, err = 0;

    if (!w) {
        reply("error udmabuf: watch the process first");
        return;
    }
    if (hold_find("udmabuf")) {
        w->udmabuf = true;
        reply("ok udmabuf %d: already", (int)w->pid);
        return;
    }
    /* built in (Fedora) or loaded: /sys/module/udmabuf/parameters; a module
       not loaded yet has none, and QEMU then gets the defaults it loads with */
    if (!rooted(path, sizeof(path), UDMABUF_DIR) || stat(path, &st) < 0) {
        reply("skip udmabuf %d: the udmabuf module is not loaded", (int)w->pid);
        return;
    }
    lock_all();
    if (!(h = hold_take("udmabuf", "on", &first))) {
        err = errno;
        unlock_all();
        reply("error udmabuf %d: cannot take the hold: %s", (int)w->pid, strerror(err));
        return;
    }
    if (!first) {
        unlock_all();
        w->udmabuf = true;
        reply("ok udmabuf %d: set by another vitrine session", (int)w->pid);
        return;
    }
    /* what they are, saved before anything is written; raised only, a
       higher value (the kernel's command line, say) stays */
    for (int i = 0; i < NUDMABUF; i++) {
        udmabuf_path(path, sizeof(path), udmabuf_params[i].name);
        wrote[i] = 0;
        if (!read_u64(path, &was[i])) {
            why = "cannot read its limits";
            goto fail;
        }
        if (was[i] < udmabuf_params[i].target) {
            wrote[i] = udmabuf_params[i].target;
            snprintf(text, sizeof(text), "%s %llu %llu\n", udmabuf_params[i].name, was[i], wrote[i]);
            strcat(state, text);
        }
    }
    if (state[0] && !state_write("udmabuf", state)) {
        err = errno;
        goto fail;
    }
    for (int i = 0; i < NUDMABUF; i++) {
        int e;

        if (!wrote[i]) {
            continue;
        }
        udmabuf_path(path, sizeof(path), udmabuf_params[i].name);
        if ((e = write_u64(path, wrote[i])) == 0 && (!read_u64(path, &now) || now != wrote[i])) {
            /* taken, not kept: back, and said */
            write_u64(path, was[i]);
            e = -EIO;
            why = "a limit did not take";
        }
        if (e < 0) {
            err = -e;
            /* the ones written before it back: all or nothing */
            for (int j = 0; j < i; j++) {
                if (wrote[j]) {
                    udmabuf_path(path, sizeof(path), udmabuf_params[j].name);
                    write_u64(path, was[j]);
                }
            }
            goto fail;
        }
        changed++;
    }
    hold_share(h);
    unlock_all();
    w->udmabuf = true;
    for (int i = 0; i < NUDMABUF; i++) {
        if (wrote[i]) {
            snprintf(text, sizeof(text), "%s %llu (was %llu)", udmabuf_params[i].name, wrote[i],
                     was[i]);
        } else {
            snprintf(text, sizeof(text), "%s %llu already", udmabuf_params[i].name, was[i]);
        }
        append(said, sizeof(said), text);
    }
    if (changed) {
        sys_log("uid %u: udmabuf %s", (unsigned)caller_uid, said);
    }
    reply("ok udmabuf %d: %s", (int)w->pid, said);
    return;
fail:
    state_remove("udmabuf");
    hold_close(h, true);
    unlock_all();
    reply("error udmabuf %d: %s", (int)w->pid, why ? why : strerror(err ? err : EIO));
}

void udmabuf_check(void)
{
    struct hold *h = hold_find("udmabuf");

    if (!h) {
        return;
    }
    for (int i = 0; i < nwatched; i++) {
        if (watched[i].udmabuf) {
            return;
        }
    }
    hold_drop(h);
}

/* --- transparent huge pages for shared memory --- */

/*
 * shmem_enabled reads "always within_size advise [never] deny force": the
 * choice in brackets.  Writing a word chooses it; it is read by the kernel
 * at each fault, a change counts at once.  "deny" and "force" are the
 * kernel's switches for emergencies and tests.
 */
static bool thp_word(const char *word)
{
    static const char *const words[] = {"always", "within_size", "advise", "never",
                                        "deny",   "force",       NULL};

    for (int i = 0; words[i]; i++) {
        if (strcmp(word, words[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* The choice now, one of the kernel's words; false when there is none to
   read (a kernel without transparent huge pages) */
static bool thp_get(char *word, size_t size)
{
    char buf[256];
    const char *lb, *rb;

    if (read_attr(THP_SHMEM, buf, sizeof(buf)) <= 0 || !(lb = strchr(buf, '[')) ||
        !(rb = strchr(lb, ']')) || (size_t)(rb - lb - 1) >= size) {
        return false;
    }
    snprintf(word, size, "%.*s", (int)(rb - lb - 1), lb + 1);
    return thp_word(word);
}

/* @word chosen and read back: 0 or -errno (-EIO: taken, not kept) */
static int thp_set(const char *word)
{
    char cmd[32], now[32];
    int err;

    snprintf(cmd, sizeof(cmd), "%s\n", word);
    if ((err = write_attr(THP_SHMEM, cmd)) < 0) {
        return err;
    }
    return thp_get(now, sizeof(now)) && strcmp(now, word) == 0 ? 0 : -EIO;
}

/*
 * Under the lock, as the last holder or for a dead one: the choice before,
 * where it still reads what was written.  One that cannot be put back now
 * stays recorded, for the next helper.
 */
static void thp_restore(void)
{
    char *text = state_read("thp"), was[32], wrote[32], now[32];

    if (!text) {
        return;
    }
    /* "was WORD" and "wrote WORD" lines */
    if (sscanf(text, "was %31s wrote %31s", was, wrote) != 2 || !thp_word(was) || !thp_word(wrote)) {
        free(text);
        state_remove("thp");
        return;
    }
    free(text);
    if (!thp_get(now, sizeof(now))) {
        reply("restored thp: shmem_enabled cannot be read now (kept for later)");
        return;
    }
    if (strcmp(now, wrote) == 0) {
        if (thp_set(was) < 0) {
            reply("restored thp: shmem huge pages could not be put back to %s (kept for later)", was);
            sys_log("shmem huge pages: %s could not be put back", was);
            return;
        }
        reply("restored thp: shmem huge pages back to %s", was);
        sys_log("shmem huge pages back to %s", was);
    } else if (strcmp(now, was) == 0) {
        /* killed before writing it, or after putting it back */
        reply("restored thp: shmem huge pages %s already", was);
    } else {
        reply("left thp: changed by someone else since");
        sys_log("shmem huge pages left at %s: changed by someone else since", now);
    }
    state_remove("thp");
}

/* What @now means for this helper, which writes only over "never" */
static void thp_found(const char *now)
{
    if (strcmp(now, "deny") == 0) {
        reply("skip thp: denied on this host (shmem_enabled is deny), left as it is");
    } else {
        /* advise, or more: QEMU's madvise takes already */
        reply("ok thp on: already %s, left as it is", now);
    }
}

void thp_on(void)
{
    char now[32], text[64];
    struct hold *h;
    bool first, readable;
    int err;

    if (hold_find("thp")) {
        reply("ok thp on: already");
        return;
    }
    if (!thp_get(now, sizeof(now))) {
        reply("skip thp: this kernel has no transparent huge pages");
        return;
    }
    /*
     * The hold first, then what the value means: "advise" may be another
     * helper's, which this one joins - else the first one out would put
     * "never" back under this one's VMs.  Not another helper's: the
     * owner's choice, left as it is.
     */
    lock_all();
    if (!(h = hold_take("thp", "on", &first))) {
        err = errno;
        unlock_all();
        reply("error thp: cannot take the hold: %s", strerror(err));
        return;
    }
    if (!first) {
        unlock_all();
        reply("ok thp on: set by another vitrine session");
        return;
    }
    /* the hold's restore (a dead helper's) may have changed it: read again */
    readable = thp_get(now, sizeof(now));
    if (!readable || strcmp(now, "never") != 0) {
        hold_close(h, true);
        unlock_all();
        if (readable) {
            thp_found(now);
        } else {
            reply("error thp: shmem_enabled cannot be read");
        }
        return;
    }
    /* recorded before anything is written */
    snprintf(text, sizeof(text), "was %s\nwrote advise\n", now);
    if (!state_write("thp", text)) {
        err = errno;
        hold_close(h, true);
        unlock_all();
        reply("error thp: %s", strerror(err));
        return;
    }
    if ((err = thp_set("advise")) < 0) {
        /* back as it was, if the write took at all */
        thp_set(now);
        state_remove("thp");
        hold_close(h, true);
        unlock_all();
        reply("error thp: %s", err == -EIO ? "the setting did not take" : strerror(-err));
        return;
    }
    hold_share(h);
    unlock_all();
    sys_log("uid %u: shmem huge pages advise (was %s)", (unsigned)caller_uid, now);
    reply("ok thp on: advise (was %s)", now);
}

void thp_off(void)
{
    struct hold *h = hold_find("thp");

    if (h) {
        hold_drop(h);
    }
    reply("ok thp off");
}

/* --- the CPU ready to wake up: the idle latency request --- */

/*
 * Not a hold: the request is this helper's open file, and the kernel takes
 * the lowest of every program's requests (other helpers', tuned's, the
 * research side's), dropping each one with its file - nothing to share,
 * record or recover, and nobody else can change it.
 */
static int latency_fd = -1;
static unsigned latency_us;

/* The online CPUs, read again by each pass that needs them (one at a time) */
static struct online online_now;

/*
 * CPU @cpu's idle state @state, from cpuidle: false when it has no such
 * state.  @usable: not POLL (the busy loop that is all a CPU has left with
 * no idle state allowed), not disabled (by the user, or by the driver's
 * default), its wake-up time known; @us that time (its exit latency, which
 * the kernel compares with the requests).
 */
static bool idle_state(unsigned cpu, unsigned state, char *name, size_t size, bool *usable,
                       u64 *us)
{
    char path[PATH_MAX];
    u64 disabled = 0;

    snprintf(path, sizeof(path), CPU_DIR "/cpu%u/cpuidle/state%u/name", cpu, state);
    if (!read_word(path, name, size)) {
        return false;
    }
    snprintf(path, sizeof(path), CPU_DIR "/cpu%u/cpuidle/state%u/disable", cpu, state);
    read_u64(path, &disabled);
    snprintf(path, sizeof(path), CPU_DIR "/cpu%u/cpuidle/state%u/latency", cpu, state);
    *usable = read_u64(path, us) && strcmp(name, "POLL") != 0 && !disabled;
    return true;
}

/* CPU @cpu's first usable idle state (C1, mostly): its wake-up time in @us,
   its name in @name; false when it has none */
static bool first_idle(unsigned cpu, u64 *us, char *name, size_t size)
{
    bool usable;

    for (unsigned s = 0; s < 64 && idle_state(cpu, s, name, size, &usable, us); s++) {
        if (usable) {
            return true;
        }
    }
    return false;
}

/* The usable idle states of CPU @cpu slower to wake than @us, by name:
   "C2, C3" */
static void idle_deeper(unsigned cpu, u64 us, char *out, size_t size)
{
    char name[32];
    bool usable;
    u64 lat;

    out[0] = '\0';
    for (unsigned s = 0; s < 64 && idle_state(cpu, s, name, sizeof(name), &usable, &lat); s++) {
        if (usable && lat > us) {
            append(out, size, name);
        }
    }
}

void cpu_latency_on(void)
{
    char path[PATH_MAX], name[32], slowest[32] = "", deeper[128], what[200], driver[32];
    unsigned *ids = NULL, slow_cpu = 0;
    int n, found = 0, without = 0, fd;
    bool same = true;
    u64 us = 0, lat;
    struct stat st;

    if (latency_fd >= 0) {
        reply("ok cpu-latency on: already %u us", latency_us);
        return;
    }
    if (!rooted(path, sizeof(path), CPU_LATENCY_DEV) || stat(path, &st) < 0) {
        reply("skip cpu-latency: this kernel has no " CPU_LATENCY_DEV);
        return;
    }
    /*
     * The largest of the online CPUs' first wake-up times: each of them may
     * rest in its first idle state.  A request below a CPU's would leave it
     * only POLL - a fixed 1 us, on Intel CPUs whose C1 takes 2 - which keeps
     * keys no faster for some 26 W more.
     */
    online_read(&online_now);
    n = cpu_dirs(CPU_DIR, &ids);
    for (int i = 0; i < n; i++) {
        if (!online_has(&online_now, ids[i])) {
            continue;
        }
        if (!first_idle(ids[i], &lat, name, sizeof(name))) {
            without++;
            continue;
        }
        same &= !found || (lat == us && strcmp(name, slowest) == 0);
        if (!found || lat > us) {
            us = lat;
            slow_cpu = ids[i];
            snprintf(slowest, sizeof(slowest), "%s", name);
        }
        found++;
    }
    free(ids);
    if (!found) {
        if (read_word(CPUIDLE_DRIVER, driver, sizeof(driver)) && strcmp(driver, "none") != 0) {
            reply("skip cpu-latency: the cpus have no idle state but polling (cpuidle driver %s)",
                  driver);
        } else {
            reply("skip cpu-latency: the cpus have no idle states (no cpuidle driver)");
        }
        return;
    }
    idle_deeper(slow_cpu, us, deeper, sizeof(deeper));
    /* never 0: with a request of 0 the idle governors only poll */
    us = us < 1 ? 1 : us > 2000000000ULL ? 2000000000ULL : us;
    fd = sys_latency_request(path, (int)us);
    if (fd < 0) {
        reply("error cpu-latency: " CPU_LATENCY_DEV ": %s", strerror(-fd));
        return;
    }
    latency_fd = fd;
    latency_us = (unsigned)us;
    if (!same) {
        snprintf(what, sizeof(what), "%s of cpu%u, the slowest first idle state%s", slowest,
                 slow_cpu, without ? "; none on some cpus" : "");
    } else if (without) {
        snprintf(what, sizeof(what), "%s on %d cpus, none on %d", slowest, found, without);
    } else {
        snprintf(what, sizeof(what), "%s on every cpu", slowest);
    }
    if (deeper[0]) {
        snprintf(what + strlen(what), sizeof(what) - strlen(what), "; %s unused", deeper);
    }
    sys_log("uid %u: cpu latency request %u us (%s)", (unsigned)caller_uid, latency_us, what);
    reply("ok cpu-latency on: %u us (%s)", latency_us, what);
}

/* The request let go, if this helper holds one */
static void latency_release(void)
{
    if (latency_fd < 0) {
        return;
    }
    sys_latency_release(latency_fd);
    latency_fd = -1;
    reply("restored cpu-latency: request of %u us ended", latency_us);
    sys_log("cpu latency request of %u us ended", latency_us);
}

void cpu_latency_off(void)
{
    latency_release();
    reply("ok cpu-latency off");
}

/* --- the CPUs' energy performance preference --- */

/*
 * cpufreq/energy_performance_preference of each CPU, in amd-pstate's and
 * intel_pstate's active modes: one of the drivers' words (performance,
 * balance_performance, balance_power, power) or, where the driver takes
 * raw values, a number up to 255, which it then shows.  An offline CPU
 * answers -EBUSY (its policy is inactive); the drivers keep its value
 * meanwhile and give it back when it comes online.  The record, epp.state,
 * has a line per CPU: "cpuN WAS performance", or "cpuN WAS -" for one found
 * at performance already.
 */
static void epp_path(char *out, size_t size, unsigned cpu)
{
    snprintf(out, size, CPU_DIR "/cpu%u/cpufreq/energy_performance_preference", cpu);
}

/* A value as the drivers show and take it: a word, or a number to 255 */
static bool epp_value(const char *v)
{
    const size_t len = strlen(v);
    bool word = len > 0 && len < 32, number = word;
    u64 n;

    for (const char *c = v; *c; c++) {
        word &= (*c >= 'a' && *c <= 'z') || *c == '_';
        number &= *c >= '0' && *c <= '9';
    }
    return word || (number && parse_uint(v, 255, &n));
}

/* CPU @cpu's preference in @buf: 0, or -errno - -ENOENT: it has none,
   -EBUSY: offline, -EINVAL: not a value of the drivers */
static int epp_get(unsigned cpu, char *buf, size_t size)
{
    char path[PATH_MAX];
    int n;

    epp_path(path, sizeof(path), cpu);
    if ((n = read_attr(path, buf, size)) < 0) {
        return n;
    }
    buf[strcspn(buf, "\n")] = '\0';
    return epp_value(buf) ? 0 : -EINVAL;
}

static int epp_put(unsigned cpu, const char *value)
{
    char path[PATH_MAX], cmd[40];

    epp_path(path, sizeof(path), cpu);
    snprintf(cmd, sizeof(cmd), "%s\n", value);
    return write_attr(path, cmd);
}

/* The record @rec has a line for CPU @cpu */
static bool epp_listed(const char *rec, unsigned cpu)
{
    for (const char *l = rec; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        unsigned id;

        if (sscanf(l, "cpu%u ", &id) == 1 && id == cpu) {
            return true;
        }
    }
    return false;
}

/* @value added to @list ("a, b") unless it is there already */
static void note_value(char *list, size_t size, const char *value)
{
    const size_t vlen = strlen(value);

    for (const char *v = list; *v;) {
        const size_t len = strcspn(v, ",");

        if (len == vlen && strncmp(v, value, vlen) == 0) {
            return;
        }
        v += len;
        v += strspn(v, ", ");
    }
    append(list, size, value);
}

/* What a pass of epp_apply() found and did */
struct epp_pass {
    int written;    /* CPUs set to EPP_TARGET */
    int already;    /* found at EPP_TARGET */
    int offline;    /* offline: taken by a later pass, once online */
    int none;       /* online, without a preference */
    char was[96];   /* the values of those written */
};

/*
 * Under the lock, by a holder: each online CPU with a preference that the
 * record @rec (NULL: none yet) does not list - every one at first, then
 * those come online since - at EPP_TARGET.  Recorded before anything is
 * written, a CPU found there already too, so that a later pass leaves it
 * to whoever changes it next.  All of them or none: after a refusal those
 * written are put back, and those that could not be stay recorded for a
 * helper to put back.  Nothing recorded when there was no record and
 * nothing to write.  0, or -1 with why in @why.
 */
static int epp_apply(const char *rec, struct epp_pass *p, char *why, size_t size)
{
    struct todo {
        unsigned id;
        char was[64];
        bool back;
    } *todo = NULL;
    const size_t reclen = rec ? strlen(rec) : 0;
    unsigned *ids = NULL;
    char *text = NULL;
    int n, ntodo = 0, rc = -1;
    size_t len;

    memset(p, 0, sizeof(*p));
    online_read(&online_now);
    if ((n = cpu_dirs(CPU_DIR, &ids)) < 0) {
        snprintf(why, size, CPU_DIR ": %s", strerror(-n));
        return -1;
    }
    todo = calloc((size_t)n + 1, sizeof(*todo));
    text = malloc(reclen + (size_t)n * 64 + 1);
    if (!todo || !text) {
        snprintf(why, size, "%s", strerror(ENOMEM));
        goto out;
    }
    len = (size_t)sprintf(text, "%s", rec ? rec : "");
    for (int i = 0; i < n; i++) {
        char now[64];
        int err;

        if (epp_listed(rec, ids[i])) {
            continue;
        }
        if (!online_has(&online_now, ids[i])) {
            p->offline++;
            continue;
        }
        err = epp_get(ids[i], now, sizeof(now));
        if (err == -ENOENT) {
            p->none++;
            continue;
        }
        if (err == -EBUSY) {
            /* going offline */
            p->offline++;
            continue;
        }
        if (err < 0) {
            snprintf(why, size, "cpu%u's preference %s", ids[i],
                     err == -EINVAL ? "is not a value it takes back" : "cannot be read");
            goto out;
        }
        if (strcmp(now, EPP_TARGET) == 0) {
            p->already++;
            len += (size_t)sprintf(text + len, "cpu%u %s -\n", ids[i], now);
            continue;
        }
        todo[ntodo].id = ids[i];
        snprintf(todo[ntodo].was, sizeof(todo[ntodo].was), "%s", now);
        ntodo++;
        len += (size_t)sprintf(text + len, "cpu%u %s " EPP_TARGET "\n", ids[i], now);
    }
    if (!ntodo && !rec) {
        rc = 0;
        goto out;
    }
    /* recorded before anything is written */
    if (len > reclen && !state_write("epp", text)) {
        snprintf(why, size, "%s", strerror(errno));
        goto out;
    }
    for (int i = 0; i < ntodo; i++) {
        char now[64];
        int err = epp_put(todo[i].id, EPP_TARGET);

        /* read back: a driver may take a value and keep another */
        if (!err && (epp_get(todo[i].id, now, sizeof(now)) < 0 || strcmp(now, EPP_TARGET) != 0)) {
            err = -EIO;
        }
        if (!err) {
            note_value(p->was, sizeof(p->was), todo[i].was);
            p->written++;
            continue;
        }
        if (err == -EIO) {
            snprintf(why, size, "cpu%u does not keep what was written", todo[i].id);
        } else if (err == -EBUSY) {
            snprintf(why, size, "cpu%u refuses it (busy: offline, or its driver sets it itself)",
                     todo[i].id);
        } else {
            snprintf(why, size, "cpu%u: %s", todo[i].id, strerror(-err));
        }
        /* all or nothing: this pass's CPUs back, this one too (a refused
           write may have taken); those not written are as they were */
        for (int j = 0; j < ntodo; j++) {
            char cur[64];

            todo[j].back = j > i || (epp_get(todo[j].id, cur, sizeof(cur)) == 0 &&
                                     (strcmp(cur, todo[j].was) == 0 ||
                                      (strcmp(cur, EPP_TARGET) == 0 &&
                                       epp_put(todo[j].id, todo[j].was) == 0)));
        }
        /* the record: what it had, and the CPUs not back */
        len = (size_t)sprintf(text, "%s", rec ? rec : "");
        for (int j = 0; j < ntodo; j++) {
            if (!todo[j].back) {
                len += (size_t)sprintf(text + len, "cpu%u %s " EPP_TARGET "\n", todo[j].id,
                                       todo[j].was);
            }
        }
        if (len) {
            state_write("epp", text);
        } else {
            state_remove("epp");
        }
        p->written = p->already = 0;
        p->was[0] = '\0';
        goto out;
    }
    rc = 0;
out:
    free(ids);
    free(todo);
    free(text);
    return rc;
}

/*
 * Under the lock, as the last holder or for a dead one: each CPU that
 * still reads EPP_TARGET back to the value it had.  One that cannot be put
 * back now (offline, say) stays recorded, for the next helper.
 */
static void epp_restore(void)
{
    char *text = state_read("epp"), *line, *save = NULL, *keep, back[96] = "";
    int restored = 0, left = 0, kept = 0;
    size_t klen = 0;

    if (!text) {
        return;
    }
    /* each line kept with its newline, the last one's too */
    if (!(keep = malloc(strlen(text) + 2))) {
        /* nothing lost: the next helper tries again */
        free(text);
        return;
    }
    keep[0] = '\0';
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char was[64], wrote[64], now[64];
        unsigned id;
        int err;

        /* "cpuN WAS -": found at EPP_TARGET, nothing to put back */
        if (sscanf(line, "cpu%u %63s %63s", &id, was, wrote) != 3 || !epp_value(was) ||
            strcmp(wrote, EPP_TARGET) != 0) {
            continue;
        }
        err = epp_get(id, now, sizeof(now));
        if (err == 0 && (strcmp(now, was) == 0 ||
                         (strcmp(now, EPP_TARGET) == 0 && epp_put(id, was) == 0))) {
            /* put back - or as it was: a helper killed before writing it,
               or after putting it back */
            restored++;
            note_value(back, sizeof(back), was);
        } else if ((err == 0 && strcmp(now, EPP_TARGET) != 0) || err == -ENOENT) {
            /* another value since (a power profile chosen meanwhile), or no
               preference any more (the driver's mode changed) */
            left++;
        } else {
            /* offline now, not readable, or not taken back */
            kept++;
            klen += (size_t)sprintf(keep + klen, "%s\n", line);
        }
    }
    free(text);
    if (klen) {
        state_write("epp", keep);
    } else {
        state_remove("epp");
    }
    free(keep);
    if (restored && kept) {
        reply("restored epp: %d cpus back to %s; %d kept for later (offline or refused)", restored,
              back, kept);
    } else if (restored) {
        reply("restored epp: %d cpus back to %s", restored, back);
    } else if (kept) {
        reply("restored epp: %d cpus kept for later (offline or refused)", kept);
    }
    if (left) {
        reply("left epp: %d cpus changed by someone else since", left);
    }
    sys_log("energy performance preference: %d cpus back to %s, %d left as changed since, %d kept "
            "for later", restored, back[0] ? back : "-", left, kept);
}

/* Why no online CPU has a preference: its cpufreq driver, if it has one */
static void epp_none(char *why, size_t size)
{
    char path[PATH_MAX], driver[32];
    unsigned *ids = NULL;
    int n = cpu_dirs(CPU_DIR, &ids);
    bool found = false;

    for (int i = 0; i < n && !found; i++) {
        snprintf(path, sizeof(path), CPU_DIR "/cpu%u/cpufreq/scaling_driver", ids[i]);
        found = online_has(&online_now, ids[i]) && read_word(path, driver, sizeof(driver));
    }
    free(ids);
    if (found) {
        snprintf(why, size, "%s has no energy performance preference (amd-pstate and "
                 "intel_pstate have one, in active mode)", driver);
    } else {
        snprintf(why, size, "no cpufreq driver, so no energy performance preference");
    }
}

void epp_on(void)
{
    struct hold *h = hold_find("epp");
    const bool had = h != NULL;
    char why[200], extra[160] = "", *rec;
    struct epp_pass p;
    bool first = false;
    int err, rc;

    lock_all();
    if (!h && !(h = hold_take("epp", "on", &first))) {
        err = errno;
        unlock_all();
        reply("error epp: cannot take the hold: %s", strerror(err));
        return;
    }
    /* the record: of the first holder, or one a restore kept for CPUs offline then */
    rec = state_read("epp");
    rc = epp_apply(rec, &p, why, sizeof(why));
    free(rec);
    if (first && (rc < 0 || !p.written)) {
        /* nothing of this helper's to hold: the hold goes, and a record
           kept (CPUs offline, an undo refused) stays for the next helper */
        hold_close(h, true);
        unlock_all();
        if (rc < 0) {
            reply("error epp: %s", why);
        } else if (p.already) {
            reply("ok epp on: already " EPP_TARGET " on %d cpus, left as it is", p.already);
        } else if (!p.none && p.offline) {
            reply("skip epp: no online cpu answers for its preference now (busy)");
        } else {
            epp_none(why, sizeof(why));
            reply("skip epp: %s", why);
        }
        return;
    }
    if (first) {
        hold_share(h);
    }
    unlock_all();
    if (rc < 0) {
        /* a later pass: what was set before holds on */
        reply("error epp: %s", why);
        return;
    }
    if (p.already && first) {
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "; %d at " EPP_TARGET
                 " already", p.already);
    }
    if (p.offline) {
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "; %d offline", p.offline);
    }
    if (p.none) {
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra), "; %d without one", p.none);
    }
    if (p.written) {
        sys_log("uid %u: energy performance preference " EPP_TARGET " on %d cpus (was %s)",
                (unsigned)caller_uid, p.written, p.was);
    }
    if (first) {
        reply("ok epp on: " EPP_TARGET " on %d cpus (was %s)%s", p.written, p.was, extra);
    } else if (p.written) {
        reply("ok epp on: %s; %d cpus online since at " EPP_TARGET " (was %s)",
              had ? "already" : "set by another vitrine session", p.written, p.was);
    } else {
        reply("ok epp on: %s", had ? "already" : "set by another vitrine session");
    }
}

void epp_off(void)
{
    struct hold *h = hold_find("epp");

    if (h) {
        hold_drop(h);
    }
    reply("ok epp off");
}

/* --- all of them --- */

/* The threads of a record's "kept T,T...", into @keep: their count (an
   older record has no "kept": none) */
static int record_kept(const char *text, pid_t *keep)
{
    const char *list = strstr(text, " kept ");
    int nkeep = 0;

    for (const char *t = list ? list + 6 : ""; *t && *t != '\n' && nkeep < MAX_KEPT;) {
        unsigned long long tid;
        char num[16];
        size_t len = strcspn(t, ",\n");

        snprintf(num, sizeof(num), "%.*s", (int)(len < 15 ? len : 15), t);
        if (parse_uint(num, 4194304, &tid)) {
            keep[nkeep++] = (pid_t)tid;
        }
        t += len + (t[len] == ',');
    }
    return nkeep;
}

/* sched-<pid>'s record ("pid P start S niced N rt R [view 1 ]kept T,T..."):
   those threads put back, but the ones kept; view 1: the process that
   started a helper (view-rt), not a QEMU */
static void sched_state_restore(const char *key)
{
    char *text = state_read(key);
    pid_t keep[MAX_KEPT];
    int pid, niced;
    u64 start;

    if (text && sscanf(text, "pid %d start %llu niced %d", &pid, &start, &niced) == 3 &&
        pid > 1) {
        const int nkeep = record_kept(text, keep);
        sched_restore((pid_t)pid, start, niced != 0, keep, nkeep,
                      strstr(text, " view 1 ") ? "view-rt" : "rt");
    }
    free(text);
    state_remove(key);
}

bool sched_record(const struct watched *w, pid_t *kept, int *nkept, bool *niced)
{
    char key[32], *text;
    int pid, n;
    u64 start;
    bool found;

    /* this helper's own, or one it joined: what it knows already */
    if (w->held) {
        return false;
    }
    snprintf(key, sizeof(key), "sched-%d", (int)w->pid);
    lock_all();
    text = state_read(key);
    unlock_all();
    /*
     * The same process (pid and start time), and a writer that knew which
     * threads were real-time before it ("rt 1": it had made them real-time
     * once); a record of an older helper says nothing of it: not taken
     */
    found = text && sscanf(text, "pid %d start %llu niced %d", &pid, &start, &n) == 3 &&
            pid == (int)w->pid && start == w->start && strstr(text, " rt 1 ");
    if (found) {
        *nkept = record_kept(text, kept);
        *niced = n != 0;
    }
    free(text);
    return found;
}

bool sched_hold(struct watched *w)
{
    char key[32], text[96 + MAX_KEPT * 12];
    struct hold *h;
    bool first = false, ok;
    int len;

    snprintf(key, sizeof(key), "sched-%d", (int)w->pid);
    lock_all();
    if (!(h = hold_find(key)) && !(h = hold_take(key, "on", &first))) {
        unlock_all();
        return false;
    }
    /* what to put back: threads made real-time (FIFO 1) but those kept,
       vCPUs niced or not; rt 1: kept is known (rt found them, or another
       helper's record said); view 1: the helper's parent, for the reply */
    len = snprintf(text, sizeof(text), "pid %d start %llu niced %d rt %d %skept ", (int)w->pid,
                   w->start, w->behind ? 1 : 0, w->rt || w->rt_once ? 1 : 0,
                   w->view ? "view 1 " : "");
    for (int i = 0; i < w->nkept; i++) {
        len += snprintf(text + len, sizeof(text) - (size_t)len, "%s%d", i ? "," : "",
                        (int)w->kept[i]);
    }
    snprintf(text + len, sizeof(text) - (size_t)len, "%s\n", w->nkept ? "" : "-");
    ok = state_write(key, text);
    if (!w->held && !ok) {
        hold_close(h, first);
    } else if (first) {
        hold_share(h);
    }
    unlock_all();
    w->held |= ok;
    return ok;
}

void sched_drop(struct watched *w)
{
    char key[32];
    struct hold *h;

    snprintf(key, sizeof(key), "sched-%d", (int)w->pid);
    if (w->held && (h = hold_find(key))) {
        hold_drop(h);
    }
    w->held = false;
}

static void restore_key(const char *key)
{
    if (strcmp(key, fair.key) == 0 || strcmp(key, ext.key) == 0) {
        /* a dead helper's real-time threads go first, whoever lets the
           bound go: they need it */
        recover_pass(true);
        server_restore(strcmp(key, fair.key) == 0 ? &fair : &ext);
    } else if (strncmp(key, "sched-", 6) == 0) {
        sched_state_restore(key);
    } else if (strncmp(key, "gpu-floor-", 10) == 0 && card_name_ok(key + 10)) {
        gpu_restore(key + 10);
    } else if (strcmp(key, "udmabuf") == 0) {
        udmabuf_restore();
    } else if (strcmp(key, "thp") == 0) {
        thp_restore();
    } else if (strcmp(key, "epp") == 0) {
        epp_restore();
    } else if (strncmp(key, "vfio-", 5) == 0) {
        /*
         * Kept: PCI devices go back to their drivers outside the lock, as a
         * driver may take its time to let go of one, or to take it back
         * (vfio.c gives them back with the group's hold: group_drop() and,
         * for a dead helper's record, the recovery's adoption)
         */
    } else if (strncmp(key, "memlock-", 8) == 0) {
        memlock_restore(key);
    } else {
        state_remove(key);
    }
}

/*
 * Under the lock: the states no live helper holds, the QEMUs' records
 * (@sched) or the settings
 */
static void recover_pass(bool sched)
{
    /* an open file of its own: a dup() would share the offset with the
       passes around it (restore_key() runs one inside another) */
    int fd = openat(state_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    struct dirent *e;
    DIR *d;

    if (fd < 0 || !(d = fdopendir(fd))) {
        if (fd >= 0) {
            close(fd);
        }
        return;
    }
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        char key[32], name[48];
        int hfd;

        if (len <= 6 || len - 6 >= sizeof(key) || strcmp(e->d_name + len - 6, ".state") != 0) {
            continue;
        }
        snprintf(key, sizeof(key), "%.*s", (int)(len - 6), e->d_name);
        if ((strncmp(key, "sched-", 6) == 0) != sched) {
            continue;
        }
        snprintf(name, sizeof(name), "%s.hold", key);
        /* LOCK_EX fails on a hold this helper has too: another open file */
        hfd = openat(state_fd, name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (hfd >= 0 && flock(hfd, LOCK_EX | LOCK_NB) == 0) {
            struct hold *h;

            /* PCI devices: this helper's to give back after the lock */
            if (strncmp(key, "vfio-", 5) == 0 && (h = hold_adopt(key, hfd))) {
                vfio_adopt(key, h);
                continue;
            }
            restore_key(key);
            hold_remove(key);
        }
        if (hfd >= 0) {
            close(hfd);
        }
    }
    closedir(d);
}

/*
 * Under the lock: everything no live helper holds.  The QEMUs' records
 * first: threads a dead helper made real-time go back to ordinary before
 * the bound they needed does.
 */
static void recover(void)
{
    recover_pass(true);
    recover_pass(false);
}

bool settings_init(void)
{
    char path[PATH_MAX];
    struct stat st;

    for (int i = 0; i < MAX_HOLDS; i++) {
        holds[i].fd = -1;
    }
    if (!rooted(path, sizeof(path), STATE_DIR) ||
        (mkdir(path, 0700) < 0 && errno != EEXIST)) {
        return false;
    }
    /* /run is root's: still, only a folder of ours, private, will do */
    state_fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (state_fd < 0 || fstat(state_fd, &st) < 0 || st.st_uid != geteuid() ||
        (st.st_mode & 077)) {
        errno = state_fd < 0 ? errno : EPERM;
        return false;
    }
    lock_fd = openat(state_fd, "lock", O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_fd < 0) {
        return false;
    }
    lock_all();
    recover();
    unlock_all();
    return true;
}

void settings_release(void)
{
    /* the QEMUs' records (the last slots) before the bound, then the
       settings in the order they were taken */
    for (int i = MAX_SETTINGS; i < MAX_HOLDS + MAX_SETTINGS; i++) {
        struct hold *h = &holds[i % MAX_HOLDS];

        if (h->key[0]) {
            hold_drop(h);
        }
    }
    latency_release();
    snprintf(bound_why, sizeof(bound_why), "the settings were let go");
}
