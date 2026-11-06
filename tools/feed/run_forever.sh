#!/usr/bin/env bash
# Records the Kraken BTC/USD order book (25 levels) and trades, one file per hour, until stopped. Never edits a finished file.
#   start:  nohup bash tools/feed/run_forever.sh > data/live/runner.log 2>&1 &
#   stop:   touch data/live/STOP        (it finishes the current file, then exits)
# Each finished file is compressed (.jsonl.gz) and its sha256 is appended to data/live/MANIFEST.txt.
# Every file starts with a fresh book snapshot, so files are independent. Short gaps between files are normal and show in the timestamps.
set -u
DIR=${DIR:-data/live}
SECS=${SECS:-3600}
MIN_FREE_MB=${MIN_FREE_MB:-5000}
MIN_LOOP=${MIN_LOOP:-10}   # never loop faster than this many seconds
REC=${REC:-python3 tools/feed/record.py}
BOOK='{"method":"subscribe","params":{"channel":"book","symbol":["BTC/USD"],"depth":25,"snapshot":true}}'
TRADE='{"method":"subscribe","params":{"channel":"trade","symbol":["BTC/USD"],"snapshot":false}}'
mkdir -p "$DIR"
while [ ! -e "$DIR/STOP" ]; do
  free_mb=$(df -Pm "$DIR" | awk 'NR==2{print $4}')
  if [ "$free_mb" -lt "$MIN_FREE_MB" ]; then echo "$(date -u +%FT%TZ) stopping: only ${free_mb} MB free"; break; fi
  f="$DIR/kraken-btcusd-$(date -u +%Y%m%dT%H%M%SZ).jsonl"
  echo "$(date -u +%FT%TZ) recording $f"
  t0=$SECONDS
  if $REC --url wss://ws.kraken.com/v2 --subscribe "$BOOK" --subscribe "$TRADE" --seconds "$SECS" --retries 20 --out "$f" > "$f.log" 2>&1; then ok=1; else ok=0; fi
  # a failed attempt that recorded almost nothing is not worth keeping
  if [ "$ok" -eq 0 ] && [ "$(stat -c %s "$f" 2>/dev/null || echo 0)" -lt 10000 ]; then rm -f "$f"; fi
  if [ -s "$f" ]; then
    gzip -f "$f" && (cd "$DIR" && sha256sum "$(basename "$f").gz" >> MANIFEST.txt)
    echo "$(date -u +%FT%TZ) saved $f.gz"
  fi
  rm -f "$f.log" 2>/dev/null
  [ $((SECONDS - t0)) -ge "$MIN_LOOP" ] || sleep "$MIN_LOOP"
  [ "$ok" -eq 1 ] || { echo "$(date -u +%FT%TZ) recorder failed, waiting 30 s"; sleep 30; }
done
echo "$(date -u +%FT%TZ) exited"
