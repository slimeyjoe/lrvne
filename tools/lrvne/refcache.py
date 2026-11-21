# SPDX-License-Identifier: GPL-3.0-or-later
"""refcache: an engine's reference results, kept between test runs by the content of what they
depend on.

A reference is what an engine's test compares itself with: the original program run under
emulation, a model of a chip, the real driver. It is the same for the same inputs, and it can
be slow, so a Cache keeps each result under a key of everything it depends on:

  * the code that computes it. By default: every Python module under the engine's trees
    (`code_roots`) that the process had loaded when the result was computed. The list is kept
    per kind of result (its manifest, which only grows), and a lookup hashes those files: an
    edit of a tool no reference loads keeps the results, an edit of any module one loads drops
    them. A dependency added later is imported by a module already in the list, whose content
    then changed. Files the import system does not see (data read as code, a module loaded
    from a path without sys.modules) are named in `code_files`;
  * the data it reads: the files and directories in `data`, by content;
  * `salt`: what is not a file, e.g. the emulator's version; Python's version is always in it;
  * the call's arguments: plain data (dicts with str keys, lists, str, int, bool, None).
Validity is decided by content only, never by file times. Only the newest key of a kind is
kept on disk.

The mode comes from the environment variable `env` (default REFCACHE):
  on       (default) use a stored result; compute and store a missing one
  off      always compute, store nothing
  refresh  always compute, store the result
  verify   always compute; fail if a stored result differs (non-determinism, or an input the
           key misses), then store it

An engine whose references depend on more than this describes overrides what it must: pass
`code_files`, `data` and `salt`, per Cache or per call; or subclass Cache and replace
code_files_for(), data_fingerprint(), args_key() or key().

Entries are <root>/<kind>/<key>/<args>.pickle.gz and <root>/<kind>/manifest.txt. Writes are
atomic (a temporary file in the directory, then a rename), so pool workers can share a cache;
an entry that cannot be read is computed again.
"""
import gzip
import hashlib
import os
import pickle
import shutil
import sys
import tempfile

MODES = ('on', 'off', 'refresh', 'verify')
FORMAT = 1


class RefcacheError(Exception):
    """not SystemExit: raised in a multiprocessing worker, that would end the worker and leave
    the pool waiting for its result for ever; an Exception reaches the parent"""


def canon(v):
    """a canonical text of plain data (no tuples: a caller that tells them from lists encodes
    them itself)"""
    if v is None or isinstance(v, bool):
        return repr(v)
    if isinstance(v, int):
        return 'i%d' % v
    if isinstance(v, str):
        return 's' + repr(v)
    if isinstance(v, list):
        return '[' + ','.join(canon(x) for x in v) + ']'
    if isinstance(v, dict):
        if not all(isinstance(x, str) for x in v):
            raise TypeError('refcache: dict keys must be str')
        return '{' + ','.join('%r:%s' % (x, canon(v[x])) for x in sorted(v)) + '}'
    raise TypeError('refcache: %s in the arguments' % type(v).__name__)


def plain(v):
    """True for what a result may be: plain data, bytes and floats included"""
    if v is None or isinstance(v, (bool, int, float, str, bytes)):
        return True
    if isinstance(v, (list, tuple)):
        return all(plain(x) for x in v)
    if isinstance(v, dict):
        return all(isinstance(x, str) and plain(y) for x, y in v.items())
    return False


def hash_paths(h, paths):
    """every file of `paths` (files, or directories walked in order), by name and content"""
    for top in paths:
        top = os.path.abspath(top)
        if os.path.isfile(top):
            files = [top]
        elif os.path.isdir(top):
            files = []
            for d, dirs, names in os.walk(top):
                dirs.sort()
                files += [os.path.join(d, n) for n in sorted(names)]
        else:
            h.update(b'missing:%s\n' % top.encode())
            continue
        for f in files:
            rel = os.path.relpath(f, top).encode() if f != top else os.path.basename(f).encode()
            with open(f, 'rb') as fh:
                data = fh.read()
            h.update(b'%d:%s:%d:' % (len(rel), rel, len(data)))
            h.update(data)


