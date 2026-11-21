#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The library's version (include/lrvne/core.h LRV_VERSION) against the release tags (vX.Y.Z):
# at a tagged commit it is the tag's; elsewhere it is not older than the newest release tag
# among the commit's ancestors (a release that forgot to raise it). Outside a git checkout, or
# without tags, there is nothing to compare with.
set -eu
cd "$(dirname "$0")/.."
v=$(sed -n 's/^#define LRV_VERSION *"\([0-9]*\.[0-9]*\.[0-9]*\)".*/\1/p' include/lrvne/core.h)
[ -n "$v" ] || { echo "check-version: no LRV_VERSION in include/lrvne/core.h"; exit 1; }
git rev-parse --git-dir >/dev/null 2>&1 || { echo "check-version: $v (no git: not compared)"; exit 0; }
at=$(git tag --points-at HEAD --list 'v[0-9]*.[0-9]*.[0-9]*' | head -1)
if [ -n "$at" ]; then
    [ "$at" = "v$v" ] || { echo "check-version: HEAD is $at but LRV_VERSION is $v"; exit 1; }
    echo "check-version: $v, the tag of this commit"
    exit 0
fi
last=$(git describe --tags --abbrev=0 --match 'v[0-9]*.[0-9]*.[0-9]*' 2>/dev/null || true)
[ -n "$last" ] || { echo "check-version: $v (no release tag yet)"; exit 0; }
older=$(printf '%s\n%s\n' "${last#v}" "$v" | sort -t. -k1,1n -k2,2n -k3,3n | head -1)
if [ "$v" != "${last#v}" ] && [ "$older" = "$v" ]; then
    echo "check-version: LRV_VERSION $v is older than the release $last before this commit"
    exit 1
fi
if [ "$v" = "${last#v}" ]; then
    echo "check-version: LRV_VERSION $v is the release $last's: raise it for the next release"
    exit 1
fi
echo "check-version: $v, after the release $last"
