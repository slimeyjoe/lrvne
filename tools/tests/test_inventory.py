# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of lrvne.inventory: the TSV, scan, import (missing and corrupt files), check, hooks."""
import contextlib
import hashlib
import io
import os
import tempfile
import time
import unittest
import zipfile

from lrvne import diskimg, inventory, source

from . import fixtures as fx
from .test_diskimg import tiny_probe

GOOD = {
    'START.EXE': fx.pattern(900, 1),
    'PIC/TITLE.PIC': fx.pattern(5000, 2),
    'PIC/END.PIC': fx.pattern(3000, 3),
    'MUSIC.DAT': fx.pattern(1200, 4),
    'SAVE1.DAT': b'the state the previous owner left',
}


def md5(data):
    return hashlib.md5(data).hexdigest()


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def make_inventory():
    """An inventory of GOOD: a release column, an optional file, an unchecked save."""
    lines = ['# made for the tests', 'release\tname\tsize\tmtime\tmd5\tkind', '']
    for name, data in GOOD.items():
        save = name.startswith('SAVE')
        lines.append('\t'.join(('disk', name, '-' if save else str(len(data)),
                                '1993-05-28 12:34', '-' if save else md5(data),
                                'state' if save else 'game')))
    lines.append('\t'.join(('extra', 'BONUS.DAT', '4', '', md5(b'gift'), 'optional')))
    return '\n'.join(lines) + '\n'


class InventoryFile(unittest.TestCase):
    def test_parse_and_dump(self):
        text = make_inventory()
        inv = inventory.Inventory.parse(text)
        self.assertEqual(inv.columns, ['name', 'release', 'size', 'mtime', 'md5', 'kind'])
        self.assertEqual(len(inv.rows), 6)
        row = inv.rows[1]
        self.assertEqual((row.name, row.basename, row.size, row.md5),
                         ('PIC/TITLE.PIC', 'TITLE.PIC', 5000, md5(GOOD['PIC/TITLE.PIC'])))
        self.assertEqual((row.get('release'), row['kind'], row.get('absent', 'x')),
                         ('disk', 'game', 'x'))
        with self.assertRaises(KeyError):
            row['absent']
        save = inv.rows[4]
        self.assertEqual((save.size, save.md5), (-1, '-'))
        again = inventory.Inventory.parse(inv.dumps())
        self.assertEqual(again.rows, inv.rows)
        self.assertEqual(again.columns, inv.columns)

    def test_path_as_the_name_column(self):
        inv = inventory.Inventory.parse('release\tpath\tsize\tmd5\n'
                                        'r\\tA\\\\B.DAT\\t1\\t%s\n'.replace('\\t', '\t')
                                        .replace('\\\\', '\\') % md5(b'x'))
        self.assertEqual(inv.columns[0], 'path')
        self.assertEqual(inv.rows[0].name, 'A/B.DAT')
        self.assertTrue(inv.dumps().startswith('path\trelease\tsize\tmd5\n'))

    def test_errors(self):
        for text, what in (('', 'no header'), ('name\tsize\n', 'header'),
                           ('name\tsize\tmd5\nA\t1\n', 'columns'),
                           ('name\tsize\tmd5\nA\tmany\t-\n', 'size'),
                           ('name\tsize\tmd5\nA\t1\tnot-a-digest\n', 'md5'),
                           ('name\tsize\tmd5\tmd5\n', 'header')):
            with self.assertRaisesRegex(inventory.InventoryError, what):
                inventory.Inventory.parse(text)
        with self.assertRaises(inventory.InventoryError):
            inventory.Inventory.load(os.path.join(tempfile.gettempdir(), 'no', 'such.tsv'))


