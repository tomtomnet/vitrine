#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The installer's move of a guest's KDE Frameworks (guest/tools/install:
# kf_moves, and take_source taking the medium's KSvg and Kirigami of the
# release they move to), its functions taken out of it as they are, with
# stand-ins for rpm (an installed set in a file; rpm --eval goes to the real
# one) and dnf's repoquery (the repositories' NAME-VERSION in a file).
#   A guest held at KDE Frameworks 6.31 by earlier tools, with their KSvg and
#   Kirigami: Fedora's kf6 packages at 6.31 move to 6.32 (those the
#   repositories have), and the medium's KSvg and Kirigami 6.32 are taken.
#   No move: the guest's Frameworks already the medium's; not Fedora's; the
#   repositories without kf6-kcoreaddons at the new release, or out of reach;
#   a medium without KSvg and Kirigami.  Without a move, KSvg of another
#   release stays.  Another vendor's kf6 package stays; Fedora's own KSvg is
#   not moved, as the medium's takes its place.
set -u
here=$(dirname "$(readlink -f "$0")")
install=$here/../tools/install
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail=0
bad() { echo "FAIL: $*"; fail=1; }
command -v rpm > /dev/null || { echo "no rpm here: skipped"; exit 0; }
real_rpm=$(command -v rpm)

mkdir -p "$work/bin" "$work/medium/repo"
# rpm: NAME|VERSION|RELEASE|ARCH|VENDOR|SOURCERPM per line in $FAKE/db; -qp from the
# medium's packages.txt; --eval to the real rpm
cat > "$work/bin/rpm" <<EOF
#!/bin/bash
[ "\$1" = --eval ] && exec "$real_rpm" "\$@"
exec python3 -I "$work/rpm.py" "\$@"
EOF
cat > "$work/rpm.py" <<'EOF'
import os, re, sys, fnmatch
fake = os.environ["FAKE"]
args = sys.argv[1:]
fmt = "%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}\n"
rest = []
i = 0
mode = None
while i < len(args):
    a = args[i]
    if a == "--qf":
        fmt = args[i + 1]; i += 2; continue
    if a in ("-q", "-qa", "-qp"):
        mode = a
    elif a == "--nosignature":
        pass
    else:
        rest.append(a)
    i += 1
fmt = fmt.replace("\\n", "\n")
rows = []
if mode == "-qp":
    for line in open(os.path.join(fake, "medium", "packages.txt")):
        n, vr, arch, f = line.split()
        if os.path.basename(rest[0]) == f:
            v, r = vr.split("-", 1)
            rows.append(dict(NAME=n, VERSION=v, RELEASE=r, ARCH=arch, VENDOR="vitrine", SOURCERPM="", EPOCHNUM="0"))
else:
    for line in open(os.path.join(fake, "db")):
        n, v, r, arch, vendor, src = line.rstrip("\n").split("|")
        rows.append(dict(NAME=n, VERSION=v, RELEASE=r, ARCH=arch, VENDOR=vendor, SOURCERPM=src, EPOCHNUM="0"))
def matches(row, spec):
    name, _, arch = spec.partition(".")
    if spec == row["NAME"] or (arch and name == row["NAME"] and arch == row["ARCH"]):
        return True
    return fnmatch.fnmatchcase(row["NAME"], spec)
out = []
if mode == "-qa":
    sel = [r for r in rows if not rest or any(matches(r, s) for s in rest)]
elif mode == "-q":
    sel = []
    for s in rest:
        found = [r for r in rows if matches(r, s) and "*" not in s]
        if not found:
            print(f"package {s} is not installed")
            sys.exit(1)
        sel += found
else:
    sel = rows
for r in sel:
    out.append(re.sub(r"%\{(\w+)\}", lambda m: r.get(m.group(1), ""), fmt))
sys.stdout.write("".join(out))
EOF
# dnf -q repoquery --qf '%{name}\n' NAME-VERSION...: those in $FAKE/repo (NAME-VERSION per line)
cat > "$work/bin/dnf" <<'EOF'
#!/bin/bash
[ "${FAKE_OFFLINE:-0}" = 1 ] && exit 1
for a in "$@"; do
	case $a in -q|repoquery|--qf|'%{name}\n') continue ;; esac
	grep -qx -- "$a" "$FAKE/repo" && echo "${a%-*}"
