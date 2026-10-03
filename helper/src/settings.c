/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The global settings: the kernel's fair server and an AMD GPU's clock
 * floor.  They belong to the whole machine, so several helpers (vitrine
 * restarted while its VMs run, other users' sessions) coordinate through
 * /run/vitrine-helper, a root-only tmpfs folder that a reboot empties:
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
    char key[32];       /* fair-server, gpu-floor-cardN; empty when free */
    int fd;             /* <key>.hold */
    char value[16];     /* what was asked: on, auto or MHz */
};
#define MAX_HOLDS 9
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
static struct hold *hold_take(const char *key, const char *value, bool *first)
{
    struct hold *h = hold_find("");
    char name[48];
    int fd;

    if (!h) {
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

/* --- fair server --- */

struct fair_cpu {
    unsigned id;
    u64 period, runtime;            /* before */
    u64 new_period, new_runtime;    /* written */
};

static void fair_path(char *out, size_t size, unsigned cpu, const char *file)
{
    snprintf(out, size, FAIR_SERVER_DIR "/cpu%u/%s", cpu, file);
}

static bool fair_get(unsigned cpu, u64 *period, u64 *runtime)
{
    char p[PATH_MAX], r[PATH_MAX];

    fair_path(p, sizeof(p), cpu, "period");
    fair_path(r, sizeof(r), cpu, "runtime");
    return read_u64(p, period) && read_u64(r, runtime);
}

/*
 * Sets CPU @cpu's server from (@period, @runtime) to (@to_period,
 * @to_runtime).  The kernel checks runtime <= period at each write, so a
 * shorter period goes after the runtime, a longer one before.  If the
 * second write fails the first is undone.
 */
static int fair_set(unsigned cpu, u64 period, u64 runtime, u64 to_period, u64 to_runtime)
{
    char p[PATH_MAX], r[PATH_MAX];
    int err;

    fair_path(p, sizeof(p), cpu, "period");
    fair_path(r, sizeof(r), cpu, "runtime");
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

/* The CPUs of the fair server, sorted; their count, or -errno */
static int fair_cpus(unsigned **out)
{
    char dir[PATH_MAX];
    struct dirent *e;
    unsigned *ids = NULL;
    int n = 0, cap = 0;
    DIR *d;

    if (!rooted(dir, sizeof(dir), FAIR_SERVER_DIR) || !(d = opendir(dir))) {
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

static const char *fair_unavailable(void)
{
    char path[PATH_MAX];
    const char *why = lockdown();
    struct stat st;

    if (why) {
        return why;
    }
    if (rooted(path, sizeof(path), FAIR_SERVER_DIR) && stat(path, &st) == 0) {
        return NULL;
    }
    if (rooted(path, sizeof(path), "/sys/kernel/debug/sched") && stat(path, &st) == 0) {
        return "this kernel has no fair server";
    }
    return "debugfs is not mounted";
}

/* Under the lock, as the last holder or for a dead one: the state back */
static void fair_restore(void)
{
    char *text = state_read("fair-server"), *line, *save = NULL;
    int restored = 0, already = 0, left = 0, failed = 0;
    u64 period = 0, runtime = 0;

    if (!text) {
        return;
    }
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        struct fair_cpu c;
        u64 cur_p, cur_r;

        if (sscanf(line, "cpu%u %llu %llu %llu %llu", &c.id, &c.period, &c.runtime,
                   &c.new_period, &c.new_runtime) != 5 || c.runtime > c.period) {
            continue;
        }
        period = c.period;
        runtime = c.runtime;
        if (!fair_get(c.id, &cur_p, &cur_r)) {
            failed++;
        } else if (cur_p == c.new_period && cur_r == c.new_runtime) {
            if (fair_set(c.id, cur_p, cur_r, c.period, c.runtime) == 0) {
                restored++;
            } else {
                failed++;
            }
        } else if (cur_p == c.period && cur_r == c.runtime) {
            already++;
        } else if ((cur_p == c.period || cur_p == c.new_period) &&
                   (cur_r == c.runtime || cur_r == c.new_runtime)) {
            /*
             * Half-way, as fair_set() leaves it between its two writes: a
             * helper killed there, setting or restoring.  Not another
             * tool's change: theirs would not be one of our pairs.  (The
             * other mixed pair cannot happen: its runtime would exceed its
             * period, which the kernel refuses.)
             */
            if (fair_set(c.id, cur_p, cur_r, c.period, c.runtime) == 0) {
                restored++;
            } else {
                failed++;
            }
        } else {
            left++;
        }
    }
    free(text);
    state_remove("fair-server");
    reply("restored fair-server: %d cpus back to %llu ms / %llu ms%s", restored + already,
          period / 1000000, runtime / 1000000, failed ? " (some failed)" : "");
    if (left) {
        reply("left fair-server: %d cpus changed by someone else since", left);
    }
    sys_log("fair server: %d cpus back to %llu / %llu ns, %d left as changed since, %d failed",
            restored, period, runtime, left, failed);
}

/* "cpuN period runtime new_period new_runtime" lines for the CPUs @c changes */
static size_t fair_state(char *text, const struct fair_cpu *c, int n)
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

void fair_server_on(void)
{
    struct fair_cpu *cpus = NULL;
    unsigned *ids = NULL;
    char *text = NULL;
    struct hold *h;
    const char *why;
    bool first;
    int n, changed = 0, failed = 0, err = 0;

    if (hold_find("fair-server")) {
        reply("ok fair-server on: already");
        return;
    }
    if ((why = fair_unavailable())) {
        reply("skip fair-server: %s", why);
        return;
    }
    lock_all();
    if (!(h = hold_take("fair-server", "on", &first))) {
        err = errno;
        unlock_all();
        reply("error fair-server: cannot take the hold: %s", strerror(err));
        return;
    }
    if (!first) {
        unlock_all();
        reply("ok fair-server on: set by another vitrine session");
        return;
    }
    n = fair_cpus(&ids);
    if (n <= 0 || !(cpus = calloc((size_t)n, sizeof(*cpus))) ||
        !(text = malloc((size_t)n * 96 + 1))) {
        err = n < 0 ? -n : n == 0 ? ENOENT : ENOMEM;
        goto fail;
    }
    /* what each CPU has, saved before anything is written (new_period 0:
       left alone, already at the target or unreadable) */
    for (int i = 0; i < n; i++) {
        struct fair_cpu *c = &cpus[i];

        c->id = ids[i];
        if (!fair_get(c->id, &c->period, &c->runtime)) {
            failed++;
        } else if (c->period != FAIR_PERIOD_NS || c->runtime != FAIR_RUNTIME_NS) {
            c->new_period = FAIR_PERIOD_NS;
            c->new_runtime = FAIR_RUNTIME_NS;
        }
    }
    if (fair_state(text, cpus, n) && !state_write("fair-server", text)) {
        err = errno;
        goto fail;
    }
    for (int i = 0; i < n; i++) {
        struct fair_cpu *c = &cpus[i];
        int e;

        if (!c->new_period) {
            continue;
        }
        if ((e = fair_set(c->id, c->period, c->runtime, c->new_period, c->new_runtime)) < 0) {
            /* fair_set left it as it was: nothing to restore there */
            c->new_period = 0;
            failed++;
            err = -e;
        } else {
            changed++;
        }
    }
    /* the state keeps only the CPUs that took the new values */
    if (!changed) {
        state_remove("fair-server");
    } else if (!state_write("fair-server", (fair_state(text, cpus, n), text))) {
        /* unrecorded changes would outlive the helper: undo them now */
        err = errno;
        for (int i = 0; i < n; i++) {
            if (cpus[i].new_period) {
                fair_set(cpus[i].id, cpus[i].new_period, cpus[i].new_runtime,
                         cpus[i].period, cpus[i].runtime);
            }
        }
        goto fail;
    }
    hold_share(h);
    unlock_all();
    if (changed) {
        const struct fair_cpu *c = cpus;

        while (!c->new_period) {
            c++;
        }
        sys_log("uid %u: fair server %llu / %llu ns on %d cpus (was %llu / %llu ns)",
                (unsigned)caller_uid, FAIR_PERIOD_NS, FAIR_RUNTIME_NS, changed, c->period,
                c->runtime);
        reply("ok fair-server on: %d cpus at 10 ms / 1 ms (was %llu ms / %llu ms)%s", changed,
              c->period / 1000000, c->runtime / 1000000, failed ? " (some cpus failed)" : "");
    } else if (failed) {
        hold_drop(h);
        reply("error fair-server: %s", strerror(err ? err : EIO));
    } else {
        reply("ok fair-server on: already 10 ms / 1 ms");
    }
    free(ids);
    free(cpus);
    free(text);
    return;
fail:
    state_remove("fair-server");
    hold_close(h, true);
    unlock_all();
    reply("error fair-server: %s", strerror(err ? err : EIO));
    free(ids);
    free(cpus);
    free(text);
}

void fair_server_off(void)
{
    struct hold *h = hold_find("fair-server");

    if (h) {
        hold_drop(h);
    }
    reply("ok fair-server off");
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
        char cmd[40];
        int err, lerr;

        /* the driver's own range back (committed: a reset not committed
           yet may hold the floor still), then the level it had */
        err = card_write(card, "pp_od_clk_voltage", "r\n");
        if (!err) {
            err = card_write(card, "pp_od_clk_voltage", "c\n");
        }
        snprintf(cmd, sizeof(cmd), "%s\n", saved);
        lerr = card_write(card, "power_dpm_force_performance_level", cmd);
        reply("restored gpu-floor %s: level %s%s", card, saved,
              err || lerr ? " (some writes failed)" : "");
        sys_log("%s: clock floor %u MHz off, level %s", card, min, saved);
        if (lerr) {
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
    struct od_table od;
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
        unlock_all();
        reply("error gpu-floor %s: cannot take the hold: %s", card, strerror(errno));
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
    if (err) {
        card_write(card, "pp_od_clk_voltage", "r\n");
        card_write(card, "pp_od_clk_voltage", "c\n");
        snprintf(cmd, sizeof(cmd), "%s\n", level);
        card_write(card, "power_dpm_force_performance_level", cmd);
        state_remove(key);
        hold_close(h, true);
        unlock_all();
        reply("error gpu-floor %s: %s", card, strerror(-err));
        return;
    }
    hold_share(h);
    unlock_all();
    sys_log("uid %u: %s clock floor %u MHz (was %u MHz, level %s)", (unsigned)caller_uid, card,
            mhz, od.min, level);
    reply("ok gpu-floor %s %u MHz (was %u MHz, level %s)", card, mhz, od.min, level);
}

/* --- all of them --- */

static void restore_key(const char *key)
{
    if (strcmp(key, "fair-server") == 0) {
        fair_restore();
    } else if (strncmp(key, "gpu-floor-", 10) == 0 && card_name_ok(key + 10)) {
        gpu_restore(key + 10);
    } else {
        state_remove(key);
    }
}

/* Under the lock: the states no live helper holds */
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
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        char key[32], name[48];
        int hfd;

        if (len <= 6 || len - 6 >= sizeof(key) || strcmp(e->d_name + len - 6, ".state") != 0) {
            continue;
        }
        snprintf(key, sizeof(key), "%.*s", (int)(len - 6), e->d_name);
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
    for (int i = 0; i < MAX_HOLDS; i++) {
        if (holds[i].key[0]) {
            hold_drop(&holds[i]);
        }
    }
}
