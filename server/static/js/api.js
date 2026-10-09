import { useState, useEffect } from './lib.js';

export async function api(path, body) {
  const o = { headers: { 'X-Req': '1' } };
  if (body !== undefined) {
    o.method = 'POST';
    o.headers['Content-Type'] = 'application/x-www-form-urlencoded';
    o.body = new URLSearchParams(body).toString();
  }
  const r = await fetch(path, o);
  let j = {};
  try { j = await r.json(); } catch (e) { /* not JSON */ }
  if (!r.ok || j.ok === 0) throw new Error(j.msg || ('HTTP ' + r.status));
  return j;
}

/* ---- toasts ---- */
const listeners = new Set();
export function toast(msg, kind) {
  const t = { id: Math.random(), msg, kind: kind || '' };
  listeners.forEach(f => f(t));
}
export function useToasts() {
  const [list, setList] = useState([]);
  useEffect(() => {
    const f = t => {
      setList(l => [...l, t]);
      setTimeout(() => setList(l => l.filter(x => x.id !== t.id)), 3800);
    };
    listeners.add(f);
    return () => listeners.delete(f);
  }, []);
  return list;
}

/* run a POST, show the result as a toast */
export async function act(path, body, onDone) {
  try {
    const j = await api(path, body || {});
    toast(j.msg || 'Done', 'ok');
    if (onDone) onDone();
    return j;
  } catch (e) {
    toast(e.message, 'err');
    return null;
  }
}

/* ---- formatting ---- */
export const esc = s => String(s == null ? '' : s);
export const fmtUp = ms => {
  let s = Math.floor(ms / 1000); const h = Math.floor(s / 3600), m = Math.floor(s % 3600 / 60); s %= 60;
  return (h ? h + 'h ' : '') + (h || m ? m + 'm ' : '') + s + 's';
};
export const fmtTime = (t, ms) => t ? new Date(t * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }) : 'T+' + fmtUp(ms || 0);
export const fmtDT = t => t ? new Date(t * 1000).toLocaleString([], { day: '2-digit', month: 'short', hour: '2-digit', minute: '2-digit' }) : '-';
export const maskPhone = p => p.slice(0, 2) + '******' + p.slice(-2);
