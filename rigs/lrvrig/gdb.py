# SPDX-License-Identifier: GPL-3.0-or-later
"""Host side of the WinUAE snapshots (../snapd.py): a minimal gdb remote-protocol client for the
WinUAE gdbserver (`debugging_features=gdbserver`, `gdb_port=N`). Attaching stops the emulated
CPU at an instruction boundary; `monitor screenshot FILE` writes a PNG of the emulated display
(the whole PAL raster on a native screen, the board framebuffer on a graphics card screen),
`m` packets read guest memory (chip RAM, graphics card memory), `D` detaches and resumes.
Also the screen geometry and the conversions the presentation oracle needs.

Nothing here waits on the clock. The emulator listens from its start until it quits, so one
connect either works or means it is gone. The CPU is known to be stopped when the server
answers `?` (it answers only once halted; until then it refuses monitor commands and memory
reads). The reads block without a timeout: the emulator's exit, or the rig's timeout killing
it, closes the socket."""
import socket


class GdbError(Exception):
    pass


def _frame(payload):
    data = payload.encode()
    return b'$' + data + b'#' + ('%02x' % (sum(data) & 0xFF)).encode()


class Gdb:
    def __init__(self, port, host='127.0.0.1'):
        try:
            self.s = socket.create_connection((host, port))
        except OSError as e:
            raise GdbError('cannot connect to the gdbserver on port %d: %s' % (port, e))
        self.buf = b''
        # attaching interrupts the CPU; the stop reply says it has stopped
        reply, _ = self.request('?')
        if not reply or reply[0] not in 'ST':
            raise GdbError('no stop reply: %r' % reply[:40])

    def _recv_packet(self):
        while True:
            start = self.buf.find(b'$')
            if start >= 0:
                end = self.buf.find(b'#', start)
                if end >= 0 and len(self.buf) >= end + 3:
                    body = self.buf[start + 1:end]
                    self.buf = self.buf[end + 3:]
                    self.s.sendall(b'+')
                    return body.decode('latin-1')
            chunk = self.s.recv(65536)
            if not chunk:
                raise GdbError('gdbserver closed the connection')
            self.buf += chunk

    def request(self, payload):
        self.s.sendall(_frame(payload))
        reply = self._recv_packet()
        out = ''
        # console output arrives as O<hex> packets before the final reply
        while reply.startswith('O') and reply != 'OK':
            out += bytes.fromhex(reply[1:]).decode(errors='replace')
            reply = self._recv_packet()
        return reply, out

    def monitor(self, cmd):
        reply, out = self.request('qRcmd,' + cmd.encode().hex())
        if reply != 'OK':
            raise GdbError('monitor %r: %r %s' % (cmd, reply, out))
        return out

    def read(self, addr, n, chunk=0x1000):
        parts = []
        done = 0
        while done < n:
            k = min(chunk, n - done)
            reply, _ = self.request('m%x,%x' % (addr + done, k))
            if reply.startswith('E') or len(reply) != 2 * k:
                raise GdbError('read %x,%x: %r' % (addr + done, k, reply[:20]))
            parts.append(bytes.fromhex(reply))
            done += k
        return b''.join(parts)

    def detach(self):
        try:
            self.s.sendall(_frame('D'))
            self._recv_packet()
        except (OSError, GdbError):
            pass
        self.s.close()


# ---- the displayed bitmap as colour indices ------------------------------------------------

def planar_indices(mem, base, planes, bpr, rows=400, width=640):
    """Colour indices (rows, width) of a planar bitmap whose plane p starts at planes[p];
    `mem` holds guest memory starting at address `base`."""
    import numpy as np
    a = np.frombuffer(mem, np.uint8)
    wbytes = width // 8
    idx = np.zeros((rows, width), np.uint8)
    for p, addr in enumerate(planes):
        off = addr - base
        plane = np.stack([a[off + y * bpr: off + y * bpr + wbytes] for y in range(rows)])
        bits = np.unpackbits(plane, axis=1)            # MSB first = leftmost pixel
        idx |= (bits.astype(np.uint8) << p)
    return idx


def chunky_indices(mem, bpr, rows=400, width=640):
    import numpy as np
    a = np.frombuffer(mem, np.uint8)
    return np.stack([a[y * bpr: y * bpr + width] for y in range(rows)])


def parse_request(line):
    """key=value words of a snap request (lrvne/backends/amiga/core/snap.c)."""
    d = {}
    for word in line.split():
        if '=' in word:
            k, v = word.split('=', 1)
            d[k] = v
    return d
