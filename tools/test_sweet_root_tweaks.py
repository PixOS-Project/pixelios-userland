"""sweet-root-tweaks.py: --plan (no root) must produce the same tree as the legacy in-place mode.

Both run under fakeroot on the synthetic tree of tools/rootless/selftest.py (owners 0/80/501 are faked);
skipped where fakeroot is missing.
"""
import importlib.util, io, os, shutil, subprocess, sys, tarfile, tempfile, unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
TW = os.path.join(TOOLS, 'sweet-root-tweaks.py')
LIST = ("cd %s && find . -printf '%%m %%U %%G %%y %%p -> %%l\\n' | sort; "
        "find . -type f -exec sha256sum {} + | sort -k2")


def sh(cmd, *fk):
    subprocess.run(['fakeroot', *fk, '--', 'sh', '-ec', 'umask 022; ' + cmd], check=True, env=env, stdout=subprocess.DEVNULL)


@unittest.skipUnless(shutil.which('fakeroot'), 'fakeroot not installed')
class PlanEqualsLive(unittest.TestCase):
    def test_same_tree(self):
        global env
        with tempfile.TemporaryDirectory() as t:
            os.mkdir(t + '/bin')
            with open(t + '/bin/heif-convert', 'w') as f:
                f.write('#!/bin/sh\nprintf "PNG of %s\\n" "$(basename "$1")" >"$2"\n')
            os.chmod(t + '/bin/heif-convert', 0o755)
            env = dict(os.environ, PATH=t + '/bin:' + os.environ['PATH'])
            sh(f'python3 -I {TOOLS}/rootless/selftest.py tree {t}/T', '-s', t + '/s0')
            src = t + '/T/os/root'
            sh(f'cp -a {src} {t}/A && python3 {TW} {t}/A', '-i', t + '/s0', '-s', t + '/s1')
            sh(f'cp -a {src} {t}/B && python3 {TW} --plan {t}/B {t}/P && R={t}/B sh -e {t}/P/ops.sh '
               f'&& tar -x -p --numeric-owner -C {t}/B -f {t}/P/overlay.tar', '-i', t + '/s0', '-s', t + '/s2')
            out = []
            for d, st in (('A', 's1'), ('B', 's2')):
                out.append(subprocess.run(['fakeroot', '-i', f'{t}/{st}', '--', 'sh', '-c', LIST % f'{t}/{d}'],
                                          check=True, capture_output=True, text=True).stdout)
            self.assertGreater(out[0].count('\n'), 50)
            self.maxDiff = None
            self.assertEqual(out[0], out[1])

    def test_guard(self):
        spec = importlib.util.spec_from_file_location('tw', TW)
        tw = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tw)
        with tempfile.TemporaryDirectory() as t:
            tar = tarfile.open(fileobj=io.BytesIO(), mode='w')
            plan = tw.Plan(t, tar, guard={'usr/lib/x'})
            plan.exists('usr/lib/y')                       # fine
            with self.assertRaises(RuntimeError):
                plan.exists('usr/lib/x')
            with self.assertRaises(RuntimeError):
                plan.listdir('usr/lib')                    # a later step adds a file there
            # F3: the change operations are guarded too
            for op in (lambda: plan.write('usr/lib/x', b'', 0, 0, 0o644), lambda: plan.makedirs('usr/lib/x/d'),
                       lambda: plan.touch('usr/lib/x'), lambda: plan.chown('usr/lib/x', 0, 0),
                       lambda: plan.chmod('usr/lib/x', 0o644), lambda: plan.move('usr/lib/y', 'usr/lib/x'),
                       lambda: plan.move('usr/lib/x', 'usr/lib/y')):
                with self.assertRaises(RuntimeError):
                    op()
            plan.makedirs('usr/lib/y')                     # allow: a path outside the guard still works
            plan.write('usr/lib/y/f', b'', 0, 0, 0o644)

    def _tw(self):
        spec = importlib.util.spec_from_file_location('tw', TW)
        tw = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tw)
        return tw

    def test_symlink_write(self):
        # F1: writing through a symlink is refused in both modes and leaves the target alone.
        tw = self._tw()
        with tempfile.TemporaryDirectory() as t:
            os.makedirs(t + '/d')
            with open(t + '/d/real', 'w') as f:
                f.write('x')
            os.chmod(t + '/d/real', 0o600)
            os.symlink('real', t + '/d/link')
            for fs in (tw.Live(t), tw.Plan(t, tarfile.open(fileobj=io.BytesIO(), mode='w'))):
                with self.assertRaises(RuntimeError):
                    fs.write('d/link', b'y', 0, 0, 0o777)
            self.assertEqual(os.stat(t + '/d/real').st_mode & 0o7777, 0o600)
            with open(t + '/d/real') as f:
                self.assertEqual(f.read(), 'x')
            tw.Live(t).write('d/new', b'z', os.getuid(), os.getgid(), 0o644)    # allow: a plain file
            tw.Plan(t, tarfile.open(fileobj=io.BytesIO(), mode='w')).write('d/real', b'y', 0, 0, 0o644)

    def test_walk_same_in_both_modes(self):
        # F2: a directory symlink is listed but not entered, in plan mode as in live mode.
        tw = self._tw()
        with tempfile.TemporaryDirectory() as t:
            os.makedirs(t + '/a/real')
            open(t + '/a/real/f', 'w').close()
            os.symlink('real', t + '/a/link')
            walks = [[(p, d, fl) for p, d, fl in fs.walk('')]
                     for fs in (tw.Live(t), tw.Plan(t, tarfile.open(fileobj=io.BytesIO(), mode='w')))]
            self.assertEqual(walks[0], walks[1])
            self.assertNotIn('a/link', [p for p, _, _ in walks[1]])
            self.assertIn('a/real', [p for p, _, _ in walks[1]])


