#!/usr/bin/env python3
"""sweet-root-tweaks.py: make an iOS 16.7.16 (20H392) root tree reach the home screen on the phone.

  sweet-root-tweaks.py ROOT             edit the mounted root ROOT in place (needs root for chown)
  sweet-root-tweaks.py --plan SRC OUT   leave SRC untouched; write OUT/overlay.tar and OUT/ops.sh (no root needed).
                                        Consumer: run ops.sh with R=<target root>, then
                                        tar -x -p --numeric-owner -C "$R" -f overlay.tar

Both modes run the same tweaks (apply) over a small file-system object: Live edits a directory, Plan records
what Live would do. Paths given to the object are relative to the root.
"""
import fnmatch, io, ipaddress, os, plistlib, shutil, subprocess, sys, tarfile, tempfile

BUILD = '20H392'
VERSION = '16.7.16'
MOBILE = 501                      # uid and gid of the user "mobile"

USAGE = __doc__


def _join(a, b):
    return a + '/' + b if a else b


class FS:
    """Shared part: glob and walk are built on listdir/isdir, so the guard of Plan covers them."""

    def glob(self, pattern):
        """Root-relative paths matching PATTERN (fnmatch per component, sorted)."""
        found = ['']
        for comp in pattern.split('/'):
            nxt = []
            for base in found:
                if not any(c in comp for c in '*?['):
                    p = _join(base, comp)
                    if self.exists(p):
                        nxt.append(p)
                    continue
                if not self.isdir(base):
                    continue
                for name in self.listdir(base):
                    if fnmatch.fnmatchcase(name, comp):
                        p = _join(base, name)
                        self._touch_check(p)
                        nxt.append(p)
            found = nxt
        return found

    def _touch_check(self, path):
        pass

    def walk(self, path):
        """Like os.walk top-down, with sorted names and root-relative dirpaths."""
        names = self.listdir(path)
        dirs = [n for n in names if self.isdir(_join(path, n))]
        files = [n for n in names if n not in dirs]
        yield path, dirs, files
        for d in dirs:
            if not self.islink(_join(path, d)):     # like os.walk: a linked dir is listed, not entered
                yield from self.walk(_join(path, d))


class Live(FS):
    """Edits the tree under ROOT."""

    def __init__(self, root):
        self.root = root

    def _p(self, path):
        return os.path.join(self.root, path) if path else self.root

    def exists(self, path):
        return os.path.lexists(self._p(path))

    def isdir(self, path):
        return os.path.isdir(self._p(path))

    def islink(self, path):
        return os.path.islink(self._p(path))

    def listdir(self, path):
        return sorted(os.listdir(self._p(path)))

    def walk(self, path):
        for dp, dn, fn in os.walk(self._p(path)):
            dn.sort()
            rel = os.path.relpath(dp, self.root)
            yield ('' if rel == '.' else rel), dn, sorted(fn)

    def stat(self, path):
        st = os.lstat(self._p(path))
        return st.st_uid, st.st_gid, st.st_mode & 0o7777

    def read(self, path):
        with open(self._p(path), 'rb') as f:
            return f.read()

    def write(self, path, data, uid, gid, mode):
        p = self._p(path)
        if os.path.islink(p):
            raise RuntimeError(f'{path}: is a symlink, not writing through it')
        with open(p, 'wb') as f:
            f.write(data)
        os.chown(p, uid, gid)
        os.chmod(p, mode)

    def makedirs(self, path):
        os.makedirs(self._p(path), exist_ok=True)

    def touch(self, path):
        with open(self._p(path), 'ab'):
            pass

    def chown(self, path, uid, gid):
        os.chown(self._p(path), uid, gid)

    def chmod(self, path, mode):
        os.chmod(self._p(path), mode)

    def move(self, old, new):
        shutil.move(self._p(old), self._p(new))

    def heic_to_png(self, heic, png):
        dst = self._p(png)
        subprocess.run(['heif-convert', self._p(heic), dst], check=True, stdout=subprocess.DEVNULL)
        os.chown(dst, 0, 0)
        os.chmod(dst, 0o644)


