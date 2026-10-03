/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A stand-in for QEMU in the tests: named qemu-system-x86_64, a few idle
   threads, runs until signalled */
#include <pthread.h>
#include <unistd.h>

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

    for (int i = 0; i < 3; i++) {
        pthread_create(&t, NULL, idle, NULL);
    }
    for (;;) {
        pause();
    }
}
