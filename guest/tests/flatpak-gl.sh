#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# vitrine's Mesa for Flatpak apps in a guest (guest/tools/libexec/flatpak-gl),
# against a stand-in for flatpak that keeps its installation in a scratch
# folder: Flathub's GL.default of a runtime the guest has goes, vitrine's
# takes its place (both of a branch: BRANCH and BRANCH-extra); nothing again
# when done; a runtime installed later gets it on the next sync; a new
# medium's build updates it; FLATPAK_GL_DRIVERS=mesa-git goes from
# /etc/environment, and comes back where Flathub's mesa-git extension is
# there for every runtime; remove brings Flathub's back, keeps vitrine's
# where Flathub is out of reach, and drops the remote.
set -u
here=$(dirname "$(readlink -f "$0")")
script=$here/../tools/libexec/flatpak-gl
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
gl=org.freedesktop.Platform.GL.default

mkdir -p "$work/bin" "$work/fake/remotes" "$work/fake/installed"
# The stand-in: installed refs in installed/NAME/BRANCH ("ORIGIN COMMIT"),
# remotes/NAME their URL, the refs of a remote on the web in
# refs/REMOTE/NAME/BRANCH (a commit), vitrine-gl's in the repository its URL
# names; each call in calls.  FAKE_OFFLINE=1: the web out of reach;
# FAKE_FAIL_UNINSTALL=REF.
cat > "$work/bin/flatpak" <<'EOF'
#!/bin/bash
f=$FAKE_FLATPAK
echo "$*" >> "$f/calls"
args=()
for a in "$@"; do
	case $a in
	--system|-y|--noninteractive|--if-not-exists|--no-gpg-verify|--force|--runtime|--prio=*|--title=*|--columns=*) ;;
	*) args+=("$a") ;;
	esac
done
key() { echo "${1/\/\///}"; }
commit_in() {
	local url k
	url=$(cat "$f/remotes/$1" 2> /dev/null) || return 1
	k=$(key "$2")
	case $url in
	file://*) cat "${url#file://}/refs/heads/runtime/${k%/*}/x86_64/${k##*/}" 2> /dev/null ;;
	*) [ "${FAKE_OFFLINE:-0}" != 1 ] && cat "$f/refs/$1/$k" 2> /dev/null ;;
	esac
}
case ${args[0]} in
--default-arch) echo x86_64 ;;
info)
	what=
	ref=
	for a in "${args[@]:1}"; do
		case $a in
		--show-origin) what=origin ;;
		--show-commit) what=commit ;;
		*) ref=$a ;;
		esac
	done
	[ -f "$f/installed/$(key "$ref")" ] || exit 1
	read -r origin commit < "$f/installed/$(key "$ref")"
	case $what in
	origin) echo "$origin" ;;
	commit) echo "$commit" ;;
	*) echo "Ref: $ref" ;;
	esac
	;;
remote-add) [ -e "$f/remotes/${args[1]}" ] || echo "${args[2]}" > "$f/remotes/${args[1]}" ;;
remote-modify)
	for a in "${args[@]:1}"; do
		case $a in --url=*) url=${a#--url=} ;; *) name=$a ;; esac
	done
	[ -e "$f/remotes/$name" ] && echo "$url" > "$f/remotes/$name"
	;;
remote-list) ls "$f/remotes" ;;
remote-delete) rm "$f/remotes/${args[1]}" ;;
remote-info) commit_in "${args[1]}" "${args[2]}" > /dev/null ;;
install)
	k=$(key "${args[2]}")
	[ ! -f "$f/installed/$k" ] || { echo "error: ${args[2]} is already installed" >&2; exit 1; }
	c=$(commit_in "${args[1]}" "${args[2]}") || { echo "error: nothing matches ${args[2]} in ${args[1]}" >&2; exit 1; }
	mkdir -p "$(dirname "$f/installed/$k")"
	echo "${args[1]} $c" > "$f/installed/$k"
	;;
uninstall)
	[ "${FAKE_FAIL_UNINSTALL:-}" != "${args[1]}" ] || { echo "error: in use" >&2; exit 1; }
	rm "$f/installed/$(key "${args[1]}")" || exit 1
	;;
update)
	k=$(key "${args[1]}")
	read -r origin commit < "$f/installed/$k"
	c=$(commit_in "$origin" "${args[1]}") || exit 1
	echo "$origin $c" > "$f/installed/$k"
	;;
list)
	(cd "$f/installed" && find . -type f) | sed 's|^\./||' | while read -r k; do
		read -r origin commit < "$f/installed/$k"
		printf '%s/x86_64/%s\t%s\n' "${k%/*}" "${k##*/}" "$origin"
	done
	;;
*) echo "the stand-in has no ${args[0]}" >&2; exit 2 ;;
esac
EOF
chmod +x "$work/bin/flatpak"
export FAKE_FLATPAK=$work/fake
export VITRINE_FLATPAK_GL_DIR=$work/state/flatpak-gl
export VITRINE_ENVIRONMENT=$work/etc/environment
export PATH=$work/bin:$PATH

