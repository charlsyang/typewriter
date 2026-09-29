#!/usr/bin/env python3
"""Type on the Inkplate from this terminal, over the USB cable.

Every key is sent straight to the board (UTF-8, so curly quotes and em dashes
work). The board's timing log is saved to build/type-log.txt.

Keys: Backspace, Enter, Ctrl+L full refresh, Ctrl+N clear page, Ctrl+] quit.

Usage: tools/type.py [port]
"""
import fcntl
import os
import select
import struct
import sys
import termios
import tty

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-110"
log_path = os.path.join(os.path.dirname(__file__), "..", "build", "type-log.txt")
os.makedirs(os.path.dirname(log_path), exist_ok=True)

fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
attrs = termios.tcgetattr(fd)
attrs[4] = attrs[5] = termios.B115200
attrs[2] |= termios.CLOCAL | termios.CREAD
attrs[3] = 0
termios.tcsetattr(fd, termios.TCSANOW, attrs)
# Release DTR and RTS so the board runs normally instead of resetting.
TIOCMBIC, RTS, DTR = 0x8004746B, 0x004, 0x002
fcntl.ioctl(fd, TIOCMBIC, struct.pack("I", DTR | RTS))

print("Typing to the Inkplate. Ctrl+L full refresh, Ctrl+N clear page, Ctrl+] quit.\r")
stdin = sys.stdin.fileno()
saved = termios.tcgetattr(stdin)
tty.setraw(stdin)
try:
    with open(log_path, "a") as log:
        while True:
            ready, _, _ = select.select([stdin, fd], [], [])
            if stdin in ready:
                data = os.read(stdin, 1024)
                if b"\x1d" in data:  # Ctrl+]
                    break
                os.write(fd, data)
            if fd in ready:
                try:
                    log.write(os.read(fd, 4096).decode(errors="replace"))
                    log.flush()
                except BlockingIOError:
                    pass
finally:
    termios.tcsetattr(stdin, termios.TCSADRAIN, saved)
    os.close(fd)
    print("\nDone. Timing log: build/type-log.txt")
