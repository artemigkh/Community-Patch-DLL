"""Offline tests (ticket 4708): the proxy generator, the engine-call audit and the dbfp test take the DLL source from
the one setting, tools/x64-dll/dll_source.py, and name no path of their own (no compiler, no source tree, < 5 s).

    python -I proxy/gen/tests/test_dll_source_setting.py [-v]

Each check runs the tool's module in a fresh interpreter (`python -I`) with a chosen environment, because the defaults
are read at import. The audit and the dbfp test are not exported (ticket 0856: server/tools/ and proxy/tools/ stay in
this repository); in the exported `64bit/` folder the checks cover the files that are there.
"""
import os
import re
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
FILES = ['proxy/gen/toolchain.py', 'proxy/gen/extract_lua.py', 'server/tools/audit_engine_calls.py',
         'proxy/tools/test_dbfp.py']
PRESENT = [f for f in FILES if os.path.isfile(os.path.join(REPO, f))]
USER_PATH = re.compile(r'users[/\\]|documents[/\\]github|appdata[/\\]', re.I)

# Prints one value per line: toolchain FORK_DEFAULT, FORK, BUILD_ROOT; extract_lua FORK, OUT; audit DEFAULT_SRC;
# test_dbfp SRC.
PROBE = r'''
import importlib.util, sys
sys.path.insert(0, 'proxy/gen')
import toolchain as tc, extract_lua as el
import os
def load(name, path):
    if not os.path.isfile(path):
        return None
    s = importlib.util.spec_from_file_location(name, path); m = importlib.util.module_from_spec(s)
    s.loader.exec_module(m); return m
au = load('audit_4708', 'server/tools/audit_engine_calls.py')
db = load('dbfp_4708', 'proxy/tools/test_dbfp.py')
for v in (tc.FORK_DEFAULT, tc.FORK, tc.BUILD_ROOT, el.FORK, el.OUT, au.DEFAULT_SRC if au else '<absent>',
          db.SRC if db else '<absent>'):
    print(v)
'''


def probe(**env):
    e = {k: v for k, v in os.environ.items()
         if k not in ('VP_DLL_SRC', 'VP_PROXY_GEN_SRC', 'VP_PROXY_GEN_COMMIT', 'VP_PROXY_GEN_WORK')}
    e.update(env)
    cp = subprocess.run([sys.executable, '-I', '-c', PROBE], cwd=REPO, env=e, capture_output=True, text=True)
    if cp.returncode != 0:
        raise AssertionError('probe failed: %s' % cp.stderr[-2000:])
    keys = ['tc_default', 'tc_fork', 'tc_work', 'el_fork', 'el_out', 'audit', 'dbfp']
    return {k: v for k, v in zip(keys, cp.stdout.splitlines()) if v != '<absent>'}   # an absent tool: no key


def setting_keys(r):
    return [k for k in ('tc_default', 'tc_fork', 'el_fork', 'audit', 'dbfp') if k in r]


def norm(p):
    return os.path.normcase(os.path.normpath(p))


class Setting(unittest.TestCase):
    def test_no_user_path_in_the_four_files(self):
        for f in PRESENT:
            with open(os.path.join(REPO, f), encoding='utf-8') as fh:
                for n, line in enumerate(fh, 1):
                    self.assertIsNone(USER_PATH.search(line), '%s:%d names a path of a machine: %s' % (f, n, line.strip()))

    def test_vp_dll_src_reaches_all_four(self):
        r = probe(VP_DLL_SRC=r'C:\x4708\vpdll')
        for k in setting_keys(r):
            self.assertEqual(norm(r[k]), norm(r'C:\x4708\vpdll'), k)

    def test_vp_proxy_gen_src_still_overrides_the_generator(self):
        r = probe(VP_DLL_SRC=r'C:\x4708\vpdll', VP_PROXY_GEN_SRC=r'C:\y4708\gen')
        self.assertEqual(norm(r['tc_fork']), norm(r'C:\y4708\gen'))
        self.assertEqual(norm(r['el_fork']), norm(r'C:\y4708\gen'))
        # the audit and the dbfp test never read VP_PROXY_GEN_SRC: they follow the setting
        self.assertEqual(norm(r['tc_default']), norm(r'C:\x4708\vpdll'))
        for k in ('audit', 'dbfp'):
            if k in r:
                self.assertEqual(norm(r[k]), norm(r'C:\x4708\vpdll'), k)

    def test_default_equals_the_setting(self):
        r = probe()
        cp = subprocess.run([sys.executable, '-I', os.path.join('tools', 'x64-dll', 'dll_source.py')], cwd=REPO,
                            env={k: v for k, v in os.environ.items() if k != 'VP_DLL_SRC'},
                            capture_output=True, text=True)
        self.assertEqual(cp.returncode, 0, cp.stderr)
        want = cp.stdout.split('\t')[0].strip()
        for k in setting_keys(r):
            self.assertEqual(norm(r[k]), norm(want), k)

    def test_this_repository_has_all_four(self):
        """In this repository all four files are there and the probe reads all seven values (the export's absence
        handling above must not hide a file lost here)."""
        if not os.path.isdir(os.path.join(REPO, 'tracker')):
            self.skipTest('not this repository (an export): the audit and the dbfp test are not exported')
        self.assertEqual(PRESENT, FILES)
        self.assertEqual(len(probe()), 7)

    def test_work_root(self):
        r = probe(VP_PROXY_GEN_WORK=r'C:\z4708\work')
        self.assertEqual(norm(r['tc_work']), norm(r'C:\z4708\work'))
        self.assertEqual(norm(r['el_out']), norm(r'C:\z4708\work\lua'))
        import tempfile
        r = probe()
        self.assertEqual(norm(r['tc_work']), norm(os.path.join(tempfile.gettempdir(), 'claude', 'team2100')))
        self.assertEqual(norm(r['el_out']), norm(os.path.join(tempfile.gettempdir(), 'claude', 'team2100', 'lua')))


if __name__ == '__main__':
    unittest.main()
