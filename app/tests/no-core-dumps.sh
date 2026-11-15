#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Every test runs with core dumps off (CMAKE_TEST_LAUNCHER).  Several start a
# real QEMU, and with virgl on radeonsi, host Mesa (26.2.6) crashes as that
# QEMU exits: its threaded context deletes a shader after the shader cache
# went.  Each run left dumps of them in /var/lib/systemd/coredump.  A crash
# still shows in ctest's output and in the journal.
ulimit -c 0
exec "$@"
