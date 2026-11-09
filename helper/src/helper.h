/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vitrine-helper: the host settings that help VMs run smoothly, applied as
 * root while the caller's VMs run and reverted after the last one.  See
 * main.c for the protocol and docs/host-tuning.md for the reasoning.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/resource.h>
#include <sys/types.h>

/* The protocol's version, in the "ready" line */
#define HELPER_PROTOCOL 1

/*
 * What the kernel's fair server gets while VMs run: 1 ms every 10 ms.  It is
 * the only bound on how long real-time threads keep ordinary ones (kernel
 * workers among them) off a CPU - 950 ms at the kernel's 50 ms every 1 s -
 * so threads are made real-time only once it is in place.  Under a sched_ext
 * scheduler, ordinary tasks are its own, served by the ext server, which
 * gets the same.
 */
#define FAIR_PERIOD_NS 10000000ULL
#define FAIR_RUNTIME_NS 1000000ULL
/* The kernel's own, for a server found at the target with no record */
#define FAIR_DEFAULT_PERIOD_NS 1000000000ULL
#define FAIR_DEFAULT_RUNTIME_NS 50000000ULL
/* The nice value of the vCPU threads of a VM behind the one in front */
#define BEHIND_NICE (-5)
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
/*
 * Transparent huge pages for shared memory while VMs run: "advise", from
 * "never" only (Fedora's default; any other choice is the host owner's).
 * vitrine's guest RAM is a memfd, which QEMU madvises (MADV_HUGEPAGE) and
 * vitrine's QEMU aligns to 2 MiB: each first touch of guest memory is then
 * one fault per 2 MiB instead of 512 - a guest's first app start after its
 * boot 1.38 -> 0.79 s (the research's vm-host-session, 2026-10-06).
 */
#define THP_SHMEM "/sys/kernel/mm/transparent_hugepage/shmem_enabled"
/*
 * The CPU ready to wake up while VMs run: its cores rest in their first
 * idle state (C1), never deeper - a PM QoS request on /dev/cpu_dma_latency
 * for that state's wake-up time - and each one's energy performance
 * preference is EPP_TARGET.  On a Ryzen 7 260 laptop (C1 1 us, C2 18 us, C3
 * 350 us) a GNOME VM's keys reached its screen in 11.3-11.9 ms instead of
 * 15.1-16.3 ms, as on the bare desktop (10.6-11.7), for ~2 W at idle; C1
 * alone 13.7 ms, the preference alone 14.7, no idle state at all 11.7 ms for
 * +26 W (the research, 2026-11-05).  The request is the helper's open
 * file: the kernel takes the lowest of all requests, and drops one when its
 * file closes, whatever the way.
 */
#define CPU_LATENCY_DEV "/dev/cpu_dma_latency"
#define EPP_TARGET "performance"
#define CPU_DIR "/sys/devices/system/cpu"
#define CPUIDLE_DRIVER "/sys/devices/system/cpu/cpuidle/current_driver"

#define FAIR_SERVER_DIR "/sys/kernel/debug/sched/fair_server"
#define EXT_SERVER_DIR "/sys/kernel/debug/sched/ext_server"
#define SCHED_EXT_STATE "/sys/kernel/sched_ext/state"
#define CPUS_ONLINE "/sys/devices/system/cpu/online"
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
/* A thread's scheduling policy and real-time priority: 0 or -errno.  The
   policy read is without SCHED_RESET_ON_FORK; the one set may have it */
int sys_getsched(pid_t tid, int *policy, int *priority);
int sys_setsched(pid_t tid, int policy, int priority);
/* A thread's nice value: 0 or -errno */
int sys_getnice(pid_t tid, int *nice);
int sys_setnice(pid_t tid, int nice);
/*
 * A process's real-time time limit (RLIMIT_RTTIME, in us, RLIM_INFINITY for
 * none): how long a real-time thread of it may run without sleeping before
 * the kernel kills the process.  0 or -errno.
 */
int sys_getrttime(pid_t pid, rlim_t *soft, rlim_t *hard);
int sys_setrttime(pid_t pid, rlim_t soft, rlim_t hard);
/*
 * A PM QoS request: the device @path (CPU_LATENCY_DEV under the root)
 * opened and @us written to it, the kernel's 32-bit value.  It holds while
 * the file returned stays open, and the kernel drops it when that file
 * closes, crash included.  The file, or -errno.
 */
int sys_latency_request(const char *path, int us);
/* The request of sys_latency_request() let go */
void sys_latency_release(int fd);
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
/* The fair server (and the ext server under sched_ext) at the target on
   every online CPU, all or nothing */
