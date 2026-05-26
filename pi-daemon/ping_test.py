#!/usr/bin/env python3
"""
ping_test.py — structured ping diagnostic for the SX1280 FLRC KISS TNC link.

Tests packet loss and RTT at sizes spanning 1–4 radio fragments.
Run on the Pi while kiss_tun.py is active (tun0 must be up).

Usage:
    python3 ping_test.py 10.0.0.2
    python3 ping_test.py 10.0.0.2 --count 50 --interval 0.5
    python3 ping_test.py 10.0.0.2 --sizes 56,97,400,472
"""

import argparse
import math
import re
import statistics
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Dict, List, Optional

# Must match firmware config.h / Framing.h
FRAG_DATA = 123   # data payload bytes per radio fragment (127-byte FLRC packet - 4-byte link header)
IP_HDR    = 28    # IPv4 (20) + ICMP (8) fixed overhead


def frag_count(ping_s: int) -> int:
    return max(1, math.ceil((ping_s + IP_HDR) / FRAG_DATA))


@dataclass
class Result:
    size:     int
    frags:    int
    sent:     int = 0
    received: int = 0
    rtts:     List[float] = field(default_factory=list)

    @property
    def loss_pct(self) -> float:
        return 100.0 * (self.sent - self.received) / self.sent if self.sent else 100.0

    def stat(self, fn) -> Optional[float]:
        return fn(self.rtts) if self.rtts else None

    @property
    def p95(self) -> Optional[float]:
        if not self.rtts:
            return None
        s = sorted(self.rtts)
        return s[min(int(0.95 * len(s)), len(s) - 1)]


def run_ping(target: str, size: int, count: int, interval: float) -> Result:
    frags = frag_count(size)
    ip_len = size + IP_HDR
    print(f"  -s {size:4d}  ({ip_len:4d}B  {frags}F)  ", end='', flush=True)

    r = Result(size=size, frags=frags, sent=count)
    dots = 0

    cmd = ['ping', '-c', str(count), '-i', str(interval),
           '-s', str(size), '-W', '2', target]
    try:
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        )
    except FileNotFoundError:
        print("ERROR: ping not found")
        return r

    for line in proc.stdout:
        m = re.search(r'time[=<]([\d.]+)\s*ms', line)
        if m:
            r.rtts.append(float(m.group(1)))
            print('.', end='', flush=True)
            dots += 1
        m = re.search(r'(\d+) packets transmitted, (\d+)', line)
        if m:
            r.sent, r.received = int(m.group(1)), int(m.group(2))

    proc.wait()

    # Pad lost-packet slots with spaces so columns stay aligned
    while dots < r.sent:
        print(' ', end='', flush=True)
        dots += 1

    mean = r.stat(statistics.mean)
    rtt_str = f"avg {mean:.1f} ms" if mean is not None else "all lost  "
    print(f"  {r.loss_pct:5.1f}% loss  {rtt_str}")
    return r


def fmt(v: Optional[float]) -> str:
    return f"{v:7.1f}" if v is not None else "      —"


def bar(value: float, max_val: float, width: int = 40) -> str:
    if max_val == 0:
        return ""
    return "█" * max(1, round(value / max_val * width))


