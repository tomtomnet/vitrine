/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A stand-in for QEMU in the tests: named qemu-system-x86_64, a few idle
 * threads - two named as QEMU names its vCPUs with debug-threads=on, "CPU
 * 0/KVM" and "CPU 1/KVM" - runs until signalled or until its stdin ends;
 * SIGUSR1 makes it exec sleep, a program that is no QEMU.  With
 * FAKE_QEMU_RTTIME=US, it sets its own real-time time limit (RLIMIT_RTTIME)
 * to US first, as PipeWire's module-rt does in QEMU when RTKit gives its
 * audio thread real-time.
 *
 * Its stdin is the pipe of the test's QProcess, whose write end only the
 * test holds: it ends when the test does, crash included, where the
 * destructor that kills it never runs.  Else every stand-in, and each fake
 * helper watching one, would run on for good, to be found by pid.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

static void become_sleep(int sig)
{
    (void)sig;
    execl("/bin/sleep", "sleep", "300", (char *)NULL);
}

static void *idle(void *arg)
{
    (void)arg;
    for (;;) {
        pause();
    }
    return NULL;
}

int main(void)
{
    const char *rttime = getenv("FAKE_QEMU_RTTIME");
    pthread_t t;
    ssize_t n;
    char c;

    if (rttime) {
        const rlim_t us = (rlim_t)strtoull(rttime, NULL, 10);
        const struct rlimit limit = {.rlim_cur = us, .rlim_max = us};

        setrlimit(RLIMIT_RTTIME, &limit);
    }
    signal(SIGUSR1, become_sleep);
    for (int i = 0; i < 3; i++) {
        char name[16];

        pthread_create(&t, NULL, idle, NULL);
        snprintf(name, sizeof(name), i < 2 ? "CPU %d/KVM" : "worker", i);
        pthread_setname_np(t, name);
    }
    /* on the main thread: the tests count 4 threads */
    for (;;) {
        n = read(STDIN_FILENO, &c, 1);
        if (n == 0 || (n < 0 && errno != EINTR)) {
            _exit(0);
        }
    }
}
