# SPDX-License-Identifier: GPL-3.0-or-later
"""One way to open whatever a player has: directories, archives, disk images, CD images.

    with source.open_source('/path/to/anything') as src:
        for entry in src.entries():
            if entry.name.upper() == 'WANTED.DAT':
                data = entry.read()

A source is given as a path, and is any of
  - a directory, searched recursively (an installed copy, an emulator's host drive, a folder
    of images and archives);
  - a zip archive (read with the standard library);
  - a 7z, rar, lzh, cab or tar archive: unpacked into a temporary directory by an external
    `bsdtar` or `7z` (or `unrar`), whichever is on the PATH; the directory is removed when the
    source is closed;
  - a disk image (the `diskimg` module: D88, FDI, HDI, NHD, raw) with the file systems of the
    registry passed as `filesystems=`;
  - a CD image (the `cdimg` module: .iso, raw .bin/.img, or its .cue/.ccd sheet);
  - `CONTAINER::member`: one member of an archive, e.g. `disks.zip::game/disk1.d88`.
Containers nest: an archive in a directory, an image in an archive in an archive. What is
*inside* a disk or CD image is not opened further.

Every file found is an `Entry`: its logical path (`disks.zip::disk1.d88::DATA/FILE.DAT`, the
containers joined with `::`), the file name, where it came from, and `read()`. Nothing is read
before `read()` is called, so an importer can match names first. A container that cannot be
opened is reported through `Source.skipped` and offered as a plain file instead.

Nothing from a source is ever written under a name the source chooses, except inside the
private temporary directory of an external unpacker, and symbolic links found there are not
followed.

The player's media location is never built in: sources are arguments, and `default_roots`
falls back to the directories named in the LRVNE_MEDIA environment variable.

Command line (python3 -I tools/lrvne source ...):
  ls [--md5] [SRC...]        every file a source holds
  extract SRC DIR            write every file below DIR (the containers become directories)
"""
import argparse
import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from dataclasses import dataclass, field
from typing import Callable, Iterator, List, Mapping, Optional, Sequence, Tuple

from . import cdimg, diskimg
from ._util import md5_hex, safe_join, write_file

MEDIA_ENV = 'LRVNE_MEDIA'
SEP = '::'
ARCHIVE_EXT = ('.7z', '.rar', '.lzh', '.lha', '.cab', '.tar', '.tgz', '.tar.gz', '.tar.bz2',
               '.tar.xz')
_SPOOL_LIMIT = 32 << 20         # larger archive members are unpacked to a file, not to memory


class SourceError(Exception):
    """A source cannot be opened."""


@dataclass(frozen=True)
class Entry:
    """A file found in a source.

    `path` is the logical path below the source ('/' inside a container, '::' between
    containers), `origin` names the innermost container for messages, `kind` says what that
    container is ('dir', 'zip', 'archive', 'iso9660', or a file system name such as 'fat').
    `volume` is the diskimg.Volume or cdimg.IsoVolume the file is on (None elsewhere): it gives
    an importer the volume label and the image itself.
    """
    path: str
    origin: str
    kind: str
    size: Optional[int] = None
    mtime: str = ''
    volume: Optional[object] = field(default=None, compare=False, repr=False)
    reader: Optional[Callable[[], bytes]] = field(default=None, compare=False, repr=False)

    @property
    def name(self) -> str:
        """The file name: the last component of the path."""
        return self.path.rsplit(SEP, 1)[-1].rsplit('/', 1)[-1]

    @property
    def inner_path(self) -> str:
        """The path inside the innermost container."""
        return self.path.rsplit(SEP, 1)[-1]

    @property
    def volume_label(self) -> str:
        """The label of the volume the file is on, '' elsewhere."""
        return getattr(self.volume, 'label', '') or ''

    def read(self) -> bytes:
        """The file's contents (the source must still be open)."""
        if self.reader is None:
            raise SourceError('%s: no data' % self.path)
        return self.reader()


