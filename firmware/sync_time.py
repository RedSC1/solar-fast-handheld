#!/usr/bin/env python3
"""Set the handheld's volatile UTC clock over its UART USB port."""
import sys
import time

import serial


def main() -> int:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <serial-port>", file=sys.stderr)
        return 2
    with serial.Serial(sys.argv[1], 115200, timeout=0.2) as port:
        time.sleep(0.5)
        epoch = int(time.time())
        port.write(f"T{epoch}\n".encode("ascii"))
        port.flush()
        print(f"Sent UTC epoch {epoch}")
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            line = port.readline().decode("utf-8", "replace").strip()
            if "UTC synced:" in line:
                print(line)
                return 0
    print("No confirmation received", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