done
exit 0
EOF
chmod +x "$work/bin/rpm" "$work/bin/dnf"
export PATH=$work/bin:$PATH FAKE=$work

# the installer's functions, as they are
for f in say medium_nevra installed from_source evr_cmp medium_evr take_source kf_moves; do
	# a function on one line ("say() { ... }") ends there
	body=$(awk -v f="$f" '!on && index($0, f "() {") == 1 { on = 1; print; if ($0 ~ /}[[:space:]]*$/ && $0 !~ /\{[[:space:]]*(#.*)?$/) exit; next }
		on { print; if ($0 ~ /^}/) exit }' "$install")
	[ -n "$body" ] || { echo "FAIL: no $f in the installer"; exit 1; }
	eval "$body"
done
packages_txt=$work/medium/packages.txt
medium=$work/medium

# the medium: KSvg and Kirigami 6.32.0 (k2, f1)
medium_has() {
	: > "$packages_txt"
	for p in "$@"; do
		echo "$p" >> "$packages_txt"
	done
}
# a guest held at KF 6.31 by earlier tools, with their KSvg and Kirigami
held_guest() {
	cat > "$work/db" <<'EOF'
kf6-kcoreaddons|6.31.0|1.fc45|x86_64|Fedora Project|kf6-kcoreaddons-6.31.0-1.fc45.src.rpm
kf6-kconfig|6.31.0|1.fc45|x86_64|Fedora Project|kf6-kconfig-6.31.0-1.fc45.src.rpm
kf6-kio|6.31.0|1.fc45|x86_64|Fedora Project|kf6-kio-6.31.0-1.fc45.src.rpm
kf6-kio-core|6.31.0|1.fc45|x86_64|Fedora Project|kf6-kio-6.31.0-1.fc45.src.rpm
kf6-ksvg|6.31.0|1.1.k2.fc45|x86_64|vitrine|kf6-ksvg-6.31.0-1.1.k2.fc45.src.rpm
kf6-kirigami|6.31.0|1.1.f1.fc45|x86_64|vitrine|kf6-kirigami-6.31.0-1.1.f1.fc45.src.rpm
kf6-kirigami-addons|1.10.0|1.fc45|x86_64|Fedora Project|kf6-kirigami-addons-1.10.0-1.fc45.src.rpm
kf6-old-gone|6.31.0|1.fc45|x86_64|Fedora Project|kf6-old-gone-6.31.0-1.fc45.src.rpm
plasma-workspace|6.8.2|1.fc45|x86_64|Fedora Project|plasma-workspace-6.8.2-1.fc45.src.rpm
EOF
	printf '%s\n' kf6-kcoreaddons-6.32.0 kf6-kconfig-6.32.0 kf6-kio-6.32.0 kf6-kio-core-6.32.0 kf6-ksvg-6.32.0 \
		kf6-kirigami-6.32.0 plasma-workspace-6.8.3 > "$work/repo"
	medium_has "kf6-ksvg 6.32.0-1.1.k2.fc45 x86_64 kf6-ksvg-6.32.0-1.1.k2.fc45.x86_64.rpm" \
		"kf6-kirigami 6.32.0-1.1.f1.fc45 x86_64 kf6-kirigami-6.32.0-1.1.f1.fc45.x86_64.rpm"
	KSVG_VERSION=6.32.0 KIRIGAMI_VERSION=6.32.0
}

# 1. held at 6.31: Fedora's move, the medium's KSvg and Kirigami taken
case=held
held_guest
kf_move=() kf_from='' kf_to=''
if kf_moves; then
	[ "$kf_from" = 6.31.0 ] && [ "$kf_to" = 6.32.0 ] || bad "$case: from $kf_from to $kf_to"
	[ "${kf_move[*]}" = "kf6-kconfig-6.32.0 kf6-kcoreaddons-6.32.0 kf6-kio-6.32.0 kf6-kio-core-6.32.0" ] ||
		bad "$case: moves ${kf_move[*]}"
else
	bad "$case: no move"
fi
take_source kf6-ksvg kf6-ksvg KSvg > "$work/out" 2>&1
[ "${source_pkgs[*]}" = "kf6-ksvg-6.32.0-1.1.k2.fc45.x86_64" ] || bad "$case: KSvg ${source_pkgs[*]} ($(cat "$work/out"))"
take_source kf6-kirigami kf6-kirigami Kirigami > "$work/out" 2>&1
[ "${source_pkgs[*]}" = "kf6-kirigami-6.32.0-1.1.f1.fc45.x86_64" ] ||
	bad "$case: Kirigami ${source_pkgs[*]} ($(cat "$work/out"))"

