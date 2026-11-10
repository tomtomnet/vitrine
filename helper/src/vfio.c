/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PCI devices handed to a VM (vitrine-helper devices and devices-bind):
 * before the VM's QEMU starts, the IOMMU groups of the devices it passes
 * through (-device vfio-pci,host=...) on vfio-pci, their /dev/vfio/<group>
 * the caller's, and the locked memory limit of the process that started the
 * helper - vitrine, whose QEMU inherits it - raised; everything back when
 * that QEMU ends.
 *
 * Before QEMU, as QEMU opens /dev/vfio/<group> and has the kernel pin all of
 * the guest's memory for the device's DMA while it creates the device,
 * before its guest runs (-S included); and the IOMMU driver checks the
 * pinned pages against the RLIMIT_MEMLOCK QEMU started with.  So a group
 * made ready waits for the next watch, VFIO_EXPIRY_MS at most, and goes
 * back with the watched QEMU, crash included; with the helper's end (input
 * closed without a QEMU, release, a signal) if no QEMU took it.  The
 * parent's limit goes back as soon as its QEMU is watched: QEMU has its own
 * copy; without a QEMU, at the first expiry, even when a group stays
 * because something has it open.  The memory to lock is capped at this
 * computer's memory and 1 GiB, which the answer says.
 *
 * One record per group, vfio-<group> (settings.c's holds): written before
 * the first change - each device's driver, driver_override and
 * vendor:device before, the node's owner and mode before, the caller, then
 * the QEMU it went to - and held by one helper only: a group opens once,
 * two VMs cannot share it.  A record nobody holds (a helper that died) goes
 * back at the next helper's start, whatever its verb: devices, which takes
 * no device from a driver, may give one back.  The parent's limit has a
 * record of its own, memlock-<pid>, shared by the helpers that raise it
 * (VMs starting side by side): the first saves it, the last one out puts it
 * back, if it still reads what was written.
 *
 * The settings' lock (lock_all()) is held only to take, share or let go of
 * the holds and for the limit, never across a driver's bind or unbind:
 * a driver may take its time, and the other helpers - a VM's settings, its
 * real-time threads - would wait for it.  The group's hold keeps the other
 * helpers off the group meanwhile.
 *
 * Nothing that would break the host.  A device is taken from a host driver
 * by devices-bind only, and never the GPU the computer started on
 * (boot_vga), a GPU with a display connected, a device whose nodes a
 * process holds open (the compositor, a game, another VM's QEMU; the sound
 * servers' hold on a card's ALSA nodes aside - PipeWire, WirePlumber and
 * PulseAudio, by the program they run - as they let go of a card that
 * goes), a disk mounted, swapped on or used by another block device, a
 * network interface that is up, nor a platform device (bridges, SMBus, the
 * IOMMU...).  The whole group or nothing, its bridges untouched.  A group
 * goes back only once nothing has it open: unbinding a vfio device that
 * something holds waits in the kernel until it lets go.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "helper.h"

typedef unsigned long long u64;

/* 0000:6e:00.0 */
#define ADDR_LEN 12
/* The devices of an IOMMU group read at most */
#define MAX_GROUP_DEVICES 64
/* The device nodes under a device looked at, and how deep its folders go */
#define MAX_NODES 256
#define WALK_DEPTH 12
/* The memory to lock, in MiB: no more than any computer has */
#define MAX_LOCK_MIB (1ULL << 26)

struct vdev {
    char addr[ADDR_LEN + 1];
    char driver[64];        /* its driver before: "" for none */
    char override[64];      /* its driver_override before: "" for none */
    char id[16];            /* vendor:device, "1002:164e": the same device still */
};

struct vgroup {
    unsigned id;
    unsigned uid;           /* the caller, whose the node was made */
    struct hold *hold;
    struct vdev devs[MAX_VFIO_DEVICES];     /* those this helper puts on vfio-pci */
    int ndevs;
    bool node;              /* the node's owner and mode were changed, from: */
    unsigned node_uid, node_gid, node_mode;
    pid_t qemu;             /* the watched QEMU it went to; 0 while it waits for one */
    u64 qemu_start;
    long long expires;      /* while it waits: CLOCK_MONOTONIC ms */
    bool overdue;           /* it waits past its first time: open, its watch late */
};

static struct vgroup groups[MAX_VFIO_GROUPS];
static int ngroups;
/* dead helpers' records the recovery took over, given back after it */
static struct {
    char key[32];
    struct hold *hold;
} orphans[MAX_VFIO_GROUPS];
static int norphans;
/* this helper's hold on its parent's locked memory limit */
static struct hold *memlock_hold;
/* why groups go back now, for their "restored" line; a rollback says none */
static const char *back_why;
static bool undoing;

/* The device nodes under a PCI device, and what uses them */
struct walk {
    struct devnum nums[MAX_NODES];
    char names[MAX_NODES][32];      /* each node's folder: card1, nvme0n1p2 */
    char holder[MAX_NODES][32];     /* a block device's first holder (dm-0), "" for none */
    int n;
    bool full;                      /* more than MAX_NODES */
    char up[32];                    /* a network interface that is up */
};

/* @text appended to @list, ", " between */
static void add(char *list, size_t size, const char *text)
{
    const size_t len = strlen(list);

    if (len < size) {
        snprintf(list + len, size - len, "%s%s", len ? ", " : "", text);
    }
}

/* printf to the end of @buf, never past it */
static void catf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void catf(char *buf, size_t size, const char *fmt, ...)
{
    const size_t len = strlen(buf);
    va_list ap;

    if (len + 1 >= size) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf + len, size - len, fmt, ap);
    va_end(ap);
}

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int hexval(char c)
{
    return c >= 'a' ? c - 'a' + 10 : c - '0';
}

/* 0000:6e:00.0 as sysfs names a PCI device: lowercase hex, its device below
   0x20, its function below 8 */
