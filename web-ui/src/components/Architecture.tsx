// What exists, why, and where it is documented. "built" means it runs and has a write-up in docs/bugs; "planned" means it does not exist yet.
const parts: { name: string; status: "built" | "planned"; why: string; doc: string }[] = [
  { name: "Matching engine (C++20)", status: "built", why: "Price-time priority order book with integer price levels, GTC/IOC/FOK/market/modify. The core every other part serves.", doc: "docs/bugs" },
  { name: "Raft consensus", status: "built", why: "Several engine copies must agree on the order of orders. Raft is written as a pure state machine so a simulator can inject faults and a mutation test can prove the tests would notice a bug.", doc: "docs/bugs" },
  { name: "Replicated books", status: "built", why: "Each replica applies the committed log to its own book; identical digests prove they agree, including after crashes.", doc: "doc 017" },
  { name: "Real processes, kill -9 failover", status: "built", why: "Raft over UDP between separate OS processes: kill the leader, a new one is elected, the old one rejoins and catches up.", doc: "doc 018" },
  { name: "Pre-vote and CheckQuorum", status: "built", why: "A node cut off from the others must not inflate the term and disrupt a healthy cluster when it comes back.", doc: "doc 019" },
  { name: "Market data recorder and checksum check", status: "built", why: "Real Kraken book data, recorded with timestamps and verified against the exchange's own checksum, so replays are of data we know was understood correctly.", doc: "doc 020" },
  { name: "Real book mirrored in the engine", status: "built", why: "The recorded exchange book is rebuilt inside our order book and its checksum is recomputed from the engine's own levels. This page replays that recording.", doc: "doc 021" },
  { name: "Gateway and order management", status: "planned", why: "A client path: HTTP API, idempotent order ids, cancel and modify. Needed for a real order ticket and for Raft panel actions.", doc: "next" },
  { name: "Engine in the browser (WASM)", status: "planned", why: "So a public link runs the real engine instead of only replaying data.", doc: "later" },
  { name: "Snapshots and log compaction", status: "planned", why: "A restarted node replays the whole log today; snapshots bound that.", doc: "later" },
];

export default function Architecture() {
  return (
    <div className="arch">
      <div className="panel">
        <h3>Data path of this page</h3>
        <svg viewBox="0 0 760 110" className="chart flow">
          {([["Kraken feed", 10, 110], ["Recorder + checksum", 150, 170], ["Mirror in engine", 340, 150], ["replay.json", 520, 100], ["This page", 650, 100]] as [string, number, number][]).map(([t, x, w]) => (
            <g key={t}><rect x={x} y={30} width={w} height={50} rx={8} className="box" /><text x={x + w / 2} y={59} textAnchor="middle" className="boxt">{t}</text></g>
          ))}
          {[[120, 150], [320, 340], [490, 520], [620, 650]].map(([a, b], i) => <line key={i} x1={a} x2={b} y1={55} y2={55} className="arrow" />)}
          <text x={380} y={100} textAnchor="middle" className="axis">the mirror is a test of the engine; the page replays the same recording</text>
        </svg>
      </div>
      <div className="panel">
        <h3>Parts and why they exist</h3>
        {parts.map((p) => (
          <div className="part" key={p.name}>
            <span className={"st " + p.status}>{p.status}</span>
            <div><b>{p.name}</b> <span className="muted small">({p.doc})</span><div className="small">{p.why}</div></div>
          </div>
        ))}
      </div>
    </div>
  );
}
