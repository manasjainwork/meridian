#!/usr/bin/env python3
"""End to end test of the gateway over real HTTP: starts the binary, sends requests, checks the answers and the final state.

    python3 tools/gateway_test.py build/gateway [port [bug]]

Checks: a crossing trade with exact position and cash; a retry (same cl_ord_id) is answered as a duplicate and changes nothing,
including 8 threads x 25 retries at once (exactly one execution); the same key with a different quantity is refused; cancel and
modify by the wrong account are refused; limits; malformed requests get 4xx and never crash the server; the server's own full
consistency check (/debug/check) passes at the end.
"""
import json, subprocess, sys, threading, time, urllib.error, urllib.request

EXE = sys.argv[1] if len(sys.argv) > 1 else "build/gateway"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8091
BUG = sys.argv[3] if len(sys.argv) > 3 else "0"      # tests of the test: start the gateway with a deliberate OMS bug, this script must then FAIL
BASE = f"http://127.0.0.1:{PORT}"
fails = []


def call(method, path, body=None, raw=None):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(BASE + path, data=data, method=method, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def check(cond, what):
    if not cond:
        fails.append(what)
        print("FAIL:", what)


def order(acc, cl, side, price, qty, **kw):
    b = {"account": acc, "cl_ord_id": cl, "side": side, "type": "limit", "tif": "gtc", "price": price, "qty": qty}
    b.update(kw)
    return call("POST", "/orders", b)


p = subprocess.Popen([EXE, "--port", str(PORT), "--max-qty", "1000000", "--max-position", "5000000", "--bug", BUG], stdout=subprocess.PIPE, text=True)
try:
    for _ in range(100):
        try:
            if call("GET", "/health")[0] == 200: break
        except Exception:
            time.sleep(0.05)
    else:
        sys.exit("gateway did not start")

    st, cfg = call("GET", "/config")
    check(st == 200 and cfg["price_decimals"] == 1 and cfg["qty_decimals"] == 5, "config")

    # 1 sells 100000 lots at 800000 ticks; 2 buys 60000 at 800100: trades 60000 at the resting price
    st, a = order(1, 1, "sell", 800000, 100000)
    check(st == 200 and a["status"] == "open" and a["rested"] == 100000, "rest a sell")
    st, b = order(2, 1, "buy", 800100, 60000)
    check(st == 200 and b["status"] == "filled" and b["filled"] == 60000 and b["fills"][0]["price"] == 800000, "crossing buy trades at the resting price")
    _, acc1 = call("GET", "/account?account=1"); _, acc2 = call("GET", "/account?account=2")
    check(acc1["position"] == -60000 and acc1["cash"] == 800000 * 60000, "seller position and cash")
    check(acc2["position"] == 60000 and acc2["cash"] == -800000 * 60000, "buyer position and cash")
    check(acc1["open_sell"] == 40000 and len(acc1["open_orders"]) == 1 and acc1["open_orders"][0]["remaining"] == 40000, "open order of the seller")
    _, tr = call("GET", "/trades")
    check(len(tr["trades"]) == 1 and tr["trades"][0]["aggressor"] == "buy" and tr["trades"][0]["qty"] == 60000, "trade tape")
    _, bk = call("GET", "/book?depth=5")
    check(bk["asks"] == [[800000, 40000]] and bk["bids"] == [], "book after the trade")

    # retry: same key and contents -> duplicate, same order id, nothing changes
    st, b2 = order(2, 1, "buy", 800100, 60000)
    check(st == 200 and b2["duplicate"] is True and b2["order_id"] == b["order_id"], "retry is a duplicate")
    _, acc2b = call("GET", "/account?account=2")
    check(acc2b["position"] == 60000, "retry did not trade again")
    # same key, different quantity -> refused
    st, k = order(2, 1, "buy", 800100, 61000)
    check(st == 422 and k["reject"] == "key_reuse", "same key with other contents is refused")

    # concurrent retries of one new request: exactly one execution
    results = []
    def worker():
        for _ in range(25):
            results.append(order(3, 777, "buy", 800000, 5000))
    ts = [threading.Thread(target=worker) for _ in range(8)]
    [t.start() for t in ts]; [t.join() for t in ts]
    fresh = [r for st, r in results if st == 200 and not r["duplicate"]]
    dups = [r for st, r in results if st == 200 and r["duplicate"]]
    check(len(fresh) == 1 and len(dups) == 199 and all(r["order_id"] == fresh[0]["order_id"] for r in dups), "200 concurrent retries: one execution, 199 duplicates")
    _, acc3 = call("GET", "/account?account=3")
    check(acc3["position"] == 5000, "concurrent retries traded exactly once")

    # ownership
    oid = a["order_id"]
    st, x = call("POST", "/cancel", {"account": 2, "order_id": oid})
    check(st == 422 and x["reject"] == "not_owner", "cancel by another account is refused")
    st, x = call("POST", "/modify", {"account": 2, "order_id": oid, "price": 800001, "qty": 10})
    check(st == 422 and x["reject"] == "not_owner", "modify by another account is refused")
    st, x = call("POST", "/modify", {"account": 1, "order_id": oid, "price": 800005, "qty": 30000})
    check(st == 200 and x["status"] == "open" and x["rested"] == 30000, "modify by the owner")
    st, x = call("POST", "/cancel", {"account": 1, "order_id": oid})
    check(st == 200 and x["status"] == "cancelled" and x["cancelled"] == 30000, "cancel by the owner")
    st, x = call("POST", "/cancel", {"account": 1, "order_id": oid})
    check(st == 422 and x["reject"] == "not_open", "cancelling twice is refused")

    # limits
    st, x = order(4, 1, "buy", 700000, 1000001)
    check(st == 422 and x["reject"] == "qty_limit", "order size limit")
    for i in range(5):
        order(5, 10 + i, "buy", 700000, 1000000)
    st, x = order(5, 99, "buy", 700000, 1000000)
    check(st == 422 and x["reject"] == "position_limit", "position limit counts open orders")

    # stop orders: a buy stop waits, a trade at its price fires it, a cancelled stop never fires
    _, b8 = order(8, 1, "buy", 790000, 3000)
    order(9, 1, "sell", 790000, 1, tif="ioc")                                    # trade at 790000: the last price is now known
    st, s = order(7, 1, "buy", 0, 500, type="stop", stop_price=791000)
    check(st == 200 and s["status"] == "pending", "stop order is accepted and waits")
    st, a7 = call("GET", "/account?account=7")
    check(len(a7["stops"]) == 1 and a7["stops"][0]["stop_price"] == 791000 and a7["position"] == 0, "waiting stop is listed, nothing traded")
    st, again = order(7, 1, "buy", 0, 500, type="stop", stop_price=791000)
    check(again["duplicate"] and again["order_id"] == s["order_id"], "retry of a stop order is answered from memory")
    _, a9 = order(9, 2, "sell", 791000, 1000)
    st, t = order(10, 1, "buy", 791000, 100, tif="ioc")
    check(st == 200 and t["filled"] == 100 and t["triggered_fills"] == 1, "a trade at the stop price fires the stop")
    _, a7 = call("GET", "/account?account=7")
    check(a7["position"] == 500 and a7["stops"] == [], "fired stop traded exactly its quantity and left the waiting list")
    st, s2 = order(7, 2, "buy", 0, 100, type="stop", stop_price=795000)
    st, cx = call("POST", "/cancel", {"account": 7, "order_id": s2["order_id"]})
    check(st == 200 and cx["status"] == "cancelled", "a waiting stop can be cancelled")
    check(call("POST", "/orders", {"account": 7, "cl_ord_id": 3, "side": "buy", "type": "stop", "qty": 5})[0] == 400, "stop order without stop_price is refused")
    call("POST", "/cancel", {"account": 8, "order_id": b8["order_id"]}); call("POST", "/cancel", {"account": 9, "order_id": a9["order_id"]})

    # malformed input never crashes the server
    check(call("POST", "/orders", raw=b"{not json")[0] == 400, "bad JSON")
    check(call("POST", "/orders", {"account": 1})[0] == 400, "missing fields")
    check(call("POST", "/orders", {"account": -1, "cl_ord_id": 1, "side": "buy", "price": 5, "qty": 5})[0] == 400, "negative account")
    check(call("POST", "/orders", {"account": 1, "cl_ord_id": 1, "side": "up", "price": 5, "qty": 5})[0] == 400, "bad side")
    check(call("POST", "/orders", {"account": 1, "cl_ord_id": 5, "side": "buy", "price": 99999999999, "qty": 5})[0] == 400, "price too large for 32 bits")
    check(call("POST", "/orders", raw=b"x" * 20000)[0] == 413, "oversized body")
    check(call("GET", "/orders")[0] == 405, "wrong method")
    check(call("GET", "/nothing")[0] == 404, "unknown path")
    check(call("GET", "/health")[0] == 200 and p.poll() is None, "server still alive")

    st, chk = call("GET", "/debug/check")
    check(st == 200 and chk["ok"], "internal consistency check: " + str(chk))
finally:
    p.terminate()
    p.wait(timeout=5)

print("ALL PASS" if not fails else f"{len(fails)} FAILED")
sys.exit(1 if fails else 0)
