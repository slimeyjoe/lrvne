# SPDX-License-Identifier: GPL-3.0-or-later
"""The asset pack ("XPAK", include/lrvne/pack.h): writer, reader, bundling into an executable.

Layout, version 2 (little-endian):
  0x00  4   "XPAK"
  0x04  u16 format version (2)
  0x06  u16 entry count
  0x08  u32 total pack size (bytes, including this header)
  0x0C  4   reserved (0)
  0x10  entries, 32 bytes each: name[20] (NUL padded), u8 type, u8 lang, u16 reserved (0),
        u32 offset, u32 size
  data, each entry starting on a 4-byte boundary (padded with zeros), offsets from the pack's
  start.
A pack appended to an executable is followed by a 16-byte trailer:
  "XPKT", u32 pack size, u32 ~pack size, "XPKT"

What this writer guarantees beyond the layout:
  - the table is sorted by (lang, name, type): lang numerically, the name as its stored bytes
    with a-z upper-cased first, compared bytewise;
  - names are stored in Shift-JIS (cp932: ASCII is itself). Names of engine types (>= 32) and
    of voice clips are a game's file names and have a-z upper-cased; names of the other types
    are stored as given (the layer's own are lower case: "profile", "langs", "font16");
  - (name, type, lang) is unique, ASCII case ignored;
  - languages are tags ("ja", "en"), given ids 1..15 in the order `langs=` lists them and
    then in the order of first use; the entry "langs" (type META, neutral) with the lines
    "<id>=<tag>" is written whenever the pack has a language. Lang 0 is neutral.

Types below 32 mean the same in every pack (TYPE_NAMES); an engine numbers its own from 32
and names them in a TypeRegistry so listings can show them:

    types = xpak.TypeRegistry({32: 'picture', 33: 'script'})
    w = xpak.PackWriter(types)
    w.add('profile', xpak.T_META, profile.dumps({...}).encode())
    w.add('title.pic', 'picture', data)                 # stored as TITLE.PIC
    w.add('text', xpak.T_STRINGS, table, lang='en')
    w.write('game.pak')

    pack = xpak.read_pack(open('game.pak', 'rb').read())
    pack.find('TITLE.PIC').data

Command line (python3 -I tools/lrvne xpak ...; --types 32=picture,33=script names engine types):
  list FILE                     the entries of a pack, or of the pack bundled in an executable
  extract FILE DIR [NAME...]    write the entries to DIR (a language's below DIR/<tag>/)
  build -o PACK SPEC...         SPEC is [LANG/]NAME:TYPE=PATH
  bundle EXE PACK -o OUT        OUT = EXE + PACK + trailer (a pack already there is replaced)
  unbundle FILE [--exe OUT] [--pack OUT]
"""
import argparse
import struct
import sys
from dataclasses import dataclass
from typing import Dict, Iterable, List, Mapping, Optional, Sequence, Tuple, Union

from ._util import safe_join, write_file

MAGIC = b'XPAK'
VERSION = 2
HEADER_SIZE = 16
ENTRY_SIZE = 32
NAME_LEN = 20
TRAILER_MAGIC = b'XPKT'
TRAILER_SIZE = 16
LANGS_MAX = 15
TAG_LEN = 8                     # a tag has at most TAG_LEN - 1 characters
LANG_ANY = 0

T_META, T_FONT, T_STRINGS, T_BLOB, T_VOICECUE, T_VOICE = 1, 2, 3, 4, 5, 6
T_ENGINE = 32
TYPE_NAMES = {T_META: 'meta', T_FONT: 'font', T_STRINGS: 'strings', T_BLOB: 'blob',
              T_VOICECUE: 'voicecue', T_VOICE: 'voice'}
LANGS_ENTRY = 'langs'


class PackError(Exception):
    """A pack cannot be built or read."""