class Plan(FS):
    """Records the commands (self.ops) and file contents (into TAR) that make SRC look like Live's result.
    SRC is only read. guard: root-relative paths that a later build step adds; looking at them raises."""

    def __init__(self, src, tar, guard=()):
        self.src, self.tar = src, tar
        self.guard = {g.strip('/') for g in guard}
        self.ops = []
        self.new_files = {}             # path -> (uid, gid, mode, data): the tar entries
        self.added_dirs = set()         # directories made by mkdir -p
        self.added_other = set()        # touched or moved-in non-directories
        self.gone = set()               # source paths removed or moved away
        self.meta = {}                  # path -> [uid, gid, mode] after chown/chmod

    # -- recording helpers (also used by the caller) --
    @staticmethod
    def q(path):
        return '"$R"/\'' + path.replace("'", "'\\''") + "'"

    def sh(self, *parts):
        self.ops.append(' '.join(parts))

    # -- guard --
    def _guard(self, path, mode='exists'):
        path = path.strip('/')
        for g in self.guard:
            hit = path == g or path.startswith(g + '/')
            if mode == 'list':
                hit = hit or os.path.dirname(g) == path
            elif mode == 'walk':
                hit = hit or path == '' or g.startswith(path + '/')
            if hit:
                raise RuntimeError(f'{path or "/"}: a later step changes {g}')

    def _touch_check(self, path):
        self._guard(path)

    # -- unguarded state (what the final tree holds) --
    def _planned(self, p):
        return p in self.new_files or p in self.added_dirs or p in self.added_other

    def _gone(self, p):
        while p:
            if p in self.gone:
                return True
            p = os.path.dirname(p)
        return False

    def _has(self, p):
        if p == '' or self._planned(p):
            return True
        return not self._gone(p) and os.path.lexists(os.path.join(self.src, p))

    def _isdir(self, p):
        if p == '' or p in self.added_dirs:
            return True
        if p in self.new_files or p in self.added_other or self._gone(p):
            return False
        return os.path.isdir(os.path.join(self.src, p))

    def _names(self, p):
        names = set()
        if not self._gone(p) and os.path.isdir(os.path.join(self.src, p)):
            names.update(n for n in os.listdir(os.path.join(self.src, p))
                         if self._planned(_join(p, n)) or not self._gone(_join(p, n)))
        for s in (self.new_files, self.added_dirs, self.added_other):
            names.update(os.path.basename(x) for x in s if x and os.path.dirname(x) == p)
        return sorted(names)

    def _stat(self, p):
        if p in self.meta:
            return tuple(self.meta[p])
        if p in self.new_files:
            u, g, m, _ = self.new_files[p]
            return u, g, m
        if p in self.added_dirs:
            return 0, 0, 0o755
        if p in self.added_other:
            return 0, 0, 0o644
        st = os.lstat(os.path.join(self.src, p))
        return st.st_uid, st.st_gid, st.st_mode & 0o7777

    # -- queries (guarded) --
    def exists(self, path):
        self._guard(path)
        return self._has(path.strip('/'))

    def isdir(self, path):
        self._guard(path)
        return self._isdir(path.strip('/'))

    def islink(self, path):
        p = path.strip('/')
        self._guard(p)
        if self._planned(p) or self._gone(p):
            return False
        return os.path.islink(os.path.join(self.src, p))

    def listdir(self, path):
        self._guard(path, 'list')
        p = path.strip('/')
        if not self._isdir(p):
            raise FileNotFoundError(p)
        return self._names(p)

    def walk(self, path):
        self._guard(path, 'walk')
        yield from FS.walk(self, path.strip('/'))

    def stat(self, path):
        self._guard(path)
        p = path.strip('/')
        if not self._has(p):
            raise FileNotFoundError(p)
        return self._stat(p)

    def read(self, path):
        self._guard(path)
        p = path.strip('/')
        if p in self.new_files:
            return self.new_files[p][3]
        if not self._has(p) or p in self.added_dirs or p in self.added_other:
            raise FileNotFoundError(p)
        with open(os.path.join(self.src, p), 'rb') as f:
            return f.read()

    # -- changes (recorded) --
    def write(self, path, data, uid, gid, mode):
        self._guard(path)
        p = path.strip('/')
        if not self._isdir(os.path.dirname(p)):
            raise FileNotFoundError(os.path.dirname(p))
        if p not in self.new_files and p not in self.added_other and self._has(p):
            if os.path.islink(os.path.join(self.src, p)):
                raise RuntimeError(f'{path}: is a symlink, not writing through it')
            if os.lstat(os.path.join(self.src, p)).st_nlink > 1:
                raise RuntimeError(f'{p}: has several hard links, an in-place write would change all of them')
            self.sh('rm -f', self.q(p))
        info = tarfile.TarInfo(p)
        info.size, info.uid, info.gid, info.mode = len(data), uid, gid, mode
        info.mtime = 0
        self.tar.addfile(info, io.BytesIO(data))
        self.new_files[p] = (uid, gid, mode, data)
        self.added_other.discard(p)
        self.gone.discard(p)
        self.meta.pop(p, None)

    def makedirs(self, path):
        self._guard(path)
        p = path.strip('/')
        if self._has(p):
            return
        self.sh('mkdir -p', self.q(p))
        while p and not self._has(p):
            self.added_dirs.add(p)
            p = os.path.dirname(p)

    def touch(self, path):
        self._guard(path)
        p = path.strip('/')
        if self._has(p):
            return
        self.sh('touch', self.q(p))
        self.added_other.add(p)

    def _no_overlay(self, p):
        if p in self.new_files:
            raise RuntimeError(f'{p}: already in the overlay tar, give the final owner/mode to write()')

    def chown(self, path, uid, gid):
        self._guard(path)
        p = path.strip('/')
        self._no_overlay(p)
        self.sh('chown', f'{uid}:{gid}', self.q(p))
        self.meta[p] = [uid, gid, self._stat(p)[2]]

    def chmod(self, path, mode):
        self._guard(path)
        p = path.strip('/')
        self._no_overlay(p)
        self.sh('chmod', '%o' % mode, self.q(p))
        self.meta[p] = [*self._stat(p)[:2], mode]

    def move(self, old, new):
        self._guard(old)
        self._guard(new)
        o, n = old.strip('/'), new.strip('/')
        self._no_overlay(o)
        isdir = self._isdir(o)
        self.sh('mv', self.q(o), self.q(n))
        for s in (self.added_dirs, self.added_other):
            s.discard(o)
        self.gone.add(o)
        self.gone.discard(n)
        (self.added_dirs if isdir else self.added_other).add(n)

    def heic_to_png(self, heic, png):
        with tempfile.TemporaryDirectory() as t:
            out = os.path.join(t, 'out.png')
            subprocess.run(['heif-convert', os.path.join(self.src, heic.strip('/')), out], check=True,
                           stdout=subprocess.DEVNULL)
            with open(out, 'rb') as f:
                data = f.read()
        self.write(png, data, 0, 0, 0o644)


def write_plan(plan, outdir):
    with open(os.path.join(outdir, 'ops.sh'), 'w') as f:
        f.write(''.join(op + '\n' for op in plan.ops))


# ---- tweaks ----

def edit_plist(fs, path, fn, fmt=plistlib.FMT_BINARY):
    """Load PATH, let FN change the dict in place, write it back with the same owner and mode."""
    uid, gid, mode = fs.stat(path)
    data = plistlib.loads(fs.read(path))
    fn(data)
    fs.write(path, plistlib.dumps(data, fmt=fmt), uid, gid, mode)