def print_report(results: List[Result]) -> None:
    print()
    print("═" * 80)
    print(f"{'SIZE':>6}  {'IP B':>5}  {'F':>2}  {'LOSS':>7}  "
          f"{'MIN':>7}  {'MEDIAN':>7}  {'MEAN':>7}  {'P95':>7}  {'MAX':>7}")
    print("─" * 80)
    for r in results:
        print(
            f"{r.size:>6}  {r.size+IP_HDR:>5}  {r.frags:>2}  "
            f"{r.loss_pct:>6.1f}%  "
            f"{fmt(r.stat(min))}  {fmt(r.stat(statistics.median))}  "
            f"{fmt(r.stat(statistics.mean))}  {fmt(r.p95)}  {fmt(r.stat(max))}"
        )
    print("─" * 80)
    print(f"{'':6}  {'':5}  {'':2}  {'':7}  "
          f"{'':>7}  {'':>7}  {'':>7}  {'':>7}  {'ms':>7}")
    print()

    # ── Loss by fragment count ────────────────────────────────────────────────
    by_frags: Dict[int, List[Result]] = defaultdict(list)
    for r in results:
        by_frags[r.frags].append(r)

    print("Loss by fragment count:")
    p1_success: Optional[float] = None
    for nf in sorted(by_frags.keys()):
        rs = by_frags[nf]
        tot_sent = sum(r.sent for r in rs)
        tot_recv = sum(r.received for r in rs)
        actual_loss = 100.0 * (tot_sent - tot_recv) / tot_sent if tot_sent else 100.0
        actual_success = 1.0 - actual_loss / 100.0

        if nf == 1:
            p1_success = actual_success

        note = ""
        if nf > 1 and p1_success is not None and p1_success > 0:
            exp_loss = (1.0 - p1_success ** nf) * 100.0
            if exp_loss > 0.1:
                ratio = actual_loss / exp_loss
                note = f"  (expected {exp_loss:.1f}%"
                if ratio > 1.5:
                    note += f",  {ratio:.1f}× worse  — burst or collision loss)"
                elif ratio < 0.7:
                    note += f",  {ratio:.1f}× better — correlated success)"
                else:
                    note += ",  matches independent-fragment model)"

        label = f"  {nf}F  (≤{FRAG_DATA*nf:3d}B IP)"
        print(f"{label:<22}  {actual_loss:5.1f}% loss{note}")

    # ── Median RTT bar chart ──────────────────────────────────────────────────
    print()
    print("Median RTT by fragment count:")
    medians: Dict[int, float] = {}
    for nf in sorted(by_frags.keys()):
        all_rtts = [rtt for r in by_frags[nf] for rtt in r.rtts]
        if all_rtts:
            medians[nf] = statistics.median(all_rtts)

    if medians:
        max_med = max(medians.values())
        for nf, med in sorted(medians.items()):
            print(f"  {nf}F: {med:6.1f} ms  {bar(med, max_med)}")

    print()


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Multi-size ping diagnostic for the SX1280 FLRC KISS TNC link"
    )
    ap.add_argument("target",
                    help="Target IP address (e.g. 10.0.0.2)")
    ap.add_argument("--count",    type=int,   default=50,
                    help="Pings per size (default: 50)")
    ap.add_argument("--interval", type=float, default=0.5,
                    help="Seconds between pings (default: 0.5)")
    ap.add_argument("--sizes",    type=str,   default=None,
                    help="Comma-separated -s values; default covers 1–4 frags")
    args = ap.parse_args()

    max_s = FRAG_DATA * 4 - IP_HDR  # 472

    if args.sizes:
        sizes = sorted(set(int(s.strip()) for s in args.sizes.split(',')))
    else:
        # Two sizes per fragment count: mid-range and ceiling.
        # This reveals whether loss is flat within a fragment count or rising.
        sizes = [
            56,    # 84 B IP  — 1F mid   (standard ping default)
            95,    # 123 B IP — 1F max
            157,   # 185 B IP — 2F mid
            218,   # 246 B IP — 2F max
            280,   # 308 B IP — 3F mid
            341,   # 369 B IP — 3F max
            403,   # 431 B IP — 4F mid
            max_s, # 492 B IP — 4F max   (MTU ceiling)
        ]

    sizes = [s for s in sizes if 1 <= s <= max_s]
    if not sizes:
        print(f"No valid sizes (max -s for this link: {max_s})")
        sys.exit(1)

    est = len(sizes) * args.count * args.interval
    print(f"SX1280 FLRC ping diagnostic → {args.target}")
    print(f"{args.count} pings × {args.interval:.1f}s × {len(sizes)} sizes  "
          f"≈ {est:.0f} s ({est/60:.1f} min)")
    print(f"Frag payload: {FRAG_DATA} B/frag  IP MTU: {FRAG_DATA*4} B")
    print()

    results = [run_ping(args.target, s, args.count, args.interval) for s in sizes]
    print_report(results)


if __name__ == "__main__":
    main()
