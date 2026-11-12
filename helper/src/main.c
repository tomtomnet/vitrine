/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vitrine-helper - host settings for vitrine's VMs, as root, while they run.
 *
 * vitrine runs it with pkexec, under one polkit action per verb (each names
 * its first argument, so pkexec's choice is never in doubt):
 *
 *   vitrine-helper session        one session: requests on stdin, one per
 *                                 line (org.vitrine.helper)
 *   vitrine-helper devices        a session for PCI devices passed through
 *                                 that are on vfio-pci already: vfio, watch
 *                                 and release only (org.vitrine.helper.devices)
 *   vitrine-helper devices-bind   the same, and devices taken from the host's
 *                                 drivers (org.vitrine.helper.devices-bind)
 *   vitrine-helper setup-group    the caller in the vitrine group (created if
 *                                 need be), then exit
 *                                 (org.vitrine.helper.setup-group)
 *
 * session and devices: no password for members of the vitrine group in
 * their local, active session (49-vitrine.rules); vitrine asks polkit first
 * and never runs session when it would want one.  devices-bind: an
 * administrator's password, which polkit keeps a few minutes
 * (auth_admin_keep); vitrine runs devices and devices-bind at the start of
 * a VM that passes PCI devices through, with polkit's dialog for whoever
 * needs a password.  setup-group: an administrator's password, always -
 * vitrine runs it only when the user clicks Set Up.  Nothing is installed
 * as a service and nothing stays applied once the VMs are gone.  QEMU
 * itself gets no privilege: the threads' scheduling is set here, and PCI
 * devices are made ready here before QEMU starts.
 *
 * Session requests, each answered by one line or more:
 *   watch PID              a QEMU of the caller's (same uids, qemu-system-*
 *                          or qemu-kvm executable), watched through a pidfd
 *   fair-server on|off     the kernel's fair server (and the ext server under
 *                          sched_ext) at 10 ms / 1 ms on every online CPU, all
 *                          or nothing, read back: the bound rt needs
 *   gpu-floor CARD MHZ|auto|off
 *                          an amdgpu card's lowest gfx clock (auto: 1800 MHz on
 *                          APUs whose minimum is lower)
 *   rt PID                 SCHED_FIFO 1 on every thread of a watched QEMU,
 *                          once the fair server is set and the process's
 *                          real-time time limit lifted: the VM in front;
 *                          one with more vCPUs (named, else its -smp) than
 *                          half the online CPUs is lowered as by behind
 *                          instead, and answered skip
 *   behind PID             those threads back to SCHED_OTHER, its vCPUs at
 *                          nice -5: a VM behind the one in front
 *   view-rt TID            SCHED_FIFO 1 on thread TID of the process that
 *                          started the helper (pkexec's caller: vitrine),
 *                          while a watched QEMU is in front and the fair
 *                          server set: vitrine's view of the VM in front
 *   view-behind            those threads back to SCHED_OTHER, as they go
 *                          whenever no watched QEMU is in front any more
 *   udmabuf PID            the udmabuf limits raised (65536 entries, 2048 MB)
 *                          while that watched QEMU runs: one with a
 *                          native-context GPU
 *   thp on|off             shared memory's transparent huge pages on madvise
 *                          (from never only): the guest RAM QEMU madvises
 *   cpu-latency on|off     the CPUs kept in their first idle state (C1): a
 *                          request on /dev/cpu_dma_latency for its wake-up
 *                          time, as long as the helper holds it
 *   epp on|off             each online CPU's energy performance preference
 *                          at performance (amd-pstate, intel_pstate)
 *   release                everything back now, then exit
 * The settings need a watched QEMU.  Replies start with a word: ready,
 * ok, skip (cannot apply here: lockdown, no debugfs, no amdgpu, no udmabuf
 * module, no huge pages, no idle states, no preference...), error;
 * unprompted: exited PID, restored ..., left ... (changed by someone else
 * since, or still in use, so not restored), bye.
 *
 * devices and devices-bind sessions take, before the VM's QEMU starts:
 *   vfio ADDRESSES MIB     the IOMMU groups of the PCI devices ADDRESSES
 *                          (0000:6e:00.0, comma-separated) on vfio-pci, the
 *                          whole group or nothing, its bridges as they are;
 *                          /dev/vfio/<group> the caller's; and the locked
 *                          memory limit of the process that started the
 *                          helper (vitrine: its QEMU inherits it) at MIB at
 *                          least.  devices takes groups already on vfio-pci
 *                          only; devices-bind also takes devices from host
 *                          drivers, never one the host uses (the GPU it
 *                          started on, a display connected, device nodes a
 *                          process holds, a disk mounted, a network
 *                          interface up, a platform device).  Answers "ok
 *                          vfio ADDRESS=DRIVER,...: ..." (the driver each
 *                          one had).  Unlike the settings, it needs no
 *                          watched QEMU: what it made ready waits for the
 *                          next watch, which takes it, VFIO_EXPIRY_MS at
 *                          most, and goes back with that QEMU - or with
 *                          the helper, if no QEMU came.
 * and watch PID, release.
 *
 * When the last watched QEMU exits, crash included, or stdin closes while
 * none is watched, everything is put back and the helper exits; with stdin
 * closed it keeps going while watched QEMUs run (they outlive vitrine).  A
 * PCI device something still has open stays on vfio-pci, recorded: a later
 * helper, whatever its verb, gives it back once it is free.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