DROPPED_JOBS = {'com.apple.CommCenter', 'com.apple.fairplayd.H2', 'com.apple.voicemail.vmd', 'com.apple.ReportCrash',
                'com.apple.spindump', 'com.apple.tailspind', 'com.apple.aggregated', 'com.apple.aggregated.addaily'}


def tweak_launchd(fs):
    # Panic-on-crash would reboot the phone on the first crashing daemon; baseband-less CommCenter loops, fairplayd
    # relaunches forever (blocks locationd), vmd retries a missing service, the diagnostics daemons cost CPU; (bluetoothd was dropped here until the BT port: B-A5, relaunched
    # every ~10 s: 138x in gputest67, kernel docs/research/44) and cannot run without a BT stack (B-A5; re-add it with the BT port);
    # backboardd needs CA_BACKDROP_SCALE or blurs break on the CPU renderer.
    counts = {'renamed': 0, 'removed': 0, 'backboardd': 0}

    def fix(pl):
        jobs = pl['LaunchDaemons']
        for key in [k for k, j in jobs.items() if j.get('Label') in DROPPED_JOBS]:
            del jobs[key]
            counts['removed'] += 1
        for job in jobs.values():
            if '_PanicOnCrash' in job:
                job['_PanicOnCrasX'] = job.pop('_PanicOnCrash')
                counts['renamed'] += 1
            if job.get('Label') == 'com.apple.backboardd':
                job.setdefault('EnvironmentVariables', {})['CA_BACKDROP_SCALE'] = '1'
                counts['backboardd'] += 1
    edit_plist(fs, 'System/Library/xpc/launchd.plist', fix)
    print('launchd.plist: %(renamed)d _PanicOnCrash renamed, %(removed)d jobs removed, %(backboardd)d backboardd patched' % counts)


WIFID_PROBE_ENV = 'PIXOS_WIFID_PROBE'          # path of a signed arm64 wifid-probe binary; unset = default root unchanged
WIFID_PROBE_PATH = 'usr/local/bin/wifid-probe'
WIFID_PROBE_LABEL = 'com.pixos.wifid-probe'
WIFID_PROBE_JOB = '/System/Library/LaunchDaemons/' + WIFID_PROBE_LABEL + '.plist'


def check_probe_binary(data):
    """Refuse (ValueError, with the reason) a wifid-probe that the kernel would run without wifid's keychain rights:
    it must be an arm64 Mach-O whose code signature carries an XML entitlements blob (special slot 5; patch 0035
    grants nothing for DER-only or missing blobs) naming application-identifier com.apple.wifid,
    com.apple.private.system-keychain and a non-empty keychain-access-groups (X-W5 s.3)."""
    if len(data) < 32 or data[:4] != b'\xcf\xfa\xed\xfe' or int.from_bytes(data[4:8], 'little') != 0x0100000c:
        raise ValueError('not an arm64 Mach-O')
    ncmds = int.from_bytes(data[16:20], 'little')
    off, sig = 32, None
    for _ in range(ncmds):
        cmd, size = int.from_bytes(data[off:off + 4], 'little'), int.from_bytes(data[off + 4:off + 8], 'little')
        if size < 8:
            raise ValueError('corrupt load command')
        if cmd == 0x1d:                                    # LC_CODE_SIGNATURE
            sig = (int.from_bytes(data[off + 8:off + 12], 'little'), int.from_bytes(data[off + 12:off + 16], 'little'))
        off += size
    if sig is None:
        raise ValueError('no code signature (the keychain refuses it with -34018)')
    blob = data[sig[0]:sig[0] + sig[1]]
    if blob[:4] != b'\xfa\xde\x0c\xc0':
        raise ValueError('code signature is not a SuperBlob')
    xml = None
    for i in range(int.from_bytes(blob[8:12], 'big')):
        typ, boff = int.from_bytes(blob[12 + 8 * i:16 + 8 * i], 'big'), int.from_bytes(blob[16 + 8 * i:20 + 8 * i], 'big')
        if typ == 5:                                       # entitlements, XML
            if blob[boff:boff + 4] != b'\xfa\xde\x71\x71':
                raise ValueError('entitlements slot is not an XML entitlements blob')
            xml = blob[boff + 8:boff + int.from_bytes(blob[boff + 4:boff + 8], 'big')]
    if xml is None:
        raise ValueError('no XML entitlements blob in the signature (DER-only gets none, patch 0035)')
    ents = plistlib.loads(xml)
    if ents.get('application-identifier') != 'com.apple.wifid':
        raise ValueError('application-identifier is not com.apple.wifid')
    if ents.get('com.apple.private.system-keychain') is not True:
        raise ValueError('com.apple.private.system-keychain is not true')
    if not ents.get('keychain-access-groups'):
        raise ValueError('keychain-access-groups missing or empty')


def tweak_wifid_probe(fs):
    # One-shot launchd job running wifid-probe (X-W5 s.5): only when PIXOS_WIFID_PROBE names a signed binary.
    src = os.environ.get(WIFID_PROBE_ENV)
    if not src:
        print(f'wifid-probe: not installed ({WIFID_PROBE_ENV} unset), default root unchanged')
        return
    with open(src, 'rb') as f:
        data = f.read()
    try:
        check_probe_binary(data)
    except ValueError as e:
        raise RuntimeError(f'wifid-probe: {src} refused: {e}') from None
    fs.makedirs(os.path.dirname(WIFID_PROBE_PATH))
    fs.write(WIFID_PROBE_PATH, data, 0, 0, 0o755)
    # No StandardOutPath/StandardErrorPath (T-99f8): launchd's default is /dev/null, and xnu patch 0048 copies what the processes named in boot-arg
    # `sweetnull` write to /dev/null into the host log as "|name| text". The run adds `wifid-probe` to it. With /dev/console (T-bd9e, 2026-11-07,
    # run 20261107T041006Z-wlan-runW4-list) the probe ran but none of its text reached the host log [observed].
    job = {'Label': WIFID_PROBE_LABEL, 'ProgramArguments': ['/' + WIFID_PROBE_PATH], 'RunAtLoad': True,
           'EnvironmentVariables': {'OS_ACTIVITY_DT_MODE': 'YES'}}     # no KeepAlive: one shot

    def add(pl):
        jobs = pl['LaunchDaemons']
        for key in [k for k, j in jobs.items() if j.get('Label') == WIFID_PROBE_LABEL]:
            del jobs[key]
        jobs[WIFID_PROBE_JOB] = job
    edit_plist(fs, 'System/Library/xpc/launchd.plist', add)
    print(f'wifid-probe: installed /{WIFID_PROBE_PATH} + job {WIFID_PROBE_LABEL} (RunAtLoad, one shot)')


