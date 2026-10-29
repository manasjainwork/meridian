#!/bin/bash
# Mirror tests: synthetic Kraken-format recordings (8 seeds) must pass through the engine with every checksum matching,
# and the naive apply order (additions before removals) must be caught. With a real recording as argument, that is checked too.
#   scripts/mirror_test.sh [build-dir] [real-recording.jsonl]
B=${1:-build}; REAL=$2; T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0
for seed in 1 2 3 4 5 6 7 8; do
  python3 tools/feed/make_synthetic.py "$T/s$seed.jsonl" $seed || { echo "generator failed (seed $seed)"; exit 1; }
  python3 tools/feed/normalize.py "$T/s$seed.jsonl" --price-decimals 1 --qty-decimals 8 > "$T/s$seed.ev" || exit 1
  py=$(python3 tools/feed/check_book.py "$T/s$seed.jsonl" | sed -n 2p)
  if "$B/mirror_check" "$T/s$seed.ev" > "$T/o$seed.txt"; then r=PASS; else r=FAIL; fail=1; fi
  echo "seed $seed: python $py | engine $r"
done
caught=0
for seed in 1 2 3 4 5 6 7 8; do "$B/mirror_check" "$T/s$seed.ev" --naive > /dev/null || caught=$((caught + 1)); done
echo "naive order (additions first): caught in $caught of 8 seeds"
[ "$caught" -gt 0 ] || fail=1
if [ -n "$REAL" ]; then
  python3 tools/feed/normalize.py "$REAL" --price-decimals 1 --qty-decimals 8 > "$T/real.ev" || exit 1
  echo "--- real recording $REAL"; "$B/mirror_check" "$T/real.ev" || fail=1
fi
[ $fail = 0 ] && echo "ALL PASS" || { echo "SOMETHING FAILED"; exit 1; }
