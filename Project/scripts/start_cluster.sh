#!/bin/bash
# Launch a 3-node kvstore cluster in the background.
# Usage: ./scripts/start_cluster.sh [build_dir]
set -e

BUILD_DIR="${1:-./build}"
BIN="$BUILD_DIR/kvstore-server"

if [ ! -x "$BIN" ]; then
    echo "Server binary not found at $BIN"
    echo "Build first:  mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
    exit 1
fi

mkdir -p data logs

for i in 1 2 3; do
    PEERS=""
    for j in 1 2 3; do
        [ "$j" != "$i" ] && PEERS="$PEERS --peer 127.0.0.1:$((7000 + j))"
    done

    "$BIN" --id "$i" \
        --client-port $((6000 + i)) \
        --raft-port $((7000 + i)) \
        --ws-port $((8000 + i)) \
        $PEERS \
        --data-dir "./data/node-$i" \
        > "./logs/node-$i.log" 2>&1 &

    echo "$!" > "./logs/node-$i.pid"
    echo "Started node $i (pid $!)  client=:$((6000 + i)) raft=:$((7000 + i)) ws=:$((8000 + i))"
done

echo ""
echo "Cluster starting. Logs in ./logs/, data in ./data/"
echo "  CLI:        $BUILD_DIR/kvstore-cli --port 6001"
echo "  Dashboard:  cd dashboard && npm run dev   (http://localhost:3000)"
echo "  Stop all:   ./scripts/stop_cluster.sh"
