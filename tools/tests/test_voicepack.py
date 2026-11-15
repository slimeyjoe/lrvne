# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of lrvne.voicepack: the "voice" entry, pcm8 clips, passthrough, Opus transcoding."""
import contextlib
import io
import math
import os
import struct
import subprocess
import tempfile
import unittest
import wave

from lrvne import profile, voicepack, xpak


def tone8(count, period=20):
    """Signed 8-bit samples of a sine."""
    return bytes(int(100 * math.sin(2 * math.pi * i / period)) & 0xFF for i in range(count))


def tone16(count, period=40):
    return struct.pack('<%dh' % count, *(int(12000 * math.sin(2 * math.pi * i / period))
                                         for i in range(count)))


def opus_encoder():
    """An encoder that works here, or None: ffmpeg must have been built with libopus."""
    found = voicepack.find_encoder()
    if found and found[0] == 'ffmpeg':
        try:
            r = subprocess.run([found[1], '-hide_banner', '-encoders'], stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL)
        except OSError:
            return None
        if b'libopus' not in r.stdout:
            return None
    return found


class Pcm8(unittest.TestCase):
    def test_pack_layout(self):
        a, b = tone8(500), tone8(300, 13)
        clips = [('b0002', b, 'pcm8', 11025), ('b0001', a, 'pcm8', 11025),
                 ('b0003', a, 'pcm8', 11025)]                       # a repeated line
        data = voicepack.build_voice_pack(clips, cues='0123abcd', meta={'source': 'test'})
        pack = xpak.read_pack(data)
        self.assertEqual([(e.name, e.type, e.lang) for e in pack.entries],
                         [('B0001', xpak.T_VOICE, 0), ('B0002', xpak.T_VOICE, 0),
                          ('B0003', xpak.T_VOICE, 0), ('voice', xpak.T_META, 0)])
        meta = pack.find('voice', xpak.T_META).data
        self.assertEqual(meta, b'format=1\ncodec=pcm8\nrate=11025\nclips=3\ncues=0123abcd\n'
                               b'source=test\n')
        self.assertEqual(voicepack.read_voice_meta(pack),
                         {'format': '1', 'codec': 'pcm8', 'rate': '11025', 'clips': '3',
                          'cues': '0123abcd', 'source': 'test'})
        clip = pack.find('b0002').data
        self.assertEqual(clip, b'pcm8' + struct.pack('<II', 11025, 300) + b)
        self.assertEqual(voicepack.read_pcm8_clip(clip), (11025, b))
        by = {e.name: e for e in pack.entries}
        self.assertEqual(by['B0001'].offset, by['B0003'].offset)        # stored once
        self.assertLess(by['B0002'].offset, by['B0001'].offset)         # in the order supplied

    def test_passthrough_of_an_engines_own_format(self):
        clips = [('x1', b'orig-format-one', 'myfmt', 22050), ('x2', b'two', 'myfmt', 22050)]
        pack = xpak.read_pack(voicepack.build_voice_pack(iter(clips)))
        self.assertEqual(pack.find('X1').data, b'orig-format-one')
        self.assertEqual(profile.loads(pack.find('voice').data),
                         {'format': '1', 'codec': 'myfmt', 'rate': '22050', 'clips': '2'})

    def test_what_is_refused(self):
        ok = ('a', tone8(10), 'pcm8', 8000)
        for clips, kw, what in (
                ([], {}, 'no clips'),
                ([ok, ('A', tone8(5), 'pcm8', 8000)], {}, 'twice'),
                ([ok, ('b', tone8(5), 'pcm8', 11025)], {}, 'one sample rate'),
                ([ok, ('b', b'x', 'other', 8000)], {}, 'one codec'),
                ([('w', tone16(10), 'pcm16', 8000)], {}, 'opus only'),
                ([ok], {'codec': 'mp3'}, 'opus only'),
                ([ok], {'meta': {'codec': 'x'}}, 'written by the builder'),
                ([ok], {'meta': {'bad key': 'a\nb'}}, 'line break'),
                ([('n' * 21, tone8(5), 'pcm8', 8000)], {}, 'name'),
                ([('z', b'x', 'myfmt', 8000)], {'codec': 'opus', 'encoder': ('ffmpeg', 'ffmpeg')},
                 'no decoder')):
            with self.assertRaisesRegex(voicepack.VoiceError, what):
                voicepack.build_voice_pack(clips, **kw)
        with self.assertRaises(voicepack.VoiceError):
            voicepack.read_pcm8_clip(b'pcm8' + struct.pack('<II', 8000, 99) + b'short')
        with self.assertRaises(voicepack.VoiceError):
            voicepack.read_voice_meta(xpak.read_pack(xpak.PackWriter().build()))

    def test_clip_helpers(self):
        self.assertEqual(voicepack.pcm8_to_pcm16(bytes((0, 1, 0xFF, 0x80))),
                         struct.pack('<4h', 0, 256, -256, -32768))
        clip = voicepack.opus_clip(24000, 480, 156, [b'\x08abc', b'\x08de'])
        self.assertEqual(clip, b'opus' + struct.pack('>IIHH', 24000, 480, 156, 0)
                         + b'\x00\x04\x08abc' + b'\x00\x03\x08de')
        self.assertEqual(voicepack.read_opus_clip(clip),
                         (24000, 480, 156, [b'\x08abc', b'\x08de']))
        with self.assertRaises(voicepack.VoiceError):
            voicepack.opus_clip(24000, 1, 0, [bytes(1276)])
        with self.assertRaises(voicepack.VoiceError):
            voicepack.read_opus_clip(clip[:-1])
        page = b'OggS\0\0' + struct.pack('<q', 960) + bytes(12) + bytes((2, 3, 2)) + b'abcde'
        self.assertEqual(voicepack.ogg_packets(page), ([b'abc', b'de'], 960))
        with self.assertRaises(voicepack.VoiceError):
            voicepack.ogg_packets(b'not ogg')

    def test_wav_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths = []
            for name, samples in (('line1', tone8(400)), ('line2', tone8(200, 9))):
                path = os.path.join(tmp, name + '.wav')
                with wave.open(path, 'wb') as w:
                    w.setnchannels(1)
                    w.setsampwidth(1)
                    w.setframerate(11025)
                    w.writeframes(bytes(b ^ 0x80 for b in samples))     # WAV bytes are unsigned
                paths.append(path)
            out = os.path.join(tmp, 'voice.pak')
            text = io.StringIO()
            with contextlib.redirect_stdout(text):
                self.assertEqual(voicepack.main(['build', '-o', out, '--cues', 'abc'] + paths), 0)
                self.assertEqual(voicepack.main(['info', out]), 0)
            self.assertIn('codec=pcm8\nrate=11025\nclips=2\ncues=abc\n', text.getvalue())
            self.assertIn('# 2 VOICE entries', text.getvalue())
            with open(out, 'rb') as f:
                pack = xpak.read_pack(f.read())
            self.assertEqual(voicepack.read_pcm8_clip(pack.find('LINE1').data),
                             (11025, tone8(400)))
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(voicepack.main(['build', '-o', out, out]), 1)
            self.assertIn('voicepack:', err.getvalue())


