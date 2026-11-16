#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Snapshot server for the WinUAE runs (started by winuae/run-lib.sh emu_run for every run). It watches the guest directories for requests and answers each one through the
WinUAE gdbserver: it attaches (the CPU stops; the stop reply says so), does the work, detaches
(the CPU goes on) and creates the acknowledgement the guest waits for.

  snap_<name>.req   from the program (backends/amiga/core/snap.c): a screenshot, the
                    displayed screen bitmap read from guest memory, a manifest line; output
                    in --out: <name>.png, <name>.bitmap.npy (absent when the bitmap cannot be
                    located) and manifest.jsonl (the request's fields plus file names and
                    errors)
  shot_<name>.req   from an engine's own probe programs: a screenshot only, --shots/<name>.png

Requests appear by rename (snap.c writes .tmp first) or empty (the probes), so a request seen
is a whole one; nothing here sleeps or times out. The directories are polled (a host-side
wait for a condition: the guest's files are on the host); the guest waits for its ack as long
as it takes, so the poll interval only adds latency. The server ends with the emulator (--pid).

    snapd.py --port 2345 --dir WORK/dh0/DRAWER --dir WORK/dh0 --out WORK/snaps \\
        --shots WORK/shots --pid EMULATOR_PID
"""
import argparse
import glob
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lrvrig import gdb as uae                               # noqa: E402

POLL = 0.1          # seconds between looks at the directories (latency only)


def handle_snap(req_path, args):
    import numpy as np                                      # only the game's snapshots need it
    with open(req_path) as f:
        line = f.read().strip()
    d = uae.parse_request(line)
    name = d.get('name', os.path.basename(req_path)[5:-4])
    rec = dict(d)
    png = os.path.join(args.out, name + '.png')
    os.makedirs(args.out, exist_ok=True)
    g = uae.Gdb(args.port)
    try:
        g.monitor('screenshot ' + png)
        rec['png'] = os.path.basename(png)
        rows = int(d.get('rows', '400'))
        rows = min(rows, 400)
        # SCALE/ASPECT (snap.c): the page is dsth rows of 640 * sx pixels at dx,dy
        width, dx, dy = 640, 0, 0
        if 'sx' in d:
            width, rows = 640 * int(d['sx']), int(d['dsth'])
            dx, dy = int(d['dx']), int(d['dy'])
        try:
            if d.get('standard') == '1':
                bpr = int(d['bpr'])
                planes = [int(x, 16) + dy * bpr + dx // 8 for x in d['planes'].split(',')]
                lo = min(planes)
                hi = max(planes) + bpr * (rows - 1) + width // 8
                mem = g.read(lo, hi - lo)
                idx = uae.planar_indices(mem, lo, planes, bpr, rows, width)
            elif 'p96mem' in d and d.get('p96fmt', '1') != '1':
                idx = None                          # a true colour screen: no pens to read
                rec['bitmap_skipped'] = 'p96fmt %s' % d['p96fmt']
            elif 'p96mem' in d:
                bpr = int(d['p96bpr'])
                mem = g.read(int(d['p96mem'], 16) + dy * bpr + dx, bpr * (rows - 1) + width)
                idx = uae.chunky_indices(mem, bpr, rows, width)
            else:
                idx = None
                rec['bitmap_error'] = 'no planar bitmap and no Picasso96API'
            if idx is not None:
                npy = os.path.join(args.out, name + '.bitmap.npy')
                np.save(npy, idx)
                rec['bitmap'] = os.path.basename(npy)
        except (uae.GdbError, ValueError, KeyError) as e:
            rec['bitmap_error'] = str(e)
    finally:
        g.detach()
    with open(os.path.join(args.out, 'manifest.jsonl'), 'a') as f:
        f.write(json.dumps(rec) + '\n')
    print('snapd: %s' % name, flush=True)


def handle_shot(req_path, args):
    name = os.path.basename(req_path)[5:-4]
    os.makedirs(args.shots, exist_ok=True)
    g = uae.Gdb(args.port)
    try:
        g.monitor('screenshot ' + os.path.join(args.shots, name + '.png'))
    finally:
        g.detach()
    print('snapd: shot %s' % name, flush=True)


def answer(req, args):
    """handles one request and acknowledges it, also when it failed (the guest goes on)"""
    snap = os.path.basename(req).startswith('snap_')
    try:
        (handle_snap if snap else handle_shot)(req, args)
    except Exception as e:
        print('snapd: %s: %s' % (req, e), file=sys.stderr, flush=True)
        if snap:
            os.makedirs(args.out, exist_ok=True)
            with open(os.path.join(args.out, 'manifest.jsonl'), 'a') as f:
                f.write(json.dumps({'req': os.path.basename(req), 'error': str(e)}) + '\n')
    try:
        os.unlink(req)
        open(req[:-4] + '.ack', 'w').close()
    except OSError:
        pass


def alive(pid):
    if not pid:
        return True
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, required=True)
    ap.add_argument('--dir', action='append', required=True,
                    help='a host directory the guest writes requests into (repeatable)')
    ap.add_argument('--out', required=True, help='where the snapshots go')
    ap.add_argument('--shots', help='where the probes\' screenshots go (default: --out)')
    ap.add_argument('--pid', type=int, help='the emulator: the server ends with it')
    args = ap.parse_args()
    args.shots = args.shots or args.out
    while alive(args.pid):
        reqs = sorted(r for d in args.dir for p in ('snap_*.req', 'shot_*.req')
                      for r in glob.glob(os.path.join(d, p)))
        for req in reqs:
            answer(req, args)
        if not reqs:
            time.sleep(POLL)


if __name__ == '__main__':
    main()
