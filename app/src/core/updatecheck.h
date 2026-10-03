// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QList>
#include <QObject>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

/*
 * Whether GitHub has commits newer than those this computer runs: of
 * qemu-gui-manager, and of the qemu-gui QEMU that the manager builds.  One
 * anonymous request to the compare API of GitHub for each.
 */
class UpdateCheck : public QObject
{
    Q_OBJECT

public:
    struct Project {
        QString name;           // e.g. qemu-gui-manager
        QString repository;     // e.g. tomtomnet/qemu-gui-manager
        QString branch;         // e.g. main
        QString commit;         // the one running
    };
    struct Result {
        Project project;
        int newCommits = -1;    // -1 when it could not tell
        QStringList subjects;   // of the newest commits, newest first
        QString url;            // the page of the changes
        QString head;           // the newest commit of the branch then
        QString error;
        /* GitHub had too many requests from here: none before then */
        QDateTime limitedUntil;
    };

    explicit UpdateCheck(QObject *parent = nullptr);

    /* Then finished(), with a result for each project that has a commit */
    void check(const QList<Project> &projects);
    bool isChecking() const { return m_pending > 0; }

    /* The commit a git checkout is at, from its files; empty when unknown */
    static QString checkoutCommit(const QString &dir);
    /* The answer of GitHub to the compare of @project's commit to its branch */
    static Result parse(const Project &project, const QByteArray &json);
    /* To keep the results until the next check */
    static QJsonArray toJson(const QList<Result> &results);
    static QList<Result> fromJson(const QJsonArray &json);

signals:
    void finished(const QList<UpdateCheck::Result> &results);

private:
    void answered(QNetworkReply *reply, const Project &project);

    QNetworkAccessManager *m_network;
    QList<Result> m_results;
    int m_pending = 0;
};
