import { useCallback, useEffect, useRef, useState } from "react";
import { api, type AccountData, type BookData, type Config, type OrderResult, type TradeItem } from "../gateway";
import { fmtUnits, parseUnits } from "../units";

const DEFAULT_BASE = "http://127.0.0.1:8081";
const safe = (k: string, d: string) => { try { return localStorage.getItem(k) ?? d; } catch { return d; } };
const save = (k: string, v: string) => { try { localStorage.setItem(k, v); } catch { /* storage can be unavailable */ } };

interface LogLine { id: number; text: string; kind: "ok" | "dup" | "bad"; }

// Talks to the C++ gateway (tools: build/gateway). Every number shown comes from the gateway; nothing is simulated here.
export default function Trade() {
  const [base, setBase] = useState(safe("gw-base", DEFAULT_BASE));
  const [account, setAccount] = useState(safe("gw-account", "1"));
  const [cfg, setCfg] = useState<Config | null>(null);
  const [online, setOnline] = useState(false);
  const [book, setBook] = useState<BookData | null>(null);
  const [trades, setTrades] = useState<TradeItem[]>([]);
  const [acct, setAcct] = useState<AccountData | null>(null);
  const [log, setLog] = useState<LogLine[]>([]);
  const [side, setSide] = useState<"buy" | "sell">("buy");
  const [type, setType] = useState<"limit" | "market">("limit");
  const [tif, setTif] = useState<"gtc" | "ioc" | "fok">("gtc");
  const [price, setPrice] = useState("");
  const [qty, setQty] = useState("0.1");
  const [editing, setEditing] = useState<number | null>(null);
  const [mPrice, setMPrice] = useState(""); const [mQty, setMQty] = useState("");
  const clId = useRef(Date.now());                 // new ids never repeat across page reloads
  const lastReq = useRef<Record<string, unknown> | null>(null);
  const logId = useRef(0);

  const pd = cfg?.price_decimals ?? 1, qd = cfg?.qty_decimals ?? 5;
  const accNum = Number(account);
  const push = (text: string, kind: LogLine["kind"]) => setLog((l) => [{ id: ++logId.current, text, kind }, ...l].slice(0, 8));

  const refresh = useCallback(async () => {
    try {
      const [b, t, a] = await Promise.all([
        api<BookData>(base, "GET", "/book?depth=10"), api<{ trades: TradeItem[] }>(base, "GET", "/trades"),
        api<AccountData>(base, "GET", `/account?account=${accNum > 0 ? accNum : 1}`)]);
      setBook(b.data); setTrades(t.data.trades); setAcct(a.data); setOnline(true);
    } catch { setOnline(false); }
  }, [base, accNum]);

  useEffect(() => {
    let dead = false;
    api<Config>(base, "GET", "/config").then((r) => { if (!dead) setCfg(r.data); }).catch(() => { if (!dead) setOnline(false); });
    refresh();
    const t = setInterval(refresh, 500);
    return () => { dead = true; clearInterval(t); };
  }, [base, refresh]);

  const describe = (path: string, status: number, r: OrderResult) => {
    const q = (n: number) => fmtUnits(n, qd);
    const why: Record<string, string> = {
      bad_param: `bad parameter: price must be between ${fmtUnits(1, pd)} and ${fmtUnits((cfg?.max_price_ticks ?? 2000000) - 1, pd)}, quantity above zero`,
      qty_limit: `above the maximum order size of ${fmtUnits(cfg?.max_order_qty_lots ?? 0, qd)} BTC`,
      position_limit: `would exceed the position limit of ${fmtUnits(cfg?.max_position_lots ?? 0, qd)} BTC (open orders count)`,
      key_reuse: "this client order id was already used for a different request", not_owner: "that order belongs to another account",
      not_open: "that order is not open any more", unknown_order: "no such order", pool_full: "the order book is full" };
    const what = r.ok ? `${r.status}: filled ${q(r.filled)}, rested ${q(r.rested)}, cancelled ${q(r.cancelled)} (${r.fills.length} fills), order ${r.order_id}` : `REJECTED, ${why[r.reject] ?? r.reject ?? r.error}`;
    push(`${path} -> ${status}: ${what}${r.duplicate ? "  [duplicate: answered from memory, nothing traded again]" : ""}`, r.duplicate ? "dup" : r.ok ? "ok" : "bad");
  };

  const send = async (path: string, body: Record<string, unknown>) => {
    try {
      const { status, data } = await api<OrderResult>(base, "POST", path, body);
      describe(path, status, data);
    } catch { push(`${path} -> could not reach the gateway`, "bad"); }
    refresh();
  };

  const placeOrder = () => {
    const qn = parseUnits(qty, qd);
    if (qn === null || qn === 0) return push(`quantity must be a positive number with at most ${qd} decimals`, "bad");
    const body: Record<string, unknown> = { account: accNum, cl_ord_id: ++clId.current, side, type, qty: qn };
    if (type === "limit") {
      const pn = parseUnits(price, pd);
      if (pn === null || pn === 0) return push(`price must be a positive number with at most ${pd} decimals`, "bad");
      body.price = pn; body.tif = tif;
    }
    lastReq.current = body;
    send("/orders", body);
  };

  const retry = () => { if (lastReq.current) send("/orders", lastReq.current); else push("nothing to retry yet", "bad"); };
  const cancel = (id: number) => send("/cancel", { account: accNum, order_id: id });
  const modify = (id: number) => {
    const pn = parseUnits(mPrice, pd), qn = parseUnits(mQty, qd);
    if (!pn || !qn) return push("modify needs a price and a quantity", "bad");
    setEditing(null); send("/modify", { account: accNum, order_id: id, price: pn, qty: qn });
  };

  const money = (n: number | null) => (n === null ? "n/a" : fmtUnits(n, pd + qd));
  const asks = (book?.asks ?? []).slice(0, 8).reverse(), bids = (book?.bids ?? []).slice(0, 8);
  const peak = Math.max(1, ...asks.map((l) => l[1]), ...bids.map((l) => l[1]));
  const level = (l: [number, number], cls: string) => (
    <div className={"row " + cls} key={cls + l[0]} onClick={() => { setPrice(fmtUnits(l[0], pd, false)); setType("limit"); }} title="click to use this price">
      <i style={{ width: `${(l[1] / peak) * 100}%` }} /><span>{fmtUnits(l[0], pd)}</span><span>{fmtUnits(l[1], qd)}</span>
    </div>);

  return (
    <div className="trade">
      <div className="panel conn">
        <span className={"dot " + (online ? "on" : "off")} />
        <b>{online ? "Gateway connected" : "Gateway not reachable"}</b>
        <span className="muted small">{online ? "orders below are real calls to the C++ order management layer" : "start it: ./build/gateway --port 8081 --seed-mid 800000"}</span>
        <label className="small">gateway <input value={base} onChange={(e) => { setBase(e.target.value); save("gw-base", e.target.value); }} /></label>
        <label className="small">account <input className="narrow" value={account} onChange={(e) => { setAccount(e.target.value); save("gw-account", e.target.value); }} /></label>
      </div>
      <div className="tgrid">
        <div className="stack">
          <div className="panel"><h3>Order book</h3>
            <div className="ladder">
              {asks.map((l) => level(l, "ask"))}
              <div className="row mid"><span>{book?.bids[0] && book?.asks[0] ? `spread ${fmtUnits(book.asks[0][0] - book.bids[0][0], pd)}` : "no two-sided book"}</span><span /></div>
              {bids.map((l) => level(l, "bid"))}
            </div>
          </div>
          <div className="panel"><h3>Trades</h3>
            <div className="tape">{trades.slice(0, 8).map((t) => (
              <div className={"trow " + t.aggressor} key={t.seq}><span>{fmtUnits(t.price, pd)}</span><span>{fmtUnits(t.qty, qd)}</span><span className="muted">#{t.seq}</span></div>))}
              {trades.length === 0 && <p className="muted small">No trades yet.</p>}</div>
          </div>
        </div>
        <div className="stack">
          <div className="panel"><h3>Order ticket (account {account})</h3>
            <div className="seg">
              <button className={side === "buy" ? "on buy" : ""} onClick={() => setSide("buy")}>Buy</button>
              <button className={side === "sell" ? "on sell" : ""} onClick={() => setSide("sell")}>Sell</button>
              <button className={type === "limit" ? "on" : ""} onClick={() => setType("limit")}>Limit</button>
              <button className={type === "market" ? "on" : ""} onClick={() => setType("market")}>Market</button>
            </div>
            <div className="form">
              {type === "limit" && <label>Price<input value={price} onChange={(e) => setPrice(e.target.value)} placeholder={`e.g. ${fmtUnits(book?.asks[0]?.[0] ?? 800000, pd, false)}`} /></label>}
              <label>Quantity (BTC)<input value={qty} onChange={(e) => setQty(e.target.value)} /></label>
              {type === "limit" && <label>Time in force<select value={tif} onChange={(e) => setTif(e.target.value as "gtc" | "ioc" | "fok")}>
                <option value="gtc">GTC (rest the remainder)</option><option value="ioc">IOC (cancel the remainder)</option><option value="fok">FOK (all or nothing)</option></select></label>}
            </div>
            <div className="seg"><button className={"go " + side} onClick={placeOrder} disabled={!online}>{side === "buy" ? "Buy" : "Sell"} BTC</button>
              <button onClick={retry} disabled={!online} title="Sends the same request again with the same client order id">Retry last order</button></div>
            <p className="muted small">Retry re-sends the exact same request (same client order id). The gateway answers from memory and trades nothing again.</p>
          </div>
          <div className="panel"><h3>Position and PnL (account {account})</h3>
            {acct ? <div className="stats2">
              <div><span className="k">Position</span>{fmtUnits(acct.position, qd)} BTC</div>
              <div><span className="k">Cash</span>{money(acct.cash)}</div>
              <div><span className="k">Mark (mid)</span>{acct.mark === null ? "n/a" : fmtUnits(acct.mark, pd)}</div>
              <div><span className="k">PnL at mark</span>{money(acct.equity)}</div>
              <div><span className="k">Open buy / sell</span>{fmtUnits(acct.open_buy, qd)} / {fmtUnits(acct.open_sell, qd)}</div>
            </div> : <p className="muted small">No data.</p>}
          </div>
          <div className="panel"><h3>Open orders</h3>
            {acct?.open_orders.length === 0 && <p className="muted small">None.</p>}
            {acct?.open_orders.map((o) => (
              <div className="oo" key={o.order_id}>
                <span className={o.side}>{o.side}</span><span>{fmtUnits(o.remaining, qd)} @ {fmtUnits(o.price, pd)}</span>
                {editing === o.order_id ? <>
                  <input className="narrow" value={mPrice} onChange={(e) => setMPrice(e.target.value)} placeholder="price" />
                  <input className="narrow" value={mQty} onChange={(e) => setMQty(e.target.value)} placeholder="qty" />
                  <button onClick={() => modify(o.order_id)}>OK</button><button onClick={() => setEditing(null)}>x</button></> : <>
                  <button onClick={() => { setEditing(o.order_id); setMPrice(fmtUnits(o.price, pd, false)); setMQty(fmtUnits(o.remaining, qd, false)); }}>Modify</button>
                  <button onClick={() => cancel(o.order_id)}>Cancel</button></>}
              </div>))}
          </div>
          <div className="panel"><h3>Gateway responses</h3>
            {log.length === 0 && <p className="muted small">Nothing sent yet.</p>}
            {log.map((l) => <div key={l.id} className={"log " + l.kind}>{l.text}</div>)}
          </div>
        </div>
      </div>
    </div>
  );
}
