# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of lrvne.cdimg: sheets, raw and cooked sectors, ISO 9660, audio tracks."""
import contextlib
import io
import os
import tempfile
import unittest
import wave

from lrvne import cdimg

from . import fixtures as fx

FILES = {
    'README.TXT': b'on the disc\r\n',
    'NOEXT': b'a name without an extension',
    'DATA/BIG.BIN': fx.pattern(9000, 7),            # five sectors
    'DATA/MORE/DEEP.DAT': fx.pattern(100, 9),
}
CCD = """[CloneCD]
Version=3
[Disc]
TocEntries=5
[Entry 0]
Point=0xa0
PLBA=0
[Entry 1]
Point=0xa2
Control=0x00
PLBA=%(leadout)d
[Entry 2]
Point=0x01
Control=0x04
PLBA=0
[Entry 3]
Point=0x02
Control=0x00
PLBA=%(audio)d
[TRACK 1]
MODE=1
INDEX 1=0
[TRACK 2]
MODE=0
INDEX 0=%(pregap)d
INDEX 1=%(audio)d
"""


class Disc(unittest.TestCase):
    """A data track of raw sectors followed by two audio tracks, as BIN + CUE."""

    @classmethod
    def setUpClass(cls):
        cls.iso = fx.make_iso(FILES)
        cls.data_sectors = len(cls.iso) // fx.SECTOR
        cls.audio2 = fx.audio_sectors(6, 3)
        cls.audio3 = fx.audio_sectors(4, 5)
        cls.silence = bytes(2 * fx.RAW_SECTOR)
        cls.bin = fx.raw_wrap(cls.iso) + cls.audio2 + cls.silence + cls.audio3
        cls.tmp = tempfile.TemporaryDirectory()
        cls.bin_path = os.path.join(cls.tmp.name, 'DISC.BIN')
        cls.cue_path = os.path.join(cls.tmp.name, 'disc.cue')
        with open(cls.bin_path, 'wb') as f:
            f.write(cls.bin)
        with open(cls.cue_path, 'w') as f:      # the sheet names the file in another case
            f.write(fx.cue_sheet('disc.bin', [('MODE1/2352', 0), ('AUDIO', cls.data_sectors),
                                              ('AUDIO', cls.data_sectors + 8)]))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def check_volume(self, cd):
        vol = cd.volume()
        self.assertEqual((vol.name, vol.system, vol.label), ('TESTVOL', 'TESTSYS', 'TESTVOL'))
        self.assertEqual(vol.info['publisher'], 'PUBLISHER')
        self.assertEqual({e.path: vol.read_entry(e) for e in vol.files()}, FILES)
        dirs = [e.path for e in vol.entries() if e.is_dir]
        self.assertEqual(dirs, ['DATA', 'DATA/MORE'])
        entry = next(e for e in vol.files() if e.path == 'DATA/BIG.BIN')
        self.assertEqual((entry.name, entry.size, entry.date), ('BIG.BIN', 9000, fx.ISO_STAMP))
        self.assertEqual(vol.read('data/more/deep.dat'), FILES['DATA/MORE/DEEP.DAT'])
        with self.assertRaises(cdimg.CdImageError):
            vol.read('ABSENT')

    def test_through_the_cue_sheet(self):
        with cdimg.open_cd(self.cue_path) as cd:
            self.assertEqual(cd.sheet, 'cue')
            self.assertEqual([(t.number, t.mode, t.start, t.end) for t in cd.tracks],
                             [(1, 'MODE1/2352', 0, self.data_sectors),
                              (2, 'AUDIO', self.data_sectors, self.data_sectors + 8),
                              (3, 'AUDIO', self.data_sectors + 8, self.data_sectors + 12)])
            self.assertEqual(cd.sector_size(), 2352)
            self.assertEqual([t.number for t in cd.audio_tracks()], [2, 3])
            self.check_volume(cd)
            self.assertEqual(cd.pcm(cd.track(2)), self.audio2 + self.silence)
            self.assertEqual(cd.pcm(cd.track(3)), self.audio3)
            with self.assertRaises(cdimg.CdImageError):
                cd.pcm(cd.track(1))
            with self.assertRaises(cdimg.CdImageError):
                cd.track(9)

    def test_through_the_image_finds_the_sheet(self):
        with cdimg.open_cd(self.bin_path) as cd:
            self.assertEqual((cd.sheet, len(cd.tracks)), ('cue', 3))
            self.check_volume(cd)

    def test_wav(self):
        out = os.path.join(self.tmp.name, 'track03.wav')
        with cdimg.open_cd(self.cue_path) as cd:
            self.assertEqual(cd.write_wav(cd.track(3), out), 4)
        with wave.open(out, 'rb') as w:
            self.assertEqual((w.getnchannels(), w.getsampwidth(), w.getframerate()),
                             (2, 2, 44100))
            self.assertEqual(w.readframes(w.getnframes()), self.audio3)

    def test_silence_before_a_late_track_start(self):
        with cdimg.open_cd(self.cue_path) as cd:
            t3 = cd.track(3)
            self.assertEqual(cd.silence_start(t3), self.data_sectors + 6)
            self.assertEqual(cd.pcm(t3, cd.silence_start(t3)), self.silence + self.audio3)
            self.assertIsNone(cd.silence_start(cd.track(2)))    # data comes before it

    def test_data_track_end_and_iso_command(self):
        out = os.path.join(self.tmp.name, 'data.iso')
        with cdimg.open_cd(self.bin_path) as cd:
            self.assertEqual(cd.data_track_end(), self.data_sectors)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(cdimg.main(['iso', self.cue_path, out]), 0)
        with open(out, 'rb') as f:
            self.assertEqual(f.read(), self.iso)

    def test_ccd(self):
        ccd = os.path.join(self.tmp.name, 'clone.ccd')
        img = os.path.join(self.tmp.name, 'clone.img')
        with open(img, 'wb') as f:
            f.write(self.bin)
        with open(ccd, 'w') as f:
            f.write(CCD % {'leadout': self.data_sectors + 12, 'audio': self.data_sectors + 2,
                           'pregap': self.data_sectors})
        for path in (ccd, img):
            with cdimg.open_cd(path) as cd:
                self.assertEqual((cd.sheet, cd.leadout), ('ccd', self.data_sectors + 12))
                self.assertEqual([(t.number, t.mode, t.pregap, t.start, t.end) for t in cd.tracks],
                                 [(1, 'MODE1/2352', None, 0, self.data_sectors + 2),
                                  (2, 'AUDIO', self.data_sectors, self.data_sectors + 2,
                                   self.data_sectors + 12)])
                self.check_volume(cd)

    def test_command_line(self):
        def run_tool(*args):
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                return cdimg.main(list(args)), out.getvalue()
        status, text = run_tool('tracks', self.cue_path)
        self.assertEqual(status, 0)
        self.assertIn('3\tAUDIO', text)
        status, text = run_tool('ls', self.bin_path)
        self.assertEqual(status, 0)
        self.assertIn('; volume: TESTVOL', text)
        self.assertIn('DATA/MORE/DEEP.DAT', text)
        dest = os.path.join(self.tmp.name, 'out')
        status, _ = run_tool('extract', self.cue_path, dest, 'data/big.bin')
        self.assertEqual(status, 0)
        self.assertEqual(os.listdir(dest), ['DATA'])
        with open(os.path.join(dest, 'DATA', 'BIG.BIN'), 'rb') as f:
            self.assertEqual(f.read(), FILES['DATA/BIG.BIN'])
        status, text = run_tool('wav', self.cue_path, '1', os.path.join(dest, 'x.wav'))
        self.assertEqual(status, 1)
        self.assertIn('not an audio track', text)