class TypeRegistry:
    """The names of the entry types: the layer's (TYPE_NAMES) and an engine's (>= 32)."""

    def __init__(self, engine_types: Optional[Mapping[int, str]] = None) -> None:
        self._names: Dict[int, str] = dict(TYPE_NAMES)
        for number, name in (engine_types or {}).items():
            self.register(number, name)

    def register(self, number: int, name: str) -> int:
        """Names an engine type; returns its number."""
        if not T_ENGINE <= number <= 255:
            raise PackError('engine type %r: the number must be in %d..255' % (name, T_ENGINE))
        if not name or name.isdigit() or any(c in name for c in ' ,=:/'):
            raise PackError('bad type name %r' % name)
        if number in self._names or name in self._names.values():
            raise PackError('type %d or the name %r is registered already' % (number, name))
        self._names[number] = name
        return number

    def name(self, number: int) -> str:
        """The name of a type; 'type<N>' for one that is not registered."""
        return self._names.get(number, 'type%d' % number)

    def number(self, type_: Union[int, str]) -> int:
        """The number of a type given as a number, a name or a decimal string."""
        if isinstance(type_, str):
            for number, name in self._names.items():
                if name == type_:
                    return number
            digits = type_[4:] if type_.startswith('type') else type_
            if not digits.isdigit():
                raise PackError('unknown entry type %r' % type_)
            type_ = int(digits)
        if not 1 <= type_ <= 255:
            raise PackError('entry type %d is not in 1..255' % type_)
        return type_

    def items(self) -> List[Tuple[int, str]]:
        """(number, name) of every registered type, by number."""
        return sorted(self._names.items())


def upper_ascii(name: str) -> str:
    """Upper-cases a-z only, as the readers compare names (other characters stay)."""
    return ''.join(c.upper() if 'a' <= c <= 'z' else c for c in name)


def check_tag(tag: str) -> str:
    """A language tag in its stored form (lower case); PackError when it cannot be stored."""
    tag = tag.lower()
    if not 1 <= len(tag) < TAG_LEN or not all(c.isascii() and (c.isalnum() or c in '-_')
                                              for c in tag):
        raise PackError('bad language tag %r (1..%d letters, digits, - or _)'
                        % (tag, TAG_LEN - 1))
    return tag


@dataclass(frozen=True)
class _Item:
    key: bytes                  # the sort and identity key of the name
    name: bytes                 # as stored
    type: int
    lang: int
    data: bytes


class PackWriter:
    """Collects entries and builds the pack.

    `types` is the registry that resolves type names; `langs` fixes the ids of the languages
    named (the first gets id 1), others get the next id when first used; `name_encoding` is
    the encoding names given as str are stored in.
    """

    def __init__(self, types: Optional[TypeRegistry] = None, langs: Sequence[str] = (),
                 name_encoding: str = 'cp932') -> None:
        self.types = types or TypeRegistry()
        self.name_encoding = name_encoding
        self._langs: List[str] = []
        self._items: List[_Item] = []
        self._keys: set = set()
        for tag in langs:
            self.lang_id(tag)

    def lang_id(self, tag: Optional[str]) -> int:
        """The id of a language tag, assigning the next free one to a new tag; 0 for None."""
        if tag is None or tag == '':
            return LANG_ANY
        tag = check_tag(tag)
        if tag not in self._langs:
            if len(self._langs) >= LANGS_MAX:
                raise PackError('more than %d languages' % LANGS_MAX)
            self._langs.append(tag)
        return self._langs.index(tag) + 1

    @property
    def langs(self) -> Dict[int, str]:
        """{id: tag} of the languages so far."""
        return {i + 1: tag for i, tag in enumerate(self._langs)}

    def add(self, name: Union[str, bytes], type: Union[int, str], data: bytes,
            lang: Optional[str] = None, upper: Optional[bool] = None) -> None:
        """Adds an entry.

        `name` is a str (encoded with the writer's name encoding) or the bytes to store;
        `type` a number or a registered name; `lang` a language tag, None for a neutral
        entry. `upper` forces (True) or prevents (False) the upper-casing of a-z; by default
        the names of engine types and of voice clips are upper-cased.
        """
        number = self.types.number(type)
        if upper is None:
            upper = number >= T_ENGINE or number == T_VOICE
        if isinstance(name, str):
            text = upper_ascii(name) if upper else name
            try:
                stored = text.encode(self.name_encoding)
                key = upper_ascii(text).encode(self.name_encoding)
            except UnicodeEncodeError:
                raise PackError('name %r cannot be stored in %s' % (name, self.name_encoding)) \
                    from None
        else:
            stored = key = bytes(name)
        if not stored or len(stored) > NAME_LEN or b'\0' in stored:
            raise PackError('name %r: 1..%d bytes, no NUL' % (name, NAME_LEN))
        if number == T_META and key == LANGS_ENTRY.upper().encode() and lang is None:
            raise PackError('the "%s" entry is written by the pack writer' % LANGS_ENTRY)
        lang_id = self.lang_id(lang)
        if (key, number, lang_id) in self._keys:
            raise PackError('duplicate entry %r, type %s, language %s'
                            % (name, self.types.name(number), lang or 'neutral'))
        self._keys.add((key, number, lang_id))
        self._items.append(_Item(key, stored, number, lang_id, bytes(data)))

    def names(self) -> List[Tuple[bytes, int, int]]:
        """(stored name, type, lang id) of the entries added so far."""
        return [(i.name, i.type, i.lang) for i in self._items]

    def build(self, dedupe: bool = False, data_order: Optional[Iterable[str]] = None) -> bytes:
        """The pack's bytes.

        dedupe: entries with identical data share one copy (a reader does not notice).
        data_order: entry names (ASCII case ignored) in the order their data is to be laid out
        in the file, for media where seeking is slow; the entries not named follow in table
        order. The table itself is always sorted.
        """
        items = list(self._items)
        if self._langs:
            text = ''.join('%d=%s\n' % (i + 1, tag) for i, tag in enumerate(self._langs))
            items.append(_Item(LANGS_ENTRY.upper().encode(), LANGS_ENTRY.encode(), T_META,
                               LANG_ANY, text.encode('ascii')))
        if len(items) > 0xFFFF:
            raise PackError('%d entries: a pack holds at most 65535' % len(items))
        items.sort(key=lambda i: (i.lang, i.key, i.type))
        rank: Dict[bytes, int] = {}
        for n in data_order or ():
            rank.setdefault(upper_ascii(n).encode(self.name_encoding), len(rank))
        layout = sorted(range(len(items)), key=lambda i: (rank.get(items[i].key, len(rank)), i))
        base = HEADER_SIZE + ENTRY_SIZE * len(items)
        blobs = bytearray()
        seen: Dict[bytes, int] = {}
        at = [0] * len(items)
        for i in layout:
            data = items[i].data
            if dedupe and data in seen:
                at[i] = seen[data]
                continue
            blobs += b'\0' * (-(base + len(blobs)) % 4)
            at[i] = base + len(blobs)
            blobs += data
            if dedupe:
                seen[data] = at[i]
        total = base + len(blobs)
        if total > 0xFFFFFFFF:
            raise PackError('the pack would be larger than 4 GB')
        out = bytearray(MAGIC + struct.pack('<HHI4x', VERSION, len(items), total))
        for item, offset in zip(items, at):
            out += struct.pack('<20sBBHII', item.name, item.type, item.lang, 0, offset,
                               len(item.data))
        return bytes(out + blobs)

    def write(self, path: str, dedupe: bool = False,
              data_order: Optional[Iterable[str]] = None) -> int:
        """Builds the pack and writes it to `path`; returns its size."""
        data = self.build(dedupe, data_order)
        write_file(path, data)
        return len(data)