# ---- external unpackers ------------------------------------------------------------------------

def archive_tools(rar: bool = False) -> List[Tuple[str, str]]:
    """The external unpackers on the PATH as (kind, executable), best first."""
    found = []
    for kind, names in (('bsdtar', ('bsdtar',)), ('7z', ('7z', '7zz', '7za', '7zr'))):
        for n in names:
            exe = shutil.which(n)
            if exe:
                found.append((kind, exe))
                break
    if rar and shutil.which('unrar'):
        found.append(('unrar', shutil.which('unrar') or 'unrar'))
    return found


def extract_archive(path: str, dest: str,
                    tools: Optional[Sequence[Tuple[str, str]]] = None) -> None:
    """Unpacks an archive into the directory `dest` with the first external tool that manages.

    SourceError when no tool is installed or none can read the archive.
    """
    rar = path.lower().endswith('.rar')
    tools = list(tools) if tools is not None else archive_tools(rar)
    if not tools:
        raise SourceError('%s: reading this kind of archive needs bsdtar (libarchive) or 7z%s on '
                          'the PATH; install one, or unpack the archive and name the directory'
                          % (path, ' or unrar' if rar else ''))
    errors = []
    for kind, exe in tools:
        if kind == 'bsdtar':
            cmd = [exe, '-x', '-f', path, '-C', dest]
        elif kind == 'unrar':
            cmd = [exe, 'x', '-y', '-inul', path, dest + os.sep]
        else:
            cmd = [exe, 'x', '-y', '-bd', '-o' + dest, '--', path]
        try:
            r = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
        except OSError as e:
            errors.append('%s: %s' % (os.path.basename(exe), e.strerror or e))
            continue
        if r.returncode == 0:
            return
        tail = r.stdout.decode('utf-8', 'replace').strip().splitlines()[-1:] or ['failed']
        errors.append('%s: %s' % (os.path.basename(exe), tail[0]))
    raise SourceError('%s: cannot be unpacked (%s)' % (path, '; '.join(errors)))


# ---- files of a container, before they are classified ------------------------------------------

class _File:
    """A file somewhere in a source: on disk, or a member of an open zip."""

    def __init__(self, name: str, path: Optional[str] = None,
                 member: Optional[Tuple[zipfile.ZipFile, zipfile.ZipInfo]] = None) -> None:
        self.name = name
        self.path = path
        self.member = member

    @property
    def size(self) -> int:
        if self.member:
            return self.member[1].file_size
        return os.path.getsize(self.path or '')

    def read(self) -> bytes:
        if self.member:
            return self.member[0].read(self.member[1])
        with open(self.path or '', 'rb') as f:
            return f.read()

    def mtime(self) -> str:
        if self.member:
            return '%04d-%02d-%02d %02d:%02d' % self.member[1].date_time[:5]
        return time.strftime('%Y-%m-%d %H:%M', time.localtime(os.path.getmtime(self.path or '')))

    def on_disk(self, tmp: Callable[[], str]) -> str:
        """The path of the file, a zip member being unpacked under a name of ours first."""
        if self.path is None and self.member:
            ext = os.path.splitext(self.name)[1]
            fd, path = tempfile.mkstemp(suffix=ext if ext.isascii() else '', dir=tmp())
            with os.fdopen(fd, 'wb') as out, self.member[0].open(self.member[1]) as src:
                shutil.copyfileobj(src, out)
            self.path, self.member = path, None
        return self.path or ''

    def peek(self, tmp: Callable[[], str]) -> Tuple[bytes, bytes]:
        """The first 16 bytes and the 6 bytes at 32768 (what tells a CD image)."""
        if self.member and self.size <= _SPOOL_LIMIT:
            data = self.read()
            return data[:16], data[32768:32774]
        with open(self.on_disk(tmp), 'rb') as f:
            head = f.read(16)
            f.seek(32768)
            return head, f.read(6)