#include "helper.h"

#define MAX_LINE 512

bool rooted(char *out, size_t size, const char *path)
{
    int n = snprintf(out, size, "%s%s", sys_root(), path);

    return n >= 0 && (size_t)n < size;
}

bool parse_uint(const char *s, unsigned long long max, unsigned long long *out)
{
    unsigned long long v = 0;

    if (!*s || (s[0] == '0' && s[1]) || strlen(s) > 20) {
        return false;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > (max - (unsigned)(*s - '0')) / 10) {
            return false;
        }
        v = v * 10 + (unsigned)(*s - '0');
    }
    *out = v;
    return true;
}

void reply(const char *fmt, ...)
{
    char buf[MAX_LINE];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n > sizeof(buf) - 2) {
        n = sizeof(buf) - 2;
    }
    buf[n++] = '\n';
    /*
     * Never wait for the reader: a caller that stopped reading would hold
     * up the watch, and the settings would outlive the VMs.  Writable means
     * half a socket's buffer free, or a page of a pipe's: the line goes out
     * whole at once; else it is dropped, as nobody reads it.  vitrine may be
     * gone (SIGPIPE ignored): the work goes on.
     */
    struct pollfd out = {.fd = STDOUT_FILENO, .events = POLLOUT};
    if (poll(&out, 1, 0) == 1 && (out.revents & POLLOUT) &&
        write(STDOUT_FILENO, buf, (size_t)n) < 0) {
        return;
    }
}

/* Who asked: pkexec's PKEXEC_UID, sudo's SUDO_UID (tests by hand), or the
   real uid of a run without privileges */
static bool find_caller(void)
{
    const char *s = getenv("PKEXEC_UID");
    unsigned long long uid;

    if (!s) {
        s = getenv("SUDO_UID");
    }
    if (s) {
        if (!parse_uint(s, 0xfffffffeULL, &uid)) {
            return false;
        }
        caller_uid = (uid_t)uid;
        return true;
    }
    if (geteuid() == 0) {
        return false;
    }
    caller_uid = getuid();
    return true;
}

/* 0, 1 and 2 open (else a file opened later could take one and get the
   replies), nothing else inherited */
static void sane_fds(void)
{
    for (int fd = 0; fd <= 2; fd++) {
        if (fcntl(fd, F_GETFD) < 0 && open("/dev/null", O_RDWR) != fd) {
            _exit(1);
        }
    }
    close_range(3, ~0U, 0);
}

/*
 * No signal ignored, caught or blocked as whoever started it left it: pkexec
 * passes both on.  An ignored SIGCHLD, say, has the kernel reap the group
 * tools before their exit status is read.
 */
static void sane_signals(void)
{
    struct sigaction dfl = {.sa_handler = SIG_DFL};
    sigset_t none;

    sigemptyset(&dfl.sa_mask);
    for (int sig = 1; sig < NSIG; sig++) {
        /* SIGKILL, SIGSTOP and libc's own refuse: nothing to do for them */
        sigaction(sig, &dfl, NULL);
    }
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
}

enum session_kind session_kind = SESSION_TUNE;