class BluetoothKept(unittest.TestCase):
    """BT-S3 g: the bluetoothd job stays in launchd.plist and BTAudioHALPlugin.driver stays in HAL (no B-A5 drop, no B-A6 isolation)."""

    def _tw(self):
        spec = importlib.util.spec_from_file_location('tw', TW)
        tw = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tw)
        return tw

    def test_keep_virtualaudio_env(self):
        tw = self._tw()
        for val, kept in ((None, False), ('1', True)):
            with tempfile.TemporaryDirectory() as t:
                os.makedirs(t + '/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin')
                fs = tw.Live(t)
                out = io.StringIO()
                old, sys.stdout, oldenv = sys.stdout, out, os.environ.get('PIXOS_KEEP_VIRTUALAUDIO')
                try:
                    if val is None:
                        os.environ.pop('PIXOS_KEEP_VIRTUALAUDIO', None)
                    else:
                        os.environ['PIXOS_KEEP_VIRTUALAUDIO'] = val
                    tw.tweak_audio(fs)
                finally:
                    sys.stdout = old
                    if oldenv is None:
                        os.environ.pop('PIXOS_KEEP_VIRTUALAUDIO', None)
                    else:
                        os.environ['PIXOS_KEEP_VIRTUALAUDIO'] = oldenv
                self.assertEqual(os.path.isdir(t + '/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin'), kept)
                self.assertEqual(os.path.isdir(t + '/Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin'), not kept)
                self.assertIn('VirtualAudio.plugin: ' + ('kept' if kept else 'moved'), out.getvalue())

    def test_bluetooth_restore(self):
        import plistlib
        tw = self._tw()
        self.assertNotIn('com.apple.bluetoothd', tw.DROPPED_JOBS)
        with tempfile.TemporaryDirectory() as t:
            os.makedirs(t + '/System/Library/xpc')
            jobs = {'/a/bt': {'Label': 'com.apple.bluetoothd'}, '/a/cc': {'Label': 'com.apple.CommCenter'}}
            with open(t + '/System/Library/xpc/launchd.plist', 'wb') as f:
                f.write(plistlib.dumps({'LaunchDaemons': jobs}, fmt=plistlib.FMT_BINARY))
            hal = t + '/Library/Audio/Plug-Ins/HAL'
            os.makedirs(hal + '/BTAudioHALPlugin.driver')
            os.makedirs(hal + '/VirtualAudio.plugin')
            os.makedirs(t + '/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver')
            fs = tw.Live(t)
            out = io.StringIO()
            old, sys.stdout = sys.stdout, out
            try:
                tw.tweak_launchd(fs)
                tw.tweak_audio(fs)
            finally:
                sys.stdout = old
            labels = {j['Label'] for j in plistlib.loads(fs.read('System/Library/xpc/launchd.plist'))['LaunchDaemons'].values()}
            self.assertIn('com.apple.bluetoothd', labels)
            self.assertNotIn('com.apple.CommCenter', labels)       # the other drops still happen
            self.assertTrue(os.path.isdir(hal + '/BTAudioHALPlugin.driver'))
            self.assertTrue(os.path.isdir(t + '/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver'))
            self.assertFalse(os.path.exists(t + '/Library/Audio/Plug-Ins/HAL.disabled/BTAudioHALPlugin.driver'))
            self.assertFalse(os.path.exists(t + '/System/Library/Audio/Plug-Ins/HAL.disabled'))
            self.assertNotIn('BTAudioHALPlugin', out.getvalue())
            self.assertNotIn('bluetooth', out.getvalue().lower())
            print('RESULT: PASS bt-userland-restore')


def rd(path):
    with open(path, 'rb') as f:
        return f.read()


