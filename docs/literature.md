# Literature and references

Only works that this project actually used are under "Used". Everything under "Not read yet" is a plan, not a source. No PDFs are
stored here; follow the links.

## Used
- Ongaro and Ousterhout, "In Search of an Understandable Consensus Algorithm" (Raft), USENIX ATC 2014. https://raft.github.io/raft.pdf
  Taken: leader election, log replication, commit rule. Where: `include/meridian/raft*`, docs/bugs/015.
- Ongaro, "Consensus: Bridging Theory and Practice" (PhD thesis, Stanford 2014). https://github.com/ongardie/dissertation
  Taken: the pre-vote phase and leader CheckQuorum. Where: raft pre-vote code and tests (`raft_prevote`).
- Kraken WebSocket API v2 book documentation (checksum rules). https://docs.kraken.com/api/
  Taken: the CRC32 checksum over the top 10 asks and bids. Where: `tools/feed/check_book.py`, `src/mirror_check.cpp`, docs 020/021.

## Not read yet (plans, not sources)
- Liskov and Cowling, "Viewstamped Replication Revisited" (2012): another view of the same replication problem.
- Cont, Stoikov, Talreja, "A Stochastic Model for Order Book Dynamics", Operations Research 2010.
- Cont, Kukanov, Stoikov, "The Price Impact of Order Book Events", Journal of Financial Econometrics 2014 (order flow imbalance).
- Avellaneda and Stoikov, "High-frequency trading in a limit order book", Quantitative Finance 2008 (market making).
- Ntakaris et al., "Benchmark Dataset for Mid-Price Forecasting of Limit Order Book Data" (FI-2010), 2018.
