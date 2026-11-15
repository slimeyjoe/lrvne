# SPDX-License-Identifier: GPL-3.0-or-later
"""The import framework: take a game's files from the player's own copy, by inventory.

An engine's repository carries no game data. It carries an *inventory*: a table of the files
the game consists of, each identified by name and md5. `import` looks through whatever the
player has (the `source` module: directories, archives, disk and CD images, nested any way)
and copies the files that match into a destination tree; `check` verifies such a tree; `scan`
writes inventory rows for what a source contains (how an inventory is made).

The inventory is a TSV file with a header line. Three columns are required:
  name   the file's path in the destination tree, '/' separated (the column may be called
         `path` instead); the last component is the name the file is recognised by, ASCII case
         ignored
  size   bytes ('-': any)
  md5    of the contents ('-': not checked: a file that differs between copies, like a save)
Every other column is free (`release`, `disk`, `kind` ...) and available to the engine's
hooks as `row.get('release')`. A column `mtime` ('YYYY-MM-DD HH:MM') is applied to the files
written. Lines starting with '#' and blank lines are ignored.

A file is recognised by name and md5 wherever it is found, so an installed copy supplies the
rows made from floppies, and several rows with the same name and contents are all filled.

Nothing here knows a game. What is game-specific is passed as `Hooks`; an engine's import tool
is then a thin file:

    from lrvne import diskimg, inventory

    def main():
        fs = diskimg.default_filesystems()
        fs.register('myfs', my_probe)                   # only if its media need one
        hooks = inventory.Hooks(
            dest_path=lambda row: row.get('release') + '/' + row.name,
            required=lambda row: row.get('release') == 'floppy',
            group=lambda row: row.get('release'),
            post_import=make_derived_data)
        return inventory.run_cli(None, 'resources/inventory.tsv', 'resources/original',
                                 hooks=hooks, filesystems=fs, prog='gamedata')

Command line of this module itself (python3 -I tools/lrvne inventory ...):
  scan [--columns a,b] [SRC...]               print inventory rows for what the sources hold
  import --inventory TSV --out DIR [SRC...]   copy the matching files to DIR
  check --inventory TSV --out DIR [-q]        verify DIR
SRC defaults to the directories named in the LRVNE_MEDIA environment variable.
"""
import argparse
import os
import sys
import time
from dataclasses import dataclass, field
from typing import Callable, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

from . import diskimg, source
from ._util import md5_file, md5_hex, safe_join, write_file

ANY = '-'
REQUIRED_COLUMNS = ('size', 'md5')
Log = Callable[[str], None]


class InventoryError(Exception):
    """An inventory cannot be read, or an import cannot be carried out."""


@dataclass(frozen=True)
class Row:
    """A line of an inventory. `size` is -1 and `md5` is '-' when not checked."""
    name: str
    size: int
    md5: str
    fields: Tuple[Tuple[str, str], ...] = ()

    def get(self, column: str, default: str = '') -> str:
        """The value of a free column."""
        for k, v in self.fields:
            if k == column:
                return v
        return default

    def __getitem__(self, column: str) -> str:
        for k, v in self.fields:
            if k == column:
                return v
        raise KeyError(column)

    @property
    def basename(self) -> str:
        """The name the file is recognised by."""
        return self.name.rsplit('/', 1)[-1]

    def matches(self, size: int, digest: str) -> bool:
        """True when a file of this size and md5 is this row's file."""
        return (self.size < 0 or self.size == size) and (self.md5 == ANY or self.md5 == digest)


