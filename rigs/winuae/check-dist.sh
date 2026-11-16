#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# A release archive (mk/amiga-dist.mk lrvne-dist, TARGET=amiga) on an emulated Amiga: unpacks it
# in WinUAE with the Workbench tree's LhA (2.x; the emulated CPU is a 68040), lists the result,
# runs `Version` on the program and the program itself from its unpacked drawer with the pack
# copied in, as a player would put it there (the archive has none: a pack is the player's), for
# a few frames (DIST_ARGS, default AUDIO=none FAST MAXTICKS=200), then compares every unpacked
# file byte for byte with the staging directory on the host. Exit status 0 when all of that
# holds; out.txt in the work dir holds LhA's and the program's output.
#
#   check-dist.sh ARCHIVE STAGE DRAWER PROGRAM VERSION PAK
#
# STAGE is the directory the archive was made from (it holds DRAWER/, maybe DRAWER.info),
# PROGRAM the program's name in the drawer, VERSION what `Version` must report after it. `make
# TARGET=amiga lrvne-dist-check` passes all of these (mk/amiga-dist.mk). Settings: run-lib.sh's.
set -eu
[ $# -eq 6 ] || { sed -n '11,15p' "$0"; exit 2; }
ARCHIVE=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
STAGE=$2 DRAWER=$3 PROGRAM=$4 VERSION=$5 PAK=$6
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/run-lib.sh"
[ -f "$ARCHIVE" ] || { echo "check-dist.sh: no archive $ARCHIVE" >&2; exit 2; }
[ -f "$PAK" ] || { echo "check-dist.sh: no pack $PAK" >&2; exit 2; }
rig_need WBDISK "a Workbench 3.1 tree with LhA 2.x in C:"

emu_setup "dist-$PROGRAM"
cp "$WBDISK/C/lha" "$W/dh0/C/lha"
chmod 755 "$W/dh0/C/lha"
# LhA opens mathieeedoubbas.library (a missing one is a requester, which would hold the run)
cp "$WBDISK"/Libs/mathieee*.library "$W/dh0/Libs/"
cp "$PAK" "$W/dh0/the.pak"
cp "$ARCHIVE" "$W/dh0/a.lha"
{
	# LhA reads ENV:; without the assign that is a "please insert volume ENV" requester
	echo "!C:MakeDir RAM:ENV"
	echo "!Assign ENV: RAM:ENV"
	echo "Version C:lha"
	echo "!MakeDir DH0:x"
	echo "!cd DH0:x"
	echo "C:lha x DH0:a.lha"
	echo "List DH0:x ALL"
	echo "!Copy DH0:the.pak DH0:x/$DRAWER/the.pak"
	echo "!cd DH0:x/$DRAWER"
	echo "Version $PROGRAM"
	echo "$PROGRAM PAK=PROGDIR:the.pak ${DIST_ARGS:-AUDIO=none FAST MAXTICKS=200}"
} | emu_script
emu_config native
sed -i -e "s/^cpu_model=.*/cpu_model=68040/" "$W/a.uae"
emu_run

# host side: the run, the unpacked tree against the staging directory, the program's output
rc=0
case $EMU_RC in
0) ;;
124|137) echo "FAIL the run did not end: timeout (backstop, EMU_TIMEOUT=$EMU_TIMEOUT)"; rc=1 ;;
*) echo "FAIL the emulator exited with $EMU_RC"; rc=1 ;;
esac
a=$(basename "$ARCHIVE")
for f in $(cd "$STAGE" && find . -type f | sed 's|^\./||' | sort); do
	if cmp -s "$W/dh0/x/$f" "$STAGE/$f"; then
		echo "ok   $a: $f"
	else
		echo "FAIL $a: $f differs or is missing"
		rc=1
	fi
done
if [ -n "$(cd "$STAGE" && find . -name '*.pak')" ]; then
	echo "FAIL the archive contains a pack"
	rc=1
else
	echo "ok   no pack in the archive"
fi
if grep -q "^$PROGRAM $VERSION " "$W/out.txt"; then
	echo "ok   Version reports $PROGRAM $VERSION"
else
	echo "FAIL Version did not report $PROGRAM $VERSION (see $W/out.txt)"
	rc=1
fi
if grep -q "max ticks reached" "$W/out.txt"; then
	echo "ok   the unpacked program ran to MAXTICKS"
else
	echo "FAIL the unpacked program did not reach MAXTICKS (see $W/out.txt)"
	rc=1
fi
grep -o '[1-9][0-9]* bytes not freed by it' "$W/out.txt" | sed 's/^/note the engine left /' || true
exit $rc