# install REMOTE NAME BRANCH COMMIT; has REMOTE NAME BRANCH COMMIT (a remote has it)
install() { mkdir -p "$work/fake/installed/$2"; echo "$1 $4" > "$work/fake/installed/$2/$3"; }
has() { mkdir -p "$work/fake/refs/$1/$2"; echo "$4" > "$work/fake/refs/$1/$2/$3"; }
# a medium's flatpak/: commits named after @1, "BRANCH MESA COMMIT" lines
medium() {
	local m=$work/$1 b v
	mkdir -p "$m/repo/refs/heads/runtime/$gl/x86_64"
	: > "$m/branches"
	for b in 25.08 26.08; do
		for v in $b $b-extra; do
			echo "$1-$v" > "$m/repo/refs/heads/runtime/$gl/x86_64/$v"
		done
		echo "$b 26.2.$((${b%%.*} - 18)) 0123" >> "$m/branches"
	done
}
origin() { cut -d' ' -f1 "$work/fake/installed/$gl/$1" 2> /dev/null; }
commit() { cut -d' ' -f2 "$work/fake/installed/$gl/$1" 2> /dev/null; }

fail=0
none() { ! "$@"; }
check() {   # NAME CONDITION...
	local name=$1
	shift
	if "$@"; then
		echo "PASS $name"
	else
		echo "FAIL $name"
		fail=1
	fi
}

echo "https://dl.flathub.org/repo/" > "$work/fake/remotes/flathub"
echo "https://dl.flathub.org/beta-repo/" > "$work/fake/remotes/flathub-beta"
for v in 25.08 25.08-extra 26.08 26.08-extra; do
	has flathub "$gl" "$v" "flathub-$v"
	has flathub-beta "$gl" "$v" "beta-$v"
done
has flathub org.freedesktop.Platform 26.08 flathub-platform-26.08
# the guest: runtime 25.08 from Flathub, with its GL extension
install flathub org.freedesktop.Platform 25.08 flathub-platform-25.08
install flathub "$gl" 25.08 flathub-25.08
install flathub "$gl" 25.08-extra flathub-25.08-extra
mkdir -p "$work/etc"
printf 'LANG=C\nFLATPAK_GL_DRIVERS=mesa-git\n' > "$work/etc/environment"
medium one

out=$(bash "$script" install "$work/one" 2>&1)
rc=$?
echo "$out"
check "install succeeds" [ "$rc" = 0 ]
check "the medium's repository is kept" [ -s "$VITRINE_FLATPAK_GL_DIR/branches" -a -d "$VITRINE_FLATPAK_GL_DIR/repo" ]
check "the remote points at it" grep -qx "file://$VITRINE_FLATPAK_GL_DIR/repo" "$work/fake/remotes/vitrine-gl"
check "25.08 is vitrine's" [ "$(origin 25.08) $(commit 25.08)" = "vitrine-gl one-25.08" ]
check "25.08-extra is vitrine's" [ "$(origin 25.08-extra) $(commit 25.08-extra)" = "vitrine-gl one-25.08-extra" ]
check "26.08, not installed, stays so" [ ! -e "$work/fake/installed/$gl/26.08" ]
check "it says what it replaced" grep -q "$gl//25.08: vitrine's Mesa 26.2.7, in place of flathub's" <<< "$out"
check "and what it left" grep -q "runtime 26.08: not installed" <<< "$out"
check "it remembers where they came from" [ "$(cat "$VITRINE_FLATPAK_GL_DIR/replaced")" = "$(printf '25.08 flathub\n25.08-extra flathub')" ]
check "FLATPAK_GL_DRIVERS=mesa-git goes" [ "$(cat "$work/etc/environment")" = "LANG=C" ]
check "and is kept for remove" [ "$(cat "$VITRINE_FLATPAK_GL_DIR/environment")" = "FLATPAK_GL_DRIVERS=mesa-git" ]

: > "$work/fake/calls"
out=$(bash "$script" sync 2>&1)
check "sync again: done already" [ $? = 0 ]
check "with no install, update or uninstall" none grep -Eq '^(install|update|uninstall) ' "$work/fake/calls"

# a runtime installed later, with its GL extension from Flathub: the trigger's sync
install flathub org.freedesktop.Platform 26.08 flathub-platform-26.08
install flathub "$gl" 26.08 flathub-26.08
install flathub "$gl" 26.08-extra flathub-26.08-extra
out=$(bash "$script" sync 2>&1)
check "a runtime installed later: sync succeeds" [ $? = 0 ]
check "26.08 is vitrine's" [ "$(origin 26.08) $(commit 26.08)" = "vitrine-gl one-26.08" ]
check "26.08-extra is vitrine's" [ "$(origin 26.08-extra)" = vitrine-gl ]
# a runtime whose -extra alone is there: both vitrine's
rm "$work/fake/installed/$gl/26.08"
out=$(bash "$script" sync 2>&1)
check "a branch's missing one comes too" [ "$(origin 26.08) $(commit 26.08)" = "vitrine-gl one-26.08" ]

