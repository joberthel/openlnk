#!/usr/bin/env python3
"""Bidirectional Serial MITM Sniffer & Logger for MS-DOS INTERLNK/INTERSVR.

Relays traffic transparently between a Client VM (INTERLNK.EXE) and a
Server VM (INTERSVR.EXE) while recording time-synchronized streams to:
  1. Raw binary capture (.bin)
  2. Timed hexlog (.hexlog): "[<ts_ns>] [<src>-><dst>] <hex>"
  3. Wireshark nanosecond PCAP (.pcap, DLT_USER0=147, 1-byte direction prefix)

Supports both external serial ports/PTYs (via pyserial) and self-contained
POSIX virtual PTY pairs (--create-ptys) for Linux/macOS without requiring socat.
"""

from __future__ import annotations

import argparse
import os
import select
import signal
import struct
import sys
import threading
import time
from pathlib import Path
from typing import BinaryIO, Optional, TextIO

try:
    import serial  # type: ignore[import-untyped]
except ImportError:
    serial = None

try:
    import pty
    import termios
    import tty
except ImportError:  # Windows
    pty = None  # type: ignore[assignment]
    termios = None  # type: ignore[assignment]
    tty = None  # type: ignore[assignment]


PCAP_MAGIC_NS = 0xA1B23C4D
PCAP_VERSION_MAJOR = 2
PCAP_VERSION_MINOR = 4
PCAP_SNAPLEN = 65535
PCAP_DLT_USER0 = 147

DIR_CLIENT_TO_SERVER = 0x00
DIR_SERVER_TO_CLIENT = 0x01