class Cache:
    def __init__(self, root, code_roots=(), code_files=(), data=(), salt='', env='REFCACHE',
                 log=None):
        self.root = os.path.abspath(root)
        self.code_roots = [os.path.realpath(r) for r in code_roots]
        self.code_files = [os.path.abspath(f) for f in code_files]
        self.data = list(data)
        self.salt = salt
        self.env = env
        self.log = log or (lambda msg: print(msg, file=sys.stderr))
        self._data_fp = {}

    # ---- the mode ----------------------------------------------------------------------------
    def mode(self):
        m = os.environ.get(self.env, 'on') or 'on'
        if m not in MODES:
            raise RefcacheError('refcache: %s=%s: not one of %s' % (self.env, m, ', '.join(MODES)))
        return m

    # ---- the code ----------------------------------------------------------------------------
    def loaded_modules(self):
        """the files of the modules loaded now that are under the code roots"""
        out = set()
        for mod in list(sys.modules.values()):
            f = getattr(mod, '__file__', None)
            if not f or not f.endswith('.py'):
                continue
            f = os.path.realpath(f)
            if any(f.startswith(r + os.sep) for r in self.code_roots) and os.path.isfile(f):
                out.add(f)
        return out

    def _manifest(self, kind):
        return os.path.join(self.root, kind, 'manifest.txt')

    def read_manifest(self, kind):
        try:
            with open(self._manifest(kind), encoding='utf-8') as f:
                return {ln.rstrip('\n') for ln in f if ln.strip()}
        except OSError:
            return None

    def write_manifest(self, kind, files):
        """the union of what is there and `files` (it only grows); atomic"""
        old = self.read_manifest(kind) or set()
        new = old | set(files)
        if new == old:
            return
        d = os.path.dirname(self._manifest(kind))
        os.makedirs(d, exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=d, suffix='.tmp')
        with os.fdopen(fd, 'w', encoding='utf-8') as f:
            f.write(''.join(x + '\n' for x in sorted(new)))
        os.replace(tmp, self._manifest(kind))

    def code_files_for(self, kind, extra=()):
        """the code files a kind's key covers: its manifest, the Cache's and the call's code
        files; None while the kind has no manifest (nothing can be a hit yet)"""
        manifest = self.read_manifest(kind)
        if manifest is None:
            return None
        return sorted(manifest | set(self.code_files) | {os.path.abspath(f) for f in extra})

    # ---- the data ----------------------------------------------------------------------------
    def data_fingerprint(self, paths):
        k = tuple(os.path.abspath(p) for p in paths)
        if k not in self._data_fp:
            h = hashlib.sha256()
            hash_paths(h, k)
            self._data_fp[k] = h.hexdigest()
        return self._data_fp[k]

    # ---- the key -----------------------------------------------------------------------------
    def key(self, kind, code_files, data, salt):
        """the directory name of a kind's results for this code, data and salt"""
        h = hashlib.sha256(b'refcache %d %s\n' % (FORMAT, kind.encode()))
        h.update(('python %d.%d\n%s\n' % (sys.version_info[0], sys.version_info[1], salt)).encode())
        for f in code_files:
            # the code by its place in the code roots, so a moved checkout keeps its results
            rel = next((os.path.relpath(f, r) for r in self.code_roots
                        if f.startswith(r + os.sep)), os.path.basename(f))
            h.update(b'code %s\n' % rel.encode())
            hash_paths(h, [f])
        h.update(b'data %s\n' % self.data_fingerprint(data).encode())
        return h.hexdigest()[:32]

    def args_key(self, args):
        return hashlib.sha256(canon(args).encode()).hexdigest()[:32]

    # ---- get ---------------------------------------------------------------------------------
    def get(self, kind, args, compute, code_files=(), data=(), salt=''):
        """compute() for these arguments, or its stored result: (result, True if stored).
        code_files, data and salt add to the Cache's for this call."""
        m = self.mode()
        if m == 'off':
            return compute(), False
        data = self.data + list(data)
        salt = self.salt + ('\n' + salt if salt else '')
        name = self.args_key(args) + '.pickle.gz'
        files = self.code_files_for(kind, code_files)
        stored = None
        if files is not None and m in ('on', 'verify'):
            path = os.path.join(self.root, kind, self.key(kind, files, data, salt), name)
            if os.path.exists(path):
                try:
                    with gzip.open(path, 'rb') as f:
                        stored = pickle.load(f)
                except (OSError, EOFError, pickle.UnpicklingError, ValueError) as e:
                    self.log('refcache: %s unreadable (%s): computed again' % (path, e))
            if m == 'on' and stored is not None:
                return stored, True
        value = compute()
        if not plain(value):
            raise RefcacheError('refcache: the %s result is not plain data' % kind)
        # the code it needed now: the manifest grows, the key is the manifest's
        self.write_manifest(kind, self.loaded_modules())
        files = self.code_files_for(kind, code_files)
        k = self.key(kind, files, data, salt)
        if m == 'verify' and stored is not None and stored != value:
            raise RefcacheError('refcache: verify: the stored %s result for these arguments '
                                'differs from the one computed now' % kind)
        self._store(kind, k, name, value)
        return value, False

    def _store(self, kind, k, name, value):
        top = os.path.join(self.root, kind)
        d = os.path.join(top, k)
        try:
            os.makedirs(d, exist_ok=True)
            fd, tmp = tempfile.mkstemp(dir=d, suffix='.tmp')
            try:
                with os.fdopen(fd, 'wb') as raw, \
                        gzip.GzipFile(fileobj=raw, mode='wb', mtime=0) as f:
                    pickle.dump(value, f, protocol=4)
                os.replace(tmp, os.path.join(d, name))
            except BaseException:
                if os.path.exists(tmp):
                    os.unlink(tmp)
                raise
            # only the newest key is kept (an older one is code or data since changed)
            for other in os.listdir(top):
                p = os.path.join(top, other)
                if other != k and os.path.isdir(p):
                    shutil.rmtree(p, ignore_errors=True)
        except OSError as e:            # another run removed the directory meanwhile
            self.log('refcache: a %s result not stored (%s)' % (kind, e))