BTAPP_ENV = 'PIXOS_BTAPP_BUNDLE'              # path of a signed BTApp.app directory (bt-client/build-app.sh); unset = default root unchanged
BTAPP_ID = 'com.pixos.btapp'
BTAPP_DIR = 'Applications/BTApp.app'
BTAPP_ENTS = {'application-identifier': BTAPP_ID, 'com.apple.bluetooth.system': True,     # sprint-bt: bluetoothd MBFramework session gate (BluetoothManager)
              'com.apple.private.tcc.allow': ['kTCCServiceBluetoothAlways']}


def macho_signature(data):
    """(entitlements dict or None, CodeDirectory identifier or None) of a signed arm64 Mach-O; ValueError when it is not one."""
    if len(data) < 32 or data[:4] != b'\xcf\xfa\xed\xfe' or int.from_bytes(data[4:8], 'little') != 0x0100000c:
        raise ValueError('not an arm64 Mach-O')
    off, sig = 32, None
    for _ in range(int.from_bytes(data[16:20], 'little')):
        cmd, size = int.from_bytes(data[off:off + 4], 'little'), int.from_bytes(data[off + 4:off + 8], 'little')
        if size < 8:
            raise ValueError('corrupt load command')
        if cmd == 0x1d:                                    # LC_CODE_SIGNATURE
            sig = (int.from_bytes(data[off + 8:off + 12], 'little'), int.from_bytes(data[off + 12:off + 16], 'little'))
        off += size
    if sig is None:
        raise ValueError('no code signature')
    blob = data[sig[0]:sig[0] + sig[1]]
    if blob[:4] != b'\xfa\xde\x0c\xc0':
        raise ValueError('code signature is not a SuperBlob')
    ents = ident = None
    for i in range(int.from_bytes(blob[8:12], 'big')):
        typ, boff = int.from_bytes(blob[12 + 8 * i:16 + 8 * i], 'big'), int.from_bytes(blob[16 + 8 * i:20 + 8 * i], 'big')
        if typ == 5 and blob[boff:boff + 4] == b'\xfa\xde\x71\x71':
            ents = plistlib.loads(blob[boff + 8:boff + int.from_bytes(blob[boff + 4:boff + 8], 'big')])
        if typ == 0 and blob[boff:boff + 4] == b'\xfa\xde\x0c\x02':
            io_ = boff + int.from_bytes(blob[boff + 20:boff + 24], 'big')
            ident = blob[io_:blob.index(b'\0', io_)].decode('ascii', 'replace')
    return ents, ident


def check_btapp_bundle(src):
    """Refuse (ValueError) a BTApp bundle that LaunchServices or bluetoothd would not treat as com.pixos.btapp: Info.plist
    CFBundleIdentifier (the one key the LS scan requires, T-9524 Q1.2.5) and CFBundleExecutable BTApp, the executable signed
    with EXACTLY BTAPP_ENTS (XML slot 5, patch 0035) and CodeDirectory identifier == the bundle id (bluetoothd keys its app map
    by the code-signing id, T-54b1 s1.2), a sealed bundle (_CodeSignature/CodeResources). Returns [(relpath, bytes, mode)]."""
    info = os.path.join(src, 'Info.plist')
    if not os.path.isfile(info):
        raise ValueError('no Info.plist')
    pl = plistlib.load(open(info, 'rb'))
    if pl.get('CFBundleIdentifier') != BTAPP_ID:
        raise ValueError(f'CFBundleIdentifier is {pl.get("CFBundleIdentifier")!r}, not {BTAPP_ID}')
    if pl.get('CFBundleExecutable') != 'BTApp':
        raise ValueError('CFBundleExecutable is not BTApp')
    if not os.path.isfile(os.path.join(src, '_CodeSignature/CodeResources')):
        raise ValueError('no _CodeSignature/CodeResources (sign the bundle, not only the executable)')
    with open(os.path.join(src, 'BTApp'), 'rb') as f:
        ents, ident = macho_signature(f.read())
    if ents != BTAPP_ENTS:
        raise ValueError(f'sealed entitlements {ents} != {BTAPP_ENTS}')
    if ident != BTAPP_ID:
        raise ValueError(f'CodeDirectory identifier {ident!r} != {BTAPP_ID}')
    files = []
    for dp, dirs, names in os.walk(src):
        dirs.sort()
        for d in dirs:  # review T-2d63 finding 2: os.walk lists a symlinked dir but never enters it (silently skipped)
            if os.path.islink(os.path.join(dp, d)):
                raise ValueError(f'{os.path.relpath(os.path.join(dp, d), src)} is a symlinked directory')
        for n in sorted(names):
            full = os.path.join(dp, n)
            if os.path.islink(full) or not os.path.isfile(full):
                raise ValueError(f'{os.path.relpath(full, src)} is not a regular file')
            rel = os.path.relpath(full, src)
            with open(full, 'rb') as f:
                files.append((rel, f.read(), 0o755 if rel == 'BTApp' else 0o644))
    return files


