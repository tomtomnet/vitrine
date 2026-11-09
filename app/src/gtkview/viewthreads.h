// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

/*
 * The threads of a VM view, for real time while its VM is in front
 * (VmDisplay::threads(), HostSettings::setViewThreads()): Linux's thread ids
 * and names, and the threads a render thread starts as it sets up its GL
 * context - Mesa's for the context (its glthread, the driver's threaded
 * context: radeonsi's "<process>:gl0", ":gdrv0"), which do the render
 * thread's GL work.  Mesa names them after the process, as it names those of
 * GTK's own contexts, so they are told apart by who started them: while a
 * Mark lives, the render thread runs at SCHED_BATCH - which any thread may
 * switch to and back from, for itself and the others of its process - and
 * the threads it starts inherit that; at the Mark's end the threads that
 * appeared meanwhile at SCHED_BATCH with its nice value are its own, and go
 * back to SCHED_OTHER with it.  Not the others at SCHED_BATCH: Mesa puts its
 * low-priority queues there itself, at nice 19 (the screen's disk cache
 * writer, a shader compiler's: made with GTK's display, before any view).
 * One Mark at a time in the process (two views can set up at once), kept
 * short: around eglCreateContext and the first eglMakeCurrent of the render
 * thread, never across a wait.
 */
namespace ViewThreads {

/* The calling thread's id */
pid_t self();
/* @tid is a thread of this process */
bool alive(pid_t tid);
/* Its name (/proc/self/task/TID/comm), "" when it ended */
std::string name(pid_t tid);
/* The first thread of this process named @name (GLib's "gdbus"), 0 for none */
pid_t find(const char *name);

class Mark
{
public:
    /* The calling thread marked: if it runs at SCHED_OTHER (not when made
       real-time, say under chrt, whose policy the threads it starts inherit
       anyway), once another thread's Mark ended - one second at most, else
       it goes unmarked */
    Mark();
    /* Ends it, if end() did not */
    ~Mark();
    Mark(const Mark &) = delete;
    Mark &operator=(const Mark &) = delete;

    bool marked() const { return m_marked; }
    /*
     * The end: the threads it started since - those that appeared, still at
     * SCHED_BATCH and its nice value (not those that chose their own: Mesa's
     * low-priority queues, SCHED_IDLE...) - back at SCHED_OTHER with the
     * calling thread.  Their ids, ascending; none when it was not marked.
     */
    std::vector<pid_t> end();

private:
    bool m_marked = false;
    bool m_locked = false;
    int m_nice = 0;
    std::vector<pid_t> m_before;    // the threads of the process at its start
};

} // namespace ViewThreads
