// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ViewThreads (src/gtkview/viewthreads.h): the threads a marked thread
 * starts are told from those of the others; one Mark at a time in the
 * process; a thread not at SCHED_OTHER is not marked; and, on the host's
 * render node, the threads the GL driver starts for a context the view's
 * Qt GUI stand-in makes (QOpenGLContext::create(), as the render thread
 * does) are exactly the ones that came with it.
 */
#include <QOpenGLContext>
#include <QScopeGuard>
#include <QTest>

#include <algorithm>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <gbm.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include "gtkview/viewthreads.h"

namespace {

/* The threads of this process */
std::set<pid_t> tasks()
{
    std::set<pid_t> out;
    if (DIR *dir = opendir("/proc/self/task")) {
        while (dirent *entry = readdir(dir)) {
            if (const int tid = atoi(entry->d_name); tid > 0) {
                out.insert(pid_t(tid));
            }
        }
        closedir(dir);
    }
    return out;
}

int policyOf(pid_t tid)
{
    return sched_getscheduler(tid) & ~SCHED_RESET_ON_FORK;
}

/* Threads that report their id and wait until released */
class Waiters
{
public:
    void start(int n)
    {
        for (int i = 0; i < n; i++) {
            m_threads.emplace_back([this]() {
                std::unique_lock lk(m_lock);
                m_tids.push_back(gettid());
                m_changed.notify_all();
                m_changed.wait(lk, [this]() { return m_quit; });
            });
        }
        std::unique_lock lk(m_lock);
        m_changed.wait(lk, [this]() { return m_tids.size() == m_threads.size(); });
    }
    std::vector<pid_t> tids()
    {
        std::lock_guard g(m_lock);
        std::vector<pid_t> out = m_tids;
        std::sort(out.begin(), out.end());
        return out;
    }
    void release()
    {
        {
            std::lock_guard g(m_lock);
            m_quit = true;
        }
        m_changed.notify_all();
        for (std::thread &t : m_threads) {
            t.join();
        }
        m_threads.clear();
    }
    ~Waiters() { release(); }

private:
    std::mutex m_lock;
    std::condition_variable m_changed;
    std::vector<pid_t> m_tids;
    std::vector<std::thread> m_threads;
    bool m_quit = false;
};

} // namespace

