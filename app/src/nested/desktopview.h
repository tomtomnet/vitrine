// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QString>

#include <functional>

class QWidget;

/*
 * The guest's desktop as the console shows it (its Desktop view): what
 * VmConsole needs of NestedView, which tests replace with a stand-in, as
 * the real one needs Qt's Wayland platform and a guest.
 */
class DesktopView : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    /* The socket QEMU's cross-domain connects to, which the view serves */
    virtual QString socket() const = 0;
    /* The embedded view; owned by the view */
    virtual QWidget *widget() const = 0;
    virtual bool isFullScreen() const = 0;
    virtual void setFullScreen(bool on) = 0;
    /* Off: the guest gets no keys, and vitrine's shortcuts work */
    virtual void setInputEnabled(bool on) = 0;
    /* The guest gets the keys now */
    virtual bool hasKeyboard() const = 0;
    /* Qt's focus to the view: the keys to the guest */
    virtual void focus() = 0;
    /* The guest's connections to the socket, and its window */
    virtual int connections() const = 0;
    virtual bool isMapped() const = 0;

    /* Makes the console's views: NestedView::create() but in tests */
    using Factory = std::function<DesktopView *(const QString &socket, QString *error,
                                                QObject *parent)>;

Q_SIGNALS:
    void connectionsChanged(int count);
    void mappedChanged(bool mapped);
    /* The guest asks for full screen, or a window (its Ctrl+Alt+F) */
    void fullScreenRequested(bool on);
    /* Ctrl+Alt+F while no guest window is there to ask */
    void fullScreenToggled();
    void fullScreenChanged(bool on);
    /* hasKeyboard() changed */
    void keyboardChanged();
};
