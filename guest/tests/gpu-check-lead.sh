#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The boot check's file of KWin's lone-frame lead for the guest's own
# sessions (guest/tools/libexec/virtio-gpu-check): written for every native
# context, none without one, a stale one removed, and whole or absent when
# it cannot be written. The check's own lines run, from `lead=` to the end of
# their case, with sh (the check's shell) and /run/environment.d in a
# scratch folder.
set -u
here=$(dirname "$(readlink -f "$0")")
check=$here/../tools/libexec/virtio-gpu-check
work=$(mktemp -d)
trap 'chmod -R u+w "$work"; rm -rf "$work"' EXIT

awk '/^lead=\/run\/environment\.d\/45-vitrine-lone-frame\.conf$/ { on = 1 } on { print } on && /^esac$/ { exit }' \
    "$check" > "$work/block"
grep -q '^esac$' "$work/block" || { echo "virtio-gpu-check: no case after lead="; exit 1; }

fail=0
# run CONTEXT [setup]: the block with context=CONTEXT; prints what it said
run() {
    rm -rf "$work/env"
    [ -z "${2:-}" ] || eval "$2"
    { echo "context='$1'"; sed "s|/run/environment\.d|$work/env|g" "$work/block"; } > "$work/run"
    sh "$work/run" 2>&1
}
lead=$work/env/45-vitrine-lone-frame.conf
result() {   # NAME OK OUTPUT
    if [ "$2" = 1 ]; then
        echo "PASS $1"
    else
        echo "FAIL $1; the check said:"
        sed 's/^/    /' <<< "$3"
        [ -e "$lead" ] && sed 's/^/    file: /' "$lead"
        fail=1
    fi
}
# the file holds comments and exactly KWIN_LONE_FRAME_LEAD_US=1000, nothing else
written() {
    [ -f "$lead" ] && [ ! -e "$lead.new" ] &&
        [ "$(grep -v '^#' "$lead")" = KWIN_LONE_FRAME_LEAD_US=1000 ]
}

for context in amdgpu xe i915 drm-9; do
    out=$(run "$context")
    ok=0
    written && grep -q "native context ($context): .*KWIN_LONE_FRAME_LEAD_US=1000" <<< "$out" && ok=1
    result "a native context, $context: the file written" $ok "$out"
done

out=$(run amdgpu 'mkdir -p "$work/env"; echo KWIN_LONE_FRAME_LEAD_US=7 > "$work/env/45-vitrine-lone-frame.conf"')
ok=0; written && ok=1
result "an older file replaced" $ok "$out"

for context in none ""; do
    out=$(run "$context")
    ok=0; [ ! -e "$lead" ] && [ -z "$out" ] && ok=1
    result "no native context (${context:-not asked}): no file" $ok "$out"
done

out=$(run none 'mkdir -p "$work/env"; echo KWIN_LONE_FRAME_LEAD_US=1000 > "$work/env/45-vitrine-lone-frame.conf"')
ok=0; [ ! -e "$lead" ] && grep -q "no native context: .* removed" <<< "$out" && ok=1
result "no native context: a file from before removed" $ok "$out"

if [ "$(id -u)" != 0 ]; then
    out=$(run amdgpu 'mkdir -p "$work/env"; chmod 555 "$work/env"')
    ok=0
    [ ! -e "$lead" ] && [ ! -e "$lead.new" ] && grep -q "could not write .*: the guest's own sessions get no lone-frame lead" <<< "$out" && ok=1
    chmod 755 "$work/env"
    result "a folder it cannot write: no file, a warning" $ok "$out"
fi
exit $fail
