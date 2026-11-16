#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# A release archive (mk/amiga-dist.mk lrvne-dist, TARGET=aros) in hosted AROS: unpacks it with
# the AROS lha (x86_64, from the 2026.09 contrib archive; the Unix one, which unpacks into the
# current directory) into Work:x, compares every file with the staging directory on the host
# (SYS: is a host directory), asks Version, and runs the unpacked program from its own drawer
# with the pack copied in (the archive has none: a pack is the player's) for a few frames
# (DIST_ARGS, default AUDIO=none MAXTICKS=200). Exit status 0 when all of that holds.
#
#   check-dist.sh ARCHIVE STAGE DRAWER PROGRAM VERSION PAK
#
# STAGE is the directory the archive was made from, PROGRAM the program's name in the drawer,
# VERSION what `Version` must report after it; `make TARGET=aros lrvne-dist-check` passes them
# (mk/amiga-dist.mk). Settings (../rig.sh): AROS_DIR, or CONTRIB (the contrib archive's tree,
# default $AROS_DIR/contrib/AROS-202609-any-x86_64-contrib) and AROS_HOSTED; RIG_RUNS, where
# the run directory goes (default ../build/rig-runs below the current directory).
set -u
[ $# -eq 6 ] || { sed -n '10,16p' "$0"; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../rig.sh"
A=$1 STAGE=$2 DRAWER=$3 PROGRAM=$4 VERSION=$5 PAK=$6
CONTRIB=${CONTRIB:-${AROS_DIR:+$AROS_DIR/contrib/AROS-202609-any-x86_64-contrib}}
[ -n "$CONTRIB" ] || rig_need CONTRIB "the unpacked AROS contrib archive"
LHA_AROS=$CONTRIB/Extras/Misc/aminet/C/lha
R=${RIG_RUNS:-../build/rig-runs}/dist-$PROGRAM
[ -f "$PAK" ] || { echo "check-dist.sh: no pack $PAK"; exit 2; }
[ -f "$A" ] || { echo "check-dist.sh: no archive $A"; exit 2; }
[ -f "$LHA_AROS" ] || { echo "check-dist.sh: no AROS lha at $LHA_AROS (unpack the contrib archive)"; exit 2; }

mkdir -p "$(dirname "$R")"
cp "$PAK" "$(dirname "$R")/the.pak"
printf '%s\n' \
    "!MakeDir Work:x" "!CD Work:x" \
    "!Work:lha xq Work:$(basename "$A") >>SYS:out.txt" \
    "Version Work:x/$DRAWER/$PROGRAM" \
    "!Copy Work:the.pak Work:x/$DRAWER/the.pak" \
    "!CD Work:x/$DRAWER" \
    "$PROGRAM PAK=PROGDIR:the.pak ${DIST_ARGS:-AUDIO=none MAXTICKS=200}" \
    | PRUNE=1 AROS_FILES="$A $LHA_AROS $(dirname "$R")/the.pak" "$HERE/aros.sh" hosted "$R" 600 >/dev/null
run_rc=$?
rm -f "$(dirname "$R")/the.pak"
cat "$R/out.txt"
rc=0
if [ $run_rc != 0 ]; then
    echo "FAIL hosted AROS: $(tail -1 "$R/run.log") (aros.sh exit $run_rc)"
    rc=1
fi
a=$(basename "$A")
for f in $(cd "$STAGE" && find . -type f | sed 's|^\./||' | sort); do
    if cmp -s "$R/Work/x/$f" "$STAGE/$f"; then
        echo "ok   $a: $f"
    else
        echo "FAIL $a: $f differs or is missing"
        rc=1
    fi
done
if [ -n "$(cd "$STAGE" && find . -name '*.pak')" ]; then echo "FAIL the archive contains a pack"; rc=1
else echo "ok   no pack in the archive"; fi
if grep -q "^$PROGRAM $VERSION " "$R/out.txt"; then echo "ok   Version reports $PROGRAM $VERSION"
else echo "FAIL Version did not report $PROGRAM $VERSION"; rc=1; fi
if grep -q "max ticks reached" "$R/out.txt"; then
    echo "ok   the unpacked program ran to MAXTICKS"
else
    echo "FAIL the unpacked program did not run to MAXTICKS"
    rc=1
fi
grep -o '[1-9][0-9]* bytes not freed by it' "$R/out.txt" | sed 's/^/note the engine left /' || true
exit $rc