class WifidProbeTweak(unittest.TestCase):
    """T-9f3b: the wifid-probe one-shot job is added only when PIXOS_WIFID_PROBE names a binary signed with wifid's
    keychain entitlements; the default root is unchanged; a binary without them is refused."""

    ENTS = {'application-identifier': 'com.apple.wifid', 'com.apple.private.system-keychain': True,
            'keychain-access-groups': ['apple', 'com.apple.identities']}

    def _tw(self):
        spec = importlib.util.spec_from_file_location('tw', TW)
        tw = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tw)
        return tw

    def _macho(self, ents=None, signed=True):
        import plistlib
        spec = importlib.util.spec_from_file_location('mkinit', os.path.join(TOOLS, 'mkinit.py'))
        mk = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mk)
        body = b'\0' * 0x1000
        if not signed:
            return bytes.fromhex('cffaedfe0c000001') + bytes(24) + body
        xml = plistlib.dumps(ents, fmt=plistlib.FMT_XML) if ents is not None else None
        head = bytearray(bytes.fromhex('cffaedfe0c000001') + bytes(4) + (2).to_bytes(4, 'little') + (1).to_bytes(4, 'little')
                         + (16).to_bytes(4, 'little') + bytes(8))
        cmd_off = len(head)
        image = bytes(head) + bytes(16) + body[:0x1000 - 48]
        sig = mk.code_signature(image, len(image), len(image), xml)
        image = bytearray(image)
        image[cmd_off:cmd_off + 16] = (0x1d).to_bytes(4, 'little') + (16).to_bytes(4, 'little') + len(image).to_bytes(4, 'little') + len(sig).to_bytes(4, 'little')
        return bytes(image) + sig

    def _root(self, t):
        import plistlib
        os.makedirs(t + '/System/Library/xpc')
        with open(t + '/System/Library/xpc/launchd.plist', 'wb') as f:
            f.write(plistlib.dumps({'LaunchDaemons': {'/a/cc': {'Label': 'com.apple.keep'}}}, fmt=plistlib.FMT_BINARY))

    def _run(self, tw, root, env, fs=None):
        out, old = io.StringIO(), sys.stdout
        saved = os.environ.get(tw.WIFID_PROBE_ENV)
        os.environ.pop(tw.WIFID_PROBE_ENV, None)
        if env:
            os.environ[tw.WIFID_PROBE_ENV] = env
        sys.stdout = out
        try:
            tw.tweak_wifid_probe(fs or tw.Live(root))
        finally:
            sys.stdout = old
            os.environ.pop(tw.WIFID_PROBE_ENV, None)
            if saved is not None:
                os.environ[tw.WIFID_PROBE_ENV] = saved
        return out.getvalue()

    def test_gated_install_and_refusals(self):
        import plistlib
        tw = self._tw()
        with tempfile.TemporaryDirectory() as t:
            self._root(t + '/R')
            before = rd(t + '/R/System/Library/xpc/launchd.plist')
            msg = self._run(tw, t + '/R', None)                      # default: unchanged, and says so
            self.assertIn('not installed', msg)
            self.assertEqual(before, rd(t + '/R/System/Library/xpc/launchd.plist'))
            self.assertFalse(os.path.exists(t + '/R/usr/local/bin/wifid-probe'))
            for name, blob, why in (('unsigned', self._macho(signed=False), 'no code signature'),
                                    ('noents', self._macho(None), 'no XML entitlements'),
                                    ('wrongid', self._macho(dict(self.ENTS, **{'application-identifier': 'x.y'})), 'com.apple.wifid'),
                                    ('nokc', self._macho({k: v for k, v in self.ENTS.items() if k != 'keychain-access-groups'}), 'keychain-access-groups'),
                                    ('notmacho', b'#!/bin/sh\n', 'not an arm64 Mach-O')):
                with open(f'{t}/{name}', 'wb') as f:
                    f.write(blob)
                with self.assertRaises(RuntimeError, msg=name) as cm:
                    self._run(tw, t + '/R', f'{t}/{name}')
                self.assertIn(why, str(cm.exception))
            self.assertEqual(before, rd(t + '/R/System/Library/xpc/launchd.plist'))   # refusals wrote nothing
            with open(t + '/good', 'wb') as f:
                f.write(self._macho(self.ENTS))
            plan = tw.Plan(t + '/R', tarfile.open(fileobj=io.BytesIO(), mode='w'))     # Plan: no root needed
            msg = self._run(tw, t + '/R', t + '/good', plan)
            self.assertIn('installed', msg)
            for _ in range(2):                                       # a second apply replaces, never duplicates
                self._run(tw, t + '/R', t + '/good', plan)
            jobs = plistlib.loads(plan.read('System/Library/xpc/launchd.plist'))['LaunchDaemons']
            probe = [j for j in jobs.values() if j['Label'] == 'com.pixos.wifid-probe']
            self.assertEqual(len(probe), 1)
            self.assertEqual(probe[0]['ProgramArguments'], ['/usr/local/bin/wifid-probe'])
            self.assertTrue(probe[0]['RunAtLoad'])
            self.assertNotIn('KeepAlive', probe[0])
            self.assertNotIn('StandardOutPath', probe[0])             # T-99f8: /dev/null (xnu 0048 sweetnull tee), not /dev/console
            self.assertNotIn('StandardErrorPath', probe[0])
            self.assertEqual(probe[0]['EnvironmentVariables'], {'OS_ACTIVITY_DT_MODE': 'YES'})
            self.assertEqual({j['Label'] for j in jobs.values()} - {'com.pixos.wifid-probe'}, {'com.apple.keep'})
            uid, gid, mode, data = plan.new_files['usr/local/bin/wifid-probe']
            self.assertEqual((uid, gid, mode), (0, 0, 0o755))
            self.assertEqual(data, rd(t + '/good'))
            self.assertEqual(before, rd(t + '/R/System/Library/xpc/launchd.plist'))   # Plan leaves SRC untouched
            print('RESULT: PASS root-tweak-wifid-probe')


