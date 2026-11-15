# SPDX-License-Identifier: GPL-3.0-or-later
"""CD images: the track table, the ISO 9660 file system of the data track, audio tracks.

Read are
  - cooked images: 2048-byte sectors (.iso);
  - raw images: 2352-byte sectors, mode 1 or mode 2 form 1 (.bin, .img), the kind a CUE sheet
    or a CloneCD control file (.ccd) describes. The sheet is looked up next to the image (or
    the image next to the sheet, whichever is named); without one the image is a single data
    track. A CUE sheet may name one file per track.

Of ISO 9660 the primary volume descriptor and plain directories are read: no Joliet, no Rock
Ridge. Names lose their version (";1") and a trailing dot. Files in several extents are joined.
The logical sectors of the file system are taken to count from the start of the file that
holds the first data track, as on a disc whose first track is the data track.

    with cdimg.open_cd('disc.cue') as cd:
        for t in cd.tracks: ...
        vol = cd.volume()
        for e in vol.files(): data = vol.read_entry(e)
        cd.write_wav(cd.track(2), 'track02.wav')

Command line (python3 -I tools/lrvne cdimg ...):
  tracks IMAGE                  the track table
  ls IMAGE                      the files: sector, size, date, path
  extract IMAGE DIR [PATH...]   all files, or the named ones
  cat IMAGE PATH                one file to standard output
  wav IMAGE TRACK OUT.wav       one audio track (44.1 kHz, 16 bit, stereo)
  iso IMAGE OUT.iso             the data track as 2048-byte sectors
"""
import argparse
import io
import os
import re
import struct
import sys
import wave
from dataclasses import dataclass
from typing import BinaryIO, Dict, Iterator, List, Optional, Sequence, Tuple, Union

from ._util import safe_join, write_file

SECTOR_RAW = 2352
SECTOR_DATA = 2048
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'
#: extensions of the files that make up a CD image (the `source` module looks for these)
CD_EXT = ('.iso', '.bin', '.img', '.cue', '.ccd')


class CdImageError(Exception):
    """A CD image, its sheet or its file system cannot be read."""


@dataclass
class Track:
    """A track. `start` (index 1), `pregap` (index 0, None when the sheet has none) and `end`
    (exclusive) are sector numbers in the track's file; `mode` is the sheet's word: 'AUDIO',
    'MODE1/2352', 'MODE1/2048', 'MODE2/2352' ..."""
    number: int
    mode: str
    start: int = 0
    pregap: Optional[int] = None
    end: int = 0
    file: Optional[str] = None      # as the CUE sheet names it; None: the image itself
    backing: int = 0                # index of the opened file

    @property
    def audio(self) -> bool:
        """True for an audio track."""
        return self.mode == 'AUDIO'

    @property
    def sectors(self) -> int:
        """Length of the track from index 1."""
        return max(self.end - self.start, 0)


