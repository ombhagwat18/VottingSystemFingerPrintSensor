import { html, mount, useState, useEffect, useRef, useCallback } from './lib.js';
import { api, act, fmtUp } from './api.js';
import { Banner, Modal, Toasts } from './ui.js';
import { Dashboard } from './pages/dashboard.js';
import { Results } from './pages/results.js';
import { Voters, RegisterModal } from './pages/voters.js';
import { Fingerprints } from './pages/fingerprints.js';
import { Sms } from './pages/sms.js';
import { Activity } from './pages/activity.js';
import { Settings } from './pages/settings.js';

const TABS = [['dash', 'Dashboard'], ['results', 'Results'], ['voters', 'Voters'], ['fp', 'Fingerprints'], ['sms', 'SMS'], ['log', 'Activity'], ['set', 'Settings']];
const EMPTY = { voters: [], sms: [], log: [], fp: null, set: null };

/* ---- enrollment overlay: follows the sensor's steps live ---- */
function EnrollOverlay({ S }) {
  const e = S.en;
  const [dismissed, setDismissed] = useState(0);
  if (e.s === 'idle' || dismissed === e.seq) return null;
  const idx = { wait1: 0, remove: e.ph ? 3 : 1, wait2: 2, wait3: 4, ok: 5, fail: -1 }[e.s];
  const names = ['Place finger', 'Lift finger', 'Place again', 'Lift', 'Verify'];
  const run = idx >= 0 && idx < 5, cls = e.s === 'ok' ? 'ok' : e.s === 'fail' ? 'err' : 'info';
  return html`<${Modal}>
    <h2>${e.resc ? 'Re-scan fingerprint' : 'Fingerprint enrollment'}</h2>
    <div class="mut sm">Voter ID ${e.id}${e.try ? ' · attempt ' + (e.try + 1) + '/3' : ''}</div>
    <div class="steps">${names.map((n, i) => html`<div key=${i} class=${e.s === 'fail' ? (i === 0 ? 'bad' : '') : (i < idx ? 'done' : i === idx ? 'act' : '')}>${i + 1}. ${n}</div>`)}</div>
    <${Banner} kind=${cls}><span class="big">${e.msg}</span><//>
    <div class="row">${run ? html`<button class="btn err" onClick=${() => act('/api/enroll/cancel')}>Cancel</button>` : html`<button class="btn" onClick=${() => setDismissed(e.seq)}>Close</button>`}</div>
  <//>`;
}

