#!/usr/bin/env python3
"""Quick non-invasive 'sniff' tests for a card already plugged in on COM7.
Runs only identity/capability probes: reinit, info, caps, status.
No surface scan, no benchmark, no writes."""
import time, sys, serial

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
PORT, BAUD = "COM7", 115200


def drain(s, win):
    end = time.time() + win
    buf = []
    while time.time() < end:
        d = s.read(8192)
        if d:
            buf.append(d.decode("utf-8", "replace"))
            end = time.time() + 1.5
    return "".join(buf)


def run(s, cmd, win):
    s.write((cmd + "\r").encode())
    return drain(s, win)


s = serial.Serial(PORT, BAUD, timeout=0.3)
s.write(b"\r")
time.sleep(0.3)
s.reset_input_buffer()

print(run(s, "reinit", 14))   # re-attempt bring-up (safe)
out = run(s, "info", 10)
print(out)
print(run(s, "caps", 8))
print(run(s, "status", 8))
s.close()
