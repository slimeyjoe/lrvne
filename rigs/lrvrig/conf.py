# SPDX-License-Identifier: GPL-3.0-or-later
"""The rigs' machine settings for Python (rig.sh, rig.conf.example): the value of KEY from the
environment, else from the rig configuration ($RIG_CONF, else tests/rig.conf below the current
directory), else the default."""
import os


def _load():
    path = os.environ.get('RIG_CONF') or os.path.join('tests', 'rig.conf')
    out = {}
    try:
        with open(path, encoding='utf-8') as f:
            for line in f:
                line = line.rstrip('\n')
                if not line or line.startswith('#') or '=' not in line:
                    continue
                k, v = line.split('=', 1)
                out[k] = os.path.expanduser(v) if v.startswith('~/') else v
    except OSError:
        pass
    return out


_CONF = _load()


def get(key, default=None):
    v = os.environ.get(key) or _CONF.get(key)
    return v if v else default
