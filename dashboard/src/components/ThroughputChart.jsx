import React, { useEffect, useRef, useState } from 'react';
import {
  AreaChart,
  Area,
  XAxis,
  YAxis,
  Tooltip,
  ResponsiveContainer,
  CartesianGrid,
} from 'recharts';
import { COLORS } from '../utils/constants';

const WINDOW_SEC = 60;

export default function ThroughputChart({ nodes }) {
  const [series, setSeries] = useState([]);
  const nodesRef = useRef(nodes);
  nodesRef.current = nodes;

  useEffect(() => {
    const timer = setInterval(() => {
      const alive = Object.values(nodesRef.current).filter((n) => n.connected);
      const leader = alive.find((n) => n.status?.role === 'leader');
      const writes = leader?.status?.metrics?.writes_per_sec ?? 0;
      const reads = leader?.status?.metrics?.reads_per_sec ?? 0;
      const t = new Date();
      const label = `${t.getMinutes()}:${String(t.getSeconds()).padStart(2, '0')}`;
      setSeries((prev) => [...prev, { t: label, writes, reads }].slice(-WINDOW_SEC));
    }, 1000);
    return () => clearInterval(timer);
  }, []);

  return (
    <div className="card">
      <h3>Throughput — rolling 60s</h3>
      <div style={{ width: '100%', height: 230 }}>
        <ResponsiveContainer>
          <AreaChart data={series} margin={{ top: 4, right: 8, bottom: 0, left: -18 }}>
            <defs>
              <linearGradient id="gWrites" x1="0" y1="0" x2="0" y2="1">
                <stop offset="0%" stopColor={COLORS.cyan} stopOpacity={0.35} />
                <stop offset="100%" stopColor={COLORS.cyan} stopOpacity={0} />
              </linearGradient>
              <linearGradient id="gReads" x1="0" y1="0" x2="0" y2="1">
                <stop offset="0%" stopColor={COLORS.green} stopOpacity={0.35} />
                <stop offset="100%" stopColor={COLORS.green} stopOpacity={0} />
              </linearGradient>
            </defs>
            <CartesianGrid stroke="rgba(255,255,255,0.05)" vertical={false} />
            <XAxis
              dataKey="t"
              tick={{ fill: COLORS.textDim, fontSize: 11 }}
              tickLine={false}
              axisLine={false}
              minTickGap={40}
            />
            <YAxis
              tick={{ fill: COLORS.textDim, fontSize: 11 }}
              tickLine={false}
              axisLine={false}
            />
            <Tooltip
              contentStyle={{
                background: '#11162a',
                border: '1px solid rgba(255,255,255,0.1)',
                borderRadius: 10,
                fontSize: 12,
              }}
              labelStyle={{ color: COLORS.textDim }}
            />
            <Area
              type="monotone"
              dataKey="writes"
              stroke={COLORS.cyan}
              strokeWidth={2}
              fill="url(#gWrites)"
              isAnimationActive={false}
              name="writes/s"
            />
            <Area
              type="monotone"
              dataKey="reads"
              stroke={COLORS.green}
              strokeWidth={2}
              fill="url(#gReads)"
              isAnimationActive={false}
              name="reads/s"
            />
          </AreaChart>
        </ResponsiveContainer>
      </div>
    </div>
  );
}
