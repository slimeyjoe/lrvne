# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of lrvne.exepack: round trips through the test packer, both header variants, damage."""
import contextlib
import io
import os
import struct
import tempfile
import unittest

from lrvne import exepack


def sample_image() -> bytes:
    """An image with long runs (fills) and varied bytes (copies) in between."""
    parts = [bytes(40), bytes(range(1, 200)), b'\xAA' * 300, b'text' * 7, b'\xFF' * 20,
             bytes((i * 37) & 0xFF for i in range(500)), bytes(64)]
    return b''.join(parts)


def mz_parts(mz: bytes):
    hsize = struct.unpack_from('<H', mz, 8)[0] * 16
    nrel, _h, minalloc, maxalloc, ss, sp, _ck, ip, cs, relpos = struct.unpack_from('<10H', mz, 6)
    relocs = [struct.unpack_from('<HH', mz, relpos + 4 * i) for i in range(nrel)]
    return mz[hsize:], relocs, (cs, ip), (ss, sp), (minalloc, maxalloc)


class RoundTrip(unittest.TestCase):
    def check(self, **kw):
        image = sample_image()
        relocs = [(0x0010, 0x0000), (0x01FE, 0x0000), (0x0002, 0x1000), (0x8000, 0xF000)]
        packed = exepack.pack(image, relocs, cs_ip=(0x0012, 0x0034), ss_sp=(0x0056, 0x0100),
                              min_alloc=0x20, max_alloc=0xFFFF, **kw)
        self.assertTrue(exepack.is_exepack(packed))
        self.assertLess(len(packed), len(image))
        out, info = exepack.unpack(packed)
        got, got_rel, cs_ip, ss_sp, alloc = mz_parts(out)
        self.assertEqual(got, image + bytes(-len(image) % 16))
        self.assertEqual(got_rel, relocs)
        self.assertEqual(cs_ip, (0x0012, 0x0034))
        self.assertEqual(ss_sp, (0x0056, 0x0100))
        self.assertEqual(alloc, (0x20, 0xFFFF))
        self.assertEqual(info['relocs'], 4)
        self.assertEqual(info['cs:ip'], '0012:0034')
        self.assertEqual(struct.unpack_from('<H', out, 8)[0] * 16 % 512, 0)   # header size
        self.assertFalse(exepack.is_exepack(out))

    def test_plain_header(self):
        self.check()

    def test_header_with_skip_field(self):
        self.check(skip_field=True)

    def test_image_of_only_varied_bytes(self):
        image = bytes((i * 7 + (i >> 3)) & 0xFF for i in range(4096))
        out, _ = exepack.unpack(exepack.pack(image))
        self.assertEqual(mz_parts(out)[0], image)


class Errors(unittest.TestCase):
    def test_not_exepack(self):
        self.assertFalse(exepack.is_exepack(b'not an executable at all, really'))
        plain = struct.pack('<2s13H', b'MZ', 64, 1, 0, 2, 0, 0xFFFF, 0, 0x80, 0, 0, 0, 0x1C, 0)
        plain += bytes(32 - len(plain)) + bytes(32)
        self.assertFalse(exepack.is_exepack(plain))
        with self.assertRaises(ValueError):
            exepack.unpack(plain)

    def test_bad_command(self):
        packed = bytearray(exepack.pack(sample_image()))
        stub = struct.unpack_from('<H', packed, 8)[0] * 16 + struct.unpack_from('<H', packed, 0x16)[0] * 16
        end = stub
        while packed[end - 1] == 0xFF:
            end -= 1
        packed[end - 1] = 0xB7                   # the last record's command
        with self.assertRaises(ValueError):
            exepack.unpack(bytes(packed))

    def test_truncated_relocations(self):
        packed = exepack.pack(sample_image(), [(2, 0)])
        with self.assertRaises(ValueError):
            exepack.unpack(packed[:-2])

    def test_relocation_segment(self):
        with self.assertRaises(ValueError):
            exepack.pack(sample_image(), [(0, 0x0800)])


class CommandLine(unittest.TestCase):
    def test_main(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = os.path.join(d, 'IN.EXE'), os.path.join(d, 'OUT.EXE')
            with open(src, 'wb') as f:
                f.write(exepack.pack(sample_image()))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(exepack.main([src, dst]), 0)
            with open(dst, 'rb') as f:
                self.assertEqual(mz_parts(f.read())[0][:40], bytes(40))
            with open(src, 'wb') as f:
                f.write(b'MZ' + bytes(100))
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(exepack.main([src, dst]), 1)
            self.assertIn('exepack:', err.getvalue())


if __name__ == '__main__':
    unittest.main()
