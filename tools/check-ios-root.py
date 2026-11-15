#!/usr/bin/env python3
"""Host-only, no-root sanity check of an iOS root image before `fastboot flash userdata`.

  python3 -I tools/check-ios-root.py IMG [--board bluejay] [--max-mib N]
  python3 -I tools/check-ios-root.py --selftest

Reads the raw file (never mounts it) and checks what XNU needs at rd=ufsroot0:
  * HFS+/HFSX volume header at offset 1024, allocation block size >= 4096 (the UFS device block;
    hfs_vfsutils.c rejects a smaller one), image no bigger than the userdata partition
    (device/<vendor>/<board>/board.json "userdata_mib", or --max-mib), not truncated;
  * by walking the catalog B-tree (leaf scan, extents overflow, Linux-hfsplus hard links):
    /sbin/launchd, /usr/lib/dyld, the dyld shared cache where dyld looks at ignition_level=1
    and in the cryptex it is linked from, the system keybag, and that
    tools/sweet-root-tweaks.py ran (no _PanicOnCrash / CommCenter left in launchd.plist);
  * only if /usr/lib/libsweetgl.dylib is in the image (overlay-ios-root.sh 5th argument): mode 755, arm64 Mach-O for
    platform iOS, and the com.apple.backboardd job has DYLD_INSERT_LIBRARIES pointing at it.
Exit 0 only if every check passes. Output: one line per check, "ok" or "FAIL".
"""
import glob, json, os, plistlib, shutil, struct, subprocess, sys, tempfile

MIB = 1 << 20
DEV_BLOCK = 4096                       # UFS block = physical block size sweet_ufs.c reports
PRIVATE = '\0\0\0\0HFS+ Private Data'      # where Linux hfsplus keeps hard link targets (iNode<N>)
DSC = 'System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64'
# (path, what it is for, nonzero size required)
FILES = [
    ('sbin/launchd', 'init (rd=ufsroot0 -> launchd)'),
    ('usr/lib/dyld', 'dynamic linker'),
    (DSC, 'shared cache where dyld looks at ignition_level=1 (mkiosroot.sh hard link)'),
    ('private/preboot/Cryptexes/OS/' + DSC, 'shared cache in the SystemOS cryptex (link source)'),
    ('private/var/keybags/systembag.kb', 'plain system keybag (keybagd, /defaults/no-effaceable-storage)'),
    ('System/Library/xpc/launchd.plist', 'launchd job list'),
]
SWEETGL = 'usr/lib/libsweetgl.dylib'


def macho_ios_arm64(head):
    """True if `head` (header + load commands) is a thin arm64 Mach-O with LC_BUILD_VERSION platform 2 (iOS)."""
    if len(head) < 32:
        return False
    magic, cpu, _, _, ncmds, szcmds = struct.unpack_from('<IIIIII', head, 0)
    if magic != 0xfeedfacf or cpu != 0x0100000c:
        return False
    off = 32
    for _ in range(ncmds):
        if off + 8 > len(head):
            return False
        cmd, sz = struct.unpack_from('<II', head, off)
        if cmd == 0x32 and struct.unpack_from('<I', head, off + 8)[0] == 2:
            return True
        off += sz
    return False


class Bad(Exception):
    pass


class Fork:
    """Byte reader over a list of (start block, block count) extents."""
    def __init__(self, f, bs, extents):
        self.f, self.bs, self.ext = f, bs, [e for e in extents if e[1]]

    def read(self, off, n):
        out, pos = b'', 0
        for start, cnt in self.ext:
            span = cnt * self.bs
            if n and off < pos + span:
                take = min(n, pos + span - off)
                self.f.seek(start * self.bs + off - pos)
                out += self.f.read(take)
                off, n = off + take, n - take
            pos += span
        if n:
            raise Bad('read past the end of a fork')
        return out


def extents_at(b, o):
    return [struct.unpack_from('>II', b, o + 8 * i) for i in range(8)]