def _ext(name: str) -> str:
    low = name.lower()
    for e in ARCHIVE_EXT:
        if '.' in e[1:] and low.endswith(e):
            return e
    return os.path.splitext(low)[1]


class Source:
    """A source opened by `open_source`. Use it in a `with` statement, or call `close`.

    `skipped` lists (label, reason) of the containers that could not be opened; `on_skip`,
    when given, is called with the same two strings as it happens.
    """

    def __init__(self, spec: str, filesystems: Optional[diskimg.Filesystems] = None,
                 on_skip: Optional[Callable[[str, str], None]] = None,
                 tools: Optional[Sequence[Tuple[str, str]]] = None) -> None:
        self.spec = spec
        self.filesystems = filesystems or diskimg.default_filesystems()
        self.on_skip = on_skip
        self.tools = tools
        self.skipped: List[Tuple[str, str]] = []
        self._stack = contextlib.ExitStack()
        self._tmpdir: Optional[str] = None
        self._path, self._members = split_spec(spec)

    def __enter__(self) -> 'Source':
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def close(self) -> None:
        """Closes the archives and images and removes the temporary directory."""
        self._stack.close()
        self._tmpdir = None

    def _tmp(self) -> str:
        if self._tmpdir is None:
            self._tmpdir = self._stack.enter_context(
                tempfile.TemporaryDirectory(prefix='lrvne-source-'))
        return self._tmpdir

    def _new_dir(self) -> str:
        return tempfile.mkdtemp(dir=self._tmp())

    def _skip(self, label: str, reason: object) -> None:
        self.skipped.append((label, str(reason)))
        if self.on_skip:
            self.on_skip(label, str(reason))

    # ---- walking ----
    def entries(self) -> Iterator[Entry]:
        """Every file of the source. May be called again: the source is walked anew."""
        if os.path.isdir(self._path) and not self._members:
            yield from self._scan_dir(self._path, self.spec, '', True)
            return
        f = _File(os.path.basename(self._path), path=self._path)
        label = self._path
        for member in self._members:
            f = self._member(f, label, member)
            label += SEP + member
        yield from self._scan_file(f, label, '', top=True)

    def _member(self, f: _File, label: str, member: str) -> _File:
        """The member of the archive `f` named `member` (exactly, else with the case ignored)."""
        want = member.replace('\\', '/').strip('/')
        ext = _ext(f.name)
        if ext == '.zip' or (ext not in ARCHIVE_EXT and self._is_zip(f)):
            z = self._open_zip(f)
            infos = [i for i in z.infolist() if not i.is_dir()]
            for match in (lambda n: n == want, lambda n: n.lower() == want.lower()):
                for i in infos:
                    if match(i.filename.replace('\\', '/').strip('/')):
                        return _File(i.filename, member=(z, i))
            raise SourceError('%s: no member %s' % (label, member))
        if ext in ARCHIVE_EXT:
            root = self._unpack(f)
            for dirpath, name, rel in self._walk(root, False):
                if rel == want or rel.lower() == want.lower():
                    return _File(name, path=os.path.join(dirpath, name))
            raise SourceError('%s: no member %s' % (label, member))
        raise SourceError('%s: not an archive, so it has no member %s' % (label, member))

    def _is_zip(self, f: _File) -> bool:
        if f.path:
            return zipfile.is_zipfile(f.path)
        return f.size <= _SPOOL_LIMIT and f.read()[:4] == b'PK\x03\x04'

    def _open_zip(self, f: _File) -> zipfile.ZipFile:
        if f.path is None and f.size <= _SPOOL_LIMIT:
            return self._stack.enter_context(zipfile.ZipFile(io.BytesIO(f.read())))
        return self._stack.enter_context(zipfile.ZipFile(f.on_disk(self._tmp)))

    def _unpack(self, f: _File) -> str:
        dest = self._new_dir()
        extract_archive(f.on_disk(self._tmp), dest, self.tools)
        return dest

    @staticmethod
    def _walk(root: str, trusted: bool) -> Iterator[Tuple[str, str, str]]:
        """(directory, file name, relative path with '/') of the regular files below `root`.

        Directory links are never followed. In a directory an unpacker filled (`trusted`
        false) file links are skipped as well: they could point anywhere on the host.
        """
        for dirpath, dirs, files in os.walk(root, followlinks=False):
            dirs.sort()
            for name in sorted(files):
                full = os.path.join(dirpath, name)
                if not os.path.isfile(full) or (not trusted and os.path.islink(full)):
                    continue
                yield dirpath, name, os.path.relpath(full, root).replace(os.sep, '/')

    def _scan_dir(self, root: str, label: str, prefix: str, trusted: bool,
                  kind: str = 'dir') -> Iterator[Entry]:
        joint = '/' if kind == 'dir' else SEP
        for dirpath, name, rel in self._walk(root, trusted):
            f = _File(name, path=os.path.join(dirpath, name))
            yield from self._scan_file(f, label + joint + rel, prefix + rel, label, kind)

    def _scan_zip(self, z: zipfile.ZipFile, label: str, prefix: str) -> Iterator[Entry]:
        for info in sorted(z.infolist(), key=lambda i: i.filename):
            if info.is_dir():
                continue
            rel = info.filename.replace('\\', '/').lstrip('/')
            f = _File(rel.rsplit('/', 1)[-1], member=(z, info))
            yield from self._scan_file(f, label + SEP + rel, prefix + rel, label, 'zip')

    def _scan_file(self, f: _File, label: str, path: str, origin: str = '',
                   kind: str = 'dir', top: bool = False) -> Iterator[Entry]:
        """The entries of one file: those of the container it is, or the file itself."""
        inner = path + SEP if path else ''
        ext = _ext(f.name)
        try:
            if ext == '.zip':
                yield from self._scan_zip(self._open_zip(f), label, inner)
                return
            if ext in ARCHIVE_EXT:
                yield from self._scan_dir(self._unpack(f), label, inner, False, 'archive')
                return
            if ext in ('.cue', '.ccd') and top and f.path:
                yield from self._scan_cd(cdimg.open_cd(f.path), label, inner)
                return
            if ext == '.iso' or (ext in ('.bin', '.img') and cdimg.is_cd_image(*f.peek(self._tmp))):
                if f.path is None and f.size <= _SPOOL_LIMIT:
                    cd = cdimg.open_cd(f.read(), label)
                else:
                    cd = cdimg.open_cd(f.on_disk(self._tmp))
                    cd.label = label
                yield from self._scan_cd(cd, label, inner)
                return
            if ext in diskimg.IMAGE_EXT or ext in diskimg.FLUX_EXT:
                yield from self._scan_disk(f, label, inner)
                return
        except (SourceError, diskimg.DiskImageError, cdimg.CdImageError, zipfile.BadZipFile,
                OSError) as e:
            self._skip(label, e)
        if top and not path:
            path = f.name
        yield Entry(path, origin or label, kind, f.size, f.mtime(), None, f.read)

    def _scan_disk(self, f: _File, label: str, prefix: str) -> Iterator[Entry]:
        images = diskimg.open_images(f.read(), label)
        found = [(img, self.filesystems.volumes(img)) for img in images]
        for img, vols in found:
            for i, vol in enumerate(vols):
                where = prefix if len(vols) == 1 else '%svolume%d/' % (prefix, i + 1)
                if len(images) > 1:
                    where += 'disk%s/' % img.label.rsplit('#', 1)[-1]
                for e in vol.files():
                    yield Entry(where + e.path, vol.origin or img.label, vol.fstype or 'disk',
                                e.size, e.mtime, vol, e.read)

    def _scan_cd(self, cd: cdimg.CdImage, label: str, prefix: str) -> Iterator[Entry]:
        self._stack.callback(cd.close)
        vol = cd.volume()
        for e in vol.files():
            yield Entry(prefix + e.path, label, vol.fstype, e.size, e.date[:16], vol,
                        lambda e=e: vol.read_entry(e))


