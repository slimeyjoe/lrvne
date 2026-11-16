# SPDX-License-Identifier: GPL-3.0-or-later
"""Command-line launcher of the lrvne tools.

    python3 -I tools/lrvne TOOL [ARGS...]

Python's isolated mode (-I) neither reads PYTHONPATH nor adds the script's directory to the
module search path, so the package cannot be run with `-m` from an arbitrary directory. Running
the package *directory* works everywhere: this file is then the main program, and it makes the
package importable before handing over to the tool.
"""
import importlib
import os
import sys

TOOLS = ('cdimg', 'diskimg', 'exepack', 'inventory', 'pmd', 'source', 'voicepack', 'xfont', 'xpak')


def _bootstrap() -> None:
    """Replaces the package directory on the module path by its parent.

    The package directory itself must not stay there: its `profile` module would hide the
    standard library's.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path[:] = [p for p in sys.path if os.path.abspath(p or os.getcwd()) != here]
    sys.path.insert(0, os.path.dirname(here))


def main(argv=None) -> int:
    """Runs `lrvne.<tool>.main(args)` for the tool named first in `argv`."""
    argv = sys.argv[1:] if argv is None else list(argv)
    if not argv or argv[0] in ('-h', '--help'):
        print('usage: python3 -I tools/lrvne TOOL [ARGS...]\n\ntools: %s\n'
              '`TOOL --help` describes one of them.' % ' '.join(TOOLS))
        return 0 if argv else 2
    if argv[0] not in TOOLS:
        print('lrvne: no tool named %r (tools: %s)' % (argv[0], ' '.join(TOOLS)), file=sys.stderr)
        return 2
    module = importlib.import_module('lrvne.' + argv[0])
    return int(module.main(argv[1:]) or 0)


if __name__ == '__main__':
    _bootstrap()
    sys.exit(main())
