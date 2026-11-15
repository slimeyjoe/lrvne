#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Smoke test of the whole stack: runs the demo engine (examples/demo) built for the host target,
# that is with the headless backend, and checks what it prints and leaves behind.
#
#   tests/smoke.sh build/host/lrvdemo
#
# Covered: the presentation test, scripted input with snapshots and pictures, storage, pack
# discovery (--pak, <program>.pak, a pack appended to the program) and the profile in the
# pack, the control protocol, the tick limit, offline sound, the command line's errors.
set -eu

exe=${1:?usage: smoke.sh PATH-TO-lrvdemo}
case "$exe" in /*) ;; *) exe=$PWD/$exe ;; esac
tmp=$(mktemp -d "${TMPDIR:-/tmp}/lrvne-smoke-XXXXXX")
trap 'rm -rf "$tmp"' EXIT

fail() {
    echo "smoke: FAILED: $*" >&2
    exit 1
}

# hash_of FILE NAME: the hash of the snapshot NAME among the lines of FILE
hash_of() {
    sed -n "s/.*\"ev\":\"snap\",\"name\":\"$2\".*\"hash\":\"\\([0-9a-f]*\\)\".*/\\1/p" "$1"
}

# ---- the presentation test: 25 snapshots with the hashes every backend must reproduce -------
"$exe" --present-test >"$tmp/ptest" || fail "--present-test exited with $?"
[ "$(wc -l <"$tmp/ptest")" -eq 25 ] || fail "--present-test: not 25 snapshots"
[ "$(hash_of "$tmp/ptest" registration)" = 89dc5ad9 ] || fail "--present-test: first hash"
[ "$(hash_of "$tmp/ptest" soak-4)" = 91c98518 ] || fail "--present-test: last hash"

# ---- scripted input, snapshots, pictures, a storage record ----------------------------------
mkdir "$tmp/store" "$tmp/shots"
spec="at:5=a,12=space,30=esc snap:10,20"
run_demo() {    # run_demo OUT [OPTION...]: a run that ends by itself at tick 30
    out=$1
    shift
    "$exe" --storage "$tmp/store" --autoplay "$spec" "$@" \
        >"$out" 2>"$out.err" || fail "autoplay run exited with $? ($*)"
}
run_demo "$tmp/run1" --shot-every 10 --shot-dir "$tmp/shots" --memstat
[ "$(wc -l <"$tmp/run1")" -eq 2 ] || fail "autoplay: not 2 snapshots"
[ -n "$(hash_of "$tmp/run1" t10)" ] && [ -n "$(hash_of "$tmp/run1" t20)" ] ||
    fail "autoplay: snapshots t10 and t20 missing"
grep -q '^memory: peak [1-9][0-9]* bytes' "$tmp/run1.err" || fail "--memstat printed nothing"
for t in 00000000 00000010 00000020; do
    f=$tmp/shots/shot_$t.ppm
    [ -f "$f" ] || fail "picture $f missing"
    [ "$(head -c 15 "$f" | tr '\n' ' ')" = "P6 640 400 255 " ] || fail "picture $f: header"
    [ "$(wc -c <"$f")" -eq 768015 ] || fail "picture $f: size"
done
[ "$(od -An -tu1 "$tmp/store/runs" | tr -s ' ')" = " 1 0 0 0" ] || fail "storage: first run"
# the same input again: the same frames, and the record counts the second run
run_demo "$tmp/run2"
[ "$(hash_of "$tmp/run1" t20)" = "$(hash_of "$tmp/run2" t20)" ] || fail "runs differ"
[ "$(od -An -tu1 "$tmp/store/runs" | tr -s ' ')" = " 2 0 0 0" ] || fail "storage: second run"

# ---- the pack and its profile ---------------------------------------------------------------
# A pack with one entry, "profile" (lrvne/pack.h): id=smoke, title, speed=3. With speed 3 the
# picture at tick 20 differs from the default's and equals the one --speed 3 gives.
{
    printf 'XPAK\002\000\001\000\124\000\000\000\000\000\000\000'
    printf 'profile\000\000\000\000\000\000\000\000\000\000\000\000\000'
    printf '\001\000\000\000\060\000\000\000\042\000\000\000'
    printf 'id=smoke\ntitle=Smoke test\nspeed=3\n\000\000'
} >"$tmp/game.pak"
[ "$(wc -c <"$tmp/game.pak")" -eq 84 ] || fail "the test pack is not 84 bytes"
run_demo "$tmp/speed3" --speed 3
want=$(hash_of "$tmp/speed3" t20)
[ "$want" != "$(hash_of "$tmp/run1" t20)" ] || fail "--speed changed nothing"
run_demo "$tmp/pak" --pak "$tmp/game.pak"
[ "$(hash_of "$tmp/pak" t20)" = "$want" ] || fail "--pak: the profile was not used"
# a renamed copy of the program finds <its name>.pak next to itself, from any directory
mkdir "$tmp/install"
cp "$exe" "$tmp/install/mygame"
cp "$tmp/game.pak" "$tmp/install/mygame.pak"
(cd "$tmp/store" && "$tmp/install/mygame" --autoplay "$spec" >"$tmp/found") ||
    fail "the copy next to its pack exited with $?"