def leaves(fork):
    """Yield the record bytes (key + data) of every leaf node, following the sibling links."""
    hdr = fork.read(0, 64)
    first, nsz, total = struct.unpack_from('>I', hdr, 24)[0], struct.unpack_from('>H', hdr, 32)[0], struct.unpack_from('>I', hdr, 36)[0]
    n = first
    for _ in range(total):
        if not n:
            return
        node = fork.read(n * nsz, nsz)
        flink, _, kind, _, nrec = struct.unpack_from('>IIbBH', node, 0)
        if kind != -1:
            raise Bad(f'B-tree node {n} is not a leaf')
        r = struct.unpack_from('>%dH' % (nrec + 1), node, nsz - 2 * (nrec + 1))
        for i in range(nrec):
            yield node[r[nrec - i]:r[nrec - i - 1]]
        n = flink
    raise Bad('B-tree leaf chain does not end')


class Volume:
    def __init__(self, f):
        self.f = f
        f.seek(0, 2)
        self.size = f.tell()
        f.seek(1024)
        vh = f.read(512)
        self.sig = vh[:2]
        if self.sig not in (b'H+', b'HX'):
            raise Bad(f'no HFS+/HFSX signature at offset 1024 (found {vh[:2].hex()}; XNU: "unknown Volume Signature")')
        self.attr, self.last_mounted = struct.unpack_from('>I4s', vh, 4)
        self.bs, self.total = struct.unpack_from('>II', vh, 40)
        self.extents_fork = vh[192:272]     # extents overflow file (file id 3): BTOpenPath checks its nodes too
        self.catalog_fork = vh[272:352]
        self.attributes_fork = vh[352:432]
        self.overflow = Fork(f, self.bs, extents_at(vh, 192 + 16))     # extents overflow file

    def full_fork(self, fileid, forkdata):
        """Fork reader for a fork record (80 bytes), adding extents from the overflow file if needed."""
        size, _, blocks = struct.unpack_from('>QII', forkdata, 0)
        ext = extents_at(forkdata, 16)
        have = sum(c for _, c in ext)
        if have < blocks:
            more = {}
            for rec in leaves(self.overflow):
                klen, ftype, _, fid, start = struct.unpack_from('>HBBII', rec, 0)
                if fid == fileid and ftype == 0:
                    more[start] = extents_at(rec, 2 + klen)
            for start in sorted(more):
                ext += more[start]
            if sum(c for _, c in ext) < blocks:
                raise Bad(f'file {fileid}: extents overflow records do not cover {blocks} blocks')
        return Fork(self.f, self.bs, ext), size

    def index(self, want):
        """{(parent, name): record} for all folders and the files named in `want` (or iNode*)."""
        fork, _ = self.full_fork(4, self.catalog_fork)
        fold = (lambda s: s) if self.sig == b'HX' else str.lower
        want = {fold(w) for w in want}
        idx = {}
        for rec in leaves(fork):
            klen, parent, nlen = struct.unpack_from('>HIH', rec, 0)
            name = rec[8:8 + 2 * nlen].decode('utf-16-be', 'surrogatepass')
            body = rec[(2 + klen + 1) & ~1:]
            rtype = struct.unpack_from('>h', body, 0)[0]
            if rtype == 1 or (rtype == 2 and (fold(name) in want or name.startswith('iNode'))):
                idx[(parent, fold(name))] = body
        self.idx, self.fold = idx, fold

    def lookup(self, path):
        cur = 2
        parts = path.strip('/').split('/')
        for i, comp in enumerate(parts):
            body = self.idx.get((cur, self.fold(comp)))
            if body is None:
                raise Bad('missing: /' + '/'.join(parts[:i + 1]))
            rtype, cnid = struct.unpack_from('>h', body, 0)[0], struct.unpack_from('>I', body, 8)[0]
            if i < len(parts) - 1 and rtype != 1:
                raise Bad('not a directory: /' + '/'.join(parts[:i + 1]))
            cur = cnid
        if rtype != 2:
            raise Bad('not a file: /' + path)
        return cnid, body

    def read_file(self, path):
        """(size, content reader) of a regular file; Linux hfsplus hard links resolved via the private dir."""
        cnid, body = self.lookup(path)
        if body[48:56] == b'hlnk' b'hfs+':
            ino = struct.unpack_from('>I', body, 44)[0]
            priv = [k[1] for k in self.idx if k[0] == 2 and k[1].startswith(self.fold('\0\0\0\0HFS+ Private'))]
            if not priv:
                raise Bad(f'hard link /{path}: no private data directory')
            pcnid = struct.unpack_from('>I', self.idx[(2, priv[0])], 8)[0]
            body = self.idx.get((pcnid, self.fold(f'iNode{ino}')))
            if body is None:
                raise Bad(f'hard link /{path}: iNode{ino} missing')
            cnid = struct.unpack_from('>I', body, 8)[0]
        fork, size = self.full_fork(cnid, body[88:168])
        return size, fork


