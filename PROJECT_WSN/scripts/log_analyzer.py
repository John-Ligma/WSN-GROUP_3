#!/usr/bin/env python3
"""
EE4552 WSN - Gateway CSV Log Analyzer (for Tests T2, T3, T4, T5, T6)
Parses raw Serial CSV logs from Gateway:
    DATA,<src_id>,<via_id>,<seq_num>,<hop_count>,<temp_c>,<humi_rh>,<rssi_dbm>,<rtt_ms>
    ALERT,NODE_OFFLINE,<node_id>

Usage:
    python scripts/log_analyzer.py docs_logs/test_t3_2hop.csv
"""

import sys
import statistics
from collections import defaultdict, Counter


def analyze_log(filepath: str) -> None:
    nodes = defaultdict(lambda: {
        "seqs": [],
        "unique_seqs": set(),
        "dups": 0,
        "hops": Counter(),
        "vias": Counter(),
        "rtts": [],
        "rtt_timeouts": 0,
        "rssis": [],
        "temps": [],
        "humis": [],
    })
    alerts = []

    with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",")
            if parts[0] == "ALERT" and len(parts) >= 3:
                alerts.append((lineno, parts[1], parts[2]))
                continue
            if parts[0] != "DATA" or len(parts) < 9:
                continue

            try:
                src_id = int(parts[1])
                via_id = int(parts[2])
                seq_num = int(parts[3])
                hop_count = int(parts[4])
                temp_c = float(parts[5])
                humi_rh = float(parts[6])
                rssi = int(parts[7])
                rtt = int(parts[8])
            except ValueError:
                continue

            n = nodes[src_id]
            n["seqs"].append(seq_num)
            if seq_num in n["unique_seqs"]:
                n["dups"] += 1
            else:
                n["unique_seqs"].add(seq_num)

            n["hops"][hop_count] += 1
            n["vias"][via_id] += 1
            n["rssis"].append(rssi)
            if temp_c > -90.0:
                n["temps"].append(temp_c)
                n["humis"].append(humi_rh)

            if rtt == 65535:
                n["rtt_timeouts"] += 1
            elif rtt > 0:
                n["rtts"].append(rtt)

    print(f"=== WSN LOG ANALYSIS: {filepath} ===")
    if not nodes:
        print("No valid DATA rows found.")
        return

    for src_id in sorted(nodes.keys()):
        n = nodes[src_id]
        uniq = n["unique_seqs"]
        min_seq, max_seq = min(uniq), max(uniq)
        expected = max_seq - min_seq + 1
        received = len(uniq)
        lost = expected - received
        pdr = (received / expected) * 100.0 if expected > 0 else 0.0

        print(f"\n--- Node {src_id} ---")
        print(f"  Seq range      : {min_seq} .. {max_seq} (expected {expected})")
        print(f"  Received/Lost  : {received} / {lost} (duplicates: {n['dups']})")
        print(f"  PDR            : {pdr:.2f}%")
        print(f"  Hop breakdown  : {dict(n['hops'])} | Via breakdown: {dict(n['vias'])}")
        if n["rtts"]:
            print(
                f"  RTT (ms)       : n={len(n['rtts'])}, "
                f"min={min(n['rtts'])}, avg={statistics.mean(n['rtts']):.2f}, "
                f"max={max(n['rtts'])} (timeouts={n['rtt_timeouts']})"
            )
        else:
            print(f"  RTT (ms)       : no valid samples (timeouts={n['rtt_timeouts']})")
        if n["rssis"]:
            print(
                f"  RSSI (dBm)     : min={min(n['rssis'])}, "
                f"avg={statistics.mean(n['rssis']):.2f}, max={max(n['rssis'])}"
            )
        if n["temps"]:
            print(
                f"  Temp / Humi    : avg T={statistics.mean(n['temps']):.2f} C, "
                f"avg RH={statistics.mean(n['humis']):.2f} %"
            )

    if alerts:
        print(f"\n--- Alerts ({len(alerts)}) ---")
        for lineno, kind, nid in alerts:
            print(f"  Line {lineno}: {kind} -> Node {nid}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python scripts/log_analyzer.py <path_to_csv>")
        sys.exit(1)
    analyze_log(sys.argv[1])
