export const NODES = [
  { id: 1, wsUrl: 'ws://127.0.0.1:8001', clientPort: 6001 },
  { id: 2, wsUrl: 'ws://127.0.0.1:8002', clientPort: 6002 },
  { id: 3, wsUrl: 'ws://127.0.0.1:8003', clientPort: 6003 },
];

export const COLORS = {
  bg: '#0a0e1a',
  card: 'rgba(255, 255, 255, 0.04)',
  border: 'rgba(255, 255, 255, 0.08)',
  cyan: '#00d4ff',
  green: '#00ff88',
  amber: '#ffaa00',
  red: '#ff4444',
  text: '#e8ecf4',
  textDim: '#8b93a7',
};

export const NODE_COLORS = ['#00d4ff', '#00ff88', '#ffaa00'];

/* A node is considered dead if no WS message arrives within this window */
export const DEAD_AFTER_MS = 3500;
