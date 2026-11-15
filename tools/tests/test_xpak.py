# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of lrvne.xpak (the layout of include/lrvne/pack.h, byte by byte) and lrvne.profile."""
import contextlib
import io
import os
import struct
import subprocess
import sys
import tempfile
import unittest

from lrvne import profile, xpak

TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sample_writer():
    types = xpak.TypeRegistry({32: 'picture', 33: 'script'})
    w = xpak.PackWriter(types)
    w.add('profile', xpak.T_META, b'id=test\n')
    w.add('title.pic', 'picture', b'PICTURE')              # 7 bytes: padding must follow
    w.add('start.scr', 33, b'ja script', lang='ja')
    w.add('start.scr', 'script', b'en script!', lang='en')
    w.add('text', xpak.T_STRINGS, b'english text', lang='en')
    w.add('font16', xpak.T_FONT, b'XFNT....')
    return w


class Layout(unittest.TestCase):
    def test_bytes_follow_pack_h(self):
        data = sample_writer().build()
        # header: "XPAK", u16 version 2, u16 count, u32 total size, 4 reserved
        self.assertEqual(data[0:4], b'XPAK')
        self.assertEqual(struct.unpack_from('<HHI', data, 4), (2, 7, len(data)))
        self.assertEqual(data[12:16], bytes(4))
        # entries: 32 bytes each from 0x10, sorted by (lang, name)
        table = []
        for i in range(7):
            e = data[16 + 32 * i:16 + 32 * i + 32]
            name, type_, lang, reserved, offset, size = struct.unpack('<20sBBHII', e)
            self.assertEqual(reserved, 0)
            self.assertEqual(offset % 4, 0, 'entry data is 4-byte aligned')
            self.assertGreaterEqual(offset, 16 + 32 * 7)
            self.assertLessEqual(offset + size, len(data))
            self.assertEqual(name, name.rstrip(b'\0').ljust(20, b'\0'))
            table.append((name.rstrip(b'\0'), type_, lang, data[offset:offset + size]))
        self.assertEqual(table, [
            (b'font16', 2, 0, b'XFNT....'),
            (b'langs', 1, 0, b'1=ja\n2=en\n'),
            (b'profile', 1, 0, b'id=test\n'),
            (b'TITLE.PIC', 32, 0, b'PICTURE'),
            (b'START.SCR', 33, 1, b'ja script'),
            (b'START.SCR', 33, 2, b'en script!'),
            (b'text', 3, 2, b'english text'),
        ])
        # the gaps between entries are zero padding of fewer than four bytes
        spans = sorted(struct.unpack_from('<II', data, 16 + 32 * i + 24) for i in range(7))
        end = 16 + 32 * 7
        for offset, size in spans:
            self.assertLess(offset - end, 4)
            self.assertEqual(data[end:offset], bytes(offset - end))
            end = offset + size
        self.assertEqual(end, len(data))

    def test_constants_match_the_header_file(self):
        header = os.path.join(os.path.dirname(TOOLS), 'include', 'lrvne', 'pack.h')
        with open(header, encoding='utf-8') as f:
            text = f.read()
        for name, value in (('LRV_PACK_VERSION', xpak.VERSION),
                            ('LRV_PACK_HEADER_SIZE', xpak.HEADER_SIZE),
                            ('LRV_PACK_ENTRY_SIZE', xpak.ENTRY_SIZE),
                            ('LRV_PACK_NAME_LEN', xpak.NAME_LEN),
                            ('LRV_PACK_TRAILER_SIZE', xpak.TRAILER_SIZE),
                            ('LRV_PACK_LANGS_MAX', xpak.LANGS_MAX),
                            ('LRV_PACK_TAG_LEN', xpak.TAG_LEN)):
            self.assertRegex(text, r'#define %s\s+%d\b' % (name, value))
        for name, value in (('META', xpak.T_META), ('FONT', xpak.T_FONT),
                            ('STRINGS', xpak.T_STRINGS), ('BLOB', xpak.T_BLOB),
                            ('VOICECUE', xpak.T_VOICECUE), ('VOICE', xpak.T_VOICE),
                            ('ENGINE', xpak.T_ENGINE)):
            self.assertRegex(text, r'LRV_PT_%s\s*=\s*%d\b' % (name, value))

    def test_sort_order(self):
        w = xpak.PackWriter(xpak.TypeRegistry({40: 'a', 41: 'b'}))
        for name, type_, lang in (('zeta', 40, None), ('Beta', xpak.T_BLOB, None),
                                  ('alpha', xpak.T_BLOB, None), ('same', 41, None),
                                  ('same', 40, None), ('aaa', xpak.T_BLOB, 'pl'),
                                  ('zzz', xpak.T_BLOB, 'ja'), ('日本', 40, None)):
            w.add(name, type_, b'x', lang)
        pack = xpak.read_pack(w.build())
        self.assertEqual([(e.lang, e.name, e.type) for e in pack.entries], [
            (0, 'alpha', 4), (0, 'Beta', 4), (0, 'langs', 1), (0, 'SAME', 40), (0, 'SAME', 41),
            (0, 'ZETA', 40), (0, '日本', 40), (1, 'aaa', 4), (2, 'zzz', 4)])
        self.assertEqual(pack.langs, {1: 'pl', 2: 'ja'})
        self.assertEqual(pack.entries[6].raw_name, '日本'.encode('cp932'))