def tweak_btapp(fs):
    # T-2d63: the own foreground app, baked into /Applications so the first boot of the fresh userdata seeds it into
    # LaunchServices as a System app (T-9524 Q1.2-Q1.4; the dev overlay refuses /Applications, patch 0117). Its launcher
    # (btapp-launch) ships in the overlay. Owner 0:0 like the other root files [guess, research T-9524 recipe].
    src = os.environ.get(BTAPP_ENV)
    if not src:
        print(f'btapp: not installed ({BTAPP_ENV} unset), default root unchanged')
        return
    try:
        files = check_btapp_bundle(src)
    except (ValueError, OSError) as e:
        raise RuntimeError(f'btapp: {src} refused: {e}') from None
    if not fs.isdir('Applications'):
        raise RuntimeError('btapp: no Applications/ in the root (tweak_stock_apps runs first)')
    for rel, data, mode in files:
        fs.makedirs(os.path.dirname(f'{BTAPP_DIR}/{rel}'))
        fs.write(f'{BTAPP_DIR}/{rel}', data, 0, 0, mode)
    print(f'btapp: installed /{BTAPP_DIR} ({len(files)} files, {BTAPP_ID})')


PING_BIN_ENV = 'PIXOS_PING_BIN'                # path of a signed arm64 pixos-ping binary; unset = default root unchanged
PING_TARGET_ENV = 'PIXOS_PING_TARGET'          # IPv4 literal of the host to ping, written into the job env at root build time
PING_KNOBS = {'PIXOS_PING_COUNT': (1, 100), 'PIXOS_PING_INTERVAL_MS': (10, 10000), 'PIXOS_PING_TIMEOUT_MS': (10, 30000),
              'PIXOS_PING_WAIT_S': (0, 600)}   # optional, passed through to the job after a range check
PING_PATH = 'usr/local/bin/pixos-ping'
PING_LABEL = 'com.pixos.pixos-ping'
PING_JOB = '/System/Library/LaunchDaemons/' + PING_LABEL + '.plist'


def check_ping_binary(data):
    """Refuse (ValueError, with the reason) a pixos-ping that the kernel would not exec: an arm64 Mach-O with a code
    signature (SuperBlob with a CodeDirectory, slot 0; ad-hoc `ldid -Cadhoc -S` is enough, it needs no entitlements)."""
    if len(data) < 32 or data[:4] != b'\xcf\xfa\xed\xfe' or int.from_bytes(data[4:8], 'little') != 0x0100000c:
        raise ValueError('not an arm64 Mach-O')
    ncmds = int.from_bytes(data[16:20], 'little')
    off, sig = 32, None
    for _ in range(ncmds):
        cmd, size = int.from_bytes(data[off:off + 4], 'little'), int.from_bytes(data[off + 4:off + 8], 'little')
        if size < 8:
            raise ValueError('corrupt load command')
        if cmd == 0x1d:                                    # LC_CODE_SIGNATURE
            sig = (int.from_bytes(data[off + 8:off + 12], 'little'), int.from_bytes(data[off + 12:off + 16], 'little'))
        off += size
    if sig is None:
        raise ValueError('no code signature (sign it: ldid -Cadhoc -S)')
    blob = data[sig[0]:sig[0] + sig[1]]
    if blob[:4] != b'\xfa\xde\x0c\xc0':
        raise ValueError('code signature is not a SuperBlob')
    if 0 not in [int.from_bytes(blob[12 + 8 * i:16 + 8 * i], 'big') for i in range(int.from_bytes(blob[8:12], 'big'))]:
        raise ValueError('code signature has no CodeDirectory')


BTCONF_ENV = 'PIXOS_BT_PLATFORM_CONFIG'        # "1" = write bluetoothd's platform defaults below; unset = default root unchanged
BTCONF_DOMAIN = 'com.apple.MobileBluetooth.debug'
# sprint-bt 2026-11-09: bluetoothd reads per-feature overrides with CFPreferencesCopyAppValue(<section>, "com.apple.MobileBluetooth.debug")
# -> dict -> bool (reader 0x10027b328 in the 20H392 bluetoothd [bin]). LE/SupportsMultipleAdvertising sets the platform flag
# [platform+0xa0] both ways (0x100259178-0x100259254 [bin]). This BCM4389 firmware answers Apple's multi-advertising VSC 0xfd48 with
# 0x01 Unknown HCI Command (bt-up9/bt-up10 [obs]); with the flag 0 bluetoothd logs "Using a single advertising instance" (0x1005096a8),
# takes mode 1 and sets its random address with HCI_LE_Set_Random_Address itself (0x10019159c -> 0x100191614 -> 0x100191b24 [bin]),
# so LE scanning with Own_Address_Type random no longer fails with 0x12. Telling the stack what the chip supports, not a rewrite.
BTCONF = {'LE': {'SupportsMultipleAdvertising': False}}
# sprint-bt 2026-11-10: BTBW_DEFAULT_ROOT=1 leaves the domain as built; the measured-default boot (the phone has no stock root to compare
# with). Default '1' = the root every sprint-bt boot so far carried; unset or any other value does not write the domain, and the
# build then says so (a missing setting must never look like the measured default).
BTCONF_DEFAULT = 'BTBW_DEFAULT_ROOT'


def tweak_btconfig(fs):
    if os.environ.get(BTCONF_ENV) != '1':
        if os.environ.get(BTCONF_DEFAULT) == '1':
            print('bt platform config: not written (%s unset, %s=1: measured default root)' % (BTCONF_ENV, BTCONF_DEFAULT))
        else:
            print('bt platform config: not written (%s unset, %s missing: NOT the measured default)' % (BTCONF_ENV, BTCONF_DEFAULT))
        return
    path = f'private/var/mobile/Library/Preferences/{BTCONF_DOMAIN}.plist'
    pl = plistlib.loads(fs.read(path)) if fs.exists(path) else {}
    for sec, keys in BTCONF.items():
        pl.setdefault(sec, {}).update(keys)
    fs.write(path, plistlib.dumps(pl, fmt=plistlib.FMT_BINARY), MOBILE, MOBILE, 0o600)
    print('bt platform config: %s %s' % (BTCONF_DOMAIN, ', '.join('%s/%s=%r' % (s, k, v) for s, d in BTCONF.items() for k, v in d.items())))


