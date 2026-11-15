# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of lrvne.source: directories, archives, nested containers, member addressing."""
import contextlib
import io
import os
import shutil
import subprocess
import tempfile
import unittest
import zipfile

from lrvne import diskimg, source

from . import fixtures as fx
from .test_diskimg import tiny_probe

DISK_A = {'A.TXT': b'on disk a', 'DIR/SHARED.DAT': fx.pattern(2000, 1)}
DISK_B = {'B.TXT': b'on disk b', 'DIR/SHARED.DAT': fx.pattern(2000, 2)}
CD = {'CD.TXT': b'on the cd', 'SUB/TRACK.BIN': fx.pattern(4100, 4)}


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def zip_bytes(members):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in members.items():
            z.writestr(name, data)
    return buf.getvalue()


def listing(spec, **kw):
    with source.open_source(spec, **kw) as src:
        return {e.path: e.read() for e in src.entries()}, src.skipped


class Sources(unittest.TestCase):
    def setUp(self):
        self.tmp_obj = tempfile.TemporaryDirectory()
        self.tmp = self.tmp_obj.name
        self.d88_a = fx.d88_wrap(fx.make_floppy(DISK_A, label='DISK_A'))
        self.d88_b = fx.d88_wrap(fx.make_floppy(DISK_B, label='DISK_B'))

    def tearDown(self):
        self.tmp_obj.cleanup()

    def test_a_directory_with_everything(self):
        root = os.path.join(self.tmp, 'media')
        write(os.path.join(root, 'installed', 'GAME.EXE'), b'loose file')
        write(os.path.join(root, 'installed', 'DATA.BIN'), b'a .bin that is no CD image')
        write(os.path.join(root, 'floppies', 'a.d88'), self.d88_a)
        write(os.path.join(root, 'disks.zip'),
              zip_bytes({'set/b.D88': self.d88_b, 'set/notes.txt': b'in the zip',
                         'inner.zip': zip_bytes({'deep/a.d88': self.d88_a})}))
        write(os.path.join(root, 'cd', 'disc.bin'), fx.raw_wrap(fx.make_iso(CD)))
        write(os.path.join(root, 'cd', 'disc.cue'), fx.cue_sheet('disc.bin', [('MODE1/2352', 0)])
              .encode())
        write(os.path.join(root, 'cd', 'plain.iso'), fx.make_iso({'ISO.TXT': b'cooked'}))
        got, skipped = listing(root)
        self.assertEqual(skipped, [])
        self.assertEqual(got, {
            'installed/GAME.EXE': b'loose file',
            'installed/DATA.BIN': b'a .bin that is no CD image',
            'floppies/a.d88::A.TXT': DISK_A['A.TXT'],
            'floppies/a.d88::DIR/SHARED.DAT': DISK_A['DIR/SHARED.DAT'],
            'disks.zip::set/b.D88::B.TXT': DISK_B['B.TXT'],
            'disks.zip::set/b.D88::DIR/SHARED.DAT': DISK_B['DIR/SHARED.DAT'],
            'disks.zip::set/notes.txt': b'in the zip',
            'disks.zip::inner.zip::deep/a.d88::A.TXT': DISK_A['A.TXT'],
            'disks.zip::inner.zip::deep/a.d88::DIR/SHARED.DAT': DISK_A['DIR/SHARED.DAT'],
            'cd/disc.bin::CD.TXT': CD['CD.TXT'],
            'cd/disc.bin::SUB/TRACK.BIN': CD['SUB/TRACK.BIN'],
            'cd/disc.cue': fx.cue_sheet('disc.bin', [('MODE1/2352', 0)]).encode(),
            'cd/plain.iso::ISO.TXT': b'cooked',
        })

    def test_entry_attributes(self):
        path = os.path.join(self.tmp, 'a.d88')
        write(path, self.d88_a)
        with source.open_source(path) as src:
            entries = {e.path: e for e in src.entries()}
        e = entries['DIR/SHARED.DAT']
        self.assertEqual((e.name, e.inner_path, e.kind, e.size, e.mtime),
                         ('SHARED.DAT', 'DIR/SHARED.DAT', 'fat', 2000, fx.DOS_STAMP))
        self.assertEqual(e.volume_label, 'DISK_A')
        self.assertEqual(e.origin, path)
        self.assertEqual(e.volume.image.kind, 'd88')        # the whole image is within reach

    def test_member_addressing(self):
        path = os.path.join(self.tmp, 'disks.zip')
        write(path, zip_bytes({'set/a.d88': self.d88_a, 'set/b.d88': self.d88_b,
                               'inner.zip': zip_bytes({'b.d88': self.d88_b})}))
        got, _ = listing(path + '::set/b.d88')
        self.assertEqual(got, {'B.TXT': DISK_B['B.TXT'],
                               'DIR/SHARED.DAT': DISK_B['DIR/SHARED.DAT']})
        got, _ = listing(path + '::SET/A.D88')               # the case of the member ignored
        self.assertEqual(sorted(got), ['A.TXT', 'DIR/SHARED.DAT'])
        got, _ = listing(path + '::inner.zip::b.d88')
        self.assertEqual(sorted(got), ['B.TXT', 'DIR/SHARED.DAT'])
        with self.assertRaisesRegex(source.SourceError, 'no member'):
            listing(path + '::set/c.d88')
        with self.assertRaisesRegex(source.SourceError, 'does not exist'):
            source.open_source(os.path.join(self.tmp, 'absent.zip') + '::a.d88')
        with self.assertRaisesRegex(source.SourceError, 'not an archive'):
            write(os.path.join(self.tmp, 'a.d88'), self.d88_a)
            listing(os.path.join(self.tmp, 'a.d88') + '::A.TXT')

    def test_cd_through_its_sheet_and_as_a_zip_member(self):
        raw = fx.raw_wrap(fx.make_iso(CD))
        write(os.path.join(self.tmp, 'disc.bin'), raw)
        cue = os.path.join(self.tmp, 'disc.cue')
        write(cue, fx.cue_sheet('disc.bin', [('MODE1/2352', 0)]).encode())
        got, _ = listing(cue)
        self.assertEqual(got, CD)
        zpath = os.path.join(self.tmp, 'cd.zip')
        write(zpath, zip_bytes({'disc.img': raw}))
        with source.open_source(zpath) as src:
            entries = list(src.entries())
            self.assertEqual({e.path: e.read() for e in entries},
                             {'disc.img::' + k: v for k, v in CD.items()})
            self.assertEqual({(e.kind, e.volume_label) for e in entries},
                             {('iso9660', 'TESTVOL')})

    def test_a_damaged_container_is_reported_and_offered_as_a_file(self):
        root = os.path.join(self.tmp, 'media')
        write(os.path.join(root, 'broken.zip'), b'PK\x03\x04 this is not a zip')
        write(os.path.join(root, 'blank.hdm'), bytes(1261568))
        write(os.path.join(root, 'flux.scp'), b'SCP' + bytes(100))
        got, skipped = listing(root)
        self.assertEqual(sorted(got), ['blank.hdm', 'broken.zip', 'flux.scp'])
        self.assertEqual(sorted(label for label, _ in skipped),
                         sorted(os.path.join(root, n) for n in got))
        self.assertTrue(any('D88' in why for _, why in skipped))
        seen = []
        with source.open_source(root, on_skip=lambda label, why: seen.append(label)) as src:
            list(src.entries())
        self.assertEqual(len(seen), 3)

    def test_an_engine_file_system_reaches_the_source(self):
        disk = (b'TINYFS\0\0' + b'HELLO   ' + b'\x05\x00' + b'world').ljust(1261568, b'\0')
        path = os.path.join(self.tmp, 'own.zip')
        write(path, zip_bytes({'own.d88': fx.d88_wrap(disk)}))
        fs = diskimg.default_filesystems()
        fs.register('tinyfs', tiny_probe)
        with source.open_source(path, filesystems=fs) as src:
            entries = list(src.entries())
        self.assertEqual([(e.path, e.kind) for e in entries], [('own.d88::HELLO', 'tinyfs')])
        got, skipped = listing(path)                         # without it: no file system
        self.assertEqual(list(got), ['own.d88'])
        self.assertEqual(len(skipped), 1)

    def test_a_container_of_a_games_own_format(self):
        def read_arc(data):                     # "ARC" + count, then name\0 + u8 size + data
            if data[:3] != b'ARC':
                return None
            members, at = [], 4
            for _ in range(data[3]):
                end = data.index(b'\0', at)     # ValueError when the directory is cut short
                size = data[end + 1]
                members.append((data[at:end].decode('ascii'), data[end + 2:end + 2 + size]))
                at = end + 2 + size
            return members

        arc = b'ARC\x02' + b'ONE.DAT\0\x03one' + b'SUB\\TWO.DAT\0\x03two'
        root = os.path.join(self.tmp, 'media')
        write(os.path.join(root, 'GAME.ARC'), arc)
        write(os.path.join(root, 'other.arc'), b'not this format')
        write(os.path.join(root, 'cut.arc'), b'ARC\x02' + b'ONE.DAT\0\x03one' + b'SUB')
        write(os.path.join(root, 'in.zip'), zip_bytes({'deep/game.Arc': arc}))
        with source.open_source(root, containers={'.ARC': read_arc}) as src:
            entries = {e.path: e for e in src.entries()}
            got = {path: e.read() for path, e in entries.items()}
            skipped = src.skipped
        self.assertEqual(got, {'GAME.ARC::ONE.DAT': b'one', 'GAME.ARC::SUB/TWO.DAT': b'two',
                               'in.zip::deep/game.Arc::ONE.DAT': b'one',
                               'in.zip::deep/game.Arc::SUB/TWO.DAT': b'two',
                               'other.arc': b'not this format',
                               'cut.arc': b'ARC\x02' + b'ONE.DAT\0\x03one' + b'SUB'})
        member = entries['GAME.ARC::SUB/TWO.DAT']
        self.assertEqual((member.name, member.inner_path, member.kind, member.size),
                         ('TWO.DAT', 'SUB/TWO.DAT', 'container', 3))
        self.assertEqual([label for label, _ in skipped], [os.path.join(root, 'cut.arc')])
        got, _ = listing(root)                              # without the reader: plain files
        self.assertEqual(got['GAME.ARC'], arc)

    @unittest.skipUnless(hasattr(os, 'symlink'), 'needs symbolic links')
    def test_links_in_a_directory(self):
        root = os.path.join(self.tmp, 'media')
        outside = os.path.join(self.tmp, 'outside')
        write(os.path.join(root, 'REAL.TXT'), b'real')
        write(os.path.join(outside, 'SECRET.TXT'), b'elsewhere')
        os.symlink(root, os.path.join(root, 'loop'))                    # a directory cycle
        os.symlink(outside, os.path.join(root, 'out'))
        os.symlink(os.path.join(root, 'REAL.TXT'), os.path.join(root, 'ALIAS.TXT'))
        got, _ = listing(root)
        # directory links are not followed; a file link in the player's own directory is read
        self.assertEqual(got, {'REAL.TXT': b'real', 'ALIAS.TXT': b'real'})
        # in a directory an unpacker filled, file links are not followed either
        walked = [rel for _d, _n, rel in source.Source._walk(root, False)]
        self.assertEqual(walked, ['REAL.TXT'])

    def test_hostile_zip_names_cannot_leave_the_output_directory(self):
        path = os.path.join(self.tmp, 'evil.zip')
        write(path, zip_bytes({'../../escape.txt': b'1', '/abs/olute.txt': b'2',
                               'ok/..\\..\\back.txt': b'3'}))
        dest = os.path.join(self.tmp, 'deep', 'out')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(source.main(['extract', path, dest]), 0)
        found = sorted(os.path.relpath(os.path.join(d, f), self.tmp)
                       for d, _dirs, files in os.walk(self.tmp) for f in files)
        self.assertEqual(found, [os.path.join('deep', 'out', 'abs', 'olute.txt'),
                                 os.path.join('deep', 'out', 'escape.txt'),
                                 os.path.join('deep', 'out', 'ok', 'back.txt'),
                                 'evil.zip'])

    def test_default_roots(self):
        media = os.path.join(self.tmp, 'media')
        write(os.path.join(media, 'a.d88'), self.d88_a)
        env = {source.MEDIA_ENV: media + os.pathsep + self.tmp}
        self.assertEqual(source.default_roots([], env), [media, self.tmp])
        self.assertEqual(source.default_roots(['/x/y'], env), ['/x/y'])
        self.assertEqual(source.default_roots(['a.d88'], env), [os.path.join(media, 'a.d88')])
        self.assertEqual(source.default_roots(['a.d88::A.TXT'], env),
                         [os.path.join(media, 'a.d88::A.TXT')])
        with self.assertRaisesRegex(source.SourceError, source.MEDIA_ENV):
            source.default_roots([], {})

    def test_ls_command(self):
        path = os.path.join(self.tmp, 'a.d88')
        write(path, self.d88_a)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(source.main(['ls', '--md5', path]), 0)
        self.assertIn('DIR/SHARED.DAT', out.getvalue())
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(source.main(['ls', os.path.join(self.tmp, 'absent')]), 1)
        self.assertIn('does not exist', err.getvalue())


