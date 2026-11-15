# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of lrvne.diskimg: image containers, the partition table, FAT12/16, the registry."""
import contextlib
import io
import os
import struct
import tempfile
import unittest

from lrvne import diskimg

from . import fixtures as fx

FILES = {
    'README.TXT': b'a small file\r\n',
    'EMPTY': b'',
    'DATA/BIG.BIN': fx.pattern(70000, 3),           # many clusters, scattered
    'DATA/SUB/DEEP.DAT': fx.pattern(3000, 5),
    'DATA/日本.TXT': b'a Shift-JIS name',
}
FLOPPY = (77, 2, 8, 1024)
HARD = (80, 4, 17, 512)


def contents(volume):
    return {e.path: e.read() for e in volume.files()}


class FloppyImages(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.raw = fx.make_floppy(FILES)

    def check(self, image, kind):
        self.assertEqual(image.kind, kind)
        self.assertEqual(image.geometry, diskimg.Geometry(*FLOPPY))
        self.assertEqual(image.data, self.raw)
        vols = diskimg.volumes(image)
        self.assertEqual(len(vols), 1)
        self.assertEqual(vols[0].label, 'TESTDISK')
        self.assertEqual(vols[0].fstype, 'fat')
        self.assertFalse(vols[0].layout.fat16)
        self.assertEqual(contents(vols[0]), FILES)
        return vols[0]

    def test_raw(self):
        self.assertEqual(len(self.raw), 1261568)
        vol = self.check(diskimg.open_image(self.raw, 'disk.hdm'), 'raw')
        entry = next(e for e in vol.files() if e.path == 'DATA/BIG.BIN')
        self.assertEqual((entry.name, entry.size, entry.mtime), ('BIG.BIN', 70000, fx.DOS_STAMP))
        self.assertEqual(vol.read('data/sub/deep.dat'), FILES['DATA/SUB/DEEP.DAT'])
        with self.assertRaises(diskimg.DiskImageError):
            vol.read('NOSUCH.FIL')

    def test_d88(self):
        image = diskimg.open_image(fx.d88_wrap(self.raw), 'disk.d88')
        self.check(image, 'd88')
        self.assertEqual(image.info['name'], 'TEST')
        self.assertEqual(len(image.sectors), 77 * 2 * 8)
        self.assertEqual(image.bad_sectors(), [])
        self.assertEqual(image.chs(0, 0, 1), self.raw[:1024])
        self.assertEqual(image.chs(1, 1, 8), self.raw[(3 * 8 + 7) * 1024:(3 * 8 + 8) * 1024])

    def test_d88_detected_without_extension_and_short_table(self):
        self.check(diskimg.open_image(fx.d88_wrap(self.raw, table_entries=160)), 'd88')

    def test_d88_two_disks_in_one_file(self):
        other = fx.make_floppy({'SECOND.TXT': b'disk two'}, label='DISK2')
        images = diskimg.open_images(fx.d88_wrap(self.raw) + fx.d88_wrap(other), 'pair.d88')
        self.assertEqual([i.label for i in images], ['pair.d88#1', 'pair.d88#2'])
        self.assertEqual(contents(diskimg.volumes(images[1])[0]), {'SECOND.TXT': b'disk two'})

    def test_d88_error_status_is_reported(self):
        d = bytearray(fx.d88_wrap(self.raw))
        first = struct.unpack_from('<I', d, 0x20)[0]
        d[first + 8] = 0xB0                         # status of the first stored sector
        image = diskimg.open_image(bytes(d), 'bad.d88')
        self.assertEqual([(s.c, s.h, s.r) for s in image.bad_sectors()], [(0, 0, 8)])

    def test_d88_with_wrong_size_field(self):
        with self.assertRaisesRegex(diskimg.DiskImageError, 'size field'):
            diskimg.open_image(fx.d88_wrap(self.raw)[:-10], 'cut.d88')

    def test_fdi(self):
        data = fx.anex_wrap(self.raw, FLOPPY)
        self.check(diskimg.open_image(data, 'disk.fdi'), 'fdi')
        self.check(diskimg.open_image(data), 'fdi')     # by its header alone

    def test_from_a_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'disk.xdf')
            with open(path, 'wb') as f:
                f.write(self.raw)
            self.check(diskimg.open_image(path), 'raw')
            with self.assertRaises(diskimg.DiskImageError):
                diskimg.open_image(os.path.join(tmp, 'absent.d88'))

    def test_raw_of_unknown_size_has_no_geometry(self):
        image = diskimg.open_image(self.raw + b'\0' * 512, 'odd.img')
        self.assertIsNone(image.geometry)
        self.assertEqual(contents(diskimg.volumes(image)[0]), FILES)
        with self.assertRaises(diskimg.DiskImageError):
            image.sector(0)

    def test_flux_images_are_refused_naming_d88(self):
        for label, data in (('disk.scp', b'\0' * 64), ('disk.hfe', b'\0' * 64),
                            ('disk.img', b'HXCPICFE' + b'\0' * 64), ('x', b'SCP' + b'\0' * 64)):
            with self.assertRaisesRegex(diskimg.DiskImageError, 'D88'):
                diskimg.open_image(data, label)

    def test_no_file_system(self):
        with self.assertRaisesRegex(diskimg.DiskImageError, 'no file system'):
            diskimg.volumes(diskimg.open_image(bytes(1261568), 'blank.hdm'))

    def test_looping_cluster_chain_ends(self):
        vol = diskimg.volumes(diskimg.open_image(self.raw, 'a.hdm'))[0]
        raw = bytearray(self.raw)
        chain = vol.chain(next(c for c in range(2, 80) if len(vol.chain(c)) > 8))
        # point the third cluster of the chain back at the first (a FAT12 entry)
        target, value = chain[2], chain[0]
        o = vol.layout.fat + target * 3 // 2
        word = struct.unpack_from('<H', raw, o)[0]
        word = (word & 0x000F) | (value << 4) if target & 1 else (word & 0xF000) | value
        struct.pack_into('<H', raw, o, word)
        damaged = diskimg.FatVolume(bytes(raw))
        self.assertEqual(damaged.chain(chain[0]), chain[:3])
        self.assertEqual(len(list(damaged.files())), len(FILES))