CATRACE_ENV = 'PIXOS_COREAUDIO_TRACE'          # "1" = VirtualAudio trace keys + persisted Debug logging for com.apple.coreaudio; unset = nothing written
CATRACE_KEYS = ('traceroutingmanager', 'traceportmanager', 'traceroutinghandler', 'tracerouting')
CATRACE_DOMAIN = 'com.apple.coreaudio'
# /Library/Preferences is a symlink to ../private/var/preferences in the staged root (hfs_read of the bt-proper-root3 image [obs]), so the
# any-user plist and the logging override live below private/var/preferences (Logging/Subsystems exists there, empty, in the stock root [obs]).
CATRACE_ANYUSER = f'private/var/preferences/{CATRACE_DOMAIN}.plist'
CATRACE_MOBILE = f'private/var/mobile/Library/Preferences/{CATRACE_DOMAIN}.plist'
CATRACE_LOGPROFILE = f'private/var/preferences/Logging/Subsystems/{CATRACE_DOMAIN}.plist'
CATRACE_LOGSTOCK = f'System/Library/Preferences/Logging/Subsystems/{CATRACE_DOMAIN}.plist'
# Key layout copied from Apple's own profiles in the 20H392 root: DEFAULT-OPTIONS -> Level {Enable, Persist} and TTL {Default, Info, Debug}
# (com.apple.AppleEmbeddedGPU.plist: Level Enable/Persist = Info, TTL 7/7/7; com.apple.ConditionInducer.HighSeverity.plist: Level Enable = Debug,
# Persist = Info). The stock com.apple.coreaudio.plist (DEFAULT-OPTIONS Default-Privacy-Setting = Public, per-category Enable-Oversize-Messages)
# is kept and the Level/TTL keys are added to its DEFAULT-OPTIONS.
CATRACE_LEVEL = {'Enable': 'Debug', 'Persist': 'Debug'}
CATRACE_TTL = {'Default': 7, 'Info': 7, 'Debug': 7}


def tweak_coreaudio_trace(fs):
    # docs/research/bt-a2dp-route-after-open.md s.7.2: VirtualAudio log scopes are CFPreferences keys (value 'minutiae' = level 6) and the decisive
    # rows are debug-level os_log messages, so the unified log must also keep Debug for the subsystem. Opt-in; every write is logged.
    if os.environ.get(CATRACE_ENV) != '1':
        print(f'coreaudio trace: not written ({CATRACE_ENV} unset)')
        return
    for path, uid, gid, mode in ((CATRACE_MOBILE, MOBILE, MOBILE, 0o600), (CATRACE_ANYUSER, 0, 0, 0o644)):
        pl = plistlib.loads(fs.read(path)) if fs.exists(path) else {}
        pl.update({k: 'minutiae' for k in CATRACE_KEYS})
        fs.write(path, plistlib.dumps(pl, fmt=plistlib.FMT_BINARY), uid, gid, mode)
        print('coreaudio trace: %s %s=minutiae' % (path, ','.join(CATRACE_KEYS)))
    if fs.exists(CATRACE_LOGPROFILE):
        prof = plistlib.loads(fs.read(CATRACE_LOGPROFILE))
    elif fs.exists(CATRACE_LOGSTOCK):
        prof = plistlib.loads(fs.read(CATRACE_LOGSTOCK))
    else:
        print('coreaudio trace GAP: stock logging profile %s missing, writing Level/TTL only' % CATRACE_LOGSTOCK)
        prof = {}
    opts = prof.setdefault('DEFAULT-OPTIONS', {})
    opts['Level'] = dict(CATRACE_LEVEL)
    opts['TTL'] = dict(CATRACE_TTL)
    fs.makedirs(os.path.dirname(CATRACE_LOGPROFILE))
    fs.write(CATRACE_LOGPROFILE, plistlib.dumps(prof, fmt=plistlib.FMT_XML), 0, 0, 0o644)
    print('coreaudio trace: %s DEFAULT-OPTIONS Level=%r TTL=%r' % (CATRACE_LOGPROFILE, CATRACE_LEVEL, CATRACE_TTL))


BLUETOOL_ENV = 'PIXOS_BLUETOOL_BIN'            # path of a signed arm64 pixos-bluetool; unset = default root unchanged
BLUETOOL_PATH = 'usr/local/libexec/pixos-bluetool'
BLUETOOL_LABEL = 'com.apple.BlueTool'
BLUETOOL_STOCK_ARGS = ['/usr/sbin/BlueTool', '-R']


