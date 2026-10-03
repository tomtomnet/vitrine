// SPDX-License-Identifier: GPL-2.0-or-later
#include "qemubuilder.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include "core/paths.h"
#include "core/updatecheck.h"

/* The options the build tree was configured with, to know when to redo it */
static const char kStamp[] = "/qgm-configure-args";

/*
 * sh -c SCRIPT sh DIR REF EXTRA PATCH...: checks out REF, or else the newest
 * of main and the last releases that all the patches apply to, then applies
 * them; if none takes them, main without them.  EXTRA is a folder whose
 * *.patch files come after the PATCHes, if it exists: those the QEMU branch
 * asks for.  ../patched records the commit and the patches, to leave the
 * source alone when they are those of the last build: rewriting the patched
 * files makes ninja rebuild.
 */
static const char kApplyPatches[] = R"sh(cd "$1" || exit 1
want=$2
extra=$3
shift 3
if [ -d "$extra" ]; then
    for p in "$extra"/*.patch; do
        [ -f "$p" ] && set -- "$@" "$p"
    done
fi
stamp=../patched
hashes=$(cat "$@" </dev/null | sha256sum | cut -d' ' -f1)
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
if [ -n "$want" ]; then
    refs=$want
else
    refs="origin/HEAD $(git tag --sort=-creatordate | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' | head -n 5)"
fi
for ref in $refs; do
    if git rev-parse -q --verify "origin/$ref" >/dev/null; then
        ref=origin/$ref
    fi
    commit=$(git rev-parse -q --verify "$ref^{commit}") || continue
    if [ "$(cat "$stamp" 2>/dev/null)" = "$commit $hashes" ]; then
        echo "virglrenderer $ref, patched already"
        exit 0
    fi
    # the patches against that commit, without touching the checkout
    GIT_INDEX_FILE=$tmp/index git read-tree "$commit" || exit 1
    ok=1
    for p in "$@"; do
        GIT_INDEX_FILE=$tmp/index git apply --cached --check "$p" 2>/dev/null || { ok=0; break; }
    done
    if [ "$ok" = 1 ]; then
        rm -f "$stamp"
        git checkout -q -f --detach "$commit" && git clean -q -f -d -x || exit 1
        for p in "$@"; do
            git apply "$p" || exit 1
        done
        echo "$commit $hashes" > "$stamp"
        echo "virglrenderer $ref, with $# patch(es)"
        exit 0
    fi
    echo "The patches do not apply to $ref"
done
[ -z "$want" ] || exit 1
echo "WARNING: the patches apply to none of $refs: building main without them"
commit=$(git rev-parse origin/HEAD^{commit}) || exit 1
if [ "$(cat "$stamp" 2>/dev/null)" != "$commit none" ]; then
    rm -f "$stamp"
    git checkout -q -f --detach "$commit" && git clean -q -f -d -x || exit 1
    echo "$commit none" > "$stamp"
fi
)sh";

/*
 * sh -c SCRIPT sh SRC BUILD PREFIX VENUS RENDERER... -- MESON-ARG...:
 * configures virglrenderer with the renderers and Venus it can build.
 * BUILD/qgm-options records them: the same options need no configuring,
 * other ones a new build folder, since reconfiguring checks them against
 * the choices of the first configuration.
 */