class Import(unittest.TestCase):
    def setUp(self):
        self.tmp_obj = tempfile.TemporaryDirectory()
        self.tmp = self.tmp_obj.name
        self.dest = os.path.join(self.tmp, 'original')
        self.inv = inventory.Inventory.parse(make_inventory())
        self.hooks = inventory.Hooks(
            dest_path=lambda row: row.get('release') + '/' + row.name,
            required=lambda row: row.get('release') == 'disk',
            group=lambda row: row.get('release'))

    def tearDown(self):
        self.tmp_obj.cleanup()

    def media(self, files, name='media'):
        """A directory holding a zip that holds a D88 with `files`."""
        root = os.path.join(self.tmp, name)
        os.makedirs(root, exist_ok=True)
        with zipfile.ZipFile(os.path.join(root, 'disks.zip'), 'w') as z:
            z.writestr('game/disk1.d88', fx.d88_wrap(fx.make_floppy(files)))
        return root

    def dest_files(self):
        return {os.path.relpath(os.path.join(d, f), self.dest).replace(os.sep, '/')
                for d, _dirs, files in os.walk(self.dest) for f in files}

    def test_one_missing_one_corrupt(self):
        files = dict(GOOD)
        del files['PIC/END.PIC']                                    # missing
        files['MUSIC.DAT'] = fx.pattern(1200, 99)                   # corrupt: same size
        files['OTHER.TXT'] = b'not in the inventory'
        log = []
        report = inventory.import_files(self.inv, [self.media(files)], self.dest, self.hooks,
                                        log=log.append)
        self.assertFalse(report.ok)
        self.assertEqual(sorted(r.name for r in report.missing),
                         ['BONUS.DAT', 'MUSIC.DAT', 'PIC/END.PIC'])
        self.assertEqual(report.mismatched,
                         [('MUSIC.DAT', md5(files['MUSIC.DAT']),
                           'disks.zip::game/disk1.d88::MUSIC.DAT')])
        self.assertEqual(report.written, 3)
        self.assertEqual(self.dest_files(),
                         {'disk/START.EXE', 'disk/PIC/TITLE.PIC', 'disk/SAVE1.DAT'})
        with open(os.path.join(self.dest, 'disk', 'PIC', 'TITLE.PIC'), 'rb') as f:
            self.assertEqual(f.read(), GOOD['PIC/TITLE.PIC'])
        stamp = time.strftime('%Y-%m-%d %H:%M', time.localtime(
            os.path.getmtime(os.path.join(self.dest, 'disk', 'START.EXE'))))
        self.assertEqual(stamp, '1993-05-28 12:34')
        text = '\n'.join(report.lines())
        self.assertRegex(text, r'disk\s+3/5\s+missing: PIC/END.PIC MUSIC.DAT')
        self.assertRegex(text, r'extra\s+0/1\s+missing: BONUS.DAT \(optional\)')
        self.assertIn('differs: MUSIC.DAT md5 ' + md5(files['MUSIC.DAT']), text)
        self.assertTrue(any('3 files written' in line for line in log))
        self.assertIn(report.found[self.inv.rows[0]].split(': ')[1], ('START.EXE',))

        # a second import from a good copy completes the tree and rewrites nothing it has
        report = inventory.import_files(self.inv, [self.media(GOOD, 'media2')], self.dest,
                                        self.hooks)
        self.assertTrue(report.ok)
        self.assertEqual(report.written, 2)
        self.assertEqual([r.name for r in report.missing], ['BONUS.DAT'])
        self.assertEqual(report.mismatched, [])

        checked = inventory.check(self.inv, self.dest, self.hooks)
        self.assertTrue(checked.ok)
        self.assertEqual(len(checked.have), 5)

    def test_check_finds_damage_and_wrong_case(self):
        inventory.import_files(self.inv, [self.media(GOOD)], self.dest, self.hooks)
        write(os.path.join(self.dest, 'disk', 'MUSIC.DAT'), b'overwritten')
        os.rename(os.path.join(self.dest, 'disk', 'START.EXE'),
                  os.path.join(self.dest, 'disk', 'start.exe'))
        write(os.path.join(self.dest, 'disk', 'SAVE1.DAT'), b'a save may differ')
        report = inventory.check(self.inv, self.dest, self.hooks)
        self.assertFalse(report.ok)
        self.assertEqual(sorted(r.name for r in report.missing),
                         ['BONUS.DAT', 'MUSIC.DAT', 'START.EXE'])
        self.assertEqual(sorted(m[0] for m in report.mismatched), ['MUSIC.DAT', 'START.EXE'])
        self.assertTrue(any('named start.exe' in m[2] for m in report.mismatched))

    def test_loose_files_any_case_and_several_sources(self):
        loose = os.path.join(self.tmp, 'installed')
        write(os.path.join(loose, 'start.exe'), GOOD['START.EXE'])
        write(os.path.join(loose, 'sub', 'Title.Pic'), GOOD['PIC/TITLE.PIC'])
        write(os.path.join(loose, 'bonus.dat'), b'gift')
        rest = {k: v for k, v in GOOD.items() if k not in ('START.EXE', 'PIC/TITLE.PIC')}
        disk = os.path.join(self.tmp, 'rest.fdi')
        write(disk, fx.anex_wrap(fx.make_floppy(rest), (77, 2, 8, 1024)))
        report = inventory.import_files(self.inv, [loose, disk], self.dest, self.hooks)
        self.assertTrue(report.ok)
        self.assertEqual(report.missing, [])
        self.assertEqual(self.dest_files(), {'disk/' + n for n in GOOD} | {'extra/BONUS.DAT'})

    def test_accept_and_on_entry_hooks(self):
        seen = []
        self.hooks.on_entry = lambda entry: seen.append((entry.volume_label, entry.name))
        self.hooks.accept = lambda row, entry: entry.volume_label == 'RIGHT'
        wrong = os.path.join(self.tmp, 'wrong.d88')
        right = os.path.join(self.tmp, 'right.d88')
        write(wrong, fx.d88_wrap(fx.make_floppy(GOOD, label='WRONG')))
        write(right, fx.d88_wrap(fx.make_floppy(GOOD, label='RIGHT')))
        report = inventory.import_files(self.inv, [wrong], self.dest, self.hooks)
        self.assertEqual(len(report.found), 0)
        self.assertEqual(report.mismatched, [])             # the contents were right
        report = inventory.import_files(self.inv, [right], self.dest, self.hooks)
        self.assertTrue(report.ok)
        self.assertIn(('WRONG', 'MUSIC.DAT'), seen)
        self.assertIn(('RIGHT', 'MUSIC.DAT'), seen)

    def test_post_import_and_engine_file_system(self):
        made = []
        disk = (b'TINYFS\0\0' + b'TINY.DAT' + b'\x05\x00' + b'world').ljust(1261568, b'\0')
        path = os.path.join(self.tmp, 'own.d88')
        write(path, fx.d88_wrap(disk))
        inv = inventory.Inventory.parse('name\tsize\tmd5\nTINY.DAT\t5\t%s\n' % md5(b'world'))
        fs = diskimg.default_filesystems()
        fs.register('tinyfs', tiny_probe)
        hooks = inventory.Hooks(post_import=lambda report, dest: made.append((report.ok, dest)))
        report = inventory.import_files(inv, [path], self.dest, hooks, filesystems=fs)
        self.assertTrue(report.ok)
        self.assertEqual(made, [(True, self.dest)])
        self.assertEqual(self.dest_files(), {'TINY.DAT'})

    def test_files_inside_a_container_of_the_games_own(self):
        def read_bag(data):                     # "BAG", then name\0 + u16 size + data
            if data[:3] != b'BAG':
                return None
            members, at = [], 3
            while at < len(data):
                end = data.index(b'\0', at)
                size = int.from_bytes(data[end + 1:end + 3], 'little')
                members.append((data[at:end].decode('ascii'), data[end + 3:end + 3 + size]))
                at = end + 3 + size
            return members

        bag = b'BAG' + b''.join(name.rsplit('/', 1)[-1].encode() + b'\0' +
                                len(data).to_bytes(2, 'little') + data
                                for name, data in GOOD.items())
        src = os.path.join(self.tmp, 'cd')
        write(os.path.join(src, 'DATA', 'GAME.BAG'), bag)
        report = inventory.import_files(self.inv, [src], self.dest, self.hooks)
        self.assertEqual(len(report.found), 0)              # a file like any other
        self.hooks.containers = {'.bag': read_bag}
        report = inventory.import_files(self.inv, [src], self.dest, self.hooks)
        self.assertTrue(report.ok)
        self.assertEqual(self.dest_files(), {'disk/' + n for n in GOOD})
        made = inventory.scan([src], hooks=self.hooks)
        self.assertEqual(sorted(r.name for r in made.rows),
                         sorted(n.rsplit('/', 1)[-1] for n in GOOD))

    def test_an_inventory_cannot_write_outside_the_destination(self):
        inv = inventory.Inventory.parse('name\tsize\tmd5\nA.TXT\t1\t%s\n' % md5(b'a'))
        src = os.path.join(self.tmp, 'src')
        write(os.path.join(src, 'A.TXT'), b'a')
        hooks = inventory.Hooks(dest_path=lambda row: '../../' + row.name)
        inventory.import_files(inv, [src], self.dest, hooks)
        self.assertEqual(self.dest_files(), {'A.TXT'})
        hooks = inventory.Hooks(dest_path=lambda row: '..')
        with self.assertRaises(inventory.InventoryError):
            inventory.import_files(inv, [src], self.dest, hooks)

    def test_absent_source(self):
        with self.assertRaisesRegex(inventory.InventoryError, 'does not exist'):
            inventory.import_files(self.inv, [os.path.join(self.tmp, 'absent')], self.dest)

    def test_scan(self):
        root = self.media(GOOD)
        made = inventory.scan([root])
        self.assertEqual(made.columns, ['name', 'size', 'md5'])
        self.assertEqual({(r.name, r.size, r.md5) for r in made.rows},
                         {(n, len(d), md5(d)) for n, d in GOOD.items()})

        def row(entry, data):
            if entry.name.startswith('SAVE'):
                return None
            return {'path': entry.volume_label + '/' + entry.inner_path, 'release': 'disk',
                    'kind': 'picture' if entry.name.endswith('.PIC') else 'other'}
        made = inventory.scan([root, root], ('release', 'path', 'size', 'mtime', 'md5', 'kind'),
                              inventory.Hooks(scan_row=row))
        self.assertEqual(made.columns, ['path', 'release', 'size', 'mtime', 'md5', 'kind'])
        self.assertEqual(len(made.rows), 4)                 # each file once, the save left out
        r = next(r for r in made.rows if r.basename == 'TITLE.PIC')
        self.assertEqual((r.name, r.get('release'), r.get('kind'), r.get('mtime')),
                         ('TESTDISK/PIC/TITLE.PIC', 'disk', 'picture', fx.DOS_STAMP))
        # what scan writes, import reads
        again = inventory.Inventory.parse(made.dumps())
        self.assertEqual(again.rows, made.rows)
        with self.assertRaises(inventory.InventoryError):
            inventory.scan([root], ('name', 'md5'))


