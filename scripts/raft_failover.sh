#!/bin/bash
# End-to-end check on real processes and real UDP sockets: elect, replicate orders, kill -9 the leader,
# re-elect, replicate more, restart the dead node from its disk, and require identical order books everywhere.
# usage: scripts/raft_failover.sh [path-to-raft_main] [base-port]
BIN=${1:-./build/raft_main}; PORT=${2:-7100}
W=$(mktemp -d /tmp/meridian-failover.XXXXXX); trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$W"' EXIT
D=$W/disk; mkdir -p "$D"
now_ms() { echo $(( $(date +%s%N) / 1000000 )); }
declare -A PID FD
start_node() {
  local i=$1
  [ -p "$W/in$i" ] || mkfifo "$W/in$i"
  exec {fd}<>"$W/in$i"; FD[$i]=$fd
  "$BIN" --id "$i" --base-port "$PORT" --dir "$D" < "$W/in$i" >> "$W/out$i" 2>>"$W/err$i" &
  PID[$i]=$!
}
last() { grep '^STATE' "$W/out$1" | tail -1; }             # latest STATE line of node $1
field() { last "$1" | tr ' ' '\n' | grep "^$2=" | cut -d= -f2; }
find_leader() {                                              # prints id of the live node that says role=L with the highest term
  local best=-1 bt=-1
  for i in "$@"; do local r t; r=$(field $i role); t=$(field $i term)
    if [ "$r" = L ] && [ "${t:-0}" -gt "$bt" ]; then best=$i; bt=$t; fi; done; echo $best
}
wait_for() { local what=$1 limit=$2; shift 2; local t=$(now_ms); until "$@"; do
  [ $(( $(now_ms) - t )) -gt $limit ] && { echo "TIMEOUT waiting for $what"; for i in 0 1 2; do last $i; done; exit 1; }; sleep 0.05; done; }
caught_up() {                                                # all given nodes: same digest, applied == commit == log
  local d="" c=""
  for i in "$@"; do local dg ap cm lg; dg=$(field $i digest); ap=$(field $i applied); cm=$(field $i commit); lg=$(field $i log)
    [ -n "$dg" ] && [ "$ap" = "$cm" ] && [ "$cm" = "$lg" ] || return 1
    [ -z "$d" ] && { d=$dg; c=$cm; }; [ "$dg" = "$d" ] && [ "$cm" = "$c" ] || return 1; done; [ "$c" -ge "$MIN" ]
}
has_leader() { [ "$(find_leader "$@")" != -1 ]; }

echo "1. start 3 processes (UDP ports $PORT..$((PORT+2)))"
T0=$(now_ms); for i in 0 1 2; do start_node $i; done
wait_for "a leader" 10000 has_leader 0 1 2
L1=$(find_leader 0 1 2); echo "   leader = node $L1 after $(( $(now_ms) - T0 )) ms (term $(field $L1 term))"

echo "2. leader takes 200 order requests"
echo "p 200" >&${FD[$L1]}; MIN=201
wait_for "replication to all 3" 15000 caught_up 0 1 2
echo "   all 3 nodes: commit=$(field 0 commit), orders applied=$(field 0 orders), digest=$(field 0 digest)"

echo "3. kill -9 the leader (node $L1)"
TK=$(now_ms); kill -9 ${PID[$L1]}; wait ${PID[$L1]} 2>/dev/null; exec {FD[$L1]}>&-
LIVE=(); for i in 0 1 2; do [ $i != $L1 ] && LIVE+=($i); done
wait_for "a new leader" 10000 has_leader "${LIVE[@]}"
L2=$(find_leader "${LIVE[@]}")
EL=$(grep "became LEADER" "$W/out$L2" | tail -1 | awk '{print $2}')
echo "   new leader = node $L2 (term $(field $L2 term)); election finished $(( EL - TK )) ms after the kill"

echo "4. new leader takes 200 more"
echo "p 200" >&${FD[$L2]}; MIN=402
wait_for "replication to the 2 live nodes" 15000 caught_up "${LIVE[@]}"
echo "   2 live nodes: commit=$(field ${LIVE[0]} commit), orders applied=$(field ${LIVE[0]} orders)"

echo "5. restart node $L1 from its state file"
: > "$W/out$L1"; start_node $L1
wait_for "restarted node to catch up" 15000 caught_up 0 1 2
echo "   all 3 nodes: commit=$(field 0 commit), orders applied=$(field 0 orders), digest=$(field 0 digest)"
echo
for i in 0 1 2; do last $i; done
if [ "$(field 0 digest)" = "$(field 1 digest)" ] && [ "$(field 1 digest)" = "$(field 2 digest)" ] && [ "$(field 0 orders)" = 400 ]; then
  echo; echo "PASS: identical order books on 3 processes after a kill -9 of the leader"
else echo; echo "FAIL"; exit 1; fi
