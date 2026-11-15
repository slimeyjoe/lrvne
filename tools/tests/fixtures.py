# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic media for the tools' tests: FAT volumes, disk image containers, ISO 9660 images.

Everything is generated here from made-up contents; no file of any game or release is used.
The builders write the formats independently of the readers under test, from the formats'
descriptions, so a test that reads back what they made checks the reader and not a mirror
image of it.
"""
import struct
from typing import Dict, List, Optional, Tuple

DOS_DATE = ((1993 - 1980) << 9) | (5 << 5) | 28         # 1993-05-28
DOS_TIME = (12 << 11) | (34 << 5)                       # 12:34
DOS_STAMP = '1993-05-28 12:34'


def pattern(size: int, seed: int) -> bytes:
    """`size` bytes that depend on `seed`: recognisable, not compressible to nothing."""
    return bytes((seed * 31 + i * 7 + (i >> 8) * 13) & 0xFF for i in range(size))


# ---- FAT ---------------------------------------------------------------------------------------

def _name83(name: str) -> bytes:
    stem, _, ext = name.partition('.')
    return stem.encode('cp932').ljust(8)[:8] + ext.encode('cp932').ljust(3)[:3]


def _dirent(name: bytes, attr: int, cluster: int, size: int) -> bytes:
    return struct.pack('<11sB10xHHHI', name, attr, DOS_TIME, DOS_DATE, cluster, size)


def make_fat(files: Dict[str, bytes], total_sectors: int, sector_size: int = 1024,
             cluster_sectors: int = 1, root_entries: int = 192, label: Optional[str] = None,
             sectors_per_track: int = 8, heads: int = 2, scatter: bool = True) -> bytes:
    """A FAT12 or FAT16 volume (which one follows from the cluster count, as on a real disk)
    holding `files` ({'DIR/NAME.EXT': data}); one level of directories or more.

    With `scatter` the clusters of a file are not contiguous: files take the free clusters
    alternately from the low and the high end, so chains must be followed through the FAT.
    """
    bps = sector_size
    root_sectors = (root_entries * 32 + bps - 1) // bps
    spf = 1
    while True:
        clusters = (total_sectors - 1 - 2 * spf - root_sectors) // cluster_sectors
        fat16 = clusters >= 4085
        need = ((clusters + 2) * 2 if fat16 else ((clusters + 2) * 3 + 1) // 2)
        if need <= spf * bps:
            break
        spf += 1
    csize = bps * cluster_sectors
    fat = [0] * (clusters + 2)
    fat[0], fat[1] = (0xFFFE, 0xFFFF) if fat16 else (0xFFE, 0xFFF)
    eoc = 0xFFFF if fat16 else 0xFFF
    free = list(range(2, clusters + 2))
    data = bytearray(clusters * csize)

    def store(blob: bytes) -> int:
        count = max((len(blob) + csize - 1) // csize, 1)
        if count > len(free):
            raise ValueError('fixture volume too small')
        chain = []
        for i in range(count):
            chain.append(free.pop(-1 if scatter and i % 2 else 0))
        for i, c in enumerate(chain):
            fat[c] = chain[i + 1] if i + 1 < len(chain) else eoc
            piece = blob[i * csize:(i + 1) * csize]
            data[(c - 2) * csize:(c - 2) * csize + len(piece)] = piece
        return chain[0]

    # directory tree: {'': {name: bytes or dict}}
    tree: Dict[str, object] = {}
    for path, blob in files.items():
        node = tree
        parts = path.split('/')
        for p in parts[:-1]:
            node = node.setdefault(p, {})       # type: ignore[assignment]
        node[parts[-1]] = blob                  # type: ignore[index]

    def build_dir(node: Dict[str, object], own: int, parent: int, is_root: bool) -> bytes:
        out = bytearray()
        if is_root:
            if label:
                out += _dirent(label.encode('cp932').ljust(11)[:11], 0x08, 0, 0)
            # things a reader must skip: a long-name part and a deleted entry
            out += _dirent(b'Aa\0b\0c\0d\0e\0', 0x0F, 0, 0)
            out += _dirent(b'\xe5ONE    TXT', 0x20, 0, 5)
        else:
            out += _dirent(b'.          ', 0x10, own, 0)
            out += _dirent(b'..         ', 0x10, parent, 0)
        for name, item in node.items():
            if isinstance(item, dict):
                first = store(b'\0' * csize)    # reserve the directory's cluster
                body = build_dir(item, first, own, False)
                if len(body) > csize:
                    raise ValueError('fixture directory too large')
                data[(first - 2) * csize:(first - 2) * csize + len(body)] = body
                out += _dirent(_name83(name), 0x10, first, 0)
            else:
                blob = item                     # type: ignore[assignment]
                out += _dirent(_name83(name), 0x20, store(blob) if blob else 0, len(blob))
        return bytes(out)

    root = build_dir(tree, 0, 0, True)          # type: ignore[arg-type]
    if len(root) > root_entries * 32:
        raise ValueError('fixture root directory too large')
    boot = bytearray(bps)
    boot[0:3] = b'\xeb\x3c\x90'
    boot[3:11] = b'LRVNETST'
    struct.pack_into('<HBHBHHBHHH', boot, 11, bps, cluster_sectors, 1, 2, root_entries,
                     total_sectors if total_sectors < 0x10000 else 0, 0xFE, spf,
                     sectors_per_track, heads)
    if total_sectors >= 0x10000:
        struct.pack_into('<I', boot, 32, total_sectors)
    table = bytearray(spf * bps)
    if fat16:
        struct.pack_into('<%dH' % len(fat), table, 0, *fat)
    else:
        for i, v in enumerate(fat):
            o = i * 3 // 2
            if i & 1:
                table[o] |= (v << 4) & 0xF0
                table[o + 1] = v >> 4
            else:
                table[o] = v & 0xFF
                table[o + 1] |= v >> 8
    out = bytes(boot) + bytes(table) * 2 + root.ljust(root_sectors * bps, b'\0') + bytes(data)
    return out.ljust(total_sectors * bps, b'\0')[:total_sectors * bps]


def make_floppy(files: Dict[str, bytes], label: Optional[str] = 'TESTDISK',
                geometry: Tuple[int, int, int, int] = (77, 2, 8, 1024)) -> bytes:
    """A raw 2HD floppy image (default 77 x 2 x 8 x 1024 = 1 261 568 bytes) with a FAT12
    volume."""
    cyls, heads, spt, ss = geometry
    return make_fat(files, cyls * heads * spt, ss, 1, 192, label, spt, heads)


# ---- containers --------------------------------------------------------------------------------

def d88_wrap(raw: bytes, geometry: Tuple[int, int, int, int] = (77, 2, 8, 1024),
             name: bytes = b'TEST', table_entries: int = 164) -> bytes:
    """A D88 image of a raw sector image. The sectors of each track are stored in reverse
    order, as an interleaved dump would have them out of order."""
    cyls, heads, spt, ss = geometry
    n = {128: 0, 256: 1, 512: 2, 1024: 3}[ss]
    header_size = 0x20 + 4 * table_entries
    offsets = []
    body = bytearray()
    for t in range(cyls * heads):
        offsets.append(header_size + len(body))
        for r in range(spt, 0, -1):
            at = (t * spt + r - 1) * ss
            body += struct.pack('<BBBBHBBB5xH', t // heads, t % heads, r, n, spt, 0, 0, 0, ss)
            body += raw[at:at + ss]
    offsets += [0] * (table_entries - len(offsets))
    head = bytearray(header_size)
    head[0:len(name)] = name
    head[0x1B] = 0x20                           # 2HD
    struct.pack_into('<I', head, 0x1C, header_size + len(body))
    struct.pack_into('<%dI' % table_entries, head, 0x20, *offsets)
    return bytes(head + body)


def anex_wrap(raw: bytes, geometry: Tuple[int, int, int, int], media: int = 0x90) -> bytes:
    """An Anex86 FDI/HDI image: a 4096-byte header, then the sectors."""
    cyls, heads, spt, ss = geometry
    head = struct.pack('<8I', 0, media, 4096, len(raw), ss, spt, heads, cyls)
    return head.ljust(4096, b'\0') + raw


def nhd_wrap(raw: bytes, geometry: Tuple[int, int, int, int]) -> bytes:
    """A T98-Next NHD image: a 512-byte header, then the sectors."""
    cyls, heads, spt, ss = geometry
    head = bytearray(512)
    head[0:15] = b'T98HDDIMAGE.R0\0'
    head[0x10:0x10 + 7] = b'fixture'
    struct.pack_into('<IIHHH', head, 0x110, 512, cyls, heads, spt, ss)
    return bytes(head) + raw


def make_hard_disk(files: Dict[str, bytes], geometry: Tuple[int, int, int, int] = (80, 4, 17, 512),
                   name: str = 'TEST PART') -> bytes:
    """The sectors of a hard disk with a PC-98 partition table (physical sector 1) and one
    partition from cylinder 1 to the last cylinder, holding a FAT16 volume."""
    cyls, heads, spt, ss = geometry
    per_cyl = heads * spt
    volume = make_fat(files, (cyls - 1) * per_cyl, ss, 1, 224, 'HARDDISK', spt, heads)
    disk = bytearray(per_cyl * ss) + volume
    entry = bytearray(32)
    entry[0], entry[1] = 0xA1, 0xA1             # bootable, active, a DOS kind
    struct.pack_into('<BBH', entry, 4, 0, 0, 1)             # IPL: sector 0, head 0, cylinder 1
    struct.pack_into('<BBH', entry, 8, 0, 0, 1)             # start
    struct.pack_into('<BBH', entry, 12, spt - 1, heads - 1, cyls - 1)   # end
    entry[16:32] = name.encode('cp932').ljust(16)
    disk[ss:ss + 32] = entry
    return bytes(disk)


# ---- ISO 9660 ----------------------------------------------------------------------------------

SECTOR = 2048
RAW_SECTOR = 2352
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'
ISO_DATE = bytes((95, 7, 16, 10, 20, 30, 0))    # 1995-07-16 10:20:30
ISO_STAMP = '1995-07-16 10:20:30'


def _both16(v: int) -> bytes:
    return struct.pack('<H', v) + struct.pack('>H', v)


def _both32(v: int) -> bytes:
    return struct.pack('<I', v) + struct.pack('>I', v)


def _record(name: bytes, lba: int, size: int, is_dir: bool) -> bytes:
    body = _both32(lba) + _both32(size) + ISO_DATE + bytes((2 if is_dir else 0, 0, 0)) + \
        _both16(1) + bytes((len(name),)) + name
    if len(body) % 2:                           # a record has an even length
        body += b'\0'
    return bytes((len(body) + 2, 0)) + body


def make_iso(files: Dict[str, bytes], volume: str = 'TESTVOL', system: str = 'TESTSYS') -> bytes:
    """An ISO 9660 image (2048-byte sectors) holding `files` ({'DIR/NAME.EXT': data}), with
    the versions (";1") a real disc has. Each directory takes one sector."""
    tree: Dict[str, object] = {}
    for path, blob in files.items():
        node = tree
        parts = path.split('/')
        for p in parts[:-1]:
            node = node.setdefault(p, {})       # type: ignore[assignment]
        node[parts[-1]] = blob                  # type: ignore[index]
    sectors: List[bytes] = [b'\0' * SECTOR] * 18      # 16 system sectors, 2 descriptors
    dirs: List[Tuple[Dict[str, object], int, int]] = []     # node, own lba, parent lba

    def reserve(node: Dict[str, object], parent: int) -> int:
        own = len(sectors)
        sectors.append(b'')
        dirs.append((node, own, parent))
        for item in node.values():
            if isinstance(item, dict):
                item['\0lba'] = reserve(item, own)           # type: ignore[index]
        return own
    root_lba = reserve(tree, 18)
    for node, own, parent in dirs:
        out = _record(b'\0', own, SECTOR, True) + _record(b'\1', parent, SECTOR, True)
        for name, item in sorted((k, v) for k, v in node.items() if k != '\0lba'):
            if isinstance(item, dict):
                out += _record(name.encode('ascii'), item['\0lba'], SECTOR, True)  # type: ignore
            else:
                blob: bytes = item              # type: ignore[assignment]
                lba = len(sectors)
                for i in range(0, len(blob), SECTOR):
                    sectors.append(blob[i:i + SECTOR].ljust(SECTOR, b'\0'))
                stored = name if '.' in name else name + '.'
                out += _record((stored + ';1').encode('ascii'), lba, len(blob), False)
        if len(out) > SECTOR:
            raise ValueError('fixture directory too large')
        sectors[own] = out.ljust(SECTOR, b'\0')
    pvd = bytearray(SECTOR)
    pvd[0:7] = b'\x01CD001\x01'
    pvd[8:40] = system.encode('ascii').ljust(32)
    pvd[40:72] = volume.encode('ascii').ljust(32)
    pvd[80:88] = _both32(len(sectors))
    pvd[120:124] = _both16(1)
    pvd[124:128] = _both16(1)
    pvd[128:132] = _both16(SECTOR)
    pvd[156:190] = _record(b'\0', root_lba, SECTOR, True)
    pvd[190:702] = b' ' * 512
    pvd[318:318 + 9] = b'PUBLISHER'
    pvd[813:829] = b'1995071610203000'
    pvd[881] = 1
    term = bytearray(SECTOR)
    term[0:7] = b'\xffCD001\x01'
    sectors[16], sectors[17] = bytes(pvd), bytes(term)
    return b''.join(sectors)


def raw_wrap(iso: bytes, mode: int = 1) -> bytes:
    """An ISO image as raw 2352-byte sectors of mode 1 (or mode 2 form 1)."""
    out = bytearray()
    for i in range(0, len(iso), SECTOR):
        lba = i // SECTOR + 150
        head = SYNC + bytes((lba // 75 // 60, lba // 75 % 60, lba % 75, mode))
        if mode == 2:
            sector = head + b'\0' * 8 + iso[i:i + SECTOR] + b'\0' * 280
        else:
            sector = head + iso[i:i + SECTOR] + b'\0' * 288
        assert len(sector) == RAW_SECTOR
        out += sector
    return bytes(out)


def audio_sectors(count: int, seed: int = 1) -> bytes:
    """`count` sectors of 16-bit stereo samples that are not silence."""
    return bytes(((i * seed) % 251) + 1 for i in range(count * RAW_SECTOR))


def cue_sheet(bin_name: str, tracks: List[Tuple[str, int]]) -> str:
    """A CUE sheet for one file: `tracks` are (mode, start sector)."""
    lines = ['FILE "%s" BINARY' % bin_name]
    for i, (mode, start) in enumerate(tracks, 1):
        lines.append('  TRACK %02d %s' % (i, mode))
        lines.append('    INDEX 01 %02d:%02d:%02d'
                     % (start // 75 // 60, start // 75 % 60, start % 75))
    return '\n'.join(lines) + '\n'
