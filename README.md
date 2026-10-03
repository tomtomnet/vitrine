# vitrine

A Qt application for running Linux desktop virtual machines with QEMU/KVM.
The guest uses the host's GPU through virtio-gpu DRM native context, and the
app shows the guest's display through QEMU's D-Bus display.

QEMU, virglrenderer and the guest's graphics stack (virtio-gpu driver, Mesa,
KWin) are used at pinned upstream versions, with the patches vitrine needs
kept in this repository.

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