# 2. no move: KSvg of another release stays, as before
case=nomove
held_guest
kf_move=() kf_from='' kf_to=''
take_source kf6-ksvg kf6-ksvg KSvg > "$work/out" 2>&1
[ ${#source_pkgs[@]} = 0 ] || bad "$case: KSvg taken without a move: ${source_pkgs[*]}"
grep -q "this guest has 6.31.0, this medium's is built from 6.32.0: the installed one stays" "$work/out" ||
	bad "$case: not said: $(cat "$work/out")"

# 3. the guest's Frameworks already the medium's release: no move
case=same
held_guest
sed -i 's/^kf6-kcoreaddons|6.31.0|/kf6-kcoreaddons|6.32.0|/' "$work/db"
kf_move=() kf_from='' kf_to=''
! kf_moves || bad "$case: a move ${kf_move[*]}"
[ -z "$kf_to" ] || bad "$case: kf_to $kf_to"

# 4. Frameworks not Fedora's: left alone
case=notfedora
held_guest
sed -i 's/^\(kf6-kcoreaddons|6.31.0|1.fc45|x86_64|\)Fedora Project/\1KDE neon/' "$work/db"
kf_move=() kf_from='' kf_to=''
! kf_moves || bad "$case: a move ${kf_move[*]}"

# 5. the repositories without kf6-kcoreaddons 6.32 (not out yet): no move, no half one
case=norepo
held_guest
sed -i '/^kf6-kcoreaddons-6.32.0$/d' "$work/repo"
kf_move=() kf_from='' kf_to=''
! kf_moves || bad "$case: a move ${kf_move[*]}"
[ ${#kf_move[@]} = 0 ] || bad "$case: a half move ${kf_move[*]}"

# 6. the repositories out of reach: no move
case=offline
held_guest
kf_move=() kf_from='' kf_to=''
! FAKE_OFFLINE=1 kf_moves || bad "$case: a move ${kf_move[*]}"

# 7. a medium without KSvg and Kirigami: nothing to move to
case=nomedium
held_guest
unset KSVG_VERSION KIRIGAMI_VERSION
kf_move=() kf_from='' kf_to=''
! kf_moves || bad "$case: a move ${kf_move[*]}"

# 8. another vendor's kf6 package at that release: left as it is
case=othervendor
held_guest
echo "kf6-kthird|6.31.0|1|x86_64|Someone Else|kf6-kthird-6.31.0-1.src.rpm" >> "$work/db"
echo kf6-kthird-6.32.0 >> "$work/repo"
kf_move=() kf_from='' kf_to=''
kf_moves || bad "$case: no move"
[[ " ${kf_move[*]} " != *" kf6-kthird-"* ]] || bad "$case: moved: ${kf_move[*]}"

# 9. Fedora's own KSvg at that release (earlier tools without theirs): the medium's
# takes its place, not Fedora's 6.32 beside it in the transaction
case=fedoraksvg
held_guest
sed -i 's/^kf6-ksvg|6.31.0|1.1.k2.fc45|x86_64|vitrine|kf6-ksvg-6.31.0-1.1.k2.fc45.src.rpm$/kf6-ksvg|6.31.0|1.fc45|x86_64|Fedora Project|kf6-ksvg-6.31.0-1.fc45.src.rpm/' "$work/db"
grep -q '^kf6-ksvg|6.31.0|1.fc45|x86_64|Fedora Project' "$work/db" || bad "$case: no Fedora KSvg in the stand-in"
kf_move=() kf_from='' kf_to=''
kf_moves || bad "$case: no move"
[[ " ${kf_move[*]} " != *" kf6-ksvg-"* ]] || bad "$case: Fedora's KSvg moved too: ${kf_move[*]}"
take_source kf6-ksvg kf6-ksvg KSvg > "$work/out" 2>&1
[ "${source_pkgs[*]}" = "kf6-ksvg-6.32.0-1.1.k2.fc45.x86_64" ] || bad "$case: KSvg ${source_pkgs[*]}"

[ "$fail" = 0 ] && echo "kf-move: all cases passed"
exit "$fail"
