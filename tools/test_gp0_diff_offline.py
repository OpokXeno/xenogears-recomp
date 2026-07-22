#!/usr/bin/env python3
"""tools/test_gp0_diff_offline.py — TDD tests for offline burst-analysis flags.

Tests the five new flags on tools/gp0_diff.py:
  T1.2  --from-dir / --env-diff / --src-hist / --strip-lo / --strip-hi
  T1.3  --cadence

Fixtures are generated into tools/testdata_gp0/ on first run (idempotent:
existing files are not rewritten, so on-disk bytes are the source of truth
once the suite has been executed once).

Run with:
  ~/xenogears-port/.venv/bin/python tools/test_gp0_diff_offline.py
"""

import json
import os
import shutil
import sys
import tempfile
import unittest
from collections import Counter
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parent
FIXTURES = TOOLS / "testdata_gp0"
TESTDATA = FIXTURES  # canonical alias

# Make `import gp0_diff` work without changing its sys.path semantics.
sys.path.insert(0, str(TOOLS))
import gp0_diff  # noqa: E402


# ---------------------------------------------------------------------------
# Fixture builders
# ---------------------------------------------------------------------------

def _entry(seq, op, src, w, pc="0x80010000", ra="0x80010000",
           func="0x80010000", n=1):
    """One GP0 entry as the live ring / jsonl writer would shape it."""
    return {"seq": seq, "op": op, "src": src, "n": n,
            "pc": pc, "func": func, "ra": ra, "w": list(w)}


def _w0_color(op_hex, color_hex):
    """A w[0] word carrying `op` in the high byte and `color` in the low 24 bits."""
    return f"0x{op_hex[2:].zfill(2)}{color_hex[2:].zfill(6).upper()}"


def _write_jsonl(path, entries):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w") as f:
        for e in entries:
            f.write(json.dumps(e) + "\n")


def _ensure_offline_diff():
    """3 prims X/Y/Z; F=104 has X missing + Y w[0] altered.
    Parity refs (frame-2, frame+2) = (100, 108). Build stride 2 so
    the live cadence check (F - P == 4 && N - F == 4) passes.
    """
    root = FIXTURES / "offline_diff"
    if (root / "gp0_f000104.jsonl").exists():
        return

    X = "0x1DB400"   # will be MISSING in 104
    Y = "0x1DB500"   # will be ALTERED (color) in 104
    Z = "0x1EB400"   # control: identical across all frames

    base = {
        "seq": 0, "pc": "0x80010000", "func": "0x80010000", "ra": "0x80010000",
    }
    def X_e(seq): return dict(base, seq=seq, op="0x24", src=X,
                              w=["0x24000000"] + ["0x00000000"] * 6)
    def Y_e(seq, w0="0x24000000"): return dict(base, seq=seq, op="0x24", src=Y,
                              w=[w0] + ["0x00000000"] * 6)
    def Z_e(seq): return dict(base, seq=seq, op="0x24", src=Z,
                              w=["0x24000000"] + ["0x00000000"] * 6)

    for f, with_X, Y_w0 in (
        (100, True,  "0x24000000"),
        (102, True,  "0x24000000"),
        (104, False, "0x24FF0000"),  # X gone, Y color flipped
        (106, True,  "0x24000000"),
        (108, True,  "0x24000000"),
    ):
        entries = []
        if with_X:
            entries.append(X_e(seq=len(entries)))
        entries.append(Y_e(seq=len(entries), w0=Y_w0))
        entries.append(Z_e(seq=len(entries)))
        _write_jsonl(root / f"gp0_f{f:06d}.jsonl", entries)

    state = {"dma_state": {"channels": [None, None,
                                        {"madr": 0x80100000, "chcr": 0x0},
                                        None, None, None, None]}}
    (root / "state.json").write_text(json.dumps(state))
    (root / "detector.json").write_text(json.dumps({"flagged_frame": 104}))


def _ensure_offline_env():
    """Inject an E3 env command with a divergent value in F=104 at index 2.

    E1..E6 are the per-frame env commands in the order they fire; we emit
    E1, E2, E3, E4, E5, E6 in every frame, but F's E3 has its w[0] = 0xE3FF
    (bottom-right offset) while parity refs use 0xE3000.
    """
    root = FIXTURES / "offline_env"
    if (root / "gp0_f000104.jsonl").exists():
        return

    base = {"seq": 0, "pc": "0x80020000", "func": "0x80020000",
            "ra": "0x80020000", "src": "0x80020000"}

    def env_frame(f, e3_w0):
        ops = ["0xE1", "0xE2", "0xE3", "0xE4", "0xE5", "0xE6"]
        # E1..E6 each take 1 word (the cmd itself); w[0] = the cmd word.
        return [dict(base, seq=i, op=op, src=f"0x{f:08X}",
                     w=[e3_w0 if (op == "0xE3") else op]) for i, op in enumerate(ops)]

    # Frames 100..108 (stride 2) so the cadence check passes; only F=104 differs.
    for f in (100, 102, 104, 106, 108):
        e3 = "0xE30000" if f != 104 else "0xE300FF"
        _write_jsonl(root / f"gp0_f{f:06d}.jsonl", env_frame(f, e3))

    state = {"dma_state": {"channels": [None, None, {"madr": 0}, None]}}
    (root / "state.json").write_text(json.dumps(state))
    (root / "detector.json").write_text(json.dumps({"flagged_frame": 104}))


