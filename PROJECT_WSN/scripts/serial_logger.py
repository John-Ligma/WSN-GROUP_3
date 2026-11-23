#!/usr/bin/env python3
"""
EE4552 WSN - Gateway Serial CSV Logger
Reads live DATA/ALERT lines from Gateway COM port, prints to screen,
saves raw CSV to docs_logs/, and runs log_analyzer automatically on exit.

Usage:
    python scripts/serial_logger.py COM4 docs_logs/test_t2_1hop.csv
    (Press Ctrl+C to stop and print PDR / RTT / RSSI summary)
"""

import os
import sys
import time
import serial
from log_analyzer import analyze_log


def main() -> None:
    if len(sys.argv) < 2:
        print("Usage: python scripts/serial_logger.py <COM_PORT> [output_csv_path] [max_data_samples]")
        print("Example: python scripts/serial_logger.py COM4 docs_logs/demo_1_hop.csv 30")
        sys.exit(1)

    port = sys.argv[1]
    if len(sys.argv) >= 3:
        out_path = sys.argv[2]
    else:
        ts = time.strftime("%Y%m%d_%H%M%S")
        out_path = os.path.join("docs_logs", f"gw_log_{ts}.csv")

    max_samples = int(sys.argv[3]) if len(sys.argv) >= 4 else 0

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)

    target_msg = f" (auto-stop at {max_samples} DATA rows)" if max_samples > 0 else " (Press Ctrl+C to stop)"
    print(f"[LOGGER] Opening {port} @ 115200 bps -> saving to {out_path}{target_msg}\n", flush=True)

    data_count = 0
    try:
        with serial.Serial(port, 115200, timeout=0.5) as ser, open(
            out_path, "w", encoding="utf-8", newline="\n"
        ) as f:
            # Soft reset Gateway for a clean start (clears SeenCache)
            ser.setDTR(False)
            ser.setRTS(True)
            time.sleep(0.1)
            ser.setRTS(False)
            time.sleep(0.2)
            ser.reset_input_buffer()

            while True:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                if line.startswith("DATA,"):
                    data_count += 1
                    print(f"[{data_count}/{max_samples if max_samples else '?'}] {line}", flush=True)
                else:
                    print(line, flush=True)
                f.write(line + "\n")
                f.flush()
                if max_samples > 0 and data_count >= max_samples:
                    print(f"\n[LOGGER] Reached target {max_samples} DATA rows -> saved to: {out_path}\n", flush=True)
                    break
    except KeyboardInterrupt:
        print(f"\n[LOGGER] Stopped. Saved {data_count} DATA rows to: {out_path}\n", flush=True)
    except serial.SerialException as e:
        print(f"\n[ERROR] Cannot open {port}: {e}")
        print("Hint: Close any open VS Code Serial Monitor tab first (trash icon)!")
        sys.exit(1)

    if data_count > 0:
        analyze_log(out_path)


if __name__ == "__main__":
    main()