# ---- reading -----------------------------------------------------------------------------------

@dataclass(frozen=True)
class PackEntry:
    """An entry of a pack that was read. `lang_tag` is '' for a neutral entry."""
    name: str
    raw_name: bytes
    type: int
    lang: int
    lang_tag: str
    offset: int
    size: int
    data: bytes


class Pack:
    """A pack that was read: `entries` in table order, `langs` as {id: tag}, `total` bytes."""

    def __init__(self, entries: List[PackEntry], langs: Dict[int, str], total: int) -> None:
        self.entries = entries
        self.langs = langs
        self.total = total

    def lang_id(self, lang: Union[None, int, str]) -> int:
        """The id of a language given as a tag (0 when the pack does not have it) or an id."""
        if lang is None:
            return LANG_ANY
        if isinstance(lang, int):
            return lang
        for i, tag in self.langs.items():
            if tag == lang.lower():
                return i
        return LANG_ANY

    def find(self, name: str, type: Optional[int] = None,
             lang: Union[None, int, str] = None) -> Optional[PackEntry]:
        """The entry of this name (ASCII case ignored) and type (None: any) in the language
        `lang`, else in the neutral set, as the C reader looks one up; None if absent."""
        want = upper_ascii(name)
        lang_id = self.lang_id(lang)
        for wanted in ([lang_id] if lang_id else []) + [LANG_ANY]:
            for e in self.entries:
                if e.lang == wanted and (type is None or e.type == type) and \
                        upper_ascii(e.name) == want:
                    return e
        return None


