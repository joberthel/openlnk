#!/usr/bin/env python3
"""Unit tests for OpenLNK Python tooling:
  - tools/sniffer/serial_mitm.py (PCAP/hexlog/bin writers + live POSIX PTY relay)
  - analysis/verifiers/verify_crc.py (deterministic CRC verification)
  - analysis/setup_ghidra.py (headless command construction)
  - tools/ci/check_cleanroom_isolation.py (positive & negative isolation guardrails)
"""

from __future__ import annotations

import json
import os
import re
import struct
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from analysis import setup_ghidra
from analysis.verifiers import verify_crc
from tools.ci import check_cleanroom_isolation as isolation
from tools.ci import verify_dos_binaries
from tools.sniffer import serial_mitm


class TestVerifyCrc(unittest.TestCase):
    def test_standard_ccitt_false_vector(self) -> None:
        self.assertEqual(verify_crc.crc16_ccitt_false(b"123456789"), 0x29B1)
        self.assertTrue(verify_crc.check_hypothesis(b"123456789", 0x29B1))
        self.assertFalse(verify_crc.check_hypothesis(b"123456789", 0x0000))
        self.assertIn("crc16_ccitt_false", verify_crc.probe_all_algorithms(b"123456789", 0x29B1))

    def test_verify_hexlog_frames(self) -> None:
        body = bytes([0x01, 0x01, 0x03, 0x00, 0x22, 0x06, 0x04])
        crc = verify_crc.crc16_ccitt_false(body)
        good_frame = b"\x55\xaa" + body + struct.pack("<H", crc)
        bad_frame = b"\x55\xaa" + body + struct.pack("<H", crc ^ 0x0001)
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "sample.hexlog"
            p.write_text(
                f"[1000] [CLIENT->SERVER] {good_frame.hex()}\n"
                f"[2000] [SERVER->CLIENT] {bad_frame.hex()}\n",
                encoding="utf-8",
            )
            passed, failed = verify_crc.verify_hexlog_frames(p)
            self.assertEqual((passed, failed), (1, 1))


class TestSetupGhidra(unittest.TestCase):
    def test_build_headless_command(self) -> None:
        cmd = setup_ghidra.build_headless_command(
            Path("/opt/ghidra/support/analyzeHeadless"),
            Path("/tmp/proj"),
            "interlnk_re",
            Path("/tmp/INTERLNK.EXE"),
            Path("/tmp/scripts"),
        )
        self.assertIn("-processor", cmd)
        self.assertIn("x86:LE:16:Real Mode", cmd)
        self.assertIn("setup_dos_driver.py", cmd)


