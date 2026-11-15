"""Runs tools/check-ios-root.py --selftest (synthetic HFSX images only, no root, no phone)."""
import importlib.util
import os
import pathlib
import struct
import tempfile
import subprocess
import sys
import unittest


TOOL = pathlib.Path(__file__).resolve().parent / "check-ios-root.py"
_spec = importlib.util.spec_from_file_location("check_ios_root", TOOL)
cir = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cir)

BS = 4096
CAT_OFF, ATT_OFF = 1024 + 272, 1024 + 352        # catalog / attributes fork data inside the volume header
FILES = {"sbin/launchd": b"L" * 100}
STRADDLE = "catalog B-tree: 4096-byte nodes never straddle two extents"


def _patch(path, fn):
    with open(path, "r+b") as f:
        img = bytearray(f.read())
        fn(img)
        f.seek(0)
        f.write(img)


def _split_catalog(img):
    """AP-52 layout: the catalog (8 KiB nodes) as a 1-block extent plus the rest."""
    _, _, blocks = struct.unpack_from(">QII", img, CAT_OFF)
    start = struct.unpack_from(">I", img, CAT_OFF + 16)[0]
    struct.pack_into(">IIII", img, CAT_OFF + 16, start, 1, start + 1, blocks - 1)


class CheckIosRoot(unittest.TestCase):
    def build(self, d, **kw):
        p = os.path.join(d, "root.img")
        cir.build_image(p, kw.pop("files", FILES), **kw)
        return p

    def msgs(self, p):
        return cir.check(p, 1 << 40)

    def cli(self, p):
        return subprocess.run([sys.executable, str(TOOL), "--max-mib", "1000", p], capture_output=True, text=True)

    def test_good_image_has_aligned_catalog_and_no_attributes_file(self):
        # also covers `if not blocks: continue` (the synthetic volume has no attributes B-tree)
        with tempfile.TemporaryDirectory() as d:
            p = self.build(d)
            res = self.msgs(p)
            self.assertIn((True, STRADDLE), res)
            self.assertFalse([m for ok, m in res if not ok and "B-tree" in m], res)
            self.assertFalse([m for _, m in res if m.startswith("attributes")])

    def test_straddling_catalog_fails(self):
        with tempfile.TemporaryDirectory() as d:
            p = self.build(d, nsz=2 * BS)
            self.assertIn((True, "catalog B-tree: 8192-byte nodes never straddle two extents"), self.msgs(p))  # one extent: aligned
            _patch(p, _split_catalog)
            bad = [m for ok, m in self.msgs(p) if not ok and "B-tree" in m]
            self.assertEqual(len(bad), 1, bad)
            self.assertIn("are not node-aligned", bad[0])
            self.assertIn("-n c=4096,a=4096", bad[0])
            r = self.cli(p)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("are not node-aligned", r.stdout)

    def test_only_the_last_extent_misaligned_fails(self):
        """extents (2 blocks)(1 block): the second starts node-aligned but is not a multiple of the 8 KiB node size."""
        def odd_tail(img):
            start = struct.unpack_from(">I", img, CAT_OFF + 16)[0]
            struct.pack_into(">I", img, CAT_OFF + 12, 3)
            struct.pack_into(">IIII", img, CAT_OFF + 16, start, 2, start + 2, 1)
        with tempfile.TemporaryDirectory() as d:
            p = self.build(d, nsz=2 * BS)
            _patch(p, odd_tail)
            self.assertTrue([m for ok, m in self.msgs(p) if not ok and "are not node-aligned" in m])

    def test_unreadable_header_is_a_failure_not_a_traceback(self):
        def zero_nsz(img):
            start = struct.unpack_from(">I", img, CAT_OFF + 16)[0]
            struct.pack_into(">H", img, start * BS + 32, 0)

        def beyond_eof(img):
            struct.pack_into(">I", img, CAT_OFF + 16, 100000)
        for fn in (zero_nsz, beyond_eof):
            with tempfile.TemporaryDirectory() as d:
                p = self.build(d)
                _patch(p, fn)
                bad = [m for ok, m in self.msgs(p) if not ok]
                self.assertTrue(any(m.startswith("catalog B-tree header unreadable") for m in bad), (fn.__name__, bad))
                r = self.cli(p)
                self.assertNotEqual(r.returncode, 0)
                self.assertNotIn("Traceback", r.stderr)

    def test_bad_node_size_is_a_failure(self):
        """review userland-extents-straddle-r1 F1: 1536 (not a power of two) and 256 (< 512) are refused before the alignment check."""
        for nsz in (1536, 256):
            def put(img, nsz=nsz):
                start = struct.unpack_from(">I", img, CAT_OFF + 16)[0]
                struct.pack_into(">H", img, start * BS + 32, nsz)
            with tempfile.TemporaryDirectory() as d:
                p = self.build(d)
                _patch(p, put)
                bad = [m for ok, m in self.msgs(p) if not ok]
                self.assertTrue(any(m.startswith("catalog B-tree header unreadable: bad node size %d" % nsz) for m in bad), (nsz, bad))

    def test_corrupt_extents_fork_is_a_failure_not_a_traceback(self):
        """review F2: the extents file claims more blocks than its 8 in-VH extents cover (it has no overflow records of its own, TN1150)."""
        def more_blocks(img):
            struct.pack_into(">I", img, 1024 + 192 + 12, 100)
        with tempfile.TemporaryDirectory() as d:
            p = self.build(d)
            _patch(p, more_blocks)
            bad = [m for ok, m in self.msgs(p) if not ok]
            self.assertTrue(any(m.startswith("extents B-tree header unreadable") and "do not cover 100 blocks" in m for m in bad), bad)
            r = self.cli(p)
            self.assertNotEqual(r.returncode, 0)
            self.assertNotIn("Traceback", r.stderr)

    def test_extents_overflow_file_is_checked_with_file_id_3(self):
        """BTOpenPath checks the extents overflow B-tree too: 8 KiB nodes in a 2-block fork pass as one extent, fail split in two."""
        with tempfile.TemporaryDirectory() as d:
            p = self.build(d, nsz=2 * BS)
            self.assertIn((True, "extents B-tree: 8192-byte nodes never straddle two extents"), self.msgs(p))
            off = 1024 + 192

            def split(img):
                start, cnt = struct.unpack_from(">II", img, off + 16)
                self.assertEqual(cnt, 2)
                struct.pack_into(">IIII", img, off + 16, start, 1, start + 1, 1)
            _patch(p, split)
            bad = [m for ok, m in self.msgs(p) if not ok and m.startswith("extents B-tree")]
            self.assertEqual(len(bad), 1, bad)
            self.assertIn("not node-aligned", bad[0])

    def test_attributes_file_is_checked_with_file_id_8(self):
        """Attributes fork = a 9-extent file whose 9th extent is only in the overflow file, keyed by file id 8."""
        sc = "att/tree"
        hdr = bytes(32) + struct.pack(">H", BS) + bytes(BS)
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "root.img")
            cir.build_image(p, {**FILES, sc: hdr}, scatter=sc)
            with open(p, "rb") as f:
                v = cir.Volume(f)
                v.index({"tree", "att"})
                k = next(k for k in v.idx if k[1] == "tree")
                forkrec = bytes(v.idx[k][88:168])
                cn = struct.unpack_from(">I", v.idx[k], 8)[0]
            key = struct.pack(">HBBII", 10, 0, 0, cn, 8)

            def mk(img):
                i = bytes(img).index(key)
                img[i + 4:i + 8] = struct.pack(">I", 8)          # overflow record now belongs to file id 8
                img[ATT_OFF:ATT_OFF + 80] = forkrec
            _patch(p, mk)
            res = self.msgs(p)
            self.assertIn((True, "attributes B-tree: 4096-byte nodes never straddle two extents"), res)
            self.assertIn((True, STRADDLE), res)

            def big_nodes(img):                                   # attributes nodes 8 KiB over 1-block extents
                struct.pack_into(">H", img, struct.unpack_from(">I", img, ATT_OFF + 16)[0] * BS + 32, 2 * BS)
            _patch(p, big_nodes)
            bad = [m for ok, m in self.msgs(p) if not ok and m.startswith("attributes")]
            self.assertEqual(len(bad), 1, bad)
            self.assertIn("are not node-aligned", bad[0])

    def test_userdata_limits_match_board_json(self):
        import glob, json, os
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        lim = json.load(open(os.path.join(root, 'tools', 'userdata-limits.json')))
        for board, mib in lim.items():
            for p in glob.glob(os.path.join(root, 'device', '*', board, 'board.json')):
                self.assertEqual(json.load(open(p)).get('userdata_mib'), mib, p)


    def test_selftest(self):
        tool = pathlib.Path(__file__).resolve().parent / "check-ios-root.py"
        r = subprocess.run([sys.executable, "-I", str(tool), "--selftest"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("selftest OK", r.stdout)


if __name__ == "__main__":
    unittest.main()
