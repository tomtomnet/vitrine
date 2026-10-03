/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A stand-in for QEMU in the tests: named qemu-system-x86_64, a few idle
 * threads, runs until signalled or until its stdin ends; SIGUSR1 makes it
 * exec sleep, a program that is no QEMU.
 *
 * Its stdin is the pipe of the test's QProcess, whose write end only the
 * test holds: it ends when the test does, crash included, where the
 * destructor that kills it never runs.  Else every stand-in, and each fake
 * helper watching one, would run on for good, to be found by pid.
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
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
    pthread_t t;
    ssize_t n;
    char c;

    signal(SIGUSR1, become_sleep);
    for (int i = 0; i < 3; i++) {
        pthread_create(&t, NULL, idle, NULL);
    }
    /* on the main thread: the tests count 4 threads */
    for (;;) {
        n = read(STDIN_FILENO, &c, 1);
        if (n == 0 || (n < 0 && errno != EINTR)) {
            _exit(0);
        }
    }
}