class RoundTrip(unittest.TestCase):
    def test_write_and_parse(self):
        w = sample_writer()
        self.assertEqual(w.langs, {1: 'ja', 2: 'en'})
        pack = xpak.read_pack(w.build())
        self.assertEqual(pack.langs, {1: 'ja', 2: 'en'})
        self.assertEqual(len(pack.entries), 7)
        self.assertEqual(pack.find('TITLE.PIC').data, b'PICTURE')
        self.assertEqual(pack.find('title.pic', 32).data, b'PICTURE')
        self.assertIsNone(pack.find('title.pic', 33))
        self.assertIsNone(pack.find('start.scr'))                       # no neutral one
        self.assertEqual(pack.find('start.scr', lang='ja').data, b'ja script')
        en = pack.find('START.SCR', lang='EN')
        self.assertEqual((en.data, en.lang, en.lang_tag), (b'en script!', 2, 'en'))
        self.assertEqual(pack.find('profile', lang='en').data, b'id=test\n')    # falls back
        self.assertEqual(pack.find('text', lang=2).data, b'english text')
        self.assertIsNone(pack.find('text', lang='pl'))
        self.assertEqual(pack.find('langs', xpak.T_META).data, b'1=ja\n2=en\n')

    def test_no_languages_no_langs_entry(self):
        w = xpak.PackWriter()
        w.add('a', xpak.T_BLOB, b'')
        pack = xpak.read_pack(w.build())
        self.assertEqual([e.name for e in pack.entries], ['a'])
        self.assertEqual(pack.langs, {})
        self.assertEqual(pack.entries[0].data, b'')

    def test_fixed_language_ids(self):
        w = xpak.PackWriter(langs=['ja', 'en', 'pl'])
        w.add('t', xpak.T_STRINGS, b'pl', lang='PL')
        pack = xpak.read_pack(w.build())
        self.assertEqual(pack.langs, {1: 'ja', 2: 'en', 3: 'pl'})
        self.assertEqual(pack.find('t', lang='pl').lang, 3)

    def test_dedupe_and_data_order(self):
        w = xpak.PackWriter()
        for name in ('C1', 'C2', 'C3', 'C4'):
            w.add(name, xpak.T_VOICE, b'same' if name in ('C1', 'C3') else name.encode() * 3)
        plain = xpak.read_pack(w.build())
        self.assertEqual(len({e.offset for e in plain.entries}), 4)
        data = w.build(dedupe=True, data_order=['c4', 'C2'])
        pack = xpak.read_pack(data)
        by = {e.name: e for e in pack.entries}
        self.assertEqual([e.name for e in pack.entries], ['C1', 'C2', 'C3', 'C4'])  # table sorted
        self.assertEqual(by['C1'].offset, by['C3'].offset)
        self.assertLess(by['C4'].offset, by['C2'].offset)
        self.assertLess(by['C2'].offset, by['C1'].offset)
        self.assertEqual(by['C3'].data, b'same')
        self.assertLess(len(data), len(w.build()))

    def test_what_the_writer_refuses(self):
        w = xpak.PackWriter()
        w.add('one', xpak.T_BLOB, b'')
        for args, what in ((('ONE', xpak.T_BLOB, b''), 'duplicate'),
                           (('x' * 21, xpak.T_BLOB, b''), 'name'),
                           (('', xpak.T_BLOB, b''), 'name'),
                           (('langs', xpak.T_META, b''), 'written by the pack writer'),
                           (('a', 'nonsense', b''), 'unknown entry type'),
                           (('a', 0, b''), 'not in 1..255'),
                           (('a', xpak.T_BLOB, b'', 'toolongtag'), 'language tag'),
                           (('a', xpak.T_BLOB, b'', 'a=b'), 'language tag'),
                           (('ą', xpak.T_BLOB, b''), 'cannot be stored')):
            with self.assertRaisesRegex(xpak.PackError, what):
                w.add(*args)
        w.add('one', xpak.T_BLOB, b'', lang='en')           # another language: not a duplicate
        w.add('one', xpak.T_META, b'')                      # another type: neither
        w.add(b'\x83\x65raw', xpak.T_BLOB, b'')             # bytes are stored as they are
        for i in range(14):
            w.lang_id('l%d' % i)
        with self.assertRaisesRegex(xpak.PackError, 'more than 15'):
            w.lang_id('extra')

    def test_type_registry(self):
        types = xpak.TypeRegistry({32: 'picture'})
        self.assertEqual(types.register(40, 'music'), 40)
        self.assertEqual((types.name(40), types.name(1), types.name(99)),
                         ('music', 'meta', 'type99'))
        self.assertEqual((types.number('music'), types.number('voice'), types.number('77'),
                          types.number('type99'), types.number(5)), (40, 6, 77, 99, 5))
        self.assertIn((32, 'picture'), types.items())
        for number, name in ((31, 'low'), (256, 'high'), (40, 'again'), (41, 'music'),
                             (42, '12'), (43, 'a b')):
            with self.assertRaises(xpak.PackError):
                types.register(number, name)
        self.assertEqual(xpak.TypeRegistry().name(40), 'type40')    # registries are separate

    def test_what_the_reader_refuses(self):
        good = sample_writer().build()
        for data, what in ((b'NOPE' + good[4:], 'not an XPAK'),
                           (good[:4] + b'\x01\x00' + good[6:], 'version 1'),
                           (good[:-1], 'damaged pack header'),
                           (good[:6] + b'\xff\xff' + good[8:], 'damaged pack header'),
                           (good[:16 + 24] + b'\xff\xff\xff\x7f' + good[16 + 28:], 'outside')):
            with self.assertRaisesRegex(xpak.PackError, what):
                xpak.read_pack(data)


