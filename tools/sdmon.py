#!/usr/bin/env python3
"""Minimal serial driver for the SD-Card Diagnosis tool.

Usage:
  sdmon.py boot                 # reset board, capture boot output
  sdmon.py cmd "info" "caps"    # send commands, capture replies
"""
import sys, time, serial

PORT = "COM7"
BAUD = 115200

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def open_port(reset=False):
    s = serial.Serial(PORT, BAUD, timeout=0.2)
    if reset:
        # Pull EN low via DTR/RTS to reset (CYD: RTS->EN). Toggle both.
        s.setDTR(False); s.setRTS(True); time.sleep(0.1)
        s.setRTS(False); time.sleep(0.1)
        s.reset_input_buffer()
    return s


def drain(s, seconds):
    end = time.time() + seconds
    out = []
    while time.time() < end:
        data = s.read(4096)
        if data:
            out.append(data.decode("utf-8", "replace"))
            end = time.time() + 0.6  # extend while data flows
    return "".join(out)


def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    mode = sys.argv[1]
    if mode == "boot":
        s = open_port(reset=True)
        print(drain(s, 6), end="")
        s.close()
    elif mode == "cmd":
        s = open_port(reset=False)
        # nudge prompt
        s.write(b"\r")
        time.sleep(0.3)
        s.reset_input_buffer()
        for c in sys.argv[2:]:
            s.write((c + "\r").encode())
            time.sleep(0.2)
            print(f"\n>>> {c}")
            print(drain(s, 8), end="")
        s.close()


if __name__ == "__main__":
    main()