def check(path, max_bytes):
    """Return a list of (ok, message)."""
    out = []

    def rec(ok, msg):
        out.append((ok, msg))
        return ok

    with open(path, 'rb') as f:
        try:
            v = Volume(f)
        except Bad as e:
            return [(False, str(e))]
        rec(True, f'volume header: {v.sig.decode()} ({"case-sensitive" if v.sig == b"HX" else "case-INsensitive"}), '
            f'block size {v.bs}, {v.total} blocks = {v.total * v.bs / MIB:.0f} MiB, '
            f'{"journaled" if v.attr & 0x2000 else "not journaled"}, {"clean" if v.attr & 0x100 else "DIRTY"} unmount, last mounted by {v.last_mounted!r}')
        if v.sig == b'H+':
            rec(False, 'iOS needs a case-sensitive volume (mkfs.hfsplus -s)')
        rec(v.bs >= DEV_BLOCK and v.bs & (v.bs - 1) == 0, f'allocation block size {v.bs} is a power of two >= {DEV_BLOCK} (device block)')
        vol = v.total * v.bs
        rec(vol <= v.size, f'image is not truncated ({v.size} bytes >= volume {vol})')
        rec(v.size % DEV_BLOCK == 0, f'image size is a multiple of {DEV_BLOCK}')
        rec(v.size <= max_bytes, f'image {v.size / MIB:.0f} MiB '
            + (f'fits the {max_bytes // MIB} MiB userdata partition' if v.size <= max_bytes else f'is bigger than the {max_bytes // MIB} MiB userdata partition'))
        # XNU refuses a journaled mount when a B-tree node straddles two extents (BTree.c BTOpenPath -> NodesAreContiguous): the
        # image mounts once, then every later boot fails with "BTOpenPath returned (5)" (observed 2026-10-25, ios-root3*).
        for name, fid, fd in (('extents', 3, v.extents_fork), ('catalog', 4, v.catalog_fork), ('attributes', 8, v.attributes_fork)):
            if not struct.unpack_from('>QII', fd, 0)[2]:
                continue                    # no such file (a minimal test volume has no attributes B-tree)
            try:
                fork, _ = v.full_fork(fid, fd)
                nsz = struct.unpack_from('>H', fork.read(0, 64), 32)[0]
                if nsz < 512 or nsz & (nsz - 1):    # TN1150 BTHeaderRec.nodeSize: a power of two, 512..32768 (review userland-extents-straddle-r1 F1)
                    raise Bad(f'bad node size {nsz} (must be a power of two >= 512)')
                pos, bad = 0, []
                for start, cnt in fork.ext:
                    if (pos * v.bs) % nsz or (cnt * v.bs) % nsz:
                        bad.append((start, cnt))
                    pos += cnt
                rec(not bad, f'{name} B-tree: {nsz}-byte nodes never straddle two extents'
                    if not bad else f'{name} B-tree has {nsz}-byte nodes but extents {bad[:3]} are not node-aligned: XNU cannot remount this '
                    f'volume (mkfs.hfsplus needs -n c=4096,a=4096 so nodes equal the {v.bs}-byte block)')
            except (Bad, struct.error, ZeroDivisionError) as e:
                rec(False, f'{name} B-tree header unreadable: {e}')
        try:
            v.index({os.path.basename(p) for p, _ in FILES + [(SWEETGL, '')]} | {c for p, _ in FILES + [(SWEETGL, '')] for c in p.split('/')})
        except (Bad, struct.error) as e:
            rec(False, f'catalog B-tree unreadable: {e}')
            return out
        plist = None
        for p, why in FILES:
            try:
                size, fork = v.read_file(p)
                if not size:
                    raise Bad(f'empty file: /{p}')
                rec(True, f'/{p} ({size} bytes): {why}')
            except (Bad, struct.error) as e:
                rec(False, f'{e}  [{why}]')
                continue
            if p.endswith('xpc/launchd.plist'):
                try:
                    plist = plistlib.loads(fork.read(0, size))
                    jobs = plist['LaunchDaemons'].values()
                    left = sum('_PanicOnCrash' in j for j in jobs) + sum(j.get('Label') == 'com.apple.CommCenter' for j in jobs)
                    lab = {j.get('Label'): j for j in plist['LaunchDaemons'].values()}
                    if any(str(k).startswith('org.pixelios.sweetmali-') for k in lab):
                        nr = lab.get('org.pixelios.sweetmali-nonroot', {})
                        rec(nr.get('UserName') == 'mobile' and nr.get('ProgramArguments', [None, None])[-1:] == ['nonroot'],
                            f"sweetmali-nonroot job runs `nonroot` as UserName {nr.get('UserName')!r} (want 'mobile')")
                    rec(left == 0, 'launchd.plist has the bring-up tweaks (no _PanicOnCrash, no CommCenter)'
                        if left == 0 else f'launchd.plist: {left} untweaked entries; tools/sweet-root-tweaks.py did not run')
                except Exception as e:
                    rec(False, f'launchd.plist unreadable: {e}')
        try:
            cnid, body = v.lookup(SWEETGL)
        except Bad:
            return out                                       # no injection in this image
        try:
            size, fork = v.read_file(SWEETGL)
            mode = struct.unpack_from('>H', body, 42)[0] & 0o7777
            rec(mode == 0o755, f'/{SWEETGL} mode {mode:o} (want 755)')
            rec(macho_ios_arm64(fork.read(0, min(size, 4096))), f'/{SWEETGL} ({size} bytes) is an arm64 Mach-O for platform iOS')
            bb = [j for j in (plist or {}).get('LaunchDaemons', {}).values() if j.get('Label') == 'com.apple.backboardd']
            env = bb[0].get('EnvironmentVariables', {}).get('DYLD_INSERT_LIBRARIES') if len(bb) == 1 else None
            rec(env == '/' + SWEETGL, f'backboardd job (x{len(bb)}) DYLD_INSERT_LIBRARIES = {env!r} (want /{SWEETGL})')
            cft = bb[0].get('EnvironmentVariables', {}).get('CA_FORCE_TIMER') if len(bb) == 1 else None
            rec(cft == '1', f'backboardd job (x{len(bb)}) CA_FORCE_TIMER = {cft!r} (want \'1\': CA uses the IOMFB vsync source)')
        except Exception as e:
            rec(False, f'/{SWEETGL} check failed: {e}')
    return out


