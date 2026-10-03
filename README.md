# vitrine

A Qt application for running Linux desktop virtual machines with QEMU/KVM.
The guest uses the host's GPU through virtio-gpu DRM native context, and the
app shows the guest's display through QEMU's D-Bus display.

Vitrine builds its own QEMU: the qemu-gui fork of QEMU and upstream
virglrenderer, each at the commit `host/versions.conf` pins, with the patches
in `host/patches`. The guest needs Mesa built with native context for the
host's GPU: see [docs/guest-mesa.md](docs/guest-mesa.md).

Early development: there is nothing to install or run yet.

## Made by AI

The code, patches and documentation in this repository are written by AI
(Anthropic's Claude, through Claude Code) under a human's direction. The
patches are not upstream submissions; QEMU, for one, does not accept
AI-generated contributions.

## License

GPL-2.0-or-later, see [LICENSE](LICENSE). Patches to other projects are under
the licenses of the files they change, and files with their own license header
keep that license.
