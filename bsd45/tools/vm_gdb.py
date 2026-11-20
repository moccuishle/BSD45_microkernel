#!/usr/bin/env python3
"""vm_gdb.py - talk to the VMware Workstation gdb stub (debugStub) of BSD45.

The BSD45 kernels have no serial console in every VM configuration, and
VMware's file-backed UART does not always capture the polled COM1 output.
VMware's built-in gdb stub gives a host-visible channel that does not need
VMware Tools: we speak the GDB remote serial protocol (RSP) over TCP and
read guest physical memory directly, e.g. the VGA text buffer at 0xB8000.

    python tools/vm_gdb.py screen            # dump the 80x25 text screen
    python tools/vm_gdb.py regs              # dump the 16 GPRs
    python tools/vm_gdb.py mem 0xb8000 4000  # hex dump
    python tools/vm_gdb.py rsp '$?#3f'       # raw packet

Requires in the .vmx:
    debugStub.enable = "TRUE"
    debugStub.port = "8864"
    debugStub.hideBreakpoints = "TRUE"
"""
import socket
import sys

PORT = 8864


def csum(payload: str) -> str:
    return "%02x" % (sum(payload.encode()) & 0xff)


class Stub:
    def __init__(self, host="127.0.0.1", port=PORT, timeout=10.0):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.settimeout(timeout)
        self.buf = b""

    def _read_packet(self):
        while True:
            while b"$" not in self.buf:
                chunk = self.s.recv(4096)
                if not chunk:
                    raise EOFError("stub closed the connection")
                self.buf += chunk
            self.buf = self.buf[self.buf.index(b"$") + 1:]
            if b"#" not in self.buf:
                continue
            body, rest = self.buf.split(b"#", 1)
            if len(rest) < 2:
                continue
            self.buf = rest[2:]
            return body.decode("latin-1")

    def cmd(self, payload: str) -> str:
        pkt = "$%s#%s" % (payload, csum(payload))
        self.s.sendall(pkt.encode())
        ack = self.s.recv(1)
        if ack not in (b"+", b""):
            # some stubs reply with the packet directly
            self.buf = ack + self.buf
        return self._read_packet()

    def read_mem(self, addr: int, length: int) -> bytes:
        # the stub answers in chunks; ask in 1 KiB pieces
        out = b""
        while length > 0:
            n = min(length, 1024)
            r = self.cmd("m%x,%x" % (addr, n))
            if not r or r.startswith("E"):
                raise RuntimeError("cannot read %#x (%s)" % (addr, r))
            out += bytes.fromhex(r)
            addr += n
            length -= n
        return out

    def write_mem(self, addr: int, data: bytes):
        for i in range(0, len(data), 256):
            piece = data[i:i + 256]
            r = self.cmd("M%x,%x:%s" % (addr + i, len(piece), piece.hex()))
            if r != "OK":
                raise RuntimeError("cannot write %#x (%s)" % (addr + i, r))

    def regs(self):
        r = self.cmd("g")
        if not r or r.startswith("E"):
            return {}
        names = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
                 "eip", "eflags", "cs", "ss", "ds", "es", "fs", "gs"]
        raw = bytes.fromhex(r)
        return {n: int.from_bytes(raw[i * 4:i * 4 + 4], "little")
                for i, n in enumerate(names) if len(raw) >= (i + 1) * 4}

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def dump_screen(stub: Stub, base=0xB8000, rows=25, cols=80):
    mem = stub.read_mem(base, rows * cols * 2)
    lines = []
    for r in range(rows):
        line = ""
        for c in range(cols):
            ch = mem[(r * cols + c) * 2]
            line += chr(ch) if 32 <= ch < 127 else " "
        lines.append(line.rstrip())
    while lines and not lines[-1]:
        lines.pop()
    return lines


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    stub = Stub()
    try:
        if cmd == "screen":
            rows = int(argv[2]) if len(argv) > 2 else 25
            for line in dump_screen(stub, rows=rows):
                print(line)
        elif cmd == "regs":
            for k, v in stub.regs().items():
                print("%-7s %08x" % (k, v))
        elif cmd == "mem":
            addr = int(argv[2], 0)
            length = int(argv[3], 0) if len(argv) > 3 else 256
            data = stub.read_mem(addr, length)
            for off in range(0, len(data), 16):
                chunk = data[off:off + 16]
                hexa = " ".join("%02x" % b for b in chunk)
                text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
                print("%08x  %-47s  %s" % (addr + off, hexa, text))
        elif cmd == "rsp":
            print(stub.cmd(argv[2]))
        else:
            print("unknown command %r" % cmd)
            return 2
    finally:
        stub.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
