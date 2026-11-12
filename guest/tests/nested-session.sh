#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The nested desktop session's KWin settings (guest/tools/nested/session), for
# nested.conf as guest tools 0.1.0-39 to -43 shipped it (nested-0.1.0-43.conf,
# the file an update keeps when it had been changed), as this tree ships it,
# and edited, on AMD and Intel xe hosts. The session's own lines run: from
# `set -u` to its first export (nested.conf and the defaults), and from
# kwin_env_value() to the proxy's socket (the host GPU's settings and the
# lead of older files), with log() printing and the paths in a scratch folder.
set -u
here=$(dirname "$(readlink -f "$0")")
session=$here/../tools/nested/session
new=$here/../tools/nested/nested.conf
old=$here/nested-0.1.0-43.conf
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
: > "$work/words"

awk '/^set -u$/ { on = 1 } on && /^export / { exit } on { print }' "$session" > "$work/part1"
awk '/^kwin_env_value\(\) \{/ { on = 1 } on && /^rm -f "\$sock"/ { exit } on { print }' "$session" > "$work/part2"
for want in 'CROSVM_DESKTOP_KWIN_ENV=' 'nested.conf'; do
    grep -q "$want" "$work/part1" || { echo "session: no '$want' before its first export"; exit 1; }
done
for want in 'host_gpu_file=' 'CROSVM_DESKTOP_CONF_FROM'; do
    grep -q "$want" "$work/part2" || { echo "session: no '$want' between kwin_env_value and the socket"; exit 1; }
done

fail=0
# check NAME CONF HOST_GPU LEAD LOG [ALSO]: the session with nested.conf CONF on
# a host whose GPU is HOST_GPU (auto: the boot check's file says xe) ends with
# lead LEAD (none: no lead), says LOG in its journal (empty: nothing about
# the lead) and ALSO
check() {
    local name=$1 conf=$2 gpu=$3 want=$4 want_log=$5 also=${6:-} out lead ok=1
    printf 'NATIVE_CONTEXT=xe\n' > "$work/host-gpu"
    {
        echo 'log() { echo "LOG: $1"; }'
        sed -e "s|^conf=.*|conf='$conf'|" -e "s|^launcher_words=.*|launcher_words='$work/words'|" "$work/part1"
        [ "$gpu" = auto ] || echo "CROSVM_DESKTOP_HOST_GPU='$gpu'"
        sed -e "s|^host_gpu_file=.*|host_gpu_file='$work/host-gpu'|" "$work/part2"
        echo 'echo "ENV: $CROSVM_DESKTOP_KWIN_ENV"'
    } > "$work/run"
    out=$(env -i PATH="$PATH" bash "$work/run" 2>&1)
    lead=$(sed -n 's/^ENV: .*KWIN_LONE_FRAME_LEAD_US=\([0-9]*\).*/\1/p' <<< "$out")
    [ "${lead:-none}" = "$want" ] || ok=0
    if [ -n "$want_log" ]; then
        grep -Eq "$want_log" <<< "$out" || ok=0
    elif grep -q 'lone-frame lead' <<< "$out"; then
        ok=0
    fi
    [ -z "$also" ] || grep -Eq "$also" <<< "$out" || ok=0
    [ "$(grep '^ENV:' <<< "$out" | grep -o 'KWIN_LONE_FRAME_LEAD_US=' | wc -l)" -le 1 ] || ok=0
    if [ $ok = 1 ]; then
        echo "PASS $name: lead ${lead:-none}"
    else
        echo "FAIL $name: want lead $want; the session said:"
        sed 's/^/    /' <<< "$out"
        fail=1
    fi
}
edit() {   # a copy of CONF, edited by sed SCRIPT
    local f
    f=$(mktemp "$work/conf.XXXXXX")
    sed "$2" "$1" > "$f"
    echo "$f"
}
replaced='lone-frame lead %s, the earlier shipped value, replaced with 1000'
xe_two='Intel xe .*: this session.s KWin gets KWIN_DISABLE_UDMABUF_IMPORT=1 KWIN_WAYLAND_SCANOUT_BUFFERS=1$'

grep -q '^CROSVM_DESKTOP_CONF_FROM=' "$new" || { echo "nested.conf: no CROSVM_DESKTOP_CONF_FROM"; exit 1; }
grep -q '^CROSVM_DESKTOP_CONF_FROM=' "$old" && { echo "$old: CROSVM_DESKTOP_CONF_FROM"; exit 1; }

check "this tree's file, AMD" "$new" amdgpu 1000 "" "none of the Intel xe settings"
check "this tree's file, xe" "$new" xe 1000 "" "$xe_two"
check "this tree's file, xe from the boot check" "$new" auto 1000 "" "Intel xe \(from the native context, at boot\)"
check "0.1.0-43's file, AMD" "$old" amdgpu 1000 "$(printf "$replaced" 2500)"
check "0.1.0-43's file, xe" "$old" xe 1000 "$(printf "$replaced" 2500)" "$xe_two"
check "0.1.0-43's file with 1500, xe" "$(edit "$old" 's/LEAD_US=2500/LEAD_US=1500/')" xe 1000 "$(printf "$replaced" 1500)"
check "0.1.0-43's file with 1500, AMD: yours" "$(edit "$old" 's/LEAD_US=2500/LEAD_US=1500/')" amdgpu 1500 ""
check "0.1.0-43's file with 2000, xe: yours" "$(edit "$old" 's/LEAD_US=2500/LEAD_US=2000/')" xe 2000 ""
check "0.1.0-43's file without a lead, xe" "$(edit "$old" 's/ KWIN_LONE_FRAME_LEAD_US=2500//')" xe none ""
check "0.1.0-43's file without a lead, AMD" "$(edit "$old" 's/ KWIN_LONE_FRAME_LEAD_US=2500//')" amdgpu none ""
check "0.1.0-43's file, an import setting of yours, xe" \
    "$(edit "$old" 's/KWIN_ASYNC_CURSOR_MOVES=1"/KWIN_ASYNC_CURSOR_MOVES=1 KWIN_DISABLE_UDMABUF_IMPORT=0"/')" xe 1000 \
    "$(printf "$replaced" 2500)" "your own: KWIN_DISABLE_UDMABUF_IMPORT=0"
check "this tree's file with 2500, AMD: yours" "$(edit "$new" 's/LEAD_US=1000/LEAD_US=2500/')" amdgpu 2500 ""
check "this tree's file with 2500, xe: yours" "$(edit "$new" 's/LEAD_US=1000/LEAD_US=2500/')" xe 2500 ""
check "this tree's file with 1500, xe: yours" "$(edit "$new" 's/LEAD_US=1000/LEAD_US=1500/')" xe 1500 ""
check "no nested.conf, AMD" "$work/none.conf" amdgpu 1000 ""
check "no nested.conf, xe" "$work/none.conf" xe 1000 "" "$xe_two"
exit $fail