[ "$(hash_of "$tmp/found" t20)" = "$want" ] || fail "<program>.pak was not found"
# and a pack appended to the program is found before any file
{
    cat "$exe" "$tmp/game.pak"
    printf 'XPKT\124\000\000\000\253\377\377\377XPKT'
} >"$tmp/install/bundled"
chmod +x "$tmp/install/bundled"
(cd "$tmp/store" && "$tmp/install/bundled" --autoplay "$spec" >"$tmp/bundled") ||
    fail "the program with a bundled pack exited with $?"
[ "$(hash_of "$tmp/bundled" t20)" = "$want" ] || fail "the bundled pack was not used"
rm -f "$tmp/store/runs"

# ---- the control protocol --------------------------------------------------------------------
"$exe" --control --storage "$tmp/store" >"$tmp/ctl" <<'EOF' || fail "control session exited with $?"
break key
run 3
press a,ok
hold shift 1
press b
run
nonsense
mouse 100 50
press mouse_l
unbreak all
run 2
snap here
readback here
press cancel
run
EOF
expect() {      # expect LINE-NUMBER PATTERN
    line=$(sed -n "$1p" "$tmp/ctl" | cut -c1-200)
    printf '%s\n' "$line" | grep -q "$2" || fail "control: line $1 is not $2: $line"
}
expect 1 '^{"ev":"record","t":0,.*"name":"runs",.*"a":\[1,0,0,0\]}$'
expect 2 '^{"ev":"wait","t":0,.*"a":\[0,0,0,0\]}$'
expect 3 '^{"ev":"stop","t":1,"why":"start","wait":0,"pending":0}$'
expect 4 '^{"ev":"ok"}$'
expect 5 '^{"ev":"stop","t":4,"why":"ticks",'
expect 9 '^{"ev":"key","t":4,.*"name":"a",.*"a":\[[0-9]*,97,0,0\]}$'
expect 10 '"name":"return",.*,13,0,0\]}$'
expect 11 '"name":"b",.*"a":\[[0-9]*,66,1,0\]}$'
expect 12 '^{"ev":"stop","t":5,"why":"break",'
expect 13 '^{"ev":"error","msg":"unknown command"}$'
expect 17 '^{"ev":"stop","t":7,"why":"ticks",'
expect 18 '^{"ev":"snap","name":"here","t":7,"hash":"[0-9a-f]\{8\}","size":\[640,400\],"pal":"'
expect 20 '^{"ev":"error","msg":"no readback on this backend"}$'
expect 22 '"name":"esc",.*"a":\[[0-9]*,27,1,0\]}$'
expect 23 '^{"ev":"wait","t":7,.*"a":\[0,1,0,0\]}$'
expect 24 '^{"ev":"quit","t":7,'
expect 25 '^{"ev":"exit","code":0,"msg":"engine returned"}$'
[ "$(wc -l <"$tmp/ctl")" -eq 25 ] || fail "control: not 25 lines"
# a driver that goes away ends the engine, with its own exit code
rc=0
"$exe" --control --storage "$tmp/store" </dev/null >"$tmp/ctl2" || rc=$?
[ "$rc" -eq 4 ] || fail "control: a closed pipe gave exit code $rc"

# ---- limits, sound, errors --------------------------------------------------------------------
rc=0
"$exe" --storage "$tmp/store" --max-ticks 50 >/dev/null 2>&1 || rc=$?
[ "$rc" -eq 3 ] || fail "--max-ticks: exit code $rc, not 3"
"$exe" --render-wav "$tmp/tone.wav" --song 440 --seconds 0.1 2>/dev/null ||
    fail "--render-wav exited with $?"
[ "$(wc -c <"$tmp/tone.wav")" -eq 8864 ] || fail "--render-wav: size"
"$exe" --storage "$tmp/store" --wav "$tmp/game.wav" --autoplay "at:5=a,30=esc" 2>/dev/null ||
    fail "--wav run exited with $?"
[ "$(head -c 4 "$tmp/game.wav")" = RIFF ] && [ "$(wc -c <"$tmp/game.wav")" -gt 20000 ] ||
    fail "--wav: no sound recorded"
for bad in "--nonsense" "--speed 9" "--autoplay at:5=nokey" "--render-wav $tmp/x.wav"; do
    rc=0
    # shellcheck disable=SC2086
    "$exe" --storage "$tmp/store" $bad >/dev/null 2>&1 || rc=$?
    [ "$rc" -eq 2 ] || fail "'$bad': exit code $rc, not 2"
done
"$exe" --help | grep -q -- '--speed 1\.\.8' || fail "--help does not list the engine's option"

echo "smoke: ok"
