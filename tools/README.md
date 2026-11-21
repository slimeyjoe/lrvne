# lrvne tools

The Python side of lrvne: what an engine's build needs before the C code runs. The tools read
a player's original media, pick out the files a game consists of, and build the packs the
engine loads. They are a package, `tools/lrvne`, used both from the command line and as a
library by an engine's own build scripts.

Python 3.9 or later, standard library only. Nothing here contains or knows any game: which
files make up a game, and what they are, is an engine's inventory and an engine's code.

| Module | What it does |
|---|---|
| `diskimg` | floppy and hard disk images: D88, Anex86 FDI/HDI, T98-Next NHD, raw sector images; the PC-98 partition table; FAT12/16; a registry for an engine's own file systems |
| `cdimg` | CD images: ISO 9660 from `.iso` and raw `.bin`/`.img`, CUE and CloneCD sheets, audio tracks to WAV |
| `source` | one way to open whatever a player has: a directory, a zip, a 7z/rar/lzh archive, a disk or CD image, nested in any order |
| `inventory` | the import framework: an inventory TSV (name, size, md5), `scan`, `import`, `check` |
| `xpak` | the asset pack of `include/lrvne/pack.h`: writer, reader, bundling into an executable |
| `profile` | the game profile text of `include/lrvne/profile.h` |
| `xfont` | BDF/PCF bitmap fonts, extra glyphs and raw glyph arrays to XFNT glyph tables |
| `exepack` | undoes Microsoft EXEPACK on DOS executables that hold game data (tables, fonts, texts) |
| `reflow` | the layout of translated text (`src/text/reflow.c` in Python): the reference an engine's tests compare its layout with; a library module, no command line |
| `voicepack` | the voice pack: clips, the `voice` description, optional Opus transcoding |
| `refcache` | an engine's test references (the original under emulation, models) kept between runs, keyed by the content of the code they loaded, the data they read and their arguments; modes on, off, refresh, verify (`REFCACHE`); an engine adds its own files, data and salts, or subclasses the key |
| `pmd` | PMD 4.7g data for the PMD module: the SSG-effect table out of a player's PMD.COM; the test songs |

Each module's docstring is its reference: formats, API, command line.

## Running a tool

```
python3 -I tools/lrvne TOOL [ARGS...]         # from an engine: python3 -I $(LRVNE)/tools/lrvne ...

python3 -I tools/lrvne diskimg ls disk.d88
python3 -I tools/lrvne cdimg tracks disc.cue
python3 -I tools/lrvne source ls --md5 /path/to/my-copy
python3 -I tools/lrvne xpak list game.pak
python3 -I tools/lrvne xfont --no-bold -o font16.xfnt font.bdf
python3 -I tools/lrvne TOOL --help
```

The tools are always run with `-I` (isolated mode), so that nothing in the environment or the
current directory can change what they import. In that mode Python reads no `PYTHONPATH` and
does not add a script's directory to the module path, so `python3 -I -m lrvne.xpak` only works
from a directory that happens to contain the package. Running the package *directory* works
from anywhere: `tools/lrvne/__main__.py` is the launcher, and it puts `tools` on the module
path itself.

## Using the modules from an engine

An engine's script adds the `tools` directory of its lrvne checkout to the module path and
imports the package; it is then run with `-I` like the tools:

```python
#!/usr/bin/env python3
"""mkpack.py: build this engine's pack."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'lrvne', 'tools'))     # the submodule
from lrvne import profile, xfont, xpak    # noqa: E402

T_PICTURE, T_SCRIPT = xpak.T_ENGINE, xpak.T_ENGINE + 1            # this engine's types
TYPES = xpak.TypeRegistry({T_PICTURE: 'picture', T_SCRIPT: 'script'})

w = xpak.PackWriter(TYPES)
w.add('profile', xpak.T_META, profile.dumps({'id': 'mygame', 'title': 'My Game',
                                             'lang': 'ja'}).encode('utf-8'))
w.add('font16', xpak.T_FONT, xfont.convert(['fonts/k16.bdf'], no_bold=True))
w.add('title.pic', T_PICTURE, picture_bytes)            # stored as TITLE.PIC
w.add('text', xpak.T_STRINGS, table, lang='en')         # the "langs" entry is written for you
w.write('build/mygame.pak')
```

From make:

