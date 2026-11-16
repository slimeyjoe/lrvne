# SPDX-License-Identifier: GPL-3.0-or-later
"""The rigs' standard-library side (rigs/lrvrig): the headless build's snapshots as frames, the
AROS requests and the comparison of a read-back picture with a frame, the settings file."""
import base64
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'rigs'))
from lrvrig import aros, frames    # noqa: E402

W, H = 640, 400


def snap_line(name, t, colour, gon=1, h='0000abcd'):
    pal = bytearray(48)
    pal[3 * colour:3 * colour + 3] = bytes((15, 8, 1))
    data = bytes(((colour << 4) | colour,)) * (W * H // 2)
    return json.dumps({'ev': 'snap', 'name': name, 't': t, 'hash': h, 'size': [W, H],
                       'pal': pal.hex(), 'gon': gon, 'scroll': 0,
                       'data': base64.b64encode(data).decode()}, separators=(',', ':'))


class RigTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        import shutil
        shutil.rmtree(self.dir)

    def put(self, name, data):
        with open(os.path.join(self.dir, name), 'wb') as f:
            f.write(data)

    def test_frames(self):
        fs = frames.parse(['{"ev":"frame"}', snap_line('first', 3, 2), 'noise'])
        self.assertEqual(len(fs), 1)
        f = fs[0]
        self.assertEqual((f.name, f.tick, f.hash, f.gon), ('first', 3, '0000abcd', 1))
        self.assertEqual(frames.present_key(f), '03-first')
        self.assertEqual(f.rgb_bytes()[:6], bytes((255, 136, 17)) * 2)
        off = frames.parse([snap_line('dark', 4, 2, gon=0)])[0]
        self.assertEqual(off.rgb_bytes(), bytes(W * H * 3))

    def test_compare(self):
        f = frames.parse([snap_line('a', 1, 5)])[0]
        ref = {'a': f, 'b': frames.parse([snap_line('b', 2, 6)])[0]}
        self.put('snap_a.req', b'name=a tick=1 hash=0000abcd image=snap_a.ppm\n')
        self.put('snap_a.ppm', b'P6\n640 400\n255\n' + f.rgb_bytes())
        reqs = aros.requests(self.dir)
        self.assertEqual(reqs, [{'name': 'a', 'tick': '1', 'hash': '0000abcd', 'image': 'snap_a.ppm'}])
        bad = aros.compare(ref, reqs, self.dir, 'x')
        self.assertEqual(bad, ['x: 1 of 2 snapshots missing (b)'])
        # a wrong hash and one wrong pixel
        px = bytearray(f.rgb_bytes())
        px[3:6] = b'\0\0\0'
        self.put('snap_a.ppm', b'P6\n640 400\n255\n' + bytes(px))
        self.put('snap_a.req', b'name=a hash=12345678 image=snap_a.ppm\n')
        bad = aros.compare({'a': f}, aros.requests(self.dir), self.dir, 'x')
        self.assertEqual(bad, ['x a: hash 12345678, reference 0000abcd',
                               'x a: 1 pixels differ, the first at x 1 y 0'])
        # no picture
        self.put('snap_a.req', b'name=a hash=0000abcd\n')
        bad = aros.compare({'a': f}, aros.requests(self.dir), self.dir, 'x')
        self.assertEqual(bad, ['x a: no 640x400 picture read back'])

    def test_outcome(self):
        self.assertIsNone(aros.Run(self.dir, 0, '', '').problem)
        self.assertIn('timeout', aros.Run(self.dir, 124, '', '').problem)
        self.assertIn('AROS ended', aros.Run(self.dir, 3, '', '').problem)


if __name__ == '__main__':
    unittest.main()
