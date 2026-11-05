// SPDX-License-Identifier: GPL-2.0-or-later
#include <QTest>

#include "core/qemuinfo.h"
#include "gtk/qemureference.h"

using namespace QemuReference;

/*
 * The GTK app's QEMU reference, in words: what a search finds and how it
 * ranks it, the line an entry adds, its documentation, and QEMU's
 * documentation as Pango markup.  No display needed.
 */
class TestQemuReference : public QObject
{
    Q_OBJECT

    static QemuInfo info()
    {
        QemuInfo i;
        QemuOptionDoc smp;
        smp.name = "smp";
        smp.synopsis = "-smp [[cpus=]n]";
        smp.help = "set the number of initial CPUs to 'n'";
        smp.details = "Simulate a SMP system with ``n`` CPUs.\n";
        smp.takesValue = true;
        QemuOptionDoc snapshot;
        snapshot.name = "snapshot";
        snapshot.help = "write to temporary files instead of disk image files";
        i.options = {smp, snapshot};
        QemuDeviceDoc vga;
        vga.name = "virtio-vga-gl";
        vga.desc = "Virtio GPU with OpenGL";
        vga.category = "Display devices";
        i.devices = {vga};
        i.properties.insert("virtio-vga-gl", {{"drm_native_context", "bool", "", "off"},
                                              {"hostmem", "size", "", "0"}});
        i.machines = {{"q35", "Standard PC (Q35 + ICH9, 2009)"}};
        return i;
    }

private slots:
    void search()
    {
        const QemuInfo i = info();

        /* the name first */
        QList<Group> groups = QemuReference::search(i, "smp");
        QCOMPARE(groups.size(), 1);
        QCOMPARE(groups.first().kind, Option);
        QCOMPARE(groups.first().hits.first().label, "-smp");

        /* properties by their own name, words as spaces */
        groups = QemuReference::search(i, "drm native");
        QCOMPARE(groups.size(), 1);
        QCOMPARE(groups.first().kind, Property);
        QCOMPARE(groups.first().hits.first().name, "virtio-vga-gl,drm_native_context");
        QCOMPARE(groups.first().hits.first().label, "drm_native_context");

        /* no search: all but the properties */
        groups = QemuReference::search(i, {});
        QStringList titles;
        for (const Group &g : groups) {
            titles << g.title;
        }
        QCOMPARE(titles, QStringList({"Options", "Devices", "Machines"}));
        /* a description finds it too, after the names */
        groups = QemuReference::search(i, "temporary");
        QCOMPARE(groups.value(0).hits.value(0).name, "snapshot");
    }

    void lines()
    {
        const QemuInfo i = info();

        QCOMPARE(lineFor(&i, Option, "smp"), "-smp ");
        QCOMPARE(lineFor(&i, Option, "snapshot"), "-snapshot");
        QCOMPARE(lineFor(&i, Device, "virtio-vga-gl"), "-device virtio-vga-gl");
        QCOMPARE(lineFor(&i, Property, "virtio-vga-gl,drm_native_context"),
                 "-device virtio-vga-gl,drm_native_context=on");
        QCOMPARE(lineFor(&i, Property, "virtio-vga-gl,hostmem"), "-device virtio-vga-gl,hostmem=");
        QCOMPARE(lineFor(&i, Machine, "q35"), "-machine q35");
    }

    void describe()
    {
        const QemuInfo i = info();
        bool loading = true;

        QList<Block> blocks = QemuReference::describe(i, Option, "smp", &loading);
        QVERIFY(!loading);
        QCOMPARE(blocks.first().type, Block::Heading);
        QCOMPARE(blocks.first().markup, "-smp");
        QVERIFY(blocks.value(1).type == Block::Code);
        QVERIFY(blocks.last().markup.contains("<tt>n</tt>"));

        /* a device's properties, or that they are coming */
        blocks = QemuReference::describe(i, Device, "virtio-vga-gl", &loading);
        QVERIFY(!loading);
        QVERIFY(blocks.last().markup.contains("<tt>hostmem</tt>"));
        QemuInfo without = i;
        without.properties.clear();
        QemuReference::describe(without, Device, "virtio-vga-gl", &loading);
        QVERIFY(loading);
    }

    /* rstToHtml()'s HTML, as Pango takes it */
    void fromHtml()
    {
        const QList<Block> blocks = QemuReference::fromHtml(
            "<p style=\"margin-left:0px\">A <b>bold</b> <code>x &lt; y</code> and "
            "<a href=\"https://qemu.org\">QEMU</a>&nbsp;<span>here</span></p>"
            "<pre style=\"margin-left:20px\">-smp 4\n-m 1G</pre>");

        QCOMPARE(blocks.size(), 2);
        QCOMPARE(blocks[0].type, Block::Text);
        QCOMPARE(blocks[0].markup, "A <b>bold</b> <tt>x &lt; y</tt> and "
                                   "<a href=\"https://qemu.org\">QEMU</a>&#160;here");
        QCOMPARE(blocks[1].type, Block::Code);
        QCOMPARE(blocks[1].indent, 4);
        QCOMPARE(blocks[1].markup, "-smp 4\n-m 1G");
    }
};

QTEST_GUILESS_MAIN(TestQemuReference)
#include "test_qemureference.moc"
