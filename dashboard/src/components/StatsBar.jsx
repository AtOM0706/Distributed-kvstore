import React, { useEffect, useRef, useState } from 'react';
import { COLORS } from '../utils/constants';

/* Animated counter: eases toward the target value */
function useAnimatedNumber(target) {
  const [display, setDisplay] = useState(target);
  const raf = useRef();

  useEffect(() => {
    const start = display;
    const delta = target - start;
    if (delta === 0) return undefined;
    const t0 = performance.now();
    const D = 400;
    const step = (t) => {
      const k = Math.min(1, (t - t0) / D);
      setDisplay(Math.round(start + delta * (1 - Math.pow(1 - k, 3))));
      if (k < 1) raf.current = requestAnimationFrame(step);
    };
    raf.current = requestAnimationFrame(step);
    return () => cancelAnimationFrame(raf.current);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [target]);

  return display;
}

function Tile({ value, label, color, suffix = '' }) {
  const animated = useAnimatedNumber(value);
  return (
    <div className="card stat-tile">
      <div className="big" style={{ color }}>
        {animated.toLocaleString()}
        {suffix}
      </div>
      <div className="small">{label}</div>
    </div>
  );
}

export default function StatsBar({ nodes }) {
  const alive = Object.values(nodes).filter((n) => n.connected);
  const leader = alive.find((n) => n.status?.role === 'leader');

  const totalKeys = leader?.status?.metrics?.total_keys ?? 0;
  const writes = leader?.status?.metrics?.writes_per_sec ?? 0;
  const reads = leader?.status?.metrics?.reads_per_sec ?? 0;
  const lat = leader?.status?.metrics?.avg_latency_us ?? 0;

  return (
    <div className="stats-bar">
      <Tile value={alive.length} label="Nodes up" color={COLORS.green} />
      <Tile value={totalKeys} label="Total keys" color={COLORS.cyan} />
      <Tile value={writes} label="Writes / sec" color={COLORS.cyan} />
      <Tile value={reads} label="Reads / sec" color={COLORS.green} />
      <Tile value={lat} label="Avg latency (µs)" color={COLORS.amber} />
    </div>
  );
}