class Inventory:
    """The rows of an inventory and the names of its columns (the name column first, as it is
    called in the file)."""

    def __init__(self, columns: Sequence[str], rows: Iterable[Row]) -> None:
        self.columns = list(columns)
        self.rows = list(rows)

    @classmethod
    def parse(cls, text: str, label: str = 'inventory') -> 'Inventory':
        """Reads the text of an inventory file."""
        head: Optional[List[str]] = None
        rows = []
        for no, line in enumerate(text.splitlines(), 1):
            if not line.strip() or line.startswith('#'):
                continue
            cells = line.split('\t')
            if head is None:
                head = [c.strip() for c in cells]
                name_col = 'name' if 'name' in head else 'path'
                missing = [c for c in (name_col,) + REQUIRED_COLUMNS if c not in head]
                if missing or len(set(head)) != len(head):
                    raise InventoryError('%s: line %d: the header needs the columns name (or '
                                         'path), size and md5, each once' % (label, no))
                continue
            if len(cells) != len(head):
                raise InventoryError('%s: line %d: %d columns, the header has %d'
                                     % (label, no, len(cells), len(head)))
            r = dict(zip(head, cells))
            name, md5 = r[name_col].replace('\\', '/').strip('/'), r['md5'].strip().lower()
            try:
                size = -1 if r['size'].strip() == ANY else int(r['size'])
            except ValueError:
                raise InventoryError('%s: line %d: size %r' % (label, no, r['size'])) from None
            if not name or (md5 != ANY and (len(md5) != 32 or set(md5) - set('0123456789abcdef'))):
                raise InventoryError('%s: line %d: bad name or md5' % (label, no))
            rows.append(Row(name, size, md5, tuple((k, v) for k, v in r.items()
                                                   if k not in (name_col, 'size', 'md5'))))
        if head is None:
            raise InventoryError('%s: no header line' % label)
        return cls([name_col] + [c for c in head if c != name_col], rows)

    @classmethod
    def load(cls, path: str) -> 'Inventory':
        """Reads an inventory file."""
        try:
            with open(path, encoding='utf-8') as f:
                return cls.parse(f.read(), path)
        except OSError as e:
            raise InventoryError('%s: %s' % (path, e.strerror or e)) from e

    def dumps(self) -> str:
        """The inventory as the text of its file."""
        name_col = self.columns[0]
        lines = ['\t'.join(self.columns)]
        for r in self.rows:
            cell = {name_col: r.name, 'size': ANY if r.size < 0 else str(r.size), 'md5': r.md5}
            lines.append('\t'.join(cell[c] if c in cell else r.get(c) for c in self.columns))
        return '\n'.join(lines) + '\n'


def _always(*_args: object) -> bool:
    return True


@dataclass
class Hooks:
    """What an engine knows about its game's files. Every hook has a neutral default.

    dest_path(row) -> str         where a row's file goes below the destination ('/' separated;
                                  default: row.name)
    required(row) -> bool         False for files an import may lack (default: all required)
    group(row) -> str             the heading a row is counted under in the report
    accept(row, entry) -> bool    a further condition for an entry (source.Entry) to fill a row
                                  whose name and md5 it matches, e.g. the right volume label
    scan_row(entry, data) -> dict or None
                                  for `scan`: the row for an entry as {column: value} (`name`
                                  and any free columns; size and md5 are filled in), None to
                                  leave the entry out (default: name = the path inside the
                                  innermost container)
    on_entry(entry)               called for every entry an import looks at, matched or not:
                                  the place to capture what is not a file of the inventory
                                  (a boot sector, a whole image: entry.volume.image)
    post_import(report, dest)     called after an import, before its report is printed: the
                                  place for derived data
    """
    dest_path: Callable[[Row], str] = lambda row: row.name
    required: Callable[[Row], bool] = _always
    group: Callable[[Row], str] = lambda row: ''
    accept: Callable[[Row, source.Entry], bool] = _always
    scan_row: Optional[Callable[[source.Entry, bytes], Optional[Mapping[str, str]]]] = None
    on_entry: Optional[Callable[[source.Entry], None]] = None
    post_import: Optional[Callable[['Report', str], None]] = None