@unittest.skipUnless(shutil.which('fakeroot'), 'fakeroot not installed')
class BTAppTweak(unittest.TestCase):
    """T-2d63: /Applications/BTApp.app is baked only when PIXOS_BTAPP_BUNDLE names a bundle whose Info.plist says
    com.pixos.btapp and whose executable is signed with exactly the app entitlements and CodeDirectory identifier
    com.pixos.btapp; the default root is unchanged; every other bundle is refused before anything is written."""

    ENTS = {'application-identifier': 'com.pixos.btapp', 'com.apple.bluetooth.system': True, 'com.apple.private.tcc.allow': ['kTCCServiceBluetoothAlways']}

    def _macho(self, ents, ident=b'com.pixos.btapp\0'):
        spec = importlib.util.spec_from_file_location('mkinit', os.path.join(TOOLS, 'mkinit.py'))
        mk = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mk)
        mk.IDENT = ident                                                    # the CodeDirectory identifier written below
        import plistlib
        body = b'\0' * 0x1000
        if ents is None:
            return bytes.fromhex('cffaedfe0c000001') + bytes(24) + body
        head = bytearray(bytes.fromhex('cffaedfe0c000001') + bytes(4) + (2).to_bytes(4, 'little') + (1).to_bytes(4, 'little')
                         + (16).to_bytes(4, 'little') + bytes(8))
        cmd_off = len(head)
        image = bytes(head) + bytes(16) + body[:0x1000 - 48]
        sig = mk.code_signature(image, len(image), len(image), plistlib.dumps(ents, fmt=plistlib.FMT_XML))
        image = bytearray(image)
        image[cmd_off:cmd_off + 16] = ((0x1d).to_bytes(4, 'little') + (16).to_bytes(4, 'little') + len(image).to_bytes(4, 'little')
                                       + len(sig).to_bytes(4, 'little'))
        return bytes(image) + sig

    def _bundle(self, d, bid='com.pixos.btapp', exe='BTApp', ents=None, ident=b'com.pixos.btapp\0', seal=True):
        import plistlib
        os.makedirs(d + '/_CodeSignature')
        with open(d + '/Info.plist', 'wb') as f:
            f.write(plistlib.dumps({'CFBundleIdentifier': bid, 'CFBundleExecutable': exe}, fmt=plistlib.FMT_BINARY))
        with open(d + '/BTApp', 'wb') as f:
            f.write(self._macho(self.ENTS if ents is None else ents, ident))
        if seal:
            with open(d + '/_CodeSignature/CodeResources', 'wb') as f:
                f.write(b'<plist/>')

    def _run(self, tw, fs, env):
        out, old = io.StringIO(), sys.stdout
        saved = os.environ.pop(tw.BTAPP_ENV, None)
        if env:
            os.environ[tw.BTAPP_ENV] = env
        sys.stdout = out
        try:
            tw.tweak_btapp(fs)
        finally:
            sys.stdout = old
            os.environ.pop(tw.BTAPP_ENV, None)
            if saved is not None:
                os.environ[tw.BTAPP_ENV] = saved
        return out.getvalue()

    def test_gated_install_and_refusals(self):
        tw = WifidProbeTweak._tw(self)
        with tempfile.TemporaryDirectory() as t:
            os.makedirs(t + '/R/Applications/Preferences.app')
            os.makedirs(t + '/R/System/Library/xpc')
            self.assertIn('not installed', self._run(tw, tw.Live(t + '/R'), None))
            self.assertEqual(os.listdir(t + '/R/Applications'), ['Preferences.app'])
            for name, kw, why in (('wrongbid', {'bid': 'com.pixos.x'}, 'CFBundleIdentifier'),
                                  ('wrongexe', {'exe': 'Other'}, 'CFBundleExecutable'),
                                  ('noseal', {'seal': False}, 'CodeResources'),
                                  ('wrongents', {'ents': {'application-identifier': 'com.pixos.btapp'}}, 'sealed entitlements'),
                                  ('extraents', {'ents': dict(self.ENTS, **{'com.apple.private.security.no-sandbox': True})},
                                   'sealed entitlements'),
                                  ('wrongident', {'ident': b'BTApp\0'}, 'CodeDirectory identifier')):
                self._bundle(f'{t}/{name}', **kw)
                with self.assertRaises(RuntimeError, msg=name) as cm:
                    self._run(tw, tw.Live(t + '/R'), f'{t}/{name}')
                self.assertIn(why, str(cm.exception), name)
            os.makedirs(t + '/unsigned')
            self._bundle(t + '/unsigned')
            with open(t + '/unsigned/BTApp', 'wb') as f:
                f.write(self._macho(None))
            with self.assertRaises(RuntimeError) as cm:
                self._run(tw, tw.Live(t + '/R'), t + '/unsigned/')
            self.assertIn('no code signature', str(cm.exception))
            self._bundle(t + '/dirlink')                                        # review T-2d63 finding 2
            os.makedirs(t + '/elsewhere')
            os.symlink(t + '/elsewhere', t + '/dirlink/Sub')
            with self.assertRaises(RuntimeError) as cm:
                self._run(tw, tw.Live(t + '/R'), t + '/dirlink')
            self.assertIn('symlinked directory', str(cm.exception))
            self.assertEqual(os.listdir(t + '/R/Applications'), ['Preferences.app'])     # refusals wrote nothing
            self._bundle(t + '/good')
            plan = tw.Plan(t + '/R', tarfile.open(fileobj=io.BytesIO(), mode='w'))
            for _ in range(2):                                                  # a second apply replaces
                self.assertIn('installed /Applications/BTApp.app (3 files', self._run(tw, plan, t + '/good'))
            self.assertEqual(plan.new_files['Applications/BTApp.app/BTApp'][:3], (0, 0, 0o755))
            self.assertEqual(plan.new_files['Applications/BTApp.app/Info.plist'][:3], (0, 0, 0o644))
            self.assertEqual(plan.new_files['Applications/BTApp.app/_CodeSignature/CodeResources'][:3], (0, 0, 0o644))
            self.assertEqual(plan.new_files['Applications/BTApp.app/BTApp'][3], rd(t + '/good/BTApp'))
            self.assertEqual(os.listdir(t + '/R/Applications'), ['Preferences.app'])     # Plan leaves SRC untouched
            print('RESULT: PASS root-tweak-btapp')

    def test_real_bundle(self):
        # the bundle bt-client/build-app.sh produced (path in PIXOS_BTAPP_REAL_BUNDLE): the checker must accept real rcodesign output
        real = os.environ.get('PIXOS_BTAPP_REAL_BUNDLE')
        if not real or not os.path.isdir(real):
            print('SKIP root-tweak-btapp-real: PIXOS_BTAPP_REAL_BUNDLE unset or missing')
            return
        tw = WifidProbeTweak._tw(self)
        files = tw.check_btapp_bundle(real)
        self.assertEqual(sorted(r for r, _, _ in files), ['BTApp', 'Info.plist', '_CodeSignature/CodeResources'])
        print('RESULT: PASS root-tweak-btapp-real')


