# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a voice pack: the second pack of a game, holding its voice clips.

A voice pack is an ordinary pack (the `xpak` module) with
  - one entry of type VOICE per clip, named after the clip (a-z upper-cased), neutral;
  - the entry "voice" of type META that describes all of them, key=value lines:
        format=1
        codec=pcm8        the payload of every VOICE entry (below, or an engine's own name)
        rate=11025        the sample rate the clips play at, Hz
        clips=<count>     number of clip names
        cues=<digest>     optional: identifies the cue table the clips belong to, so an
                          engine can refuse a voice pack that does not match its main pack
        bitrate=<kbit/s>  opus only
    and whatever further keys the caller passes.
Identical clips are stored once, and the clips' data is laid out in the order the caller
supplies them (the order of play, so that reads move forward through the file). Which line of
a script plays which clip (the cue table) is an engine's business and is not in this pack.

Clip payloads:
  pcm8  (little-endian)  "pcm8", u32 sample rate, u32 sample count, then the samples: signed
                         8-bit, mono
  opus  (big-endian)     "opus", u32 sample rate the clip decodes at (24000), mono,
                         u32 sample count after the pre-skip, u16 pre-skip (decoded samples to
                         drop at the start), u16 0, then packets until the end of the entry:
                         u16 byte count, the Opus packet (RFC 6716)
  any other codec name   the caller's bytes, stored as they are (an original format the engine
                         decodes itself)

`build_voice_pack` takes the clips as an iterable of (name, data, codec, rate), where `data`
is, by codec: 'pcm8' the signed 8-bit samples, 'pcm16' signed 16-bit little-endian samples
(both mono, without a header), anything else a finished payload. With `codec='opus'` the
clips are transcoded through an external `ffmpeg` (with libopus) or `opusenc`; a payload in an
engine's own format needs a decoder from the caller for that.

Command line (python3 -I tools/lrvne voicepack ...):
  info PACK                                     the "voice" entry and the clip count
  build -o PACK [--codec opus] [--bitrate K] [--cues DIGEST] CLIP.wav...
                                                a pack from mono WAV files (8 or 16 bit), the
                                                clips named after the files
"""
import argparse
import concurrent.futures
import os
import shutil
import struct
import subprocess
import sys
import wave
from typing import Callable, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

from . import profile, xpak

PCM8, PCM16, OPUS = 'pcm8', 'pcm16', 'opus'
OPUS_RATE = 24000               # the rate opus clips decode at
META_ENTRY = 'voice'
FORMAT = 1

Clip = Tuple[str, bytes, str, int]                      # name, data, codec, sample rate
Decoder = Callable[[bytes], Tuple[int, bytes]]          # payload -> (rate, 16-bit LE samples)


class VoiceError(Exception):
    """A voice pack cannot be built."""


# ---- clip payloads -----------------------------------------------------------------------------

def pcm8_clip(samples: bytes, rate: int) -> bytes:
    """The pcm8 payload of signed 8-bit mono samples."""
    return b'pcm8' + struct.pack('<II', rate, len(samples)) + bytes(samples)


def read_pcm8_clip(data: bytes) -> Tuple[int, bytes]:
    """(sample rate, samples) of a pcm8 payload."""
    if data[:4] != b'pcm8' or len(data) < 12:
        raise VoiceError('not a pcm8 clip')
    rate, count = struct.unpack_from('<II', data, 4)
    if count != len(data) - 12:
        raise VoiceError('pcm8 clip: %d samples stated, %d present' % (count, len(data) - 12))
    return rate, data[12:]


def opus_clip(rate: int, count: int, preskip: int, packets: Sequence[bytes]) -> bytes:
    """The opus payload of a list of Opus packets."""
    out = [b'opus', struct.pack('>IIHH', rate, count, preskip, 0)]
    for p in packets:
        if not 0 < len(p) <= 1275:
            raise VoiceError('Opus packet of %d bytes' % len(p))
        out += [struct.pack('>H', len(p)), p]
    return b''.join(out)


def read_opus_clip(data: bytes) -> Tuple[int, int, int, List[bytes]]:
    """(sample rate, sample count, pre-skip, packets) of an opus payload."""
    if data[:4] != b'opus' or len(data) < 16:
        raise VoiceError('not an opus clip')
    rate, count, preskip, _ = struct.unpack_from('>IIHH', data, 4)
    packets, pos = [], 16
    while pos < len(data):
        if pos + 2 > len(data):
            raise VoiceError('opus clip: cut off in a packet length')
        n, = struct.unpack_from('>H', data, pos)
        if pos + 2 + n > len(data):
            raise VoiceError('opus clip: cut off in a packet')
        packets.append(data[pos + 2:pos + 2 + n])
        pos += 2 + n
    return rate, count, preskip, packets


def pcm8_to_pcm16(samples: bytes) -> bytes:
    """Signed 8-bit samples as signed 16-bit little-endian ones."""
    values = struct.unpack('%db' % len(samples), samples)
    return struct.pack('<%dh' % len(values), *(v << 8 for v in values))


# ---- Opus through an external encoder ----------------------------------------------------------

def ogg_packets(data: bytes) -> Tuple[List[bytes], int]:
    """The packets of a single-stream Ogg file and the last page's granule position."""
    out: List[bytes] = []
    cur, pos, granule = b'', 0, 0
    while pos < len(data):
        if data[pos:pos + 4] != b'OggS' or pos + 27 > len(data):
            raise VoiceError('bad Ogg page at byte %d' % pos)
        granule = struct.unpack_from('<q', data, pos + 6)[0]
        nseg = data[pos + 26]
        lacing = data[pos + 27:pos + 27 + nseg]
        pos += 27 + nseg
        for n in lacing:
            cur += data[pos:pos + n]
            pos += n
            if n < 255:
                out.append(cur)
                cur = b''
    return out, granule


