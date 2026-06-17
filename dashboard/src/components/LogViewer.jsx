import React, { useEffect, useRef, useState } from 'react';

export default function LogViewer({ events }) {
  const boxRef = useRef(null);
  const [paused, setPaused] = useState(false);

  /* Auto-scroll to the bottom unless the user is hovering */
  useEffect(() => {
    if (!paused && boxRef.current)
      boxRef.current.scrollTop = boxRef.current.scrollHeight;
  }, [events, paused]);

  /* Show the wall-clock time the event arrived at the dashboard */
  const fmtTime = (ev) =>
    new Date(ev.receivedAt || Date.now()).toLocaleTimeString('en-GB', {
      hour12: false,
    });

  return (
    <div className="card">
      <h3>
        Raft event log{' '}
        {paused && <span style={{ color: 'var(--amber)' }}>(paused)</span>}
      </h3>
      <div
        className="log-viewer"
        ref={boxRef}
        onMouseEnter={() => setPaused(true)}
        onMouseLeave={() => setPaused(false)}
      >
        {events.length === 0 && (
          <div style={{ color: 'var(--text-dim)' }}>
            Waiting for cluster events…
          </div>
        )}
        {events.map((ev, i) => (
          <div className="log-line" key={i}>
            <span className="log-time">{fmtTime(ev)}</span>
            <span className={`log-tag ${ev.type}`}>{ev.type}</span>
            <span>{ev.msg}</span>
          </div>
        ))}
      </div>
    </div>
  );
}
