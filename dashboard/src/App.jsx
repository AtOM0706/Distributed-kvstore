import React from 'react';
import useWebSocket from './hooks/useWebSocket';
import NodeCard from './components/NodeCard';
import StatsBar from './components/StatsBar';
import ThroughputChart from './components/ThroughputChart';
import HashRing from './components/HashRing';
import LogViewer from './components/LogViewer';
import { NODES } from './utils/constants';

export default function App() {
  const { nodes, events } = useWebSocket();

  const leader = Object.values(nodes).find(
    (n) => n.connected && n.status?.role === 'leader'
  );

  return (
    <div className="app">
      <div className="header">
        <div>
          <h1>
            <span>◆</span> KVStore Cluster
          </h1>
          <div className="sub">
            Distributed key-value store · Raft consensus · 3 nodes
          </div>
        </div>
        <div className="sub mono">
          {leader
            ? `leader: node ${leader.status.node_id} · term ${leader.status.term}`
            : 'no leader — election in progress'}
        </div>
      </div>

      <StatsBar nodes={nodes} />

      <div className="grid-nodes">
        {NODES.map((n) => (
          <NodeCard key={n.id} id={n.id} node={nodes[n.id]} />
        ))}
      </div>

      <div className="grid-mid">
        <ThroughputChart nodes={nodes} />
        <HashRing nodes={nodes} />
      </div>

      <div className="grid-bottom">
        <LogViewer events={events} />
      </div>
    </div>
  );
}
