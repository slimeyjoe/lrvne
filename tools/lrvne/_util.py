# SPDX-License-Identifier: GPL-3.0-or-later
"""Small helpers shared by the lrvne tools: little-endian reads, digests, safe output paths."""
import hashlib
import os
import struct
from typing import List


def u16(data: bytes, offset: int) -> int:
    """The little-endian 16-bit number at `offset`."""
    return struct.unpack_from('<H', data, offset)[0]


def u32(data: bytes, offset: int) -> int:
    """The little-endian 32-bit number at `offset`."""
    return struct.unpack_from('<I', data, offset)[0]


def md5_hex(data: bytes) -> str:
    """The md5 of `data` in hexadecimal: the digest the inventories identify files by."""
    return hashlib.md5(data).hexdigest()


def md5_file(path: str) -> str:
    """The md5 of a file, read in pieces."""
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def path_parts(path: str) -> List[str]:
    """The components of a path from an image or an archive that are safe to create.

    Both separators are understood; empty components, '.', '..' and drive prefixes are dropped
    and characters a host file system cannot take are replaced, so the result never leaves the
    directory it is joined to.
    """
    parts = []
    for part in path.replace('\\', '/').split('/'):
        part = part.replace('\0', '_').replace(':', '_').strip()
        if part in ('', '.', '..'):
            continue
        parts.append(part)
    return parts


def safe_join(root: str, path: str) -> str:
    """Joins a path taken from an image, an archive or an inventory to `root`.

    Raises ValueError when nothing usable is left of the path or when an existing symbolic link
    would lead the result out of `root`.
    """
    parts = path_parts(path)
    if not parts:
        raise ValueError('unusable path %r' % path)
    dest = os.path.join(root, *parts)
    real_root = os.path.realpath(root)
    real = os.path.realpath(dest)
    if real != real_root and not real.startswith(real_root.rstrip(os.sep) + os.sep):
        raise ValueError('%r leads out of %s' % (path, root))
    return dest


def write_file(dest: str, data: bytes) -> None:
    """Writes `data` to `dest`, creating the directories above it."""
    os.makedirs(os.path.dirname(dest) or '.', exist_ok=True)
    with open(dest, 'wb') as f:
        f.write(data)
