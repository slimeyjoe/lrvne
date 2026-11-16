# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of lrvne.pmd: the song builder, the test songs and table, the table out of PMD.COM."""
import hashlib
import os
import struct
import tempfile
import unittest

from lrvne import pmd

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tests', 'data', 'pmd')


class SongTest(unittest.TestCase):
    def test_layout(self):
        song = pmd.build_song({'A': b'\x40\x18', 'K': b'\x00'}, rhythm=[b'\x80\x01\x06'],
                              voices=[pmd.VOICE1])
        self.assertEqual(song[0], 0)                        # the type byte: OPN
        d = song[1:]
        offs = struct.unpack_from('<11H', d, 0)
        self.assertEqual(offs[0], 0x1A)                     # A first, right after the header
        self.assertEqual(d[offs[0]:offs[0] + 3], b'\x40\x18\x80')
        for name in 'BCDEFGHIJ':                            # absent parts: only the 80h
            self.assertEqual(d[offs[pmd.PART_NAMES.index(name)]], 0x80)
        self.assertEqual(d[offs[10]:offs[10] + 2], b'\x00\x80')
        rtab, vtab = struct.unpack_from('<HH', d, 0x16)
        pat = struct.unpack_from('<H', d, rtab)[0]
        self.assertEqual(d[pat:pat + 4], b'\x80\x01\x06\xFF')
        self.assertEqual(d[vtab - 2:vtab], b'\x47\xFE')     # version 4.7 and the marker
        memo = struct.unpack_from('<H', d, vtab - 4)[0]
        self.assertEqual(d[memo:memo + 2], b'\x00\x00')
        self.assertEqual(d[vtab:vtab + 26], bytes(pmd.VOICE1))
        self.assertEqual(d[-2:], b'\x00\xFF')

    def test_no_rhythm_is_one_empty_pattern(self):
        d = pmd.build_song({})[1:]
        rtab = struct.unpack_from('<H', d, 0x16)[0]
        pat = struct.unpack_from('<H', d, rtab)[0]
        self.assertEqual(d[pat], 0xFF)


class SynthTest(unittest.TestCase):
    def test_committed_data_is_what_synth_writes(self):
        self.assertEqual(pmd.main(['check', DATA]), 0)

    def test_ssg_table_format(self):
        t = pmd.synth_ssg_table()
        self.assertLessEqual(len(t), pmd.SSG_TABLE_END - pmd.SSG_TABLE_START)
        for e in range(14):
            pri, off = struct.unpack_from('<BH', t, 3 * e)
            self.assertIn(pri, (1, 2))
            frames = 0
            while t[off] != 0xFF:                           # 11-byte frames up to FFh
                self.assertGreater(t[off], 0)
                off += 11
                frames += 1
            self.assertGreaterEqual(frames, 1)


class TableTest(unittest.TestCase):
    def test_other_files_are_refused(self):
        with self.assertRaises(ValueError):
            pmd.ssg_effect_table(b'\x00' * 20000)

    def test_slice(self):
        fake = bytes((i * 7) & 0xFF for i in range(19699))
        saved = pmd.PMD_COM_MD5
        pmd.PMD_COM_MD5 = hashlib.md5(fake).hexdigest()
        try:
            t = pmd.ssg_effect_table(fake)
        finally:
            pmd.PMD_COM_MD5 = saved
        self.assertEqual(t, fake[0x2BE9:0x2F96])
        self.assertEqual(len(t), 941)

    def test_command_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, 'PMD.COM')
            with open(src, 'wb') as f:
                f.write(b'not a driver')
            self.assertEqual(pmd.main(['ssgtab', src, os.path.join(tmp, 'out')]), 1)
            self.assertEqual(pmd.main(['synth', os.path.join(tmp, 's')]), 0)
            self.assertEqual(pmd.main(['check', os.path.join(tmp, 's')]), 0)
            with open(os.path.join(tmp, 's', 'SYNTH2.M'), 'ab') as f:
                f.write(b'\x00')
            self.assertEqual(pmd.main(['check', os.path.join(tmp, 's')]), 1)


if __name__ == '__main__':
    unittest.main()
