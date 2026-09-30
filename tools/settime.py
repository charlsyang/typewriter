#!/usr/bin/env python3
"""Set the Inkplate's clock and time zone from this Mac, over USB.

Sends Ctrl+T, then "<UTC epoch> <POSIX TZ>". The TZ rule comes from the Mac's
own zone file, so daylight saving changes are handled on the device.

Usage: tools/settime.py [port]
"""
import fcntl
import os
import struct
import sys
import termios
import time


def posix_tz():
    """The POSIX TZ string at the end of the Mac's zoneinfo file (TZif v2+)."""
    zone_file = os.path.realpath("/etc/localtime")
    with open(zone_file, "rb") as f:
        footer = f.read().rstrip(b"\n").rsplit(b"\n", 1)[-1]
    return footer.decode()


port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-110"
tz = posix_tz()
fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[4] = attrs[5] = termios.B115200
attrs[2] |= termios.CLOCAL | termios.CREAD
attrs[2] &= ~termios.HUPCL
attrs[3] = 0
termios.tcsetattr(fd, termios.TCSANOW, attrs)
fcntl.ioctl(fd, 0x8004746B, struct.pack("I", 0x006))  # release DTR/RTS: no reset
epoch = int(time.time())
os.write(fd, b"\x14" + f"{epoch} {tz}\n".encode())
time.sleep(0.5)
os.close(fd)
print(f"Sent {time.strftime('%Y-%m-%d %H:%M:%S')} ({tz}) to the Inkplate.")