def read_pack(data: bytes, name_encoding: str = 'cp932') -> Pack:
    """Parses a pack that starts at the first byte of `data` (see find_pack for a bundle)."""
    if len(data) < HEADER_SIZE or data[:4] != MAGIC:
        raise PackError('not an XPAK pack')
    version, count, total = struct.unpack_from('<HHI', data, 4)
    if version != VERSION:
        raise PackError('pack format version %d, this reader knows version %d'
                        % (version, VERSION))
    if total > len(data) or HEADER_SIZE + count * ENTRY_SIZE > total:
        raise PackError('damaged pack header (size %d, %d entries, %d bytes present)'
                        % (total, count, len(data)))
    raw = []
    for i in range(count):
        name, type_, lang, _, offset, size = struct.unpack_from(
            '<20sBBHII', data, HEADER_SIZE + ENTRY_SIZE * i)
        if offset > total or size > total - offset:
            raise PackError('entry %d lies outside the pack' % i)
        raw.append((name.rstrip(b'\0'), type_, lang, offset, size))
    langs: Dict[int, str] = {}
    for name, type_, lang, offset, size in raw:
        if name.lower() == LANGS_ENTRY.encode() and type_ == T_META and lang == LANG_ANY:
            for line in data[offset:offset + size].decode('ascii', 'replace').splitlines():
                ident, eq, tag = line.partition('=')
                if eq and ident.isdigit() and 1 <= int(ident) <= LANGS_MAX:
                    langs[int(ident)] = tag.strip()[:TAG_LEN - 1]
            break
    entries = [PackEntry(name.decode(name_encoding, 'replace'), name, type_, lang,
                         langs.get(lang, '') if lang else '', offset, size,
                         data[offset:offset + size])
               for name, type_, lang, offset, size in raw]
    return Pack(entries, langs, total)


# ---- bundling ----------------------------------------------------------------------------------

def trailer(pack_size: int) -> bytes:
    """The 16 bytes that follow a pack appended to an executable."""
    return TRAILER_MAGIC + struct.pack('<II', pack_size, pack_size ^ 0xFFFFFFFF) + TRAILER_MAGIC


def find_pack(data: bytes) -> Tuple[int, int]:
    """(offset, size) of the pack in `data`: a pack file, or a file with a bundled pack.

    PackError when there is neither, or when the trailer is damaged.
    """
    if len(data) >= TRAILER_SIZE and data[-16:-12] == TRAILER_MAGIC and \
            data[-4:] == TRAILER_MAGIC:
        size, inverse = struct.unpack_from('<II', data, len(data) - 12)
        if size ^ inverse != 0xFFFFFFFF or size > len(data) - TRAILER_SIZE:
            raise PackError('damaged pack trailer')
        base = len(data) - TRAILER_SIZE - size
        if data[base:base + 4] != MAGIC:
            raise PackError('the trailer does not lead to a pack')
        return base, size
    if data[:4] == MAGIC and len(data) >= HEADER_SIZE:
        return 0, struct.unpack_from('<I', data, 8)[0]
    raise PackError('no pack found (neither a pack file nor an executable with a bundled pack)')


def unbundle(data: bytes) -> Tuple[bytes, Optional[bytes]]:
    """Splits a file into (the executable, its bundled pack); the pack is None without one."""
    if len(data) < TRAILER_SIZE or data[-4:] != TRAILER_MAGIC or data[-16:-12] != TRAILER_MAGIC:
        return data, None
    base, size = find_pack(data)
    return data[:base], data[base:base + size]


def bundle(exe: bytes, pack: bytes) -> bytes:
    """An executable with the pack appended and the trailer after it. A pack that is bundled
    in `exe` already is replaced."""
    if pack[:4] != MAGIC or len(pack) < HEADER_SIZE or \
            struct.unpack_from('<I', pack, 8)[0] != len(pack):
        raise PackError('not a pack, or its size field disagrees with its length')
    return unbundle(exe)[0] + pack + trailer(len(pack))


# ---- command line ------------------------------------------------------------------------------

def _registry(spec: Optional[str]) -> TypeRegistry:
    types = TypeRegistry()
    for item in (spec or '').split(','):
        if item:
            number, eq, name = item.partition('=')
            if not eq or not number.isdigit():
                raise PackError('--types: %r is not NUMBER=NAME' % item)
            types.register(int(number), name)
    return types


def _load(path: str) -> Pack:
    with open(path, 'rb') as f:
        data = f.read()
    base, size = find_pack(data)
    return read_pack(data[base:base + size])


