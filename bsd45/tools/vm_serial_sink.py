#!/usr/bin/env python3
"""vm_serial_sink.py - receive the BSD45 console over a VMware TCP serial port.

VMware Workstation's *file* backed serial port buffers guest output in the
vmx process and does not flush it on a hard power-off, which is why
serial.log stays 0 bytes for a BSD45 guest.  A *network* backed port does
not have that problem: VMware connects to this listener and every guest
byte is delivered immediately.

.vmx side:
    serial0.present  = "TRUE"
    serial0.fileType = "network"
    serial0.fileName = "tcp://127.0.0.1:9977"

usage: python tools/vm_serial_sink.py [port] [outfile]
"""
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9977
out = sys.argv[2] if len(sys.argv) > 2 else "vms/serial.log"

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(1)
print("listening on %d -> %s" % (port, out), flush=True)

conn, addr = srv.accept()
print("connected from %s" % (addr,), flush=True)
with open(out, "wb") as f:
    conn.settimeout(5.0)
    idle = 0
    while True:
        try:
            data = conn.recv(4096)
        except socket.timeout:
            idle += 1
            if idle > 24:          # ~2 minutes of silence: guest is done
                break
            continue
        if not data:
            break
        idle = 0
        f.write(data)
        f.flush()
print("sink done", flush=True)