def split_spec(spec: str) -> Tuple[str, List[str]]:
    """Splits `CONTAINER::member::member` into the path that exists and the member names.

    A path that exists as it is written is taken whole, whatever it contains.
    """
    if os.path.exists(spec):
        return spec, []
    parts = spec.split(SEP)
    for n in range(len(parts) - 1, 0, -1):
        head = SEP.join(parts[:n])
        if os.path.isfile(head):
            return head, parts[n:]
    raise SourceError('%s does not exist' % spec)


def open_source(spec: str, filesystems: Optional[diskimg.Filesystems] = None,
                on_skip: Optional[Callable[[str, str], None]] = None,
                tools: Optional[Sequence[Tuple[str, str]]] = None) -> Source:
    """Opens a source (see the module's description).

    `filesystems` is the registry of file systems to look for on disk images (default: the
    built-in ones); `tools` replaces the external unpackers found on the PATH, as
    (kind, executable) pairs with kind 'bsdtar', '7z' or 'unrar'.
    """
    return Source(spec, filesystems, on_skip, tools)


def default_roots(specs: Sequence[str], environ: Optional[Mapping[str, str]] = None) -> List[str]:
    """The sources to use: the ones named, else the directories of LRVNE_MEDIA.

    A named source that does not exist and is a relative path is also looked for below the
    LRVNE_MEDIA directories. SourceError when nothing is named and the variable is not set.
    """
    env = os.environ if environ is None else environ
    roots = [r for r in env.get(MEDIA_ENV, '').split(os.pathsep) if r]
    if not specs:
        if not roots:
            raise SourceError('no source named: give the files or directories of your copy, or '
                              'set %s to the directory they are in' % MEDIA_ENV)
        return roots
    out = []
    for spec in specs:
        head = spec.split(SEP)[0]
        if not os.path.exists(spec) and not os.path.exists(head) and not os.path.isabs(spec):
            for r in roots:
                if os.path.exists(os.path.join(r, head)):
                    spec = os.path.join(r, spec)
                    break
        out.append(spec)
    return out


