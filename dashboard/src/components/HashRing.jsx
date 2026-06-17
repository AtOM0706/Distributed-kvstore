import React, { useMemo, useState } from 'react';
import { NODE_COLORS, COLORS } from '../utils/constants';

const SIZE = 260;
const CX = SIZE / 2;
const CY = SIZE / 2;
const R = 100;

/* SVG visualization of the consistent hash ring: every virtual node is a
 * dot positioned by hash value (angle = hash / 2^32 * 360°). */
export default function HashRing({ nodes }) {
  const [hover, setHover] = useState(null);

  /* Take the ring from any alive node (they all agree) */
  const ring = useMemo(() => {
    const alive = Object.values(nodes).find(
      (n) => n.connected && n.status?.hash_ring?.vnodes?.length
    );
    return alive?.status?.hash_ring?.vnodes ?? [];
  }, [nodes]);

  const deadIds = useMemo(() => {
    const dead = new Set();
    for (const [id, n] of Object.entries(nodes))
      if (!n.connected) dead.add(Number(id));
    return dead;
  }, [nodes]);

  return (
    <div className="card">
      <h3>Consistent hash ring</h3>
      <svg
        viewBox={`0 0 ${SIZE} ${SIZE}`}
        style={{ width: '100%', maxHeight: 240, display: 'block' }}
      >
        <circle
          cx={CX}
          cy={CY}
          r={R}
          fill="none"
          stroke="rgba(255,255,255,0.08)"
          strokeWidth="14"
        />
        {ring.map((v, i) => {
          const angle = (v.hash / 4294967296) * Math.PI * 2 - Math.PI / 2;
          const x = CX + R * Math.cos(angle);
          const y = CY + R * Math.sin(angle);
          const dead = deadIds.has(v.node_id);
          const color = NODE_COLORS[(v.node_id - 1) % NODE_COLORS.length];
          return (
            <circle
              key={i}
              cx={x}
              cy={y}
              r={hover === i ? 5 : 3}
              fill={dead ? 'rgba(120,120,130,0.45)' : color}
              style={{ transition: 'r 0.15s, fill 0.5s', cursor: 'pointer' }}
              onMouseEnter={() => setHover(i)}
              onMouseLeave={() => setHover(null)}
            />
          );
        })}
        <text
          x={CX}
          y={CY - 6}
          textAnchor="middle"
          fill={COLORS.text}
          fontSize="20"
          fontWeight="700"
          fontFamily="JetBrains Mono, monospace"
        >
          {ring.length}
        </text>
        <text
          x={CX}
          y={CY + 14}
          textAnchor="middle"
          fill={COLORS.textDim}
          fontSize="10"
          style={{ textTransform: 'uppercase', letterSpacing: '0.08em' }}
        >
          vnodes shown
        </text>
      </svg>
      <div className="ring-tooltip">
        {hover !== null && ring[hover]
          ? `hash ${ring[hover].hash.toString(16).padStart(8, '0')} → node ${ring[hover].node_id}`
          : ' '}
      </div>
      <div className="legend">
        {[1, 2, 3].map((id) => (
          <span key={id}>
            <span
              className="dot"
              style={{
                background: deadIds.has(id)
                  ? 'rgba(120,120,130,0.45)'
                  : NODE_COLORS[(id - 1) % NODE_COLORS.length],
              }}
            />
            node {id}
          </span>
        ))}
      </div>
    </div>
  );
}