def find_encoder() -> Optional[Tuple[str, str]]:
    """The Opus encoder on the PATH as (kind, executable): 'ffmpeg' or 'opusenc'; None when
    neither is installed."""
    for kind in ('ffmpeg', 'opusenc'):
        exe = shutil.which(kind)
        if exe:
            return kind, exe
    return None


def encode_opus(pcm16: bytes, rate: int, bitrate: int = 24,
                encoder: Optional[Tuple[str, str]] = None) -> bytes:
    """Signed 16-bit little-endian mono samples -> an opus payload, by an external encoder.

    `encoder` is (kind, executable) as find_encoder returns it (default: what is on the PATH).
    VoiceError when there is no encoder or it fails.
    """
    encoder = encoder or find_encoder()
    if encoder is None:
        raise VoiceError('encoding to Opus needs ffmpeg (built with libopus) or opusenc on the '
                         'PATH')
    kind, exe = encoder
    if kind == 'ffmpeg':
        cmd = [exe, '-v', 'error', '-f', 's16le', '-ar', str(rate), '-ac', '1', '-i', '-',
               '-ar', str(OPUS_RATE), '-c:a', 'libopus', '-b:a', '%dk' % bitrate,
               '-application', 'audio', '-f', 'ogg', '-']
    else:
        cmd = [exe, '--quiet', '--raw', '--raw-bits', '16', '--raw-rate', str(rate),
               '--raw-chan', '1', '--raw-endianness', '0', '--bitrate', str(bitrate), '-', '-']
    try:
        r = subprocess.run(cmd, input=pcm16, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as e:
        raise VoiceError('%s: %s' % (exe, e.strerror or e)) from e
    if r.returncode != 0:
        tail = r.stderr.decode('utf-8', 'replace').strip().splitlines()[-1:] or ['failed']
        raise VoiceError('%s: %s' % (os.path.basename(exe), tail[0]))
    packets, granule = ogg_packets(r.stdout)
    if len(packets) < 2 or not packets[0].startswith(b'OpusHead') or \
            not packets[1].startswith(b'OpusTags'):
        raise VoiceError('%s did not write an Ogg Opus stream' % os.path.basename(exe))
    pre48 = struct.unpack_from('<H', packets[0], 10)[0]     # Ogg Opus counts in 48 kHz units
    scale = 48000 // OPUS_RATE
    count = (granule - pre48) // scale
    expect = (len(pcm16) // 2) * OPUS_RATE // rate
    if abs(count - expect) > 2:
        raise VoiceError('Opus stream of %d samples, expected %d' % (count, expect))
    return opus_clip(OPUS_RATE, count, pre48 // scale, packets[2:])


# ---- the pack ----------------------------------------------------------------------------------

def _to_pcm16(name: str, data: bytes, codec: str, rate: int,
              decoders: Mapping[str, Decoder]) -> Tuple[int, bytes]:
    if codec == PCM16:
        return rate, data
    if codec == PCM8:
        return rate, pcm8_to_pcm16(data)
    if codec in decoders:
        return decoders[codec](data)
    raise VoiceError('clip %s: no decoder for codec %s, so it cannot be transcoded' % (name, codec))


def build_voice_pack(clips: Iterable[Clip], codec: Optional[str] = None,
                     cues: Optional[str] = None, bitrate: int = 24, jobs: Optional[int] = None,
                     decoders: Optional[Mapping[str, Decoder]] = None,
                     encoder: Optional[Tuple[str, str]] = None,
                     meta: Optional[Mapping[str, profile.Value]] = None) -> bytes:
    """The bytes of a voice pack.

    clips     iterable of (name, data, codec, rate) in the order of play (see the module's
              description for `data`); a name may come once only
    codec     None: store every clip in the codec it comes in; 'opus': transcode them
              (`bitrate` kbit/s, `jobs` encoders at a time, default: one per processor)
    cues      the digest written as cues=
    decoders  {codec: function(payload) -> (rate, 16-bit LE mono samples)} for transcoding
              clips that come in an engine's own format
    encoder   the Opus encoder as (kind, executable); default: found on the PATH
    meta      further key=value pairs for the "voice" entry
    All clips must end up with one codec and one sample rate: the "voice" entry states one.
    """
    if codec not in (None, OPUS):
        raise VoiceError('codec %r: clips can be transcoded to opus only' % codec)
    items: List[Clip] = []
    seen = set()
    for name, data, clip_codec, rate in clips:
        key = xpak.upper_ascii(name)
        if key in seen:
            raise VoiceError('clip %s is supplied twice' % name)
        seen.add(key)
        items.append((name, bytes(data), clip_codec, int(rate)))
    if not items:
        raise VoiceError('no clips')
    payloads: List[bytes]
    if codec == OPUS and any(c != OPUS for _n, _d, c, _r in items):
        encoder = encoder or find_encoder()
        if encoder is None:
            raise VoiceError('encoding to Opus needs ffmpeg (built with libopus) or opusenc on '
                             'the PATH')
        todo: Dict[Tuple[bytes, str, int], bytes] = {}      # identical clips are encoded once

        def encode(key: Tuple[bytes, str, int]) -> bytes:
            data, clip_codec, rate = key
            if clip_codec == OPUS:
                return data
            rate, pcm = _to_pcm16('', data, clip_codec, rate, decoders or {})
            return encode_opus(pcm, rate, bitrate, encoder)
        for name, data, clip_codec, rate in items:
            if clip_codec not in (OPUS, PCM8, PCM16) and clip_codec not in (decoders or {}):
                raise VoiceError('clip %s: no decoder for codec %s, so it cannot be transcoded'
                                 % (name, clip_codec))
            todo[(data, clip_codec, rate)] = b''
        keys = list(todo)
        # the work is done by the encoder processes: threads are enough to run them side by side
        with concurrent.futures.ThreadPoolExecutor(jobs or os.cpu_count() or 1) as pool:
            for key, payload in zip(keys, pool.map(encode, keys)):
                todo[key] = payload
        payloads = [todo[(d, c, r)] for _n, d, c, r in items]
        codecs, rates = {OPUS}, {OPUS_RATE}
    else:
        payloads = []
        for name, data, clip_codec, rate in items:
            if clip_codec == PCM16:
                raise VoiceError('clip %s: 16-bit samples are stored as opus only (codec=\'opus\')'
                                 % name)
            payloads.append(pcm8_clip(data, rate) if clip_codec == PCM8 else data)
        codecs = {c for _n, _d, c, _r in items}
        rates = {r for _n, _d, _c, r in items}
    if len(codecs) != 1 or len(rates) != 1:
        raise VoiceError('the clips must share one codec and one sample rate (found %s at %s Hz)'
                         % ('/'.join(sorted(codecs)), '/'.join(str(r) for r in sorted(rates))))
    out_codec = codecs.pop()
    values: Dict[str, profile.Value] = {'format': FORMAT, 'codec': out_codec,
                                        'rate': rates.pop(), 'clips': len(items)}
    if cues is not None:
        values['cues'] = cues
    if out_codec == OPUS and codec == OPUS:
        values['bitrate'] = bitrate
    for k, v in (meta or {}).items():
        if k in values:
            raise VoiceError('the key %s of the "voice" entry is written by the builder' % k)
        values[k] = v
    try:
        text = profile.dumps(values)
        w = xpak.PackWriter()
        for (name, _d, _c, _r), payload in zip(items, payloads):
            w.add(name, xpak.T_VOICE, payload)
        w.add(META_ENTRY, xpak.T_META, text.encode('utf-8'))
        return w.build(dedupe=True, data_order=[n for n, _d, _c, _r in items])
    except (xpak.PackError, profile.ProfileError) as e:
        raise VoiceError(str(e)) from None


def read_voice_meta(pack: xpak.Pack) -> Dict[str, str]:
    """The key=value pairs of a voice pack's "voice" entry."""
    e = pack.find(META_ENTRY, xpak.T_META)
    if e is None:
        raise VoiceError('not a voice pack: no "%s" entry' % META_ENTRY)
    return profile.loads(e.data)


# ---- command line ------------------------------------------------------------------------------

def wav_clip(path: str) -> Clip:
    """A mono WAV file (8 or 16 bit) as a clip named after the file."""
    try:
        with wave.open(path, 'rb') as w:
            if w.getnchannels() != 1 or w.getsampwidth() not in (1, 2):
                raise VoiceError('%s: not a mono WAV file of 8 or 16 bits' % path)
            frames = w.readframes(w.getnframes())
            name = os.path.splitext(os.path.basename(path))[0]
            if w.getsampwidth() == 2:
                return name, frames, PCM16, w.getframerate()
            return name, bytes(b ^ 0x80 for b in frames), PCM8, w.getframerate()    # to signed
    except (wave.Error, EOFError, OSError) as e:
        raise VoiceError('%s: %s' % (path, e)) from None


def main(argv: Optional[Sequence[str]] = None) -> int:
    """The command line (see the module's description)."""
    ap = argparse.ArgumentParser(prog='lrvne voicepack', description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('info', help='the "voice" entry and the clip count of a voice pack')
    p.add_argument('pack')
    p = sub.add_parser('build', help='a voice pack from mono WAV files')
    p.add_argument('wavs', nargs='+')
    p.add_argument('-o', '--output', required=True)
    p.add_argument('--codec', choices=(OPUS,), help='transcode the clips')
    p.add_argument('--bitrate', type=int, default=24, help='opus: kbit/s (default 24)')
    p.add_argument('--jobs', type=int, help='opus: encoders at a time')
    p.add_argument('--cues', help='the digest of the cue table the clips belong to')
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'info':
            with open(a.pack, 'rb') as f:
                data = f.read()
            base, size = xpak.find_pack(data)
            pack = xpak.read_pack(data[base:base + size])
            for k, v in read_voice_meta(pack).items():
                print('%s=%s' % (k, v))
            print('# %d VOICE entries, %d bytes' % (
                sum(1 for e in pack.entries if e.type == xpak.T_VOICE), pack.total))
            return 0
        data = build_voice_pack((wav_clip(p) for p in a.wavs), a.codec, a.cues, a.bitrate, a.jobs)
        with open(a.output, 'wb') as f:
            f.write(data)
        print('%s: %d clips, %d bytes' % (a.output, len(a.wavs), len(data)))
        return 0
    except (VoiceError, xpak.PackError, OSError) as e:
        print('voicepack: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