class PingTweak(unittest.TestCase):
    """T-847c: the pixos-ping one-shot job is added only when PIXOS_PING_BIN names a signed arm64 binary and
    PIXOS_PING_TARGET an IPv4 literal; the default root is unchanged; the address never reaches the output."""

    TARGET = '192.0.2.77'
    KEYS = ('PIXOS_PING_BIN', 'PIXOS_PING_TARGET', 'PIXOS_PING_COUNT', 'PIXOS_PING_INTERVAL_MS', 'PIXOS_PING_TIMEOUT_MS',
            'PIXOS_PING_WAIT_S')

    def _run(self, tw, fs, **env):
        out, old, saved = io.StringIO(), sys.stdout, {k: os.environ.pop(k, None) for k in self.KEYS}
        os.environ.update(env)
        sys.stdout = out
        try:
            tw.tweak_ping(fs)
        finally:
            sys.stdout = old
            for k in self.KEYS:
                os.environ.pop(k, None)
                if saved[k] is not None:
                    os.environ[k] = saved[k]
        return out.getvalue()

    def test_gated_install_and_refusals(self):
        import plistlib
        helper = WifidProbeTweak()
        tw = helper._tw()
        with tempfile.TemporaryDirectory() as t:
            helper._root(t + '/R')
            before = rd(t + '/R/System/Library/xpc/launchd.plist')
            self.assertIn('not installed', self._run(tw, tw.Live(t + '/R')))                  # default: unchanged, and says so
            self.assertEqual(before, rd(t + '/R/System/Library/xpc/launchd.plist'))
            self.assertFalse(os.path.exists(t + '/R/usr/local/bin/pixos-ping'))
            good = helper._macho(None)                                                       # signed, no entitlements needed
            for name, blob in (('unsigned', helper._macho(signed=False)), ('notmacho', b'#!/bin/sh\n')):
                with open(f'{t}/{name}', 'wb') as f:
                    f.write(blob)
                with self.assertRaises(RuntimeError, msg=name) as cm:
                    self._run(tw, tw.Live(t + '/R'), PIXOS_PING_BIN=f'{t}/{name}', PIXOS_PING_TARGET=self.TARGET)
                self.assertIn('refused', str(cm.exception))
            with open(t + '/good', 'wb') as f:
                f.write(good)
            for bad in ('', 'gateway.local', '192.0.2', '192.0.2.256', '::1', '192.0.2.01'):    # target missing or not a dotted IPv4
                with self.assertRaises(RuntimeError, msg=bad):
                    self._run(tw, tw.Live(t + '/R'), PIXOS_PING_BIN=t + '/good', PIXOS_PING_TARGET=bad)
            for knob, v in (('PIXOS_PING_COUNT', '0'), ('PIXOS_PING_COUNT', '101'), ('PIXOS_PING_WAIT_S', 'x'),
                            ('PIXOS_PING_INTERVAL_MS', '5')):
                with self.assertRaises(RuntimeError, msg=knob):
                    self._run(tw, tw.Live(t + '/R'), PIXOS_PING_BIN=t + '/good', PIXOS_PING_TARGET=self.TARGET, **{knob: v})
            self.assertEqual(before, rd(t + '/R/System/Library/xpc/launchd.plist'))        # refusals wrote nothing
            plan = tw.Plan(t + '/R', tarfile.open(fileobj=io.BytesIO(), mode='w'))
            msg = self._run(tw, plan, PIXOS_PING_BIN=t + '/good', PIXOS_PING_TARGET=self.TARGET, PIXOS_PING_COUNT='3')
            self.assertIn('installed', msg)
            self.assertNotIn(self.TARGET, msg)                                               # the address is a private value
            for _ in range(2):                                                               # a second apply replaces, never duplicates
                self._run(tw, plan, PIXOS_PING_BIN=t + '/good', PIXOS_PING_TARGET=self.TARGET, PIXOS_PING_COUNT='3')
            jobs = plistlib.loads(plan.read('System/Library/xpc/launchd.plist'))['LaunchDaemons']
            job = [j for j in jobs.values() if j['Label'] == 'com.pixos.pixos-ping']
            self.assertEqual(len(job), 1)
            self.assertEqual(job[0]['ProgramArguments'], ['/usr/local/bin/pixos-ping'])
            self.assertTrue(job[0]['RunAtLoad'])
            self.assertNotIn('KeepAlive', job[0])
            self.assertNotIn('StandardOutPath', job[0])
            self.assertEqual(job[0]['EnvironmentVariables'], {'PIXOS_PING_TARGET': self.TARGET, 'PIXOS_PING_COUNT': '3'})
            self.assertEqual({j['Label'] for j in jobs.values()} - {'com.pixos.pixos-ping'}, {'com.apple.keep'})
            uid, gid, mode, data = plan.new_files['usr/local/bin/pixos-ping']
            self.assertEqual((uid, gid, mode), (0, 0, 0o755))
            self.assertEqual(data, good)
            self.assertEqual(before, rd(t + '/R/System/Library/xpc/launchd.plist'))        # Plan leaves SRC untouched
            print('RESULT: PASS root-tweak-ping')

    def test_real_ldid_binary(self):
        # The binary build-ios.sh produced (path in PIXOS_PING_REAL_BIN): the checker must accept real ldid output.
        real = os.environ.get('PIXOS_PING_REAL_BIN')
        if not real or not os.path.exists(real):
            print('SKIP root-tweak-ping-real: PIXOS_PING_REAL_BIN unset or missing (no iOS build to check)')
            return
        tw = WifidProbeTweak()._tw()
        tw.check_ping_binary(rd(real))
        print('RESULT: PASS root-tweak-ping-real-binary')