def tweak_bluetool(fs):
    # docs/research/bt-pixos-bluetool-spec.md: replace the com.apple.BlueTool service by pixos-bluetool. ONLY the job's ProgramArguments
    # change; a missing job or non-stock ProgramArguments is refused (never guess what else the root carries), every other key is verified unchanged.
    src = os.environ.get(BLUETOOL_ENV)
    if not src:
        print(f'pixos-bluetool: not installed ({BLUETOOL_ENV} unset), default root unchanged')
        return
    with open(src, 'rb') as f:
        data = f.read()
    try:
        macho_signature(data)
    except ValueError as e:
        raise RuntimeError(f'pixos-bluetool: {src} refused: {e}') from None
    plist = 'System/Library/xpc/launchd.plist'
    jobs = plistlib.loads(fs.read(plist)).get('LaunchDaemons', {})
    hits = [k for k, j in jobs.items() if j.get('Label') == BLUETOOL_LABEL]
    if len(hits) != 1:
        raise RuntimeError(f'pixos-bluetool: expected exactly one {BLUETOOL_LABEL} job in launchd.plist, found {len(hits)}')
    if jobs[hits[0]].get('ProgramArguments') != BLUETOOL_STOCK_ARGS:
        raise RuntimeError(f'pixos-bluetool: {BLUETOOL_LABEL} ProgramArguments are {jobs[hits[0]].get("ProgramArguments")!r}, '
                           f'not the stock {BLUETOOL_STOCK_ARGS!r}; refusing')
    before = {}

    def edit(pl):
        job = pl['LaunchDaemons'][hits[0]]
        before.update(job)
        job['ProgramArguments'] = ['/' + BLUETOOL_PATH]
        if {k: v for k, v in job.items() if k != 'ProgramArguments'} != {k: v for k, v in before.items() if k != 'ProgramArguments'}:
            raise RuntimeError('pixos-bluetool: a job key other than ProgramArguments changed')
    edit_plist(fs, plist, edit)
    fs.makedirs(os.path.dirname(BLUETOOL_PATH))
    fs.write(BLUETOOL_PATH, data, 0, 0, 0o755)
    print(f'pixos-bluetool: installed /{BLUETOOL_PATH}; {BLUETOOL_LABEL} ProgramArguments {BLUETOOL_STOCK_ARGS} -> {["/" + BLUETOOL_PATH]}')


def tweak_ping(fs):
    # One-shot launchd job running pixos-ping (T-847c, P-W3): only when PIXOS_PING_BIN names a signed binary. The target
    # address is a private value: it comes from PIXOS_PING_TARGET at build time and is never printed or stored in git.
    src = os.environ.get(PING_BIN_ENV)
    if not src:
        print(f'pixos-ping: not installed ({PING_BIN_ENV} unset), default root unchanged')
        return
    with open(src, 'rb') as f:
        data = f.read()
    try:
        check_ping_binary(data)
    except ValueError as e:
        raise RuntimeError(f'pixos-ping: {src} refused: {e}') from None
    target = os.environ.get(PING_TARGET_ENV, '')
    try:
        if not target or str(ipaddress.IPv4Address(target)) != target:
            raise ValueError
    except ValueError:
        raise RuntimeError(f'pixos-ping: {PING_TARGET_ENV} must be set to an IPv4 literal (the job has nothing to ping otherwise)') from None
    env = {PING_TARGET_ENV: target}
    for name, (lo, hi) in PING_KNOBS.items():
        v = os.environ.get(name)
        if v is None:
            continue
        if not v.isdigit() or not lo <= int(v) <= hi:
            raise RuntimeError(f'pixos-ping: {name}={v} is not an integer in {lo}..{hi}')
        env[name] = v
    fs.makedirs(os.path.dirname(PING_PATH))
    fs.write(PING_PATH, data, 0, 0, 0o755)
    job = {'Label': PING_LABEL, 'ProgramArguments': ['/' + PING_PATH], 'RunAtLoad': True,
           'EnvironmentVariables': env}                    # no KeepAlive: one shot; stdout = launchd default (xnu 0048 sweetnull tee)

    def add(pl):
        jobs = pl['LaunchDaemons']
        for key in [k for k, j in jobs.items() if j.get('Label') == PING_LABEL]:
            del jobs[key]
        jobs[PING_JOB] = job
    edit_plist(fs, 'System/Library/xpc/launchd.plist', add)
    print(f'pixos-ping: installed /{PING_PATH} + job {PING_LABEL} (RunAtLoad, one shot, {len(env)} env keys)')