class PcapWriter:
    """Minimal, dependency-free nanosecond-resolution Wireshark PCAP writer."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self._fp: BinaryIO = path.open("wb")
        self._lock = threading.Lock()
        # Global PCAP header (24 bytes, little-endian, nanosecond timestamps)
        header = struct.pack(
            "<IHHiIII",
            PCAP_MAGIC_NS,
            PCAP_VERSION_MAJOR,
            PCAP_VERSION_MINOR,
            0,  # thiszone
            0,  # sigfigs
            PCAP_SNAPLEN,
            PCAP_DLT_USER0,
        )
        self._fp.write(header)
        self._fp.flush()

    def write_packet(self, ts_ns: int, source_name: str, data: bytes) -> None:
        direction = (
            DIR_CLIENT_TO_SERVER
            if source_name.upper().startswith("CLIENT")
            else DIR_SERVER_TO_CLIENT
        )
        payload = bytes([direction]) + data
        ts_sec = ts_ns // 1_000_000_000
        ts_nsec = ts_ns % 1_000_000_000
        rec_hdr = struct.pack("<IIII", ts_sec, ts_nsec, len(payload), len(payload))
        with self._lock:
            self._fp.write(rec_hdr)
            self._fp.write(payload)
            self._fp.flush()

    def close(self) -> None:
        with self._lock:
            if not self._fp.closed:
                self._fp.close()


class CaptureSink:
    """Thread-safe multiplexer writing .bin, .hexlog, and .pcap simultaneously."""

    def __init__(self, out_dir: Path, session_name: str) -> None:
        out_dir.mkdir(parents=True, exist_ok=True)
        self.bin_path = out_dir / f"{session_name}.bin"
        self.hexlog_path = out_dir / f"{session_name}.hexlog"
        self.pcap_path = out_dir / f"{session_name}.pcap"

        self._lock = threading.Lock()
        self._bin_fp: BinaryIO = self.bin_path.open("wb")
        self._hex_fp: TextIO = self.hexlog_path.open("w", encoding="utf-8")
        self.pcap = PcapWriter(self.pcap_path)
        self.bytes_c2s = 0
        self.bytes_s2c = 0

    def record(self, ts_ns: int, source_name: str, dest_name: str, data: bytes) -> None:
        if not data:
            return
        with self._lock:
            self._bin_fp.write(data)
            self._bin_fp.flush()
            self._hex_fp.write(f"[{ts_ns}] [{source_name}->{dest_name}] {data.hex()}\n")
            self._hex_fp.flush()
            if source_name.upper().startswith("CLIENT"):
                self.bytes_c2s += len(data)
            else:
                self.bytes_s2c += len(data)
        self.pcap.write_packet(ts_ns, source_name, data)

    def close(self) -> None:
        with self._lock:
            if not self._bin_fp.closed:
                self._bin_fp.close()
            if not self._hex_fp.closed:
                self._hex_fp.close()
        self.pcap.close()


def capture_frame(
    source_name: str,
    dest_name: str,
    src_conn: object,
    dst_conn: object,
    sink: CaptureSink,
    stop_event: threading.Event,
) -> None:
    """Relay bytes from src_conn to dst_conn (pyserial-compatible objects)."""
    while not stop_event.is_set():
        waiting = getattr(src_conn, "in_waiting", 0)
        data = src_conn.read(waiting or 1)  # type: ignore[attr-defined]
        if data:
            ts_ns = time.time_ns()
            dst_conn.write(data)  # type: ignore[attr-defined]
            dst_conn.flush()  # type: ignore[attr-defined]
            sink.record(ts_ns, source_name, dest_name, data)


def _configure_raw_pty(fd: int) -> None:
    if tty is not None and termios is not None:
        tty.setraw(fd)


def create_pty_pair() -> tuple[int, int, str]:
    """Create a raw POSIX PTY master/slave pair and return (master_fd, slave_fd, slave_path)."""
    if pty is None:
        raise RuntimeError("--create-ptys is only supported on POSIX systems (Linux/macOS).")
    master_fd, slave_fd = pty.openpty()
    _configure_raw_pty(master_fd)
    _configure_raw_pty(slave_fd)
    slave_path = os.ttyname(slave_fd)
    return master_fd, slave_fd, slave_path


def relay_fd_pair(
    client_master_fd: int,
    server_master_fd: int,
    sink: CaptureSink,
    stop_event: threading.Event,
    poll_timeout: float = 0.05,
) -> None:
    """Bidirectional non-blocking select() relay over two POSIX file descriptors."""
    fds = [client_master_fd, server_master_fd]
    while not stop_event.is_set():
        readable, _, _ = select.select(fds, [], [], poll_timeout)
        for fd in readable:
            try:
                data = os.read(fd, 4096)
            except OSError:
                continue
            if not data:
                continue
            ts_ns = time.time_ns()
            if fd == client_master_fd:
                os.write(server_master_fd, data)
                sink.record(ts_ns, "CLIENT", "SERVER", data)
            else:
                os.write(client_master_fd, data)
                sink.record(ts_ns, "SERVER", "CLIENT", data)


def run_serial_mode(
    client_port: str,
    server_port: str,
    baud: int,
    sink: CaptureSink,
    stop_event: threading.Event,
) -> None:
    if serial is None:
        raise RuntimeError(
            "pyserial is required for --client-port/--server-port mode. "
            "Install with: pip install pyserial (or use --create-ptys on POSIX)."
        )

    cli_ser = serial.Serial(client_port, baudrate=baud, timeout=0.05)
    srv_ser = serial.Serial(server_port, baudrate=baud, timeout=0.05)
    try:
        t_c2s = threading.Thread(
            target=capture_frame,
            args=("CLIENT", "SERVER", cli_ser, srv_ser, sink, stop_event),
            daemon=True,
        )
        t_s2c = threading.Thread(
            target=capture_frame,
            args=("SERVER", "CLIENT", srv_ser, cli_ser, sink, stop_event),
            daemon=True,
        )
        t_c2s.start()
        t_s2c.start()
        while not stop_event.is_set():
            time.sleep(0.1)
        t_c2s.join(timeout=1.0)
        t_s2c.join(timeout=1.0)
    finally:
        cli_ser.close()
        srv_ser.close()


def create_fifo_pair(prefix: Path) -> tuple[Path, Path, int, int]:
    """Create POSIX .in and .out FIFOs for 86Box Named Pipe (COM) Client mode.

    In 86Box Client mode (mode=1), 86Box writes to <prefix>.in and reads from <prefix>.out.
    Therefore the MITM proxy reads from .in and writes to .out, opening both with
    O_RDWR | O_NONBLOCK so neither end ever deadlocks on open or disconnect.
    """
    in_path = Path(f"{prefix}.in")
    out_path = Path(f"{prefix}.out")
    for p in (in_path, out_path):
        if p.exists():
            p.unlink()
        os.mkfifo(p, 0o600)
    read_fd = os.open(str(in_path), os.O_RDWR | os.O_NONBLOCK)
    write_fd = os.open(str(out_path), os.O_RDWR | os.O_NONBLOCK)
    return in_path, out_path, read_fd, write_fd


def relay_pipe_quad(
    client_read_fd: int,
    client_write_fd: int,
    server_read_fd: int,
    server_write_fd: int,
    sink: CaptureSink,
    stop_event: threading.Event,
    poll_timeout: float = 0.05,
) -> None:
    """Bidirectional non-blocking select() relay over two separate in/out FIFO pairs."""
    read_fds = [client_read_fd, server_read_fd]
    while not stop_event.is_set():
        readable, _, _ = select.select(read_fds, [], [], poll_timeout)
        for fd in readable:
            try:
                data = os.read(fd, 4096)
            except OSError:
                continue
            if not data:
                continue
            ts_ns = time.time_ns()
            if fd == client_read_fd:
                os.write(server_write_fd, data)
                sink.record(ts_ns, "CLIENT", "SERVER", data)
            else:
                os.write(client_write_fd, data)
                sink.record(ts_ns, "SERVER", "CLIENT", data)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Bidirectional Serial MITM Sniffer for MS-DOS INTERLNK/INTERSVR"
    )
    parser.add_argument("--client-port", help="Serial device or PTY connected to Client VM")
    parser.add_argument("--server-port", help="Serial device or PTY connected to Server VM")
    parser.add_argument(
        "--create-ptys",
        action="store_true",
        help="Automatically allocate two POSIX virtual PTYs for Client & Server VMs",
    )
    parser.add_argument(
        "--create-pipes",
        action="store_true",
        help="Create POSIX .in/.out FIFO pairs for 86Box Named Pipe (COM) mode (asserts virtual DSR/CTS)",
    )
    parser.add_argument(
        "--pipe-dir",
        type=Path,
        default=Path("/tmp"),
        help="Directory for --create-pipes FIFOs (default: /tmp)",
    )
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("analysis/traces"),
        help="Directory for .bin, .hexlog, and .pcap outputs",
    )
    parser.add_argument(
        "--session-name",
        default="capture",
        help="Base filename for capture outputs (default: capture)",
    )
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    if not args.create_ptys and not args.create_pipes and (not args.client_port or not args.server_port):
        parser.error("Specify --create-ptys, --create-pipes, OR both --client-port and --server-port.")

    stop_event = threading.Event()

    def _handle_sig(_signum: int, _frame: object) -> None:
        stop_event.set()

    signal.signal(signal.SIGINT, _handle_sig)
    signal.signal(signal.SIGTERM, _handle_sig)

    sink = CaptureSink(args.out_dir, args.session_name)
    try:
        if args.create_pipes:
            args.pipe_dir.mkdir(parents=True, exist_ok=True)
            c_prefix = args.pipe_dir / "openlnk_cli"
            s_prefix = args.pipe_dir / "openlnk_srv"
            c_in, c_out, c_rfd, c_wfd = create_fifo_pair(c_prefix)
            s_in, s_out, s_rfd, s_wfd = create_fifo_pair(s_prefix)
            print(f"[serial_mitm] Client Pipe Prefix: {c_prefix} (.in/.out)", flush=True)
            print(f"[serial_mitm] Server Pipe Prefix: {s_prefix} (.in/.out)", flush=True)
            print(f"[serial_mitm] Logging to: {sink.hexlog_path} / {sink.pcap_path}", flush=True)
            try:
                relay_pipe_quad(c_rfd, c_wfd, s_rfd, s_wfd, sink, stop_event)
            finally:
                for fd in (c_rfd, c_wfd, s_rfd, s_wfd):
                    try:
                        os.close(fd)
                    except OSError:
                        pass
                for p in (c_in, c_out, s_in, s_out):
                    p.unlink(missing_ok=True)
        elif args.create_ptys:
            c_master, c_slave, c_path = create_pty_pair()
            s_master, s_slave, s_path = create_pty_pair()
            print(f"[serial_mitm] Client PTY: {c_path}", flush=True)
            print(f"[serial_mitm] Server PTY: {s_path}", flush=True)
            print(f"[serial_mitm] Logging to: {sink.hexlog_path} / {sink.pcap_path}", flush=True)
            try:
                relay_fd_pair(c_master, s_master, sink, stop_event)
            finally:
                for fd in (c_master, c_slave, s_master, s_slave):
                    try:
                        os.close(fd)
                    except OSError:
                        pass
        else:
            print(
                f"[serial_mitm] Bridging {args.client_port} <-> {args.server_port} @ {args.baud} bps",
                flush=True,
            )
            run_serial_mode(args.client_port, args.server_port, args.baud, sink, stop_event)
    finally:
        sink.close()
        print(
            f"[serial_mitm] Stopped. Captured C->S={sink.bytes_c2s}B, S->C={sink.bytes_s2c}B",
            flush=True,
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