@dataclass
class Report:
    """The outcome of an import or a check.

    have        the rows whose files are in the destination tree and verified
    found       {row: where it was found} for the rows this import filled
    written     files written (the others found were there already)
    missing     the rows not there
    mismatched  (name, md5, where): files with an inventory name whose contents match no row
                of that name, for the names that are still missing
    """
    rows: List[Row]
    have: set = field(default_factory=set)
    found: Dict[Row, str] = field(default_factory=dict)
    written: int = 0
    missing: List[Row] = field(default_factory=list)
    mismatched: List[Tuple[str, str, str]] = field(default_factory=list)
    hooks: Hooks = field(default_factory=Hooks)

    @property
    def ok(self) -> bool:
        """True when no required row is missing."""
        return not any(self.hooks.required(r) for r in self.missing)

    def lines(self, limit: int = 12) -> List[str]:
        """The report as text: a count per group, what is missing, what differs."""
        out = []
        groups: Dict[str, List[Row]] = {}
        for r in self.rows:
            groups.setdefault(self.hooks.group(r), []).append(r)
        for g in sorted(groups):
            rows = groups[g]
            miss = [r for r in rows if r not in self.have]
            line = '  %-12s %4d/%d' % (g or 'files', len(rows) - len(miss), len(rows))
            if miss:
                names = [r.name + ('' if self.hooks.required(r) else ' (optional)') for r in miss]
                line += '  missing: ' + ' '.join(names[:limit]) + \
                    (' ...' if len(names) > limit else '')
            out.append(line)
        for name, digest, where in self.mismatched[:3 * limit]:
            out.append('  differs: %s md5 %s (%s)' % (name, digest, where))
        return out


def _verify(path: str, row: Row) -> bool:
    try:
        if row.size >= 0 and os.path.getsize(path) != row.size:
            return False
        return os.path.isfile(path) and (row.md5 == ANY or md5_file(path) == row.md5)
    except OSError:
        return False


def _set_mtime(path: str, stamp: str) -> None:
    try:
        t = time.mktime(time.strptime(stamp, '%Y-%m-%d %H:%M'))
        os.utime(path, (t, t))
    except (ValueError, OverflowError, OSError):
        pass


def _dest(dest: str, row: Row, hooks: Hooks) -> str:
    try:
        return safe_join(dest, hooks.dest_path(row))
    except ValueError as e:
        raise InventoryError('inventory row %s: %s' % (row.name, e)) from None


def _finish(report: Report, differs: List[Tuple[str, str, str]]) -> Report:
    report.missing = [r for r in report.rows if r not in report.have]
    open_names = {r.basename.upper() for r in report.missing}
    report.mismatched = [d for d in differs if d[0].upper() in open_names]
    return report


def import_files(inventory: Inventory, specs: Sequence[str], dest: str,
                 hooks: Optional[Hooks] = None,
                 filesystems: Optional[diskimg.Filesystems] = None,
                 log: Optional[Log] = None) -> Report:
    """Copies the files of the inventory found in the sources `specs` to the tree `dest`.

    Repeated imports add to what is there. Returns the report; `report.ok` says whether every
    required file is in the tree afterwards. `log`, when given, receives progress lines.
    """
    hooks = hooks or Hooks()
    say = log or (lambda _line: None)
    report = Report(inventory.rows, hooks=hooks)
    by_name: Dict[str, List[Row]] = {}
    for r in inventory.rows:
        by_name.setdefault(r.basename.upper(), []).append(r)
    differs: List[Tuple[str, str, str]] = []
    for spec in specs:
        before = len(report.found)
        try:
            src = source.open_source(spec, filesystems,
                                     lambda label, why: say('skipped %s: %s' % (label, why)))
        except source.SourceError as e:
            raise InventoryError(str(e)) from None
        with src:
            for entry in src.entries():
                if hooks.on_entry:
                    hooks.on_entry(entry)
                rows = by_name.get(entry.name.upper())
                if not rows:
                    continue
                todo = [r for r in rows if r not in report.found
                        and (r.size < 0 or entry.size is None or r.size == entry.size)]
                if not todo and all(r in report.found for r in rows):
                    continue
                data = entry.read()
                digest = md5_hex(data)
                matched = [r for r in todo
                           if r.matches(len(data), digest) and hooks.accept(r, entry)]
                if not matched and not any(r.matches(len(data), digest) for r in rows):
                    differs.append((entry.name, digest, entry.path))
                for r in matched:
                    path = _dest(dest, r, hooks)
                    if not _verify(path, r):
                        write_file(path, data)
                        report.written += 1
                    if r.get('mtime'):
                        _set_mtime(path, r.get('mtime'))
                    report.found[r] = '%s: %s' % (entry.origin, entry.inner_path)
        say('%s: %d files of the inventory' % (spec, len(report.found) - before))
    say('%d files written to %s (%d already there)'
        % (report.written, dest, len(report.found) - report.written))
    report.have = {r for r in inventory.rows
                   if r in report.found or _verify(_dest(dest, r, hooks), r)}
    _finish(report, differs)
    if hooks.post_import:
        hooks.post_import(report, dest)
    return report


