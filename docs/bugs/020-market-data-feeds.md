# 020: Free real-time market data: what works without a key

## What was done
`tools/feed/record.py` records a public WebSocket feed to a file exactly as received (receive time in ns, a tab, the raw message). `tools/feed/check_book.py` rebuilds the Kraken v2 order book from such a recording and compares every update with the exchange's own CRC32 checksum. Recordings live in `data/` (not committed).

## Findings (my machine, WSL2, one 30 s recording per exchange, BTC-USD / BTC/USD)
- Coinbase Exchange public feed (`wss://ws-feed.exchange.coinbase.com`): subscribing to `level2` was refused. The server answered: "level2, level3, and full channels now require authentication". Without a key only `ticker` and `heartbeat` work: 84 ticker and 29 heartbeat messages in 30 s. A ticker message has price, best bid and ask with sizes, last trade size and side, and a trade id: top of the book and trades, not depth.
- Kraken v2 public feed (`wss://ws.kraken.com/v2`), channel `book`, depth 25, no key: 272 updates and 2 snapshots in the recording.
- Rebuilding the book from that recording and checking each update: checksum matched in 272 of 272 updates, 0 mismatches.

## What this does not show
- One recording of 30 s, one symbol, one run. The connection dropped 3 times in that window (see docs/doubts.md, item 12), so the book data spans only about 7 seconds between the first and last book message.
- The checksum covers the top 10 levels only; the subscription was depth 25 (doubts item 13).
- Nothing yet about Binance, OKX or stocks. Alpaca's free plan is real-time but IEX only and needs an account key (not tried).
- Prices and quantities are decimals (price tick 0.1 in the recording, quantities with 8 decimals). The C++ engine uses integer prices and quantities, so a scaling rule is still to be designed.
