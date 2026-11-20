#!/usr/bin/env python3
"""vm_screen.py - read the BSD45 VGA text console from a VMware memory dump.

VMware Workstation cannot capture a BSD45 guest screen with
`vmrun captureScreen` (that needs VMware Tools), and its file-backed serial
port swallows the fetched COM1 output.  A snapshot does work without tools:

    vmrun -T ws snapshot <vmx> shot1
    python tools/vm_screen.py vms/<name>-Snapshot1.vmem

The snapshot `.vmem` is a raw image of guest physical memory, so the VGA
text buffer at physical 0xB8000 is directly readable.  Because the BSD45
kernel is linked at KERNBASE 0xFE000000 and its low physical memory is
identity mapped, the same file also exposes kernel globals: their physical
address is (symbol address - 0xFE000000).

    python tools/vm_screen.py dump.vmem                 # 80x25 text screen
    python tools/vm_screen.py dump.vmem --mem 0x8e000 256
    python tools/vm_screen.py dump.vmem --sym bsd45.elf procs
    python tools/vm_screen.py dump.vmem --klog bsd45.elf   # console log from RAM
"""
import struct
import sys

VGA = 0xB8000
COLS, ROWS = 80, 25


def read_vga(path, rows=ROWS, cols=COLS, base=VGA):
    with open(path, "rb") as f:
        f.seek(base)
        data = f.read(rows * cols * 2)
    lines = []
    for r in range(rows):
        text = ""
        for c in range(cols):
            ch = data[(r * cols + c) * 2]
            text += chr(ch) if 32 <= ch < 127 else " "
        lines.append(text.rstrip())
    while lines and not lines[-1]:
        lines.pop()
    return lines


def hexdump(path, addr, length):
    with open(path, "rb") as f:
        f.seek(addr)
        data = f.read(length)
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexa = " ".join("%02x" % b for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print("%08x  %-47s  %s" % (addr + off, hexa, text))


def read_u32(path, addr, n=1):
    with open(path, "rb") as f:
        f.seek(addr)
        return list(struct.unpack("<%dI" % n, f.read(4 * n)))


def elf_symbols(path):
    """Minimal ELF32 symbol table reader (no external deps)."""
    data = open(path, "rb").read()
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    secs = []
    for i in range(shnum):
        off = shoff + i * shentsize
        name, typ, flags, addr, offset, size, link, info, align, entsize = \
            struct.unpack_from("<IIIIIIIIII", data, off)
        secs.append(dict(name=name, typ=typ, addr=addr, offset=offset,
                         size=size, link=link, entsize=entsize))
    syms = {}
    for s in secs:
        if s["typ"] != 2:          # SHT_SYMTAB
            continue
        strtab = secs[s["link"]]
        count = s["size"] // s["entsize"]
        for i in range(count):
            off = s["offset"] + i * s["entsize"]
            nameoff, value, size, info, other, shndx = \
                struct.unpack_from("<IIIBBH", data, off)
            if not nameoff:
                continue
            end = data.index(b"\0", strtab["offset"] + nameoff)
            name = data[strtab["offset"] + nameoff:end].decode()
            syms[name] = value
    return syms


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    if "--klog" in argv:
        i = argv.index("--klog")
        elf = argv[i + 1] if len(argv) > i + 1 else "build/bsd45.elf"
        syms = elf_symbols(elf)
        # PE/COFF objects carry a leading underscore in the symbol name
        for name in ("_klog_buf", "klog_buf"):
            if name in syms:
                break
        else:
            print("no symbol klog_buf")
            return 1
            pa = syms[name] - 0xFE000000
            with open(path, "rb") as f:
                f.seek(pa)
                buf = f.read(8192)
            end = buf.find(b"\0")
            if end >= 0:
                buf = buf[:end]
            sys.stdout.write(buf.decode("latin-1").replace("\r", ""))
        return 0
    if "--mem" in argv:
        i = argv.index("--mem")
        hexdump(path, int(argv[i + 1], 0), int(argv[i + 2], 0))
        return 0
    if "--sym" in argv:
        i = argv.index("--sym")
        elf, sym = argv[i + 1], argv[i + 2]
        n = int(argv[i + 3]) if len(argv) > i + 3 else 8
        syms = elf_symbols(elf)
        if sym not in syms:
            sym = "_" + sym
        if sym not in syms:
            print("no such symbol: %s" % sym)
            return 1
        pa = syms[sym] - 0xFE000000
        vals = read_u32(path, pa, n)
        print("%s @ %08x (phys %08x): %s" % (sym, syms[sym], pa,
              " ".join("%08x" % v for v in vals)))
        return 0
    for line in read_vga(path):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