def check(inventory: Inventory, dest: str, hooks: Optional[Hooks] = None) -> Report:
    """Verifies the tree `dest` against the inventory; nothing is written."""
    hooks = hooks or Hooks()
    report = Report(inventory.rows, hooks=hooks)
    differs = []
    for r in inventory.rows:
        path = _dest(dest, r, hooks)
        if _verify(path, r):
            report.have.add(r)
        elif os.path.isfile(path):
            differs.append((r.basename, md5_file(path), path))
        else:                           # a file of the right name in another case?
            folder, want = os.path.dirname(path), os.path.basename(path)
            try:
                other = [f for f in os.listdir(folder) if f.upper() == want.upper()]
            except OSError:
                other = []
            if other:
                differs.append((r.basename, ANY, 'named %s: the tools need the name %s'
                                % (other[0], want)))
    return _finish(report, differs)


def scan(specs: Sequence[str], columns: Sequence[str] = ('name', 'size', 'md5'),
         hooks: Optional[Hooks] = None, filesystems: Optional[diskimg.Filesystems] = None,
         log: Optional[Log] = None) -> Inventory:
    """Makes inventory rows for every file the sources hold (each name and md5 once), sorted.

    `columns` are the columns of the result; the values of free columns come from
    `hooks.scan_row`, except `mtime`, which is the entry's date unless the hook sets it.
    """
    hooks = hooks or Hooks()
    say = log or (lambda _line: None)
    columns = list(columns)
    name_col = 'path' if 'path' in columns and 'name' not in columns else 'name'
    for c in (name_col,) + REQUIRED_COLUMNS:
        if c not in columns:
            raise InventoryError('scan: the columns must include name (or path), size and md5')
    rows: Dict[Tuple[str, str], Row] = {}
    for spec in specs:
        try:
            src = source.open_source(spec, filesystems,
                                     lambda label, why: say('skipped %s: %s' % (label, why)))
        except source.SourceError as e:
            raise InventoryError(str(e)) from None
        with src:
            for entry in src.entries():
                data = entry.read()
                cells: Dict[str, str] = {'name': entry.inner_path, 'mtime': entry.mtime}
                if hooks.scan_row:
                    made = hooks.scan_row(entry, data)
                    if made is None:
                        continue
                    cells.update(made)
                    if name_col in made:
                        cells['name'] = made[name_col]
                name = cells['name'].replace('\\', '/').strip('/')
                digest = md5_hex(data)
                rows.setdefault((name, digest), Row(name, len(data), digest, tuple(
                    (c, cells.get(c, '')) for c in columns if c not in (name_col, 'size', 'md5'))))
    ordered = sorted(rows.values(), key=lambda r: (r.fields, r.name, r.md5))
    return Inventory([name_col] + [c for c in columns if c != name_col], ordered)


