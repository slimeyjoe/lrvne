# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs of an Amiga program in WinUAE (../winuae/run-program.sh, run-wos.sh) and the check of what
it showed there (oracle.py): the configurations of the Amiga backend's screen paths, starting
runs in parallel (one gdb port each), their outcome, the calibration of the picture's position,
and the per-configuration report. ../present.py is the presentation test built on this; an
engine's own checks of scripted game runs use the same pieces.

Configurations: native (AGA HiRes Laced, no Picasso96), rtg (Picasso96 loaded, the program on
the native mode), promote (FAKENATIVEMODES: the HiRes Laced mode promoted to a uaegfx screen),
uaegfx (MODE= the uaegfx 640x480x8 board mode). The scaled ones (SCALED, "scaled"):
native-aspect (ASPECT on PAL HiRes Laced: 640x480), promote-aspect, uaegfx-x2 (SCALE=2: a
board mode found by the program), uaegfx-x2a (SCALE=2 ASPECT). The windows on a Workbench
(WINDOWED, "window"): wb-rtg8 (800x600 8-bit), wb-rtg16 (1024x768 16-bit, ASPECT), wb-rtg32x2
(1280x1024 32-bit, SCALE=2: a borderless window), wb-aga256 (PAL HiRes Laced, 256 colours: a
borderless window), wb-aga8 (8 colours: no free pens, the program must open its own screen)."""
import json
import os
import signal
import subprocess

from . import conf, oracle

HERE = os.path.dirname(os.path.abspath(__file__))
RIG = os.path.abspath(os.path.join(HERE, '..', 'winuae'))
WORKTOP = conf.get('RIG_WORKTOP') or os.path.join(conf.get('EMU', os.path.expanduser('~/amiga-emu')),
                                                  'aos', 'work', 'rig')

CONFIGS = ('native', 'rtg', 'promote', 'uaegfx')
SCALED = ('native-aspect', 'promote-aspect', 'uaegfx-x2', 'uaegfx-x2a')
WINDOWED = ('wb-rtg8', 'wb-rtg16', 'wb-rtg32x2', 'wb-aga256', 'wb-aga8')
ALL = CONFIGS + SCALED + WINDOWED
# run-program.sh variant and extra program arguments per configuration
VARIANT = {'native': ('native', ''), 'rtg': ('rtg', ''), 'promote': ('promote', ''),
           'uaegfx': ('rtg', ' MODE=0x50031000'),
           'native-aspect': ('native', ' ASPECT'), 'promote-aspect': ('promote', ' ASPECT'),
           'uaegfx-x2': ('rtg', ' SCALE=2'), 'uaegfx-x2a': ('rtg', ' SCALE=2 ASPECT'),
           'wb-rtg8': ('rtg', ' WINDOW'), 'wb-rtg16': ('rtg', ' WINDOW ASPECT'),
           'wb-rtg32x2': ('rtg', ' WINDOW SCALE=2'), 'wb-aga256': ('native', ' WINDOW'),
           'wb-aga8': ('native', ' WINDOW')}
# the Workbench's mode and depth (run-program.sh WB=) of the window configurations
WB_MODE = {'wb-rtg8': '0x50041000:8', 'wb-rtg16': '0x50051100:16', 'wb-rtg32x2': '0x50071303:24',
           'wb-aga256': '0x29004:8', 'wb-aga8': '0x29004:3'}
# where the 640x400 picture is in a screenshot when no calibration says otherwise
DEFAULT_GEOM = {'native': (76, 39), 'rtg': (76, 39), 'promote': (0, 0), 'uaegfx': (0, 0),
                'native-aspect': (76, 39), 'promote-aspect': (0, 0), 'uaegfx-x2': (0, 112),
                'uaegfx-x2a': (0, 32),
                'wb-rtg8': (0, 0), 'wb-rtg16': (0, 0), 'wb-rtg32x2': (0, 0), 'wb-aga256': (76, 39),
                'wb-aga8': (76, 39)}


def configs(spec):
    """A comma list of configuration names, 'scaled' and 'window' standing for their groups."""
    names = []
    for c in spec.split(','):
        names += list(SCALED) if c == 'scaled' else list(WINDOWED) if c == 'window' else [c] if c else []
    for c in names:
        if c not in ALL:
            raise ValueError('unknown configuration %s' % c)
    return names


def start_run(program, cfg, run_name, port, args, timeout, log_path, guest_files=(), saves=None,
              saves_dir=None, warp=True, drawer=None):
    """Starts run-program.sh for one configuration; returns the process. args: the program's
    arguments, the configuration's own are added."""
    env = dict(os.environ)
    variant, extra = VARIANT[cfg]
    env.update(RUN=run_name, GDB_PORT=str(port), PROGRAM_ARGS=args + extra,
               EMU_TIMEOUT=str(timeout))
    if cfg in WB_MODE:
        env['WB'] = WB_MODE[cfg]
    if guest_files:
        env['GUEST_FILES'] = ' '.join(os.path.abspath(f) for f in guest_files)
    if warp:
        env['WARP'] = '1'
    if saves:
        env['GUEST_SAVES'] = os.path.abspath(saves)
    if saves_dir:
        env['SAVES_DIR'] = saves_dir
    if drawer:
        env['DRAWER'] = drawer
    log = open(log_path, 'w')
    return subprocess.Popen([os.path.join(RIG, 'run-program.sh'), os.path.abspath(program), variant],
                            env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)