def _cmd_list(a: argparse.Namespace) -> int:
    types = _registry(a.types)
    pack = _load(a.file)
    print('# %d entries, %d bytes, languages: %s'
          % (len(pack.entries), pack.total,
             ' '.join('%d=%s' % kv for kv in sorted(pack.langs.items())) or 'none'))
    for e in pack.entries:
        print('%-20s  %-9s %-4s %9d  @%08X' % (e.name, types.name(e.type),
                                               e.lang_tag or (str(e.lang) if e.lang else '-'),
                                               e.size, e.offset))
    return 0


def _cmd_extract(a: argparse.Namespace) -> int:
    types = _registry(a.types)
    pack = _load(a.file)
    wanted = {upper_ascii(n) for n in a.names}
    plain = [(e.lang, upper_ascii(e.name)) for e in pack.entries]
    count = 0
    for e in pack.entries:
        if wanted and upper_ascii(e.name) not in wanted:
            continue
        name = e.name
        if plain.count((e.lang, upper_ascii(e.name))) > 1:     # same name, several types
            name += '.' + types.name(e.type)
        folder = (e.lang_tag or 'lang%d' % e.lang) + '/' if e.lang else ''
        write_file(safe_join(a.dir, folder + name), e.data)
        count += 1
    print('%d entries -> %s' % (count, a.dir))
    return 0 if count or not wanted else 1


def _cmd_build(a: argparse.Namespace) -> int:
    w = PackWriter(_registry(a.types), langs=[t for t in (a.langs or '').split(',') if t])
    for spec in a.specs:
        left, eq, path = spec.partition('=')
        name, colon, type_ = left.rpartition(':')
        if not eq or not colon or not name:
            raise PackError('%r is not [LANG/]NAME:TYPE=PATH' % spec)
        lang, slash, name = name.rpartition('/')
        with open(path, 'rb') as f:
            w.add(name, type_, f.read(), lang if slash else None)
    size = w.write(a.output, dedupe=a.dedupe)
    print('%s: %d entries, %d bytes' % (a.output, len(w.names()) + bool(w.langs), size))
    return 0


def _cmd_bundle(a: argparse.Namespace) -> int:
    with open(a.exe, 'rb') as f:
        exe = f.read()
    with open(a.pack, 'rb') as f:
        pack = f.read()
    out = bundle(exe, pack)
    write_file(a.output, out)
    print('%s: %d bytes (pack of %d bytes at %d)' % (a.output, len(out), len(pack),
                                                     len(out) - TRAILER_SIZE - len(pack)))
    return 0


def _cmd_unbundle(a: argparse.Namespace) -> int:
    with open(a.file, 'rb') as f:
        exe, pack = unbundle(f.read())
    if pack is None:
        raise PackError('%s has no bundled pack' % a.file)
    if a.exe:
        write_file(a.exe, exe)
    if a.pack:
        write_file(a.pack, pack)
    print('%s: executable %d bytes, pack %d bytes' % (a.file, len(exe), len(pack)))
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    """The command line (see the module's description)."""
    ap = argparse.ArgumentParser(prog='lrvne xpak', description=__doc__.split('\n')[0])
    ap.add_argument('--types', metavar='N=NAME,...', help='names of the engine types (>= 32)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('list', help='the entries of a pack or of a bundled pack')
    p.add_argument('file')
    p.set_defaults(func=_cmd_list)
    p = sub.add_parser('extract', help='write the entries to a directory')
    p.add_argument('file')
    p.add_argument('dir')
    p.add_argument('names', nargs='*')
    p.set_defaults(func=_cmd_extract)
    p = sub.add_parser('build', help='build a pack from files')
    p.add_argument('specs', nargs='+', metavar='[LANG/]NAME:TYPE=PATH')
    p.add_argument('-o', '--output', required=True)
    p.add_argument('--langs', metavar='TAG,...', help='fix the language ids: the first is 1')
    p.add_argument('--dedupe', action='store_true', help='store identical data once')
    p.set_defaults(func=_cmd_build)
    p = sub.add_parser('bundle', help='append a pack to an executable')
    p.add_argument('exe')
    p.add_argument('pack')
    p.add_argument('-o', '--output', required=True)
    p.set_defaults(func=_cmd_bundle)
    p = sub.add_parser('unbundle', help='split an executable and its bundled pack')
    p.add_argument('file')
    p.add_argument('--exe', help='write the executable without the pack here')
    p.add_argument('--pack', help='write the pack here')
    p.set_defaults(func=_cmd_unbundle)
    a = ap.parse_args(argv)
    try:
        return a.func(a)
    except (PackError, ValueError, OSError) as e:
        print('xpak: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
