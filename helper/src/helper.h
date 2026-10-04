/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vitrine-helper: the host settings that help VMs run smoothly, applied as
 * root while the caller's VMs run and reverted after the last one.  See
 * main.c for the protocol and docs/host-tuning.md for the reasoning.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* The protocol's version, in the "ready" line */
#define HELPER_PROTOCOL 1

/* What the kernel's fair server gets while VMs run: 1 ms every 10 ms */
#define FAIR_PERIOD_NS 10000000ULL
#define FAIR_RUNTIME_NS 1000000ULL
/* The GPU clock floor "auto" sets on AMD APUs whose minimum is lower */
#define GPU_FLOOR_AUTO_MHZ 1800
/*
 * The udmabuf limits while a VM with a native-context GPU runs: QEMU gives
 * each blob of guest memory a udmabuf, made of one entry per contiguous
 * piece of guest RAM - a maximized 4K window is ~32 MB in 1,200 to 8,000
 * pieces - and the kernel's defaults (1024 entries, 64 MB) refuse those.
 * Raised only, never lowered.
 */
#define UDMABUF_LIST_LIMIT 65536ULL
#define UDMABUF_SIZE_LIMIT_MB 2048ULL

#define FAIR_SERVER_DIR "/sys/kernel/debug/sched/fair_server"
#define LOCKDOWN_FILE "/sys/kernel/security/lockdown"
#define DRM_DIR "/sys/class/drm"
#define UDMABUF_DIR "/sys/module/udmabuf/parameters"
#define STATE_DIR "/run/vitrine-helper"

/* --- system access: sys.c on the real system, tests/fakesys.c on a fake tree --- */

/* Sets up the access; false (with a message on stderr) when it must not run */
bool sys_init(void);
/* "" on the real system, the fake tree's root in the test build */
const char *sys_root(void);
/* Reads the attribute file @path (absolute, under the root) into @buf, NUL
   terminated: its length, or -errno */
int sys_read(const char *path, char *buf, size_t size);
/* Writes @value to the attribute file @path in one write(2): 0 or -errno */
int sys_write(const char *path, const char *value);
/* A thread's scheduling policy and real-time priority: 0 or -errno */
int sys_getsched(pid_t tid, int *policy, int *priority);
int sys_setsched(pid_t tid, int policy, int priority);
/* cap_sys_nice=ep on the open file @fd (@path for messages): 0 or -errno */
int sys_set_file_cap(int fd, const char *path);
/* An entry in the system log (the journal), for the record of what root did */
void sys_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* The group @name in the user database: 1 with its gid in @gid, 0 if there
   is none */
int sys_group_find(const char *name, gid_t *gid);
/* @user, whose primary group is @primary, is a member of the group @gid */
bool sys_group_has(const char *user, gid_t primary, gid_t gid);
/* Creates the system group @name: 0, or -1 with why in @err */
int sys_group_create(const char *name, char *err, size_t size);
/* Adds @user to the members of the group @name: 0, or -1 with why in @err */
int sys_group_add_user(const char *name, const char *user, char *err, size_t size);

/* --- helpers shared by the files --- */

/* sys_root() + @path into @out: false if too long */
bool rooted(char *out, size_t size, const char *path);
/* A decimal number without sign, spaces or leading zeros, at most @max */
bool parse_uint(const char *s, unsigned long long max, unsigned long long *out);
/* A reply on stdout, one line */
void reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* --- settings.c: global settings held while VMs run --- */

/* The amdgpu overdrive table, from pp_od_clk_voltage */
struct od_table {
    unsigned min, max;          /* OD_SCLK 0: and 1:, MHz */
    unsigned range_lo, range_hi; /* OD_RANGE SCLK: */
};
/* false when @text has no complete OD_SCLK and OD_RANGE SCLK */
bool od_parse(const char *text, struct od_table *od);

/* Opens /run/vitrine-helper and restores what a crashed helper left; false
   when the state folder is unusable */
bool settings_init(void);
void fair_server_on(void);
void fair_server_off(void);
/* @card: "cardN"; @value: a clock in MHz, "auto" or "off" */
void gpu_floor(const char *card, const char *value);
/* The protocol's udmabuf <pid>: the udmabuf limits raised while that
   watched QEMU runs */
void udmabuf_on(const char *arg);
/* After a watched QEMU exits: the limits let go when none of those still
   watched asked for them */
void udmabuf_check(void);
/* Drops every setting this helper holds, restoring those it was the last
   one to hold */
void settings_release(void);

/* --- process.c: QEMU processes, real-time threads, file capability --- */

/* The caller: PKEXEC_UID, else SUDO_UID, else the real uid of a non-root run */
extern uid_t caller_uid;

struct watched {
    pid_t pid;
    int pidfd;   /* readable once the process has exited */
    int procfd;  /* /proc/<pid>, bound to that process */
    bool rt;     /* rt was asked for it */
    bool udmabuf; /* udmabuf was asked for it */
};
#define MAX_WATCHED 64
extern struct watched watched[MAX_WATCHED];
extern int nwatched;

/* The protocol's watch <pid>: true when watched */
bool watch(const char *arg);
/* The watched process with this pid, or NULL */
struct watched *find_watched(const char *arg);
/* The protocol's rt <pid> */
void rt_on(const char *arg);
/* Back to SCHED_OTHER for the threads rt made real-time, of the processes
   still running */
void rt_off_all(void);
/* Forgets watched[i], which has exited */
void unwatch(int i);
/* vitrine-helper setcap PATH: the exit status */
int setcap(const char *path);

/* --- group.c: the vitrine group --- */

/* The group polkit's rule (49-vitrine.rules) lets use the helper without a
   password */
#define VITRINE_GROUP "vitrine"
/* vitrine-helper setup-group: the exit status */
int setup_group(void);
