# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of lrvne.refcache: hits and misses by code, data, salt and arguments; the manifest of
the code a result needed; the modes; damaged entries; only the newest key kept."""
import importlib
import os
import sys
import tempfile
import unittest
from unittest import mock

from lrvne import refcache


class Refcache(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = self.tmp.name
        self.code = os.path.join(t, 'engine')          # the engine's tree
        self.data = os.path.join(t, 'original')        # the game's files
        os.makedirs(self.code)
        os.makedirs(self.data)
        self.write(os.path.join(self.code, 'refmod_used.py'), 'FACTOR = 3\n')
        self.write(os.path.join(self.code, 'refmod_unused.py'), 'X = 1\n')
        self.write(os.path.join(self.data, 'GAME.DAT'), 'abc')
        sys.path.insert(0, self.code)
        self.calls = 0
        self.env = mock.patch.dict(os.environ, {'REFCACHE': 'on'})
        self.env.start()

    def tearDown(self):
        self.env.stop()
        sys.path.remove(self.code)
        for m in ('refmod_used', 'refmod_unused'):
            sys.modules.pop(m, None)
        self.tmp.cleanup()

    def write(self, path, text):
        with open(path, 'w') as f:
            f.write(text)

    def cache(self, **kw):
        kw.setdefault('code_roots', [self.code])
        kw.setdefault('data', [self.data])
        return refcache.Cache(os.path.join(self.tmp.name, 'cache'), log=lambda m: None, **kw)

    def compute(self, n):
        """a reference that uses refmod_used and the game's file"""
        def run():
            self.calls += 1
            mod = importlib.import_module('refmod_used')
            importlib.reload(mod)
            with open(os.path.join(self.data, 'GAME.DAT')) as f:
                return {'value': n * mod.FACTOR, 'data': f.read()}
        return run

    def test_hit_after_miss(self):
        c = self.cache()
        self.assertEqual(c.get('k', {'n': 2}, self.compute(2)), ({'value': 6, 'data': 'abc'}, False))
        self.assertEqual(c.get('k', {'n': 2}, self.compute(2)), ({'value': 6, 'data': 'abc'}, True))
        self.assertEqual(self.calls, 1)
        # other arguments: another entry
        self.assertEqual(c.get('k', {'n': 3}, self.compute(3))[1], False)
        self.assertEqual(self.calls, 2)

    def test_the_code_it_loaded(self):
        c = self.cache()
        c.get('k', [1], self.compute(1))
        manifest = c.read_manifest('k')
        self.assertIn(os.path.realpath(os.path.join(self.code, 'refmod_used.py')), manifest)
        self.assertNotIn(os.path.realpath(os.path.join(self.code, 'refmod_unused.py')), manifest)
        # a module the reference does not load: still a hit
        self.write(os.path.join(self.code, 'refmod_unused.py'), 'X = 2\n')
        self.assertTrue(self.cache().get('k', [1], self.compute(1))[1])
        # one it loads: a miss, and the new code's result (another size: Python's own cache
        # of compiled code goes by size and time)
        self.write(os.path.join(self.code, 'refmod_used.py'), 'FACTOR = 50\n')
        self.assertEqual(self.cache().get('k', [1], self.compute(1)), ({'value': 50, 'data': 'abc'},
                                                                       False))

    def test_data_salt_and_code_files(self):
        self.cache().get('k', [1], self.compute(1))
        self.write(os.path.join(self.data, 'GAME.DAT'), 'abd')
        self.assertFalse(self.cache().get('k', [1], self.compute(1))[1])
        self.assertTrue(self.cache().get('k', [1], self.compute(1))[1])
        self.assertFalse(self.cache(salt='emulator 2').get('k', [1], self.compute(1))[1])
        extra = os.path.join(self.tmp.name, 'game.py')        # code outside the roots
        self.write(extra, 'A = 1\n')
        self.assertFalse(self.cache().get('k', [1], self.compute(1), code_files=[extra])[1])
        self.assertTrue(self.cache().get('k', [1], self.compute(1), code_files=[extra])[1])
        self.write(extra, 'A = 2\n')
        self.assertFalse(self.cache().get('k', [1], self.compute(1), code_files=[extra])[1])
        # data given per call
        more = os.path.join(self.tmp.name, 'fonts')
        os.makedirs(more)
        self.assertFalse(self.cache().get('k', [1], self.compute(1), data=[more])[1])
        self.assertTrue(self.cache().get('k', [1], self.compute(1), data=[more])[1])

    def test_modes(self):
        c = self.cache()
        with mock.patch.dict(os.environ, {'REFCACHE': 'off'}):
            self.assertFalse(c.get('k', [1], self.compute(1))[1])
            self.assertFalse(c.get('k', [1], self.compute(1))[1])
        self.assertIsNone(c.read_manifest('k'))                # off stores nothing
        with mock.patch.dict(os.environ, {'REFCACHE': 'refresh'}):
            self.assertFalse(c.get('k', [1], self.compute(1))[1])
        self.assertTrue(c.get('k', [1], self.compute(1))[1])
        with mock.patch.dict(os.environ, {'REFCACHE': 'verify'}):
            self.assertFalse(c.get('k', [1], self.compute(1))[1])        # the same: fine
            with self.assertRaises(refcache.RefcacheError):
                c.get('k', [1], lambda: {'value': 'other'})              # differs
        with mock.patch.dict(os.environ, {'REFCACHE': 'sometimes'}):
            with self.assertRaises(refcache.RefcacheError):
                c.get('k', [1], self.compute(1))
        self.assertEqual(self.calls, 4)

    def test_damaged_entry_and_plain_data(self):
        c = self.cache()
        c.get('k', [1], self.compute(1))
        top = os.path.join(self.tmp.name, 'cache', 'k')
        d = next(x for x in os.listdir(top) if os.path.isdir(os.path.join(top, x)))
        entry = os.path.join(top, d, os.listdir(os.path.join(top, d))[0])
        with open(entry, 'wb') as f:
            f.write(b'not a gzip file')
        self.assertEqual(c.get('k', [1], self.compute(1)), ({'value': 3, 'data': 'abc'}, False))
        self.assertTrue(c.get('k', [1], self.compute(1))[1])
        with self.assertRaises(refcache.RefcacheError):
            c.get('j', [1], lambda: object())
        with self.assertRaises(TypeError):
            c.get('k', (1,), self.compute(1))                  # a tuple in the arguments

    def test_only_the_newest_key_kept(self):
        self.cache().get('k', [1], self.compute(1))
        self.write(os.path.join(self.data, 'GAME.DAT'), 'xyz')
        self.cache().get('k', [1], self.compute(1))
        top = os.path.join(self.tmp.name, 'cache', 'k')
        self.assertEqual(len([x for x in os.listdir(top) if os.path.isdir(os.path.join(top, x))]),
                         1)


if __name__ == '__main__':
    unittest.main()