class TestViewThreads : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /* Its own, not another thread's started meanwhile; all back at SCHED_OTHER */
    void startedOnes()
    {
        Waiters own, other;
        bool marked = false;
        int during = -1, after = -1;
        std::vector<pid_t> started;
        std::atomic<bool> markedNow{false}, otherDone{false};
        std::thread render([&]() {
            ViewThreads::Mark mark;
            marked = mark.marked();
            during = policyOf(0);
            own.start(2);
            markedNow = true;
            /* another thread of the process starts one meanwhile */
            while (!otherDone) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            started = mark.end();
            after = policyOf(0);
        });
        while (!markedNow) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        other.start(1);
        otherDone = true;
        render.join();
        QVERIFY(marked);
        QCOMPARE(during, SCHED_BATCH);
        QCOMPARE(after, SCHED_OTHER);
        QCOMPARE(started, own.tids());
        for (const pid_t tid : own.tids()) {
            QCOMPARE(policyOf(tid), SCHED_OTHER);
        }
        QCOMPARE(policyOf(other.tids().front()), SCHED_OTHER);
        /* once ended: nothing more, and the destructor does nothing */
        own.release();
        other.release();
    }

    /*
     * Threads at SCHED_BATCH of their own choice are left alone: one there
     * before the Mark (Mesa's low-priority queues of the screen: its disk
     * cache writer, SCHED_BATCH at nice 19), and one started under it that
     * puts itself at nice 19 at once, as Mesa's do
     */
    void ownChoicesLeftAlone()
    {
        std::atomic<pid_t> earlier{0}, lowPriority{0};
        std::atomic<bool> quit{false};
        const auto background = [&quit](std::atomic<pid_t> *tid) {
            sched_param param{};
            sched_setscheduler(0, SCHED_BATCH, &param);
            setpriority(PRIO_PROCESS, 0, 19);
            *tid = gettid();
            while (!quit) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        };
        std::thread before(background, &earlier);
        while (!earlier) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Waiters own;
        std::vector<pid_t> started;
        std::thread low;
        std::thread render([&]() {
            ViewThreads::Mark mark;
            own.start(1);
            low = std::thread(background, &lowPriority);
            while (!lowPriority) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            started = mark.end();
        });
        render.join();
        QCOMPARE(started, own.tids());
        QCOMPARE(policyOf(earlier), SCHED_BATCH);
        QCOMPARE(policyOf(lowPriority), SCHED_BATCH);
        QCOMPARE(policyOf(own.tids().front()), SCHED_OTHER);
        quit = true;
        before.join();
        low.join();
    }

    /* One at a time: a second waits for the first's end - a second at most */
    void oneAtATime()
    {
        std::atomic<bool> holding{false};
        std::atomic<long long> firstEnd{0}, secondStart{0};
        bool secondMarked = false;
        const auto now = []() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
        };
        std::thread first([&]() {
            ViewThreads::Mark mark;
            holding = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            firstEnd = now();
            mark.end();
        });
        while (!holding) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::thread second([&]() {
            ViewThreads::Mark mark;
            secondStart = now();
            secondMarked = mark.marked();
        });
        first.join();
        second.join();
        QVERIFY(secondMarked);
        QVERIFY(secondStart >= firstEnd);

        /* one that holds it longer than a second: the other goes unmarked */
        std::atomic<bool> done{false};
        holding = false;
        std::thread slow([&]() {
            ViewThreads::Mark mark;
            holding = true;
            while (!done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        while (!holding) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bool unmarked = false;
        std::vector<pid_t> none{1};
        std::thread late([&]() {
            ViewThreads::Mark mark;
            unmarked = !mark.marked();
            none = mark.end();
        });
        late.join();
        done = true;
        slow.join();
        QVERIFY(unmarked);
        QVERIFY(none.empty());
    }

    /* A thread at another policy than SCHED_OTHER is not marked: those it
       starts inherit its own (real-time under chrt, say) */
    void notMarkedElsewhere()
    {
        bool marked = true;
        int policy = -1;
        std::vector<pid_t> started{1};
        Waiters own;
        std::thread batch([&]() {
            sched_param param{};
            sched_setscheduler(0, SCHED_BATCH, &param);
            ViewThreads::Mark mark;
            marked = mark.marked();
            own.start(1);
            started = mark.end();
            policy = policyOf(0);
        });
        batch.join();
        QVERIFY(!marked);
        QVERIFY(started.empty());
        QCOMPARE(policy, SCHED_BATCH);
        QCOMPARE(policyOf(own.tids().front()), SCHED_BATCH);
    }

    void names()
    {
        QVERIFY(ViewThreads::alive(ViewThreads::self()));
        QVERIFY(!ViewThreads::alive(0));
        QVERIFY(!ViewThreads::name(ViewThreads::self()).empty());
        std::thread named([]() {
            pthread_setname_np(pthread_self(), "viewthreads-t");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        });
        QTRY_VERIFY(ViewThreads::find("viewthreads-t") > 0);
        const pid_t tid = ViewThreads::find("viewthreads-t");
        QCOMPARE(ViewThreads::name(tid), std::string("viewthreads-t"));
        named.join();
        QVERIFY(!ViewThreads::alive(tid));
        QCOMPARE(ViewThreads::name(tid), std::string());
        QCOMPARE(ViewThreads::find("viewthreads-t"), pid_t(0));
    }

    /*
     * A GL context made as the render thread makes it, on the render node:
     * the threads the driver started with it (Mesa's per-context threads)
     * are those the stand-in's create() reports, every one, and no other
     */
    void contextThreads()
    {
        const int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            QSKIP("no render node to make a context on");
        }
        gbm_device *gbm = gbm_create_device(fd);
        EGLDisplay dpy = gbm ? eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr)
                             : EGL_NO_DISPLAY;
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
            if (gbm) {
                gbm_device_destroy(gbm);
            }
            close(fd);
            QSKIP("no EGL on the render node");
        }
        /* the display set up on this thread, as GTK's thread does it */
        QtGuiCompat::setEglDisplay(dpy);
        std::set<pid_t> before, after;
        std::vector<pid_t> started;
        std::vector<std::string> names;
        bool created = false;
        std::atomic<bool> made{false}, release{false};
        std::thread render([&]() {
            before = tasks();
            {
                QOpenGLContext ctx;
                QSurfaceFormat format;
                format.setAlphaBufferSize(0);
                ctx.setFormat(format);
                created = ctx.create();
                after = tasks();
                started = ctx.startedThreads();
                for (const pid_t tid : started) {
                    names.push_back(ViewThreads::name(tid));
                }
                /* the context, and its threads, until the checks are done */
                made = true;
                while (!release) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            eglReleaseThread();
        });
        while (!made) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        /* a failed check returns: the render thread ends all the same */
        const auto join = qScopeGuard([&]() {
            release = true;
            if (render.joinable()) {
                render.join();
            }
        });
        QVERIFY(created);
        std::vector<pid_t> appeared;
        std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                            std::back_inserter(appeared));
        std::string list;
        for (size_t i = 0; i < started.size(); i++) {
            list += std::to_string(started[i]) + " " + names[i] + "; ";
        }
        qInfo("threads the driver started for the context: %s",
              list.empty() ? "none" : list.c_str());

        /* every one reported came with the context, and the others that
           came with it are low-priority ones of their own choice (Mesa's
           u_trace queue: SCHED_BATCH at nice 19), or SCHED_IDLE */
        for (const pid_t tid : started) {
            QVERIFY(std::find(appeared.begin(), appeared.end(), tid) != appeared.end());
        }
        for (const pid_t tid : appeared) {
            if (std::find(started.begin(), started.end(), tid) == started.end()) {
                errno = 0;
                const int nice = getpriority(PRIO_PROCESS, id_t(tid));
                QVERIFY2((policyOf(tid) == SCHED_BATCH && nice > 0) || policyOf(tid) == SCHED_IDLE,
                         qPrintable(QString("thread %1 (%2) came with the context and was not "
                                            "reported").arg(tid).arg(QString::fromStdString(
                                                ViewThreads::name(tid)))));
            }
        }
        release = true;
        render.join();
        QtGuiCompat::setEglDisplay(EGL_NO_DISPLAY);
        eglTerminate(dpy);
        gbm_device_destroy(gbm);
        close(fd);
        if (appeared.empty()) {
            QSKIP("the driver starts no thread of its own for a context here");
        }
    }
};

QTEST_GUILESS_MAIN(TestViewThreads)
#include "test_viewthreads.moc"