def _ensure_offline_hist():
    """Three known src clusters, all in frame 100 only.

    Bucket size for the test is 0x100 (matches gp0_diff.src_histogram default).
    """
    root = FIXTURES / "offline_hist"
    if (root / "gp0_f000100.jsonl").exists():
        return

    base = {"seq": 0, "pc": "0x80010000", "func": "0x80010000",
            "ra": "0x80010000", "op": "0x24", "n": 1}
    w0 = ["0x24000000"] + ["0x00000000"] * 6

    entries = []
    # 10 entries at 0x1DB400 (bucket 0x1DB400)
    for i in range(10):
        entries.append(dict(base, seq=i, src="0x1DB400", w=list(w0)))
    # 5 entries at 0x1EB400 (bucket 0x1EB400)
    for i in range(5):
        entries.append(dict(base, seq=10 + i, src="0x1EB400", w=list(w0)))
    # 3 entries at 0x80123456 (bucket 0x80123400)
    for i in range(3):
        entries.append(dict(base, seq=20 + i, src="0x80123456", w=list(w0)))
    _write_jsonl(root / "gp0_f000100.jsonl", entries)

    state = {"dma_state": {"channels": [None, None, {"madr": 0}, None]}}
    (root / "state.json").write_text(json.dumps(state))
    (root / "detector.json").write_text(json.dumps({"flagged_frame": 100}))


def _ensure_cadence_clustered():
    """5 captures, all flagged frames land in 0..4 mod 120 (near wake boundary)."""
    root = FIXTURES / "cadence_clustered"
    if (root / "capture_005" / "detector.json").exists():
        return
    # mod 120: 5, 5, 2, 0, 0
    for tag, frame in [("001", 5), ("002", 125), ("003", 242),
                       ("004", 360), ("005", 480)]:
        (root / f"capture_{tag}").mkdir(parents=True, exist_ok=True)
        (root / f"capture_{tag}" / "detector.json").write_text(
            json.dumps({"flagged_frame": frame}))


def _ensure_cadence_uniform():
    """5 captures, frames spread across 25..100, none within 0..4 mod 120."""
    root = FIXTURES / "cadence_uniform"
    if (root / "capture_014" / "detector.json").exists():
        return
    for tag, frame in [("010", 50), ("011", 75), ("012", 25),
                       ("013", 100), ("014", 30)]:
        (root / f"capture_{tag}").mkdir(parents=True, exist_ok=True)
        (root / f"capture_{tag}" / "detector.json").write_text(
            json.dumps({"flagged_frame": frame}))


def _ensure_all():
    _ensure_offline_diff()
    _ensure_offline_env()
    _ensure_offline_hist()
    _ensure_cadence_clustered()
    _ensure_cadence_uniform()


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class FromDirTest(unittest.TestCase):
    """--from-dir: offline MISSING / ALTERED classification."""

    @classmethod
    def setUpClass(cls):
        _ensure_all()

    def test_offline_diff_reports_missing_and_altered(self):
        d = FIXTURES / "offline_diff"
        report = gp0_diff.offline_from_dir(str(d))

        # The only anomaly frame is 104.
        f104 = next(r for r in report if r["frame"] == 104)
        self.assertEqual(len(f104["missing"]), 1,
                         f"expected exactly 1 MISSING, got {f104['missing']}")
        self.assertEqual(f104["missing"][0]["op"], "0x24")
        self.assertEqual(f104["missing"][0]["src"], "0x1DB400")

        self.assertEqual(len(f104["altered"]), 1,
                         f"expected exactly 1 ALTERED, got {f104['altered']}")
        self.assertEqual(f104["altered"][0]["op"], "0x24")
        self.assertEqual(f104["altered"][0]["src"], "0x1DB500")
        # The word delta must be w[0] (cmd|color).
        self.assertIn(0, f104["altered"][0]["word_idx"])


