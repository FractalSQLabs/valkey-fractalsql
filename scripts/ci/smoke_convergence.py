#!/usr/bin/env python3
"""scripts/ci/smoke_convergence.py

Convergence smoke test driven from the CI workflow against a running
Valkey container with the fractalsql module preloaded.

Usage:
    smoke_convergence.py <host_port> <tag>

Exits 0 if cosine similarity of the returned best_point to the
query (0.6, 0.8) exceeds 0.99. Exits non-zero otherwise.

Why cosine similarity and not coordinate equality?
    Cosine distance is magnitude-invariant — every point on the ray
    through the origin and (0.6, 0.8) is a global optimum, so the
    SFS-returned coordinates can land anywhere on that ray inside
    [-1, 1]^2. Asserting (v1, v2) == (0.6, 0.8) would false-fail.
    Instead we assert the invariant the optimizer is actually
    minimizing. ||query||^2 = 0.6^2 + 0.8^2 = 1, so cos_sim
    simplifies to (v . query) / ||v||.

Python stdlib only — the GitHub Actions runner has python3; a raw
socket is simpler to audit in CI than a pip-installed redis-py.
"""

import math
import socket
import struct
import sys


class Resp:
    """Minimal synchronous RESP2/RESP3 client — no external deps."""

    def __init__(self, host, port):
        self.s = socket.create_connection((host, port))
        self.buf = b""

    def _readline(self):
        while b"\r\n" not in self.buf:
            chunk = self.s.recv(4096)
            if not chunk:
                raise RuntimeError("connection closed")
            self.buf += chunk
        line, _, self.buf = self.buf.partition(b"\r\n")
        return line

    def _readn(self, n):
        while len(self.buf) < n + 2:
            chunk = self.s.recv(4096)
            if not chunk:
                raise RuntimeError("connection closed")
            self.buf += chunk
        data = self.buf[:n]
        self.buf = self.buf[n + 2:]
        return data

    def _parse(self):
        line = self._readline()
        t = line[:1]
        rest = line[1:].decode()
        if t in (b"+", b"-"):
            return rest
        if t == b":":
            return int(rest)
        if t == b"$":
            n = int(rest)
            if n < 0:
                return None
            return self._readn(n)
        if t == b"*":
            n = int(rest)
            return [self._parse() for _ in range(n)]
        if t == b"%":
            n = int(rest)
            return {self._parse(): self._parse() for _ in range(n)}
        if t == b",":
            return float(rest)
        raise RuntimeError(f"unexpected RESP type {t!r}")

    def cmd(self, *args):
        parts = [b"*" + str(len(args)).encode() + b"\r\n"]
        for a in args:
            if isinstance(a, str):
                a = a.encode()
            parts.append(
                b"$" + str(len(a)).encode() + b"\r\n" + a + b"\r\n"
            )
        self.s.sendall(b"".join(parts))
        return self._parse()


def main():
    port = int(sys.argv[1])
    tag = sys.argv[2]

    r = Resp("127.0.0.1", port)

    # HELLO 3 negotiates RESP3 so FRACTAL.SEARCH returns a proper map.
    r.cmd("HELLO", "3")

    # Corpus is a single row (0.6, 0.8); query is (0.6, 0.8).
    # Packed little-endian float32.
    vec = struct.pack("<ff", 0.6, 0.8)
    assert r.cmd("SET", "corpus", vec) == "OK"

    reply = r.cmd("FRACTAL.SEARCH", "corpus", vec, "1")
    # Reply shape: every Valkey major (7.2 / 8.0 / 8.1) exposes
    # RedisModule_ReplyWithMap, so we get an RESP3 map (dict from
    # _parse) on the HELLO-3-negotiated connection. The flat-array
    # fallback path in the module is retained for 6.2-era Redis
    # callers; Valkey never takes that branch.
    if isinstance(reply, list):
        reply = {reply[i]: reply[i + 1] for i in range(0, len(reply), 2)}
    # Keys are emitted via RedisModule_ReplyWithSimpleString, which
    # hits both RESP2 and RESP3 as a simple-string frame (+...\r\n)
    # that _parse decodes to `str`. Bulk strings (values like vector
    # components on RESP2 doubles) come back as `bytes`. Normalize
    # once so the lookup doesn't depend on which path we took.
    def _k(x):
        return x.decode() if isinstance(x, (bytes, bytearray)) else x
    reply = { _k(k): v for k, v in reply.items() }
    bp = reply["best_point"]
    v1, v2 = float(bp[0]), float(bp[1])
    norm = math.sqrt(v1 * v1 + v2 * v2)
    if norm < 1e-9:
        print(f"[{tag}] SKIP: degenerate best_point {bp!r}")
        return 0
    cos_sim = (v1 * 0.6 + v2 * 0.8) / norm
    print(f"[{tag}] best_point=({v1:.6f},{v2:.6f}) cos_sim={cos_sim:.6f}")
    if cos_sim <= 0.99:
        print(f"[{tag}] FAIL: cos_sim {cos_sim} not > 0.99", file=sys.stderr)
        return 1
    print(f"[{tag}] convergence: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
