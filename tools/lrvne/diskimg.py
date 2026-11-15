# SPDX-License-Identifier: GPL-3.0-or-later
"""Floppy and hard disk images and the file systems on them (read only).

Two layers:

  image layer   `open_image(path_or_bytes) -> Image`: the plain sector data of
                  D88         floppy images (.d88 .d98 .88d; several disks in one file: open_images)
                  Anex86      FDI floppy and HDI hard disk images (.fdi .hdi)
                  T98-Next    NHD hard disk images (.nhd)
                  raw         the sectors in order (.hdm .xdf .dup .img .tfd ...); the geometry
                              is told from the size (RAW_GEOMETRIES)
                and, for hard disks, the PC-98 partition table (`Image.partitions`).
  file systems  `volumes(image) -> [Volume]`: FAT12 and FAT16 (`FatVolume`) are built in. An
                engine whose media carry another file system registers a probe:

                    fs = diskimg.default_filesystems()
                    fs.register('myfs', my_probe)       # my_probe(image) -> [Volume] or []
                    for volume in fs.volumes(image): ...

                The registry is an object the caller owns and passes on (the `source` and
                `inventory` modules take it as `filesystems=`); nothing is registered globally.

Flux images (SCP, MFI, HFE) are not sector images and are refused: convert them to D88.

Command line (python3 -I tools/lrvne diskimg ...):
  info IMAGE              container, geometry, partitions, volumes
  ls IMAGE [--offset N]   every file: size, date, md5, path
  extract IMAGE DIR       write every file below DIR
  raw IMAGE OUT           write the plain sector image
"""
import argparse
import os
import struct
import sys
from dataclasses import dataclass, field
from typing import Callable, Dict, Iterator, List, Optional, Sequence, Tuple, Union

from ._util import md5_hex, safe_join, u16, u32, write_file

D88_EXT = ('.d88', '.d98', '.88d')
ANEX_EXT = ('.fdi', '.hdi')
RAW_EXT = ('.hdm', '.xdf', '.dup', '.img', '.tfd')
FLUX_EXT = ('.scp', '.mfi', '.hfe')
#: every extension `open_image` is meant for (the `source` module looks for these)
IMAGE_EXT = D88_EXT + ANEX_EXT + ('.nhd',) + RAW_EXT

#: raw floppy images, by size: (cylinders, heads, sectors per track, sector size)
RAW_GEOMETRIES = {
    1261568: (77, 2, 8, 1024),      # 2HD, the PC-98 standard format
    1419264: (77, 2, 9, 1024),      # 2HD with nine sectors
    1228800: (80, 2, 15, 512),      # 2HC
    1474560: (80, 2, 18, 512),      # 1.44 MB
    737280: (80, 2, 9, 512),        # 2DD, 720 KB
    655360: (80, 2, 8, 512),        # 2DD, 640 KB
}

_D88_MEDIA = (0x00, 0x10, 0x20, 0x30, 0x40)
_D88_MIN = 0x2A0                    # header with a table of 160 tracks
_NHD_MAGIC = b'T98HDDIMAGE.R0\0'
_FLUX_MAGIC = (b'SCP', b'MFI2', b'MESSFLOPPYIMAGE', b'HXCPICFE', b'HXCHFEV3')


class DiskImageError(Exception):
    """A disk image or a file system on it cannot be read."""


# ---- image layer -------------------------------------------------------------------------------

@dataclass(frozen=True)
class Geometry:
    """The shape of a disk."""
    cylinders: int
    heads: int
    sectors: int            # per track
    sector_size: int

    @property
    def size(self) -> int:
        """Bytes of a disk of this shape."""
        return self.cylinders * self.heads * self.sectors * self.sector_size


@dataclass(frozen=True)
class Partition:
    """One entry of the PC-98 partition table."""
    index: int
    mid: int                # boot indicator and system kind
    sid: int                # bit 7: active; the rest: file system kind
    name: str
    start: Tuple[int, int, int]     # cylinder, head, sector
    end: Tuple[int, int, int]
    offset: int             # of the partition in Image.data, bytes
    size: int               # bytes, to the end of the last cylinder


