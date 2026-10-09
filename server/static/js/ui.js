import { html, useEffect, useMemo } from './lib.js';
import { fmtTime, useToasts } from './api.js';

export const CAND_COLORS = ['var(--c0)', 'var(--c1)', 'var(--c2)', 'var(--c3)'];

export const Card = ({ title, right, children, cls }) => html`
  <section class=${'card ' + (cls || '')}>
    ${title ? html`<h2><span>${title}</span>${right ? html`<span class="row">${right}</span>` : null}</h2>` : null}
    ${children}
  </section>`;

export const Stat = ({ value, label, sub, children }) => html`
  <div class="stat">${children}<div><div class="v">${value}${sub ? html`<span class="mut" style=${{ fontSize: '1rem' }}>${sub}</span>` : null}</div><div class="l">${label}</div></div></div>`;

export const Ring = ({ pct }) => html`<div class="ring" style=${{ '--p': pct }} role="img" aria-label=${pct + ' percent'}><span>${pct}%</span></div>`;
export const Badge = ({ kind, children }) => html`<span class=${'badge ' + (kind || '')}>${children}</span>`;
export const Banner = ({ kind, children }) => html`<div class=${'banner ' + (kind || '')} role=${kind === 'err' ? 'alert' : 'status'}>${children}</div>`;

export function Modal({ onClose, children, wide }) {
  useEffect(() => {
    const f = e => { if (e.key === 'Escape' && onClose) onClose(); };
    document.addEventListener('keydown', f);
    return () => document.removeEventListener('keydown', f);
  }, [onClose]);
  return html`<div class="ov" onClick=${e => { if (e.target === e.currentTarget && onClose) onClose(); }}>
    <div class="modal" role="dialog" aria-modal="true" style=${wide ? { maxWidth: '640px' } : null}>${children}</div></div>`;
}

export function Toasts() {
  const list = useToasts();
  return html`<div class="toasts" aria-live="polite">${list.map(t => html`<div key=${t.id} class=${'toast ' + t.kind}>${t.msg}</div>`)}</div>`;
}

export const EventRow = ({ e }) => html`
  <div class=${'ev ' + ['', 'ok', 'warn', 'err'][e.y]}><time>${fmtTime(e.t, e.ms)}</time><b>${e.m}</b></div>`;

export const SmsBadge = ({ s }) => html`<${Badge} kind=${({ sent: 'ok', failed: 'err', retry: 'warn', queued: 'info' })[s] || ''}>${s}<//>`;

/* ---- candidate result bars (rank + percent are written out, so colour is never the only cue) ---- */
export function CandidateBars({ cand, votes }) {
  const tot = votes.reduce((a, b) => a + b, 0), mx = Math.max(...votes);
  return html`<div>${cand.map((n, i) => {
    const v = votes[i], p = tot ? Math.round(v * 100 / tot) : 0;
    return html`<div class="cand" key=${i}>
      <div class="t"><span><span class="key" style=${{ background: CAND_COLORS[i] }}></span>${tot && v === mx ? '\u{1F451} ' : ''}${n}</span>
        <span>${v} <span class="mut sm">(${p}%)</span></span></div>
      <div class="bar" role="img" aria-label=${n + ': ' + v + ' votes, ' + p + ' percent'}><i style=${{ width: p + '%', background: CAND_COLORS[i] }}></i></div></div>`;
  })}</div>`;
}

/* ---- donut chart ---- */
export function Donut({ cand, votes }) {
  const tot = votes.reduce((a, b) => a + b, 0);
  const R = 70, C = 2 * Math.PI * R;
  let off = 0;
  return html`<div>
    <svg class="chart" viewBox="0 0 200 200" style=${{ maxWidth: '260px', margin: '0 auto' }} role="img" aria-label="Share of votes per candidate">
      <circle cx="100" cy="100" r=${R} fill="none" stroke="var(--line)" strokeWidth="26"/>
      ${tot ? votes.map((v, i) => {
        const len = C * v / tot, el = html`<circle key=${i} cx="100" cy="100" r=${R} fill="none" stroke=${CAND_COLORS[i]} strokeWidth="26"
          strokeDasharray=${len + ' ' + (C - len)} strokeDashoffset=${-off} transform="rotate(-90 100 100)"/>`;
        off += len; return el;
      }) : null}
      <text x="100" y="96" textAnchor="middle" style=${{ fontSize: '28px', fontWeight: 800, fill: 'var(--ink)' }}>${tot}</text>
      <text x="100" y="116" textAnchor="middle">votes</text>
    </svg>
    <div class="legend">${cand.map((n, i) => html`<span key=${i}><span class="key" style=${{ background: CAND_COLORS[i] }}></span>${n}</span>`)}</div></div>`;
}

/* ---- votes over time (totals only: the history never records a candidate) ---- */
export function Timeline({ history, reg }) {
  const W = 900, H = 240, L = 34, B = 24, T = 10, Rr = 10;
  const pts = useMemo(() => {
    if (!history.length) return null;
    const t0 = history[0][0], t1 = Math.max(history[history.length - 1][0], t0 + 60);
    const top = Math.max(reg || 0, history[history.length - 1][1], 1);
    const x = t => L + (W - L - Rr) * (t - t0) / (t1 - t0), y = v => H - B - (H - B - T) * v / top;
    let d = 'M' + x(t0) + ' ' + y(0);
    let last = 0;
    history.forEach(([t, v]) => { d += ' L' + x(t) + ' ' + y(last) + ' L' + x(t) + ' ' + y(v); last = v; });
    d += ' L' + x(t1) + ' ' + y(last);
    return { d, t0, t1, top, area: d + ' L' + x(t1) + ' ' + y(0) + ' Z' };
  }, [history, reg]);
  if (!pts) return html`<p class="mut">No votes yet. The line appears when the first vote is cast.</p>`;
  const hhmm = t => new Date(t * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  return html`<svg class="chart" viewBox=${'0 0 ' + W + ' ' + H} role="img" aria-label="Cumulative votes over time">
    <line x1=${L} y1=${H - B} x2=${W - Rr} y2=${H - B} stroke="var(--line)"/>
    <line x1=${L} y1=${T} x2=${L} y2=${H - B} stroke="var(--line)"/>
    <text x=${L - 6} y=${T + 8} textAnchor="end">${pts.top}</text><text x=${L - 6} y=${H - B} textAnchor="end">0</text>
    <text x=${L} y=${H - 6}>${hhmm(pts.t0)}</text><text x=${W - Rr} y=${H - 6} textAnchor="end">${hhmm(pts.t1)}</text>
    <path d=${pts.area} fill="var(--acc)" opacity=".12"/>
    <path d=${pts.d} fill="none" stroke="var(--acc)" strokeWidth="2.2" strokeLinejoin="round"/></svg>`;
}
