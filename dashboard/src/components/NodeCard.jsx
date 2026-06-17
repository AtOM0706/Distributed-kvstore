import React from 'react';

export default function NodeCard({ id, node }) {
  const status = node?.status;
  const alive = node?.connected;
  const role = !alive ? 'dead' : status?.role || 'unknown';

  const fmt = (v) => (v === undefined || v === null ? '—' : v.toLocaleString());

  return (
    <div className={`card node-card ${role === 'leader' ? 'leader' : ''} ${!alive ? 'dead' : ''}`}>
      <div className="top">
        <div className="name">Node {id}</div>
        <div className={`role-badge ${role}`}>{!alive ? 'DOWN' : role}</div>
      </div>
      <div className="node-stats">
        <div className="node-stat">
          <div className="label">Term</div>
          <div className="value">{fmt(status?.term)}</div>
        </div>
        <div className="node-stat">
          <div className="label">Commit index</div>
          <div className="value">{fmt(status?.commit_index)}</div>
        </div>
        <div className="node-stat">
          <div className="label">Keys</div>
          <div className="value">{fmt(status?.metrics?.total_keys)}</div>
        </div>
        <div className="node-stat">
          <div className="label">WAL size</div>
          <div className="value">
            {status?.metrics
              ? `${(status.metrics.wal_size_bytes / 1024).toFixed(1)} KB`
              : '—'}
          </div>
        </div>
        <div className="node-stat">
          <div className="label">Writes/s</div>
          <div className="value">{fmt(status?.metrics?.writes_per_sec)}</div>
        </div>
        <div className="node-stat">
          <div className="label">Uptime</div>
          <div className="value">
            {status?.metrics ? `${status.metrics.uptime_sec}s` : '—'}
          </div>
        </div>
      </div>
    </div>
  );
}
