// SPDX-License-Identifier: GPL-2.0-or-later
#include "viewthreads.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

/* the Mark of the process: one render thread setting up at a time */
std::timed_mutex g_markLock;
constexpr auto kMarkWait = std::chrono::seconds(1);
/* for threads started under a Mark to run their first lines - Mesa's set
   their own name and nice value there - before they are told apart */
constexpr auto kSettle = std::chrono::milliseconds(2);

/* The thread's policy, without SCHED_RESET_ON_FORK; -1 when it ended */
int policyOf(pid_t tid)
{
    const int policy = sched_getscheduler(tid);
    return policy < 0 ? -1 : policy & ~SCHED_RESET_ON_FORK;
}

/* The thread's nice value (Linux: of that thread alone); 99 when it ended */
int niceOf(pid_t tid)
{
    errno = 0;
    const int nice = getpriority(PRIO_PROCESS, id_t(tid));
    return nice == -1 && errno ? 99 : nice;
}

/* SCHED_OTHER or SCHED_BATCH, its nice value kept: between the two any
   thread may switch itself or another of its process */
bool setPolicy(pid_t tid, int policy)
{
    sched_param param{};
    param.sched_priority = 0;
    return sched_setscheduler(tid, policy, &param) == 0;
}

/* The threads of this process, by id */
std::vector<pid_t> tasks()
{
    std::vector<pid_t> out;
    DIR *dir = opendir("/proc/self/task");

    if (!dir) {
        return out;
    }
    while (dirent *entry = readdir(dir)) {
        char *end = nullptr;
        const long tid = strtol(entry->d_name, &end, 10);
        if (tid > 0 && end && !*end) {
            out.push_back(pid_t(tid));
        }
    }
    closedir(dir);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

pid_t ViewThreads::self()
{
    return gettid();
}

bool ViewThreads::alive(pid_t tid)
{
    char path[64];

    snprintf(path, sizeof(path), "/proc/self/task/%d", int(tid));
    return tid > 0 && access(path, F_OK) == 0;
}

std::string ViewThreads::name(pid_t tid)
{
    char path[64], buf[64];

    snprintf(path, sizeof(path), "/proc/self/task/%d/comm", int(tid));
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return {};
    }
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return {};
    }
    buf[n] = '\0';
    buf[strcspn(buf, "\n")] = '\0';
    return buf;
}

pid_t ViewThreads::find(const char *name)
{
    for (const pid_t tid : tasks()) {
        if (ViewThreads::name(tid) == name) {
            return tid;
        }
    }
    return 0;
}

ViewThreads::Mark::Mark()
{
    if (!g_markLock.try_lock_for(kMarkWait)) {
        return;
    }
    m_locked = true;
    if (policyOf(0) != SCHED_OTHER) {
        return;
    }
    m_before = tasks();
    m_nice = niceOf(self());
    m_marked = setPolicy(0, SCHED_BATCH);
}

ViewThreads::Mark::~Mark()
{
    end();
}

std::vector<pid_t> ViewThreads::Mark::end()
{
    std::vector<pid_t> started;

    if (m_marked) {
        const pid_t me = self();
        std::vector<pid_t> appeared;
        for (const pid_t tid : tasks()) {
            if (tid != me && !std::binary_search(m_before.begin(), m_before.end(), tid)) {
                appeared.push_back(tid);
            }
        }
        if (!appeared.empty()) {
            std::this_thread::sleep_for(kSettle);
        }
        for (const pid_t tid : appeared) {
            if (policyOf(tid) == SCHED_BATCH && niceOf(tid) == m_nice) {
                setPolicy(tid, SCHED_OTHER);
                started.push_back(tid);
            }
        }
        setPolicy(0, SCHED_OTHER);
        m_marked = false;
        m_before.clear();
    }
    if (m_locked) {
        g_markLock.unlock();
        m_locked = false;
    }
    return started;
}