function App() {
  const [S, setS] = useState(null);
  const [L, setL] = useState(EMPTY);
  const [R, setR] = useState({ history: [] });
  const [online, setOnline] = useState(false);
  const [tab, setTab] = useState(() => (TABS.find(t => t[0] === location.hash.slice(1)) || TABS[0])[0]);
  const [reg, setReg] = useState(null);                 // register modal: {preset}
  const [theme, setTheme] = useState(() => document.documentElement.dataset.theme || 'light');
  const revs = useRef({}), timer = useRef(0), stateRef = useRef(null);

  const go = useCallback(t => { setTab(t); history.replaceState(null, '', '#' + t); window.scrollTo(0, 0); }, []);
  const toggleTheme = () => {
    const t = theme === 'dark' ? 'light' : 'dark';
    setTheme(t); document.documentElement.dataset.theme = t;
    try { localStorage.setItem('vs-theme', t); } catch (e) { /* private mode */ }
  };

  /* poll the live state; refetch a list only when its revision counter moved */
  const poll = useCallback(async () => {
    clearTimeout(timer.current);
    let busy = false;
    try {
      const d = await api('/api/state');
      stateRef.current = d; setS(d); setOnline(true);
      busy = d.en.s !== 'idle' || d.st.s === 'auth' || d.idn.on || d.ver.run;
      const r = d.rev, jobs = [], rv = revs.current;
      const pull = (key, path, set) => { if (rv[key] !== r[key]) { rv[key] = r[key]; jobs.push(api(path).then(set)); } };
      pull('l', '/api/log', x => setL(l => ({ ...l, log: x })));
      pull('s', '/api/sms', x => setL(l => ({ ...l, sms: x })));
      pull('v', '/api/voters', x => setL(l => ({ ...l, voters: x })));
      pull('f', '/api/fp', x => setL(l => ({ ...l, fp: x })));
      pull('c', '/api/settings', x => setL(l => ({ ...l, set: x })));
      if (rv.t !== d.votes.join() + d.reg) { rv.t = d.votes.join() + d.reg; jobs.push(api('/api/results').then(setR)); }
      await Promise.all(jobs).catch(() => { rv.l = rv.s = rv.v = rv.f = rv.c = undefined; });
    } catch (e) { setOnline(false); }
    timer.current = setTimeout(poll, busy ? 500 : 1500);
  }, []);
  useEffect(() => { poll(); return () => clearTimeout(timer.current); }, [poll]);
  const refresh = useCallback(() => { revs.current = {}; poll(); }, [poll]);

  useEffect(() => { document.title = S ? (S.open ? '● ' : '') + 'Voting Console - ' + S.booth : 'Voting Console'; }, [S && S.open, S && S.booth]);

  if (!S) return html`<div style=${{ padding: '32px', textAlign: 'center' }}>${online ? 'Loading...' : 'Connecting to the voting server...'}<${Toasts}/></div>`;

  const openRegister = preset => setReg({ preset: typeof preset === 'number' ? preset : 0 });
  const page = { dash: Dashboard, results: Results, voters: Voters, fp: Fingerprints, sms: Sms, log: Activity, set: Settings }[tab];
  const props = { S, L, R, go, refresh, openRegister };
  return html`<div>
    <header class="top"><div class="top-in">
      <div class="brand"><div class="logo" aria-hidden="true">\u{1F5F3}️</div><div><b>Fingerprint Voting Console</b>
        <small>${S.booth} · ${online ? (S.ep ? new Date(S.ep * 1000).toLocaleString() : '') + ' · up ' + fmtUp(S.up) : 'connection lost - retrying...'}</small></div></div>
      <div class="hr">
        <span class=${'dot ' + (online ? 'on' : 'off')} role="img" aria-label=${online ? 'Server connected' : 'Server not reachable'} title=${online ? 'Server connected' : 'Server not reachable'}></span>
        <span class=${'pill ' + (S.open ? 'open' : 'closed')}>${S.open ? 'ELECTION OPEN' : 'ELECTION CLOSED'}</span>
        <button class="btn sm ghost" onClick=${() => { if (S.open && !confirm('Close the election? Voters will not be able to vote.')) return; act('/api/election', { open: S.open ? 0 : 1 }, refresh); }}>${S.open ? 'Close election' : 'Open election'}</button>
        <button class="iconbtn" onClick=${toggleTheme} aria-label="Switch light or dark theme" title="Switch theme">${theme === 'dark' ? '☀️' : '\u{1F319}'}</button>
      </div></div>
      <nav class="tabs" aria-label="Sections">${TABS.map(([k, n]) => html`<button key=${k} aria-current=${tab === k ? 'page' : null} onClick=${() => go(k)}>${n}</button>`)}</nav>
    </header>
    <main>
      ${!online ? html`<div class="alertbar" role="alert">Cannot reach the voting server. Check that it is still running on the PC.</div>` : null}
      ${online && !S.bridge.ok ? html`<div class="alertbar" role="alert">Voting hardware is not connected: ${S.bridge.msg}. Votes cannot be taken until the NodeMCU is plugged into this PC.</div>` : null}
      ${html`<${page} ...${props}/>`}
    </main>
    <footer>Ballot secrecy: only totals are stored. · Everything runs on this PC; the NodeMCU only reads the sensor and buttons.</footer>
    <${EnrollOverlay} S=${S}/>
    ${reg ? html`<${RegisterModal} S=${S} L=${L} presetId=${reg.preset} onClose=${() => setReg(null)}/>` : null}
    <${Toasts}/>
  </div>`;
}

mount(document.getElementById('root'), html`<${App}/>`);
