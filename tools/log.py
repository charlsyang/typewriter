#!/usr/bin/env python3
"""Print the Inkplate's serial output without resetting it.

Usage: tools/log.py [seconds] [port]   (seconds 0 = until interrupted)
"""
import fcntl
import os
import struct
import sys
import termios
import time

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 0
port = sys.argv[2] if len(sys.argv) > 2 else "/dev/cu.usbserial-110"

fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[4] = attrs[5] = termios.B115200
attrs[2] |= termios.CLOCAL | termios.CREAD
attrs[2] &= ~termios.HUPCL  # don't drop the control lines on close
attrs[3] = 0
termios.tcsetattr(fd, termios.TCSANOW, attrs)
# Release DTR and RTS so the board keeps running.
fcntl.ioctl(fd, 0x8004746B, struct.pack("I", 0x006))

end = time.time() + seconds if seconds else float("inf")
while time.time() < end:
    try:
        chunk = os.read(fd, 4096).decode(errors="replace")
        sys.stdout.write(time.strftime("") + chunk)
        sys.stdout.flush()
    except BlockingIOError:
        time.sleep(0.05)
