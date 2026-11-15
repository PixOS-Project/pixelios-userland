#!/usr/bin/env python3
"""T-2d63: check a signed arm64 Mach-O: XML entitlements (slot 5) EQUAL the plist file, and (optional) the CodeDirectory
identifier (slot 0, CodeDirectory +20 identOffset) equals IDENT. bluetoothd keys its app map by the code-signing id
(T-54b1 s1.2 [bin]), so for BTApp it must be the bundle id com.pixos.btapp.

usage: sigcheck.py BINARY ENTITLEMENTS.plist [IDENT]
prints  sigcheck: <name> ident=<id> entitlements=<keys>  then  RESULT: PASS|FAIL sigcheck <reason>"""
import plistlib
import struct
import sys


def main(argv):
    if len(argv) not in (3, 4):
        print(__doc__)
        return 2
    path, entf = argv[1], argv[2]
    want_ident = argv[3] if len(argv) == 4 else None
    d = open(path, 'rb').read()
    if d[:4] != b'\xcf\xfa\xed\xfe' or struct.unpack_from('<I', d, 4)[0] != 0x0100000c:
        print('RESULT: FAIL sigcheck not a thin arm64 Mach-O')
        return 1
    n = struct.unpack_from('<I', d, 16)[0]
    off, sig = 32, None
    for _ in range(n):
        c, s = struct.unpack_from('<II', d, off)
        if c == 0x1d:
            sig = struct.unpack_from('<II', d, off + 8)
        off += s
    if not sig:
        print('RESULT: FAIL sigcheck no LC_CODE_SIGNATURE')
        return 1
    b = d[sig[0]:sig[0] + sig[1]]
    if b[:4] != b'\xfa\xde\x0c\xc0':
        print('RESULT: FAIL sigcheck not a SuperBlob')
        return 1
    ents, ident = None, None
    for i in range(struct.unpack_from('>I', b, 8)[0]):
        t, o = struct.unpack_from('>II', b, 12 + 8 * i)
        if t == 5 and b[o:o + 4] == b'\xfa\xde\x71\x71':
            ents = plistlib.loads(b[o + 8:o + struct.unpack_from('>I', b, o + 4)[0]])
        if t == 0 and b[o:o + 4] == b'\xfa\xde\x0c\x02':
            io = struct.unpack_from('>I', b, o + 20)[0]
            ident = b[o + io:b.index(b'\0', o + io)].decode()
    want = plistlib.load(open(entf, 'rb'))
    print('sigcheck: %s ident=%s entitlements=%s' % (path.rsplit('/', 1)[-1], ident, ','.join(sorted(ents or {}))))
    if ents != want:
        print('RESULT: FAIL sigcheck sealed entitlements %s != %s' % (ents, want))
        return 1
    if want_ident is not None and ident != want_ident:
        print('RESULT: FAIL sigcheck CodeDirectory identifier %r != %r' % (ident, want_ident))
        return 1
    print('RESULT: PASS sigcheck')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