class TestSerialMitm(unittest.TestCase):
    def test_capture_sink_and_pcap_format(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            sink = serial_mitm.CaptureSink(Path(tmp), "unit_test")
            sink.record(1_700_000_000_123_456_789, "CLIENT", "SERVER", b"\x55\xaa\x01")
            sink.record(1_700_000_000_223_456_789, "SERVER", "CLIENT", b"\x55\xaa\x81")
            sink.close()

            self.assertEqual(sink.bin_path.read_bytes(), b"\x55\xaa\x01\x55\xaa\x81")
            lines = sink.hexlog_path.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 2)
            self.assertIn("[CLIENT->SERVER] 55aa01", lines[0])
            self.assertIn("[SERVER->CLIENT] 55aa81", lines[1])

            pcap_bytes = sink.pcap_path.read_bytes()
            magic, maj, min_, _, _, snap, linktype = struct.unpack("<IHHiIII", pcap_bytes[:24])
            self.assertEqual(magic, serial_mitm.PCAP_MAGIC_NS)
            self.assertEqual((maj, min_), (2, 4))
            self.assertEqual(snap, 65535)
            self.assertEqual(linktype, serial_mitm.PCAP_DLT_USER0)
            # First record header + direction byte + 3 payload bytes
            sec, nsec, incl, orig = struct.unpack("<IIII", pcap_bytes[24:40])
            self.assertEqual(sec, 1_700_000_000)
            self.assertEqual(nsec, 123_456_789)
            self.assertEqual((incl, orig), (4, 4))
            self.assertEqual(pcap_bytes[40:44], b"\x00\x55\xaa\x01")

    @unittest.skipIf(os.name != "posix", "POSIX PTY test")
    def test_live_posix_pty_bidirectional_relay(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            sink = serial_mitm.CaptureSink(Path(tmp), "pty_relay")
            c_master, c_slave, _ = serial_mitm.create_pty_pair()
            s_master, s_slave, _ = serial_mitm.create_pty_pair()
            stop_event = threading.Event()
            t = threading.Thread(
                target=serial_mitm.relay_fd_pair,
                args=(c_master, s_master, sink, stop_event, 0.01),
                daemon=True,
            )
            t.start()
            try:
                os.write(c_slave, b"HELLO_SRV")
                time.sleep(0.05)
                got_at_srv = os.read(s_slave, 64)
                self.assertEqual(got_at_srv, b"HELLO_SRV")

                os.write(s_slave, b"ACK_CLI")
                time.sleep(0.05)
                got_at_cli = os.read(c_slave, 64)
                self.assertEqual(got_at_cli, b"ACK_CLI")
            finally:
                stop_event.set()
                t.join(timeout=1.0)
                sink.close()
                for fd in (c_master, c_slave, s_master, s_slave):
                    os.close(fd)

            self.assertEqual(sink.bytes_c2s, len(b"HELLO_SRV"))
            self.assertEqual(sink.bytes_s2c, len(b"ACK_CLI"))

    @unittest.skipIf(os.name != "posix", "POSIX FIFO test")
    def test_live_posix_fifo_pipe_relay(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tdir = Path(tmp)
            sink = serial_mitm.CaptureSink(tdir, "fifo_relay")
            c_in, c_out, c_rfd, c_wfd = serial_mitm.create_fifo_pair(tdir / "cli")
            s_in, s_out, s_rfd, s_wfd = serial_mitm.create_fifo_pair(tdir / "srv")
            stop_event = threading.Event()
            t = threading.Thread(
                target=serial_mitm.relay_pipe_quad,
                args=(c_rfd, c_wfd, s_rfd, s_wfd, sink, stop_event, 0.01),
                daemon=True,
            )
            t.start()
            try:
                # 86Box writes to .out and reads from .in
                cli_tx = os.open(str(c_out), os.O_WRONLY | os.O_NONBLOCK)
                srv_rx = os.open(str(s_in), os.O_RDONLY | os.O_NONBLOCK)
                srv_tx = os.open(str(s_out), os.O_WRONLY | os.O_NONBLOCK)
                cli_rx = os.open(str(c_in), os.O_RDONLY | os.O_NONBLOCK)
                try:
                    os.write(cli_tx, b"\x03RCOPY")
                    time.sleep(0.05)
                    self.assertEqual(os.read(srv_rx, 64), b"\x03RCOPY")

                    os.write(srv_tx, b"C:\\>")
                    time.sleep(0.05)
                    self.assertEqual(os.read(cli_rx, 64), b"C:\\>")
                finally:
                    for fd in (cli_tx, srv_rx, srv_tx, cli_rx):
                        os.close(fd)
            finally:
                stop_event.set()
                t.join(timeout=1.0)
                sink.close()
                for fd in (c_rfd, c_wfd, s_rfd, s_wfd):
                    os.close(fd)

            self.assertEqual(sink.bytes_c2s, len(b"\x03RCOPY"))
            self.assertEqual(sink.bytes_s2c, len(b"C:\\>"))

    @unittest.skipIf(os.name != "posix", "POSIX pacing test")
    def test_paced_write_persists_across_single_byte_calls(self) -> None:
        rfd, wfd = os.pipe()
        try:
            t0 = time.perf_counter_ns()
            nxt = 0
            for _ in range(8):
                nxt = serial_mitm._write_paced(wfd, b"X", 1000, nxt)
            elapsed_ms = (time.perf_counter_ns() - t0) / 1_000_000.0
            self.assertGreaterEqual(elapsed_ms, 6.5)
            self.assertEqual(os.read(rfd, 16), b"XXXXXXXX")
        finally:
            os.close(rfd)
            os.close(wfd)


class TestCleanroomIsolation(unittest.TestCase):
    def test_repo_passes_cleanroom_isolation(self) -> None:
        violations = isolation.run_all_checks(REPO_ROOT)
        self.assertEqual(violations, [], f"Unexpected violations: {violations}")

    def test_catches_malloc_and_float_in_core(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "cleanroom" / "src").mkdir(parents=True)
            (root / "cleanroom" / "src" / "bad.c").write_text(
                "/* comment mentioning malloc and float is allowed */\n"
                "void f(void) { float x = 1.0f; void *p = malloc(16); }\n",
                encoding="utf-8",
            )
            violations = isolation.check_core_c_constraints(root)
            self.assertEqual(len(violations), 2)

    def test_catches_analysis_ref_in_cleanroom(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "cleanroom" / "src").mkdir(parents=True)
            (root / "cleanroom" / "src" / "leaky.c").write_text(
                '#include "../../analysis/header.h"\n',
                encoding="utf-8",
            )
            violations = isolation.check_cleanroom_references(root)
            self.assertTrue(any("CLEANROOM_NO_ANALYSIS_REF" in v for v in violations))

    def test_catches_dual_agent_commit_boundary(self) -> None:
        violations = isolation.check_dual_agent_boundary(
            ["analysis/setup_ghidra.py", "cleanroom/src/openlnk.c"]
        )
        self.assertEqual(len(violations), 1)
        self.assertIn("DUAL_AGENT_BOUNDARY", violations[0])
        self.assertEqual(
            isolation.check_dual_agent_boundary(["specs/interlnk_wire.ksy", "cleanroom/src/openlnk.c"]),
            [],
        )


class TestReleaseMetadataAndDosVerifier(unittest.TestCase):
    def test_release_version_files_are_synchronized(self) -> None:
        manifest = json.loads((REPO_ROOT / ".release-please-manifest.json").read_text(encoding="utf-8"))
        version = manifest["."]
        major, minor, patch = (int(part) for part in version.split("."))

        self.assertEqual((REPO_ROOT / "version.txt").read_text(encoding="utf-8").strip(), version)

        header = (REPO_ROOT / "cleanroom/include/openlnk/version.h").read_text(encoding="utf-8")
        self.assertRegex(header, rf"#define\s+OLNK_VERSION_MAJOR\s+{major}\b")
        self.assertRegex(header, rf"#define\s+OLNK_VERSION_MINOR\s+{minor}\b")
        self.assertRegex(header, rf"#define\s+OLNK_VERSION_PATCH\s+{patch}\b")
        self.assertIn(f'#define OLNK_VERSION_STRING "{version}"', header)

        cmake = (REPO_ROOT / "cleanroom/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertRegex(cmake, rf"project\(libopenlnk VERSION {re.escape(version)} LANGUAGES C\)")

        idf = (REPO_ROOT / "esp32/idf_component.yml").read_text(encoding="utf-8")
        self.assertRegex(idf, re.compile(rf'^version:\s*"{re.escape(version)}"', re.M))

        pio = json.loads((REPO_ROOT / "esp32/library.json").read_text(encoding="utf-8"))
        self.assertEqual(pio["version"], version)

        arduino = (REPO_ROOT / "esp32/library.properties").read_text(encoding="utf-8")
        self.assertRegex(arduino, re.compile(rf"^version={re.escape(version)}$", re.M))

    def test_verify_dos_binaries_validates_headers_and_budgets(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            bdir = Path(tmp)
            (bdir / "OPENSVR.EXE").write_bytes(b"MZ" + b"\x00" * 1022)
            (bdir / "OPENSVR.MAP").write_text("Open Watcom Linker\n", encoding="utf-8")
            (bdir / "OPENLNK.EXE").write_bytes(b"MZ" + b"\x00" * 1022)
            (bdir / "OPENLNKX.MAP").write_text("Open Watcom Linker\n", encoding="utf-8")

            sys_hdr = struct.pack("<IHHHB", 0xFFFFFFFF, 0x2842, 0x0012, 0x001D, 0) + b"OPENLNK"
            sys_body = sys_hdr + b"\x00" * (256 - len(sys_hdr))
            (bdir / "OPENLNK.SYS").write_bytes(sys_body)
            (bdir / "OPENLNK.MAP").write_text(
                "Open Watcom Linker\n"
                "0100  _sys_bss_start\n"
                "0400  _sys_resident_end\n",
                encoding="utf-8",
            )

            lines = verify_dos_binaries.verify_dos_binaries(bdir)
            self.assertEqual(len(lines), 3)

            # Reject accidental clibs.lib linkage
            (bdir / "OPENSVR.MAP").write_text("clibs.lib(cstart)\n", encoding="utf-8")
            with self.assertRaises(AssertionError):
                verify_dos_binaries.verify_dos_binaries(bdir)


if __name__ == "__main__":
    unittest.main()