class ExternalArchives(unittest.TestCase):
    """7z and the like go through an external unpacker; skipped where none is installed."""

    def setUp(self):
        self.tmp_obj = tempfile.TemporaryDirectory()
        self.tmp = self.tmp_obj.name

    def tearDown(self):
        self.tmp_obj.cleanup()

    def make_7z(self):
        """A .7z holding a D88 and a loose file, made by whichever tool can write one."""
        tree = os.path.join(self.tmp, 'tree')
        write(os.path.join(tree, 'disks', 'a.d88'), fx.d88_wrap(fx.make_floppy(DISK_A)))
        write(os.path.join(tree, 'LOOSE.TXT'), b'loose in the archive')
        out = os.path.join(self.tmp, 'media.7z')
        for exe in ('7z', '7zz', '7za', '7zr'):
            if shutil.which(exe):
                cmd = [exe, 'a', '-bd', '-y', out, '.']
                break
        else:
            cmd = ['bsdtar', '-c', '--format', '7zip', '-f', out, '.']
        r = subprocess.run(cmd, cwd=tree, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            self.skipTest('cannot create a 7z archive here')
        return out

    @unittest.skipUnless(source.archive_tools(), 'needs bsdtar or 7z')
    def test_7z_with_an_image_inside(self):
        archive = self.make_7z()
        want = {'LOOSE.TXT': b'loose in the archive',
                'disks/a.d88::A.TXT': DISK_A['A.TXT'],
                'disks/a.d88::DIR/SHARED.DAT': DISK_A['DIR/SHARED.DAT']}
        for kind, exe in source.archive_tools():
            with self.subTest(tool=kind):
                with source.open_source(archive, tools=[(kind, exe)]) as src:
                    entries = list(src.entries())
                    self.assertEqual({e.path: e.read() for e in entries}, want)
                    self.assertEqual({e.kind for e in entries}, {'archive', 'fat'})
                    tmpdir = src._tmpdir
                    self.assertTrue(os.path.isdir(tmpdir))
                self.assertFalse(os.path.exists(tmpdir))        # cleaned up on close
        got, _ = listing(archive + '::disks/a.d88')
        self.assertEqual(sorted(got), ['A.TXT', 'DIR/SHARED.DAT'])
        # inside a directory, and inside a zip
        got, skipped = listing(self.tmp)
        self.assertIn('media.7z::disks/a.d88::A.TXT', got)
        zpath = os.path.join(self.tmp, 'outer.zip')
        with open(archive, 'rb') as f:
            write(zpath, zip_bytes({'in/media.7z': f.read()}))
        got, skipped = listing(zpath)
        self.assertEqual(skipped, [])
        self.assertEqual(got, {'in/media.7z::' + k: v for k, v in want.items()})

    def test_without_a_tool_the_message_says_what_to_install(self):
        path = os.path.join(self.tmp, 'x.7z')
        write(path, b"7z\xbc\xaf'\x1c" + bytes(64))
        with source.open_source(path, tools=[]) as src:
            entries = list(src.entries())
            self.assertEqual([e.path for e in entries], ['x.7z'])
            self.assertEqual(len(src.skipped), 1)
            self.assertRegex(src.skipped[0][1], 'bsdtar.*7z')

    @unittest.skipUnless(source.archive_tools(), 'needs bsdtar or 7z')
    def test_an_archive_no_tool_can_read(self):
        path = os.path.join(self.tmp, 'bad.7z')
        write(path, b'this is not an archive at all')
        with source.open_source(path) as src:
            self.assertEqual([e.path for e in src.entries()], ['bad.7z'])
            self.assertIn('cannot be unpacked', src.skipped[0][1])


if __name__ == '__main__':
    unittest.main()