def userdata_mib(board):
    """The userdata size limit: device/*/<board>/board.json (kernel repo) or, in a checkout without it (the userland repo),
    tools/userdata-limits.json. tools/test_check_ios_root.py requires the two to agree wherever both exist."""
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for p in glob.glob(os.path.join(here, 'device', '*', board, 'board.json')):
        with open(p) as f:
            return json.load(f).get('userdata_mib')
    try:
        with open(os.path.join(here, 'tools', 'userdata-limits.json')) as f:
            return json.load(f).get(board)
    except OSError:
        return None


# ---------------------------------------------------------------- selftest: synthetic image

def build_image(path, files, bs=4096, sig=b'HX', hardlink=None, scatter=None, size=96 * 4096, modes=None, nsz=None):
    """Write a tiny HFSX image. files: {path: bytes}; hardlink: (path, bytes) stored the way Linux hfsplus
    does (hlnk record -> iNode99 in the private dir); scatter: path whose data is split over nine
    one-block extents (the ninth is only reachable through the extents overflow file). nsz: B-tree node size (default bs;
    2*bs reproduces the AP-52 layout when the catalog extents are then split to one block)."""
    nsz = nsz or bs
    files = dict(files)
    if hardlink:
        files[PRIVATE + '/iNode99'] = hardlink[1]
        files[hardlink[0]] = None
    ids = iter(range(16, 1 << 20))
    dirs, cnid = {'': 2}, {}
    for p in sorted(files):
        parts = p.split('/')
        for i in range(1, len(parts)):
            dirs.setdefault('/'.join(parts[:i]), next(ids))
        cnid[p] = next(ids)
    # data from byte 48 KiB, a gap block after every file so extents are not contiguous
    blk, placed, data = 49152 // bs, {}, []
    for p in sorted(files):
        content = files[p] or b''
        if p == scatter:
            exts, blk = [(blk + 2 * i, 1) for i in range(9)], blk + 18
        else:
            n = -(-len(content) // bs)
            exts, blk = ([(blk, n)] if n else []), blk + n + 1
        placed[p] = exts
        pos = 0
        for s, c in exts:
            data.append((s, content[pos:pos + c * bs]))
            pos += c * bs
    assert blk * bs < size, 'selftest image too small'

    def key(parent, name):
        nb = name.encode('utf-16-be')
        k = struct.pack('>HIH', 6 + len(nb), parent, len(nb) // 2) + nb
        return k + b'\0' * (len(k) & 1)

    def folder(c):
        r = bytearray(88)
        struct.pack_into('>hHII', r, 0, 1, 0, 0, c)
        return bytes(r)

    recs = [(1, 'root', folder(2))]
    recs += [('/'.join(d.split('/')[:-1]), d.split('/')[-1], folder(c)) for d, c in dirs.items() if d]
    recs = [(dirs[p] if isinstance(p, str) else p, n, r) for p, n, r in recs]
    for p, content in files.items():
        r = bytearray(248)
        struct.pack_into('>hHII', r, 0, 2, 0, 0, cnid[p])
        struct.pack_into('>H', r, 42, 0o100000 | (modes or {}).get(p, 0o755))
        if content is None:                                 # Linux hfsplus hard link: type/creator hlnk/hfs+, iNode number in "special"
            r[48:56] = b'hlnkhfs+'
            struct.pack_into('>I', r, 44, 99)
        struct.pack_into('>QII', r, 88, len(content or b''), 0, sum(c for _, c in placed[p]))
        for i, (s, c) in enumerate(placed[p][:8]):
            struct.pack_into('>II', r, 88 + 16 + 8 * i, s, c)
        recs.append((dirs['/'.join(p.split('/')[:-1])], p.split('/')[-1], bytes(r)))
    recs.sort(key=lambda t: (t[0], t[1]))

    def btree(items):
        """Node 0 = header, then the leaf nodes holding the key+record blobs."""
        nodes, cur = [], []
        for it in items:
            if 14 + sum(len(x) + 2 for x in cur) + len(it) + 4 > nsz:
                nodes.append(cur)
                cur = []
            cur.append(it)
        if cur:
            nodes.append(cur)
        hdr = bytearray(nsz)
        struct.pack_into('>IIbBH', hdr, 0, 0, 0, 1, 0, 1)
        struct.pack_into('>HIIIIHHI', hdr, 14, 1, 1 if nodes else 0, len(items), 1 if nodes else 0, len(nodes), nsz, 516, len(nodes) + 1)
        blob = bytearray(hdr)
        for i, cur in enumerate(nodes):
            nd = bytearray(nsz)
            struct.pack_into('>IIbBH', nd, 0, i + 2 if i + 1 < len(nodes) else 0, i, -1, 1, len(cur))
            off, offs = 14, []
            for x in cur:
                nd[off:off + len(x)] = x
                offs.append(off)
                off += len(x)
            offs.append(off)
            for j, o in enumerate(offs):
                struct.pack_into('>H', nd, nsz - 2 * (j + 1), o)
            blob += nd
        return bytes(blob)

    cat = btree([key(p, n) + r for p, n, r in recs])
    ovf = btree([struct.pack('>HBBII', 10, 0, 0, cnid[scatter], 8) + struct.pack('>II', placed[scatter][8][0], 1) + b'\0' * 56] if scatter else [])
    ovf_blk, cat_blk = 8192 // bs, 16384 // bs
    assert len(cat) <= 32768 // bs * bs - 16384 and len(ovf) <= 8192, 'catalog overlaps the data area'
    img = bytearray(size)
    img[ovf_blk * bs:ovf_blk * bs + len(ovf)] = ovf
    img[cat_blk * bs:cat_blk * bs + len(cat)] = cat
    for b, chunk in data:
        img[b * bs:b * bs + len(chunk)] = chunk
    vh = bytearray(512)
    struct.pack_into('>2sHI4s', vh, 0, sig, 5 if sig == b'HX' else 4, 0x2100, b'H+Lx')
    struct.pack_into('>II', vh, 40, bs, size // bs)
    for off, blob, b0 in ((192, ovf, ovf_blk), (272, cat, cat_blk)):          # extents, catalog fork data
        struct.pack_into('>QII', vh, off, len(blob), 0, len(blob) // bs)
        struct.pack_into('>II', vh, off + 16, b0, len(blob) // bs)
    img[1024:1536] = vh
    with open(path, 'wb') as f:
        f.write(img)


def selftest():
    plist = plistlib.dumps({'LaunchDaemons': {'a': {'Label': 'x', '_PanicOnCrasX': True}}, 'pad': 'x' * 33000}, fmt=plistlib.FMT_BINARY)
    bad_plist = plistlib.dumps({'LaunchDaemons': {'a': {'Label': 'x', '_PanicOnCrash': True}, 'c': {'Label': 'com.apple.CommCenter'}}})
    files = {'sbin/launchd': b'L' * 5000, 'usr/lib/dyld': b'D' * 100,
             'private/preboot/Cryptexes/OS/' + DSC: b'C' * 50, 'private/var/keybags/systembag.kb': b'<plist/>',
             'System/Library/xpc/launchd.plist': plist}
    link = (DSC, b'C' * 50)
    big = 112914 * MIB

    def run(tag, **kw):
        files_ = kw.pop('files', files)
        maxb = kw.pop('max_bytes', big)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'root.img')
            build_image(p, files_, hardlink=link, scatter=kw.pop('scatter', 'System/Library/xpc/launchd.plist'), **kw)
            res = check(p, maxb)
        bad = [m for ok, m in res if not ok]
        print(f'[{tag}] {len(res)} checks, {len(bad)} failed' + ''.join('\n    FAIL ' + m for m in bad))
        return res, bad

    res, bad = run('good image')
    assert not bad, bad
    def jobs(**nr):
        j = {'a': {'Label': 'x'}, 'b': {'Label': 'org.pixelios.sweetmali-map'}}
        if nr:
            j['c'] = {'Label': 'org.pixelios.sweetmali-nonroot', 'ProgramArguments': ['/t', 'nonroot'], **nr}
        return {**files, 'System/Library/xpc/launchd.plist': plistlib.dumps({'LaunchDaemons': j, 'pad': 'x' * 33000}, fmt=plistlib.FMT_BINARY)}
    _, bad = run('nonroot job ok', files=jobs(UserName='mobile'))
    assert not bad, bad
    _, bad = run('nonroot job missing', files=jobs())
    assert len(bad) == 1 and 'sweetmali-nonroot' in bad[0], bad
    _, bad = run('nonroot job as root', files=jobs(UserName='root'))
    assert len(bad) == 1 and 'sweetmali-nonroot' in bad[0], bad
    assert any('(33' in m or 'launchd.plist' in m for _, m in res)
    _, bad = run('no launchd', files={k: v for k, v in files.items() if k != 'sbin/launchd'})
    assert len(bad) == 1 and 'missing: /sbin' in bad[0], bad
    _, bad = run('untweaked launchd.plist', files={**files, 'System/Library/xpc/launchd.plist': bad_plist})
    assert len(bad) == 1 and 'untweaked' in bad[0], bad
    _, bad = run('too big for userdata', max_bytes=96 * 4096 - 4096)
    assert len(bad) == 1 and 'bigger than' in bad[0], bad
    _, bad = run('case-insensitive', sig=b'H+')
    assert any('case-sensitive' in m for m in bad), bad
    _, bad = run('block size 512', bs=512, scatter=None)
    assert any('block size 512' in m for m in bad), bad
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'junk.img')
        open(p, 'wb').write(b'\xde\xad' * 4096)
        res = check(p, big)
        assert not res[0][0] and 'no HFS+/HFSX signature' in res[0][1], res
        print('[junk] ' + res[0][1])
        mk = shutil.which('mkfs.hfsplus', path=os.environ.get('PATH', '') + ':/usr/sbin:/sbin')
        if mk:
            p = os.path.join(d, 'real.img')
            with open(p, 'wb') as f:
                f.truncate(64 * MIB)
            subprocess.run([mk, '-s', '-J', '-v', 'iOSRoot', p], check=True, stdout=subprocess.DEVNULL)
            res = check(p, big)
            print(f'[real mkfs.hfsplus image] {len(res)} checks; header + catalog walk on the real on-disk format:')
            for ok, m in res[:3]:
                print('    ' + ('ok   ' if ok else 'FAIL ') + m)
            assert res[0][0] and any('missing: /sbin' in m for ok, m in res), res
        else:
            print('[real mkfs.hfsplus image] skipped: mkfs.hfsplus not installed (sudo apt install hfsprogs)')
    # libsweetgl injection checks
    macho = struct.pack('<IIIIIIII', 0xfeedfacf, 0x0100000c, 0, 6, 1, 24, 0, 0) + struct.pack('<IIIIII', 0x32, 24, 2, 0, 0, 0)
    inj = {'LaunchDaemons': {'/S/b.plist': {'Label': 'com.apple.backboardd', 'EnvironmentVariables': {'DYLD_INSERT_LIBRARIES': '/' + SWEETGL, 'CA_FORCE_TIMER': '1'}}}}
    ok_files = {**files, 'System/Library/xpc/launchd.plist': plistlib.dumps(inj), SWEETGL: macho}
    _, bad = run('injected dylib', files=ok_files, scatter=None)
    assert not bad, bad
    _, bad = run('dylib mode 644', files=ok_files, scatter=None, modes={SWEETGL: 0o644})
    assert len(bad) == 1 and 'mode 644' in bad[0], bad
    _, bad = run('dylib not iOS', files={**ok_files, SWEETGL: macho[:40] + struct.pack('<I', 1) + macho[44:]}, scatter=None)
    assert len(bad) == 1 and 'platform iOS' in bad[0], bad
    noenv = {'LaunchDaemons': {'/S/b.plist': {'Label': 'com.apple.backboardd'}}}
    _, bad = run('no DYLD env', files={**ok_files, 'System/Library/xpc/launchd.plist': plistlib.dumps(noenv)}, scatter=None)
    assert any('DYLD_INSERT_LIBRARIES' in b for b in bad), bad
    noft = {'LaunchDaemons': {'/S/b.plist': {'Label': 'com.apple.backboardd', 'EnvironmentVariables': {'DYLD_INSERT_LIBRARIES': '/' + SWEETGL}}}}
    _, bad = run('no CA_FORCE_TIMER', files={**ok_files, 'System/Library/xpc/launchd.plist': plistlib.dumps(noft)}, scatter=None)
    assert len(bad) == 1 and 'CA_FORCE_TIMER' in bad[0], bad
    print('selftest OK')


def main():
    args = sys.argv[1:]
    if args == ['--selftest']:
        return selftest()
    board, max_mib, img = 'bluejay', None, None
    while args:
        a = args.pop(0)
        if a == '--board' and args:
            board = args.pop(0)
        elif a == '--max-mib' and args:
            max_mib = int(args.pop(0))
        elif img is None and not a.startswith('-'):
            img = a
        else:
            sys.exit(__doc__)
    if img is None:
        sys.exit(__doc__)
    max_mib = max_mib or userdata_mib(board)
    if not max_mib:
        sys.exit(f'no "userdata_mib" for board {board} in device/*/{board}/board.json: pass --max-mib N')
    try:
        res = check(img, max_mib * MIB)
    except OSError as e:
        sys.exit(f'cannot read {img}: {e}')
    for ok, m in res:
        print(('ok   ' if ok else 'FAIL ') + m)
    failed = sum(not ok for ok, _ in res)
    print(f'{"FAIL" if failed else "PASS"}: {img}: {len(res)} checks, {failed} failed'
          + ('' if failed else f' (userdata limit {max_mib} MiB, board {board}); host check only, XNU decides at mount time'))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
