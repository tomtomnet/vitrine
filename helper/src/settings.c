/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The global settings: the kernel's fair server (and ext server), an AMD
 * GPU's clock floor and the udmabuf limits; and the record of each QEMU
 * whose threads a helper made real-time or ordinary (sched-<pid>).  The
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
                           sched-<pid>; empty when free */
    int fd;             /* <key>.hold */
    char value[16];     /* what was asked: on, auto or MHz */
};
/* the fair and ext servers, the udmabuf limits and eight cards; then the
   records of the QEMUs watched */
#define MAX_CARDS 8
#define MAX_SETTINGS (3 + MAX_CARDS)
#define MAX_HOLDS (MAX_SETTINGS + MAX_WATCHED)
static struct hold holds[MAX_HOLDS];

static void lock_all(void)
{
    flock(lock_fd, LOCK_EX);
}

static void unlock_all(void)
{
    flock(lock_fd, LOCK_UN);
}

/* --- state files --- */

static bool state_write(const char *key, const char *text)
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
static char *state_read(const char *key)
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

static void state_remove(const char *key)
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

static struct hold *hold_find(const char *key)
{
    for (int i = 0; i < MAX_HOLDS; i++) {
        if (strcmp(holds[i].key, key) == 0) {
            return &holds[i];
        }
    }
    return NULL;
}

static void restore_key(const char *key);

/*
 * Under the lock: takes the hold of @key.  *@first tells whether no other
 * helper holds it: this helper then changes the setting, keeping LOCK_EX
 * until hold_share().
 */
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

static struct hold *hold_take(const char *key, const char *value, bool *first)
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

static void hold_share(struct hold *h)
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
static void hold_close(struct hold *h, bool last)
{
    if (last) {
        hold_remove(h->key);
    }
    close(h->fd);
    h->fd = -1;
    h->key[0] = '\0';
}

/* Lets go of @h, restoring the setting when this helper was its last holder */
static void hold_drop(struct hold *h)
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

