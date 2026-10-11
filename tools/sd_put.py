#!/usr/bin/env python3
"""Copy files from this Mac onto the Inkplate's SD card, over USB.

For each file, sends Ctrl+R "put <path> <bytes> <crc32>", then the file in
chunks, waiting for the device to acknowledge each one. The device writes to
<path>.tmp and only replaces <path> once the whole file arrived with a matching
CRC, so a failed copy leaves the old file in place.

Usage: tools/sd_put.py [--port PORT] LOCAL_FILE:/sd/path [LOCAL_FILE:/sd/path ...]
Example: tools/sd_put.py build/test.bin:/test.bin
"""
import argparse
import fcntl
import os
import select
import struct
import sys
import termios
import time
import zlib


def open_port(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios.B115200
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[2] &= ~termios.HUPCL
    attrs[3] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    fcntl.ioctl(fd, 0x8004746B, struct.pack("I", 0x006))  # release DTR/RTS: no reset
    return fd


class Lines:
    """Reads the device's serial log line by line, skipping everything but put replies."""

    def __init__(self, fd):
        self.fd, self.buf = fd, b""

    def wait_for(self, timeout):
        deadline = time.time() + timeout
        while True:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").strip()
                if text.startswith("put: "):
                    return text[5:]
            left = deadline - time.time()
            if left <= 0:
                raise SystemExit("no reply from the Inkplate (is it awake and on this port?)")
            if select.select([self.fd], [], [], left)[0]:
                try:
                    self.buf += os.read(self.fd, 4096)
                except BlockingIOError:
                    pass


def write_all(fd, data):
    while data:
        try:
            n = os.write(fd, data)
            data = data[n:]
        except BlockingIOError:
            select.select([], [fd], [])


def put(fd, lines, local, remote):
    data = open(local, "rb").read()
    crc = zlib.crc32(data)
    write_all(fd, f"\x12put {remote} {len(data)} {crc:08x}\n".encode())
    reply = lines.wait_for(10)
    if not reply.startswith("ready"):
        raise SystemExit(f"{remote}: {reply}")
    chunk = int(reply.split()[1])
    sent, t0 = 0, time.time()
    while sent < len(data):
        piece = data[sent:sent + chunk]
        write_all(fd, piece)
        sent += len(piece)
        reply = lines.wait_for(15)
        if reply != f"ok {sent}":
            raise SystemExit(f"{remote}: {reply}")
        rate = sent / max(time.time() - t0, 0.001) / 1024
        print(f"\r{remote}: {sent * 100 // len(data)}% ({sent} of {len(data)} bytes, {rate:.1f} KB/s)", end="", flush=True)
    reply = lines.wait_for(15)
    print()
    if not reply.startswith("done"):
        raise SystemExit(f"{remote}: {reply}")
    print(f"{remote}: copied in {time.time() - t0:.1f} s, CRC {crc:08x} checked")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/cu.usbserial-110")
    ap.add_argument("files", nargs="+", metavar="LOCAL:REMOTE")
    args = ap.parse_args()
    pairs = []
    for spec in args.files:
        local, sep, remote = spec.rpartition(":")
        if not sep or not remote.startswith("/") or " " in remote:
            sys.exit(f"{spec}: expected LOCAL_FILE:/path/on/card (no spaces in the card path)")
        pairs.append((local, remote))
    fd = open_port(args.port)
    lines = Lines(fd)
    for local, remote in pairs:
        put(fd, lines, local, remote)
    os.close(fd)


if __name__ == "__main__":
    main()