class HardDiskImages(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.disk = fx.make_hard_disk(FILES)

    def check(self, image, kind):
        self.assertEqual(image.kind, kind)
        self.assertTrue(image.hard_disk)
        self.assertEqual(image.geometry, diskimg.Geometry(*HARD))
        parts = image.partitions()
        self.assertEqual(len(parts), 1)
        p = parts[0]
        self.assertEqual((p.index, p.mid, p.sid, p.name), (0, 0xA1, 0xA1, 'TEST PART'))
        self.assertEqual((p.start, p.end), ((1, 0, 0), (79, 3, 16)))
        self.assertEqual(p.offset, 4 * 17 * 512)
        self.assertEqual(p.offset + p.size, len(self.disk))
        vols = diskimg.volumes(image)
        self.assertEqual(len(vols), 1)
        self.assertTrue(vols[0].layout.fat16)
        self.assertEqual(vols[0].label, 'HARDDISK')
        self.assertIn('partition at byte %d' % p.offset, vols[0].origin)
        self.assertEqual(contents(vols[0]), FILES)

    def test_hdi(self):
        self.check(diskimg.open_image(fx.anex_wrap(self.disk, HARD, media=0), 'disk.hdi'), 'hdi')

    def test_small_hard_disk_by_header_alone(self):
        # without a name the size decides between 'fdi' and 'hdi'; the partition is found anyway
        image = diskimg.open_image(fx.anex_wrap(self.disk, HARD, media=0))
        self.assertEqual(image.kind, 'fdi')
        self.assertEqual(contents(diskimg.volumes(image)[0]), FILES)
        self.check(diskimg.open_image(fx.anex_wrap(self.disk, HARD, media=0), kind='hdi'), 'hdi')

    def test_nhd(self):
        image = diskimg.open_image(fx.nhd_wrap(self.disk, HARD))
        self.check(image, 'nhd')
        self.assertEqual(image.info['comment'], 'fixture')

    def test_volume_at_an_explicit_offset(self):
        vol = diskimg.FatVolume(self.disk, 4 * 17 * 512)
        self.assertEqual(contents(vol), FILES)
        with self.assertRaises(diskimg.DiskImageError):
            diskimg.FatVolume(self.disk, 512)


class TinyVolume(diskimg.Volume):
    """A made-up file system: "TINYFS" at byte 0, then name[8], u16 size, data."""
    fstype = 'tinyfs'

    def __init__(self, image):
        self.image = image
        self.origin = image.label
        self.label = 'tiny'

    def files(self):
        d = self.image.data
        name = d[8:16].rstrip(b' ').decode('ascii')
        size = struct.unpack_from('<H', d, 16)[0]
        yield diskimg.FileEntry(name, size, reader=lambda: d[18:18 + size])


def tiny_probe(image):
    return [TinyVolume(image)] if image.data[:6] == b'TINYFS' else []


class Registry(unittest.TestCase):
    def test_an_engine_registers_its_own_file_system(self):
        fs = diskimg.default_filesystems()
        fs.register('tinyfs', tiny_probe)
        self.assertEqual(fs.names(), ['tinyfs', 'fat'])
        disk = (b'TINYFS\0\0' + b'HELLO   ' + struct.pack('<H', 5) + b'world').ljust(1261568, b'\0')
        vols = fs.volumes(diskimg.open_image(fx.d88_wrap(disk), 'own.d88'))
        self.assertEqual([(v.fstype, v.label) for v in vols], [('tinyfs', 'tiny')])
        self.assertEqual(contents(vols[0]), {'HELLO': b'world'})
        self.assertEqual(vols[0].read('hello'), b'world')
        # a FAT disk still goes to FAT, and the default registry never learns of tinyfs
        fat = fs.volumes(diskimg.open_image(fx.make_floppy({'A.TXT': b'a'}), 'fat.hdm'))
        self.assertEqual(fat[0].fstype, 'fat')
        self.assertEqual(diskimg.default_filesystems().names(), ['fat'])
        with self.assertRaises(diskimg.DiskImageError):
            diskimg.volumes(diskimg.open_image(disk, 'own.hdm'))
        with self.assertRaises(diskimg.DiskImageError):
            fs.register('fat', tiny_probe)

    def test_a_probe_that_fails_is_not_the_one(self):
        def broken(image):
            raise diskimg.DiskImageError('not mine')
        fs = diskimg.Filesystems([('broken', broken), ('fat', diskimg.fat_probe)])
        vols = fs.volumes(diskimg.open_image(fx.make_floppy({'A.TXT': b'a'}), 'fat.hdm'))
        self.assertEqual(contents(vols[0]), {'A.TXT': b'a'})


class CommandLine(unittest.TestCase):
    def run_tool(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            status = diskimg.main(list(args))
        return status, out.getvalue()

    def test_info_ls_extract_raw(self):
        raw = fx.make_floppy(FILES)
        with tempfile.TemporaryDirectory() as tmp:
            image = os.path.join(tmp, 'disk.d88')
            with open(image, 'wb') as f:
                f.write(fx.d88_wrap(raw))
            status, text = self.run_tool('info', image)
            self.assertEqual(status, 0)
            self.assertIn('77 cylinders, 2 heads, 8 sectors of 1024 bytes', text)
            self.assertIn("volume 'TESTDISK' (fat, FAT12", text)
            status, text = self.run_tool('ls', image)
            self.assertEqual(status, 0)
            self.assertIn('DATA/SUB/DEEP.DAT', text)
            status, _ = self.run_tool('extract', image, os.path.join(tmp, 'out'))
            self.assertEqual(status, 0)
            with open(os.path.join(tmp, 'out', 'DATA', 'BIG.BIN'), 'rb') as f:
                self.assertEqual(f.read(), FILES['DATA/BIG.BIN'])
            status, _ = self.run_tool('raw', image, os.path.join(tmp, 'disk.img'))
            self.assertEqual(status, 0)
            with open(os.path.join(tmp, 'disk.img'), 'rb') as f:
                self.assertEqual(f.read(), raw)
            status, text = self.run_tool('ls', os.path.join(tmp, 'absent.d88'))
            self.assertEqual(status, 1)
            self.assertIn('diskimg:', text)


if __name__ == '__main__':
    unittest.main()
