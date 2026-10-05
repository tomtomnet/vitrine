// SPDX-License-Identifier: GPL-2.0-or-later
#include "perfstats.h"

#include <QDir>
#include <QFile>
#include <QJsonValue>

#include <cmath>

namespace PerfStats {

/* What a VM exit costs on the Ryzen host, with its branch predictor flush */
static const double kExitCostUs = 4;

static Latency latency(const QJsonValue &v)
{
    const QJsonObject o = v.toObject();

    return {o["samples"].toInt(), o["median"].toDouble() / 1000, o["p99"].toDouble() / 1000,
            o["max"].toDouble() / 1000};
}

bool parseDisplay(const QJsonObject &reply, Screen *display, QString *type)
{
    const QJsonArray consoles = reply["consoles"].toArray();
    qsizetype best = -1;
    double bestFrames = -1;

    if (type) {
        *type = reply["type"].toString();
    }
    for (qsizetype i = 0; i < consoles.size(); i++) {
        const QJsonObject c = consoles[i].toObject();
        const double frames = c["presented"].toDouble() + c["flushes"].toDouble();
        if (frames > bestFrames) {
            best = i;
            bestFrames = frames;
        }
    }
    if (best < 0) {
        return false;
    }

    const QJsonObject c = consoles[best].toObject();
    Screen d;
    d.console = c["console"].toInt();
    d.width = c["width"].toInt();
    d.height = c["height"].toInt();
    d.refreshHz = c["refresh-rate"].toDouble() / 1000;
    d.method = c["method"].toString();
    d.flushes = c["flushes"].toDouble();
    d.presented = c["presented"].toDouble();
    d.dropped = c["dropped"].toDouble();
    d.direct = c["direct"].toDouble();
    d.redraws = c["redraws"].toDouble();
    d.frame = latency(c["frame-latency"]);
    d.qemu = latency(c["qemu-latency"]);
    d.interval = latency(c["frame-interval"]);
    d.input = latency(c["input-latency"]);
    d.hasInputWait = c.contains("input-wait");
    d.inputWait = latency(c["input-wait"]);
    *display = d;
    return true;
}

QHash<qint64, Thread> readThreads(qint64 pid, const QString &proc)
{
    QHash<qint64, Thread> threads;
    const QDir tasks(QString("%1/%2/task").arg(proc).arg(pid));

    if (pid <= 0) {
        return threads;
    }
    for (const QString &name : tasks.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool ok = false;
        const qint64 tid = name.toLongLong(&ok);
        QFile f(tasks.filePath(name + "/schedstat"));

        if (!ok || !f.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QList<QByteArray> fields = f.readAll().simplified().split(' ');
        if (fields.size() >= 3) {
            threads.insert(tid, {fields[0].toLongLong(), fields[1].toLongLong(),
                                 fields[2].toLongLong()});
        }
    }
    return threads;
}

Load load(const QHash<qint64, Thread> &before, const QHash<qint64, Thread> &after,
          const QList<qint64> &tids, qint64 elapsedNs)
{
    Load l;
    qint64 run = 0, wait = 0, slices = 0;

    if (elapsedNs <= 0) {
        return l;
    }
    for (const qint64 tid : tids) {
        const auto b = before.constFind(tid);
        const auto a = after.constFind(tid);

        if (b == before.cend() || a == after.cend()) {
            continue;
        }
        l.threads++;
        run += qMax<qint64>(0, a->runNs - b->runNs);
        wait += qMax<qint64>(0, a->waitNs - b->waitNs);
        slices += qMax<qint64>(0, a->slices - b->slices);
    }
    if (l.threads) {
        l.cpu = 100.0 * run / elapsedNs;
        l.wait = 100.0 * wait / (double(elapsedNs) * l.threads);
        l.waitPerRunMs = slices ? wait / 1e6 / slices : 0;
    }
    return l;
}

QHash<QString, double> sumKvmStats(const QJsonArray &reply)
{
    QHash<QString, double> sums;

    for (const QJsonValue &entry : reply) {
        if (entry["provider"].toString() != "kvm") {
            continue;
        }
        for (const QJsonValue &stat : entry["stats"].toArray()) {
            const QJsonValue value = stat["value"];
            if (value.isDouble()) {
                sums[stat["name"].toString()] += value.toDouble();
            }
        }
    }
    return sums;
}

QString formatMs(double ms)
{
    if (ms < 1) {
        return QObject::tr("%1 ms").arg(ms, 0, 'f', 2);
    }
    if (ms < 10) {
        return QObject::tr("%1 ms").arg(ms, 0, 'f', 1);
    }
    return QObject::tr("%1 ms").arg(qRound(ms));
}

QString formatPercent(double p)
{
    if (p < 0.05) {
        return QObject::tr("< 0.1 %");
    }
    return p < 10 ? QObject::tr("%1 %").arg(p, 0, 'f', 1) : QObject::tr("%1 %").arg(qRound(p));
}

static QString perSecond(double n)
{
    if (n >= 10000) {
        return QObject::tr("%1 k/s").arg(qRound(n / 1000));
    }
    if (n >= 1000) {
        return QObject::tr("%1 k/s").arg(n / 1000, 0, 'f', 1);
    }
    return QObject::tr("%1/s").arg(qRound(n));
}

QString summary(const Snapshot &s)
{
    QStringList parts{displaySummary(s), mainLoopSummary(s)};

    parts.removeAll(QString());
    return parts.join(QObject::tr(" · "));
}

QString displaySummary(const Snapshot &s)
{
    QStringList parts;

    if (s.display) {
        const Screen &d = s.screen;
        if (d.presented >= 0.5) {
            QString frames = QObject::tr("%1 fps").arg(qRound(d.presented));
            if (d.frame.samples) {
                frames += QObject::tr(" · frame %1").arg(formatMs(d.frame.median));
                if (d.method == "swap") {
                    frames += QObject::tr(" (to swap)");
                }
            }
            parts << frames;
        } else {
            parts << QObject::tr("display idle");
        }
        if (d.input.samples) {
            parts << QObject::tr("input %1").arg(formatMs(d.input.median));
        }
    }
    return parts.join(QObject::tr(" · "));
}

QString mainLoopSummary(const Snapshot &s)
{
    return s.threads ? QObject::tr("main loop wait %1").arg(formatPercent(s.mainLoop.wait))
                     : QString();
}

/* A row of plain text: values like "< 0.1 %" are no markup */
static QString row(const QString &label, const QString &value, const QString &note = {})
{
    return QString("<tr><td nowrap>%1</td><td nowrap align=\"right\">&nbsp;&nbsp;%2</td>"
                   "<td nowrap>&nbsp;&nbsp;%3</td></tr>")
        .arg(label.toHtmlEscaped(), value.toHtmlEscaped(), note.toHtmlEscaped());
}

QString tableRow(const QString &label, const QString &value, const QString &note)
{
    return row(label, value, note);
}

static QString p99(const Latency &l)
{
    return QObject::tr("p99 %1").arg(formatMs(l.p99));
}

QString details(const Snapshot &s)
{
    QString html;

    if (s.display) {
        const Screen &d = s.screen;
        const bool swap = d.method == "swap";
        QString title = QObject::tr("<b>Display</b>, over the last second");
        QStringList about;

        if (d.width && d.height) {
            about << QObject::tr("%1×%2").arg(d.width).arg(d.height);
        }
        if (d.refreshHz > 0) {
            about << QObject::tr("monitor at %1 Hz").arg(d.refreshHz, 0, 'f',
                                                         d.refreshHz < 100 ? 1 : 0);
        }
        if (!about.isEmpty()) {
            title += QObject::tr(" (%1)").arg(about.join(QObject::tr(", ")));
        }
        html += title + "<table>";
        html += row(QObject::tr("Frames on screen"), perSecond(d.presented),
                    QObject::tr("of %1 the guest made; %2 dropped")
                        .arg(perSecond(d.flushes), perSecond(d.dropped)));
        if (d.frame.samples) {
            html += row(QObject::tr("Frame latency"), formatMs(d.frame.median),
                        p99(d.frame) + (swap ? QObject::tr(": guest flush to buffer swap, "
                                                           "without the compositor")
                                             : QObject::tr(": guest flush to on screen")));
            if (!swap && d.qemu.samples) {
                html += row(QObject::tr("of which QEMU"), formatMs(d.qemu.median),
                            p99(d.qemu) + QObject::tr(": flush to buffer swap"));
            }
        }
        if (d.interval.samples) {
            html += row(QObject::tr("Frame interval"), formatMs(d.interval.median),
                        p99(d.interval));
        }
        if (d.direct >= 0.5) {
            html += row(QObject::tr("Direct scanout"), perSecond(d.direct),
                        QObject::tr("frames the compositor showed without composing"));
        }
        if (d.input.samples) {
            html += row(QObject::tr("Input latency"), formatMs(d.input.median),
                        p99(d.input) + QObject::tr(", %n key presses or clicks in 10 s: "
                                                   "to the guest's answer on screen",
                                                   nullptr, d.input.samples));
        } else {
            html += row(QObject::tr("Input latency"), QObject::tr("–"),
                        QObject::tr("needs a key press or click while the guest is idle"));
        }
        if (d.hasInputWait && d.inputWait.samples) {
            html += row(QObject::tr("Input wait"), formatMs(d.inputWait.median),
                        QObject::tr("max %1: until QEMU read the event, as it polls input "
                                    "with its display refresh")
                            .arg(formatMs(d.inputWait.max)));
        }
        html += "</table>";
        if (swap) {
            html += QObject::tr("<i>No presentation feedback from the compositor (X11): "
                                "latencies end at the buffer swap.</i><br>");
        }
    } else if (!s.displayType.isEmpty()) {
        html += QObject::tr("<b>Display</b>: %1, which does not measure its frames: "
                            "use the SDL display (<code>-display sdl</code>).<br>")
                    .arg(s.displayType);
    } else {
        html += QObject::tr("<b>Display</b>: this QEMU does not measure its frames; "
                            "Vitrine's QEMU does.<br>");
    }

    return html + hostDetails(s);
}

QString hostDetails(const Snapshot &s)
{
    QString html;

    if (s.threads) {
        html += QObject::tr("<b>QEMU on the host</b>") + "<table>";
        html += row(QObject::tr("Main loop"),
                    QObject::tr("%1 CPU").arg(formatPercent(s.mainLoop.cpu)),
                    QObject::tr("waited %1 of the time for a CPU, %2 per run")
                        .arg(formatPercent(s.mainLoop.wait),
                             formatMs(s.mainLoop.waitPerRunMs)));
        if (s.vcpus.threads) {
            html += row(QObject::tr("%n vCPU(s)", nullptr, s.vcpus.threads),
                        QObject::tr("%1 CPU").arg(formatPercent(s.vcpus.cpu)),
                        QObject::tr("waited %1 for a CPU").arg(formatPercent(s.vcpus.wait)));
        }
        if (s.others.threads) {
            html += row(QObject::tr("Other threads"),
                        QObject::tr("%1 CPU").arg(formatPercent(s.others.cpu)),
                        QObject::tr("%n thread(s): GPU, I/O, audio…", nullptr,
                                    s.others.threads));
        }
        html += "</table>";
    }

    if (s.kvm) {
        html += QObject::tr("<b>KVM</b>") + "<table>";
        html += row(QObject::tr("VM exits"), perSecond(s.exits),
                    QObject::tr("≈ %1 of a CPU at %2 µs each")
                        .arg(formatPercent(s.exits * kExitCostUs / 1e4)).arg(kExitCostUs));
        html += row(QObject::tr("Halt exits"), perSecond(s.haltExits));
        html += row(QObject::tr("Device exits"), perSecond(s.deviceExits),
                    QObject::tr("I/O and MMIO: emulated devices"));
        if (s.haltPollSuccess >= 0) {
            html += row(QObject::tr("Halt polling"),
                        QObject::tr("%1 successful")
                            .arg(formatPercent(100 * s.haltPollSuccess)),
                        QObject::tr("%1 of a CPU polling").arg(formatPercent(s.haltPollCpu)));
        }
        html += "</table>";
    }
    return html;
}

}