# the next medium: its build in place of the last one, as an update
medium two
printf 'LANG=C\n' > "$work/etc/environment"
out=$(bash "$script" install "$work/two" 2>&1)
check "the next medium: install succeeds" [ $? = 0 ]
check "its builds updated" [ "$(commit 25.08) $(commit 26.08-extra)" = "two-25.08 two-26.08-extra" ]
check "as updates" grep -q "$gl//25.08: vitrine's Mesa 26.2.7, updated" <<< "$out"
check "what it replaced still known" grep -qx "25.08 flathub" "$VITRINE_FLATPAK_GL_DIR/replaced"
check "the line taken out still known" [ -s "$VITRINE_FLATPAK_GL_DIR/environment" ]
check "status" grep -qx "$gl//26.08-extra: vitrine's Mesa 26.2.8 (two-26.08-ex)" <<< "$(bash "$script" status)"

# one that cannot go (an app runs on it): said, the others done
install flathub "$gl" 26.08 flathub-26.08
out=$(FAKE_FAIL_UNINSTALL="$gl//26.08" bash "$script" sync 2>&1)
check "an uninstall refused: sync fails" [ $? != 0 ]
check "and says so" grep -q "$gl//26.08 of flathub could not be uninstalled: it stays" <<< "$out"
check "Flathub's stays there" [ "$(origin 26.08)" = flathub ]
bash "$script" sync > /dev/null 2>&1

# Flathub out of reach: vitrine's stay, with what to do later, and no remote
for d in fake state etc; do
	cp -r "$work/$d" "$work/$d-before-remove"
done
out=$(FAKE_OFFLINE=1 bash "$script" remove 2>&1)
check "remove offline: fails" [ $? != 0 ]
check "vitrine's stay" [ "$(origin 25.08)" = vitrine-gl ]
check "with what to do later" grep -q "flatpak uninstall $gl//25.08 && flatpak install flathub $gl//25.08" <<< "$out"
check "the remote goes all the same" [ ! -e "$work/fake/remotes/vitrine-gl" ]
for d in fake state etc; do
	rm -rf "${work:?}/$d"
	mv "$work/$d-before-remove" "$work/$d"
done

# where they came from forgotten, runtime 25.08 from Flathub's beta now and
# 26.08 gone: 25.08's from the beta, as the runtime, 26.08's from Flathub;
# Flathub's mesa-git extension there for 25.08 alone
rm "$VITRINE_FLATPAK_GL_DIR/replaced" "$work/fake/installed/org.freedesktop.Platform/26.08"
install flathub-beta org.freedesktop.Platform 25.08 beta-platform-25.08
install flathub-beta org.freedesktop.Platform.GL.mesa-git 25.08 beta-mesa-git-25.08
out=$(bash "$script" remove 2>&1)
rc=$?
echo "$out"
check "remove succeeds" [ "$rc" = 0 ]
for v in 25.08 25.08-extra; do
	check "$v is the runtime's remote's" [ "$(origin "$v") $(commit "$v")" = "flathub-beta beta-$v" ]
done
for v in 26.08 26.08-extra; do
	check "$v is Flathub's again" [ "$(origin "$v") $(commit "$v")" = "flathub flathub-$v" ]
done
check "no remote vitrine-gl" [ ! -e "$work/fake/remotes/vitrine-gl" ]
check "no repository" [ ! -e "$VITRINE_FLATPAK_GL_DIR" ]
check "FLATPAK_GL_DRIVERS=mesa-git stays out: no mesa-git for 26.08" [ "$(cat "$work/etc/environment")" = "LANG=C" ]
check "and says why" grep -q "FLATPAK_GL_DRIVERS=mesa-git stays out of $work/etc/environment: the runtimes 26.08 have no mesa-git" <<< "$out"

# installed again with the line, then mesa-git there for both runtimes: the line comes back
printf 'LANG=C\nFLATPAK_GL_DRIVERS=mesa-git\n' > "$work/etc/environment"
bash "$script" install "$work/two" > /dev/null 2>&1
check "the line out again" [ "$(cat "$work/etc/environment")" = "LANG=C" ]
install flathub-beta org.freedesktop.Platform.GL.mesa-git 26.08 beta-mesa-git-26.08
out=$(bash "$script" remove 2>&1)
check "remove with mesa-git everywhere succeeds" [ $? = 0 ]
check "FLATPAK_GL_DRIVERS=mesa-git back" [ "$(cat "$work/etc/environment")" = "$(printf 'LANG=C\nFLATPAK_GL_DRIVERS=mesa-git')" ]

# no flatpak: nothing to do (a PATH of id alone)
mkdir -p "$work/nobin"
ln -s "$(command -v id)" "$work/nobin/id"
out=$(PATH=$work/nobin "$BASH" "$script" sync 2>&1)
check "no flatpak: nothing to do" [ $? = 0 ]
check "and says so" grep -q "no flatpak in this guest" <<< "$out"

exit "$fail"