class CommandLine(unittest.TestCase):
    def setUp(self):
        self.tmp_obj = tempfile.TemporaryDirectory()
        self.tmp = self.tmp_obj.name
        self.tsv = os.path.join(self.tmp, 'inventory.tsv')
        with open(self.tsv, 'w', encoding='utf-8') as f:
            f.write(make_inventory())
        self.dest = os.path.join(self.tmp, 'original')
        self.disk = os.path.join(self.tmp, 'media', 'disk.d88')
        write(self.disk, fx.d88_wrap(fx.make_floppy(GOOD)))
        self.hooks = inventory.Hooks(required=lambda row: row.get('release') == 'disk',
                                     group=lambda row: row.get('release'))

    def tearDown(self):
        self.tmp_obj.cleanup()

    def run_cli(self, *args, **kw):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            status = inventory.run_cli(list(args), **kw)
        return status, out.getvalue()

    def test_an_engine_wrapper(self):
        ran = []
        kw = dict(inventory=self.tsv, dest=self.dest, hooks=self.hooks, prog='gamedata',
                  commands={'derive': (lambda p: p.add_argument('parts', nargs='*'),
                                       lambda a: ran.append(a.parts) or 0)})
        status, text = self.run_cli('check', **kw)
        self.assertEqual(status, 1)
        self.assertIn('import the game first', text)
        status, text = self.run_cli('import', self.disk, **kw)
        self.assertEqual(status, 0, text)
        self.assertIn('gamedata: 5 files written', text)
        self.assertIn('every required file is there', text)
        self.assertEqual(self.run_cli('check', '-q', **kw), (0, ''))
        os.remove(os.path.join(self.dest, 'MUSIC.DAT'))
        status, text = self.run_cli('check', '-q', **kw)
        self.assertEqual(status, 1)
        self.assertIn('INCOMPLETE', text)
        self.assertEqual(self.run_cli('derive', 'a', 'b', **kw)[0], 0)
        self.assertEqual(ran, [['a', 'b']])
        status, text = self.run_cli('scan', self.disk, **kw)
        self.assertEqual(status, 0)
        self.assertTrue(text.startswith('name\trelease\tsize\tmtime\tmd5\tkind\n'))
        self.assertIn('PIC/TITLE.PIC\t\t5000\t%s\t%s' % (fx.DOS_STAMP, md5(GOOD['PIC/TITLE.PIC'])),
                      text)

    def test_media_from_the_environment(self):
        old = os.environ.get(source.MEDIA_ENV)
        try:
            os.environ.pop(source.MEDIA_ENV, None)
            status, text = self.run_cli('import', inventory=self.tsv, dest=self.dest)
            self.assertEqual(status, 1)
            self.assertIn(source.MEDIA_ENV, text)
            os.environ[source.MEDIA_ENV] = os.path.dirname(self.disk)
            status, text = self.run_cli('import', inventory=self.tsv, dest=self.dest)
            self.assertEqual(status, 1)                     # BONUS.DAT is required by default
            self.assertIn('missing: BONUS.DAT', text)
            self.assertTrue(os.path.exists(os.path.join(self.dest, 'PIC', 'END.PIC')))
        finally:
            if old is None:
                os.environ.pop(source.MEDIA_ENV, None)
            else:
                os.environ[source.MEDIA_ENV] = old

    def test_the_module_command_needs_its_options(self):
        with self.assertRaises(SystemExit):
            self.run_cli('import', self.disk)
        status, _ = self.run_cli('import', '--inventory', self.tsv, '--out', self.dest, self.disk)
        self.assertEqual(status, 1)
        status, text = self.run_cli('scan', '--columns', 'name,size,md5', self.disk)
        self.assertEqual(status, 0)
        self.assertEqual(len(text.splitlines()), 1 + len(GOOD))


if __name__ == '__main__':
    unittest.main()
