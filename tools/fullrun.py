#!/usr/bin/env python3
"""Wait for COM7 to (re)appear after a USB power-cycle, capture the cold boot,
and if the card enumerates run the full read-only diagnostic suite."""
import time, sys, serial

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
PORT, BAUD = "COM7", 115200


def port_present():
    try:
        s = serial.Serial(PORT, BAUD, timeout=0.1)
        s.close()
        return True
    except Exception:
        return False


def wait_for_recycle(timeout=120):
    """Wait until the port disappears (unplug) then reappears (replug)."""
    end = time.time() + timeout
    # Phase 1: wait for the port to go away.
    print("  (waiting for you to UNPLUG the USB...)")
    while time.time() < end and port_present():
        time.sleep(0.4)
    # Phase 2: wait for it to come back, then settle.
    print("  (unplug detected — waiting for REPLUG...)")
    while time.time() < end and not port_present():
        time.sleep(0.4)
    time.sleep(3.0)  # let the CH340 settle; board boot takes ~0.3s after that
    for _ in range(10):
        try:
            return serial.Serial(PORT, BAUD, timeout=0.3)
        except Exception:
            time.sleep(0.5)
    return None


def drain(s, win):
    end = time.time() + win
    buf = []
    while time.time() < end:
        d = s.read(8192)
        if d:
            buf.append(d.decode("utf-8", "replace"))
            end = time.time() + 2.5
    return "".join(buf)


def run(s, cmd, win):
    s.write((cmd + "\r").encode())
    return drain(s, win)


print("Power-cycle the board on COM7 now (unplug, wait, replug)...")
s = wait_for_recycle(120)
if not s:
    print("Board did not re-appear on COM7.")
    sys.exit(1)

# The one-shot boot banner may have already scrolled past before the CH340
# port became openable. Drain leftovers, then drive `reinit` to re-run the
# card bring-up on demand and capture *that* (identical) diagnosis.
boot = drain(s, 4)
i = boot.find("=====")
if i >= 0:
    print("--- captured boot banner ---")
    print(boot[i:])

print("--- forcing reinit ---")
out = run(s, "reinit", 14)
print(out)

if "Card ready" in out or "Card initialised OK" in out:
    print("\n##### CARD ENUMERATED — running full read-only suite #####")
    print(run(s, "info", 10))
    print(run(s, "caps", 8))
    print(run(s, "status", 8))
    print(run(s, "scan", 60))
    print(run(s, "bench", 30))
    print(run(s, "json", 8))
else:
    print("\n(Card did not enumerate; the bring-up diagnosis above is the result.)")
s.close()