class OtherImages(unittest.TestCase):
    def test_cooked_iso_from_bytes(self):
        with cdimg.open_cd(fx.make_iso(FILES), 'disc.iso') as cd:
            self.assertEqual((cd.sector_size(), cd.tracks[0].mode), (2048, 'MODE1/2048'))
            vol = cd.volume()
            self.assertEqual({e.path: vol.read_entry(e) for e in vol.files()}, FILES)

    def test_mode_2_form_1(self):
        with cdimg.open_cd(fx.raw_wrap(fx.make_iso(FILES), mode=2)) as cd:
            self.assertEqual(cd.volume().read('README.TXT'), FILES['README.TXT'])

    def test_bytes_with_a_sheet(self):
        iso = fx.make_iso(FILES)
        n = len(iso) // fx.SECTOR
        sheet = fx.cue_sheet('x.bin', [('MODE1/2352', 0), ('AUDIO', n)])
        with cdimg.open_cd(fx.raw_wrap(iso) + fx.audio_sectors(2), 'x.bin', sheet) as cd:
            self.assertEqual(len(cd.pcm(cd.track(2))), 2 * 2352)

    def test_not_a_cd(self):
        with self.assertRaises(cdimg.CdImageError):
            cdimg.open_cd(bytes(40000), 'floppy.img')
        with cdimg.open_cd(bytes(40000), 'empty.iso') as cd:
            with self.assertRaisesRegex(cdimg.CdImageError, 'primary volume descriptor'):
                cd.volume()

    def test_one_file_per_track(self):
        iso = fx.make_iso(FILES)
        audio = fx.audio_sectors(3, 7)
        with tempfile.TemporaryDirectory() as tmp:
            with open(os.path.join(tmp, 'd (Track 1).bin'), 'wb') as f:
                f.write(fx.raw_wrap(iso))
            with open(os.path.join(tmp, 'd (Track 2).bin'), 'wb') as f:
                f.write(audio)
            cue = os.path.join(tmp, 'd.cue')
            with open(cue, 'w') as f:
                f.write('FILE "d (Track 1).bin" BINARY\n  TRACK 01 MODE1/2352\n'
                        '    INDEX 01 00:00:00\n'
                        'FILE "d (Track 2).bin" BINARY\n  TRACK 02 AUDIO\n'
                        '    INDEX 00 00:00:00\n    INDEX 01 00:00:01\n')
            with cdimg.open_cd(cue) as cd:
                self.assertEqual(cd.volume().read('NOEXT'), FILES['NOEXT'])
                t = cd.track(2)
                self.assertEqual((t.pregap, t.start, t.end), (0, 1, 3))
                self.assertEqual(cd.pcm(t), audio[2352:])

    def test_sheet_errors(self):
        with self.assertRaises(cdimg.CdImageError):
            cdimg.parse_cue('REM nothing here\n')
        with self.assertRaises(cdimg.CdImageError):
            cdimg.parse_cue('FILE "a.bin" BINARY\nTRACK 01 AUDIO\nINDEX 01 xx:yy:zz\n')
        with self.assertRaises(cdimg.CdImageError):
            cdimg.parse_ccd('[Disc]\nTocEntries=0\n')
        with tempfile.TemporaryDirectory() as tmp:
            cue = os.path.join(tmp, 'lost.cue')
            with open(cue, 'w') as f:
                f.write(fx.cue_sheet('lost.bin', [('MODE1/2352', 0)]))
            with self.assertRaisesRegex(cdimg.CdImageError, 'lost.bin'):
                cdimg.open_cd(cue)


if __name__ == '__main__':
    unittest.main()