class EnvDiffTest(unittest.TestCase):
    """--env-diff: first divergent E1..E6 env word vs parity refs."""

    @classmethod
    def setUpClass(cls):
        _ensure_all()

    def test_env_diff_finds_first_e3_divergence(self):
        d = FIXTURES / "offline_env"
        report = gp0_diff.env_diff_report(str(d))

        f104 = next((r for r in report if r["frame"] == 104), None)
        self.assertIsNotNone(f104, f"no env-diff row for frame 104 in {report}")
        # First divergence must be E3 at index 2 (E1=0, E2=1, E3=2).
        self.assertEqual(f104["index"], 2)
        self.assertEqual(f104["op"], "0xE3")
        self.assertNotEqual(f104["f_val"], f104["ref_val"])
        # Frame 100 has no anomaly -> should NOT appear in the report.
        self.assertNotIn(100, [r["frame"] for r in report])


class SrcHistTest(unittest.TestCase):
    """--src-hist: bucketed src-address histogram."""

    @classmethod
    def setUpClass(cls):
        _ensure_all()

    def test_src_hist_buckets_match_fixture(self):
        d = FIXTURES / "offline_hist"
        hist = gp0_diff.src_histogram(str(d), bucket_size=0x100)

        # hist is a list of (bucket_addr, count) sorted by count desc.
        bucket_map = {addr: count for addr, count in hist}
        self.assertEqual(bucket_map.get(0x1DB400), 10)
        self.assertEqual(bucket_map.get(0x1EB400), 5)
        self.assertEqual(bucket_map.get(0x80123400), 3)
        # Top bucket must be 0x1DB400 (10 entries).
        self.assertEqual(hist[0][0], 0x1DB400)
        self.assertEqual(hist[0][1], 10)


class StripOverrideTest(unittest.TestCase):
    """--strip-lo / --strip-hi: arm range becomes parameterizable."""

    def test_default_uses_worldmap_constants(self):
        snaps, payload = gp0_diff.build_arm_payload(
            gp0_diff.TRACE_LO, gp0_diff.TRACE_HI)
        self.assertEqual(payload["lo"], "0x001DB400")
        self.assertEqual(payload["hi"], "0x001DB600")
        self.assertEqual(snaps, [0x1DB400, 0x1DB480, 0x1DB500, 0x1DB580])

    def test_override_derives_snaps_in_new_range(self):
        snaps, payload = gp0_diff.build_arm_payload(0x80100000, 0x80100200)
        self.assertEqual(payload["lo"], "0x80100000")
        self.assertEqual(payload["hi"], "0x80100200")
        # 4 evenly-spaced slots across the new range.
        self.assertEqual(snaps,
                         [0x80100000, 0x80100080, 0x80100100, 0x80100180])

    def test_arm_traps_consumes_override_via_query(self):
        """--arm --strip-lo/--strip-hi must hit the wire with the new range."""
        captured = []
        def fake_query(host, port, cmd):
            captured.append(cmd)
            return {"ok": True, "slot": len(captured)}
        with mock.patch.object(gp0_diff, "query", side_effect=fake_query):
            gp0_diff.arm_traps(lo=0x80100000, hi=0x80100200)

        wtrace = [c for c in captured if c.get("cmd") == "wtrace_arm"]
        self.assertEqual(len(wtrace), 1)
        self.assertEqual(wtrace[0]["lo"], "0x80100000")
        self.assertEqual(wtrace[0]["hi"], "0x80100200")

        snaps = [c for c in captured if c.get("cmd") == "set_snapshot"]
        self.assertEqual(len(snaps), 4)
        addrs = [c["addr"] for c in snaps]
        self.assertEqual(addrs,
                         ["0x80100000", "0x80100080", "0x80100100", "0x80100180"])


class CadenceTest(unittest.TestCase):
    """--cadence: histogram flagged frame % 120 and verdict."""

    @classmethod
    def setUpClass(cls):
        _ensure_all()

    def test_cadence_clustered_near_wake_boundary(self):
        d = FIXTURES / "cadence_clustered"
        verdict = gp0_diff.cadence_scan(str(d), period=120)["verdict"]
        self.assertIn("CLUSTERED at wake boundary", verdict)

    def test_cadence_uniform_when_spread(self):
        d = FIXTURES / "cadence_uniform"
        verdict = gp0_diff.cadence_scan(str(d), period=120)["verdict"]
        self.assertEqual(verdict, "UNIFORM")


class MainArgparseTest(unittest.TestCase):
    """--help must list all 6 new flags."""

    def test_help_lists_new_flags(self):
        with mock.patch.object(sys, "argv", ["gp0_diff.py", "--help"]):
            with self.assertRaises(SystemExit) as cm:
                gp0_diff.main()
            # argparse exits 0 on --help
            self.assertEqual(cm.exception.code, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
