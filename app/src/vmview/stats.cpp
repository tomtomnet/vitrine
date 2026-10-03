#include "stats.h"
#include "common.h"

#include <algorithm>

namespace {
double pct(std::vector<int64_t> &v, double p, double unit)
{
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    size_t i = std::min(v.size() - 1, size_t(p * v.size()));
    return v[i] / unit;
}
} // namespace

void Stats::updateReceived(int64_t recvNs, int64_t replyNs)
{
    std::lock_guard g(m_lock);
    m_updates++;
    m_reply.push_back(replyNs - recvNs);
    if (m_lastArrival) {
        m_arrival.push_back(recvNs - m_lastArrival);
    }
    m_lastArrival = recvNs;
}

void Stats::scanoutReceived()
{
    std::lock_guard g(m_lock);
    m_scanouts++;
}

void Stats::frameDrawn(int64_t recvNs, int64_t swapEndNs, bool imported)
{
    std::lock_guard g(m_lock);
    m_drawn++;
    m_imports += imported;
    if (recvNs) {
        m_draw.push_back(swapEndNs - recvNs);
    }
}

void Stats::framePresented(int64_t recvNs, int64_t presentNs)
{
    std::lock_guard g(m_lock);
    m_presented++;
    if (recvNs && presentNs > recvNs) {
        m_present.push_back(presentNs - recvNs);
    }
    if (m_lastPresent) {
        m_interval.push_back(presentNs - m_lastPresent);
    }
    m_lastPresent = presentNs;
}

void Stats::frameDiscarded()
{
    std::lock_guard g(m_lock);
    m_discarded++;
}

void Stats::inputRoundTrip(int64_t ns)
{
    std::lock_guard g(m_lock);
    m_input.push_back(ns);
}

void Stats::gdbusHops(int64_t inNs, int64_t outNs)
{
    std::lock_guard g(m_lock);
    if (inNs > 0) {
        m_hopIn.push_back(inNs);
    }
    if (outNs > 0) {
        m_hopOut.push_back(outNs);
    }
}

void Stats::replyDelay(int64_t ns)
{
    std::lock_guard g(m_lock);
    m_late.push_back(ns);
}

Stats::Summary Stats::take()
{
    std::lock_guard g(m_lock);
    Summary s;
    int64_t now = nowNs();
    if (!m_windowStart) {
        m_windowStart = now;
    }
    s.seconds = (now - m_windowStart) / 1e9;
    s.updates = m_updates;
    s.scanouts = m_scanouts;
    s.drawn = m_drawn;
    s.imports = m_imports;
    s.presented = m_presented;
    s.discarded = m_discarded;
    s.replyUsP50 = pct(m_reply, 0.5, 1e3);
    s.replyUsMax = m_reply.empty() ? 0 : m_reply.back() / 1e3;
    s.drawMsP50 = pct(m_draw, 0.5, 1e6);
    s.drawMsP99 = pct(m_draw, 0.99, 1e6);
    s.presentMsP50 = pct(m_present, 0.5, 1e6);
    s.presentMsP99 = pct(m_present, 0.99, 1e6);
    s.intervalMsP50 = pct(m_interval, 0.5, 1e6);
    s.intervalMsP99 = pct(m_interval, 0.99, 1e6);
    s.arrivalMsP50 = pct(m_arrival, 0.5, 1e6);
    s.arrivalMsP99 = pct(m_arrival, 0.99, 1e6);
    s.inputs = int(m_input.size());
    s.inputUsP50 = pct(m_input, 0.5, 1e3);
    s.inputUsP99 = pct(m_input, 0.99, 1e3);
    s.lateReplyMsP50 = pct(m_late, 0.5, 1e6);
    s.hopInUsP50 = pct(m_hopIn, 0.5, 1e3);
    s.hopOutUsP50 = pct(m_hopOut, 0.5, 1e3);

    m_windowStart = now;
    m_updates = m_scanouts = m_drawn = m_imports = m_presented = m_discarded = 0;
    for (auto *v : {&m_reply, &m_draw, &m_present, &m_interval, &m_arrival, &m_input, &m_late,
                    &m_hopIn, &m_hopOut}) {
        v->clear();
    }
    if (m_trace) {
        fflush(m_trace);
    }
    return s;
}

void Stats::openTrace(const char *path)
{
    std::lock_guard g(m_lock);
    m_trace = fopen(path, "w");
}

void Stats::trace(char kind, uint64_t serial, int64_t a, int64_t b)
{
    if (!m_trace) {
        return;
    }
    std::lock_guard g(m_lock);
    fprintf(m_trace, "%c %llu %lld %lld\n", kind, (unsigned long long)serial,
            (long long)a, (long long)b);
}