# ---- command line ------------------------------------------------------------------------------

def _warn(label: str, reason: str) -> None:
    print('source: skipped %s: %s' % (label, reason), file=sys.stderr)


def _cmd_ls(a: argparse.Namespace) -> int:
    for spec in default_roots(a.src):
        with open_source(spec, on_skip=_warn) as src:
            for e in src.entries():
                digest = md5_hex(e.read()) + '  ' if a.md5 else ''
                size = '' if e.size is None else e.size
                print('%9s  %-16s  %s%s' % (size, e.mtime, digest, e.path))
    return 0


def _cmd_extract(a: argparse.Namespace) -> int:
    count = 0
    with open_source(a.src, on_skip=_warn) as src:
        for e in src.entries():
            write_file(safe_join(a.dir, e.path.replace(SEP, '/')), e.read())
            count += 1
    print('%d files -> %s' % (count, a.dir))
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    """The command line (see the module's description)."""
    ap = argparse.ArgumentParser(prog='lrvne source', description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('ls', help='every file a source holds')
    p.add_argument('src', nargs='*', help='default: the directories of %s' % MEDIA_ENV)
    p.add_argument('--md5', action='store_true', help='read every file and print its md5')
    p.set_defaults(func=_cmd_ls)
    p = sub.add_parser('extract', help='write every file below DIR')
    p.add_argument('src')
    p.add_argument('dir')
    p.set_defaults(func=_cmd_extract)
    a = ap.parse_args(argv)
    try:
        return a.func(a)
    except (SourceError, ValueError, OSError) as e:
        print('source: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
