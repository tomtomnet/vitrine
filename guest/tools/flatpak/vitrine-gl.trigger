#!/bin/sh
# vitrine guest tools: run by flatpak after each transaction that installs,
# updates or removes an app, with the path of its installation ($1), the one
# place its sandbox lets it write.  The
# mark that vitrine-flatpak-gl.path watches in the system installation: a
# runtime installed now gets vitrine's Mesa for Flatpak apps too
# (/usr/libexec/vitrine-guest-tools/flatpak-gl sync).
date +%s > "$1/.vitrine-gl-changed" 2> /dev/null || :
