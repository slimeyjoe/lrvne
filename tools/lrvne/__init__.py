# SPDX-License-Identifier: GPL-3.0-or-later
"""The build and import tools of lrvne (Python 3.9+, standard library only).

Modules:
  diskimg    floppy and hard disk images (D88, FDI, HDI, NHD, raw) and FAT12/16
  cdimg      CD images (ISO 9660, raw 2352-byte sectors, CUE and CCD sheets)
  source     one way to open what a player has: directories, archives, images
  inventory  the import framework: inventory TSV, scan, import, check
  xpak       the asset pack (include/lrvne/pack.h): writer, reader, bundling
  profile    the game profile text (include/lrvne/profile.h)
  xfont      BDF/PCF bitmap fonts to XFNT glyph tables
  voicepack  the voice pack builder

`python3 -I tools/lrvne TOOL ...` runs a tool's command line; a script that uses the modules
puts the `tools` directory on `sys.path` first (see tools/README.md).
"""

__all__ = ['cdimg', 'diskimg', 'inventory', 'profile', 'source', 'voicepack', 'xfont', 'xpak']
