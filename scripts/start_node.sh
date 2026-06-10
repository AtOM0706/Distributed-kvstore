#!/bin/bash
# (Re)start a single node — e.g. after kill_node.sh, to demo WAL recovery.
# Usage: ./scripts/start_node.sh <node_id> [build_dir]
set -e

ID="$1"
BUILD_DIR="${2:-./build}"
BIN="$BUILD_DIR/kvstore-server"

if [ -z "$ID" ]; then
    echo "usage: $0 <node_id 1-3> [build_dir]"
    exit 1
fi

mkdir -p data logs

PEERS=""
for j in 1 2 3; do
    [ "$j" != "$ID" ] && PEERS="$PEERS --peer 127.0.0.1:$((7000 + j))"
done

"$BIN" --id "$ID" \
    --client-port $((6000 + ID)) \
    --raft-port $((7000 + ID)) \
    --ws-port $((8000 + ID)) \
    $PEERS \
    --data-dir "./data/node-$ID" \
    >> "./logs/node-$ID.log" 2>&1 &

echo "$!" > "./logs/node-$ID.pid"
echo "Restarted node $ID (pid $!) — it will replay its WAL and rejoin"