def msf(sectors: int) -> str:
    """A sector count as minutes:seconds:frames."""
    return '%02d:%02d:%02d' % (sectors // 75 // 60, sectors // 75 % 60, sectors % 75)


def parse_cue(text: str) -> List[Track]:
    """The tracks of a CUE sheet (FILE, TRACK and INDEX 00/01 lines)."""
    tracks: List[Track] = []
    current: Optional[str] = None
    for line in text.splitlines():
        words = line.split()
        if not words:
            continue
        key = words[0].upper()
        try:
            if key == 'FILE':
                m = re.match(r'\s*FILE\s+(?:"([^"]*)"|(\S+))', line, re.I)
                if not m:
                    raise ValueError
                current = m.group(1) if m.group(1) is not None else m.group(2)
            elif key == 'TRACK':
                tracks.append(Track(int(words[1]), words[2].upper(), file=current))
            elif key == 'INDEX' and tracks:
                mm, ss, ff = (int(x) for x in words[2].split(':'))
                lba = (mm * 60 + ss) * 75 + ff
                if int(words[1]) == 0:
                    tracks[-1].pregap = lba
                elif int(words[1]) == 1:
                    tracks[-1].start = lba
        except (ValueError, IndexError):
            raise CdImageError('CUE sheet: cannot read the line %r' % line.strip()) from None
    if not tracks:
        raise CdImageError('CUE sheet: no tracks')
    return tracks


def parse_ccd(text: str) -> Tuple[List[Track], Optional[int]]:
    """The tracks and the lead-out sector of a CloneCD control file.

    [Entry n] sections give Point (the track number; 0xA2 is the lead-out), Control (bit 2:
    data) and PLBA; [TRACK n] sections give MODE and INDEX 0/1.
    """
    entries: List[Dict[str, str]] = []
    per_track: Dict[int, Dict[str, str]] = {}
    cur: Dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        m = re.match(r'\[(.+)\]$', line)
        if m:
            cur = {}
            words = m.group(1).split()
            if words[0].lower() == 'entry':
                entries.append(cur)
            elif words[0].upper() == 'TRACK' and len(words) > 1 and words[1].isdigit():
                per_track[int(words[1])] = cur
            continue
        if '=' in line:
            k, v = line.split('=', 1)
            cur[k.strip()] = v.strip()
    tracks: List[Track] = []
    leadout = None
    try:
        for e in entries:
            point = int(e.get('Point', '0'), 0)
            lba = int(e.get('PLBA', '0'), 0)
            if point == 0xA2:
                leadout = lba
            elif 1 <= point <= 99:
                data = int(e.get('Control', '0'), 0) & 4
                t = Track(point, 'MODE1/2352' if data else 'AUDIO', lba)
                x = per_track.get(point, {})
                if 'INDEX 0' in x:
                    t.pregap = int(x['INDEX 0'], 0)
                if 'INDEX 1' in x:
                    t.start = int(x['INDEX 1'], 0)
                if data and 'MODE' in x:
                    t.mode = 'MODE%s/2352' % x['MODE']
                tracks.append(t)
    except ValueError:
        raise CdImageError('CCD file: a number cannot be read') from None
    if not tracks:
        raise CdImageError('CCD file: no tracks')
    tracks.sort(key=lambda t: t.number)
    return tracks, leadout


@dataclass
class _Backing:
    f: BinaryIO
    size: int
    sector_size: int
    name: str

    @property
    def sectors(self) -> int:
        return self.size // self.sector_size


def _detect_sector_size(f: BinaryIO, size: int, label: str) -> int:
    f.seek(0)
    if f.read(12) == SYNC:
        return SECTOR_RAW
    f.seek(16 * SECTOR_DATA)
    if f.read(6)[1:] == b'CD001' or label.lower().endswith('.iso'):
        return SECTOR_DATA
    raise CdImageError('%s: not a CD image (neither raw sectors nor an ISO 9660 volume)' % label)


def _mode_sector_size(mode: str) -> int:
    return SECTOR_DATA if mode.endswith('/2048') else SECTOR_RAW


class CdImage:
    """A CD image opened by `open_cd`: its tracks and the sectors behind them."""

    def __init__(self, backings: List[_Backing], tracks: List[Track], label: str,
                 sheet: str = 'none', leadout: Optional[int] = None) -> None:
        self._backings = backings
        self.tracks = tracks
        self.label = label
        self.sheet = sheet              # 'cue', 'ccd' or 'none'
        self.leadout = leadout
        for i, t in enumerate(tracks):
            later = [u for u in tracks[i + 1:] if u.backing == t.backing]
            t.end = later[0].start if later else backings[t.backing].sectors

    def __enter__(self) -> 'CdImage':
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def close(self) -> None:
        """Closes the image's files."""
        for b in self._backings:
            b.f.close()

    # ---- sectors ----
    def track(self, number: int) -> Track:
        """The track with this number."""
        for t in self.tracks:
            if t.number == number:
                return t
        raise CdImageError('%s: no track %d' % (self.label, number))

    def data_track(self) -> Track:
        """The first data track."""
        for t in self.tracks:
            if not t.audio:
                return t
        raise CdImageError('%s: no data track' % self.label)

    def sector_size(self, track: Optional[Track] = None) -> int:
        """Bytes per stored sector of a track's file (default: the data track's)."""
        return self._backings[(track or self.data_track()).backing].sector_size

    def raw(self, track: Track, sector: int, count: int = 1) -> bytes:
        """`count` stored sectors of the file of `track`, from sector number `sector` of it."""
        b = self._backings[track.backing]
        b.f.seek(sector * b.sector_size)
        return b.f.read(count * b.sector_size)

    def read_sector(self, lba: int) -> bytes:
        """The 2048 bytes of user data of logical sector `lba` of the data track's file."""
        t = self.data_track()
        ss = self.sector_size(t)
        r = self.raw(t, lba)
        if len(r) < ss:
            raise CdImageError('%s: sector %d is past the end of the image' % (self.label, lba))
        if ss == SECTOR_DATA:
            return r
        if r[:12] != SYNC:
            raise CdImageError('%s: sector %d has no sync pattern (not a data sector)'
                               % (self.label, lba))
        return r[24:24 + SECTOR_DATA] if r[15] == 2 else r[16:16 + SECTOR_DATA]

    def read(self, lba: int, size: int) -> bytes:
        """`size` bytes of user data starting at logical sector `lba`."""
        t = self.data_track()
        if self.sector_size(t) == SECTOR_DATA:
            data = self.raw(t, lba, (size + SECTOR_DATA - 1) // SECTOR_DATA)
            if len(data) < size:
                raise CdImageError('%s: sector %d is past the end of the image'
                                   % (self.label, lba + len(data) // SECTOR_DATA))
            return data[:size]
        out = bytearray()
        while len(out) < size:
            out += self.read_sector(lba)
            lba += 1
        return bytes(out[:size])

    def data_track_end(self) -> int:
        """The sector after the last data sector of the data track (raw images: the last one
        with a sync pattern, which leaves out a pregap of the next track)."""
        t = self.data_track()
        end = t.end
        if self.sector_size(t) == SECTOR_RAW:
            while end > t.start and self.raw(t, end - 1)[:12] != SYNC:
                end -= 1
        return end

    def volume(self) -> 'IsoVolume':
        """The ISO 9660 volume of the data track."""
        return IsoVolume(self)

    # ---- audio ----
    def audio_tracks(self) -> List[Track]:
        """The audio tracks."""
        return [t for t in self.tracks if t.audio]

    def silence_start(self, track: Track, back: int = 300) -> Optional[int]:
        """The first sector of the last run of digital silence that ends within `back` sectors
        before the start of `track`, None when there is none.

        Some sheets place an audio track's start a little late; the music then begins in the
        silence the sheet assigns to the previous track. `write_wav(..., start=...)` with this
        sector repairs that.
        """
        best = None
        s = track.start - 1
        while s >= max(track.start - back, 0):
            if self.raw(track, s).strip(b'\0'):
                if best is not None:
                    return best
            else:
                best = s
            s -= 1
        return best

    def pcm(self, track: Track, start: Optional[int] = None) -> bytes:
        """The samples of an audio track: 44.1 kHz, 16 bit little-endian, stereo."""
        if not track.audio:
            raise CdImageError('%s: track %d is not an audio track' % (self.label, track.number))
        first = track.start if start is None else start
        return self.raw(track, first, max(track.end - first, 0))

    def write_wav(self, track: Track, out: Union[str, BinaryIO],
                  start: Optional[int] = None) -> int:
        """Writes an audio track as a WAV file; returns the number of sectors written."""
        if not track.audio:
            raise CdImageError('%s: track %d is not an audio track' % (self.label, track.number))
        first = track.start if start is None else start
        left = total = max(track.end - first, 0)
        with wave.open(out, 'wb') as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(44100)
            while left:
                n = min(left, 512)
                w.writeframes(self.raw(track, first + total - left, n))
                left -= n
        return total


@dataclass(frozen=True)
class IsoEntry:
    """A file or directory of an ISO 9660 volume. `date` is 'YYYY-MM-DD HH:MM:SS'."""
    path: str
    lba: int
    size: int
    date: str
    is_dir: bool
    extents: Tuple[Tuple[int, int], ...] = ()

    @property
    def name(self) -> str:
        """The last component of the path."""
        return self.path.rsplit('/', 1)[-1]


class IsoVolume:
    """The ISO 9660 file system of a CdImage (primary volume descriptor, plain directories).

    Attributes: `name` (the volume identifier), `system`, and `info` with the other text
    fields of the descriptor. `label` and `fstype` are there so that code which handles
    diskimg.Volume objects can treat this one alike.
    """
    fstype = 'iso9660'

    @property
    def label(self) -> str:
        """The volume identifier."""
        return self.name

    def __init__(self, cd: CdImage, encoding: str = 'cp932') -> None:
        self.cd = cd
        self.encoding = encoding
        d = cd.read_sector(16)
        if d[0] != 1 or d[1:6] != b'CD001':
            raise CdImageError('%s: no ISO 9660 primary volume descriptor at sector 16' % cd.label)

        def text(start: int, end: int) -> str:
            return d[start:end].decode(encoding, 'replace').rstrip(' \0')
        self.system = text(8, 40)
        self.name = text(40, 72)
        self.block_size = struct.unpack_from('<H', d, 128)[0]
        if self.block_size != SECTOR_DATA:
            raise CdImageError('%s: logical blocks of %d bytes are not supported'
                               % (cd.label, self.block_size))
        self.info = {'system': self.system, 'volume': self.name,
                     'blocks': struct.unpack_from('<I', d, 80)[0], 'block_size': self.block_size,
                     'volume_set': text(190, 318), 'publisher': text(318, 446),
                     'preparer': text(446, 574), 'application': text(574, 702),
                     'created': text(813, 829), 'modified': text(830, 846)}
        self._root = (struct.unpack_from('<I', d, 158)[0], struct.unpack_from('<I', d, 166)[0])

    def entries(self) -> Iterator[IsoEntry]:
        """Every directory entry, depth first; a directory comes before what is in it."""
        seen: set = set()
        yield from self._walk(self._root[0], self._root[1], '', seen)

    def _walk(self, lba: int, size: int, prefix: str, seen: set) -> Iterator[IsoEntry]:
        if lba in seen:                         # a damaged volume may loop
            return
        seen.add(lba)
        data = self.cd.read(lba, size)
        p = 0
        pending: List[Tuple[int, int]] = []     # earlier extents of a file in several
        while p < len(data):
            n = data[p]
            if n == 0:                          # records do not cross sectors
                p = (p // SECTOR_DATA + 1) * SECTOR_DATA
                continue
            r = data[p:p + n]
            p += n
            if len(r) < 34:
                raise CdImageError('%s: damaged directory at sector %d' % (self.cd.label, lba))
            e_lba = struct.unpack_from('<I', r, 2)[0] + r[1]    # + extended attribute blocks
            e_size = struct.unpack_from('<I', r, 10)[0]
            flags = r[25]
            raw_name = r[33:33 + r[32]]
            if raw_name in (b'\0', b'\1'):
                continue
            name = raw_name.decode(self.encoding, 'replace')
            is_dir = bool(flags & 2)
            if not is_dir:
                name = name.split(';')[0]
                if name.endswith('.'):
                    name = name[:-1]
            if flags & 0x80:                    # more extents follow
                pending.append((e_lba, e_size))
                continue
            extents = tuple(pending) + ((e_lba, e_size),)
            pending = []
            y, mo, dd, hh, mi, ss = r[18:24]
            date = '%04d-%02d-%02d %02d:%02d:%02d' % (1900 + y, mo, dd, hh, mi, ss)
            entry = IsoEntry(prefix + name, extents[0][0], sum(s for _, s in extents), date,
                             is_dir, extents)
            yield entry
            if is_dir:
                yield from self._walk(e_lba, e_size, prefix + name + '/', seen)

    def files(self) -> Iterator[IsoEntry]:
        """Every file (no directories)."""
        return (e for e in self.entries() if not e.is_dir)

    def read_entry(self, entry: IsoEntry) -> bytes:
        """The contents of a file."""
        return b''.join(self.cd.read(lba, size) for lba, size in entry.extents)

    def read(self, path: str) -> bytes:
        """The contents of the file at `path` (ASCII case ignored)."""
        want = path.replace('\\', '/').strip('/').upper()
        for e in self.files():
            if e.path.upper() == want:
                return self.read_entry(e)
        raise CdImageError('%s: no file %s' % (self.cd.label, path))


# ---- opening -----------------------------------------------------------------------------------

def _sibling(path: str, name: str) -> Optional[str]:
    """The file called `name` next to `path`, its case ignored; None when there is none."""
    folder = os.path.dirname(path)
    cand = os.path.join(folder, os.path.basename(name.replace('\\', '/')))
    if os.path.isfile(cand):
        return cand
    want = os.path.basename(cand).lower()
    try:
        for f in sorted(os.listdir(folder or '.')):
            if f.lower() == want and os.path.isfile(os.path.join(folder, f)):
                return os.path.join(folder, f)
    except OSError:
        pass
    return None


def _read_text(path: str) -> str:
    with open(path, encoding='latin-1') as f:
        return f.read()


def _open_backing(path: str, sector_size: Optional[int]) -> _Backing:
    try:
        f = open(path, 'rb')
    except OSError as e:
        raise CdImageError('%s: %s' % (path, e.strerror or e)) from e
    size = os.path.getsize(path)
    try:
        return _Backing(f, size, sector_size or _detect_sector_size(f, size, path), path)
    except CdImageError:
        f.close()
        raise


def _open_cue(cue: str, tracks: List[Track]) -> CdImage:
    backings: List[_Backing] = []
    index: Dict[str, int] = {}
    try:
        for t in tracks:
            name = t.file or os.path.splitext(os.path.basename(cue))[0] + '.bin'
            if name not in index:
                path = _sibling(cue, name)
                if path is None:
                    raise CdImageError('%s: the file %s it names is not next to it' % (cue, name))
                index[name] = len(backings)
                backings.append(_open_backing(path, _mode_sector_size(t.mode)))
            t.backing = index[name]
    except CdImageError:
        for b in backings:
            b.f.close()
        raise
    return CdImage(backings, tracks, cue, 'cue')


def open_cd(src: Union[str, bytes, bytearray, BinaryIO], label: Optional[str] = None,
            sheet: Optional[str] = None) -> CdImage:
    """Opens a CD image.

    `src` is the path of the image or of its CUE/CCD sheet (the other is looked up next to it,
    by the same base name, case ignored), or the image itself as bytes or as a seekable binary
    file. `sheet` is the text of a CUE sheet for an image given as bytes or as a file. Close
    the result, or use it in a `with` statement.
    """
    if not isinstance(src, str):
        f: BinaryIO = io.BytesIO(bytes(src)) if isinstance(src, (bytes, bytearray)) else src
        label = label or '<bytes>'
        size = f.seek(0, io.SEEK_END)
        tracks = parse_cue(sheet) if sheet else None
        ss = _mode_sector_size(tracks[0].mode) if tracks else _detect_sector_size(f, size, label)
        if tracks is None:
            tracks = [Track(1, 'MODE1/%d' % ss)]
        return CdImage([_Backing(f, size, ss, label)], tracks, label, 'cue' if sheet else 'none')
    ext = os.path.splitext(src)[1].lower()
    base = os.path.splitext(os.path.basename(src))[0]
    if ext == '.cue':
        return _open_cue(src, parse_cue(_read_text(src)))
    if ext == '.ccd':
        image = _sibling(src, base + '.img') or _sibling(src, base + '.bin')
        if image is None:
            raise CdImageError('%s: no %s.img next to it' % (src, base))
        tracks, leadout = parse_ccd(_read_text(src))
        return CdImage([_open_backing(image, SECTOR_RAW)], tracks, image, 'ccd', leadout)
    ccd = _sibling(src, base + '.ccd')
    if ccd:
        tracks, leadout = parse_ccd(_read_text(ccd))
        return CdImage([_open_backing(src, SECTOR_RAW)], tracks, src, 'ccd', leadout)
    cue = _sibling(src, base + '.cue')
    if cue:
        tracks = parse_cue(_read_text(cue))
        names = {t.file for t in tracks}
        if len(names) > 1:
            return _open_cue(cue, tracks)
        return CdImage([_open_backing(src, _mode_sector_size(tracks[0].mode))], tracks, src,
                       'cue')
    b = _open_backing(src, None)
    return CdImage([b], [Track(1, 'MODE1/%d' % b.sector_size)], src)


def is_cd_image(head: bytes, probe: bytes = b'') -> bool:
    """True when a file is a CD data image: `head` is its first 12 bytes or more, `probe` the
    6 bytes at offset 32768 (16 sectors of 2048 bytes)."""
    return head[:12] == SYNC or probe[1:6] == b'CD001'


# ---- command line ------------------------------------------------------------------------------

def _cmd_tracks(cd: CdImage, a: argparse.Namespace) -> int:
    print('%s: track table from %s%s' % (cd.label, 'the .%s sheet' % cd.sheet if cd.sheet != 'none'
                                         else 'the image alone',
                                         ', lead-out %d' % cd.leadout if cd.leadout is not None
                                         else ''))
    print('track\tmode\tpregap\tstart\tstart_msf\tsectors\tlength\tseconds')
    for t in cd.tracks:
        print('%d\t%s\t%s\t%d\t%s\t%d\t%s\t%.2f' % (
            t.number, t.mode, '' if t.pregap is None else t.pregap, t.start, msf(t.start),
            t.sectors, msf(t.sectors), t.sectors / 75))
    return 0


def _cmd_ls(cd: CdImage, a: argparse.Namespace) -> int:
    vol = cd.volume()
    for k in ('system', 'volume', 'publisher', 'preparer', 'application', 'created', 'modified'):
        print('; %s: %s' % (k, vol.info[k]))
    count = total = 0
    for e in vol.entries():
        print('%7d %9s  %s  %s%s' % (e.lba, '' if e.is_dir else e.size, e.date, e.path,
                                     '/' if e.is_dir else ''))
        if not e.is_dir:
            count += 1
            total += e.size
    print('; %d files, %d bytes' % (count, total))
    return 0


def _cmd_extract(cd: CdImage, a: argparse.Namespace) -> int:
    vol = cd.volume()
    wanted = {w.strip('/').upper() for w in a.paths}
    count = 0
    for e in vol.files():
        if wanted and e.path.upper() not in wanted:
            continue
        write_file(safe_join(a.dir, e.path), vol.read_entry(e))
        count += 1
    print('%d files -> %s' % (count, a.dir))
    return 0 if count or not wanted else 1


def _cmd_cat(cd: CdImage, a: argparse.Namespace) -> int:
    sys.stdout.buffer.write(cd.volume().read(a.path))
    return 0


def _cmd_wav(cd: CdImage, a: argparse.Namespace) -> int:
    t = cd.track(a.track)
    start = cd.silence_start(t) if a.snap_silence else None
    n = cd.write_wav(t, a.out, start)
    print('track %d: %d sectors, %.2f s -> %s' % (t.number, n, n / 75, a.out))
    return 0


def _cmd_iso(cd: CdImage, a: argparse.Namespace) -> int:
    t = cd.data_track()
    end = cd.data_track_end()
    with open(a.out, 'wb') as out:
        for lba in range(t.start, end):
            out.write(cd.read_sector(lba))
    print('%s: %d sectors' % (a.out, end - t.start))
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    """The command line (see the module's description)."""
    ap = argparse.ArgumentParser(prog='lrvne cdimg', description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)

    def command(name: str, func: object, text: str) -> argparse.ArgumentParser:
        p = sub.add_parser(name, help=text)
        p.add_argument('image', help='the image or its .cue / .ccd sheet')
        p.set_defaults(func=func)
        return p
    command('tracks', _cmd_tracks, 'the track table')
    command('ls', _cmd_ls, 'the files of the ISO 9660 volume')
    p = command('extract', _cmd_extract, 'write all files, or the named ones, below DIR')
    p.add_argument('dir')
    p.add_argument('paths', nargs='*')
    p = command('cat', _cmd_cat, 'one file to standard output')
    p.add_argument('path')
    p = command('wav', _cmd_wav, 'one audio track as a WAV file')
    p.add_argument('track', type=int)
    p.add_argument('out')
    p.add_argument('--snap-silence', action='store_true',
                   help='start at the digital silence just before the sheet\'s position')
    p = command('iso', _cmd_iso, 'the data track as 2048-byte sectors')
    p.add_argument('out')
    a = ap.parse_args(argv)
    try:
        with open_cd(a.image) as cd:
            return a.func(cd, a)
    except (CdImageError, ValueError, OSError) as e:
        print('cdimg: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