/* A request of a devices or devices-bind session */
static void devices_request(char **words, int n)
{
#define IS(verb, args) (strcmp(words[0], verb) == 0 && n == (args) + 1)
    if (IS("watch", 1)) {
        /* the QEMU that takes what vfio made ready */
        if (watch(words[1])) {
            vfio_claim(find_watched(words[1]));
        }
    } else if (IS("vfio", 2)) {
        vfio_request(words[1], words[2], session_kind == SESSION_DEVICES_BIND);
    } else {
        reply("error %.32s: unknown request", words[0]);
    }
#undef IS
}

/* One request: false when it asks the helper to end */
static bool request(char *line)
{
    char *words[4] = {NULL, NULL, NULL, NULL};
    char *save = NULL;
    int n = 0;

    for (const char *c = line; *c; c++) {
        if (*c < 0x20 || *c > 0x7e) {
            reply("error: requests are printable ASCII");
            return true;
        }
    }
    for (char *w = strtok_r(line, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
        if (n == 4) {
            reply("error: too many words");
            return true;
        }
        words[n++] = w;
    }
    if (n == 0) {
        return true;
    }
#define IS(verb, args) (strcmp(words[0], verb) == 0 && n == (args) + 1)
    if (IS("release", 0)) {
        return false;
    }
    if (session_kind != SESSION_TUNE) {
        devices_request(words, n);
        return true;
    }
    if (strcmp(words[0], "vfio") == 0) {
        /* a polkit action of their own: taking a device from the host is more
           than tuning it */
        reply("error vfio: PCI devices go through vitrine-helper devices or devices-bind");
        return true;
    }
    if (IS("watch", 1)) {
        watch(words[1]);
    } else if (IS("fair-server", 1) && strcmp(words[1], "off") == 0) {
        fair_server_off();
    } else if (IS("thp", 1) && strcmp(words[1], "off") == 0) {
        thp_off();
    } else if (IS("cpu-latency", 1) && strcmp(words[1], "off") == 0) {
        cpu_latency_off();
    } else if (IS("epp", 1) && strcmp(words[1], "off") == 0) {
        epp_off();
    } else if (IS("gpu-floor", 2) && strcmp(words[2], "off") == 0) {
        gpu_floor(words[1], words[2]);
    } else if (IS("view-behind", 0)) {
        view_behind();
    } else if ((IS("fair-server", 1) || IS("thp", 1) || IS("cpu-latency", 1) || IS("epp", 1) ||
                IS("gpu-floor", 2) || IS("rt", 1) || IS("behind", 1) || IS("udmabuf", 1) ||
                IS("view-rt", 1)) &&
               !nwatched) {
        /* nothing applied while no VM runs */
        reply("error %s: watch a QEMU first", words[0]);
    } else if (IS("fair-server", 1) && strcmp(words[1], "on") == 0) {
        fair_server_on();
    } else if (IS("thp", 1) && strcmp(words[1], "on") == 0) {
        thp_on();
    } else if (IS("cpu-latency", 1) && strcmp(words[1], "on") == 0) {
        cpu_latency_on();
    } else if (IS("epp", 1) && strcmp(words[1], "on") == 0) {
        epp_on();
    } else if (IS("gpu-floor", 2)) {
        gpu_floor(words[1], words[2]);
    } else if (IS("rt", 1)) {
        rt_on(words[1]);
    } else if (IS("behind", 1)) {
        behind(words[1]);
    } else if (IS("udmabuf", 1)) {
        udmabuf_on(words[1]);
    } else if (IS("view-rt", 1)) {
        view_rt(words[1]);
    } else {
        reply("error %.32s: unknown request", words[0]);
    }
#undef IS
    return true;
}

static int session(enum session_kind kind)
{
    char buf[MAX_LINE + 1];
    size_t len = 0;
    bool input = true, ever = false, overlong = false;
    sigset_t mask;
    int sfd;

    session_kind = kind;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd < 0 || !settings_init()) {
        reply("error: %s", sfd < 0 ? strerror(errno) : "cannot use " STATE_DIR);
        return 1;
    }
    /* PCI devices a dead helper left, given back outside the lock: any verb
       gives back what an earlier one took, none takes one without its own */
    vfio_recover();
    /* who started it, before any request can come */
    parent_init();
    reply("ready %d", HELPER_PROTOCOL);

    for (;;) {
        struct pollfd fds[2 + MAX_WATCHED];
        pid_t gone[MAX_WATCHED];
        int nfds = 0, ngone = 0, first;

        fds[nfds++] = (struct pollfd){.fd = sfd, .events = POLLIN};
        fds[nfds++] = (struct pollfd){.fd = input ? STDIN_FILENO : -1, .events = POLLIN};
        first = nfds;
        for (int i = 0; i < nwatched; i++) {
            fds[nfds++] = (struct pollfd){.fd = watched[i].pidfd, .events = POLLIN};
        }
        /* awake for the PCI devices that wait for their QEMU */
        if (poll(fds, (nfds_t)nfds, vfio_timeout()) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        /* those no QEMU came for in time go back */
        vfio_expire();
        if (fds[0].revents) {
            /* SIGTERM (shutdown) or SIGINT: everything back first */
            break;
        }
        if (fds[1].revents) {
            ssize_t r = read(STDIN_FILENO, buf + len, sizeof(buf) - 1 - len);
            char *nl;

            if (r <= 0 && !(r < 0 && (errno == EINTR || errno == EAGAIN))) {
                input = false;
            } else if (r > 0) {
                len += (size_t)r;
                buf[len] = '\0';
                while ((nl = memchr(buf, '\n', len))) {
                    size_t used = (size_t)(nl - buf) + 1;
                    bool go_on = true;

                    *nl = '\0';
                    if (overlong) {
                        overlong = false;
                    } else {
                        go_on = request(buf);
                    }
                    memmove(buf, nl + 1, len - used);
                    len -= used;
                    buf[len] = '\0';
                    if (!go_on) {
                        goto end;
                    }
                    ever |= nwatched > 0;
                }
                if (len == sizeof(buf) - 1) {
                    /* no newline in MAX_LINE bytes: dropped up to the next one */
                    if (!overlong) {
                        reply("error: request too long");
                    }
                    overlong = true;
                    len = 0;
                }
            }
        }
        for (int i = first; i < nfds; i++) {
            if (fds[i].revents) {
                gone[ngone++] = watched[i - first].pid;
            }
        }
        for (int g = 0; g < ngone; g++) {
            for (int i = 0; i < nwatched; i++) {
                if (watched[i].pid == gone[g]) {
                    reply("exited %d", (int)gone[g]);
                    unwatch(i);
                    break;
                }
            }
        }
        /* the udmabuf limits go with the last QEMU that asked for them, not
           with the last QEMU; the view's threads with the QEMU in front; PCI
           devices with the QEMU that had them */
        if (ngone) {
            udmabuf_check();
            view_check();
            vfio_check();
        }
        /* the last VM is gone, or vitrine with none left */
        if ((ever && !nwatched) || (!input && !nwatched)) {
            break;
        }
    }
end:
    /* release, a signal, or the end: threads of QEMUs still running first,
       then PCI devices (those still open stay recorded) */
    rt_off_all();
    vfio_release();
    settings_release();
    reply("bye");
    return 0;
}

int main(int argc, char **argv)
{
    sane_signals();
    sane_fds();
    umask(077);
    signal(SIGPIPE, SIG_IGN);
    /* a terminal closing is no reason to drop what running VMs use */
    signal(SIGHUP, SIG_IGN);
    /* never more privileges than now, through exec or otherwise */
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    if (!sys_init()) {
        return 1;
    }
    if (!find_caller()) {
        fprintf(stderr, "vitrine-helper: run it through pkexec (no PKEXEC_UID)\n");
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "session") == 0) {
        return session(SESSION_TUNE);
    }
    if (argc == 2 && strcmp(argv[1], "devices") == 0) {
        return session(SESSION_DEVICES);
    }
    if (argc == 2 && strcmp(argv[1], "devices-bind") == 0) {
        return session(SESSION_DEVICES_BIND);
    }
    /* no argument: the group and the user are not the caller's to choose */
    if (argc == 2 && strcmp(argv[1], "setup-group") == 0) {
        return setup_group();
    }
    /* none at all included: pkexec would run that under its generic action */
    fprintf(stderr, "usage: vitrine-helper session | devices | devices-bind | setup-group\n");
    return 2;
}
