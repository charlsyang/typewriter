#!/usr/bin/env python3
"""Reset the Inkplate and print its serial output for a few seconds.

Usage: tools/monitor.py [seconds] [port]
"""
import fcntl
import os
import struct
import sys
import termios
import time

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 8
port = sys.argv[2] if len(sys.argv) > 2 else "/dev/cu.usbserial-110"

fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[4] = attrs[5] = termios.B115200
attrs[2] |= termios.CLOCAL | termios.CREAD
attrs[3] = 0
termios.tcsetattr(fd, termios.TCSANOW, attrs)

# Pulse RTS with DTR low: a normal reset, not bootloader mode.
TIOCMBIS, TIOCMBIC, RTS, DTR = 0x8004746C, 0x8004746B, 0x004, 0x002
fcntl.ioctl(fd, TIOCMBIC, struct.pack("I", DTR))
fcntl.ioctl(fd, TIOCMBIS, struct.pack("I", RTS))
time.sleep(0.1)
fcntl.ioctl(fd, TIOCMBIC, struct.pack("I", RTS))

end = time.time() + seconds
while time.time() < end:
    try:
        sys.stdout.write(os.read(fd, 4096).decode(errors="replace"))
        sys.stdout.flush()
    except BlockingIOError:
        time.sleep(0.05)
