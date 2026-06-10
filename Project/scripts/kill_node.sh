#!/bin/bash
# Kill one node (for failover demos): ./scripts/kill_node.sh <node_id>
set -e

ID="$1"
if [ -z "$ID" ]; then
    echo "usage: $0 <node_id 1-3>"
    exit 1
fi

PIDFILE="./logs/node-$ID.pid"
if [ ! -f "$PIDFILE" ]; then
    echo "No pid file for node $ID ($PIDFILE)"
    exit 1
fi

PID=$(cat "$PIDFILE")
if kill -9 "$PID" 2>/dev/null; then
    echo "Killed node $ID (pid $PID) — watch the dashboard react"
    rm -f "$PIDFILE"
else
    echo "Node $ID (pid $PID) was not running"
fi