```make
LRVNE_PY := python3 -I
LRVNE_TOOL := $(LRVNE_PY) $(LRVNE)/tools/lrvne

build/font16.xfnt: fonts/k16.bdf
	$(LRVNE_TOOL) xfont --no-bold -o $@ $<

build/mygame.pak: tools/mkpack.py build/font16.xfnt
	$(LRVNE_PY) tools/mkpack.py -o $@

build/mygame-full$(EXESUF): $(EXE) build/mygame.pak
	$(LRVNE_TOOL) xpak bundle $(EXE) build/mygame.pak -o $@

data:                           # make data SRC="my-copy.zip"   (or LRVNE_MEDIA=... make data)
	$(LRVNE_PY) tools/gamedata.py import $(SRC)
```

## Importing a game: the engine's thin `gamedata.py`

The engine's repository carries an inventory of the game's files, not the files. Its import
tool is the framework plus what only the engine knows:

```python
from lrvne import diskimg, inventory

def my_probe(image):                    # only if the media carry a file system of their own
    return [MyVolume(image)] if image.data[0x1000:0x1008] == b'MYFORMAT' else []

def keep_boot_sector(entry):            # things that are not files of the inventory
    ...                                 # entry.volume.image.data is the whole disk

def main():
    fs = diskimg.default_filesystems()
    fs.register('myfs', my_probe)
    hooks = inventory.Hooks(
        dest_path=lambda row: row.get('release') + '/' + row.name,
        required=lambda row: row.get('release') == 'floppy',
        group=lambda row: row.get('release'),
        on_entry=keep_boot_sector,
        post_import=lambda report, dest: None)          # derived data
    return inventory.run_cli(None, 'resources/inventory.tsv', 'resources/original',
                             hooks=hooks, filesystems=fs, prog='gamedata',
                             commands={'derive': (add_derive_arguments, run_derive)})
```

That gives `gamedata.py import [SRC...]`, `check [-q]` and `scan [SRC...]` (which prints
inventory rows for what a source holds: how the inventory is first made), plus the engine's own
subcommands. A file is recognised by name and md5 wherever it is found, so floppy images, a
hard disk image, an installed directory or an archive of any of them all work, in any mix, and
repeated imports add to what is there. The exit status is 1 while required files are missing;
the report names them and the files that have a right name but wrong contents.

A file system registered this way implements `diskimg.Volume`: `fstype`, `label`, `origin`,
`image` and `files()`, which yields `diskimg.FileEntry(path, size, mtime, attr, reader)`.

A release that keeps its files in an archive format of its own (a resource archive on a CD) is
read through a function registered by the archive's file name extension:
`inventory.Hooks(containers={'.arc': read_arc})`, where `read_arc(data)` returns the archive's
members as `(name, contents)` pairs, or `None` when the file is not such an archive. Import and
scan then see the members wherever the archive is found (`source.open_source(...,
containers=...)` does the same for a script of the engine's).

## Where the player's media are

Never in the tools and never in a repository. Every command takes its sources as arguments;
with none, the directories listed in the `LRVNE_MEDIA` environment variable are searched
(several are separated like `PATH`), and a relative source that does not exist is looked for
below them:

```
LRVNE_MEDIA=/path/to/my/copies python3 -I tools/gamedata.py import
python3 -I tools/gamedata.py import /path/to/disk1.d88 "/path/to/archive.zip::inner/disk2.d88"
```

`CONTAINER::member` names one member of an archive. Archives other than zip are unpacked by
an external `bsdtar` (libarchive) or `7z`, whichever is installed, into a temporary directory
that is removed afterwards; without either the message says so and nothing else is affected.
Flux images (SCP, MFI, HFE) are not read: convert them to D88.

## Optional external programs

| Program | Needed for |
|---|---|
| `bsdtar` or `7z` (`unrar` for some rar files) | sources in 7z, rar, lzh, cab or tar archives |
| `ffmpeg` with libopus, or `opusenc` | `voicepack` with `codec='opus'` |

## Tests

```
python3 -I -m unittest discover -s tools/tests -t tools         # also part of `make check`
```

The tests build every disk image, CD image, font and pack they use, in memory or in a
temporary directory (`tools/tests/fixtures.py`); there is no game data in this repository.
Tests that need an external program are skipped where it is not installed.
