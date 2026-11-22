#!/usr/bin/env python3
"""Smoke test for seq_gateway (protocol v2): a buy rests, a smaller sell crosses it, a heartbeat is echoed, a bad message closes the connection.
Usage: seq_smoke.py [port]   (the server must be running on 127.0.0.1)"""
import socket, struct, sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9100

def header(length, typ, seq):
    return struct.pack(">HBBQ", length, typ, 2, seq)

def submit(seq, account, cl, side, price, qty):
    return header(39, 16, seq) + struct.pack(">IQBBBIII", account, cl, side, 0, 0, price, qty, 0)

def recv_exact(s, n):
    b = b""
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise SystemExit("connection closed early")
        b += c
    return b

def reply(s):
    b = recv_exact(s, 43)
    length, typ, ver, seq = struct.unpack(">HBBQ", b[:12])
    assert (length, typ, ver) == (43, 19, 2), (length, typ, ver)
    cseq, oid, rej, status, filled, rested, cancelled, dup = struct.unpack(">QQBBIIIB", b[12:])
    return dict(seq=seq, client_seq=cseq, order_id=oid, reject=rej, status=status, filled=filled, rested=rested, cancelled=cancelled, dup=dup)

s = socket.create_connection(("127.0.0.1", port), timeout=5)
s.sendall(submit(1, 1, 1001, 0, 500, 10))
r1 = reply(s)
s.sendall(submit(2, 2, 2001, 1, 500, 4))
r2 = reply(s)
s.sendall(header(12, 0, 77))
hb = recv_exact(s, 12)
print("buy 10 @ 500   ->", r1)
print("sell 4 @ 500   ->", r2)
assert r1["client_seq"] == 1 and r1["rested"] == 10 and r1["filled"] == 0, r1
assert r2["client_seq"] == 2 and r2["filled"] == 4, r2
assert r2["seq"] == r1["seq"] + 1
assert struct.unpack(">HBBQ", hb) == (12, 0, 2, 77)
s.sendall(b"\xff" * 12)   # bad type: the server must close this connection
s.settimeout(3)
assert s.recv(1) == b"", "server did not close a connection that sent a bad message"
print("smoke test passed")
