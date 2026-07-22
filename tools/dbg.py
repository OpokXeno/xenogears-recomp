#!/usr/bin/env python3
"""One-shot client for the psxrecomp TCP debug server.

The server is one-command-per-connection: connect, send one JSON object,
read the reply until EOF. Usage:

  python3 tools/dbg.py '{"cmd":"screenshot","path":"/tmp/shot.png"}'
  python3 tools/dbg.py '{"cmd":"press","buttons":65527,"frames":4}'
  PSX_DBG_PORT=4371 python3 tools/dbg.py '{"cmd":"state"}'

Pad word is ACTIVE-LOW (0xFFFF = all released): START=0xFFF7 (65527),
CROSS=0xBFFF (49151), SELECT=0xFFFE (65534), CIRCLE=0xDFFF (57343).
"""

import os
import socket
import sys

HOST = "127.0.0.1"
PORT = int(os.environ.get("PSX_DBG_PORT", "4370"))


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    payload = argv[1].encode()
    if not payload.endswith(b"\n"):
        payload += b"\n"          # server reads newline-terminated lines
    with socket.create_connection((HOST, PORT), timeout=30) as s:
        s.sendall(payload)
        chunks = []
        while True:
            data = s.recv(65536)
            if not data:
                break
            chunks.append(data)
            # The DuckStation oracle (4371) keeps the connection open after the
            # reply; the native server closes it. Stop at the first full line
            # so both work.
            if b"\n" in data:
                break
    sys.stdout.buffer.write(b"".join(chunks))
    if not b"".join(chunks).endswith(b"\n"):
        sys.stdout.buffer.write(b"\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