def tweak_containers(fs):
    # containermanagerd expects these directories to exist on first boot.
    base = 'private/var/containers'
    for sub, owner in (('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)):
        path = f'{base}/{sub}'
        if fs.isdir(path):
            continue
        fs.makedirs(path)
        fs.chown(path, owner, owner)
        fs.chmod(path, 0o755)
        print('made', path)


def tweak_activation(fs):
    # Mark the phone as activated so setup does not wait for Apple's servers.
    ident = 'com.apple.mobileactivationd'
    for d in fs.glob('private/var/containers/Data/System/*'):
        meta = d + '/.com.apple.mobile_container_manager.metadata.plist'
        if fs.exists(meta) and plistlib.loads(fs.read(meta)).get('MCMMetadataIdentifier') == ident:
            break
    else:
        print('warning: no mobileactivationd data container found, activation not set')
        return
    internal = d + '/Library/internal'
    fs.makedirs(internal)
    fs.chown(internal, MOBILE, MOBILE)
    fs.chmod(internal, 0o755)
    ark = {'-ActivationState': 'Activated', '-BrickState': False, '-BuildVersion': BUILD, '-ProductVersion': VERSION,
           '-DataMigrationCompleted': True, '-PreSydroDataMigrationCompleted': True}
    fs.write(internal + '/data_ark.plist', plistlib.dumps(ark, fmt=plistlib.FMT_BINARY), MOBILE, MOBILE, 0o600)
    print('activation:', d)


def tweak_feature_flags(fs):
    # Without a signing-capable xpc cache, extension loading fails: switch the enforcement flag off.
    d = 'private/var/preferences/FeatureFlags/Domain'
    fs.makedirs(d)
    fs.write(d + '/ExtensionKit.plist',
             plistlib.dumps({'enforce_xpc_cache_code_signing': {'Enabled': False}}, fmt=plistlib.FMT_XML), 0, 0, 0o644)


def tweak_audio(fs):
    # The virtual audio HAL plug-in cannot load here; the recorder server of MediaToolbox is turned off.
    plugin = 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin'
    if fs.exists(plugin):
        if os.environ.get('PIXOS_KEEP_VIRTUALAUDIO') == '1':
            # Opt-in (docs/research/ios-audio-route-no-codec.md s.4.1): without VirtualAudio there is no VAD and AudioQueueStart gives -66680.
            # It crash-looped mediaserverd on 2026-10-17 without a default device, so the boot must watch for that.
            print('VirtualAudio.plugin: kept in /Library/Audio/Plug-Ins/HAL (PIXOS_KEEP_VIRTUALAUDIO=1)')
        else:
            fs.makedirs('Library/Audio/Plug-Ins/HAL.disabled')
            fs.move(plugin, 'Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin')
            print('VirtualAudio.plugin: moved to /Library/Audio/Plug-Ins/HAL.disabled (default; PIXOS_KEEP_VIRTUALAUDIO=1 keeps it)')
    for cfg in fs.glob('System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist'):
        edit_plist(fs, cfg, lambda pl: pl.__setitem__('StartRecorderServer', False), plistlib.FMT_XML)


def tweak_stock_apps(fs):
    # Stock apps ship staged; SpringBoard only shows those in /Applications.
    staged = 'private/var/staged_system_apps'
    if not fs.isdir(staged):
        print('no staged_system_apps, skipped')
        return
    if not fs.isdir('Applications'):
        fs.makedirs('Applications')
    moved = 0
    for name in fs.listdir(staged):
        if not fs.exists('Applications/' + name):
            fs.move(f'{staged}/{name}', 'Applications/' + name)
            moved += 1
    print('stock apps moved:', moved)


def tweak_wallpapers(fs):
    # Wallpaper layers are HEIC; the CPU renderer here reads PNG, so add PNG copies and point main.caml at them.
    top = 'Library/Wallpaper/Collections'
    if not fs.isdir(top):
        return
    if not shutil.which('heif-convert'):
        print('heif-convert not found: wallpapers not converted')
        return
    count = 0
    for dp, _, files in list(fs.walk(top)):
        for name in files:
            if name.lower().endswith('.heic'):
                count += 1
                png = _join(dp, name[:-5] + '.png')
                if not fs.exists(png):
                    fs.heic_to_png(_join(dp, name), png)
            elif name == 'main.caml':
                path = _join(dp, name)
                text = fs.read(path).decode('utf-8', 'surrogateescape')
                if '.HEIC"' in text:
                    uid, gid, mode = fs.stat(path)
                    fs.write(path, text.replace('.HEIC"', '.png"').encode('utf-8', 'surrogateescape'), uid, gid, mode)
    print('wallpaper HEIC files:', count)


def tweak_device_name(fs):
    # Show a sensible name in Settings and on the network.
    d = 'private/var/preferences/SystemConfiguration'
    fs.makedirs(d)
    path = d + '/preferences.plist'
    pl = plistlib.loads(fs.read(path)) if fs.exists(path) else {}
    name = 'Pixel 6a'
    sysd = pl.setdefault('System', {}).setdefault('System', {})
    sysd['ComputerName'] = name
    sysd['ComputerNameEncoding'] = 0x8000000
    sysd['HostName'] = name.replace(' ', '-')
    fs.write(path, plistlib.dumps(pl, fmt=plistlib.FMT_BINARY), 0, 0, 0o644)


def tweak_migration(fs):
    # An existing sentinel tells datamigrator that the data is already current.
    fs.makedirs('private/var/datamigrator')
    fs.touch('private/var/datamigrator/migrate')
    fs.chown('private/var/datamigrator/migrate', MOBILE, MOBILE)


PREFS = (
    ('com.apple.springboard', {'BKNoWatchdogs': True, 'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
                               'MCForceShowCloudConfigurationUI': False, 'BSUIVibrancyEffectViewDisabled': True}),
    ('com.apple.purplebuddy', {'SetupDone': True, 'SetupFinishedAllSteps': True, 'SetupVersion': 11,
                               'daemonizeNeedsToRun': False}),
    ('com.apple.purplebuddy.notbackedup', {k: False for k in (
        'ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy', 'ShouldRunKeychainSyncMiniBuddy',
        'ShouldRunMesaMiniBuddy', 'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy', 'SilentICDPUpgrade')}),
    ('com.apple.managedconfiguration.notbackedup', {'LockdownActivationIndicatesCloudConfigurationAvailable': False}),
)


def tweak_preferences(fs):
    # Skip Setup Assistant and watchdogs that kill slow first launches.
    for name, keys in PREFS:
        path = f'private/var/mobile/Library/Preferences/{name}.plist'
        pl = plistlib.loads(fs.read(path)) if fs.exists(path) else {}
        pl.update(keys)
        fs.write(path, plistlib.dumps(pl, fmt=plistlib.FMT_BINARY), MOBILE, MOBILE, 0o600)
        print(name + ':', ', '.join(sorted(keys)))


def apply(fs):
    for step in (tweak_launchd, tweak_wifid_probe, tweak_ping, tweak_containers, tweak_activation, tweak_feature_flags, tweak_audio,
                 tweak_stock_apps, tweak_btapp, tweak_btconfig, tweak_coreaudio_trace, tweak_bluetool, tweak_wallpapers, tweak_device_name, tweak_migration, tweak_preferences):
        step(fs)


def main(argv):
    def usage():
        sys.stderr.write(USAGE)
        return 2

    def is_root(d):
        return os.path.isdir(os.path.join(d, 'System/Library/xpc'))

    if len(argv) == 4 and argv[1] == '--plan':
        src, outdir = argv[2], argv[3]
        if not is_root(src):
            return usage()
        os.makedirs(outdir, exist_ok=True)
        with tarfile.open(os.path.join(outdir, 'overlay.tar'), 'w', format=tarfile.GNU_FORMAT) as tar:
            plan = Plan(src, tar)
            apply(plan)
        write_plan(plan, outdir)
        return 0
    if len(argv) == 2 and not argv[1].startswith('-') and is_root(argv[1]):
        apply(Live(argv[1]))
        return 0
    return usage()


if __name__ == '__main__':
    sys.exit(main(sys.argv))
