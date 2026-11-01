// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

struct HostGpu;

/*
 * gpu-input-awake, an optional component (host/gpu-input-awake/README.md):
 * the research's root service that wakes the GPU from its deep idle at each
 * key or pointer event - an Intel xe GPU's GT out of gt-c6, an AMD GPU's
 * GFXOFF held off.  On a dedicated GPU it is what a clock floor is on an AMD
 * APU, where the firmware gives the CPU the power first and the GPU idles
 * low: a floor does nothing on a dedicated GPU (measured on an Arc Pro B50
 * and an RX 6900 XT), the wake does (bare keys ~9.0 -> 7.8 ms on the B50, ~2
 * W while typing, nothing at rest).  Vitrine installs its pinned copy where
 * the research's setup puts it, with the administrator's password, and
 * removes it the same way; once installed it runs from boot, all the time.
 */
namespace GpuInputAwake {

/* What host tuning gives a GPU */
enum class Kind {
    ClockFloor,     // an AMD APU: the clock floor while VMs run (HostSettings)
    InputWake,      // a dedicated GPU it serves: Intel xe, or AMD without an APU's metrics
    None,           // another (i915, NVIDIA...): neither
};
/*
 * @gpu's, from sysfs under @sysRoot (HostGpus's): amdgpu describes an APU's
 * metrics with the APU layouts (gpu_metrics v2 and v3), a discrete GPU's
 * with v1
 */
Kind kind(const HostGpu &gpu, const QString &sysRoot = {});
/* The GPUs of @gpus it serves */
QList<HostGpu> served(const QList<HostGpu> &gpus, const QString &sysRoot = {});

/* Where its files are looked for: "", this host's /; $VITRINE_AWAKE_ROOT
   names a fake root (tests: pkexec passes no environment to install.sh) */
QString root();
/* Its files under @root */
QString scriptPath(const QString &root = GpuInputAwake::root());
QString unitPath(const QString &root = GpuInputAwake::root());
QString unitName();

/* The pinned copy and its installer: <host>/gpu-input-awake, "" if this
   installation has none ($VITRINE_AWAKE_DIR names another, for tests) */
QString sourceDir();

enum class Installed {
    No,         // neither file
    Vitrine,    // both, byte for byte the pinned copy of @source
    Other,      // both, not Vitrine's version: the research's own install, an older one
    Partial,    // one of the two
};
Installed installed(const QString &source, const QString &root = GpuInputAwake::root());

/* "pkexec bash <source>/install.sh install" or "... remove" */
QStringList command(const QString &source, bool install);

/* What came of an install or a removal */
enum class Result {
    Done,
    Cancelled,  // the password dialog was dismissed, or the password refused
    Failed,     // with why
};
/*
 * The output of install.sh, or of pkexec when it did not run it, and how
 * it ended (@code: the exit status, -1 if it crashed): the result, and why
 * it failed in @error
 */
Result result(int code, const QString &output, QString *error);

/*
 * Installs (true) or removes it: command() run, @done told in @context's
 * thread.  One at a time: false (and nothing run) while one runs.
 */
bool run(QObject *context, bool install,
         const std::function<void(Result result, const QString &error)> &done,
         const QStringList &commandLine = {});
bool running();

/*
 * The unit's state for systemctl: is-active ("active", "inactive",
 * "failed"...) and is-enabled ("enabled", "disabled"...), "" when systemctl
 * did not say; @done in @context's thread
 */
void serviceState(QObject *context,
                  const std::function<void(const QString &active, const QString &enabled)> &done);

}