def wait_runs(procs):
    """Waits for the runs; on an interrupt stops them (run-lib.sh stops its emulator on TERM)."""
    try:
        for p in procs:
            p.wait()
    except BaseException:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            p.wait()
        raise


def snapdir(run_name):
    return os.path.join(WORKTOP, run_name, 'snaps')


def run_outcome(run_dir):
    """The emulator's exit status as run-lib.sh emu_run recorded it (emu.rc next to snaps/), as
    a problem, or None."""
    try:
        with open(os.path.join(os.path.dirname(os.path.abspath(run_dir)), 'emu.rc')) as f:
            rc = int(f.read().strip())
    except (OSError, ValueError):
        return None
    if rc in (124, 137):
        return 'emulator timeout (the backstop): the run did not end'
    if rc:
        return 'emulator exit status %d' % rc
    return None


def check_run(kind, cfg, run_dir, ref, out, calib):
    """(snapshots checked, picture position, problems) of one configuration's run: its outcome,
    every snapshot of the manifest against ref ({name: frame with idx, rgb(), hash}), and the
    reference snapshots missing. kind 'present' calibrates the position from the presentation
    test's registration snapshot (00-...) and records it in calib."""
    recs = oracle.load_manifest(run_dir)
    problems = []
    outcome = run_outcome(run_dir)
    if outcome:
        problems.append('%s: %s' % (cfg, outcome))
    geom = calib.get(cfg)
    reg = [r for r in recs if r.get('name', '').startswith('00-')]
    if any(r.get('window') == '1' for r in recs):
        # a window: the screen's position (no registration image on a Workbench); the
        # oracle adds the window's inside from each request
        geom = DEFAULT_GEOM[cfg]
    elif kind == 'present':
        if reg and 'png' in reg[0]:
            try:
                sx, dst_h = oracle.scale_of(reg[0])
                geom = oracle.calibrate(oracle.load_rgb(os.path.join(run_dir, reg[0]['png'])),
                                        640 * sx, dst_h)
                calib[cfg] = geom
            except ValueError as e:
                problems.append('%s: calibration: %s' % (cfg, e))
    if geom is None:
        geom = DEFAULT_GEOM[cfg]
        problems.append('%s: no calibration (run the presentation test first), using %s' % (cfg, geom))
    seen = set()
    for rec in recs:
        name = rec.get('name')
        if name is None:
            problems += oracle.check(None, rec, run_dir, geom, None, cfg)
            continue
        frame = ref.get(name)
        if frame is None:
            problems.append('%s %s: no reference snapshot' % (cfg, name))
            continue
        seen.add(name)
        problems += oracle.check(frame, rec, run_dir, geom, os.path.join(out, 'diff', cfg), cfg)
    missing = sorted(set(ref) - seen)
    if missing:
        problems.append('%s: %d snapshots missing (%s%s)' % (cfg, len(missing), ', '.join(missing[:5]),
                                                             ' ...' if len(missing) > 5 else ''))
    return len(seen), geom, problems


def report(results):
    """Prints PASS/FAIL per configuration; 1 when any failed."""
    failed = 0
    for cfg, (n, geom, problems) in results.items():
        if problems:
            failed += 1
            print('FAIL %s (%d snapshots, image at %s)' % (cfg, n, geom))
            for p in problems[:60]:
                print('    ' + p)
            if len(problems) > 60:
                print('    ... %d more' % (len(problems) - 60))
        else:
            print('PASS %s (%d snapshots, image at %s)' % (cfg, n, geom))
    return 1 if failed else 0


def load_calib(out):
    path = os.path.join(out, 'calibration.json')
    if os.path.exists(path):
        with open(path) as f:
            return {k: tuple(v) for k, v in json.load(f).items()}
    return {}


def save_calib(out, calib):
    with open(os.path.join(out, 'calibration.json'), 'w') as f:
        json.dump(calib, f, indent=1)
