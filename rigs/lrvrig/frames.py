# SPDX-License-Identifier: GPL-3.0-or-later
"""The headless backend's snapshots (backends/common/shot.c lrv_shot_emit_snap: one JSON line per
snapshot) as frames: the reference a rig compares a machine's screen with. Standard library
only; numpy is imported when an oracle asks for arrays (idx, rgb)."""
import base64
import json
import subprocess

W, H = 640, 400


class Frame:
    """One snapshot: name, tick (the `t` field: the tick, or a presentation test step's index),
    hash (lrv_frame_hash32, 8 hex digits), pal (48 bytes, 4-bit DAC values), gon, scroll, and
    the colour indices of the page in display order (two per byte in `data`)."""

    def __init__(self, d):
        self.name = d.get('name')
        self.tick = d['t']
        self.hash = d.get('hash')
        self.pal = bytes.fromhex(d['pal'])
        self.gon = d.get('gon', 1)
        self.scroll = d.get('scroll', 0)
        self.packed = base64.b64decode(d['data'])
        self._idx = None

    def rgb_bytes(self):
        """The picture as W x H RGB bytes: each DAC value times 17, black with graphics off."""
        if not self.gon:
            return bytes(W * H * 3)
        lut = [bytes(v * 17 for v in self.pal[3 * i:3 * i + 3]) for i in range(16)]
        pair = [lut[b >> 4] + lut[b & 15] for b in range(256)]
        return b''.join(pair[b] for b in self.packed)

    # ---- numpy views, for the WinUAE oracle (oracle.py) ----
    @property
    def idx(self):
        if self._idx is None:
            import numpy as np
            packed = np.frombuffer(self.packed, np.uint8)
            idx = np.empty(packed.size * 2, np.uint8)
            idx[0::2] = packed >> 4
            idx[1::2] = packed & 15
            self._idx = idx.reshape(H, W)
        return self._idx

    def rgb(self):
        import numpy as np
        if not self.gon:
            return np.zeros((H, W, 3), np.uint8)
        pal = np.array([v * 17 for v in self.pal], np.uint8).reshape(16, 3)
        return pal[self.idx]


def parse(lines):
    """The snapshots among a run's output lines, in order."""
    out = []
    for line in lines:
        if line.startswith('{"ev":"snap"'):
            out.append(Frame(json.loads(line)))
    return out


def run(cmd, cwd=None, log=None):
    """Runs the headless build; its snapshots, in order. log: a list that gets its stderr lines
    (a run ended by --max-ticks exits with 3, which is not an error here)."""
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if log is not None:
        log += p.stderr.splitlines()
    return parse(p.stdout.splitlines())


def present_key(frame):
    """A presentation test snapshot's name as the machine's request names it: NN-name."""
    return '%02d-%s' % (frame.tick, frame.name)


def present_reference(host_bin, pak, mode=None):
    """The presentation test on the headless build: {NN-name: Frame}. mode: a machine's mode
    (--present-test-mode), which needs no pack."""
    cmd = [host_bin] + (['--present-test-mode', mode] if mode else ['--pak', pak, '--present-test'])
    return {present_key(f): f for f in run(cmd)}
