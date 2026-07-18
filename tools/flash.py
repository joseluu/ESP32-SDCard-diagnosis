#!/usr/bin/env python3
"""Build and flash the SD-Card Diagnosis firmware onto the CYD board.

The CYD ESP32-2432S032C is also used for other projects; this script restores
the SD-diagnosis firmware in one command:

  python tools/flash.py               # full build (capcheck/wtest, runtime-gated)
  python tools/flash.py --safe        # safe build (never writes to the card)
  python tools/flash.py --build-only  # compile without flashing
  python tools/flash.py --boot        # after flashing, show the boot output

PlatformIO's ESP-IDF tooling refuses to run under MSYS/MinGW (Git Bash), so
the build subprocess gets MSYSTEM removed and the Git MSYS directories
stripped from PATH — the same trick as firmware/bp.ps1. Because only the
child environment is touched, this script works from any shell.
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PROJ = REPO / "firmware"
PIO = Path.home() / ".platformio" / "penv" / "Scripts" / "pio.exe"
if not PIO.exists():
    PIO = Path.home() / ".platformio" / "penv" / "bin" / "pio"  # non-Windows


def clean_env():
    env = os.environ.copy()
    env.pop("MSYSTEM", None)
    # When stdout is a pipe, Python defaults to cp1252 on Windows and pio's
    # output thread dies on esptool's Unicode progress bar — which then blocks
    # esptool itself mid-flash on a full pipe. Force UTF-8 end to end.
    env["PYTHONIOENCODING"] = "utf-8"
    env["PYTHONUTF8"] = "1"
    parts = [
        p for p in env.get("PATH", "").split(os.pathsep)
        if "Git\\usr" not in p and "Git\\mingw64" not in p
        and not p.rstrip("\\").endswith("Git\\bin")
    ]
    gitcmd = r"C:\Program Files\Git\cmd"  # keep git itself reachable
    if os.name == "nt" and gitcmd not in parts:
        parts.insert(0, gitcmd)
    env["PATH"] = os.pathsep.join(parts)
    return env


def show_boot(port):
    import serial  # pyserial, only needed for --boot
    s = serial.Serial(port, 115200, timeout=0.2)
    s.setDTR(False)
    s.setRTS(True)
    time.sleep(0.1)
    s.setRTS(False)  # CYD: RTS drives EN — release resets the board
    s.reset_input_buffer()
    end = time.time() + 8
    while time.time() < end:
        d = s.read(4096)
        if d:
            sys.stdout.write(d.decode("utf-8", "replace"))
            end = time.time() + 1.0
    s.close()


def main():
    ap = argparse.ArgumentParser(
        description="Build and flash the SD-Card Diagnosis firmware.")
    ap.add_argument("--safe", action="store_true",
                    help="flash the safe build (no write command compiled in)"
                         " instead of the full cyd_esp32_destructive build")
    ap.add_argument("--port", default="COM7", help="serial port (default COM7)")
    ap.add_argument("--build-only", action="store_true",
                    help="compile without flashing")
    ap.add_argument("--boot", action="store_true",
                    help="capture the boot output after flashing (pyserial)")
    a = ap.parse_args()

    envname = "cyd_esp32" if a.safe else "cyd_esp32_destructive"
    cmd = [str(PIO), "run", "-d", str(PROJ), "-e", envname]
    if not a.build_only:
        cmd += ["-t", "upload", "--upload-port", a.port]
    print(f"[flash.py] {envname} -> "
          f"{'(build only)' if a.build_only else a.port}")
    r = subprocess.run(cmd, env=clean_env())
    if r.returncode != 0:
        sys.exit(r.returncode)

    if a.boot and not a.build_only:
        show_boot(a.port)


if __name__ == "__main__":
    main()
