// SPDX-License-Identifier: GPL-2.0-or-later
#include "updatecheck.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTimeZone>

/* How many subjects of new commits a result keeps */
static const int kSubjects = 8;

UpdateCheck::UpdateCheck(QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this))
{
}

void UpdateCheck::check(const QList<Project> &projects)
{
    /* another server for the tests */
    const QString api = qEnvironmentVariable("QGM_GITHUB_API", "https://api.github.com");

    if (m_pending > 0) {
        return;
    }
    m_results.clear();
    for (const Project &project : projects) {
        if (project.commit.isEmpty()) {
            continue;
        }
        QNetworkRequest request(QUrl(QString("%1/repos/%2/compare/%3...%4")
                                         .arg(api, project.repository, project.commit,
                                              project.branch)));
        request.setRawHeader("Accept", "application/vnd.github+json");
        request.setHeader(QNetworkRequest::UserAgentHeader, "qemu-gui-manager");
        request.setTransferTimeout(15000);
        QNetworkReply *reply = m_network->get(request);
        m_pending++;
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, project]() { answered(reply, project); });
    }
    if (m_pending == 0) {
        emit finished({});
    }
}

void UpdateCheck::answered(QNetworkReply *reply, const Project &project)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    Result result = parse(project, reply->readAll());

    reply->deleteLater();
    /* the rate limit: 60 requests an hour without an account */
    if (status == 403 || status == 429) {
        const QByteArray reset = reply->rawHeader("x-ratelimit-reset");
        const QByteArray retry = reply->rawHeader("retry-after");
        result.limitedUntil = !reset.isEmpty()
                                  ? QDateTime::fromSecsSinceEpoch(reset.toLongLong(), QTimeZone::UTC)
                                  : QDateTime::currentDateTimeUtc().addSecs(
                                        retry.isEmpty() ? 3600 : retry.toLongLong());
    }
    if (status == 404) {
        /* a commit GitHub does not have: a local one */
        result.error = tr("GitHub does not have commit %1").arg(project.commit.left(12));
    } else if (reply->error() != QNetworkReply::NoError && result.newCommits < 0 &&
               result.error.isEmpty()) {
        result.error = reply->errorString();
    }
    m_results << result;
    if (--m_pending == 0) {
        emit finished(m_results);
    }
}

UpdateCheck::Result UpdateCheck::parse(const Project &project, const QByteArray &json)
{
    const QJsonObject answer = QJsonDocument::fromJson(json).object();
    const QString status = answer["status"].toString();
    /* oldest first */
    const QJsonArray commits = answer["commits"].toArray();
    Result result;

    result.project = project;
    result.url = answer["html_url"].toString();
    if (status.isEmpty()) {
        result.error = answer["message"].toString(tr("GitHub gave an answer of no use"));
        return result;
    }
    /* "ahead": the branch has commits the running one has not */
    result.newCommits = status == "ahead" || status == "diverged" ? answer["ahead_by"].toInt()
                                                                  : 0;
    result.head = commits.isEmpty() ? (status == "identical" ? project.commit : QString())
                                    : commits.last()["sha"].toString();
    for (qsizetype i = commits.size() - 1; i >= 0 && result.subjects.size() < kSubjects; i--) {
        result.subjects << commits[i]["commit"]["message"].toString().section('\n', 0, 0);
    }
    return result;
}

QJsonArray UpdateCheck::toJson(const QList<Result> &results)
{
    QJsonArray array;

    for (const Result &r : results) {
        array.append(QJsonObject{{"name", r.project.name},
                                 {"repository", r.project.repository},
                                 {"branch", r.project.branch},
                                 {"commit", r.project.commit},
                                 {"newCommits", r.newCommits},
                                 {"subjects", QJsonArray::fromStringList(r.subjects)},
                                 {"url", r.url},
                                 {"head", r.head},
                                 {"error", r.error}});
    }
    return array;
}

QList<UpdateCheck::Result> UpdateCheck::fromJson(const QJsonArray &json)
{
    QList<Result> results;

    for (const QJsonValue &v : json) {
        Result r;
        r.project = {v["name"].toString(), v["repository"].toString(), v["branch"].toString(),
                     v["commit"].toString()};
        r.newCommits = v["newCommits"].toInt(-1);
        for (const QJsonValue &subject : v["subjects"].toArray()) {
            r.subjects << subject.toString();
        }
        r.url = v["url"].toString();
        r.head = v["head"].toString();
        r.error = v["error"].toString();
        results << r;
    }
    return results;
}

QString UpdateCheck::checkoutCommit(const QString &dir)
{
    static const QRegularExpression sha("^[0-9a-f]{40}$");
    QFile head(dir + "/.git/HEAD");
    QString ref;

    if (!head.open(QIODevice::ReadOnly)) {
        return {};
    }
    ref = QString::fromLatin1(head.readAll()).trimmed();
    if (!ref.startsWith("ref: ")) {
        return sha.match(ref).hasMatch() ? ref : QString();
    }
    ref = ref.mid(5);

    QFile loose(dir + "/.git/" + ref);
    if (loose.open(QIODevice::ReadOnly)) {
        const QString commit = QString::fromLatin1(loose.readAll()).trimmed();
        return sha.match(commit).hasMatch() ? commit : QString();
    }
    /* git gc packs them: "<sha> refs/heads/master" lines */
    QFile packed(dir + "/.git/packed-refs");
    if (packed.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : packed.readAll().split('\n')) {
            if (line.endsWith(' ' + ref.toLatin1()) && sha.match(line.left(40)).hasMatch()) {
                return QString::fromLatin1(line.left(40));
            }
        }
    }
    return {};
}