/* The CPUs of @srv, sorted; their count, or -errno */
static int server_cpus(const struct server *srv, unsigned **out)
{
    char dir[PATH_MAX];
    struct dirent *e;
    unsigned *ids = NULL;
    int n = 0, cap = 0;
    DIR *d;

    if (!rooted(dir, sizeof(dir), srv->dir) || !(d = opendir(dir))) {
        return -errno;
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

/* The online CPUs (/sys/devices/system/cpu/online: "0-3,8-11"); all of
   them when that cannot be read */
struct online {
    unsigned lo[64], hi[64];
    int n;
};

static void online_read(struct online *o)
{
    char buf[1024], *save = NULL;

    o->n = 0;
    if (read_attr(CPUS_ONLINE, buf, sizeof(buf)) <= 0) {
        o->n = -1;
        return;
    }
    for (char *t = strtok_r(buf, ",\n", &save); t; t = strtok_r(NULL, ",\n", &save)) {
        unsigned a, b;
        int got = sscanf(t, "%u-%u", &a, &b);

        if (o->n == 64 || got < 1) {
            /* more ranges than kept, or not a list: all of them */
            o->n = -1;
            return;
        }
        o->lo[o->n] = a;
        o->hi[o->n++] = got == 2 ? b : a;
    }
    if (o->n == 0) {
        o->n = -1;
    }
}

static bool online_has(const struct online *o, unsigned cpu)
{
    if (o->n < 0) {
        return true;
    }
    for (int i = 0; i < o->n; i++) {
        if (cpu >= o->lo[i] && cpu <= o->hi[i]) {
            return true;
        }
    }
    return false;
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

/*
 * Under the lock, by the first holder: @srv at the target on every online
 * CPU, or nothing changed.  Offline CPUs run nothing (and the kernel
 * refuses their writes): left out.  A CPU at the target while others are
 * not is a leftover - a run cut short, its record lost - recorded with what
 * the others have (the kernel's default if they differ), to be put back
 * with them.  All of them at the target: another tool's, which may still
 * need it, or a leftover nobody can tell from that - left as found.
 * 0 with what was done, or -1 with why in @why.
 */
static int server_apply(const struct server *srv, char *why, size_t size, int *changed,
                        int *adopted, u64 *was_period, u64 *was_runtime)
{
    struct online online;
    struct server_cpu *cpus = NULL;
    unsigned *ids = NULL;
    char *text = NULL;
    u64 base_p = 0, base_r = 0;
    bool base = false, mixed = false;
    int n, m = 0, rc = -1;

    online_read(&online);
    n = server_cpus(srv, &ids);
    if (n <= 0) {
        snprintf(why, size, "%s: %s", srv->name, n < 0 ? strerror(-n) : "no cpus");
        goto out;
    }
    if (!(cpus = calloc((size_t)n, sizeof(*cpus))) || !(text = malloc((size_t)n * 96 + 1))) {
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
        struct server_cpu *c = &cpus[i];

        if (c->period == FAIR_PERIOD_NS && c->runtime == FAIR_RUNTIME_NS) {
            if (!base) {
                continue;
            }
            c->period = base_p;
            c->runtime = base_r;
            c->adopted = true;
            (*adopted)++;
        }
        c->new_period = FAIR_PERIOD_NS;
        c->new_runtime = FAIR_RUNTIME_NS;
    }
    /* recorded before anything is written */
    if (server_state(text, cpus, m) && !state_write(srv->key, text)) {
        snprintf(why, size, "%s: %s", srv->name, strerror(errno));
        goto out;
    }
    for (int i = 0; i < m; i++) {
        struct server_cpu *c = &cpus[i];
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
        /* all or nothing: those written before it back; what cannot be put
           back stays recorded, for a helper to try again */
        c->new_period = 0;
        for (int j = 0; j < i; j++) {
            struct server_cpu *b = &cpus[j];

            if (!b->new_period || b->adopted ||
                server_set(srv, b->id, b->new_period, b->new_runtime, b->period, b->runtime) == 0) {
                b->new_period = 0;
            }
        }
        for (int j = i + 1; j < m; j++) {
            cpus[j].new_period = 0;
        }
        if (server_state(text, cpus, m)) {
            state_write(srv->key, text);
        } else {
            state_remove(srv->key);
        }
        *changed = *adopted = 0;
        goto out;
    }
    if (!*changed && !*adopted) {
        state_remove(srv->key);
    }
    rc = 0;
out:
    free(ids);
    free(cpus);
    free(text);
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
        reply("ok fair-server on: already");
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
    if (hold_find(fair.key) && (!ext_needed || hold_find(ext.key))) {
        return true;
    }
    *why = bound_why[0] ? bound_why : "the fair server is not set";
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

/* --- all of them --- */

/* sched-<pid>'s record ("pid P start S niced N"): those threads put back */
static void sched_state_restore(const char *key)
{
    char *text = state_read(key);
    int pid, niced;
    u64 start;

    if (text && sscanf(text, "pid %d start %llu niced %d", &pid, &start, &niced) == 3 &&
        pid > 1) {
        sched_restore((pid_t)pid, start, niced != 0, true);
    }
    free(text);
    state_remove(key);
}

bool sched_hold(struct watched *w)
{
    char key[32], text[96];
    struct hold *h;
    bool first = false, ok;

    snprintf(key, sizeof(key), "sched-%d", (int)w->pid);
    lock_all();
    if (!(h = hold_find(key)) && !(h = hold_take(key, "on", &first))) {
        unlock_all();
        return false;
    }
    /* what to put back: threads made real-time (FIFO 1), vCPUs niced or not */
    snprintf(text, sizeof(text), "pid %d start %llu niced %d\n", (int)w->pid, w->start,
             w->behind ? 1 : 0);
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
    if (strcmp(key, fair.key) == 0) {
        server_restore(&fair);
    } else if (strcmp(key, ext.key) == 0) {
        server_restore(&ext);
    } else if (strncmp(key, "sched-", 6) == 0) {
        sched_state_restore(key);
    } else if (strncmp(key, "gpu-floor-", 10) == 0 && card_name_ok(key + 10)) {
        gpu_restore(key + 10);
    } else if (strcmp(key, "udmabuf") == 0) {
        udmabuf_restore();
    } else {
        state_remove(key);
    }
}

/*
 * Under the lock: the states no live helper holds.  The QEMUs' records
 * first: threads a dead helper made real-time go back to ordinary before
 * the bound they needed does.
 */
static void recover(void)
{
    int fd = dup(state_fd);
    struct dirent *e;
    DIR *d;

    if (fd < 0 || !(d = fdopendir(fd))) {
        if (fd >= 0) {
            close(fd);
        }
        return;
    }
    for (int pass = 0; pass < 2; pass++) {
        rewinddir(d);
        while ((e = readdir(d))) {
            size_t len = strlen(e->d_name);
            char key[32], name[48];
            int hfd;

            if (len <= 6 || len - 6 >= sizeof(key) ||
                strcmp(e->d_name + len - 6, ".state") != 0) {
                continue;
            }
            snprintf(key, sizeof(key), "%.*s", (int)(len - 6), e->d_name);
            if ((strncmp(key, "sched-", 6) == 0) != (pass == 0)) {
                continue;
            }
            snprintf(name, sizeof(name), "%s.hold", key);
            hfd = openat(state_fd, name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (hfd >= 0 && flock(hfd, LOCK_EX | LOCK_NB) == 0) {
                restore_key(key);
                hold_remove(key);
            }
            if (hfd >= 0) {
                close(hfd);
            }
        }
    }
    closedir(d);
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
    /* the QEMUs' records (the last slots) before the bound */
    for (int i = MAX_HOLDS - 1; i >= 0; i--) {
        if (holds[i].key[0]) {
            hold_drop(&holds[i]);
        }
    }
    snprintf(bound_why, sizeof(bound_why), "the settings were let go");
}
