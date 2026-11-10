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
 * <file>.writes N takes N more writes, then refuses them.  transparent_
 * hugepage/shmem_enabled takes one of its words and shows it in brackets
 * among them, as the kernel does.  A CPU's cpufreq/energy_performance_
 * preference takes the drivers' words or a number up to 255, and a CPU
 * that the online list leaves out answers -EBUSY for its cpufreq files, as
 * an inactive policy does.  /dev/cpu_dma_latency's requests are files
 * locked by their process (see sys_latency_request).  Other files (the
 * udmabuf module's parameters) take any value, as the kernel's int
 * parameters do.  Every write, request, scheduler and nice
 * change and log line is appended to <root>/journal.  Schedulers and nice
 * values are kept in <root>/sched (an unprivileged test cannot make threads
 * real-time nor lower their nice), where the next helper finds them, and
 * real-time time limits in <root>/rttime-<pid> (see sys_getrttime); the
 * processes and /proc are real.  The group database is <root>/etc/group
 * (name:x:gid:members lines); the users are the real ones.  This build
 * refuses to run as root, and the installed helper has no test root at all.
 *
 * The PCI bus plays the kernel's binding rules (see pci_bind): a device's
 * driver is its folder's link, driver_override names the one driver that
 * may take it, drivers_probe picks that one or the device's fake-native,
 * vfio-pci makes /dev/vfio/<group> for the first device of a group and
 * takes it away with the last, and an unbind of a vfio device whose group
 * node is held - a stand-in QEMU's flock on it - would wait in the kernel:
 * it fails here, the journal saying "WHILE IN USE".  Nodes are files with
 * sidecars: <node>.owner ("uid gid mode", root's 0600 if none) and
 * <node>.rdev ("MAJ:MIN", "b MAJ:MIN" for a block device).  The processes
 * holding device nodes are <root>/users ("c|b MAJ:MIN PID COMM [EXE]"
 * lines); modules load by making their driver's folder (modprobe.refuse:
 * they do not); locked memory limits are <root>/memlock-<pid>, as real-time
 * time limits are (memlock.refuse: a change fails); <root>/chown.refuse
 * makes a node's owner change fail; a driver's unbind.slow (ms) makes its
 * unbind take that long.  VITRINE_HELPER_TEST_VFIO_EXPIRY_MS shortens the
 * wait of prepared groups for their QEMU.
 *
 * VITRINE_HELPER_TEST_KILL_AT=N kills it (SIGKILL) right after its Nth
 * write (a latency request, a module, a node's owner and a limit count as
 * one), as a crash or a kill in the middle of a change would;
 * VITRINE_HELPER_TEST_KILL_BEFORE=TEXT kills it right before its first
 * write to a path holding TEXT.
 */
#define _GNU_SOURCE
#include <dirent.h>
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
#include <sys/stat.h>
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

static bool cpu_offline(const char *path);

int sys_read(const char *path, char *buf, size_t size)
{
    int fd;
    ssize_t n;

    /* an offline CPU's cpufreq policy is inactive: its files answer -EBUSY */
    if (strncmp(path, root, strlen(root)) == 0 && strstr(path + strlen(root), "/cpufreq/") &&
        cpu_offline(path)) {
        return -EBUSY;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
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

/* The CPU of a path ".../cpuN/..." under the root, or -1 */
static int path_cpu(const char *path)
{
    const char *rel = strncmp(path, root, strlen(root)) == 0 ? path + strlen(root) : path;

    for (const char *c = strstr(rel, "/cpu"); c; c = strstr(c + 1, "/cpu")) {
        unsigned cpu;
        char slash;

        if (sscanf(c, "/cpu%u%c", &cpu, &slash) == 2 && slash == '/') {
            return (int)cpu;
        }
    }
    return -1;
}

/* The CPU of a path ".../cpuN/..." (a server's period, a cpufreq file) is
   offline: the fake tree's /sys/devices/system/cpu/online leaves it out */
static bool cpu_offline(const char *path)
{
    char online[PATH_MAX], text[256], *save = NULL;
    const int cpu = path_cpu(path);

    snprintf(online, sizeof(online), "%s" CPUS_ONLINE, root);
    if (cpu < 0 || sys_read(online, text, sizeof(text)) <= 0) {
        return false;
    }
    for (char *t = strtok_r(text, ",\n", &save); t; t = strtok_r(NULL, ",\n", &save)) {
        unsigned a, b;
        int got = sscanf(t, "%u-%u", &a, &b);

        if (got >= 1 && (unsigned)cpu >= a && (unsigned)cpu <= (got == 2 ? b : a)) {
            return false;
        }
    }
    return true;
}

/* <path>.writes N: N more writes taken, then refused with @refusal; 0 when
   this one is taken */
static int take_write(const char *path, int refusal)
{
    char file[PATH_MAX], text[32];
    unsigned long long left;

    snprintf(file, sizeof(file), "%s.writes", path);
    if (access(file, F_OK) != 0) {
        return 0;
    }
    if (!get_u64(file, &left) || left == 0) {
        return refusal;
    }
    snprintf(text, sizeof(text), "%llu\n", left - 1);
    put(file, text);
    return 0;
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
    if (take_write(path, -EACCES) < 0) {
        return -EACCES;
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

/* transparent_hugepage/shmem_enabled: "always within_size advise [never] deny force" */
static int thp_write(const char *path, const char *value)
{
    static const char *const words[] = {"always", "within_size", "advise", "never",
                                        "deny",   "force",       NULL};
    char text[160] = "", stuck[PATH_MAX];
    size_t len = 0;
    bool known = false;

    for (int i = 0; words[i]; i++) {
        known |= strcmp(value, words[i]) == 0;
    }
    if (!known) {
        return -EINVAL;
    }
    snprintf(stuck, sizeof(stuck), "%s.stuck", path);
    if (access(stuck, F_OK) == 0) {
        /* taken, not kept */
        return 0;
    }
    for (int i = 0; words[i]; i++) {
        const bool chosen = strcmp(value, words[i]) == 0;

        len += (size_t)snprintf(text + len, sizeof(text) - len, "%s%s%s%s", i ? " " : "",
                                chosen ? "[" : "", words[i], chosen ? "]" : "");
    }
    snprintf(text + len, sizeof(text) - len, "\n");
    return put(path, text);
}

/*
 * cpufreq's energy_performance_preference: one of the drivers' words, or a
 * number up to 255 (shown as written); "default" gives the firmware's,
 * balance_performance here.  -EBUSY for an offline CPU, whose policy is
 * inactive, and once <file>.writes N has run out (a driver that sets the
 * preference itself: amd-pstate's dynamic EPP).
 */
static int epp_write(const char *path, const char *value)
{
    static const char *const words[] = {"default", "performance", "balance_performance",
                                        "balance_power", "power", NULL};
    char stuck[PATH_MAX], text[160];
    unsigned long long n;
    bool known = parse_uint(value, 255, &n);

    for (int i = 0; words[i]; i++) {
        known |= strcmp(value, words[i]) == 0;
    }
    if (!known) {
        return -EINVAL;
    }
    if (cpu_offline(path) || take_write(path, -EBUSY) < 0) {
        return -EBUSY;
    }
    snprintf(stuck, sizeof(stuck), "%s.stuck", path);
    if (access(stuck, F_OK) == 0) {
        /* taken, not kept */
        return 0;
    }
    snprintf(text, sizeof(text), "%s\n", strcmp(value, "default") == 0 ? "balance_performance" : value);
    return put(path, text);
}

/* --- the PCI bus: drivers, overrides, vfio's nodes --- */

/* @addr's folder in the fake tree, and @rest under it */
static void pci_dev(char *out, size_t size, const char *addr, const char *rest)
{
    snprintf(out, size, "%s" PCI_DIR "/devices/%s%s", root, addr, rest);
}

/* The first line of @path, "" when there is none */
static void first_line(const char *path, char *out, size_t size)
{
    out[0] = '\0';
    if (sys_read(path, out, size) < 0) {
        out[0] = '\0';
    }
    out[strcspn(out, "\n")] = '\0';
}

/* The name of @addr's driver, "" for none */
static void pci_driver(const char *addr, char *out, size_t size)
{
    char path[PATH_MAX], link[PATH_MAX];
    ssize_t n;

    out[0] = '\0';
    pci_dev(path, sizeof(path), addr, "/driver");
    if ((n = readlink(path, link, sizeof(link) - 1)) > 0) {
        const char *base;
        size_t len;

        link[n] = '\0';
        base = strrchr(link, '/') ? strrchr(link, '/') + 1 : link;
        len = strlen(base) < size ? strlen(base) : size - 1;
        memcpy(out, base, len);
        out[len] = '\0';
    }
}

static int pci_group(const char *addr)
{
    char path[PATH_MAX], link[PATH_MAX];
    ssize_t n;

    pci_dev(path, sizeof(path), addr, "/iommu_group");
    if ((n = readlink(path, link, sizeof(link) - 1)) <= 0) {
        return -1;
    }
    link[n] = '\0';
    return atoi(strrchr(link, '/') ? strrchr(link, '/') + 1 : link);
}

/* /dev/vfio/@group of the fake tree, with @suffix (a sidecar's) */
static void vfio_node(char *out, size_t size, int group, const char *suffix)
{
    snprintf(out, size, "%s" VFIO_DEV_DIR "/%d%s", root, group, suffix);
}

/* A device of @group but @addr is on vfio-pci */
static bool group_on_vfio(int group, const char *addr)
{
    char dir[PATH_MAX], drv[64];
    struct dirent *e;
    bool found = false;
    DIR *d;

    snprintf(dir, sizeof(dir), "%s" IOMMU_GROUPS_DIR "/%d/devices", root, group);
    if (!(d = opendir(dir))) {
        return false;
    }
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.' || strcmp(e->d_name, addr) == 0) {
            continue;
        }
        pci_driver(e->d_name, drv, sizeof(drv));
        found = strcmp(drv, "vfio-pci") == 0;
    }
    closedir(d);
    return found;
}

/* A node some process has "open": a stand-in QEMU's lock on its file
   (fakeqemu.c, FAKE_QEMU_HOLD) */
static bool node_held(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    bool held;

    if (fd < 0) {
        return false;
    }
    held = flock(fd, LOCK_EX | LOCK_NB) < 0 && errno == EWOULDBLOCK;
    close(fd);
    return held;
}

/*
 * drivers/@name/bind: @addr bound as the kernel binds it - not over
 * another driver, only by the driver its driver_override names, and
 * without one only by its own (its fake-native file; vfio-pci then takes
 * it only through an override); <driver>/bind.refuse makes the probe fail.
 * vfio-pci makes the group's node, root's at 0600.
 */
static int pci_bind(const char *name, const char *addr)
{
    char path[PATH_MAX], target[PATH_MAX], drv[64], wanted[64], node[PATH_MAX], text[32];
    int group;

    pci_dev(path, sizeof(path), addr, "");
    if (access(path, F_OK) != 0) {
        return -ENODEV;
    }
    snprintf(target, sizeof(target), "%s" PCI_DIR "/drivers/%s", root, name);
    if (access(target, F_OK) != 0) {
        return -ENOENT;
    }
    pci_driver(addr, drv, sizeof(drv));
    if (drv[0]) {
        return -EBUSY;
    }
    pci_dev(path, sizeof(path), addr, "/driver_override");
    first_line(path, wanted, sizeof(wanted));
    if (!wanted[0] || strcmp(wanted, "(null)") == 0) {
        pci_dev(path, sizeof(path), addr, "/fake-native");
        first_line(path, wanted, sizeof(wanted));
    }
    if (strcmp(wanted, name) != 0) {
        return -ENODEV;
    }
    if (snprintf(path, sizeof(path), "%s/bind.refuse", target) < (int)sizeof(path) &&
        access(path, F_OK) == 0) {
        return -EINVAL;
    }
    pci_dev(path, sizeof(path), addr, "/driver");
    if (symlink(target, path) < 0) {
        return -errno;
    }
    if (strcmp(name, "vfio-pci") == 0 && (group = pci_group(addr)) >= 0) {
        vfio_node(node, sizeof(node), group, "");
        if (access(node, F_OK) != 0) {
            snprintf(path, sizeof(path), "%s" VFIO_DEV_DIR, root);
            mkdir(path, 0755);
            put(node, "");
            vfio_node(path, sizeof(path), group, ".owner");
            put(path, "0 0 600\n");
            vfio_node(path, sizeof(path), group, ".rdev");
            snprintf(text, sizeof(text), "239:%d\n", group);
            put(path, text);
        }
    }
    return 0;
}

/*
 * drivers/@name/unbind: @addr let go.  A vfio device whose group a
 * process has open would wait in the kernel until it is closed: it fails
 * here ("WHILE IN USE" in the journal), which the tests look for.  The last
 * vfio device of a group takes its node with it.
 */
static int pci_unbind(const char *name, const char *addr)
{
    char path[PATH_MAX], drv[64], node[PATH_MAX], slow[32];
    const int group = pci_group(addr);
    const bool vfio = strcmp(name, "vfio-pci") == 0;

    pci_driver(addr, drv, sizeof(drv));
    if (strcmp(drv, name) != 0) {
        return -ENODEV;
    }
    /* a driver that takes its time to let go */
    snprintf(path, sizeof(path), "%s" PCI_DIR "/drivers/%s/unbind.slow", root, name);
    first_line(path, slow, sizeof(slow));
    if (slow[0]) {
        usleep((useconds_t)atoi(slow) * 1000);
    }
    if (vfio && group >= 0) {
        vfio_node(node, sizeof(node), group, "");
        if (node_held(node)) {
            journal("unbind %s %s WHILE IN USE", addr, name);
            return -EBUSY;
        }
    }
    pci_dev(path, sizeof(path), addr, "/driver");
    if (unlink(path) < 0) {
        return -errno;
    }
    if (vfio && group >= 0 && !group_on_vfio(group, addr)) {
        vfio_node(node, sizeof(node), group, "");
        unlink(node);
        vfio_node(node, sizeof(node), group, ".owner");
        unlink(node);
        vfio_node(node, sizeof(node), group, ".rdev");
        unlink(node);
    }
    return 0;
}

/* drivers_probe: the driver its override names, else its own */
static int pci_probe(const char *addr)
{
    char path[PATH_MAX], drv[64], name[64];

    pci_driver(addr, drv, sizeof(drv));
    if (drv[0]) {
        return 0;
    }
    pci_dev(path, sizeof(path), addr, "/driver_override");
    first_line(path, name, sizeof(name));
    if (!name[0] || strcmp(name, "(null)") == 0) {
        pci_dev(path, sizeof(path), addr, "/fake-native");
        first_line(path, name, sizeof(name));
    }
    /* as the kernel's: whatever came of the probe, the write is taken */
    if (name[0]) {
        pci_bind(name, addr);
    }
    return 0;
}

/* driver_override: a name, or none ("(null)" as sysfs shows it) */
static int override_write(const char *path, const char *value)
{
    char text[160];

    snprintf(text, sizeof(text), "%s\n", value[0] ? value : "(null)");
    return put(path, text);
}

/* VITRINE_HELPER_TEST_KILL_AT=N: killed right after the Nth write */
static void count_write(void)
{
    const char *at = getenv("VITRINE_HELPER_TEST_KILL_AT");

    if (at && ++writes == atoi(at)) {
        journal("killed");
        raise(SIGKILL);
    }
}

int sys_write(const char *path, const char *value)
{
    const char *rel = path + strlen(root);
    const char *base = strrchr(path, '/') + 1;
    const char *before = getenv("VITRINE_HELPER_TEST_KILL_BEFORE");
    char v[128];
    int err;

    if (before && before[0] && strstr(rel, before)) {
        journal("killed before %s", rel);
        raise(SIGKILL);
    }

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
    } else if (strcmp(rel, THP_SHMEM) == 0) {
        err = thp_write(path, v);
    } else if (strcmp(base, "energy_performance_preference") == 0) {
        err = epp_write(path, v);
    } else if (strcmp(base, "driver_override") == 0) {
        err = override_write(path, v);
    } else if (strncmp(rel, PCI_DIR "/drivers/", sizeof(PCI_DIR "/drivers/") - 1) == 0 &&
               (strcmp(base, "bind") == 0 || strcmp(base, "unbind") == 0)) {
        const char *driver = rel + sizeof(PCI_DIR "/drivers/") - 1;
        char name[64];

        snprintf(name, sizeof(name), "%.*s", (int)strcspn(driver, "/"), driver);
        err = strcmp(base, "bind") == 0 ? pci_bind(name, v) : pci_unbind(name, v);
    } else if (strcmp(rel, PCI_DIR "/drivers_probe") == 0) {
        err = pci_probe(v);
    } else {
        char stuck[PATH_MAX];

        snprintf(stuck, sizeof(stuck), "%s.stuck", path);
        /* taken, not kept */
        err = access(stuck, F_OK) == 0 ? 0 : put(path, value);
    }
    journal("write %s %s%s", rel, v, err ? " FAILED" : "");
    count_write();
    return err;
}

/*
 * /dev/cpu_dma_latency: a request is a process's open file of it, which
 * the kernel drops when that file closes.  Here <path>.<pid> holds its
 * value, locked (LOCK_EX) through the file returned: the lock goes when it
 * closes or the process dies, whatever the way, and a request file nobody
 * locks is no request.  The device is <path>, there or not; with
 * <path>.refuse beside it, opening it fails with EACCES.
 */
int sys_latency_request(const char *path, int us)
{
    char file[PATH_MAX], text[32];
    size_t len;
    int fd;

    if (access(path, F_OK) < 0) {
        return -errno;
    }
    snprintf(file, sizeof(file), "%s.refuse", path);
    if (access(file, F_OK) == 0) {
        journal("request %s %d FAILED", path + strlen(root), us);
        return -EACCES;
    }
    snprintf(file, sizeof(file), "%s.%d", path, (int)getpid());
    fd = open(file, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -errno;
    }
    snprintf(text, sizeof(text), "%d\n", us);
    len = strlen(text);
    if (flock(fd, LOCK_EX | LOCK_NB) < 0 || write(fd, text, len) != (ssize_t)len) {
        close(fd);
        return -EIO;
    }
    journal("request %s %d", path + strlen(root), us);
    count_write();
    return fd;
}

void sys_latency_release(int fd)
{
    journal("release " CPU_LATENCY_DEV);
    close(fd);
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

/* the policy as the kernel keeps it, SCHED_RESET_ON_FORK too (the saved
   file shows it); read as sched_getscheduler() is, through sys_getsched() */
static void get_entry(struct sched_entry *e, void *data)
{
    struct sched_args *a = data;

    a->policy = e->policy & ~SCHED_RESET_ON_FORK;
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
        journal("sched %d %s %d%s", (int)tid,
                (policy & ~SCHED_RESET_ON_FORK) == SCHED_FIFO ? "fifo" : "other", priority,
                policy & SCHED_RESET_ON_FORK ? " reset-on-fork" : "");
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

/*
 * Locked memory limits, as real-time time limits: <root>/memlock-<pid>
 * ("soft hard"), else the process's own; <root>/memlock.refuse makes a
 * change fail with EPERM
 */
int sys_getmemlock(pid_t pid, rlim_t *soft, rlim_t *hard)
{
    char path[PATH_MAX], text[64], a[32], b[32];
    struct rlimit own;
    int fd;

    snprintf(path, sizeof(path), "%s/memlock-%d", root, (int)pid);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) >= 0) {
        ssize_t n = read(fd, text, sizeof(text) - 1);

        close(fd);
        text[n > 0 ? n : 0] = '\0';
        if (sscanf(text, "%31s %31s", a, b) == 2 && rttime_parse(a, soft) && rttime_parse(b, hard)) {
            return 0;
        }
        return -EIO;
    }
    if (prlimit(pid, RLIMIT_MEMLOCK, NULL, &own) < 0) {
        return -errno;
    }
    *soft = own.rlim_cur;
    *hard = own.rlim_max;
    return 0;
}

int sys_setmemlock(pid_t pid, rlim_t soft, rlim_t hard)
{
    char path[PATH_MAX], a[32], b[32];
    const char *sw = rttime_word(soft, a, sizeof(a)), *hw = rttime_word(hard, b, sizeof(b));
    int fd;

    if (kill(pid, 0) < 0 && errno == ESRCH) {
        return -ESRCH;
    }
    snprintf(path, sizeof(path), "%s/memlock.refuse", root);
    if (access(path, F_OK) == 0) {
        return -EPERM;
    }
    snprintf(path, sizeof(path), "%s/memlock-%d", root, (int)pid);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -errno;
    }
    dprintf(fd, "%s %s\n", sw, hw);
    close(fd);
    journal("memlock %d %s %s", (int)pid, sw, hw);
    count_write();
    return 0;
}

/* A module: its driver's folder appears (with its bind and unbind) */
int sys_load_module(const char *name, char *err, size_t size)
{
    char path[PATH_MAX], file[PATH_MAX + 16];

    snprintf(path, sizeof(path), "%s/modprobe.refuse", root);
    if (access(path, F_OK) == 0) {
        snprintf(err, size, "modprobe: FATAL: Module %s not found", name);
        journal("modprobe %s FAILED", name);
        return -1;
    }
    snprintf(path, sizeof(path), "%s" PCI_DIR "/drivers/%s", root, name);
    if (mkdir(path, 0755) < 0 && errno != EEXIST) {
        snprintf(err, size, "%s", strerror(errno));
        return -1;
    }
    snprintf(file, sizeof(file), "%s/bind", path);
    put(file, "");
    snprintf(file, sizeof(file), "%s/unbind", path);
    put(file, "");
    journal("modprobe %s", name);
    count_write();
    return 0;
}

/* A node's owner and mode: its <node>.owner, root's 0600 without one */
int sys_node_get(const char *path, uid_t *uid, gid_t *gid, mode_t *mode)
{
    char side[PATH_MAX], text[64];
    unsigned u, g, m;

    if (access(path, F_OK) != 0) {
        return -ENOENT;
    }
    snprintf(side, sizeof(side), "%s.owner", path);
    if (sys_read(side, text, sizeof(text)) <= 0 || sscanf(text, "%u %u %o", &u, &g, &m) != 3) {
        u = g = 0;
        m = 0600;
    }
    *uid = (uid_t)u;
    *gid = (gid_t)g;
    *mode = (mode_t)m;
    return 0;
}

int sys_node_set(const char *path, uid_t uid, gid_t gid, mode_t mode)
{
    char side[PATH_MAX], text[64];
    int err;

    if (access(path, F_OK) != 0) {
        return -ENOENT;
    }
    snprintf(side, sizeof(side), "%s/chown.refuse", root);
    if (access(side, F_OK) == 0) {
        journal("chown %s FAILED", path + strlen(root));
        return -EPERM;
    }
    snprintf(side, sizeof(side), "%s.owner", path);
    snprintf(text, sizeof(text), "%u %u %o\n", (unsigned)uid, (unsigned)gid, (unsigned)mode);
    err = put(side, text);
    journal("chown %s %u %u %o%s", path + strlen(root), (unsigned)uid, (unsigned)gid,
            (unsigned)mode, err ? " FAILED" : "");
    count_write();
    return err;
}

int sys_group_busy(const char *path)
{
    if (access(path, F_OK) != 0) {
        return -ENOENT;
    }
    return node_held(path) ? 1 : 0;
}

/* A node's number: its <node>.rdev */
int sys_rdev(const char *path, bool *block, unsigned *maj, unsigned *min)
{
    char side[PATH_MAX], text[64], kind;

    snprintf(side, sizeof(side), "%s.rdev", path);
    if (sys_read(side, text, sizeof(text)) <= 0) {
        return -ENOENT;
    }
    if (sscanf(text, "%u:%u", maj, min) == 2) {
        *block = false;
        return 0;
    }
    if (sscanf(text, "%c %u:%u", &kind, maj, min) == 3) {
        *block = kind == 'b';
        return 0;
    }
    return -EINVAL;
}

/* The processes of <root>/users: "c|b MAJ:MIN PID COMM" for each node one has open */
int sys_device_users(const struct devnum *devs, int ndevs, struct devuser *users, int max)
{
    char path[PATH_MAX], *text, *line, *save = NULL;
    int n = 0;

    snprintf(path, sizeof(path), "%s/users", root);
    if (!(text = sys_read_all(path, 1 << 20))) {
        return 0;
    }
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        bool other = false, sound = false;
        char kind, comm[16], exe[128] = "";
        unsigned maj, min;
        int pid, j;

        if (sscanf(line, "%c %u:%u %d %15s %127s", &kind, &maj, &min, &pid, comm, exe) < 5) {
            continue;
        }
        for (int i = 0; i < ndevs; i++) {
            if (devs[i].block == (kind == 'b') && devs[i].maj == maj &&
                (devs[i].min == DEVNUM_ANY || devs[i].min == min)) {
                sound |= devs[i].sound;
                other |= !devs[i].sound;
            }
        }
        if (!other && !sound) {
            continue;
        }
        /* one entry per process */
        for (j = 0; j < n && users[j].pid != (pid_t)pid; j++) {
        }
        if (j == n) {
            if (n == max) {
                continue;
            }
            users[n] = (struct devuser){.pid = (pid_t)pid};
            snprintf(users[n].comm, sizeof(users[n].comm), "%s", comm);
            snprintf(users[n].exe, sizeof(users[n].exe), "%s", exe);
            n++;
        }
        users[j].other |= other;
    }
    free(text);
    return n;
}

char *sys_read_all(const char *path, size_t max)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
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
    const char *ms = getenv("VITRINE_HELPER_TEST_VFIO_EXPIRY_MS");

    return ms ? atoi(ms) : VFIO_EXPIRY_MS;
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