static const char kConfigureVirgl[] = R"sh(src=$1 build=$2 prefix=$3 venus=$4
shift 4
renderers=
while [ $# -gt 0 ] && [ "$1" != -- ]; do
    if grep -q "'$1'" "$src/meson_options.txt"; then
        renderers="$renderers${renderers:+,}$1"
    else
        echo "No $1 renderer in this virglrenderer"
    fi
    shift
done
[ $# -gt 0 ] && shift
if [ "$venus" = true ] && ! pkg-config --exists vulkan; then
    echo "WARNING: no Vulkan headers, so no Venus (sudo dnf install vulkan-loader-devel)"
    venus=false
fi
echo "Native context renderers: ${renderers:-none}; Venus: $venus"
options="$prefix $renderers $venus $*"
if [ -f "$build/build.ninja" ] && [ "$(cat "$build/qgm-options" 2>/dev/null)" = "$options" ]; then
    echo "Configured already"
    exit 0
fi
rm -rf "$build"
meson setup "$build" "$src" --prefix="$prefix" --libdir=lib --buildtype=release \
    -Ddrm-renderers="$renderers" -Dvenus="$venus" "$@" || exit 1
echo "$options" > "$build/qgm-options"
)sh";

/* Writes the resource @resource to @file; the error, if any */
static QString writeOut(const QString &resource, const QString &file)
{
    QFile in(resource);
    QFile out(file);

    if (!in.open(QIODevice::ReadOnly)) {
        return QString("%1: %2").arg(resource, in.errorString());
    }
    const QByteArray data = in.readAll();
    if (!QDir().mkpath(QFileInfo(file).absolutePath()) ||
        !out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(data) != data.size() ||
        !out.flush()) {
        return QString("%1: %2").arg(file, out.errorString());
    }
    return {};
}

QString QemuBuilder::defaultSourceDir()
{
    return Paths::dataDir() + "/qemu";
}

QString QemuBuilder::defaultUrl()
{
    return "https://github.com/tomtomnet/qemu-gui.git";
}

QString QemuBuilder::defaultBranch()
{
    return "master";
}

QStringList QemuBuilder::defaultConfigureArgs()
{
    return {"--target-list=" + Paths::hostArch() + "-softmmu"};
}

QString QemuBuilder::buildDir(const QString &sourceDir)
{
    return sourceDir + "/build-qgm";
}

QString QemuBuilder::builtBranch(const QString &sourceDir)
{
    QFile stamp(buildDir(sourceDir) + "/qgm-built-branch");

    return stamp.open(QIODevice::ReadOnly) ? QString::fromUtf8(stamp.readAll()).trimmed()
                                           : QString();
}

QString QemuBuilder::branchVirglPatches(const QString &sourceDir)
{
    return sourceDir + "/contrib/qemu-gui/virglrenderer";
}

QStringList QemuBuilder::parseHeads(const QByteArray &lsRemote)
{
    QStringList heads;

    for (const QByteArray &line : lsRemote.split('\n')) {
        const qsizetype at = line.indexOf("\trefs/heads/");
        if (at > 0) {
            heads << QString::fromUtf8(line.mid(at + 12)).trimmed();
        }
    }
    return heads;
}

QString QemuBuilder::builtCommit(const QString &sourceDir)
{
    QFile stamp(buildDir(sourceDir) + "/qgm-built-commit");

    return stamp.open(QIODevice::ReadOnly) ? QString::fromLatin1(stamp.readAll()).trimmed()
                                           : QString();
}

QString QemuBuilder::binary(const QString &sourceDir)
{
    return buildDir(sourceDir) + '/' + Paths::qemuSystemName();
}

QString QemuBuilder::defaultVirglDir()
{
    return Paths::dataDir() + "/virglrenderer";
}

QString QemuBuilder::defaultVirglUrl()
{
    return "https://gitlab.freedesktop.org/virgl/virglrenderer.git";
}

QString QemuBuilder::xePatchUrl()
{
    return "https://raw.githubusercontent.com/cmspam/xe-native-context-enablement/master/"
           "virglrenderer-xe-native-context.patch";
}

QString QemuBuilder::amdgpuWcPatch()
{
    return ":/patches/virglrenderer-amdgpu-force-wc.patch";
}

QStringList QemuBuilder::allRenderers()
{
    return {"amdgpu-experimental", "i915-experimental", "xe-experimental", "msm", "asahi",
            "panfrost-experimental"};
}

QemuBuilder::Virgl QemuBuilder::defaultVirgl()
{
    Virgl virgl;

    virgl.enabled = true;
    virgl.dir = defaultVirglDir();
    virgl.url = defaultVirglUrl();
    virgl.patches = {xePatchUrl(), amdgpuWcPatch()};
    virgl.renderers = allRenderers();
    virgl.venus = true;
    return virgl;
}

QString QemuBuilder::virglLibDir(const QString &virglDir)
{
    return virglDir + "/install/lib";
}

QString QemuBuilder::loadedVirgl(const QString &binary)
{
    static const QRegularExpression line("libvirglrenderer\\.so[.\\d]*\\s+=>\\s+(\\S+)");
    const QFileInfo fi(binary);

    /* built with modules, virtio-gpu-gl and virglrenderer come as one */
    for (const QString &file : {fi.filePath(), fi.dir().filePath("hw-display-virtio-gpu-gl.so")}) {
        QProcess ldd;

        if (!QFileInfo::exists(file)) {
            continue;
        }
        ldd.start("ldd", {file});
        if (!ldd.waitForFinished(5000)) {
            continue;
        }
        const QRegularExpressionMatch m =
            line.match(QString::fromLocal8Bit(ldd.readAllStandardOutput()));
        if (m.hasMatch()) {
            return QFileInfo(m.captured(1)).canonicalFilePath();
        }
    }
    return {};
}

int QemuBuilder::defaultJobs()
{
    static const QRegularExpression total("MemTotal:\\s+(\\d+) kB");
    QFile meminfo("/proc/meminfo");
    int memoryGiB = 4;

    if (meminfo.open(QIODevice::ReadOnly)) {
        const QRegularExpressionMatch m = total.match(QString::fromLatin1(meminfo.readAll()));
        if (m.hasMatch()) {
            memoryGiB = int(m.captured(1).toLongLong() >> 20);
        }
    }
    return qBound(1, qMin(QThread::idealThreadCount(), memoryGiB - 1), 64);
}

QemuBuilder::QemuBuilder(QObject *parent) : QObject(parent)
{
}

QemuBuilder::~QemuBuilder()
{
    if (m_process) {
        m_process->disconnect(this);
        m_process->kill();
        m_process->waitForFinished(3000);
    }
}

void QemuBuilder::start(const Options &options)
{
    const QString build = buildDir(options.sourceDir);
    const int jobs = options.jobs > 0 ? options.jobs : defaultJobs();
    QStringList env;

    if (isRunning()) {
        return;
    }
    m_options = options;
    m_configureArgs = options.configureArgs;
    m_steps.clear();
    m_cancelled = false;

    /* the branch first: it may ask for virglrenderer patches of its own */
    if (!QFileInfo::exists(options.sourceDir + "/.git")) {
        m_steps << Step{tr("Downloading QEMU"), "git",
                        {"clone", "--depth", "1", "--branch", options.branch, options.url,
                         options.sourceDir}, {}};
    } else if (options.update) {
        m_steps << Step{tr("Downloading the changes"), "git",
                        {"fetch", "--depth", "1", options.url, options.branch},
                        options.sourceDir};
        m_steps << Step{tr("Updating the sources"), "git", {"reset", "--hard", "FETCH_HEAD"},
                        options.sourceDir};
    }

    if (options.virgl.enabled) {
        const QString lib = virglLibDir(options.virgl.dir);
        const QString pkgConfigPath = qEnvironmentVariable("PKG_CONFIG_PATH");

        addVirglSteps(options.virgl, jobs, branchVirglPatches(options.sourceDir));
        /* QEMU builds against it, and loads it from there */
        env << "PKG_CONFIG_PATH=" + lib + "/pkgconfig" +
                   (pkgConfigPath.isEmpty() ? QString() : ':' + pkgConfigPath);
        m_configureArgs << "--extra-ldflags=-Wl,-rpath," + lib;
    }

    Step configure{tr("Configuring"), options.sourceDir + "/configure", m_configureArgs, build,
                   true, env};
    /* decided when it comes: virglrenderer's patches, applied just before, count */
    configure.skip = [this, build]() {
        QFile stamp(build + kStamp);
        return QFileInfo::exists(build + "/build.ninja") && stamp.open(QIODevice::ReadOnly) &&
               QString::fromUtf8(stamp.readAll()) == configureStamp();
    };
    m_steps << configure;
    m_steps << Step{tr("Compiling"), "ninja",
                    {"-j", QString::number(jobs), Paths::qemuSystemName(), "qemu-img"},
                    build, false, env, {}, binary(options.sourceDir)};
    runNext();
}

/*
 * What the build tree is configured for: the options, and the patches of
 * the virglrenderer they build against, which decide what configure finds
 * in it (virgl_renderer_resource_set_guest_dmabuf(), say)
 */
QString QemuBuilder::configureStamp() const
{
    QString stamp = m_configureArgs.join('\n');

    if (m_options.virgl.enabled) {
        QFile patched(m_options.virgl.dir + "/patched");
        if (patched.open(QIODevice::ReadOnly)) {
            stamp += "\nvirglrenderer " + QString::fromUtf8(patched.readAll()).trimmed();
        }
    }
    return stamp;
}

void QemuBuilder::addVirglSteps(const Virgl &virgl, int jobs, const QString &branchPatches)
{
    const QString src = virgl.dir + "/src";
    const QString build = virgl.dir + "/build";
    const QString patchDir = virgl.dir + "/patches";
    QStringList patches, meson;

    if (!QFileInfo::exists(src + "/.git")) {
        m_steps << Step{tr("Downloading virglrenderer"), "git",
                        {"clone", "--quiet", virgl.url, src}, {}};
    } else {
        m_steps << Step{tr("Downloading the changes to virglrenderer"), "git",
                        {"fetch", "--quiet", "--tags", "--force", "origin"}, src};
    }
    for (qsizetype i = 0; i < virgl.patches.size(); i++) {
        const QString &patch = virgl.patches[i];
        const bool resource = patch.startsWith(":/");
        const QString name = resource ? QFileInfo(patch).fileName() : QUrl(patch).fileName();
        const QString file = QString("%1/%2-%3").arg(patchDir).arg(i + 1)
                                 .arg(name.isEmpty() ? QString("patch") : name);

        if (resource) {
            /* the patch script takes files */
            const QString error = writeOut(patch, file);
            if (!error.isEmpty()) {
                m_steps << Step{tr("Writing %1").arg(name), "sh",
                                {"-c", "echo \"$1\" >&2; exit 1", "sh", error}, patchDir};
            }
            patches << file;
            continue;
        }
        if (!patch.contains("://")) {
            patches << QFileInfo(patch).absoluteFilePath();
            continue;
        }
        patches << file;
        m_steps << Step{tr("Downloading %1").arg(name), "curl",
                        {"--fail", "--silent", "--show-error", "--location", "--retry", "2",
                         "--output", patches.last(), patch}, patchDir};
    }
    m_steps << Step{tr("Patching virglrenderer"), "sh",
                    QStringList{"-c", kApplyPatches, "sh", src, virgl.ref, branchPatches} +
                        patches,
                    src, false, {},
                    "git apply " + patches.join(' ') + " [" + branchPatches +
                        "/*.patch]  # on " +
                        (virgl.ref.isEmpty() ? "main, else the newest release taking them"
                                             : virgl.ref)};

    meson << "-c" << kConfigureVirgl << "sh" << src << build << virgl.dir + "/install"
          << (virgl.venus ? "true" : "false") << virgl.renderers << "--" << virgl.mesonArgs;
    m_steps << Step{tr("Configuring virglrenderer"), "sh", meson, virgl.dir, false, {},
                    QString("meson setup %1 %2 --prefix=%3/install -Ddrm-renderers=%4 "
                            "-Dvenus=%5  # the renderers this virglrenderer has")
                        .arg(build, src, virgl.dir, virgl.renderers.join(','),
                             virgl.venus ? "true" : "false")};
    m_steps << Step{tr("Compiling virglrenderer"), "ninja",
                    {"-C", build, "-j", QString::number(jobs), "install"}, virgl.dir};
}

void QemuBuilder::cancel()
{
    if (!m_process) {
        return;
    }
    m_cancelled = true;
    m_process->terminate();
    QTimer::singleShot(5000, m_process, &QProcess::kill);
}

void QemuBuilder::runNext()
{
    if (m_steps.isEmpty()) {
        /* for the update check: what the binary is built from */
        QFile stamp(buildDir(m_options.sourceDir) + "/qgm-built-commit");
        if (stamp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            stamp.write(UpdateCheck::checkoutCommit(m_options.sourceDir).toLatin1() + '\n');
        }
        QFile branch(buildDir(m_options.sourceDir) + "/qgm-built-branch");
        if (branch.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            branch.write(m_options.branch.toUtf8() + '\n');
        }
        emit finished({});
        return;
    }

    const Step step = m_steps.first();
    if (step.skip && step.skip()) {
        m_steps.removeFirst();
        runNext();
        return;
    }
    if (!step.dir.isEmpty()) {
        QDir().mkpath(step.dir);
    }
    emit stepStarted(step.description);
    emit output("$ " + (step.shown.isEmpty() ? step.program + ' ' + step.args.join(' ')
                                             : step.shown) + '\n');

    m_line.clear();
    m_productTime = step.product.isEmpty() ? QDateTime() : QFileInfo(step.product).lastModified();
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setWorkingDirectory(step.dir);
    if (!step.env.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const QString &var : step.env) {
            env.insert(var.section('=', 0, 0), var.section('=', 1));
        }
        m_process->setProcessEnvironment(env);
    }
    connect(m_process, &QProcess::readyRead, this, [this]() {
        const QString text = QString::fromLocal8Bit(m_process->readAll());
        emit output(text);
        parseProgress(text);
    });
    connect(m_process, &QProcess::errorOccurred, this, [this, step](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            const QString error = m_process->errorString();
            m_process->deleteLater();
            m_process = nullptr;
            emit finished(tr("Cannot run %1: %2").arg(step.program, error));
        }
    });
    connect(m_process, &QProcess::finished, this,
            [this, step](int code, QProcess::ExitStatus status) {
        m_process->deleteLater();
        m_process = nullptr;
        if (m_cancelled) {
            emit finished(tr("Cancelled"));
            return;
        }
        if (status != QProcess::NormalExit || code != 0) {
            emit finished(tr("%1 failed").arg(step.description));
            return;
        }
        /* ninja stops quietly after its first line ("[1/7] Generating qemu-version.h") */
        if (m_productTime.isValid() && QFileInfo(step.product).lastModified() == m_productTime) {
            emit output(tr("%1 was up to date: nothing to compile.\n")
                            .arg(QFileInfo(step.product).fileName()));
        }
        if (step.configure) {
            QFile stamp(buildDir(m_options.sourceDir) + kStamp);
            if (stamp.open(QIODevice::WriteOnly)) {
                stamp.write(configureStamp().toUtf8());
            }
        }
        m_steps.removeFirst();
        runNext();
    });
    m_process->start(step.program, step.args);
}

void QemuBuilder::parseProgress(const QString &text)
{
    static const QRegularExpression status("^\\[(\\d+)/(\\d+)\\]");

    m_line += text;
    qsizetype end;
    while ((end = m_line.indexOf('\n')) >= 0) {
        const QRegularExpressionMatch m = status.match(m_line.left(end));
        if (m.hasMatch()) {
            emit progress(m.captured(1).toInt(), m.captured(2).toInt());
        }
        m_line.remove(0, end + 1);
    }
}