class BtConfigTweak(unittest.TestCase):
    """sprint-bt: PIXOS_BT_PLATFORM_CONFIG=1 writes LE/SupportsMultipleAdvertising=false into bluetoothd's debug defaults domain
    (mobile-owned, 0600), keeps other keys of an existing file, and leaves the root unchanged when unset."""

    def test_gated_write(self):
        import plistlib
        tw = WifidProbeTweak._tw(self)
        path = 'private/var/mobile/Library/Preferences/com.apple.MobileBluetooth.debug.plist'
        chown = tw.os.chown
        tw.os.chown = lambda *a: None      # mobile ownership needs root; recorded below instead
        with tempfile.TemporaryDirectory() as t:
            fs = tw.Live(t)
            saved = os.environ.pop(tw.BTCONF_ENV, None)
            try:
                out = io.StringIO(); old = sys.stdout; sys.stdout = out
                try:
                    tw.tweak_btconfig(fs)
                finally:
                    sys.stdout = old
                self.assertFalse(fs.exists(path))
                self.assertTrue('not written' in out.getvalue())
                os.makedirs(os.path.dirname(os.path.join(t, path)))
                with open(os.path.join(t, path), 'wb') as f:
                    f.write(plistlib.dumps({'LE': {'Other': 1}, 'Stack': {'X': True}}, fmt=plistlib.FMT_BINARY))
                os.environ[tw.BTCONF_ENV] = '1'
                out = io.StringIO(); sys.stdout = out
                try:
                    tw.tweak_btconfig(fs)
                finally:
                    sys.stdout = old
                pl = plistlib.loads(fs.read(path))
                self.assertIs(pl['LE']['SupportsMultipleAdvertising'], False)
                self.assertEqual(pl['LE']['Other'], 1)
                self.assertIs(pl['Stack']['X'], True)
                self.assertIn('LE/SupportsMultipleAdvertising=False', out.getvalue())
                self.assertEqual(os.stat(os.path.join(t, path)).st_mode & 0o777, 0o600)
            finally:
                tw.os.chown = chown
                os.environ.pop(tw.BTCONF_ENV, None)
                if saved is not None:
                    os.environ[tw.BTCONF_ENV] = saved
        print('RESULT: PASS root-tweak-btconfig')

    def test_default_root_marker(self):
        """A missing setting is only the measured default when BTBW_DEFAULT_ROOT=1 says so; otherwise the build prints NOT the default."""
        tw = WifidProbeTweak._tw(self)
        path = 'private/var/mobile/Library/Preferences/com.apple.MobileBluetooth.debug.plist'
        tw.os.chown = lambda *a: None
        saved = {k: os.environ.pop(k, None) for k in (tw.BTCONF_ENV, tw.BTCONF_DEFAULT)}
        try:
            with tempfile.TemporaryDirectory() as t:
                fs = tw.Live(t)
                out = io.StringIO(); old = sys.stdout; sys.stdout = out
                try:
                    tw.tweak_btconfig(fs)
                    os.environ[tw.BTCONF_DEFAULT] = '1'
                    tw.tweak_btconfig(fs)
                    os.environ[tw.BTCONF_DEFAULT] = '0'
                    tw.tweak_btconfig(fs)
                finally:
                    sys.stdout = old
                lines = out.getvalue().splitlines()
                self.assertEqual(len(lines), 3)
                self.assertNotIn(': measured default', lines[0])
                self.assertIn(': measured default root', lines[1])
                self.assertNotIn(': measured default', lines[2])
                self.assertFalse(fs.exists(path))
        finally:
            for k, v in saved.items():
                os.environ.pop(k, None)
                if v is not None:
                    os.environ[k] = v
        print('RESULT: PASS root-tweak-btconfig-default')


