import { useEffect, useRef, useState } from 'react';
import { NODES, DEAD_AFTER_MS } from '../utils/constants';

/* -----------------------------------------------------------------------
 * Connects to every node's WebSocket port, auto-reconnects with backoff,
 * and merges all node statuses into one cluster state object:
 *
 * {
 *   nodes: { 1: { status, connected, lastSeen }, 2: {...}, 3: {...} },
 *   events: [ { node, type, msg, at } ... ]  (merged, newest last)
 * }
 * ----------------------------------------------------------------------- */

export default function useWebSocket() {
  const [cluster, setCluster] = useState({ nodes: {}, events: [] });
  const socketsRef = useRef({});
  const seenEvents = useRef(new Set());

  useEffect(() => {
    let alive = true;
    const timers = {};

    const connect = (node) => {
      if (!alive) return;
      let ws;
      try {
        ws = new WebSocket(node.wsUrl);
      } catch {
        timers[node.id] = setTimeout(() => connect(node), 2000);
        return;
      }
      socketsRef.current[node.id] = ws;

      ws.onmessage = (e) => {
        let msg;
        try {
          msg = JSON.parse(e.data);
        } catch {
          return;
        }
        if (msg.type !== 'status') return;

        setCluster((prev) => {
          const nodes = {
            ...prev.nodes,
            [msg.node_id]: {
              status: msg,
              connected: true,
              lastSeen: Date.now(),
            },
          };

          /* Merge new events (dedupe across reconnects/nodes) */
          let events = prev.events;
          if (msg.events && msg.events.length) {
            const fresh = msg.events.filter((ev) => {
              const key = `${msg.node_id}|${ev.type}|${ev.msg}|${ev.at}`;
              if (seenEvents.current.has(key)) return false;
              seenEvents.current.add(key);
              return true;
            });
            if (fresh.length) {
              events = [
                ...prev.events,
                ...fresh.map((ev) => ({
                  node: msg.node_id,
                  receivedAt: Date.now(),
                  ...ev,
                })),
              ].slice(-200);
            }
          }
          return { nodes, events };
        });
      };

      ws.onclose = () => {
        if (!alive) return;
        setCluster((prev) => ({
          ...prev,
          nodes: prev.nodes[node.id]
            ? {
                ...prev.nodes,
                [node.id]: { ...prev.nodes[node.id], connected: false },
              }
            : prev.nodes,
        }));
        timers[node.id] = setTimeout(() => connect(node), 1500);
      };

      ws.onerror = () => ws.close();
    };

    NODES.forEach(connect);

    /* Periodically mark stale nodes as disconnected */
    const staleTimer = setInterval(() => {
      setCluster((prev) => {
        let changed = false;
        const nodes = { ...prev.nodes };
        for (const id of Object.keys(nodes)) {
          const n = nodes[id];
          if (n.connected && Date.now() - n.lastSeen > DEAD_AFTER_MS) {
            nodes[id] = { ...n, connected: false };
            changed = true;
          }
        }
        return changed ? { ...prev, nodes } : prev;
      });
    }, 1000);

    return () => {
      alive = false;
      clearInterval(staleTimer);
      Object.values(timers).forEach(clearTimeout);
      Object.values(socketsRef.current).forEach((ws) => {
        try {
          ws.close();
        } catch {
          /* ignore */
        }
      });
    };
  }, []);

  return cluster;
}
