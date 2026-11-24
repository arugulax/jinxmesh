"""Files for the SD card of a device over its serial console: the host side of firmware/src/jinx/JinxFiles.cpp.

Talks through the raw TCP bridge of the webflash server, so the bytes go through the flasher page and its
Web Serial port. The protocol is described in JinxFiles.cpp.
"""

import os
import socket
import struct
import time
import zlib

HELLO = b"JXFILES1\n"
BEGIN, DATA, END, STAT, DELETE, BAUD, EXIT, LIST, CRC, TONE, READ = range(1, 12)
ACK, ERROR, OFFSET = 0x80, 0x81, 0x82
CHUNK = 7680  # two of them fit into the receive buffer of the console of the device
WINDOW = 2  # Data frames on their way before an answer is in


class Problem(Exception):
    pass


class Link:
    def __init__(self, bridge_port):
        self.bridge_port = bridge_port
        self.sock = None
        self.buffer = b""
        self.sequence = 0
        self.connect()

    def connect(self):
        if self.sock:
            self.sock.close()
        self.sock = socket.create_connection(("127.0.0.1", self.bridge_port), timeout=5)
        self.buffer = b""

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def frame(self, timeout):
        """The next good frame from the device as (type, sequence, payload), or None. Log text is skipped."""
        until = time.monotonic() + timeout
        while True:
            start = self.buffer.find(b"\xa5\x4a")
            if start < 0:
                self.buffer = self.buffer[-1:]
            else:
                self.buffer = self.buffer[start:]
                if len(self.buffer) >= 6:
                    length = self.buffer[4] | self.buffer[5] << 8
                    if length > 4096:
                        self.buffer = self.buffer[2:]
                        continue
                    if len(self.buffer) >= 10 + length:
                        body = self.buffer[2 : 6 + length]
                        (crc,) = struct.unpack_from("<I", self.buffer, 6 + length)
                        if zlib.crc32(body) == crc:
                            self.buffer = self.buffer[10 + length :]
                            return body[0], body[1], body[4:]
                        self.buffer = self.buffer[2:]
                        continue
            left = until - time.monotonic()
            if left <= 0:
                return None
            self.sock.settimeout(left)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                return None
            if not data:
                raise Problem("the bridge closed the connection")
            self.buffer += data

    def send(self, kind, payload=b""):
        self.sequence = (self.sequence + 1) & 0xFF
        body = struct.pack("<BBH", kind, self.sequence, len(payload)) + payload
        self.sock.sendall(b"\xa5\x4a" + body + struct.pack("<I", zlib.crc32(body)))
        return self.sequence

    def ask(self, kind, payload=b"", timeout=3.0, tries=6):
        """Sends a request until its answer comes. Returns (type, payload) of the answer."""
        for _ in range(tries):
            sequence = self.send(kind, payload)
            until = time.monotonic() + timeout
            while (left := until - time.monotonic()) > 0:
                got = self.frame(left)
                if got and got[1] == sequence:
                    if got[0] == ERROR:
                        raise Problem("the device says: " + got[2].decode(errors="replace"))
                    return got[0], got[2]
        raise Problem("the device does not answer")

    def hello(self):
        """Asks the console for the file transfer. True when it answers."""
        for _ in range(4):
            self.sock.sendall(b"\n" + HELLO)
            got = self.frame(2.0)
            if got and got[0] == ACK and got[2][:8] == HELLO[:8]:
                return True
        # it may still be in the transfer from an earlier run
        try:
            self.ask(STAT, b"/", tries=2)
            return True
        except Problem:
            return False


def put(link, local, remote, resume=True, progress=print):
    size = os.path.getsize(local)
    _, answer = link.ask(BEGIN, struct.pack("<QB", size, 1 if resume else 0) + remote.encode(), timeout=10)
    (offset,) = struct.unpack("<Q", answer)
    if offset:
        progress(f"  going on at {offset:,} of {size:,} bytes")
    started, begun, shown = time.monotonic(), offset, -1
    pending = []  # sequence numbers of the Data frames without an answer yet, oldest first
    sent, silent = offset, 0  # sent: where the next frame starts; offset: what the device has confirmed
    with open(local, "rb") as f:
        while offset < size:
            while len(pending) < WINDOW and sent < size:
                f.seek(sent)
                data = f.read(CHUNK)
                pending.append(link.send(DATA, struct.pack("<Q", sent) + data))
                sent += len(data)
            got = link.frame(5.0)
            if got is None:
                silent += 1
                if silent >= 8:
                    raise Problem("the device does not answer")
                pending, sent = [], offset  # again from what is confirmed
                continue
            kind, sequence, answer = got
            if not pending or sequence != pending[0]:
                continue  # the answer to a frame that was given up
            silent = 0
            if kind == ERROR:
                raise Problem("the device says: " + answer.decode(errors="replace"))
            (offset,) = struct.unpack("<Q", answer)
            if kind == OFFSET:
                pending, sent = [], offset  # a frame got lost: the device says where it is
            else:
                pending.pop(0)
            percent = 100 * offset // max(size, 1)
            if percent != shown:
                shown = percent
                rate = (offset - begun) / max(time.monotonic() - started, 0.001)
                left = (size - offset) / rate if rate > 0 else 0
                progress(f"  {percent}%  {offset:,} bytes  {rate / 1000:.0f} kB/s  {left / 60:.1f} min left")
    link.ask(END, timeout=20)
    return size, time.monotonic() - started


def get(link, remote):
    """The content of a file on the card."""
    out = b""
    while True:
        _, block = link.ask(READ, struct.pack("<Q", len(out)) + remote.encode(), timeout=10, tries=3)
        if not block:
            return out
        out += block


def crc_of_file(path):
    crc = 0
    with open(path, "rb") as f:
        while data := f.read(1 << 20):
            crc = zlib.crc32(data, crc)
    return crc
