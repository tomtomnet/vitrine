/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * vitrine-helper - host settings for vitrine's VMs, as root, while they run.
 *
 * vitrine starts it with pkexec when a VM starts (polkit action
 * org.vitrine.helper: no password for members of the vitrine group, never
 * asked otherwise).  Nothing is installed as a service and nothing stays
 * applied once the VMs are gone.
 *
 *   vitrine-helper                one session: requests on stdin, one per line
 *   vitrine-helper setcap PATH    cap_sys_nice=ep on vitrine's QEMU build, then exit
 *
 * Session requests, each answered by one line or more:
 *   watch PID              a QEMU of the caller's (same uids, qemu-system-*
 *                          or qemu-kvm executable), watched through a pidfd
 *   fair-server on|off     the kernel's fair server at 10 ms / 1 ms on every CPU
 *   gpu-floor CARD MHZ|auto|off
 *                          an amdgpu card's lowest gfx clock (auto: 1800 MHz on
 *                          APUs whose minimum is lower)
 *   rt PID                 SCHED_FIFO 1 on every thread of a watched QEMU
 *   release                everything back now, then exit
 * The settings need a watched QEMU.  Replies start with a word: ready,
 * ok, skip (cannot apply here: lockdown, no debugfs, no amdgpu...), error;
 * unprompted: exited PID, restored ..., left ... (changed by someone else
 * since, so not restored), bye.
 *
 * When the last watched QEMU exits, crash included, or stdin closes while
 * none is watched, everything is put back and the helper exits; with stdin
 * closed it keeps going while watched QEMUs run (they outlive vitrine).
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
    if (IS("watch", 1)) {
        watch(words[1]);
    } else if (IS("fair-server", 1) && strcmp(words[1], "off") == 0) {
        fair_server_off();
    } else if (IS("gpu-floor", 2) && strcmp(words[2], "off") == 0) {
        gpu_floor(words[1], words[2]);
    } else if ((IS("fair-server", 1) || IS("gpu-floor", 2) || IS("rt", 1)) && !nwatched) {
        /* nothing applied while no VM runs */
        reply("error %s: watch a QEMU first", words[0]);
    } else if (IS("fair-server", 1) && strcmp(words[1], "on") == 0) {
        fair_server_on();
    } else if (IS("gpu-floor", 2)) {
        gpu_floor(words[1], words[2]);
    } else if (IS("rt", 1)) {
        rt_on(words[1]);
    } else {
        reply("error %.32s: unknown request", words[0]);
    }
#undef IS
    return true;
}

static int session(void)
{
    char buf[MAX_LINE + 1];
    size_t len = 0;
    bool input = true, ever = false, overlong = false;
    sigset_t mask;
    int sfd;

    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd < 0 || !settings_init()) {
        reply("error: %s", sfd < 0 ? strerror(errno) : "cannot use " STATE_DIR);
        return 1;
    }
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
        if (poll(fds, (nfds_t)nfds, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
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
        /* the last VM is gone, or vitrine with none left */
        if ((ever && !nwatched) || (!input && !nwatched)) {
            break;
        }
    }
end:
    /* release, a signal, or the end: threads of QEMUs still running first */
    rt_off_all();
    settings_release();
    reply("bye");
    return 0;
}

int main(int argc, char **argv)
{
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
    if (argc == 3 && strcmp(argv[1], "setcap") == 0) {
        return setcap(argv[2]);
    }
    if (argc != 1) {
        fprintf(stderr, "usage: vitrine-helper [setcap PATH]\n");
        return 2;
    }
    return session();
}