class Bundle(unittest.TestCase):
    def test_trailer_layout(self):
        pack = sample_writer().build()
        exe = b'MZ' + bytes(range(256)) * 3
        out = xpak.bundle(exe, pack)
        self.assertEqual(out[:len(exe)], exe)
        self.assertEqual(out[len(exe):len(exe) + len(pack)], pack)
        tr = out[-16:]
        self.assertEqual(len(out), len(exe) + len(pack) + 16)
        self.assertEqual((tr[0:4], tr[12:16]), (b'XPKT', b'XPKT'))
        size, inverse = struct.unpack('<II', tr[4:12])
        self.assertEqual((size, inverse), (len(pack), len(pack) ^ 0xFFFFFFFF))
        self.assertEqual(xpak.find_pack(out), (len(exe), len(pack)))
        self.assertEqual(xpak.find_pack(pack), (0, len(pack)))
        self.assertEqual(xpak.unbundle(out), (exe, pack))
        self.assertEqual(xpak.unbundle(exe), (exe, None))

    def test_bundling_again_replaces_the_pack(self):
        exe = b'\x7fELF' + bytes(100)
        first = sample_writer().build()
        w = xpak.PackWriter()
        w.add('other', xpak.T_BLOB, b'second pack')
        second = w.build()
        out = xpak.bundle(xpak.bundle(exe, first), second)
        self.assertEqual(xpak.unbundle(out), (exe, second))

    def test_damage(self):
        pack = sample_writer().build()
        out = bytearray(xpak.bundle(b'exe', pack))
        out[-8] ^= 1                                        # the inverted size
        with self.assertRaisesRegex(xpak.PackError, 'trailer'):
            xpak.find_pack(bytes(out))
        with self.assertRaisesRegex(xpak.PackError, 'no pack'):
            xpak.find_pack(b'just an executable')
        with self.assertRaises(xpak.PackError):
            xpak.bundle(b'exe', pack[:-4])


