#!/usr/bin/env python3
"""
Automated validation runner for the SX1280 FLRC KISS TNC link.

This script follows docs/link_validation_matrix.md. It assumes the bridge is
already running on both nodes, preferably with --quiet and the selected MTU.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import pathlib
import re
import statistics
import subprocess
import sys
from dataclasses import dataclass, field
from typing import Iterable, Optional


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
REPORT_DIR = REPO_ROOT / "reports"

FRAG_DATA = 114
IP_HDR_ICMP = 28
IP_HDR_UDP = 28


@dataclass
class CommandResult:
    name: str
    command: list[str]
    returncode: int
    stdout: str
    stderr: str

    @property
    def ok(self) -> bool:
        return self.returncode == 0


@dataclass
class PingResult:
    size: int
    count: int
    interval: float
    sent: int = 0
    received: int = 0
    duplicates: int = 0
    rtts: list[float] = field(default_factory=list)
    returncode: int = 0
    output: str = ""

    @property
    def ip_len(self) -> int:
        return self.size + IP_HDR_ICMP

    @property
    def frags(self) -> int:
        return max(1, (self.ip_len + FRAG_DATA - 1) // FRAG_DATA)

    @property
    def loss_pct(self) -> float:
        if self.sent <= 0:
            return 100.0
        return 100.0 * (self.sent - self.received) / self.sent

    def stat(self, fn) -> Optional[float]:
        return fn(self.rtts) if self.rtts else None

    @property
    def p95(self) -> Optional[float]:
        if not self.rtts:
            return None
        ordered = sorted(self.rtts)
        return ordered[min(int(0.95 * len(ordered)), len(ordered) - 1)]


@dataclass
class IperfResult:
    udp_len: int
    bitrate: str
    returncode: int
    sender_loss_pct: Optional[float]
    receiver_loss_pct: Optional[float]
    receiver_jitter_ms: Optional[float]
    receiver_received: Optional[int]
    receiver_lost: Optional[int]
    output: str

    @property
    def ip_len(self) -> int:
        return self.udp_len + IP_HDR_UDP

    @property
    def frags(self) -> int:
        return max(1, (self.ip_len + FRAG_DATA - 1) // FRAG_DATA)

    @property
    def ok(self) -> bool:
        return self.returncode == 0


def run_command(name: str, command: list[str], cwd: pathlib.Path = REPO_ROOT,
                timeout: Optional[float] = None) -> CommandResult:
    try:
        proc = subprocess.run(
            command,
            cwd=str(cwd),
            text=True,
            capture_output=True,
            timeout=timeout,
        )
        return CommandResult(name, command, proc.returncode, proc.stdout, proc.stderr)
    except FileNotFoundError as exc:
        return CommandResult(name, command, 127, "", str(exc))
    except subprocess.TimeoutExpired as exc:
        return CommandResult(
            name,
            command,
            124,
            exc.stdout or "",
            f"timeout after {timeout}s",
        )


def run_ping(target: str, size: int, count: int, interval: float,
             timeout_s: Optional[float]) -> PingResult:
    command = [
        "ping",
        "-c", str(count),
        "-i", str(interval),
        "-s", str(size),
        "-W", "2",
        target,
    ]
    timeout = timeout_s or max(10.0, count * interval + count * 2.5)
    cmd = run_command(f"ping-{size}", command, timeout=timeout)
    output = cmd.stdout + cmd.stderr

    result = PingResult(
        size=size,
        count=count,
        interval=interval,
        returncode=cmd.returncode,
        output=output,
    )

    for match in re.finditer(r"time[=<]([\d.]+)\s*ms", output):
        result.rtts.append(float(match.group(1)))

    stats = re.search(r"(\d+) packets transmitted, (\d+) received(?:, \+(\d+) duplicates)?", output)
    if stats:
        result.sent = int(stats.group(1))
        result.received = int(stats.group(2))
        result.duplicates = int(stats.group(3) or 0)
    else:
        result.sent = count
        result.received = len(result.rtts)
        result.duplicates = output.count("DUP!")

    return result


def parse_iperf_json(raw: str) -> tuple[Optional[float], Optional[float], Optional[float], Optional[int], Optional[int]]:
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        return None, None, None, None, None

    end = data.get("end", {})
    sender = end.get("sum_sent") or {}
    receiver = end.get("sum_received") or {}

    sender_loss = sender.get("lost_percent")
    receiver_loss = receiver.get("lost_percent")
    jitter = receiver.get("jitter_ms")
    received = receiver.get("packets")
    lost = receiver.get("lost_packets")
    return sender_loss, receiver_loss, jitter, received, lost


def run_iperf(target: str, udp_len: int, bitrate: str, duration: int,
              timeout_s: Optional[float]) -> IperfResult:
    command = [
        "iperf3",
        "-c", target,
        "-u",
        "-l", str(udp_len),
        "-b", bitrate,
        "-t", str(duration),
        "-J",
    ]
    timeout = timeout_s or duration + 15
    cmd = run_command(f"iperf-l{udp_len}-{bitrate}", command, timeout=timeout)
    output = cmd.stdout + cmd.stderr
    sender_loss, receiver_loss, jitter, received, lost = parse_iperf_json(cmd.stdout)

    return IperfResult(
        udp_len=udp_len,
        bitrate=bitrate,
        returncode=cmd.returncode,
        sender_loss_pct=sender_loss,
        receiver_loss_pct=receiver_loss,
        receiver_jitter_ms=jitter,
        receiver_received=received,
        receiver_lost=lost,
        output=output,
    )


def fmt_ms(value: Optional[float]) -> str:
    return f"{value:.1f}" if value is not None else "-"


def fmt_pct(value: Optional[float]) -> str:
    return f"{value:.1f}%" if value is not None else "-"


def command_line(command: Iterable[str]) -> str:
    return " ".join(command)


def write_report(
    path: pathlib.Path,
    args: argparse.Namespace,
    conformance: list[CommandResult],
    pings: list[PingResult],
    iperfs: list[IperfResult],
) -> None:
    lines: list[str] = []
    lines.append("# FLRC Link Validation Report")
    lines.append("")
    lines.append(f"- Timestamp: {dt.datetime.now().isoformat(timespec='seconds')}")
    lines.append(f"- Target: `{args.target}`")
    lines.append(f"- MTU profile: `{args.mtu}`")
    lines.append(f"- Git commit: `{current_commit()}`")
    lines.append("")

    if conformance:
        lines.append("## Phase 1: KISS Conformance")
        lines.append("")
        for item in conformance:
            status = "PASS" if item.ok else "FAIL"
            lines.append(f"- `{status}` `{command_line(item.command)}`")
        lines.append("")

    if pings:
        lines.append("## Phase 2/3: Ping Matrix")
        lines.append("")
        lines.append("| Ping -s | IP bytes | Frags | Sent | Recv | Dup | Loss | Min ms | Avg ms | P95 ms | Max ms |")
        lines.append("| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
        for item in pings:
            lines.append(
                f"| {item.size} | {item.ip_len} | {item.frags}F | {item.sent} | {item.received} | "
                f"{item.duplicates} | {item.loss_pct:.1f}% | {fmt_ms(item.stat(min))} | "
                f"{fmt_ms(item.stat(statistics.mean))} | {fmt_ms(item.p95)} | {fmt_ms(item.stat(max))} |"
            )
        lines.append("")

    if iperfs:
        lines.append("## Phase 4: UDP Sweep")
        lines.append("")
        lines.append("| UDP -l | IP bytes | Frags | Bitrate | Receiver loss | Jitter ms | Receiver packets | Lost packets |")
        lines.append("| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |")
        for item in iperfs:
            lines.append(
                f"| {item.udp_len} | {item.ip_len} | {item.frags}F | {item.bitrate} | "
                f"{fmt_pct(item.receiver_loss_pct)} | {fmt_ms(item.receiver_jitter_ms)} | "
                f"{item.receiver_received if item.receiver_received is not None else '-'} | "
                f"{item.receiver_lost if item.receiver_lost is not None else '-'} |"
            )
        lines.append("")

    lines.append("## Raw Command Output")
    lines.append("")
    for item in conformance:
        append_raw(lines, item.name, item.command, item.stdout + item.stderr)
    for item in pings:
        append_raw(lines, f"ping -s {item.size}", ["ping"], item.output)
    for item in iperfs:
        append_raw(lines, f"iperf3 -l {item.udp_len} -b {item.bitrate}", ["iperf3"], item.output)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def append_raw(lines: list[str], title: str, command: list[str], output: str) -> None:
    lines.append(f"### {title}")
    lines.append("")
    lines.append("```text")
    if command and command[0] not in ("ping", "iperf3"):
        lines.append(f"$ {command_line(command)}")
    lines.append(output.strip() or "<no output>")
    lines.append("```")
    lines.append("")


def current_commit() -> str:
    result = run_command("git-rev-parse", ["git", "rev-parse", "--short", "HEAD"])
    return result.stdout.strip() if result.ok else "unknown"


def run_conformance() -> list[CommandResult]:
    return [
        run_command("python-kiss-conformance", ["python3", "-m", "unittest", "tests.test_kiss_conformance"]),
        run_command("rust-kiss-tests", ["cargo", "test"], cwd=REPO_ROOT / "pi-daemon-rust"),
    ]


def parse_int_list(value: str) -> list[int]:
    return [int(part.strip()) for part in value.split(",") if part.strip()]


def parse_str_list(value: str) -> list[str]:
    return [part.strip() for part in value.split(",") if part.strip()]


def default_ping_sizes(mtu_profile: int) -> list[int]:
    safe = [56, 95, 157, 218]
    boundary = [280, 341, 403, 464]
    return safe if mtu_profile <= 246 else safe + boundary


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run the FLRC link validation matrix. For UDP tests, run 'iperf3 -s' on the peer first."
    )
    parser.add_argument("target", help="Peer tunnel IP address, e.g. 10.0.0.2")
    parser.add_argument("--mtu", type=int, default=238,
                        help="Operational MTU profile used for this run (default: 238)")
    parser.add_argument("--skip-conformance", action="store_true",
                        help="Skip local Python/Rust KISS tests")
    parser.add_argument("--skip-ping", action="store_true",
                        help="Skip ping matrix")
    parser.add_argument("--skip-iperf", action="store_true",
                        help="Skip UDP iperf3 sweep")
    parser.add_argument("--ping-count", type=int, default=50,
                        help="Ping packets per size (default: 50)")
    parser.add_argument("--ping-interval", type=float, default=1.0,
                        help="Ping interval in seconds for 1F/2F tests (default: 1.0)")
    parser.add_argument("--ping-sizes", default=None,
                        help="Comma-separated ping -s values")
    parser.add_argument("--iperf-duration", type=int, default=10,
                        help="iperf3 UDP test duration in seconds (default: 10)")
    parser.add_argument("--iperf-lengths", default="95,218",
                        help="Comma-separated UDP payload lengths (default: 95,218)")
    parser.add_argument("--bitrates", default="8k,16k,24k,32k",
                        help="Comma-separated UDP bitrates (default: 8k,16k,24k,32k)")
    parser.add_argument("--output", default=None,
                        help="Markdown report path (default: reports/link_validation_<timestamp>.md)")
    parser.add_argument("--fail-fast", action="store_true",
                        help="Stop after the first failed local conformance command")
    args = parser.parse_args()

    timestamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    report_path = pathlib.Path(args.output) if args.output else REPORT_DIR / f"link_validation_{timestamp}.md"
    if not report_path.is_absolute():
        report_path = REPO_ROOT / report_path

    conformance: list[CommandResult] = []
    pings: list[PingResult] = []
    iperfs: list[IperfResult] = []

    if not args.skip_conformance:
        print("[validation] Phase 1: KISS conformance")
        conformance = run_conformance()
        for item in conformance:
            print(f"[validation] {'PASS' if item.ok else 'FAIL'} {command_line(item.command)}")
            if args.fail_fast and not item.ok:
                write_report(report_path, args, conformance, pings, iperfs)
                print(f"[validation] report: {report_path}")
                return 1

    if not args.skip_ping:
        print("[validation] Phase 2/3: ping matrix")
        sizes = parse_int_list(args.ping_sizes) if args.ping_sizes else default_ping_sizes(args.mtu)
        for size in sizes:
            interval = args.ping_interval
            if size >= 280:
                interval = max(interval, 1.5 if size <= 341 else 2.0)
            result = run_ping(args.target, size, args.ping_count, interval, None)
            pings.append(result)
            print(
                f"[validation] ping -s {size:3d} {result.frags}F "
                f"loss={result.loss_pct:.1f}% dup={result.duplicates} "
                f"avg={fmt_ms(result.stat(statistics.mean))}ms"
            )

    if not args.skip_iperf:
        print("[validation] Phase 4: UDP iperf3 sweep")
        lengths = parse_int_list(args.iperf_lengths)
        bitrates = parse_str_list(args.bitrates)
        for udp_len in lengths:
            for bitrate in bitrates:
                result = run_iperf(args.target, udp_len, bitrate, args.iperf_duration, None)
                iperfs.append(result)
                print(
                    f"[validation] iperf -l {udp_len:3d} -b {bitrate:>4s} "
                    f"{result.frags}F rx_loss={fmt_pct(result.receiver_loss_pct)} "
                    f"jitter={fmt_ms(result.receiver_jitter_ms)}ms"
                )

    write_report(report_path, args, conformance, pings, iperfs)
    print(f"[validation] report: {report_path}")

    failed_conformance = any(not item.ok for item in conformance)
    duplicate_pings = any(item.duplicates > 0 for item in pings)
    safe_ping_loss = any(item.frags <= 2 and item.loss_pct > 0 for item in pings)
    safe_iperf_loss = any(item.frags <= 2 and (item.receiver_loss_pct or 0.0) > 0 for item in iperfs)

    if failed_conformance or duplicate_pings or safe_ping_loss or safe_iperf_loss:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
