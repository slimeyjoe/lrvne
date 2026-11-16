# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs in hosted AROS (../aros/aros.sh) and the check of what the program showed there.

On AROS the program reads its shown page back itself (backends/amiga/aros/snap_aros.c): every
snapshot is a request file in SNAPDIR (snap_<name>.req, key=value words: the frame hash the
program computed, the picture's file) and a 640x400 PPM of the screen. Each is checked against
the headless build's frame for the same snapshot (frames.py): the hash must be the reference's,
and the picture its colours, each 4-bit DAC value times 17, black with graphics off. Standard
library only (a scaled or windowed picture, sx= or window= in the request, needs the WinUAE
oracle's numpy code instead: winuae.py)."""
import glob
import os
import re
import subprocess

from . import conf

HERE = os.path.dirname(os.path.abspath(__file__))
ARGS_SH = os.path.join(HERE, '..', 'aros', 'aros.sh')
W, H = 640, 400
OUTCOMES = {3: 'AROS ended before the commands were done', 4: 'the X server could not start',
            124: 'timeout (the backstop): the run did not end'}


class Run:
    """A finished run: its directory, aros.sh's log, the commands' output (out.txt) and the
    problem when the run did not finish (None when it did)."""

    def __init__(self, rundir, status, log, out):
        self.rundir = rundir
        self.status = status
        self.log = log
        self.out = out
        self.problem = None
        if status:
            self.problem = 'hosted AROS: %s' % OUTCOMES.get(status, 'aros.sh exit %d: %s' % (
                status, log.strip()[-300:]))

    def work(self, *parts):
        """A path below the run's Work: (SYS:Work, or RUNDIR/Work after PRUNE)."""
        top = os.path.join(self.rundir, 'Work')
        if not os.path.isdir(top):
            top = os.path.join(self.rundir, 'AROS', 'Work')
        return os.path.join(top, *parts)


def runs_dir():
    """Where run directories go: RIG_RUNS, else ../build/rig-runs below the current directory."""
    return os.path.abspath(conf.get('RIG_RUNS', os.path.join('..', 'build', 'rig-runs')))


def run(name, files, commands, timeout=None, prune=False, env=None):
    """Runs the commands in a fresh hosted AROS (aros.sh hosted) in runs_dir()/name, with the
    files in Work:. timeout: the backstop in seconds (AROS_TIMEOUT, default 900)."""
    rundir = os.path.join(runs_dir(), name)
    e = dict(os.environ, AROS_FILES=' '.join(os.path.abspath(f) for f in files))
    if prune:
        e['PRUNE'] = '1'
    e.update(env or {})
    timeout = timeout or int(conf.get('AROS_TIMEOUT', '900'))
    p = subprocess.run([ARGS_SH, 'hosted', rundir, str(timeout)],
                       input='\n'.join(commands) + '\n', text=True, env=e,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        with open(os.path.join(rundir, 'out.txt'), errors='replace') as f:
            out = f.read()
    except OSError:
        out = ''
    return Run(rundir, p.returncode, p.stdout, out)


def requests(snapdir):
    """The program's snapshot requests in SNAPDIR, in name order: dicts of their key=value words."""
    reqs = []
    for p in sorted(glob.glob(os.path.join(snapdir, 'snap_*.req'))):
        with open(p) as f:
            reqs.append(dict(w.split('=', 1) for w in f.read().split() if '=' in w))
    return reqs


def read_ppm(path):
    """(width, height, RGB bytes) of a binary PPM, or (None, None, None)."""
    try:
        with open(path, 'rb') as f:
            d = f.read()
    except OSError:
        return None, None, None
    m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', d)
    if not m:
        return None, None, None
    return int(m.group(1)), int(m.group(2)), d[m.end():]


def compare(ref, reqs, snapdir, label, key=lambda q: q.get('name')):
    """Problems of the requests against the reference frames ({key: frames.Frame}): every
    reference snapshot must be there, each with the reference's hash and colours."""
    bad, seen = [], set()
    for q in reqs:
        k = key(q)
        frame = ref.get(k)
        if frame is None:
            bad.append('%s %s: no reference snapshot' % (label, q.get('name')))
            continue
        seen.add(k)
        if q.get('hash', 'none') != frame.hash:
            bad.append('%s %s: hash %s, reference %s' % (label, q.get('name'), q.get('hash'), frame.hash))
        if 'sx' in q or q.get('window') == '1':
            bad.append('%s %s: a scaled or windowed picture (winuae.py checks those)' % (label, q.get('name')))
            continue
        w, h, px = read_ppm(os.path.join(snapdir, q.get('image', '-')))
        if px is None or (w, h) != (W, H):
            bad.append('%s %s: no %dx%d picture read back' % (label, q.get('name'), W, H))
            continue
        exp = frame.rgb_bytes()
        if px != exp:
            n = sum(1 for i in range(0, len(exp), 3) if px[i:i + 3] != exp[i:i + 3])
            bad.append('%s %s: %d pixels differ' % (label, q.get('name'), n))
    missing = sorted(k for k in ref if k not in seen)
    if missing:
        bad.append('%s: %d of %d snapshots missing (%s%s)' % (label, len(missing), len(ref),
                                                           ', '.join(map(str, missing[:5])),
                                                           ' ...' if len(missing) > 5 else ''))
    return bad


def summary(out, prefixes=('stack:', 'memory:', 'exit ', 'timer:', 'max ticks', 'voice:', 'audio:')):
    """The program's closing lines worth printing."""
    return [l for l in out.splitlines() if l.startswith(prefixes)]
