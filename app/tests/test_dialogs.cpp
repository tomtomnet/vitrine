// SPDX-License-Identifier: GPL-2.0-or-later
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStyleFactory>
#include <QTemporaryDir>

#include <algorithm>

#include "core/paths.h"
#include "core/vmstore.h"
#include "scales.h"
#include "ui/cardupdatedialog.h"
#include "ui/clonedialog.h"
#include "ui/guesttoolsdialog.h"
#include "ui/newvmdialog.h"
#include "ui/qemubuilddialog.h"
#include "ui/textdialog.h"

/*
 * The dialogs at every scale (scales.h): as high as their content at the
 * width they open with, their wrapped notes no higher than their text,
 * the argument lines of the 3D card's update all shown however they wrap,
 * and in their buttons, icons on all or on none, as the style has them on
 * the standard ones.
 */

/* A Linux VM made while Fedora's QEMU stood in for vitrine's: its card lacks vitrine's settings */
static const char kVm[] = "-name Desktop\n"
                          "#guest linux,desktop=kde\n"
                          "-machine q35,memory-backend=mem\n"
                          "-accel kvm\n"
                          "-object memory-backend-memfd,id=mem,size=4G,share=on\n"
                          "-vga none\n"
                          "-device virtio-gpu-gl-pci,hostmem=4G,blob=on,venus=off\n"
                          "-display dbus,p2p=yes,gl=on\n"
                          "-drive file=disk.qcow2,format=qcow2,if=virtio\n";

