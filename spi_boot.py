#!/usr/bin/env python3
"""Boot CrowPanel ESP32-P4 after esptool --after no-reset.

CH340 DTR is wired to BOOT (GPIO35). Empirically:
  DTR=False + reset  → ROM download (boot:0x317)
  DTR=True  + reset  → SPI app boot

esptool's hard-reset leaves DOWNLOAD mode, so we never use --after hard-reset.
"""
from __future__ import annotations

import sys
import time

import serial


def boot(port: str, listen_s: float = 14.0) -> bool:
    s = serial.Serial(port, 115200, timeout=0.25, dsrdtr=False, rtscts=False)
    try:
        # Hold in reset with BOOT released (SPI boot straps).
        s.dtr = True
        s.rts = True
        time.sleep(0.15)
        s.rts = False  # release EN → run app from flash
        buf = b""
        t0 = time.time()
        while time.time() - t0 < listen_s:
            d = s.read(8192)
            if d:
                buf += d
            if b"setup done" in buf:
                print("booted")
                return True
            if b"waiting for download" in buf and time.time() - t0 > 1.5:
                print("download")
                return False
        print("silent")
        return False
    finally:
        # Leave DTR asserted so a follow-up physical RESET still SPI-boots
        # while this process exits (best-effort; OS may still toggle lines).
        try:
            s.dtr = True
            s.rts = False
        except Exception:
            pass
        s.close()


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
    ok = boot(port)
    if ok:
        return 0
    # One retry with a longer hold
    time.sleep(0.3)
    return 0 if boot(port, listen_s=18.0) else 1


if __name__ == "__main__":
    raise SystemExit(main())
