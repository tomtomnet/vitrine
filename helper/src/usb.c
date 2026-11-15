/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * USB devices that reconnect to a VM (vitrine-helper devices and
 * devices-bind).  A VM keeps a USB device by its IDs (-device usb-host,
 * vendorid=,productid=): QEMU takes it again by itself at each reconnect,
 * on any port, even with vitrine closed.  What it cannot do without root
 * is two udev rules:
 *
 * - usb-aside: the device's node open to the user at the desktop at each
 *   plug, for good - systemd's uaccess tag, which 73-seat-late turns into
 *   that user's ACL - a line per device in USB_ASIDE_DIR/USB_ASIDE_FILE.
 *   Added by devices-bind only (an administrator's password), taken out by
 *   either verb: giving a device back needs no password.
 * - usb: while the VM runs, the host's desktop kept away from those devices
 *   - no automount (udisks), no sound card (PipeWire, PulseAudio), no
 *   input device (libinput), no modem (ModemManager), no network
 *   (NetworkManager: a phone's tethering), no phone or camera (libmtp,
 *   libgphoto2, gvfs) - through a runtime rules file of this
 *   helper's, USB_RUN_DIR/60-vitrine-usb-<pid>.rules, that matches the
 *   device and every device under it (its disks, sound cards, input
 *   devices, ttys).  Only for devices set aside.  Like vfio's groups, it is
 *   made before QEMU, waits for the next watch, VFIO_EXPIRY_MS at most, and
 *   goes with the watched QEMU, crash included - or with the helper, if no
 *   QEMU took it.
 *
 * After each change udev reads its rules again (udevadm control --reload)
 * and each matching device present gets a change event, so that the rules
 * apply now and not at its next plug: for usb, the devices under it too,
 * so that the desktop lets go of them, and sees them again once the file
 * is gone.
 *
 * The files are written whole or not at all (a file of their own beside
 * them, synced, renamed over them), root's, 0644, in folders root's alone,
 * never through a link: a link where the aside file goes (an
 * administrator's mask, /dev/null) is left as it is, and so is a file with
 * lines vitrine-helper did not write.  The aside file changes under the
 * helpers' lock (lock_all()), so that two helpers' changes do not undo each
 * other; udev is told outside of it.
 *
 * The runtime file has a record, usb-<pid> (settings.c's holds), written
 * before the file with every ID the file may hold: a helper killed while
 * it has the file leaves the record, and the next helper, whatever its
 * verb, removes the file and tells udev.  The aside file needs no record:
 * it is meant to stay.
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

/* vvvv:pppp */
#define ID_LEN 9
/* The IDs the aside file holds at most */
#define MAX_ASIDE_IDS 256
/* The change events of one request at most, how deep the folders under a
   device go, and the entries of one folder looked at */
#define MAX_EVENTS 512
#define WALK_DEPTH 12
#define MAX_ENTRIES 512

struct ids {
    char id[MAX_ASIDE_IDS][ID_LEN + 1];
    int n;
};

/* The change events of a request: how many, and what went wrong */
struct events {
    int sent;
    int failed;
    bool full;              /* more than MAX_EVENTS */
    char first[200];        /* the first failure */
};

/* This helper's runtime file */
static struct {
    bool on;
    struct hold *hold;
    struct ids ids;         /* in the file, sorted */
    pid_t qemu;             /* the watched QEMU that took it; 0 while it waits */
    u64 qemu_start;
    long long expires;      /* while it waits: CLOCK_MONOTONIC ms */
} run;

/* dead helpers' records the recovery took over, given back after it */
static struct {
    char key[32];
    struct hold *hold;
} orphans[MAX_USB_ORPHANS];
static int norphans;

static const char aside_header[] =
    "# Written by vitrine-helper (usb-aside): USB devices set aside for virtual machines, their\n"
    "# nodes open to the user at the desktop at each plug.  vitrine's USB settings change it.\n";
static const char run_header[] =
    "# Written by vitrine-helper (usb) while a virtual machine has these USB devices: the desktop\n"
    "# keeps away from them.  It goes with the virtual machine.\n";

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

/* --- IDs --- */

static bool hex4(const char *s)
{
    for (int i = 0; i < 4; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

/* vvvv:pppp, @len long, as sysfs shows them: lowercase hex */
static bool id_ok(const char *s, size_t len)
{
    return len == ID_LEN && hex4(s) && s[4] == ':' && hex4(s + 5);
}

static bool ids_has(const struct ids *set, const char *id)
{
    for (int i = 0; i < set->n; i++) {
        if (strcmp(set->id[i], id) == 0) {
            return true;
        }
    }
    return false;
}

static void ids_add(struct ids *set, const char *id)
{
    if (!ids_has(set, id) && set->n < MAX_ASIDE_IDS) {
        memcpy(set->id[set->n++], id, ID_LEN + 1);
    }
}

static int cmp_id(const void *a, const void *b)
{
    return strcmp(a, b);
}

static void ids_sort(struct ids *set)
{
    qsort(set->id, (size_t)set->n, ID_LEN + 1, cmp_id);
}

/* The same IDs (both sorted) */
static bool ids_equal(const struct ids *a, const struct ids *b)
{
    if (a->n != b->n) {
        return false;
    }
    for (int i = 0; i < a->n; i++) {
        if (strcmp(a->id[i], b->id[i]) != 0) {
            return false;
        }
    }
    return true;
}

/* "vvvv:pppp,..." into @out */
static void ids_text(const struct ids *set, char *out, size_t size)
{
    out[0] = '\0';
    for (int i = 0; i < set->n; i++) {
        catf(out, size, "%s%s", i ? "," : "", set->id[i]);
    }
}

/* The protocol's IDS: vvvv:pppp separated by commas, MAX_USB_IDS at most,
   none twice */
static bool ids_parse(const char *list, struct ids *out)
{
    const char *p = list;

    out->n = 0;
    for (;;) {
        const size_t len = strcspn(p, ",");
        char id[ID_LEN + 1];

        if (!id_ok(p, len) || out->n == MAX_USB_IDS) {
            return false;
        }
        memcpy(id, p, ID_LEN);
        id[ID_LEN] = '\0';
        if (ids_has(out, id)) {
            return false;
        }
        memcpy(out->id[out->n++], id, ID_LEN + 1);
        if (!p[len]) {
            return true;
        }
        p += len + 1;
    }
}

/*
 * A root hub (the Linux Foundation's 1d6b:0001 to 0003): the bus itself,
 * whose node gives control of every port, and under which every device of
 * the bus is - the host's keyboard and mouse would go with it
 */
static const char *root_hub(const struct ids *set)
{
    for (int i = 0; i < set->n; i++) {
        if (strncmp(set->id[i], "1d6b:000", 8) == 0 && set->id[i][8] >= '1' &&
            set->id[i][8] <= '3') {
            return set->id[i];
        }
    }
    return NULL;
}

/* --- the rules files --- */

/* The line that sets @id aside, without its newline */
static void aside_line(char *out, size_t size, const char *id)
{
    snprintf(out, size,
             "SUBSYSTEM==\"usb\", ENV{DEVTYPE}==\"usb_device\", ATTR{idVendor}==\"%.4s\", "
             "ATTR{idProduct}==\"%.4s\", TAG+=\"uaccess\"",
             id, id + 5);
}

/* The line that keeps the desktop away from @id and what is under it */
static void away_line(char *out, size_t size, const char *id)
{
    snprintf(out, size,
             "SUBSYSTEMS==\"usb\", ATTRS{idVendor}==\"%.4s\", ATTRS{idProduct}==\"%.4s\", "
             "ENV{UDISKS_IGNORE}=\"1\", ENV{ACP_IGNORE}=\"1\", ENV{PULSE_IGNORE}=\"1\", "
             "ENV{LIBINPUT_IGNORE_DEVICE}=\"1\", ENV{ID_MM_DEVICE_IGNORE}=\"1\", "
             "ENV{ID_MTP_DEVICE}=\"\", ENV{ID_GPHOTO2}=\"\", ENV{MTP_NO_PROBE}=\"1\", ENV{NM_UNMANAGED}=\"1\"",
             id, id + 5);
}

/* The ID of @line when it is a line aside_line() writes, else false */
static bool aside_parse(const char *line, char *id)
{
    static const char vendor[] = "ATTR{idVendor}==\"", product[] = "ATTR{idProduct}==\"";
    const char *v = strstr(line, vendor), *p = strstr(line, product);
    char want[256];

    if (!v || !p || strlen(v) < sizeof(vendor) + 3 || strlen(p) < sizeof(product) + 3) {
        return false;
    }
    snprintf(id, ID_LEN + 1, "%.4s:%.4s", v + sizeof(vendor) - 1, p + sizeof(product) - 1);
    if (!id_ok(id, strlen(id))) {
        return false;
    }
    aside_line(want, sizeof(want), id);
    return strcmp(line, want) == 0;
}

/*
 * The folder @dir (under the root), opened without following a link: root's
 * alone (nobody else may write in it).  When @make, made (0755) if missing,
 * its parent there.  The folder, or -errno with why in @why (-ENOENT: there
 * is none).
 */
static int rules_dir(const char *dir, bool make, char *why, size_t size)
{
    char full[PATH_MAX];
    bool made = false;
    struct stat st;
    int fd, err;

    if (!rooted(full, sizeof(full), dir)) {
        snprintf(why, size, "%s: %s", dir, strerror(ENAMETOOLONG));
        return -ENAMETOOLONG;
    }
    if (make && mkdir(full, 0755) == 0) {
        made = true;
    }
    fd = open(full, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        err = errno;
        snprintf(why, size, "cannot open %s: %s", dir, strerror(err));
        return -err;
    }
    /* the umask took group and others' bits */
    if (made) {
        fchmod(fd, 0755);
    }
    if (fstat(fd, &st) < 0 || st.st_uid != geteuid() || (st.st_mode & 022)) {
        snprintf(why, size, "%s is not root's alone: left as it is", dir);
        close(fd);
        return -EPERM;
    }
    return fd;
}

/*
 * The IDs the aside file of the folder @dirfd sets aside: the lines
 * aside_line() writes, in @set; *@foreign when it has other lines
 * (comments and empty lines aside).  0 (no file: nothing set aside), or
 * -errno: -ELOOP for a link, -EINVAL for something else than a file.
 */
static int aside_read(int dirfd, struct ids *set, bool *foreign)
{
    const int fd = openat(dirfd, USB_ASIDE_FILE, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    char *text, *line, *save = NULL;
    struct stat st;
    ssize_t n;

    set->n = 0;
    *foreign = false;
    if (fd < 0) {
        return errno == ENOENT ? 0 : -errno;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size > (1 << 20) ||
        !(text = malloc((size_t)st.st_size + 1))) {
        close(fd);
        return -EINVAL;
    }
    n = read(fd, text, (size_t)st.st_size);
    close(fd);
    if (n < 0) {
        free(text);
        return -EIO;
    }
    text[n] = '\0';
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char id[ID_LEN + 1];

        if (line[0] == '#' || line[strspn(line, " \t")] == '\0') {
            continue;
        }
        if (aside_parse(line, id) && set->n < MAX_ASIDE_IDS) {
            ids_add(set, id);
        } else {
            *foreign = true;
        }
    }
    free(text);
    return 0;
}

/*
 * @text as @name in the folder @dirfd, whole or not at all: written to a
 * file of its own beside it (@name.new, made anew: a leftover goes), then
 * root's, 0644, synced and renamed over @name (sys_rules_put).  0 or -errno.
 */
static int rules_write(int dirfd, const char *name, const char *text)
{
    const size_t len = strlen(text);
    char tmp[96];
    size_t done = 0;
    struct stat st;
    int fd, err = 0;

    snprintf(tmp, sizeof(tmp), "%s.new", name);
    unlinkat(dirfd, tmp, 0);
    fd = openat(dirfd, tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -errno;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || fchmod(fd, 0644) < 0) {
        err = -EPERM;
    }
    while (!err && done < len) {
        const ssize_t n = write(fd, text + done, len - done);

        if (n < 0 && errno != EINTR) {
            err = -errno;
        } else if (n > 0) {
            done += (size_t)n;
        }
    }
    if (!err) {
        err = sys_rules_put(dirfd, fd, tmp, name);
    }
    close(fd);
    if (err) {
        unlinkat(dirfd, tmp, 0);
    }
    return err;
}

/* @name of the folder @dirfd removed, and what a write of it left: 0 or
   -errno (-ENOENT: there was none) */
static int rules_remove(int dirfd, const char *name)
{
    char tmp[96];

    snprintf(tmp, sizeof(tmp), "%s.new", name);
    unlinkat(dirfd, tmp, 0);
    return sys_rules_remove(dirfd, name);
}

/* udev reads its rules again: a warning in @warn when it did not */
static void reload(char *warn, size_t size)
{
    char err[200];

    if (sys_udev_reload(err, sizeof(err)) < 0) {
        catf(warn, size, " warning: udev did not read its rules again (%s)", err);
    }
}

/* --- change events --- */

/* "change" to @dev's uevent (@dev: a device's folder, under the root's
   USB_DEVICES_DIR) */
static void event(const char *dev, struct events *ev)
{
    char path[PATH_MAX], full[PATH_MAX];
    int err;

    if (ev->sent + ev->failed == MAX_EVENTS) {
        ev->full = true;
        return;
    }
    if (snprintf(path, sizeof(path), "%s/uevent", dev) >= (int)sizeof(path) ||
        !rooted(full, sizeof(full), path)) {
        return;
    }
    if ((err = sys_write(full, "change")) < 0) {
        if (!ev->failed) {
            snprintf(ev->first, sizeof(ev->first), "%s: %s", dev + strlen(USB_DEVICES_DIR) + 1,
                     strerror(-err));
        }
        ev->failed++;
    } else {
        ev->sent++;
    }
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/*
 * The device folder @fd (taken) at @path, and the device folders under it
 * @depth levels down, a change event each, a folder before those under it,
 * in name order: real folders only, never sysfs's links to others
 * (subsystem, driver, port...), which the walk does not follow
 */
static void walk(int fd, const char *path, int depth, struct events *ev)
{
    DIR *d = fdopendir(fd);
    char **names = NULL;
    struct dirent *e;
    int n = 0;

    if (!d) {
        close(fd);
        return;
    }
    /* a device's folder has its uevent; power/, queue/ and the like have none */
    if (faccessat(dirfd(d), "uevent", F_OK, AT_SYMLINK_NOFOLLOW) == 0) {
        event(path, ev);
    }
    if (depth > 0 && (names = calloc(MAX_ENTRIES, sizeof(*names)))) {
        while ((e = readdir(d)) && n < MAX_ENTRIES) {
            struct stat st;

            if (e->d_name[0] == '.') {
                continue;
            }
            if (e->d_type == DT_UNKNOWN) {
                if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
                    !S_ISDIR(st.st_mode)) {
                    continue;
                }
            } else if (e->d_type != DT_DIR) {
                continue;
            }
            if ((names[n] = strdup(e->d_name))) {
                n++;
            }
        }
        qsort(names, (size_t)n, sizeof(*names), cmp_name);
        for (int i = 0; i < n; i++) {
            char sub[PATH_MAX];
            const int sfd = openat(dirfd(d), names[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

            if (sfd >= 0 && snprintf(sub, sizeof(sub), "%s/%s", path, names[i]) < (int)sizeof(sub)) {
                walk(sfd, sub, depth - 1, ev);
            } else if (sfd >= 0) {
                close(sfd);
            }
            free(names[i]);
        }
        free(names);
    }
    closedir(d);
}

/* A folder of USB_DEVICES_DIR that is a USB device: 2-1, 3-1.4, usb1 (an
   interface, 2-1:1.0, has a colon) */
static bool device_name_ok(const char *s)
{
    const size_t len = strlen(s);

    if (len == 0 || len > 31 || s[0] == '.') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'z') || s[i] == '.' ||
              s[i] == '-')) {
            return false;
        }
    }
    return true;
}

/*
 * A change event for each USB device present with one of the IDs of @set,
 * in name order, and with @under for the devices under each one too
 */
static void events_for(const struct ids *set, bool under, struct events *ev)
{
    char full[PATH_MAX], **names;
    struct dirent *e;
    int n = 0;
    DIR *d;

    if (!set->n || !rooted(full, sizeof(full), USB_DEVICES_DIR) || !(d = opendir(full))) {
        return;
    }
    if (!(names = calloc(MAX_ENTRIES, sizeof(*names)))) {
        closedir(d);
        return;
    }
    while ((e = readdir(d)) && n < MAX_ENTRIES) {
        char path[PATH_MAX], vendor[16], product[16], id[ID_LEN + 1];

        if (!device_name_ok(e->d_name) ||
            snprintf(path, sizeof(path), "%s/%s/idVendor", full, e->d_name) >= (int)sizeof(path) ||
            sys_read(path, vendor, sizeof(vendor)) <= 0 ||
            snprintf(path, sizeof(path), "%s/%s/idProduct", full, e->d_name) >= (int)sizeof(path) ||
            sys_read(path, product, sizeof(product)) <= 0) {
            continue;
        }
        vendor[strcspn(vendor, "\n")] = '\0';
        product[strcspn(product, "\n")] = '\0';
        if (strlen(vendor) != 4 || strlen(product) != 4) {
            continue;
        }
        snprintf(id, sizeof(id), "%s:%s", vendor, product);
        if (ids_has(set, id) && (names[n] = strdup(e->d_name))) {
            n++;
        }
    }
    qsort(names, (size_t)n, sizeof(*names), cmp_name);
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        int fd;

        snprintf(path, sizeof(path), USB_DEVICES_DIR "/%s", names[i]);
        /* the bus's link to the device's folder, followed: nothing under it */
        if ((fd = openat(dirfd(d), names[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC)) >= 0) {
            walk(fd, path, under ? WALK_DEPTH : 0, ev);
        }
        free(names[i]);
    }
    free(names);
    closedir(d);
}

/* What went wrong with @ev, as a warning in @warn */
static void events_said(const struct events *ev, char *warn, size_t size)
{
    if (ev->failed) {
        catf(warn, size, " warning: %d change event%s not sent (%s)", ev->failed,
             ev->failed > 1 ? "s" : "", ev->first);
    }
    if (ev->full) {
        catf(warn, size, " warning: more devices than vitrine-helper sends change events to");
    }
}

/* --- set aside: usb-aside --- */

void usb_aside(const char *action, const char *list, bool bind)
{
    const bool add = strcmp(action, "add") == 0;
    struct ids want, set, next;
    struct events ev = {0};
    char why[300] = "", warn[400] = "", text[MAX_ASIDE_IDS * 160 + 256], line[256];
    const char *hub;
    bool foreign = false, changed;
    int dirfd, err;

    if (!add && strcmp(action, "remove") != 0) {
        reply("error usb-aside: add or remove");
        return;
    }
    if (!ids_parse(list, &want)) {
        reply("error usb-aside: give the USB devices as vvvv:pppp (lowercase hex), at most %d, "
              "separated by commas, none twice", MAX_USB_IDS);
        return;
    }
    if (add && !bind) {
        reply("error usb-aside: setting USB devices aside needs vitrine-helper devices-bind");
        return;
    }
    if (add && (hub = root_hub(&want))) {
        reply("error usb-aside %s: a USB root hub: it stays with this computer", hub);
        return;
    }

    /* read, changed and written under the lock: another helper's change
       made meanwhile stays */
    lock_all();
    if ((dirfd = rules_dir(USB_ASIDE_DIR, add, why, sizeof(why))) < 0) {
        unlock_all();
        /* no folder: nothing set aside, nothing to take out */
        if (!add && dirfd == -ENOENT) {
            reply("ok usb-aside %s", list);
        } else {
            reply("error usb-aside: %s", why);
        }
        return;
    }
    if ((err = aside_read(dirfd, &set, &foreign)) < 0 || foreign) {
        unlock_all();
        close(dirfd);
        reply("error usb-aside: " USB_ASIDE_DIR "/" USB_ASIDE_FILE " %s: left as it is",
              err == -ELOOP   ? "is a link (a mask?)"
              : err < 0       ? "is not a file of vitrine-helper's"
                              : "has lines vitrine-helper did not write");
        return;
    }
    next = set;
    if (add) {
        for (int i = 0; i < want.n; i++) {
            if (!ids_has(&next, want.id[i]) && next.n == MAX_ASIDE_IDS) {
                unlock_all();
                close(dirfd);
                reply("error usb-aside: more than %d USB devices set aside", MAX_ASIDE_IDS);
                return;
            }
            ids_add(&next, want.id[i]);
        }
    } else {
        next.n = 0;
        for (int i = 0; i < set.n; i++) {
            if (!ids_has(&want, set.id[i])) {
                ids_add(&next, set.id[i]);
            }
        }
    }
    ids_sort(&set);
    ids_sort(&next);
    changed = !ids_equal(&set, &next);
    err = 0;
    if (changed && !next.n) {
        err = rules_remove(dirfd, USB_ASIDE_FILE);
    } else if (changed) {
        snprintf(text, sizeof(text), "%s", aside_header);
        for (int i = 0; i < next.n; i++) {
            aside_line(line, sizeof(line), next.id[i]);
            catf(text, sizeof(text), "%s\n", line);
        }
        err = rules_write(dirfd, USB_ASIDE_FILE, text);
    }
    unlock_all();
    close(dirfd);
    if (err < 0 && err != -ENOENT) {
        reply("error usb-aside: cannot write " USB_ASIDE_DIR "/" USB_ASIDE_FILE ": %s",
              strerror(-err));
        return;
    }
    if (changed) {
        reload(warn, sizeof(warn));
        sys_log("uid %u: USB devices %s %s for virtual machines", (unsigned)caller_uid, list,
                add ? "set aside" : "no longer set aside");
    }
    /* their nodes the user's now (or not from their next plug on) */
    events_for(&want, false, &ev);
    events_said(&ev, warn, sizeof(warn));
    reply("ok usb-aside %s%s", list, warn);
}

/* --- kept away from the desktop while the VM runs: usb --- */

static void run_key(char *out, size_t size, pid_t pid)
{
    snprintf(out, size, "usb-%d", (int)pid);
}

static void run_name(char *out, size_t size, pid_t pid)
{
    snprintf(out, size, "60-vitrine-usb-%d.rules", (int)pid);
}

/* "ids vvvv:pppp,...", then "qemu PID START" once a QEMU took it */
static bool record_write(const struct ids *set)
{
    char key[32], text[MAX_ASIDE_IDS * 12 + 64] = "ids ";

    run_key(key, sizeof(key), getpid());
    ids_text(set, text + 4, sizeof(text) - 4);
    catf(text, sizeof(text), "\n");
    if (run.qemu) {
        catf(text, sizeof(text), "qemu %d %llu\n", (int)run.qemu, run.qemu_start);
    }
    return state_write(key, text);
}

/* The IDs of the record @key, into @set */
static void record_ids(const char *key, struct ids *set)
{
    char *text = state_read(key);
    const char *line;

    set->n = 0;
    if (text && strncmp(text, "ids ", 4) == 0) {
        line = text + 4;
        for (;;) {
            const size_t len = strcspn(line, ",\n");
            char id[ID_LEN + 1];

            if (id_ok(line, len)) {
                memcpy(id, line, ID_LEN);
                id[ID_LEN] = '\0';
                ids_add(set, id);
            }
            if (line[len] != ',') {
                break;
            }
            line += len + 1;
        }
    }
    free(text);
}

/*
 * The runtime file of the helper of the record @key (usb-<pid>: this one,
 * or one that died) removed, udev told, a change event for each device of
 * the record and those under it - the desktop sees them again - and the
 * record removed.  @why: why now, for the reply.
 */
static void run_restore(const char *key, const char *why)
{
    char name[64], err[200] = "", warn[400] = "", list[MAX_ASIDE_IDS * 10 + 1];
    struct events ev = {0};
    struct ids set;
    u64 pid;
    int dirfd, r = -ENOENT;

    if (strncmp(key, "usb-", 4) != 0 || !parse_uint(key + 4, 4194304, &pid) || pid < 2) {
        /* not a record of ours: nothing to give back */
        state_remove(key);
        return;
    }
    record_ids(key, &set);
    run_name(name, sizeof(name), (pid_t)pid);
    if ((dirfd = rules_dir(USB_RUN_DIR, false, err, sizeof(err))) >= 0) {
        r = rules_remove(dirfd, name);
        close(dirfd);
    } else {
        /* no folder: no file; one others may write in: left as it is */
        r = dirfd;
    }
    if (r < 0 && r != -ENOENT) {
        /* the record stays, for a later helper to try again */
        reply("left usb: cannot remove " USB_RUN_DIR "/%s: %s", name, strerror(-r));
        sys_log("cannot remove " USB_RUN_DIR "/%s: %s", name, strerror(-r));
        return;
    }
    reload(warn, sizeof(warn));
    events_for(&set, true, &ev);
    events_said(&ev, warn, sizeof(warn));
    state_remove(key);
    ids_sort(&set);
    ids_text(&set, list, sizeof(list));
    reply("restored usb %s%s%s%s%s", set.n ? list : "-", why ? " (" : "", why ? why : "",
          why ? ")" : "", warn);
    sys_log("USB devices %s: the desktop sees them again", set.n ? list : "-");
}

/* This helper's runtime file goes, and its hold */
static void run_drop(const char *why)
{
    char key[32];

    if (!run.on) {
        return;
    }
    run_key(key, sizeof(key), getpid());
    run_restore(key, why);
    hold_let_go(run.hold);
    memset(&run, 0, sizeof(run));
}

void usb_request(const char *list)
{
    struct ids want, aside, all;
    char why[300] = "", warn[400] = "", key[32], name[64], line[512];
    char text[MAX_USB_IDS * 512 + 256];
    struct events ev = {0};
    const char *hub;
    bool foreign = false, fresh = false;
    int dirfd, err;

    if (!ids_parse(list, &want)) {
        reply("error usb: give the USB devices as vvvv:pppp (lowercase hex), at most %d, "
              "separated by commas, none twice", MAX_USB_IDS);
        return;
    }
    if ((hub = root_hub(&want))) {
        reply("error usb %s: a USB root hub: it stays with this computer", hub);
        return;
    }
    /* devices set aside only: the user's at each plug, by an administrator's
       choice - the rules file no one else writes */
    err = -ENOENT;
    if ((dirfd = rules_dir(USB_ASIDE_DIR, false, why, sizeof(why))) >= 0) {
        err = aside_read(dirfd, &aside, &foreign);
        close(dirfd);
    }
    for (int i = 0; i < want.n; i++) {
        if (err < 0 || !ids_has(&aside, want.id[i])) {
            reply("error usb %s: not set aside: set it aside first (usb-aside add, vitrine-helper "
                  "devices-bind)", want.id[i]);
            return;
        }
    }
    ids_sort(&want);
    if (run.on && ids_equal(&run.ids, &want)) {
        reply("ok usb %s", list);
        return;
    }

    /* the record first, with every ID the file may have meanwhile */
    run_key(key, sizeof(key), getpid());
    if (!run.on) {
        bool first = false;

        lock_all();
        run.hold = hold_take(key, "on", &first);
        if (run.hold && !first) {
            hold_close(run.hold, false);
            run.hold = NULL;
            errno = EBUSY;
        } else if (run.hold) {
            hold_share(run.hold);
        }
        unlock_all();
        if (!run.hold) {
            reply("error usb: cannot take the hold: %s", strerror(errno));
            return;
        }
        fresh = true;
        run.ids.n = 0;
    }
    all = run.ids;
    for (int i = 0; i < want.n; i++) {
        ids_add(&all, want.id[i]);
    }
    if (!record_write(&all)) {
        reply("error usb: cannot record it: %s", strerror(errno));
        goto undo;
    }
    snprintf(text, sizeof(text), "%s", run_header);
    for (int i = 0; i < want.n; i++) {
        away_line(line, sizeof(line), want.id[i]);
        catf(text, sizeof(text), "%s\n", line);
    }
    run_name(name, sizeof(name), getpid());
    if ((dirfd = rules_dir(USB_RUN_DIR, true, why, sizeof(why))) < 0) {
        reply("error usb: %s", why);
        goto undo;
    }
    err = rules_write(dirfd, name, text);
    close(dirfd);
    if (err < 0) {
        reply("error usb: cannot write " USB_RUN_DIR "/%s: %s", name, strerror(-err));
        goto undo;
    }
    reload(warn, sizeof(warn));
    /* the desktop lets go of them (and of the IDs a usb before had) */
    events_for(&all, true, &ev);
    events_said(&ev, warn, sizeof(warn));
    run.ids = want;
    run.on = true;
    record_write(&run.ids);
    if (!run.qemu) {
        run.expires = now_ms() + sys_vfio_expiry_ms();
    }
    sys_log("uid %u: the desktop keeps away from USB devices %s while a VM has them",
            (unsigned)caller_uid, list);
    reply("ok usb %s%s", list, warn);
    return;
undo:
    if (fresh) {
        /* nothing written but the record */
        state_remove(key);
        hold_let_go(run.hold);
        memset(&run, 0, sizeof(run));
    } else {
        record_write(&run.ids);
    }
}

void usb_claim(const struct watched *w)
{
    if (w && run.on && !run.qemu) {
        run.qemu = w->pid;
        run.qemu_start = w->start;
        record_write(&run.ids);
    }
}

void usb_check(void)
{
    if (!run.on || !run.qemu) {
        return;
    }
    for (int i = 0; i < nwatched; i++) {
        if (watched[i].pid == run.qemu) {
            return;
        }
    }
    run_drop(NULL);
}

int usb_timeout(void)
{
    const long long now = now_ms();

    if (!run.on || run.qemu) {
        return -1;
    }
    return run.expires <= now ? 0 : run.expires - now > INT_MAX ? INT_MAX : (int)(run.expires - now);
}

void usb_expire(void)
{
    if (run.on && !run.qemu && run.expires <= now_ms()) {
        run_drop("no QEMU came for it");
    }
}

void usb_release(void)
{
    run_drop(NULL);
}

void usb_adopt(const char *key, struct hold *h)
{
    if (norphans == MAX_USB_ORPHANS) {
        /* left for a later helper */
        hold_close(h, false);
        return;
    }
    snprintf(orphans[norphans].key, sizeof(orphans[0].key), "%s", key);
    orphans[norphans].hold = h;
    norphans++;
}

void usb_recover(void)
{
    for (int i = 0; i < norphans; i++) {
        run_restore(orphans[i].key, NULL);
        hold_let_go(orphans[i].hold);
    }
    norphans = 0;
}
