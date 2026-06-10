#!/bin/bash
# Stop all cluster nodes started by start_cluster.sh.

for i in 1 2 3; do
    PIDFILE="./logs/node-$i.pid"
    if [ -f "$PIDFILE" ]; then
        PID=$(cat "$PIDFILE")
        if kill "$PID" 2>/dev/null; then
            echo "Stopped node $i (pid $PID)"
        fi
        rm -f "$PIDFILE"
    fi
done
echo "Cluster stopped."