class TestDialogs : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    VmStore *m_store = nullptr;

    bool write(const QString &path, const QByteArray &text, bool executable = false)
    {
        QFile f(path);

        if (!QDir().mkpath(QFileInfo(path).path()) || !f.open(QIODevice::WriteOnly) ||
            f.write(text) < 0) {
            return false;
        }
        f.close();
        return !executable || f.setPermissions(QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    }

    Vm *vm() const { return m_store->vms().value(0); }

    /* The window of @className that a function opened */
    static QWidget *opened(const char *className)
    {
        for (QWidget *top : QApplication::topLevelWidgets()) {
            if (top->inherits(className)) {
                return top;
            }
        }
        return nullptr;
    }

    /* Each dialog, made, shown and checked by @check, then deleted */
    QStringList eachDialog(const std::function<QStringList(QWidget *)> &check)
    {
        QStringList wrong;
        const QList<std::pair<QString, std::function<QWidget *()>>> dialogs = {
            {"New VM", [this]() { return new NewVmDialog(m_store); }},
            {"Clone", [this]() { return new CloneDialog(m_store, vm()); }},
            {"3D card", [this]() { return new CardUpdateDialog(vm()); }},
            {"QEMU build", []() { return new QemuBuildDialog; }},
            {"Command line", []() {
                 TextDialog::showText(nullptr, "Command Line", "qemu-system-x86_64 -m 4G");
                 return opened("TextDialog");
             }},
            {"Guest tools", [this]() {
                 GuestToolsDialog::run(nullptr, vm());
                 return opened("GuestToolsDialog");
             }},
        };
        for (const auto &[name, make] : dialogs) {
            QWidget *dialog = make();
            if (!dialog) {
                wrong << name + ": not made";
                continue;
            }
            dialog->setAttribute(Qt::WA_DeleteOnClose, false);
            Scales::settle(dialog);
            for (const QString &w : check(dialog)) {
                wrong << name + ": " + w;
            }
            delete dialog;
        }
        return wrong;
    }

private slots:
    void initTestCase()
    {
        Scales::quieter();
        Scales::bigScreen();
        QVERIFY(m_tmp.isValid());
        if (Paths::hostArch() != "x86_64") {
            QSKIP("the 3D card of Linux on a PC");
        }
        QIcon::setThemeSearchPaths(QIcon::themeSearchPaths() << "/usr/share/icons");
        if (QDir("/usr/share/icons/breeze").exists()) {
            QIcon::setThemeName("breeze");
        }
        /* settings and stack in here: vitrine's QEMU not built, no guest tools */
        qputenv("XDG_DATA_HOME", m_tmp.filePath("data").toUtf8());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_tmp.filePath("cache").toUtf8());
        /* the system's QEMU: Fedora's, without native context */
        QVERIFY(write(m_tmp.filePath("bin/" + Paths::qemuSystemName()),
                      "#!/bin/sh\n"
                      "case \"$1 $2\" in\n"
                      "'-device virtio-gpu-gl-pci,help')\n"
                      "  echo 'virtio-gpu-gl-pci options:'\n"
                      "  echo '  blob=<bool>'\n"
                      "  echo '  hostmem=<size>'\n"
                      "  echo '  venus=<bool>' ;;\n"
                      "esac\n",
                      true));
        qputenv("PATH", m_tmp.filePath("bin").toUtf8());
        Paths::setQemuBinary({});
        QVERIFY(write(m_tmp.filePath("vms/desktop/vm.args"), kVm));
        m_store = new VmStore(m_tmp.filePath("vms"), this);
        QVERIFY(vm());
    }

    void sizes_data()
    {
        QTest::addColumn<QString>("style");
        QTest::newRow("fusion") << "Fusion";
        if (QStyleFactory::keys().contains("Breeze", Qt::CaseInsensitive)) {
            QTest::newRow("breeze") << "Breeze";
        }
    }

    /* As high as their content at the width they open with, nothing stretched */
    void sizes()
    {
        QFETCH(QString, style);
        QApplication::setStyle(style);
        Scales::sweep([this](int) {
            return eachDialog([](QWidget *dialog) {
                QStringList wrong;
                const int need = dialog->heightForWidth(dialog->width());
                /* those that open at a size of their own have editors to fill it */
                const QList<QPlainTextEdit *> editors = dialog->findChildren<QPlainTextEdit *>();
                const bool fills = std::any_of(editors.begin(), editors.end(), [](auto *e) {
                    return e->minimumHeight() != e->maximumHeight();
                });
                if (!fills && need > 0 && dialog->height() > need + 1) {
                    wrong << QString("%1 high for its content's %2").arg(dialog->height())
                                 .arg(need);
                }
                for (QLabel *label : dialog->findChildren<QLabel *>()) {
                    if (label->wordWrap() && label->isVisible() && !label->text().isEmpty() &&
                        label->height() > label->heightForWidth(label->width()) + 1) {
                        wrong << QString("a note %1 high for its text's %2")
                                     .arg(label->height())
                                     .arg(label->heightForWidth(label->width()));
                    }
                    /* a line of nothing between the others */
                    if (label->isVisible() && label->text().isEmpty() &&
                        label->pixmap().isNull() && label->height() > 0) {
                        wrong << QString("an empty line of %1 pixels").arg(label->height());
                    }
                }
                return wrong;
            });
        });
    }

    void cardLines_data()
    {
        sizes_data();
    }

    /* The argument lines all shown, however they wrap, and no more room than they need */
    void cardLines()
    {
        QFETCH(QString, style);
        QApplication::setStyle(style);
        Scales::sweep([this](int n) {
            QStringList wrong;
            CardUpdateDialog dialog(vm());
            Scales::settle(&dialog);
            /* narrower, then wider: the lines wrap otherwise */
            for (int width : {dialog.minimumWidth(), dialog.minimumWidth() + 40 + n % 50,
                              dialog.width() + 200}) {
                dialog.resize(width, dialog.height());
                Scales::settle(&dialog);
                for (const char *name : {"before", "after"}) {
                    auto *box = dialog.findChild<QPlainTextEdit *>(name);
                    if (!box) {
                        wrong << QString("no %1").arg(name);
                        continue;
                    }
                    if (box->verticalScrollBar()->maximum() > 0) {
                        wrong << QString("%1 at %2: scrolls, %3 lines hidden").arg(name)
                                     .arg(width).arg(box->verticalScrollBar()->maximum());
                    }
                    /* no line's room more than needed */
                    const qreal line = QFontMetricsF(box->font()).lineSpacing();
                    const qreal lines = box->document()->lineCount();
                    const qreal room = box->viewport()->height() -
                                       2 * box->document()->documentMargin();
                    if (room > (lines + 1) * line) {
                        wrong << QString("%1 at %2: room for %3 lines, %4 shown").arg(name)
                                     .arg(width).arg(room / line, 0, 'f', 1).arg(lines);
                    }
                }
            }
            return wrong;
        });
    }

    void buttons_data()
    {
        sizes_data();
    }

    /* Icons on all of a dialog's buttons, or on none, as the style has them */
    void buttons()
    {
        QFETCH(QString, style);
        QApplication::setStyle(style);
        const QStringList wrong = eachDialog([](QWidget *dialog) {
            QStringList wrong;
            for (QDialogButtonBox *box : dialog->findChildren<QDialogButtonBox *>()) {
                QStringList with, without;
                for (QAbstractButton *button : box->buttons()) {
                    if (!button->isHidden()) {
                        (button->icon().isNull() ? without : with) << button->text().remove('&');
                    }
                }
                if (!with.isEmpty() && !without.isEmpty()) {
                    wrong << QString("icons on %1, not on %2").arg(with.join(", "),
                                                                   without.join(", "));
                }
            }
            return wrong;
        });
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join("; ")));
    }
};

QTEST_MAIN(TestDialogs)
#include "test_dialogs.moc"