static bool addr_ok(const char *s)
{
    if (strlen(s) != ADDR_LEN || s[4] != ':' || s[7] != ':' || s[10] != '.') {
        return false;
    }
    for (int i = 0; i < ADDR_LEN; i++) {
        const char c = s[i];

        if (i != 4 && i != 7 && i != 10 && !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return hexval(s[8]) * 16 + hexval(s[9]) < 0x20 && hexval(s[11]) < 8;
}

/* A driver's name as sysfs shows it: amdgpu, snd_hda_intel, vfio-pci */
static bool name_ok(const char *s)
{
    const size_t len = strlen(s);

    if (len == 0 || len > 63 || strcmp(s, ".") == 0 || strcmp(s, "..") == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = s[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }
    return true;
}

/* --- sysfs --- */

static int attr_read(const char *path, char *buf, size_t size)
{
    char full[PATH_MAX];

    if (!rooted(full, sizeof(full), path)) {
        return -ENAMETOOLONG;
    }
    return sys_read(full, buf, size);
}

/* The first line of @path, without its newline */
static bool attr_word(const char *path, char *buf, size_t size)
{
    if (attr_read(path, buf, size) < 0) {
        return false;
    }
    buf[strcspn(buf, "\n")] = '\0';
    return true;
}

static int attr_write(const char *path, const char *value)
{
    char full[PATH_MAX];

    if (!rooted(full, sizeof(full), path)) {
        return -ENAMETOOLONG;
    }
    return sys_write(full, value);
}

/* A proc file whole (rooted: the fake tree's in the tests) */
static char *read_proc(const char *path)
{
    char full[PATH_MAX];

    return rooted(full, sizeof(full), path) ? sys_read_all(full, 4 << 20) : NULL;
}

/* PCI_DIR/devices/@addr/@attr */
static void dev_path(char *out, size_t size, const char *addr, const char *attr)
{
    snprintf(out, size, PCI_DIR "/devices/%s/%s", addr, attr);
}

static bool dev_exists(const char *addr)
{
    char path[PATH_MAX], full[PATH_MAX];
    struct stat st;

    dev_path(path, sizeof(path), addr, "");
    return rooted(full, sizeof(full), path) && stat(full, &st) == 0 && S_ISDIR(st.st_mode);
}

/* The name of the link @attr of @addr's folder (driver, iommu_group), ""
   when it has none: false when it cannot be read */
static bool dev_link(const char *addr, const char *attr, char *out, size_t size)
{
    char path[PATH_MAX], full[PATH_MAX], link[PATH_MAX];
    const char *base;
    ssize_t n;

    out[0] = '\0';
    dev_path(path, sizeof(path), addr, attr);
    if (!rooted(full, sizeof(full), path)) {
        return false;
    }
    if ((n = readlink(full, link, sizeof(link) - 1)) < 0) {
        return errno == ENOENT;
    }
    link[n] = '\0';
    base = strrchr(link, '/') ? strrchr(link, '/') + 1 : link;
    if (strlen(base) >= size) {
        return false;
    }
    memcpy(out, base, strlen(base) + 1);
    return true;
}

/* @addr's driver, "" for none */
static bool dev_driver(const char *addr, char *out, size_t size)
{
    return dev_link(addr, "driver", out, size) && (!out[0] || name_ok(out));
}

/* @addr's driver_override, "" for none */
static bool dev_override(const char *addr, char *out, size_t size)
{
    char path[PATH_MAX];

    dev_path(path, sizeof(path), addr, "driver_override");
    if (!attr_word(path, out, size)) {
        return false;
    }
    if (strcmp(out, "(null)") == 0) {
        out[0] = '\0';
    }
    return !out[0] || name_ok(out);
}

static bool dev_group(const char *addr, unsigned *group)
{
    char name[32];
    u64 v;

    if (!dev_link(addr, "iommu_group", name, sizeof(name)) || !name[0] ||
        !parse_uint(name, 1000000, &v)) {
        return false;
    }
    *group = (unsigned)v;
    return true;
}

static unsigned dev_class(const char *addr)
{
    char path[PATH_MAX], word[32];

    dev_path(path, sizeof(path), addr, "class");
    return attr_word(path, word, sizeof(word)) ? (unsigned)strtoul(word, NULL, 16) : 0;
}

/* @addr's vendor:device, "1002:164e" */
static bool dev_id(const char *addr, char *out, size_t size)
{
    char path[PATH_MAX], vendor[16], device[16];
    unsigned long v, d;

    dev_path(path, sizeof(path), addr, "vendor");
    if (!attr_word(path, vendor, sizeof(vendor))) {
        return false;
    }
    dev_path(path, sizeof(path), addr, "device");
    if (!attr_word(path, device, sizeof(device))) {
        return false;
    }
    v = strtoul(vendor, NULL, 16);
    d = strtoul(device, NULL, 16);
    snprintf(out, size, "%04lx:%04lx", v & 0xffff, d & 0xffff);
    return true;
}

static bool driver_loaded(const char *name)
{
    char path[PATH_MAX], full[PATH_MAX];
    struct stat st;

    snprintf(path, sizeof(path), PCI_DIR "/drivers/%s", name);
    return rooted(full, sizeof(full), path) && stat(full, &st) == 0 && S_ISDIR(st.st_mode);
}

/* What @cls makes a device of the platform's, which stays with the host:
   NULL for another */
static const char *platform_device(unsigned cls)
{
    const unsigned base = cls >> 16, sub = (cls >> 8) & 0xff;

    if (base == 0x06) {
        return "a bridge";
    }
    if (base == 0x08 && (sub <= 0x03 || sub == 0x06)) {
        /* the interrupt and DMA controllers, the timer, the clock, the IOMMU */
        return "a system peripheral of the platform";
    }
    if (base == 0x0c && sub == 0x05) {
        return "the SMBus controller";
    }
    return NULL;
}

static int cmp_addr(const void *a, const void *b)
{
    return strcmp(a, b);
}

/* The devices of IOMMU group @id, sorted: their count, -1 when they cannot
   be read (or more than @max) */
static int group_devices(unsigned id, char (*out)[ADDR_LEN + 1], int max)
{
    char path[PATH_MAX], full[PATH_MAX];
    struct dirent *e;
    DIR *d;
    int n = 0;

    snprintf(path, sizeof(path), IOMMU_GROUPS_DIR "/%u/devices", id);
    if (!rooted(full, sizeof(full), path) || !(d = opendir(full))) {
        return -1;
    }
    while ((e = readdir(d))) {
        if (!addr_ok(e->d_name)) {
            continue;
        }
        if (n == max) {
            closedir(d);
            return -1;
        }
        /* addr_ok(): ADDR_LEN long */
        memcpy(out[n++], e->d_name, ADDR_LEN + 1);
    }
    closedir(d);
    qsort(out, (size_t)n, ADDR_LEN + 1, cmp_addr);
    return n;
}

/* --- what uses a device --- */

/* @name in the folder @dirfd, NUL terminated: its length, or -1 */
static int read_at(int dirfd, const char *name, char *buf, size_t size)
{
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    ssize_t n;

    if (fd < 0) {
        return -1;
    }
    n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    buf[n] = '\0';
    return (int)n;
}

/* The folder @dirfd, @name, in @parent: a device node's ("dev"), with a
   block device's holders, or a network interface (net/IF) and its state */
static void walk_here(int dirfd, const char *name, const char *parent, struct walk *w)
{
    char text[64], link[PATH_MAX];
    unsigned maj, min;
    ssize_t n;

    if (strcmp(parent, "net") == 0) {
        /* "up": carrier and administratively up; down, dormant... are not */
        if (!w->up[0] && read_at(dirfd, "operstate", text, sizeof(text)) > 0 &&
            strcmp(text, "up\n") == 0) {
            snprintf(w->up, sizeof(w->up), "%s", name);
        }
        return;
    }
    if (read_at(dirfd, "dev", text, sizeof(text)) <= 0 || sscanf(text, "%u:%u", &maj, &min) != 2) {
        return;
    }
    if (w->n == MAX_NODES) {
        w->full = true;
        return;
    }
    w->nums[w->n] = (struct devnum){.maj = maj, .min = min};
    if ((n = readlinkat(dirfd, "subsystem", link, sizeof(link) - 1)) > 0) {
        const char *base;

        link[n] = '\0';
        base = strrchr(link, '/') ? strrchr(link, '/') + 1 : link;
        w->nums[w->n].block = strcmp(base, "block") == 0;
        w->nums[w->n].sound = strcmp(base, "sound") == 0;
    }
    snprintf(w->names[w->n], sizeof(w->names[0]), "%s", name);
    w->holder[w->n][0] = '\0';
    if (w->nums[w->n].block) {
        /* device mapper (LUKS, LVM), RAID: what is built on it */
        const int hfd = openat(dirfd, "holders", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        DIR *hd = hfd >= 0 ? fdopendir(hfd) : NULL;
        struct dirent *e;

        if (!hd && hfd >= 0) {
            close(hfd);
        }
        while (hd && (e = readdir(hd))) {
            if (e->d_name[0] != '.' && !w->holder[w->n][0]) {
                snprintf(w->holder[w->n], sizeof(w->holder[0]), "%.31s", e->d_name);
            }
        }
        if (hd) {
            closedir(hd);
        }
    }
    w->n++;
}

/* The folders under @fd (taken), @depth levels down: real folders only,
   never the links sysfs has to others (subsystem, driver, device...) */
static void walk(int fd, const char *name, const char *parent, int depth, struct walk *w)
{
    DIR *d = fdopendir(fd);
    struct dirent *e;

    if (!d) {
        close(fd);
        return;
    }
    walk_here(dirfd(d), name, parent, w);
    while (depth > 0 && (e = readdir(d))) {
        struct stat st;
        int sub;

        if (e->d_name[0] == '.') {
            continue;
        }
        if (e->d_type == DT_UNKNOWN) {
            if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 || !S_ISDIR(st.st_mode)) {
                continue;
            }
        } else if (e->d_type != DT_DIR) {
            continue;
        }
        sub = openat(dirfd(d), e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (sub >= 0) {
            walk(sub, e->d_name, name, depth - 1, w);
        }
    }
    closedir(d);
}

/*
 * The sound servers, which let go of a card's ALSA nodes when it goes: by
 * the program they run, the installed one (pipewire-pulse is pipewire's),
 * not by the name a process gives itself
 */
static bool sound_server(const struct devuser *user)
{
    static const char *const servers[] = {"/usr/bin/pipewire", "/usr/bin/wireplumber",
                                          "/usr/bin/pipewire-pulse", "/usr/bin/pulseaudio", NULL};

    for (int i = 0; servers[i]; i++) {
        if (strcmp(user->exe, servers[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Who has one of @nums open, but a sound server holding ALSA nodes only:
   "gnome-shell (pid 2345)" in @out; 0 for nobody, -1 when it cannot be told */
static int users_of(const struct devnum *nums, int n, char *out, size_t size)
{
    struct devuser users[8];
    const int found = n > 0 ? sys_device_users(nums, n, users, 8) : 0;
    int named = 0;

    out[0] = '\0';
    if (found < 0) {
        return -1;
    }
    for (int i = 0; i < found; i++) {
        if (!users[i].other && sound_server(&users[i])) {
            continue;
        }
        if (named < 3) {
            catf(out, size, "%s%s (pid %d)", named ? ", " : "", users[i].comm[0] ? users[i].comm : "?",
                 (int)users[i].pid);
        } else if (named == 3) {
            catf(out, size, " and others");
        }
        named++;
    }
    return named > 0;
}

/* The block device of @w at @maj:@min, or -1 */
static int block_num(const struct walk *w, unsigned maj, unsigned min)
{
    for (int i = 0; i < w->n; i++) {
        if (w->nums[i].block && w->nums[i].maj == maj && w->nums[i].min == min) {
            return i;
        }
    }
    return -1;
}

/* The block device of @w that the node @source is (a mount's source, a
   swap's), or -1: only nodes under /dev are looked at */
static int block_at(const struct walk *w, const char *source)
{
    char full[PATH_MAX];
    unsigned maj, min;
    bool block;

    if (strncmp(source, "/dev/", 5) != 0 || !rooted(full, sizeof(full), source) ||
        sys_rdev(full, &block, &maj, &min) < 0 || !block) {
        return -1;
    }
    return block_num(w, maj, min);
}

/* A disk of @w (on @addr) mounted, swapped on or built upon: why in @why */
static bool disk_in_use(const struct walk *w, const char *addr, char *why, size_t size)
{
    char *text, *line, *save = NULL;
    bool any = false;

    for (int i = 0; i < w->n; i++) {
        if (w->nums[i].block && w->holder[i][0]) {
            snprintf(why, size, "%s on %s is in use by %s (an encrypted or LVM volume, RAID)",
                     w->names[i], addr, w->holder[i]);
            return true;
        }
        any |= w->nums[i].block;
    }
    if (!any) {
        return false;
    }
    /*
     * The mounts: the device number of the file system (ext4, xfs), or its
     * source (btrfs gives each subvolume a number of its own)
     */
    if ((text = read_proc("/proc/self/mountinfo"))) {
        for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char point[256], fstype[64], source[256] = "";
            const char *dash = strstr(line, " - ");
            unsigned maj, min;
            int i;

            if (sscanf(line, "%*s %*s %u:%u %*s %255s", &maj, &min, point) != 3) {
                continue;
            }
            if (dash && sscanf(dash + 3, "%63s %255s", fstype, source) != 2) {
                source[0] = '\0';
            }
            if ((i = block_num(w, maj, min)) >= 0 || (i = block_at(w, source)) >= 0) {
                snprintf(why, size, "%s on %s is mounted on %s", w->names[i], addr, point);
                free(text);
                return true;
            }
        }
        free(text);
    }
    if ((text = read_proc("/proc/swaps"))) {
        save = NULL;
        for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char source[256];
            int i;

            if (sscanf(line, "%255s", source) == 1 && (i = block_at(w, source)) >= 0) {
                snprintf(why, size, "%s on %s is in use as swap", w->names[i], addr);
                free(text);
                return true;
            }
        }
        free(text);
    }
    return false;
}

/* NVIDIA's own nodes (/dev/nvidia0, nvidiactl...), not under sysfs: every
   number of its majors */
static void nvidia_nodes(struct walk *w)
{
    char *text = read_proc("/proc/devices"), *line, *save = NULL;
    bool chars = false;

    if (!text) {
        return;
    }
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char name[64];
        unsigned maj;

        if (strncmp(line, "Character devices:", 18) == 0) {
            chars = true;
        } else if (strncmp(line, "Block devices:", 14) == 0) {
            chars = false;
        } else if (chars && w->n < MAX_NODES && sscanf(line, "%u %63s", &maj, name) == 2 &&
                   strncmp(name, "nvidia", 6) == 0) {
            w->nums[w->n] = (struct devnum){.maj = maj, .min = DEVNUM_ANY};
            snprintf(w->names[w->n], sizeof(w->names[0]), "%.31s", name);
            w->holder[w->n][0] = '\0';
            w->n++;
        }
    }
    free(text);
}

/* A display connected to @addr's GPU: its connector (DP-1) in @out */
static bool display_connected(const char *addr, char *out, size_t size)
{
    char path[PATH_MAX], full[PATH_MAX];
    struct dirent *c;
    bool found = false;
    DIR *drm;

    dev_path(path, sizeof(path), addr, "drm");
    if (!rooted(full, sizeof(full), path) || !(drm = opendir(full))) {
        return false;
    }
    while (!found && (c = readdir(drm))) {
        char card[PATH_MAX];
        struct dirent *k;
        size_t len;
        unsigned n;
        DIR *cd;

        if (sscanf(c->d_name, "card%u", &n) != 1 || strchr(c->d_name, '-')) {
            continue;
        }
        if (snprintf(card, sizeof(card), "%s/%s", full, c->d_name) >= (int)sizeof(card) ||
            !(cd = opendir(card))) {
            continue;
        }
        len = strlen(c->d_name);
        /* card1-DP-1/status: connected, disconnected, unknown */
        while (!found && (k = readdir(cd))) {
            char status[32], file[PATH_MAX];

            if (strncmp(k->d_name, c->d_name, len) != 0 || k->d_name[len] != '-' ||
                snprintf(file, sizeof(file), "%s/%s/status", card, k->d_name) >= (int)sizeof(file)) {
                continue;
            }
            if (sys_read(file, status, sizeof(status)) > 0 && strcmp(status, "connected\n") == 0) {
                snprintf(out, size, "%s", k->d_name + len + 1);
                found = true;
            }
        }
        closedir(cd);
    }
    closedir(drm);
    return found;
}

/*
 * Why @addr, on the host driver @driver, cannot be taken from the host, in
 * @why: false when it can
 */
static bool dev_in_use(const char *addr, const char *driver, char *why, size_t size)
{
    char path[PATH_MAX], full[PATH_MAX], text[200];
    struct walk *w;
    int fd, users;

    /* the firmware's console GPU: the host's screen, and its desktop's */
    dev_path(path, sizeof(path), addr, "boot_vga");
    if (attr_word(path, text, sizeof(text)) && strcmp(text, "1") == 0) {
        snprintf(why, size, "%s is the GPU this computer started on (boot_vga)", addr);
        return true;
    }
    if (display_connected(addr, text, sizeof(text))) {
        snprintf(why, size, "a display is connected to %s (%s)", addr, text);
        return true;
    }
    dev_path(path, sizeof(path), addr, "");
    if (!rooted(full, sizeof(full), path) ||
        (fd = open(full, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) {
        snprintf(why, size, "cannot read the folder of %s", addr);
        return true;
    }
    if (!(w = calloc(1, sizeof(*w)))) {
        close(fd);
        snprintf(why, size, "%s", strerror(ENOMEM));
        return true;
    }
    walk(fd, addr, "", WALK_DEPTH, w);
    if (w->full) {
        snprintf(why, size, "%s has more devices under it than vitrine-helper looks at", addr);
    } else if (w->up[0]) {
        snprintf(why, size, "the network interface %s of %s is up: take it down first (sudo ip link "
                            "set %s down)", w->up, addr, w->up);
    } else if (!disk_in_use(w, addr, why, size)) {
        if (strcmp(driver, "nvidia") == 0) {
            nvidia_nodes(w);
        }
        users = users_of(w->nums, w->n, text, sizeof(text));
        if (users < 0) {
            snprintf(why, size, "cannot tell what uses %s", addr);
        } else if (users > 0) {
            snprintf(why, size, "%s is in use by %s", addr, text);
        } else {
            free(w);
            return false;
        }
    }
    free(w);
    return true;
}

/* @addr's own vfio node (/dev/vfio/devices/vfioN), which opens without its
   group: its number */
static bool vfio_cdev(const char *addr, unsigned *maj, unsigned *min)
{
    char path[PATH_MAX], full[PATH_MAX];
    struct dirent *e;
    bool found = false;
    DIR *d;

    dev_path(path, sizeof(path), addr, "vfio-dev");
    if (!rooted(full, sizeof(full), path) || !(d = opendir(full))) {
        return false;
    }
    while (!found && (e = readdir(d))) {
        char file[PATH_MAX], text[32];

        if (strncmp(e->d_name, "vfio", 4) == 0 &&
            snprintf(file, sizeof(file), "%s/%s/dev", full, e->d_name) < (int)sizeof(file) &&
            sys_read(file, text, sizeof(text)) > 0) {
            found = sscanf(text, "%u:%u", maj, min) == 2;
        }
    }
    closedir(d);
    return found;
}

/*
 * Who has IOMMU group @id open - another VM's QEMU, a program - in @why
 * ("IOMMU group 21 is in use by qemu-system-x86 (pid 1234)"): false when
 * nothing does
 */
static bool group_held(unsigned id, char *why, size_t size)
{
    char path[64], node[PATH_MAX], who[200] = "";
    char devs[MAX_GROUP_DEVICES][ADDR_LEN + 1];
    struct devnum nums[MAX_GROUP_DEVICES + 1];
    unsigned maj, min;
    int n = 0, busy, ndevs, users;
    bool block;

    snprintf(path, sizeof(path), VFIO_DEV_DIR "/%u", id);
    if (!rooted(node, sizeof(node), path)) {
        return false;
    }
    busy = sys_group_busy(node);
    if (sys_rdev(node, &block, &maj, &min) == 0 && !block) {
        nums[n++] = (struct devnum){.maj = maj, .min = min};
    }
    ndevs = group_devices(id, devs, MAX_GROUP_DEVICES);
    for (int i = 0; i < ndevs && n < MAX_GROUP_DEVICES + 1; i++) {
        if (vfio_cdev(devs[i], &maj, &min)) {
            nums[n++] = (struct devnum){.maj = maj, .min = min};
        }
    }
    users = users_of(nums, n, who, sizeof(who));
    if (busy != 1 && users <= 0) {
        return false;
    }
    snprintf(why, size, "IOMMU group %u is in use by %s", id, users > 0 ? who : "another program");
    return true;
}

/* --- records --- */

static void group_key(char *out, size_t size, unsigned id)
{
    snprintf(out, size, "vfio-%u", id);
}

/* "group N", "uid U", "dev ADDR DRIVER OVERRIDE VENDOR:DEVICE" ("-" for
   none), "node UID GID MODE" (before), "qemu PID START" */
static bool record_write(const struct vgroup *g)
{
    char key[32], text[MAX_VFIO_DEVICES * 180 + 256] = "";

    group_key(key, sizeof(key), g->id);
    catf(text, sizeof(text), "group %u\nuid %u\n", g->id, g->uid);
    for (int i = 0; i < g->ndevs; i++) {
        catf(text, sizeof(text), "dev %s %s %s %s\n", g->devs[i].addr,
             g->devs[i].driver[0] ? g->devs[i].driver : "-",
             g->devs[i].override[0] ? g->devs[i].override : "-",
             g->devs[i].id[0] ? g->devs[i].id : "-");
    }
    if (g->node) {
        catf(text, sizeof(text), "node %u %u %o\n", g->node_uid, g->node_gid, g->node_mode);
    }
    if (g->qemu) {
        catf(text, sizeof(text), "qemu %d %llu\n", (int)g->qemu, g->qemu_start);
    }
    return state_write(key, text);
}

/* @text, the record of @key, into @g: false when it is not one */
static bool record_parse(char *text, const char *key, struct vgroup *g)
{
    char *line, *save = NULL;
    bool have_group = false, have_uid = false;
    unsigned id;

    memset(g, 0, sizeof(*g));
    if (sscanf(key, "vfio-%u", &id) != 1) {
        return false;
    }
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char a[32], b[64], c[64], ids[16] = "-";
        unsigned u1, u2, u3;
        int pid;
        u64 start;

        if (sscanf(line, "group %u", &u1) == 1) {
            if (u1 != id) {
                return false;
            }
            g->id = id;
            have_group = true;
        } else if (sscanf(line, "uid %u", &u1) == 1) {
            g->uid = u1;
            have_uid = true;
        } else if (sscanf(line, "dev %31s %63s %63s %15s", a, b, c, ids) >= 3) {
            struct vdev *d;

            if (!addr_ok(a) || g->ndevs == MAX_VFIO_DEVICES) {
                return false;
            }
            d = &g->devs[g->ndevs++];
            memcpy(d->addr, a, ADDR_LEN + 1);
            snprintf(d->driver, sizeof(d->driver), "%s", strcmp(b, "-") == 0 ? "" : b);
            snprintf(d->override, sizeof(d->override), "%s", strcmp(c, "-") == 0 ? "" : c);
            snprintf(d->id, sizeof(d->id), "%s", strcmp(ids, "-") == 0 ? "" : ids);
            if ((d->driver[0] && !name_ok(d->driver)) || (d->override[0] && !name_ok(d->override))) {
                return false;
            }
        } else if (sscanf(line, "node %u %u %o", &u1, &u2, &u3) == 3) {
            g->node = true;
            g->node_uid = u1;
            g->node_gid = u2;
            g->node_mode = u3 & 07777;
        } else if (sscanf(line, "qemu %d %llu", &pid, &start) == 2 && pid > 1) {
            g->qemu = (pid_t)pid;
            g->qemu_start = start;
        }
    }
    return have_group && have_uid;
}

/* --- binding --- */

/* @d to vfio-pci: its driver_override first, then its driver lets go of it,
   then vfio-pci's bind, read back: 0, or -1 with why in @why */
static int bind_dev(const struct vdev *d, char *why, size_t size)
{
    char path[PATH_MAX], value[32], now[64];
    int err;

    snprintf(value, sizeof(value), "%s\n", d->addr);
    dev_path(path, sizeof(path), d->addr, "driver_override");
    if ((err = attr_write(path, "vfio-pci\n")) < 0) {
        snprintf(why, size, "cannot set the driver_override of %s: %s", d->addr, strerror(-err));
        return -1;
    }
    if (d->driver[0]) {
        snprintf(path, sizeof(path), PCI_DIR "/drivers/%s/unbind", d->driver);
        if ((err = attr_write(path, value)) < 0) {
            snprintf(why, size, "%s did not let go of %s: %s", d->driver, d->addr, strerror(-err));
            return -1;
        }
    }
    err = attr_write(PCI_DIR "/drivers/vfio-pci/bind", value);
    if (err < 0 || !dev_driver(d->addr, now, sizeof(now)) || strcmp(now, "vfio-pci") != 0) {
        snprintf(why, size, "vfio-pci did not take %s%s%s", d->addr, err < 0 ? ": " : "",
                 err < 0 ? strerror(-err) : "");
        return -1;
    }
    return 0;
}

/*
 * @d back to the driver it had, as far as it is still this helper's doing:
 * the same device still there (not unplugged, nor another in its place),
 * off vfio-pci, its driver_override back (if it still says vfio-pci), its
 * driver's bind - the kernel's choice only when that driver's module is
 * gone, but for pci-stub - read back.  What came of it appended to @back or
 * @left.
 */
static void give_back_dev(const struct vdev *d, char *back, size_t bsize, char *left, size_t lsize)
{
    char path[PATH_MAX], value[32], now[64], override[64], id[16], text[240];

    snprintf(value, sizeof(value), "%s\n", d->addr);
    if (!dev_exists(d->addr)) {
        snprintf(text, sizeof(text), "%s is gone", d->addr);
        add(left, lsize, text);
        return;
    }
    if (d->id[0] && dev_id(d->addr, id, sizeof(id)) && strcmp(id, d->id) != 0) {
        snprintf(text, sizeof(text), "%s is another device now (%s, it was %s): left as it is",
                 d->addr, id, d->id);
        add(left, lsize, text);
        return;
    }
    if (!dev_driver(d->addr, now, sizeof(now))) {
        snprintf(text, sizeof(text), "the driver of %s cannot be read", d->addr);
        add(left, lsize, text);
        return;
    }
    if (strcmp(now, "vfio-pci") == 0 && strcmp(d->driver, "vfio-pci") != 0) {
        attr_write(PCI_DIR "/drivers/vfio-pci/unbind", value);
        if (!dev_driver(d->addr, now, sizeof(now)) || strcmp(now, "vfio-pci") == 0) {
            snprintf(text, sizeof(text), "vfio-pci did not let go of %s", d->addr);
            add(left, lsize, text);
            return;
        }
    }
    if (dev_override(d->addr, override, sizeof(override)) && strcmp(override, "vfio-pci") == 0) {
        char put[80];

        snprintf(put, sizeof(put), "%s\n", d->override);
        dev_path(path, sizeof(path), d->addr, "driver_override");
        attr_write(path, put);
    }
    if (now[0] && strcmp(now, d->driver) != 0) {
        /* someone else's choice since: left as it is */
        snprintf(text, sizeof(text), "%s is on %s now: changed by someone else since", d->addr, now);
        add(left, lsize, text);
        return;
    }
    if (!now[0] && d->driver[0]) {
        if (driver_loaded(d->driver)) {
            snprintf(path, sizeof(path), PCI_DIR "/drivers/%s/bind", d->driver);
            attr_write(path, value);
        } else if (strcmp(d->driver, "pci-stub") != 0) {
            /*
             * its module unloaded meanwhile: the kernel's choice among the
             * drivers loaded.  Never when the driver is there but refused
             * it: another one could take it - radeon, nouveau, vfio-pci by
             * ids=
             */
            attr_write(PCI_DIR "/drivers_probe", value);
        }
        if (!dev_driver(d->addr, now, sizeof(now))) {
            now[0] = '\0';
        }
    }
    if (strcmp(now, d->driver) == 0) {
        snprintf(text, sizeof(text), "%s back to %s", d->addr, d->driver[0] ? d->driver : "no driver");
        add(back, bsize, text);
    } else if (now[0]) {
        snprintf(text, sizeof(text), "%s is on %s: %s did not take it back", d->addr, now, d->driver);
        add(left, lsize, text);
    } else {
        snprintf(text, sizeof(text), "%s has no driver: %s did not take it back", d->addr, d->driver);
        add(left, lsize, text);
    }
}

/* The node /dev/vfio/@id (under the root) */
static bool node_path(char *out, size_t size, unsigned id)
{
    char path[64];

    snprintf(path, sizeof(path), VFIO_DEV_DIR "/%u", id);
    return rooted(out, size, path);
}

/* The group's node the caller's (owner read and write), recorded first:
   false with why in @why */
static bool node_give(struct vgroup *g, char *why, size_t size)
{
    char node[PATH_MAX];
    uid_t uid;
    gid_t gid;
    mode_t mode;
    int err;

    if (!node_path(node, sizeof(node), g->id)) {
        snprintf(why, size, "no /dev/vfio/%u", g->id);
        return false;
    }
    if ((err = sys_node_get(node, &uid, &gid, &mode)) < 0) {
        snprintf(why, size, "no /dev/vfio/%u: %s", g->id, strerror(-err));
        return false;
    }
    if (uid == caller_uid && (mode & 0600) == 0600) {
        /* the caller's already (a udev rule of the owner's) */
        return true;
    }
    g->node = true;
    g->node_uid = (unsigned)uid;
    g->node_gid = (unsigned)gid;
    g->node_mode = (unsigned)mode;
    if (!record_write(g)) {
        snprintf(why, size, "cannot record it: %s", strerror(errno));
        g->node = false;
        return false;
    }
    if ((err = sys_node_set(node, caller_uid, gid, mode | 0600)) < 0) {
        snprintf(why, size, "cannot give /dev/vfio/%u to uid %u: %s", g->id, (unsigned)caller_uid,
                 strerror(-err));
        return false;
    }
    return true;
}

void vfio_restore(const char *key)
{
    char back[600] = "", left[600] = "", why[300], node[PATH_MAX], text[120];
    char *record = state_read(key);
    struct vgroup *g = calloc(1, sizeof(*g));

    if (!record || !g || !record_parse(record, key, g)) {
        if (record && g) {
            /* not a record of ours: nothing to give back */
            state_remove(key);
        }
        free(record);
        free(g);
        return;
    }
    free(record);
    /* the node first: from now on, only root opens it */
    if (g->node && node_path(node, sizeof(node), g->id)) {
        uid_t uid;
        gid_t gid;
        mode_t mode;

        if (sys_node_get(node, &uid, &gid, &mode) == 0 && uid == (uid_t)g->uid &&
            sys_node_set(node, g->node_uid, g->node_gid, g->node_mode) == 0 && !g->ndevs) {
            snprintf(text, sizeof(text), "/dev/vfio/%u back to uid %u", g->id, g->node_uid);
            add(back, sizeof(back), text);
        }
        g->node = false;
    }
    if (g->ndevs) {
        /* an unbind of a vfio device that something has open would wait for it */
        if (group_held(g->id, why, sizeof(why))) {
            record_write(g);
            reply("left vfio %u: %s: it stays on vfio-pci until a later vitrine-helper finds it "
                  "free", g->id, why);
            sys_log("IOMMU group %u left on vfio-pci: %s", g->id, why);
            free(g);
            return;
        }
        /*
         * In address order, function 0 first: a GPU before its audio
         * function, whose driver binds to the GPU's (snd_hda_intel to
         * amdgpu's HDMI, through the component framework)
         */
        for (int i = 0; i < g->ndevs; i++) {
            give_back_dev(&g->devs[i], back, sizeof(back), left, sizeof(left));
        }
    }
    state_remove(key);
    if (back[0] && !undoing) {
        reply("restored vfio %u: %s%s%s%s", g->id, back, back_why ? " (" : "", back_why ? back_why : "",
              back_why ? ")" : "");
    }
    if (left[0]) {
        reply("left vfio %u: %s", g->id, left);
    }
    if (back[0] || left[0]) {
        sys_log("IOMMU group %u: %s%s%s", g->id, back[0] ? back : "", back[0] && left[0] ? "; " : "",
                left);
    }
    free(g);
}

/* --- the parent's locked memory limit --- */

static const char *rlim_word(rlim_t v, char *buf, size_t size)
{
    if (v == RLIM_INFINITY) {
        return "unlimited";
    }
    snprintf(buf, size, "%llu", (u64)v);
    return buf;
}

static bool rlim_parse(const char *word, rlim_t *v)
{
    u64 n;

    if (strcmp(word, "unlimited") == 0) {
        *v = RLIM_INFINITY;
        return true;
    }
    if (!parse_uint(word, ~0ULL, &n)) {
        return false;
    }
    *v = (rlim_t)n;
    return true;
}

/* A limit as the replies say it: "8 MiB", "unlimited" */
static const char *rlim_text(rlim_t v, char *buf, size_t size)
{
    if (v == RLIM_INFINITY) {
        return "unlimited";
    }
    snprintf(buf, size, "%llu MiB", (u64)v >> 20);
    return buf;
}

/* "pid P start S was SOFT HARD wrote SOFT HARD" */
static bool memlock_write(const char *key, pid_t pid, u64 start, rlim_t was_soft, rlim_t was_hard,
                          rlim_t soft, rlim_t hard)
{
    char text[200], a[32], b[32], c[32], d[32];

    snprintf(text, sizeof(text), "pid %d start %llu was %s %s wrote %s %s\n", (int)pid, start,
             rlim_word(was_soft, a, sizeof(a)), rlim_word(was_hard, b, sizeof(b)),
             rlim_word(soft, c, sizeof(c)), rlim_word(hard, d, sizeof(d)));
    return state_write(key, text);
}

static bool memlock_read(const char *key, pid_t *pid, u64 *start, rlim_t *was_soft,
                         rlim_t *was_hard, rlim_t *soft, rlim_t *hard)
{
    char *text = state_read(key), a[32], b[32], c[32], d[32];
    int p = 0;
    bool ok;

    ok = text &&
         sscanf(text, "pid %d start %llu was %31s %31s wrote %31s %31s", &p, start, a, b, c, d) == 6 &&
         p > 1 && rlim_parse(a, was_soft) && rlim_parse(b, was_hard) && rlim_parse(c, soft) &&
         rlim_parse(d, hard);
    free(text);
    *pid = (pid_t)p;
    return ok;
}

/*
 * Under the lock: the locked memory limit of the process that started the
 * helper at @bytes at least, soft and hard - its QEMU, started next,
 * inherits it - through the hold memlock-<pid>, the first holder saving
 * what it was; said in @said ("locked memory 3072 MiB (was 8 MiB)"), else
 * false and why in @why
 */
static bool memlock_raise(rlim_t bytes, char *said, size_t ssize, char *why, size_t wsize)
{
    rlim_t soft, hard, was_soft, was_hard, wrote_soft, wrote_hard, want_soft, want_hard, s, h;
    char key[32], a[32], b[32];
    const char *pwhy;
    bool first = false;
    pid_t pid, rpid;
    u64 start, rstart;
    int err;

    if (!parent_get(&pid, &start, &pwhy)) {
        snprintf(why, wsize, "cannot raise the locked memory limit: %s", pwhy);
        return false;
    }
    snprintf(key, sizeof(key), "memlock-%d", (int)pid);
    if (!memlock_hold && !(memlock_hold = hold_take(key, "on", &first))) {
        snprintf(why, wsize, "cannot take the hold of the locked memory limit: %s", strerror(errno));
        return false;
    }
    if ((err = sys_getmemlock(pid, &soft, &hard)) < 0) {
        snprintf(why, wsize, "cannot read the locked memory limit: %s", strerror(-err));
        goto fail;
    }
    /* what it was before the first holder: its record, else what it is */
    if (first || !memlock_read(key, &rpid, &rstart, &was_soft, &was_hard, &wrote_soft, &wrote_hard) ||
        rpid != pid || rstart != start) {
        was_soft = soft;
        was_hard = hard;
    }
    want_soft = soft == RLIM_INFINITY || soft >= bytes ? soft : bytes;
    want_hard = hard == RLIM_INFINITY || (want_soft != RLIM_INFINITY && hard >= want_soft) ? hard
                                                                                          : want_soft;
    if ((first || want_soft != soft || want_hard != hard) &&
        !memlock_write(key, pid, start, was_soft, was_hard, want_soft, want_hard)) {
        snprintf(why, wsize, "cannot record the locked memory limit: %s", strerror(errno));
        goto fail;
    }
    if (want_soft != soft || want_hard != hard) {
        if ((err = sys_setmemlock(pid, want_soft, want_hard)) < 0 ||
            (err = sys_getmemlock(pid, &s, &h)) < 0 || s != want_soft || h != want_hard) {
            sys_setmemlock(pid, soft, hard);
            /*
             * What is in place again, in the record of the helpers that
             * hold it: else the last one out finds the limit other than
             * "wrote", takes it for someone else's, and leaves it raised
             */
            if (!first) {
                memlock_write(key, pid, start, was_soft, was_hard, soft, hard);
            }
            snprintf(why, wsize, "cannot raise the locked memory limit: %s",
                     err < 0 ? strerror(-err) : "it did not take");
            goto fail;
        }
        sys_log("uid %u: locked memory limit of pid %d at %s (was %s): its QEMU passes PCI devices "
                "through", (unsigned)caller_uid, (int)pid, rlim_text(want_soft, a, sizeof(a)),
                rlim_text(soft, b, sizeof(b)));
    }
    if (first) {
        hold_share(memlock_hold);
    }
    snprintf(said, ssize, "locked memory %s", rlim_text(want_soft, a, sizeof(a)));
    if (want_soft != was_soft) {
        catf(said, ssize, " (was %s)", rlim_text(was_soft, b, sizeof(b)));
    }
    return true;
fail:
    if (first) {
        /* this helper's own and nobody else's: as it was */
        state_remove(key);
        hold_close(memlock_hold, true);
        memlock_hold = NULL;
    }
    return false;
}

/* The parent's limit let go: the last helper out puts it back */
static void memlock_drop(void)
{
    if (memlock_hold) {
        hold_drop(memlock_hold);
        memlock_hold = NULL;
    }
}

void memlock_restore(const char *key)
{
    rlim_t was_soft, was_hard, wrote_soft, wrote_hard, soft, hard;
    char a[32];
    pid_t pid;
    u64 start;

    if (memlock_read(key, &pid, &start, &was_soft, &was_hard, &wrote_soft, &wrote_hard) &&
        process_started(pid, start) && sys_getmemlock(pid, &soft, &hard) == 0) {
        if (soft == wrote_soft && hard == wrote_hard) {
            if ((soft != was_soft || hard != was_hard) &&
                sys_setmemlock(pid, was_soft, was_hard) == 0) {
                reply("restored memlock %d: %s", (int)pid, rlim_text(was_soft, a, sizeof(a)));
                sys_log("pid %d: locked memory limit back to %s", (int)pid,
                        rlim_text(was_soft, a, sizeof(a)));
            }
        } else if (soft != was_soft || hard != was_hard) {
            reply("left memlock %d: changed by someone else since", (int)pid);
        }
    }
    state_remove(key);
}

/* --- the requests --- */

/* @list: addresses separated by commas, each once, into @out */
static bool parse_list(const char *list, char (*out)[ADDR_LEN + 1], int *n)
{
    const char *p = list;

    *n = 0;
    for (;;) {
        const size_t len = strcspn(p, ",");
        char addr[ADDR_LEN + 1];
        bool dup = false;

        if (len != ADDR_LEN) {
            return false;
        }
        memcpy(addr, p, len);
        addr[len] = '\0';
        if (!addr_ok(addr)) {
            return false;
        }
        for (int i = 0; i < *n; i++) {
            dup |= strcmp(out[i], addr) == 0;
        }
        if (!dup) {
            if (*n == MAX_VFIO_DEVICES) {
                return false;
            }
            memcpy(out[(*n)++], addr, ADDR_LEN + 1);
        }
        if (!p[len]) {
            return true;
        }
        p += len + 1;
    }
}

/* This computer's memory in MiB (MemTotal), 0 when it cannot be read */
static u64 memory_mib(void)
{
    char *text = read_proc("/proc/meminfo");
    const char *line;
    u64 kb = 0;

    if (text && (line = strstr(text, "MemTotal:"))) {
        sscanf(line + 9, "%llu", &kb);
    }
    free(text);
    return kb >> 10;
}

static struct vgroup *find_group(unsigned id)
{
    for (int i = 0; i < ngroups; i++) {
        if (groups[i].id == id) {
            return &groups[i];
        }
    }
    return NULL;
}

/*
 * Whether IOMMU group @id can go to a VM that passes @named through, as
 * things are: the devices to put on vfio-pci, with what they have, into
 * @g; else why not in @why.  @bind: devices may be taken from host drivers.
 */
static bool check_group(struct vgroup *g, char (*named)[ADDR_LEN + 1], int nnamed, bool bind,
                        char *why, size_t size)
{
    char devs[MAX_GROUP_DEVICES][ADDR_LEN + 1];
    const int n = group_devices(g->id, devs, MAX_GROUP_DEVICES);

    if (n <= 0) {
        snprintf(why, size, "cannot read the devices of IOMMU group %u", g->id);
        return false;
    }
    for (int i = 0; i < n; i++) {
        const char *a = devs[i];
        char driver[64], override[64];
        const unsigned cls = dev_class(a);
        const char *platform;
        bool is_named = false;
        struct vdev *d;

        for (int j = 0; j < nnamed; j++) {
            is_named |= strcmp(named[j], a) == 0;
        }
        if (!dev_driver(a, driver, sizeof(driver)) || !dev_override(a, override, sizeof(override))) {
            snprintf(why, size, "cannot read the driver of %.*s", ADDR_LEN, a);
            return false;
        }
        if (!is_named) {
            /* the bridges stay as they are; vfio wants the others free */
            if ((cls >> 16) == 0x06 || !driver[0] || strcmp(driver, "vfio-pci") == 0 ||
                strcmp(driver, "pci-stub") == 0) {
                continue;
            }
            snprintf(why, size, "%.*s shares IOMMU group %u and is bound to %s: pass it through too",
                     ADDR_LEN, a, g->id, driver);
            return false;
        }
        if ((platform = platform_device(cls))) {
            snprintf(why, size, "%.*s is %s: it stays with this computer", ADDR_LEN, a, platform);
            return false;
        }
        if (strcmp(driver, "vfio-pci") == 0) {
            continue;
        }
        if (!bind) {
            if (driver[0]) {
                snprintf(why, size, "%.*s is bound to %s: taking it from this computer needs "
                                    "vitrine-helper devices-bind", ADDR_LEN, a, driver);
            } else {
                snprintf(why, size, "%.*s has no driver: binding it to vfio-pci needs "
                                    "vitrine-helper devices-bind", ADDR_LEN, a);
            }
            return false;
        }
        if (driver[0] && strcmp(driver, "pci-stub") != 0 && dev_in_use(a, driver, why, size)) {
            return false;
        }
        if (g->ndevs == MAX_VFIO_DEVICES) {
            snprintf(why, size, "IOMMU group %u has more than %d devices to bind", g->id,
                     MAX_VFIO_DEVICES);
            return false;
        }
        d = &g->devs[g->ndevs++];
        memcpy(d->addr, a, ADDR_LEN + 1);
        snprintf(d->driver, sizeof(d->driver), "%s", driver);
        snprintf(d->override, sizeof(d->override), "%s", override);
        if (!dev_id(a, d->id, sizeof(d->id))) {
            d->id[0] = '\0';
        }
    }
    /* another VM's QEMU, or a program, has it open */
    return !group_held(g->id, why, size);
}

/*
 * The group @id, whose hold @hold this helper has, given back - without the
 * lock: a driver may take its time to let go of a device or take it back,
 * and the group's hold keeps other helpers out meanwhile - then its hold
 * let go
 */
static void group_drop(struct hold *hold, unsigned id)
{
    char key[32];

    group_key(key, sizeof(key), id);
    vfio_restore(key);
    hold_let_go(hold);
}

/* The groups @fresh of this request back as they were, and their holds let
   go: a rollback, which says nothing of what it puts back */
static void undo(struct vgroup **fresh, int n)
{
    undoing = true;
    for (int i = n - 1; i >= 0; i--) {
        if (fresh[i]->hold) {
            group_drop(fresh[i]->hold, fresh[i]->id);
            fresh[i]->hold = NULL;
        }
    }
    undoing = false;
}

/* The first of @named in group @id, for the replies */
static const char *named_in(unsigned id, char (*named)[ADDR_LEN + 1], const unsigned *ids,
                            int nnamed)
{
    for (int i = 0; i < nnamed; i++) {
        if (ids[i] == id) {
            return named[i];
        }
    }
    return "";
}

void vfio_request(const char *list, const char *mib_text, bool bind)
{
    char named[MAX_VFIO_DEVICES][ADDR_LEN + 1];
    unsigned ids[MAX_VFIO_DEVICES];
    struct vgroup *fresh[MAX_VFIO_GROUPS];
    char why[400] = "", said[600] = "", lock[160] = "", key[32], err[200] = "";
    const u64 memory = memory_mib();
    int nnamed = 0, nfresh = 0;
    bool need_driver = false, capped = false, locked;
    u64 mib;

    if (!parse_list(list, named, &nnamed)) {
        reply("error vfio: give the PCI addresses as 0000:00:00.0, separated by commas");
        return;
    }
    if (!parse_uint(mib_text, MAX_LOCK_MIB, &mib) || mib == 0) {
        reply("error vfio: give the memory to lock in MiB");
        return;
    }
    /* no more than this computer's memory and 1 GiB: more cannot be pinned
       anyway (a virtual IOMMU counts the guest's memory once per device) */
    if (memory && mib > memory + 1024) {
        mib = memory + 1024;
        capped = true;
    }
    for (int i = 0; i < nnamed; i++) {
        if (!dev_exists(named[i])) {
            reply("error vfio %s: no such PCI device", named[i]);
            return;
        }
        if (!dev_group(named[i], &ids[i])) {
            reply("error vfio %s: it has no IOMMU group: the IOMMU is off", named[i]);
            return;
        }
    }
    /* the groups this helper has already: the same request again */
    for (int i = 0; i < nnamed; i++) {
        bool seen = find_group(ids[i]) != NULL;

        for (int j = 0; j < nfresh; j++) {
            seen |= fresh[j]->id == ids[i];
        }
        if (seen) {
            continue;
        }
        if (ngroups + nfresh == MAX_VFIO_GROUPS) {
            reply("error vfio: more than %d IOMMU groups", MAX_VFIO_GROUPS);
            return;
        }
        fresh[nfresh] = &groups[ngroups + nfresh];
        memset(fresh[nfresh], 0, sizeof(*fresh[nfresh]));
        fresh[nfresh]->id = ids[i];
        fresh[nfresh]->uid = (unsigned)caller_uid;
        nfresh++;
    }
    if (!nfresh) {
        reply("ok vfio %s: already", list);
        return;
    }

    /*
     * One helper at a time per group, under the lock: the first holder, its
     * hold shared at once - another helper's hold_take() then finds it held
     * without waiting for it.  The rest goes on without the lock: a driver
     * may take its time to let go of a device, and one that hangs must not
     * hold up every other helper.
     */
    lock_all();
    for (int i = 0; i < nfresh; i++) {
        bool first;

        group_key(key, sizeof(key), fresh[i]->id);
        if (!(fresh[i]->hold = hold_take(key, "on", &first))) {
            const int e = errno;

            unlock_all();
            undo(fresh, i);
            reply("error vfio: cannot take the hold: %s", strerror(e));
            return;
        }
        if (!first) {
            hold_close(fresh[i]->hold, false);
            fresh[i]->hold = NULL;
            unlock_all();
            undo(fresh, i);
            reply("error vfio %s: IOMMU group %u is in use by another VM",
                  named_in(fresh[i]->id, named, ids, nnamed), fresh[i]->id);
            return;
        }
        hold_share(fresh[i]->hold);
    }
    unlock_all();
    /* what a dead helper left under a group, given back first if it can be */
    for (int i = 0; i < nfresh; i++) {
        char *left;

        group_key(key, sizeof(key), fresh[i]->id);
        if ((left = state_read(key))) {
            free(left);
            vfio_restore(key);
        }
    }
    /* the checks, as things are now - then the records, before any change */
    for (int i = 0; i < nfresh; i++) {
        if (!check_group(fresh[i], named, nnamed, bind, why, sizeof(why))) {
            undo(fresh, nfresh);
            reply("error vfio %s: %s", named_in(fresh[i]->id, named, ids, nnamed), why);
            return;
        }
        need_driver |= fresh[i]->ndevs > 0;
    }
    for (int i = 0; i < nfresh; i++) {
        if (!record_write(fresh[i])) {
            const int e = errno;

            undo(fresh, nfresh);
            reply("error vfio: cannot record it: %s", strerror(e));
            return;
        }
    }
    /* not recorded: loading it may bind the devices its ids= option names,
       which stay on it, as they would have at the boot */
    if (need_driver && !driver_loaded("vfio-pci") &&
        (sys_load_module("vfio-pci", err, sizeof(err)) < 0 || !driver_loaded("vfio-pci"))) {
        undo(fresh, nfresh);
        reply("error vfio: cannot load vfio-pci: %s", err[0] ? err : "it did not come");
        return;
    }
    for (int i = 0; i < nfresh; i++) {
        for (int j = 0; j < fresh[i]->ndevs; j++) {
            if (bind_dev(&fresh[i]->devs[j], why, sizeof(why)) < 0) {
                undo(fresh, nfresh);
                reply("error vfio %s: %s", fresh[i]->devs[j].addr, why);
                return;
            }
        }
        if (!node_give(fresh[i], why, sizeof(why))) {
            undo(fresh, nfresh);
            reply("error vfio: %s", why);
            return;
        }
    }
    /* the limit its QEMU starts with: its record is shared, under the lock */
    lock_all();
    locked = memlock_raise((rlim_t)mib << 20, lock, sizeof(lock), why, sizeof(why));
    unlock_all();
    if (!locked) {
        undo(fresh, nfresh);
        reply("error vfio: %s", why);
        return;
    }
    if (capped) {
        catf(lock, sizeof(lock), ", capped at this computer's memory and 1 GiB");
    }
    for (int i = 0; i < nfresh; i++) {
        fresh[i]->expires = now_ms() + sys_vfio_expiry_ms();
    }
    ngroups += nfresh;

    /* "0000:6e:00.0=amdgpu,...": what each device had */
    for (int i = 0; i < nnamed; i++) {
        const char *had = "vfio-pci";

        for (int j = 0; j < nfresh; j++) {
            for (int k = 0; k < fresh[j]->ndevs; k++) {
                if (strcmp(fresh[j]->devs[k].addr, named[i]) == 0) {
                    had = fresh[j]->devs[k].driver[0] ? fresh[j]->devs[k].driver : "none";
                }
            }
        }
        catf(said, sizeof(said), "%s%s=%s", i ? "," : "", named[i], had);
    }
    catf(said, sizeof(said), ": IOMMU group%s", nfresh > 1 ? "s" : "");
    for (int i = 0; i < nfresh; i++) {
        catf(said, sizeof(said), "%s %u", i ? "," : "", fresh[i]->id);
    }
    catf(said, sizeof(said), " on vfio-pci, /dev/vfio/%u", fresh[0]->id);
    for (int i = 1; i < nfresh; i++) {
        catf(said, sizeof(said), ", /dev/vfio/%u", fresh[i]->id);
    }
    catf(said, sizeof(said), " for uid %u, %s", (unsigned)caller_uid, lock);
    for (int i = 0; i < nfresh; i++) {
        if (fresh[i]->ndevs || fresh[i]->node) {
            sys_log("uid %u: IOMMU group %u on vfio-pci for a VM (%d devices taken from their "
                    "drivers)", (unsigned)caller_uid, fresh[i]->id, fresh[i]->ndevs);
        }
    }
    reply("ok vfio %s", said);
}

/* No group waits for its QEMU within its first time any more: the parent's
   limit goes back (a QEMU of it that came has its own copy) */
static void memlock_check(void)
{
    for (int i = 0; i < ngroups; i++) {
        if (!groups[i].qemu && !groups[i].overdue) {
            return;
        }
    }
    memlock_drop();
}

void vfio_claim(const struct watched *w)
{
    if (!w || !ngroups) {
        return;
    }
    /* the groups are this helper's alone: no lock for their records */
    for (int i = 0; i < ngroups; i++) {
        if (!groups[i].qemu) {
            groups[i].qemu = w->pid;
            groups[i].qemu_start = w->start;
            record_write(&groups[i]);
        }
    }
    /* its QEMU has a copy of the limit of its own: the parent's goes back */
    memlock_check();
}

/* A watched QEMU of this pid */
static bool watching(pid_t pid)
{
    for (int i = 0; i < nwatched; i++) {
        if (watched[i].pid == pid) {
            return true;
        }
    }
    return false;
}

void vfio_check(void)
{
    for (int i = 0; i < ngroups;) {
        if (groups[i].qemu && !watching(groups[i].qemu)) {
            back_why = NULL;
            group_drop(groups[i].hold, groups[i].id);
            groups[i] = groups[--ngroups];
        } else {
            i++;
        }
    }
}

int vfio_timeout(void)
{
    const long long now = now_ms();
    long long soonest = -1;

    for (int i = 0; i < ngroups; i++) {
        if (!groups[i].qemu && (soonest < 0 || groups[i].expires < soonest)) {
            soonest = groups[i].expires;
        }
    }
    if (soonest < 0) {
        return -1;
    }
    return soonest <= now ? 0 : soonest - now > INT_MAX ? INT_MAX : (int)(soonest - now);
}

void vfio_expire(void)
{
    const long long now = now_ms();
    bool due = false;

    for (int i = 0; i < ngroups;) {
        struct vgroup *g = &groups[i];
        char why[300];

        if (g->qemu || g->expires > now) {
            i++;
            continue;
        }
        due = true;
        g->overdue = true;
        /* opened meanwhile - its QEMU started, its watch late: it waits on */
        if (group_held(g->id, why, sizeof(why))) {
            g->expires = now + sys_vfio_expiry_ms();
            i++;
            continue;
        }
        back_why = "no QEMU came for it";
        group_drop(g->hold, g->id);
        back_why = NULL;
        groups[i] = groups[--ngroups];
    }
    /* the first time is up: a QEMU that came has its own copy of the limit */
    if (due) {
        memlock_check();
    }
}

void vfio_release(void)
{
    while (ngroups > 0) {
        back_why = NULL;
        group_drop(groups[ngroups - 1].hold, groups[ngroups - 1].id);
        ngroups--;
    }
    memlock_drop();
}

void vfio_adopt(const char *key, struct hold *h)
{
    if (norphans == MAX_VFIO_GROUPS) {
        /* left for a later helper */
        hold_close(h, false);
        return;
    }
    snprintf(orphans[norphans].key, sizeof(orphans[0].key), "%s", key);
    orphans[norphans].hold = h;
    norphans++;
}

void vfio_recover(void)
{
    for (int i = 0; i < norphans; i++) {
        back_why = NULL;
        vfio_restore(orphans[i].key);
        hold_let_go(orphans[i].hold);
    }
    norphans = 0;
}