void fair_server_on(void);
void fair_server_off(void);
/* That bound is in place, held by this helper; else why not in @why */
bool settings_bound(const char **why);
/* Shared memory's transparent huge pages on madvise (THP_SHMEM), and back */
void thp_on(void);
void thp_off(void);
/* The CPUs kept in their first idle state: this helper's PM QoS request,
   held until cpu_latency_off() or the end */
void cpu_latency_on(void);
void cpu_latency_off(void);
/* Each online CPU's energy performance preference at EPP_TARGET, and back */
void epp_on(void);
void epp_off(void);
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

/* The record of a QEMU whose threads this helper made real-time or ordinary
   at nice BEHIND_NICE (rt- and behind-), or of the process that started it
   (view-rt), for the next helper to put back should this one die: taken (or
   updated) before the change; false when it cannot be, and then nothing is
   changed */
struct watched;
bool sched_hold(struct watched *w);
/* Lets go of it: the threads put back by the last holder, if still there */
void sched_drop(struct watched *w);
/*
 * The record of @w's QEMU another helper keeps (vitrine restarted while it
 * runs) and this one has not joined yet: the threads that were real-time
 * before any helper's rt (@kept, at most MAX_KEPT) and whether the vCPUs
 * were niced.  False when there is none, or it does not say which threads
 * were real-time before.
 */
bool sched_record(const struct watched *w, pid_t *kept, int *nkept, bool *niced);

/* --- process.c: QEMU processes and their threads' scheduling --- */

/* The caller: PKEXEC_UID, else SUDO_UID, else the real uid of a non-root run */
extern uid_t caller_uid;

/* Threads real-time before a helper's first rt, kept as they are: a few */
#define MAX_KEPT 16

struct watched {
    pid_t pid;
    int pidfd;   /* readable once the process has exited */
    int procfd;  /* /proc/<pid>, bound to that process */
    unsigned long long start;   /* its start time (/proc/<pid>/stat) */
    bool rt;     /* rt was asked for it: in front */
    bool rt_once; /* rt made changes once: kept[] is known */
    bool behind; /* its vCPUs were set to BEHIND_NICE */
    bool held;   /* sched_hold() holds its record */
    bool udmabuf; /* udmabuf was asked for it */
    bool view;   /* not a QEMU: the process that started the helper (view-rt) */
    pid_t kept[MAX_KEPT];   /* threads at SCHED_FIFO 1 before its first rt */
    int nkept;
};
#define MAX_WATCHED 64
extern struct watched watched[MAX_WATCHED];
extern int nwatched;

/* The protocol's watch <pid>: true when watched */
bool watch(const char *arg);
/* The watched process with this pid, or NULL */
struct watched *find_watched(const char *arg);
/* The protocol's rt <pid>: the VM in front */
void rt_on(const char *arg);
/* The protocol's behind <pid>: a VM behind the one in front */
void behind(const char *arg);
/* Every watched QEMU's threads as they were before rt and behind, and the
   threads view-rt made real-time */
void rt_off_all(void);
/* Forgets watched[i], which has exited */
void unwatch(int i);
/*
 * The threads of the QEMU @pid (or of the process that started a helper),
 * if it is still the process that started at @start, put back from what rt,
 * behind or view-rt made them (@niced: its vCPUs at BEHIND_NICE), but the
 * @nkeep threads of @keep, real-time before: the record of a helper that let
 * go, or died.  Replies how many, as "restored @verb": always for rt, for
 * view-rt when there were some; never without @verb.
 */
void sched_restore(pid_t pid, unsigned long long start, bool niced, const pid_t *keep,
                   int nkeep, const char *verb);

/*
 * The process that started the helper: pkexec runs the helper in place, so
 * that is the program that ran pkexec - vitrine, the process polkit was
 * asked about - whose VM view's threads view-rt makes real-time while one
 * of its QEMUs is in front.  Taken at the session's start, before any
 * request.
 */
void parent_init(void);
/* The protocol's view-rt <tid>: a thread of that process real-time */
void view_rt(const char *arg);
/* The protocol's view-behind: those threads ordinary again */
void view_behind(void);
/* After a QEMU went behind or exited: with none in front any more, the
   threads view-rt made real-time back to SCHED_OTHER */
void view_check(void);

/* --- group.c: the vitrine group --- */

/* The group polkit's rule (49-vitrine.rules) lets use the helper without a
   password */
#define VITRINE_GROUP "vitrine"
/* vitrine-helper setup-group: the exit status */
int setup_group(void);