class Opus(unittest.TestCase):
    def test_no_encoder(self):
        clips = [('a', tone16(100), 'pcm16', 22050)]
        missing = ('ffmpeg', os.path.join(tempfile.gettempdir(), 'no-such-dir', 'ffmpeg'))
        with self.assertRaises(voicepack.VoiceError):
            voicepack.build_voice_pack(clips, codec='opus', encoder=missing)

    @unittest.skipUnless(opus_encoder(), 'needs ffmpeg with libopus, or opusenc')
    def test_transcode(self):
        encoder = opus_encoder()
        rate, count = 22050, 22050 // 2                                 # half a second
        clips = [('line1', tone16(count), 'pcm16', rate),
                 ('line2', tone8(11025 // 2), 'pcm8', 11025),           # another input rate
                 ('line3', tone16(count), 'pcm16', rate),               # identical to line1
                 ('line4', b'own:' + tone16(count, 25), 'myfmt', 0)]
        data = voicepack.build_voice_pack(
            clips, codec='opus', bitrate=16, jobs=2, encoder=encoder,
            decoders={'myfmt': lambda payload: (rate, payload[4:])})
        pack = xpak.read_pack(data)
        self.assertEqual(voicepack.read_voice_meta(pack),
                         {'format': '1', 'codec': 'opus', 'rate': '24000', 'clips': '4',
                          'bitrate': '16'})
        by = {e.name: e for e in pack.entries}
        self.assertEqual(by['LINE1'].offset, by['LINE3'].offset)
        for name in ('LINE1', 'LINE2', 'LINE4'):
            clip_rate, samples, preskip, packets = voicepack.read_opus_clip(by[name].data)
            self.assertEqual(clip_rate, 24000)
            self.assertAlmostEqual(samples, 12000, delta=2)
            self.assertGreater(preskip, 0)
            self.assertGreater(len(packets), 10)
            self.assertTrue(all(0 < len(p) <= 1275 for p in packets))
        self.assertLess(len(data), count * 2 * 3)                       # it did compress


if __name__ == '__main__':
    unittest.main()