class CoreAudioTraceTweak(unittest.TestCase):
    """PIXOS_COREAUDIO_TRACE=1: four trace keys = 'minutiae' in mobile's and the any-user com.apple.coreaudio plists (merged), plus a
    persisted-Debug logging profile built on the stock coreaudio profile; unset writes nothing."""

    def test_gated_write(self):
        import plistlib
        tw = WifidProbeTweak._tw(self)
        chown = tw.os.chown
        tw.os.chown = lambda *a: None
        saved = os.environ.pop(tw.CATRACE_ENV, None)
        old = sys.stdout
        try:
            with tempfile.TemporaryDirectory() as t:
                fs = tw.Live(t)
                out = sys.stdout = io.StringIO()
                tw.tweak_coreaudio_trace(fs)
                sys.stdout = old
                self.assertIn('not written', out.getvalue())
                for p in (tw.CATRACE_MOBILE, tw.CATRACE_ANYUSER, tw.CATRACE_LOGPROFILE):
                    self.assertFalse(fs.exists(p))
                for p in (os.path.dirname(tw.CATRACE_MOBILE), os.path.dirname(tw.CATRACE_LOGPROFILE), os.path.dirname(tw.CATRACE_LOGSTOCK)):
                    os.makedirs(os.path.join(t, p))
                with open(os.path.join(t, tw.CATRACE_MOBILE), 'wb') as f:
                    f.write(plistlib.dumps({'Other': 1}, fmt=plistlib.FMT_BINARY))
                stock = {'DEFAULT-OPTIONS': {'Default-Privacy-Setting': 'Public'}, 'aura': {'Enable-Oversize-Messages': True}}
                with open(os.path.join(t, tw.CATRACE_LOGSTOCK), 'wb') as f:
                    f.write(plistlib.dumps(stock))
                os.environ[tw.CATRACE_ENV] = '1'
                out = sys.stdout = io.StringIO()
                tw.tweak_coreaudio_trace(fs)
                sys.stdout = old
                for p in (tw.CATRACE_MOBILE, tw.CATRACE_ANYUSER):
                    pl = plistlib.loads(fs.read(p))
                    for k in ('traceroutingmanager', 'traceportmanager', 'traceroutinghandler', 'tracerouting'):
                        self.assertEqual(pl[k], 'minutiae')
                self.assertEqual(plistlib.loads(fs.read(tw.CATRACE_MOBILE))['Other'], 1)
                self.assertEqual(os.stat(os.path.join(t, tw.CATRACE_MOBILE)).st_mode & 0o777, 0o600)
                self.assertEqual(os.stat(os.path.join(t, tw.CATRACE_ANYUSER)).st_mode & 0o777, 0o644)
                prof = plistlib.loads(fs.read(tw.CATRACE_LOGPROFILE))
                self.assertEqual(prof['DEFAULT-OPTIONS']['Level'], {'Enable': 'Debug', 'Persist': 'Debug'})
                self.assertEqual(prof['DEFAULT-OPTIONS']['Default-Privacy-Setting'], 'Public')
                self.assertTrue(prof['aura']['Enable-Oversize-Messages'])
                self.assertIn('minutiae', out.getvalue())
        finally:
            sys.stdout = old
            tw.os.chown = chown
            os.environ.pop(tw.CATRACE_ENV, None)
            if saved is not None:
                os.environ[tw.CATRACE_ENV] = saved
        print('RESULT: PASS root-tweak-coreaudio-trace')


