#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The presentation test of an engine's Amiga-family program on a machine, against its headless
build (backends/common/ptest.h): PRESENTTEST on the machine, --present-test on the host, every
snapshot compared (README.md).

    present.py aros   --host-bin H --program P --pak PAK [--out DIR] [--guest-args A] [--name N]
    present.py winuae --host-bin H --program P --pak PAK [--out DIR] [--configs C] [--port N]
                      [--timeout S] [--real-time] [--guest-args A]
    present.py warpos --host-bin H --program P --pak PAK [--out DIR] [--configs C] [--timeout S]

aros: hosted AROS (aros/aros.sh); the program reads its screen back (snap_aros.c), each snapshot's
frame hash and picture must be the reference's. winuae: the 68k program in WinUAE in each
configuration (lrvrig/winuae.py; default native,rtg,promote,uaegfx; "scaled" and "window" name
groups), in parallel, in warp (--real-time throttles; the results are the same), one gdb port
each from --port; the frame hash, the screenshot at the calibrated position (saved in
DIR/calibration.json for later checks) and the displayed bitmap read from guest memory
(lrvrig/oracle.py). warpos: the PPC program the same way, one configuration at a time
(run-wos.sh, real time; promote is not available there).

--guest-args adds program options (WINDOW, SCALE=2, ...). The program finds its pack through
PAK=PROGDIR:<name>. --mode NAME runs the test in a machine's mode instead of the engine's
(PRESENTTESTMODE / --present-test-mode), with no pack. Exit status 1 when anything differs.
Needs Python 3; winuae and warpos also numpy and Pillow."""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lrvrig import aros, frames, winuae                    # noqa: E402


def guest_args(args, pak_path):
    a = 'PRESENTTEST'
    if args.mode:
        a += ' PRESENTTESTMODE=' + args.mode
    else:
        a += ' PAK=' + pak_path
    return a + (' ' + args.guest_args if args.guest_args else '')


def cmd_aros(args, ref):
    prog = os.path.basename(args.program)
    name = args.name or 'present-' + prog
    files = [args.program] + ([] if args.mode else [args.pak])
    t0 = time.time()
    run = aros.run(name, files, ['!MakeDir Work:snaps',
                                 'Work:%s %s SNAPDIR=Work:snaps/' % (
                                     prog, guest_args(args, 'Work:' + os.path.basename(args.pak or '')))],
                   prune=True)
    print('hosted AROS run: %.0f s' % (time.time() - t0))
    snapdir = run.work('snaps')
    reqs = aros.requests(snapdir)
    label = 'hosted' + ('-' + args.guest_args.replace(' ', '-') if args.guest_args else '')
    if any('sx' in q or q.get('window') == '1' for q in reqs):
        # a scaled or windowed picture: the WinUAE oracle's expansion, at (0, 0) of the PPM
        from lrvrig import oracle
        problems = []
        for q in reqs:
            rec = dict(q, png=q['image']) if 'image' in q else dict(q, error='no image read back')
            frame = ref.get(q.get('name'))
            if frame is None:
                problems.append('%s %s: no reference snapshot' % (label, q.get('name')))
                continue
            problems += oracle.check(frame, rec, snapdir, (0, 0), os.path.join(args.out, 'diff', label), label)
        missing = sorted(set(ref) - {q.get('name') for q in reqs})
        if missing:
            problems.append('%s: %d snapshots missing' % (label, len(missing)))
    else:
        problems = aros.compare(ref, reqs, snapdir, label)
    if run.problem:
        problems.insert(0, run.problem)
    for line in aros.summary(run.out):
        print('    ' + line)
    return winuae.report({label: (len(reqs), (0, 0), problems)})


def cmd_winuae(args, ref):
    prog = os.path.basename(args.program)
    procs, dirs = [], {}
    for i, cfg in enumerate(args.configs):
        run_name = 'present-%s-%s' % (prog, cfg)
        dirs[cfg] = winuae.snapdir(run_name)
        procs.append(winuae.start_run(args.program, cfg, run_name, args.port + i,
                                      guest_args(args, 'PROGDIR:' + os.path.basename(args.pak or '')),
                                      args.timeout, os.path.join(args.out, run_name + '.log'),
                                      guest_files=[] if args.mode else [args.pak],
                                      warp=not args.real_time))
    t0 = time.time()
    winuae.wait_runs(procs)
    print('emulator runs: %.0f s' % (time.time() - t0))
    calib = winuae.load_calib(args.out)
    results = {cfg: winuae.check_run('present', cfg, dirs[cfg], ref, args.out, calib)
               for cfg in args.configs}
    winuae.save_calib(args.out, calib)
    return winuae.report(results)


# run-wos.sh settings per configuration: (WOS_RTG, WOS_WB, extra program arguments)
WARPOS = {'native': (0, None, ''), 'rtg': (1, None, ''), 'uaegfx': (1, None, ' MODE=0x50031000'),
          'native-aspect': (0, None, ' ASPECT'), 'uaegfx-x2': (1, None, ' SCALE=2'),
          'uaegfx-x2a': (1, None, ' SCALE=2 ASPECT')}
for _c, _m in winuae.WB_MODE.items():
    WARPOS[_c] = (1 if 'rtg' in _c else 0, _m, winuae.VARIANT[_c][1])


def cmd_warpos(args, ref):
    import subprocess
    prog = os.path.basename(args.program)
    calib = winuae.load_calib(args.out)
    results = {}
    for cfg in args.configs:
        if cfg not in WARPOS:
            print('FAIL %s: not available on WarpOS' % cfg)
            results[cfg] = (0, None, ['%s: not available on WarpOS' % cfg])
            continue
        rtg, wb, extra = WARPOS[cfg]
        name = 'present-%s-%s' % (prog, cfg)
        env = dict(os.environ, EMU_TIMEOUT=str(args.timeout),
                   WOS_FILES=' '.join(os.path.abspath(f) for f in [args.program] + ([] if args.mode else [args.pak])))
        if rtg:
            env['WOS_RTG'] = '1'
        if wb:
            env['WOS_WB'] = wb
        cmd = 'DH0:%s %s%s' % (prog, guest_args(args, 'DH0:' + os.path.basename(args.pak or '')), extra)
        t0 = time.time()
        with open(os.path.join(args.out, 'wos-' + name + '.log'), 'w') as log:
            subprocess.run([os.path.join(winuae.RIG, 'run-wos.sh'), name, 'wb'], input=cmd + '\n',
                           text=True, env=env, stdout=log, stderr=subprocess.STDOUT)
        print('%s: emulator run %.0f s' % (cfg, time.time() - t0))
        results[cfg] = winuae.check_run('present', cfg, winuae.snapdir('wos-' + name), ref, args.out, calib)
    winuae.save_calib(args.out, calib)
    return winuae.report(results)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('machine', choices=('aros', 'winuae', 'warpos'))
    ap.add_argument('--host-bin', required=True, help='the headless build')
    ap.add_argument('--program', required=True, help='the program for the machine')
    ap.add_argument('--pak', help='the pack (not with --mode)')
    ap.add_argument('--mode', help='a machine\'s mode instead of the engine\'s (no pack)')
    ap.add_argument('--out', default='present', help='results: diffs, calibration.json, logs')
    ap.add_argument('--guest-args', default='', help='more program options, e.g. "WINDOW"')
    ap.add_argument('--name', help='aros: the run\'s name (default present-<program>)')
    ap.add_argument('--configs', default=None, help='winuae/warpos: configurations')
    ap.add_argument('--port', type=int, default=2351, help='winuae: first gdb port')
    ap.add_argument('--timeout', type=int, default=None, help='a run\'s backstop, seconds')
    ap.add_argument('--real-time', action='store_true', help='winuae: throttled emulation')
    args = ap.parse_args()
    if not args.mode and not args.pak:
        ap.error('--pak or --mode')
    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)
    try:
        args.configs = winuae.configs(args.configs or ('native' if args.machine == 'warpos'
                                                       else ','.join(winuae.CONFIGS)))
    except ValueError as e:
        ap.error(str(e))
    args.timeout = args.timeout or (1800 if args.machine == 'warpos' else 600)
    ref = frames.present_reference(args.host_bin, args.pak, args.mode)
    if not ref:
        sys.exit('present.py: the headless build printed no snapshots')
    return {'aros': cmd_aros, 'winuae': cmd_winuae, 'warpos': cmd_warpos}[args.machine](args, ref)


if __name__ == '__main__':
    sys.exit(main())