# ---- command line ------------------------------------------------------------------------------

Command = Tuple[Callable[[argparse.ArgumentParser], None], Callable[[argparse.Namespace], int]]


def run_cli(argv: Optional[Sequence[str]], inventory: Optional[str] = None,
            dest: Optional[str] = None, hooks: Optional[Hooks] = None,
            filesystems: Optional[diskimg.Filesystems] = None, prog: str = 'lrvne inventory',
            description: Optional[str] = None, scan_columns: Optional[Sequence[str]] = None,
            commands: Optional[Mapping[str, Command]] = None) -> int:
    """The `scan`, `import` and `check` commands with an engine's defaults; returns the exit
    status (0: complete, 1: files missing or an error).

    `inventory` and `dest` are the default inventory file and destination tree (without them
    the options --inventory and --out are required). `scan_columns` are the columns `scan`
    prints (default: those of the inventory file if there is one). `commands` adds the engine's
    own subcommands: {name: (configure(parser), run(args) -> status)}.
    """
    ap = argparse.ArgumentParser(prog=prog, description=description or __doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)

    def common(p: argparse.ArgumentParser, out: bool = True) -> None:
        p.add_argument('--inventory', default=inventory, required=inventory is None and out,
                       help='the inventory TSV' + (' (default: %s)' % inventory if inventory
                                                   else ''))
        if out:
            p.add_argument('--out', default=dest, required=dest is None,
                           help='the destination tree' + (' (default: %s)' % dest if dest else ''))
    media = 'the player\'s media: files, directories, archives, images (default: the ' \
            'directories of %s)' % source.MEDIA_ENV
    p = sub.add_parser('import', help='copy the files of the inventory from the sources')
    p.add_argument('src', nargs='*', help=media)
    common(p)
    p = sub.add_parser('check', help='verify the destination tree against the inventory')
    p.add_argument('-q', '--quiet', action='store_true', help='print nothing when complete')
    common(p)
    p = sub.add_parser('scan', help='print inventory rows for what the sources hold')
    p.add_argument('src', nargs='*', help=media)
    p.add_argument('--columns', help='comma-separated columns of the output')
    common(p, out=False)
    for name, (configure, _run) in (commands or {}).items():
        configure(sub.add_parser(name))
    a = ap.parse_args(argv)

    def say(line: str) -> None:
        print('%s: %s' % (prog, line))
    try:
        if commands and a.cmd in commands:
            return int(commands[a.cmd][1](a) or 0)
        if a.cmd == 'scan':
            columns = a.columns.split(',') if a.columns else scan_columns
            if columns is None:
                columns = (Inventory.load(a.inventory).columns
                           if a.inventory and os.path.exists(a.inventory)
                           else ('name', 'size', 'md5'))
            made = scan(source.default_roots(a.src), columns, hooks, filesystems,
                        lambda line: print('%s: %s' % (prog, line), file=sys.stderr))
            sys.stdout.write(made.dumps())
            return 0
        inv = Inventory.load(a.inventory)
        if a.cmd == 'import':
            report = import_files(inv, source.default_roots(a.src), a.out, hooks, filesystems,
                                  say)
        else:
            if not os.path.isdir(a.out):
                say('no directory %s: import the game first' % a.out)
                return 1
            report = check(inv, a.out, hooks)
            if a.quiet and report.ok:
                return 0
        print('\n'.join(report.lines()))
        say('%s: %s' % (a.out, 'every required file is there' if report.ok
                        else 'INCOMPLETE: required files are missing'))
        return 0 if report.ok else 1
    except (InventoryError, source.SourceError, OSError) as e:
        print('%s: %s' % (prog, e), file=sys.stderr)
        return 1


def main(argv: Optional[Sequence[str]] = None) -> int:
    """The command line of the module itself: no defaults, no hooks."""
    return run_cli(argv)


if __name__ == '__main__':
    sys.exit(main())