if __name__ == '__main__':
    unittest.main()


class BlueToolTweak(unittest.TestCase):
    """PIXOS_BLUETOOL_BIN installs pixos-bluetool (0755 root:wheel) and changes only the com.apple.BlueTool job's ProgramArguments;
    a missing job, non-stock ProgramArguments or an unsigned binary is refused before anything is written; unset = no change."""

    STOCK = {'Label': 'com.apple.BlueTool', 'ProgramArguments': ['/usr/sbin/BlueTool', '-R'], 'UserName': 'mobile',
             'MachServices': {'com.apple.BlueTool': True}}

    def _setup(self, t, jobs):
        import plistlib
        os.makedirs(t + '/R/System/Library/xpc')
        with open(t + '/R/System/Library/xpc/launchd.plist', 'wb') as f:
            f.write(plistlib.dumps({'LaunchDaemons': jobs}, fmt=plistlib.FMT_BINARY))

    def _run(self, tw, t, binpath):
        saved = os.environ.pop(tw.BLUETOOL_ENV, None)
        if binpath:
            os.environ[tw.BLUETOOL_ENV] = binpath
        out, old = io.StringIO(), sys.stdout
        sys.stdout = out
        try:
            tw.tweak_bluetool(tw.Live(t + '/R'))
        finally:
            sys.stdout = old
            os.environ.pop(tw.BLUETOOL_ENV, None)
            if saved is not None:
                os.environ[tw.BLUETOOL_ENV] = saved
        return out.getvalue()

    def _jobs(self, t):
        import plistlib
        return plistlib.loads(open(t + '/R/System/Library/xpc/launchd.plist', 'rb').read())['LaunchDaemons']

    def test_install_refusals_unset(self):
        tw = WifidProbeTweak._tw(self)
        macho = WifidProbeTweak._macho(self, WifidProbeTweak.ENTS)
        chown = tw.os.chown
        tw.os.chown = lambda *a: None
        try:
            with tempfile.TemporaryDirectory() as t:
                binp = t + '/bin'
                open(binp, 'wb').write(macho)
                other = {'Label': 'com.apple.keep', 'ProgramArguments': ['/x']}
                # unset = no change
                self._setup(t, {'/a/bt.plist': dict(self.STOCK), '/a/k.plist': other})
                raw = open(t + '/R/System/Library/xpc/launchd.plist', 'rb').read()
                self.assertIn('unset', self._run(tw, t, None))
                self.assertEqual(open(t + '/R/System/Library/xpc/launchd.plist', 'rb').read(), raw)
                self.assertFalse(os.path.exists(t + '/R/usr/local/libexec/pixos-bluetool'))
                # install
                out = self._run(tw, t, binp)
                jobs = self._jobs(t)
                self.assertEqual(jobs['/a/bt.plist'], dict(self.STOCK, ProgramArguments=['/usr/local/libexec/pixos-bluetool']))
                self.assertEqual(jobs['/a/k.plist'], other)
                self.assertEqual(open(t + '/R/usr/local/libexec/pixos-bluetool', 'rb').read(), macho)
                self.assertEqual(os.stat(t + '/R/usr/local/libexec/pixos-bluetool').st_mode & 0o777, 0o755)
                self.assertIn('installed', out)
            bad = [('nojob', {'/a/k.plist': other}, macho, 'found 0'),
                   ('twojobs', {'/a/1': dict(self.STOCK), '/a/2': dict(self.STOCK)}, macho, 'found 2'),
                   ('nonstock', {'/a/bt.plist': dict(self.STOCK, ProgramArguments=['/usr/sbin/BlueTool'])}, macho, 'not the stock'),
                   ('unsigned', {'/a/bt.plist': dict(self.STOCK)}, WifidProbeTweak._macho(self, signed=False), 'no code signature'),
                   ('notmacho', {'/a/bt.plist': dict(self.STOCK)}, b'junk' * 16, 'not an arm64 Mach-O')]
            for name, jobs, blob, why in bad:
                with tempfile.TemporaryDirectory() as t:
                    open(t + '/bin', 'wb').write(blob)
                    self._setup(t, jobs)
                    raw = open(t + '/R/System/Library/xpc/launchd.plist', 'rb').read()
                    with self.assertRaises(RuntimeError, msg=name) as cm:
                        self._run(tw, t, t + '/bin')
                    self.assertIn(why, str(cm.exception), name)
                    self.assertEqual(open(t + '/R/System/Library/xpc/launchd.plist', 'rb').read(), raw, name)
                    self.assertFalse(os.path.exists(t + '/R/usr/local/libexec/pixos-bluetool'), name)
        finally:
            tw.os.chown = chown
        print('RESULT: PASS root-tweak-bluetool')


class DroppedJobs(unittest.TestCase):
    """tweak_launchd removes the DROPPED_JOBS labels and keeps the rest (B-A5: bluetoothd, kernel docs/research/44)."""