class CommandLine(unittest.TestCase):
    def run_tool(self, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            status = xpak.main(list(args))
        return status, out.getvalue()

    def test_build_list_extract_bundle(self):
        with tempfile.TemporaryDirectory() as tmp:
            def path(name):
                return os.path.join(tmp, name)
            for name, data in (('p.txt', b'id=cli\n'), ('pic.bin', b'PIC'), ('en.txt', b'hello'),
                               ('exe', b'#!program')):
                with open(path(name), 'wb') as f:
                    f.write(data)
            types = '--types=32=picture'
            status, text = self.run_tool(types, 'build', '-o', path('g.pak'),
                                         'profile:meta=' + path('p.txt'),
                                         'title.pic:picture=' + path('pic.bin'),
                                         'en/text:strings=' + path('en.txt'))
            self.assertEqual(status, 0, text)
            self.assertIn('4 entries', text)
            status, text = self.run_tool(types, 'list', path('g.pak'))
            self.assertEqual(status, 0)
            self.assertIn('languages: 1=en', text)
            self.assertRegex(text, r'TITLE\.PIC\s+picture\s+-\s+3')
            self.assertRegex(text, r'text\s+strings\s+en\s+5')
            status, text = self.run_tool('bundle', path('exe'), path('g.pak'), '-o', path('game'))
            self.assertEqual(status, 0, text)
            status, text = self.run_tool('list', path('game'))
            self.assertRegex(text, r'TITLE\.PIC\s+type32')
            status, _ = self.run_tool('extract', path('game'), path('out'))
            self.assertEqual(status, 0)
            with open(os.path.join(tmp, 'out', 'en', 'text'), 'rb') as f:
                self.assertEqual(f.read(), b'hello')
            with open(os.path.join(tmp, 'out', 'TITLE.PIC'), 'rb') as f:
                self.assertEqual(f.read(), b'PIC')
            status, _ = self.run_tool('unbundle', path('game'), '--exe', path('exe2'),
                                      '--pack', path('g2.pak'))
            self.assertEqual(status, 0)
            with open(path('exe2'), 'rb') as a, open(path('g2.pak'), 'rb') as b, \
                    open(path('g.pak'), 'rb') as c:
                self.assertEqual((a.read(), b.read()), (b'#!program', c.read()))
            status, text = self.run_tool('unbundle', path('exe'))
            self.assertEqual(status, 1)
            status, text = self.run_tool('list', path('exe'))
            self.assertEqual(status, 1)
            self.assertIn('xpak: no pack found', text)

    def test_the_launcher_under_isolated_mode(self):
        """`python3 -I tools/lrvne TOOL ...` from another directory, with no PYTHONPATH."""
        with tempfile.TemporaryDirectory() as tmp:
            pak = os.path.join(tmp, 'g.pak')
            with open(pak, 'wb') as f:
                f.write(sample_writer().build())
            env = {k: v for k, v in os.environ.items() if k != 'PYTHONPATH'}
            r = subprocess.run([sys.executable, '-I', os.path.join(TOOLS, 'lrvne'), 'xpak',
                                'list', pak], cwd=tmp, env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('7 entries', r.stdout)
            # the package's own `profile` module does not hide the standard library's
            r = subprocess.run([sys.executable, '-I', os.path.join(TOOLS, 'lrvne'), 'nonsense'],
                               cwd=tmp, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True)
            self.assertEqual(r.returncode, 2)
            self.assertIn('no tool named', r.stderr)


class Profile(unittest.TestCase):
    VALUES = {'id': 'mygame', 'title': 'My Game: the ゲーム = fun', 'lang': 'ja',
              'modules': 'a,b', 'width': 640, 'offset': -3, 'big': 0x7FFFFFFF, 'mouse': True,
              'debug': False, 'palette': [0, 7, 15, -1], 'none': [], 'empty': ''}

    def test_text(self):
        self.assertEqual(profile.dumps({'id': 'x', 'n': 5, 'on': True, 'list': (1, 2)}),
                         'id=x\nn=5\non=yes\nlist=1,2\n')

    def test_round_trip(self):
        text = profile.dumps(self.VALUES)
        raw = profile.loads(text)
        self.assertEqual(list(raw), list(self.VALUES))
        self.assertEqual(raw['title'], self.VALUES['title'])
        self.assertEqual(raw['empty'], '')
        self.assertEqual(profile.parse_int(raw['width']), 640)
        self.assertEqual(profile.parse_int(raw['offset']), -3)
        self.assertEqual(raw['big'], '0x7FFFFFFF')          # beyond the decimal the C side reads
        self.assertEqual(profile.parse_int(raw['big']), 0x7FFFFFFF)
        self.assertIs(profile.parse_bool(raw['mouse']), True)
        self.assertIs(profile.parse_bool(raw['debug']), False)
        self.assertEqual(profile.parse_ints(raw['palette']), [0, 7, 15, -1])
        self.assertEqual(profile.parse_ints(raw['none']), [])
        self.assertEqual(profile.loads(text.encode('utf-8')), raw)

    def test_reading_as_the_c_parser_does(self):
        raw = profile.loads('# comment\n\n  key = value with spaces \r\nx=1\nx=2\n=\n')
        self.assertEqual(raw, {'key': 'value with spaces', 'x': '2'})
        with self.assertRaisesRegex(profile.ProfileError, "no '='"):
            profile.loads('just a word\n')
        self.assertEqual(profile.parse_int('0x1F'), 31)
        self.assertEqual(profile.parse_int('+7'), 7)
        self.assertEqual(profile.parse_int('1342177279'), 1342177279)
        for bad in ('', '-', '0x', '12a', '1342177280', '1.5', '0x80000000'):
            with self.assertRaises(profile.ProfileError):
                profile.parse_int(bad)
        self.assertTrue(profile.parse_bool('ON'))
        self.assertFalse(profile.parse_bool('off'))
        with self.assertRaises(profile.ProfileError):
            profile.parse_bool('maybe')
        self.assertEqual(profile.parse_ints('1, 0x10 ,3'), [1, 16, 3])

    def test_what_the_c_parser_would_not_read_back(self):
        for values in ({'a\nb': 1}, {'a=b': 1}, {'': 1}, {' a': 1}, {'#a': 1}, {'k' * 48: 1},
                       {'a': 'line\nbreak'}, {'a': 'cr\r'}, {'a': ' padded'}, {'a': 'padded\t'},
                       {'a': 'nul\0'}, {'a': 1 << 31}, {'a': [1, 'x']}, {'a': 1.5}, {'a': None},
                       {'a': [True]}, {5: 'x'}):
            with self.assertRaises(profile.ProfileError, msg=repr(values)):
                profile.dumps(values)

    def test_in_a_pack(self):
        w = xpak.PackWriter()
        w.add('profile', xpak.T_META, profile.dumps(self.VALUES).encode('utf-8'))
        e = xpak.read_pack(w.build()).find('profile', xpak.T_META)
        self.assertEqual(profile.loads(e.data)['title'], self.VALUES['title'])


if __name__ == '__main__':
    unittest.main()