@dataclass(frozen=True)
class D88Sector:
    """A sector of a D88 image with its header fields."""
    track: int              # index in the track table (cylinder * 2 + head on 2-sided disks)
    c: int
    h: int
    r: int
    n: int
    density: int
    deleted: int
    status: int
    data: bytes


@dataclass
class Image:
    """A disk image opened: the sectors as one byte string, in disk order.

    `kind` is 'd88', 'fdi', 'hdi', 'nhd' or 'raw'. `geometry` is None when the container does
    not state it and the size is not a standard one. `sectors` holds the sector headers of a
    D88 image (others: empty); `info` what the container's header says.
    """
    kind: str
    label: str
    data: bytes
    geometry: Optional[Geometry] = None
    info: Dict[str, object] = field(default_factory=dict)
    sectors: List[D88Sector] = field(default_factory=list)

    @property
    def hard_disk(self) -> bool:
        """True for the hard disk containers (which have a partition table)."""
        return self.kind in ('hdi', 'nhd')

    def read(self, offset: int, size: int) -> bytes:
        """`size` bytes at `offset`; DiskImageError beyond the end of the image."""
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise DiskImageError('%s: read of %d bytes at %d is outside the image (%d bytes)'
                                 % (self.label, size, offset, len(self.data)))
        return self.data[offset:offset + size]

    def sector(self, lba: int, count: int = 1) -> bytes:
        """`count` sectors from logical sector `lba` (the geometry must be known)."""
        if self.geometry is None:
            raise DiskImageError('%s: the sector size is not known' % self.label)
        ss = self.geometry.sector_size
        return self.read(lba * ss, count * ss)

    def chs(self, cylinder: int, head: int, sector: int) -> bytes:
        """The sector at a cylinder, head and sector number (sectors count from 1)."""
        g = self.geometry
        if g is None:
            raise DiskImageError('%s: the geometry is not known' % self.label)
        if not (0 <= cylinder < g.cylinders and 0 <= head < g.heads and 1 <= sector <= g.sectors):
            raise DiskImageError('%s: no sector %d/%d/%d' % (self.label, cylinder, head, sector))
        return self.sector((cylinder * g.heads + head) * g.sectors + sector - 1)

    def bad_sectors(self) -> List[D88Sector]:
        """The D88 sectors recorded with an error status (0x10, deleted data, is not one)."""
        return [s for s in self.sectors if s.status not in (0x00, 0x10)]

    def partitions(self) -> List[Partition]:
        """The entries of the PC-98 partition table, [] when the image cannot have one.

        The table is physical sector 1: 32-byte entries, the start and end as cylinder, head
        and sector in the image's geometry (sectors count from 0 there).
        """
        g = self.geometry
        if g is None or self.kind in ('d88', 'raw') or len(self.data) < g.sector_size + 512:
            return []
        table = self.data[g.sector_size:g.sector_size + 512]
        per_cyl = g.heads * g.sectors
        out = []
        for i in range(len(table) // 32):
            e = table[32 * i:32 * i + 32]
            if e[0] == 0:                   # also the all-zero entries
                continue
            start = (u16(e, 10), e[9], e[8])
            end = (u16(e, 14), e[13], e[12])
            offset = (start[0] * per_cyl + start[1] * g.sectors + start[2]) * g.sector_size
            stop = min((end[0] + 1) * per_cyl * g.sector_size, len(self.data))
            out.append(Partition(i, e[0], e[1], e[16:32].decode('cp932', 'replace').rstrip(' \0'),
                                 start, end, offset, max(stop - offset, 0)))
        return out


def _d88_spans(data: bytes) -> List[Tuple[int, int]]:
    """(offset, size) of each disk in a D88 file, [] when `data` is not one."""
    spans, pos = [], 0
    while pos < len(data):
        if len(data) - pos < _D88_MIN:
            return []
        size = u32(data, pos + 0x1C)
        if data[pos + 0x1B] not in _D88_MEDIA or not _D88_MIN <= size <= len(data) - pos:
            return []
        first = u32(data, pos + 0x20)
        if first and not _D88_MIN <= first < size:
            return []
        spans.append((pos, size))
        pos += size
    return spans


def is_d88(data: bytes) -> bool:
    """True when `data` is a D88 file: the size fields of its disks add up to its length."""
    return bool(_d88_spans(data))


def _d88_image(d: bytes, label: str) -> Image:
    """One disk of a D88 file (`d` is exactly that disk) as an Image."""
    offsets = [u32(d, 0x20 + 4 * i) for i in range(164)]
    used = [o for o in offsets if o]
    if used:                                # a table of 160 entries ends where the data begins
        offsets = offsets[:min(164, (min(used) - 0x20) // 4)]
    sectors: List[D88Sector] = []
    out = bytearray()
    shapes: Dict[Tuple[int, int], int] = {}
    tracks = 0
    for ti, off in enumerate(offsets):
        if not off:
            continue
        p, count, track = off, None, []
        while count is None or len(track) < count:
            if p + 16 > len(d):
                raise DiskImageError('%s: D88 track %d runs past the end of the file' % (label, ti))
            c, h, r, n, count, density, deleted, status, size = struct.unpack_from(
                '<BBBBHBBB5xH', d, p)
            if count == 0:                  # an unformatted track
                break
            if p + 16 + size > len(d):
                raise DiskImageError('%s: D88 track %d runs past the end of the file' % (label, ti))
            track.append(D88Sector(ti, c, h, r, n, density, deleted, status,
                                   d[p + 16:p + 16 + size]))
            p += 16 + size
        if not track:
            continue
        tracks += 1
        track.sort(key=lambda s: s.r)       # tracks are often stored interleaved
        shape = (len(track), len(track[0].data))
        shapes[shape] = shapes.get(shape, 0) + 1
        for s in track:
            out += s.data
        sectors += track
    geometry = None
    if len(shapes) == 1:
        (per_track, ss), = shapes
        heads = 2 if any(s.h for s in sectors) else 1
        if tracks % heads == 0 and ss:
            geometry = Geometry(tracks // heads, heads, per_track, ss)
    info = {'name': d[0:17].split(b'\0')[0].decode('cp932', 'replace'),
            'write_protect': d[0x1A], 'media': d[0x1B], 'tracks': tracks}
    return Image('d88', label, bytes(out), geometry, info, sectors)


def _anex_image(data: bytes, label: str, kind: Optional[str]) -> Image:
    """An Anex86 FDI or HDI: a header of 4096 bytes (it says so itself), then the sectors."""
    _, media, hsize, dsize, ss, spt, heads, cyls = struct.unpack_from('<8I', data, 0)
    if not 32 <= hsize < len(data) or ss not in (256, 512, 1024, 2048) or not spt or not heads:
        raise DiskImageError('%s: not an Anex86 image (implausible header)' % label)
    body = data[hsize:hsize + dsize] if dsize else data[hsize:]
    if kind is None:
        kind = 'hdi' if len(body) > 4 << 20 else 'fdi'
    return Image(kind, label, body, Geometry(cyls, heads, spt, ss),
                 {'media': media, 'header_size': hsize, 'data_size': dsize})


def _nhd_image(data: bytes, label: str) -> Image:
    """A T98-Next NHD: "T98HDDIMAGE.R0", a comment, then the geometry at 0x110."""
    hsize, cyls = u32(data, 0x110), u32(data, 0x114)
    heads, spt, ss = u16(data, 0x118), u16(data, 0x11A), u16(data, 0x11C)
    if not 0x120 <= hsize < len(data) or not ss or not spt or not heads:
        raise DiskImageError('%s: damaged NHD header' % label)
    comment = data[0x10:0x110].split(b'\0')[0].decode('cp932', 'replace')
    return Image('nhd', label, data[hsize:], Geometry(cyls, heads, spt, ss),
                 {'header_size': hsize, 'comment': comment})


def _looks_anex(data: bytes) -> bool:
    if len(data) <= 4096:
        return False
    hsize, dsize, ss = u32(data, 8), u32(data, 12), u32(data, 16)
    return hsize >= 32 and hsize + dsize == len(data) and ss in (256, 512, 1024, 2048)


def open_images(src: Union[str, bytes, bytearray], label: Optional[str] = None,
                kind: Optional[str] = None) -> List[Image]:
    """Opens an image file and returns every disk in it (more than one only for D88).

    `src` is a path or the file's bytes; `label` names it in messages and supplies the file
    name extension when `src` is bytes. `kind` ('d88', 'fdi', 'hdi', 'nhd', 'raw') overrides
    the detection, which goes by content first and by extension where content cannot tell.
    """
    if isinstance(src, (bytes, bytearray)):
        data = bytes(src)
        label = label or '<bytes>'
    else:
        label = label or src
        try:
            with open(src, 'rb') as f:
                data = f.read()
        except OSError as e:
            raise DiskImageError('%s: %s' % (label, e.strerror or e)) from e
    ext = os.path.splitext(label)[1].lower()
    if kind is None and (ext in FLUX_EXT or data.startswith(_FLUX_MAGIC)):
        raise DiskImageError('%s: flux images (SCP, MFI, HFE) are not read; convert the disk to '
                             'a sector image, D88 for preference' % label)
    if not data:
        raise DiskImageError('%s: empty file' % label)
    if kind == 'd88' or (kind is None and (is_d88(data) or ext in D88_EXT)):
        spans = _d88_spans(data)
        if not spans:
            raise DiskImageError('%s: not a D88 image (its size field disagrees with the file)'
                                 % label)
        return [_d88_image(data[o:o + n], label if len(spans) == 1 else '%s#%d' % (label, i + 1))
                for i, (o, n) in enumerate(spans)]
    if kind == 'nhd' or (kind is None and data.startswith(_NHD_MAGIC)):
        if len(data) < 0x120:
            raise DiskImageError('%s: too short for an NHD image' % label)
        return [_nhd_image(data, label)]
    if kind in ('fdi', 'hdi') or (kind is None and (ext in ANEX_EXT or _looks_anex(data))):
        if len(data) < 32:
            raise DiskImageError('%s: too short for an Anex86 image' % label)
        return [_anex_image(data, label, kind or (ext[1:] if ext in ANEX_EXT else None))]
    if kind not in (None, 'raw'):
        raise DiskImageError('unknown image kind %r' % kind)
    shape = RAW_GEOMETRIES.get(len(data))
    return [Image('raw', label, data, Geometry(*shape) if shape else None)]


def open_image(src: Union[str, bytes, bytearray], label: Optional[str] = None,
               kind: Optional[str] = None) -> Image:
    """Opens an image file (see open_images) and returns its first, normally only, disk."""
    return open_images(src, label, kind)[0]


# ---- file system layer -------------------------------------------------------------------------

@dataclass(frozen=True)
class FileEntry:
    """A file of a volume. `path` uses '/'; `mtime` is 'YYYY-MM-DD HH:MM' or ''."""
    path: str
    size: int
    mtime: str = ''
    attr: int = 0
    reader: Optional[Callable[[], bytes]] = field(default=None, compare=False, repr=False)

    @property
    def name(self) -> str:
        """The last component of the path."""
        return self.path.rsplit('/', 1)[-1]

    def read(self) -> bytes:
        """The file's contents."""
        if self.reader is None:
            raise DiskImageError('%s: no data' % self.path)
        return self.reader()


class Volume:
    """A file system found on an image; what a registered probe returns.

    Attributes: `fstype` (a short name: 'fat'), `label` (the volume label, '' if none),
    `origin` (where it was found, for messages), `image` (the Image, None for a volume opened
    from bytes).
    """
    fstype = ''
    label = ''
    origin = ''
    image: Optional[Image] = None

    def files(self) -> Iterator[FileEntry]:
        """Every file of the volume, directories walked."""
        raise NotImplementedError

    def read(self, path: str) -> bytes:
        """The contents of the file at `path` (ASCII case ignored)."""
        want = path.replace('\\', '/').strip('/').upper()
        for e in self.files():
            if e.path.upper() == want:
                return e.read()
        raise DiskImageError('%s: no file %s' % (self.origin or self.fstype, path))


@dataclass(frozen=True)
class FatLayout:
    """Where the parts of a FAT12/16 volume are (byte offsets in the image data)."""
    sector_size: int
    cluster_sectors: int
    fat: int
    root: int
    root_entries: int
    data: int
    clusters: int
    fat16: bool
    end: int

    @property
    def cluster_size(self) -> int:
        """Bytes per cluster."""
        return self.sector_size * self.cluster_sectors


def fat_layout(data: bytes, base: int = 0) -> Optional[FatLayout]:
    """The layout of the FAT volume whose boot sector is at `base`, None without a plausible
    BIOS parameter block there."""
    if base < 0 or base + 64 > len(data):
        return None
    b = data[base:base + 64]
    bps, spc, res, nfats, nroot, tot16, media, spf = struct.unpack_from('<HBHBHHBH', b, 11)
    total = tot16 or u32(b, 32)
    if bps not in (256, 512, 1024, 2048) or spc == 0 or spc & (spc - 1) or res < 1:
        return None
    if nfats not in (1, 2) or nroot == 0 or media < 0xF0 or spf == 0 or total == 0:
        return None
    fat = base + res * bps
    root = fat + nfats * spf * bps
    first = root + (nroot * 32 + bps - 1) // bps * bps
    clusters = (total * bps - (first - base)) // (spc * bps)
    if clusters <= 0 or first > len(data):
        return None
    return FatLayout(bps, spc, fat, root, nroot, first, clusters, clusters >= 4085,
                     base + total * bps)


def _dos_time(date: int, time: int) -> str:
    if not date:
        return ''
    return '%04d-%02d-%02d %02d:%02d' % (1980 + (date >> 9), date >> 5 & 15, date & 31,
                                         time >> 11, time >> 5 & 63)


class FatVolume(Volume):
    """A FAT12 or FAT16 volume.

    Names are the 8.3 names as stored (upper case), decoded as Shift-JIS; long-name entries,
    deleted entries and the volume label are skipped. Cluster chains are followed through the
    first FAT; a chain that loops or leaves the volume ends there, so a damaged disk gives
    short files instead of an endless walk.
    """
    fstype = 'fat'

    def __init__(self, data: bytes, base: int = 0, origin: str = '',
                 image: Optional[Image] = None, encoding: str = 'cp932') -> None:
        layout = fat_layout(data, base)
        if layout is None:
            raise DiskImageError('%s: no FAT file system at byte %d' % (origin or 'image', base))
        self.data = data
        self.base = base
        self.layout = layout
        self.origin = origin
        self.image = image
        self.encoding = encoding
        self.label = ''
        for e in self._raw_entries(self._root()):
            if e[11] != 0x0F and e[11] & 0x08:
                self.label = e[0:11].decode(encoding, 'replace').rstrip()
                break

    def _root(self) -> bytes:
        g = self.layout
        return self.data[g.root:g.root + g.root_entries * 32]

    @staticmethod
    def _raw_entries(raw: bytes) -> Iterator[bytes]:
        for i in range(0, len(raw) - 31, 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                return
            if e[0] != 0xE5:
                yield e

    def next_cluster(self, cluster: int) -> Optional[int]:
        """The cluster after `cluster` in its chain, None at the end of the chain."""
        g = self.layout
        if g.fat + 2 * cluster + 2 > len(self.data):
            return None
        if g.fat16:
            v = u16(self.data, g.fat + 2 * cluster)
            return None if v >= 0xFFF8 else v
        v = u16(self.data, g.fat + cluster * 3 // 2)
        v = v >> 4 if cluster & 1 else v & 0xFFF
        return None if v >= 0xFF8 else v

    def chain(self, cluster: int) -> List[int]:
        """The clusters of the chain that starts at `cluster`."""
        out: List[int] = []
        seen = set()
        cur: Optional[int] = cluster
        while cur is not None and 2 <= cur < self.layout.clusters + 2 and cur not in seen:
            seen.add(cur)
            out.append(cur)
            cur = self.next_cluster(cur)
        return out

    def read_chain(self, cluster: int, size: Optional[int] = None) -> bytes:
        """The data of a cluster chain, cut to `size` bytes when given."""
        g = self.layout
        csize = g.cluster_size
        parts = []
        for c in self.chain(cluster):
            o = g.data + (c - 2) * csize
            parts.append(self.data[o:o + csize])
            if size is not None and len(parts) * csize >= size:
                break
        data = b''.join(parts)
        return data if size is None else data[:size]

    def files(self) -> Iterator[FileEntry]:
        return self._walk(self._root(), '', set())

    def _walk(self, raw: bytes, prefix: str, seen: set) -> Iterator[FileEntry]:
        for e in self._raw_entries(raw):
            attr = e[11]
            if attr == 0x0F or attr & 0x08:         # long-name part, volume label
                continue
            stem = e[0:8].rstrip(b' ')
            if stem[:1] == b'\x05':                 # stands for a first byte of 0xE5
                stem = b'\xe5' + stem[1:]
            ext = e[8:11].rstrip(b' ')
            name = (stem + (b'.' + ext if ext else b'')).decode(self.encoding, 'replace')
            cluster, size = u16(e, 26), u32(e, 28)
            if attr & 0x10:
                # a damaged directory may point back up the tree
                if name in ('.', '..') or cluster < 2 or cluster in seen:
                    continue
                seen.add(cluster)
                yield from self._walk(self.read_chain(cluster), prefix + name + '/', seen)
            else:
                yield FileEntry(prefix + name, size, _dos_time(u16(e, 24), u16(e, 22)), attr,
                                self._reader(cluster, size))

    def _reader(self, cluster: int, size: int) -> Callable[[], bytes]:
        return lambda: self.read_chain(cluster, size) if size else b''


Probe = Callable[[Image], Sequence[Volume]]


def fat_probe(image: Image) -> List[Volume]:
    """The FAT volumes of an image: those of its partitions, else one at its first sector."""
    offsets = [p.offset for p in image.partitions() if fat_layout(image.data, p.offset)]
    out: List[Volume] = []
    for o in offsets or [0]:
        if fat_layout(image.data, o):
            origin = '%s (partition at byte %d)' % (image.label, o) if o else image.label
            out.append(FatVolume(image.data, o, origin, image))
    return out


class Filesystems:
    """The file systems to look for on an image: an ordered list of named probes.

    A probe takes an Image and returns the volumes it recognises ([] or None: not mine; a
    DiskImageError it raises counts as that too). The first probe that returns volumes wins;
    probes registered later are asked first, so an engine's own format is tried before FAT.
    """

    def __init__(self, probes: Sequence[Tuple[str, Probe]] = ()) -> None:
        self._probes: List[Tuple[str, Probe]] = list(probes)

    def register(self, name: str, probe: Probe, first: bool = True) -> None:
        """Adds a probe under a name; `first=False` asks it after the ones already there."""
        if any(n == name for n, _ in self._probes):
            raise DiskImageError('file system %r is registered already' % name)
        self._probes.insert(0 if first else len(self._probes), (name, probe))

    def names(self) -> List[str]:
        """The names of the probes in the order they are asked."""
        return [n for n, _ in self._probes]

    def volumes(self, image: Image) -> List[Volume]:
        """The volumes of an image; DiskImageError when no probe recognises it."""
        for _name, probe in self._probes:
            try:
                found = probe(image)
            except (DiskImageError, struct.error, IndexError):
                continue
            if found:
                return list(found)
        raise DiskImageError('%s: no file system found (looked for: %s)'
                             % (image.label, ', '.join(self.names()) or 'nothing'))


def default_filesystems() -> Filesystems:
    """A new registry that knows the built-in file systems (FAT12/16)."""
    return Filesystems([('fat', fat_probe)])


def volumes(image: Image, filesystems: Optional[Filesystems] = None) -> List[Volume]:
    """The volumes of an image, looked for with `filesystems` (default: the built-in ones)."""
    return (filesystems or default_filesystems()).volumes(image)


# ---- command line ------------------------------------------------------------------------------

def _cmd_info(a: argparse.Namespace) -> int:
    for img in open_images(a.image):
        g = img.geometry
        print('%s: %s, %d bytes of sectors' % (img.label, img.kind, len(img.data)))
        if g:
            print('  geometry: %d cylinders, %d heads, %d sectors of %d bytes'
                  % (g.cylinders, g.heads, g.sectors, g.sector_size))
        for k, v in img.info.items():
            print('  %s: %s' % (k, v))
        if img.bad_sectors():
            print('  sectors with an error status: %d' % len(img.bad_sectors()))
        for p in img.partitions():
            print('  partition %d: mid %02X sid %02X, %d/%d/%d to %d/%d/%d, byte %d, %r'
                  % ((p.index, p.mid, p.sid) + p.start + p.end + (p.offset, p.name)))
        try:
            for v in volumes(img):
                extra = ''
                if isinstance(v, FatVolume):
                    lay = v.layout
                    extra = ', FAT%d, %d clusters of %d bytes' % (16 if lay.fat16 else 12,
                                                                 lay.clusters, lay.cluster_size)
                print('  volume %r (%s%s)' % (v.label, v.fstype, extra))
        except DiskImageError as e:
            print('  %s' % e)
    return 0


def _volumes_of(a: argparse.Namespace) -> List[Volume]:
    out: List[Volume] = []
    for img in open_images(a.image):
        if getattr(a, 'offset', None) is not None:
            out.append(FatVolume(img.data, a.offset, '%s@%d' % (img.label, a.offset), img))
        else:
            out += volumes(img)
    return out


def _cmd_ls(a: argparse.Namespace) -> int:
    for v in _volumes_of(a):
        print('# %s: volume %r (%s)' % (v.origin, v.label, v.fstype))
        for e in v.files():
            data = e.read()
            print('%8d  %-16s  %s  %s' % (len(data), e.mtime, md5_hex(data), e.path))
    return 0


def _cmd_extract(a: argparse.Namespace) -> int:
    vols = _volumes_of(a)
    count = 0
    for i, v in enumerate(vols):
        root = a.dir if len(vols) == 1 else os.path.join(a.dir, 'volume%d' % (i + 1))
        for e in v.files():
            write_file(safe_join(root, e.path), e.read())
            count += 1
    print('%d files -> %s' % (count, a.dir))
    return 0


def _cmd_raw(a: argparse.Namespace) -> int:
    images = open_images(a.image)
    if len(images) != 1:
        raise DiskImageError('%s holds %d disks' % (a.image, len(images)))
    write_file(a.out, images[0].data)
    print('%s: %d bytes' % (a.out, len(images[0].data)))
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    """The command line (see the module's description)."""
    ap = argparse.ArgumentParser(prog='lrvne diskimg', description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('info', help='container, geometry, partitions, volumes')
    p.add_argument('image')
    p.set_defaults(func=_cmd_info)
    p = sub.add_parser('ls', help='every file: size, date, md5, path')
    p.add_argument('image')
    p.add_argument('--offset', type=int, help='read a FAT volume at this byte of the sectors')
    p.set_defaults(func=_cmd_ls)
    p = sub.add_parser('extract', help='write every file below DIR')
    p.add_argument('image')
    p.add_argument('dir')
    p.add_argument('--offset', type=int, help='read a FAT volume at this byte of the sectors')
    p.set_defaults(func=_cmd_extract)
    p = sub.add_parser('raw', help='write the plain sector image')
    p.add_argument('image')
    p.add_argument('out')
    p.set_defaults(func=_cmd_raw)
    a = ap.parse_args(argv)
    try:
        return a.func(a)
    except (DiskImageError, ValueError, OSError) as e:
        print('diskimg: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
