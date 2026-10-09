import { html, useState, useMemo } from '../lib.js';
import { Card, Badge, Modal } from '../ui.js';
import { act, fmtDT, maskPhone } from '../api.js';

const FP = { O: ['ok', 'OK'], M: ['err', 'Missing'], U: ['info', 'Unverified'], X: ['warn', 'Orphan'] };

export function RegisterModal({ S, L, onClose, presetId }) {
  const used = new Set(L.voters.map(v => v.id));
  let id = presetId || 1;
  if (!presetId) while (used.has(id) && id < S.maxid) id++;
  const [busy, setBusy] = useState(false);
  const submit = async e => {
    e.preventDefault();
    setBusy(true);
    const r = await act('/api/enroll', Object.fromEntries(new FormData(e.target)));
    setBusy(false);
    if (r) onClose();
  };
  return html`<${Modal} onClose=${onClose}>
    <h2>Register voter</h2>
    <div class="mut sm">After you submit, the sensor asks the voter to scan the same finger twice.</div>
    <form onSubmit=${submit}>
      <label for="r-id">Voter ID (1-${S.maxid}, also the sensor slot)</label><input id="r-id" name="id" type="number" min="1" max=${S.maxid} defaultValue=${id} required/>
      <label for="r-name">Full name</label><input id="r-name" name="name" maxLength="39" autoComplete="off" required/>
      <label for="r-age">Age (18+)</label><input id="r-age" name="age" type="number" min="18" max="120" required/>
      <label for="r-ph">Mobile (10 digits)</label><input id="r-ph" name="phone" inputMode="numeric" pattern="[0-9]{10}" maxLength="10" required/>
      <label for="r-ad">Address</label><input id="r-ad" name="address" maxLength="79" autoComplete="off" required/>
      <label class="chk" style=${{ marginTop: '14px' }}><input type="checkbox" required/> <span>The voter agrees to be registered, to keep a fingerprint template on the sensor, and to receive SMS messages about their registration and vote. The mobile number is used for nothing else.</span></label>
      <div class="row" style=${{ marginTop: '16px' }}><button class="btn ok" type="submit" disabled=${busy}>Start enrollment</button><button class="btn ghost" type="button" onClick=${onClose}>Cancel</button></div>
    </form><//>`;
}

function EditModal({ v, onClose }) {
  const submit = async e => {
    e.preventDefault();
    const r = await act('/api/voter/update', Object.fromEntries(new FormData(e.target)));
    if (r) onClose();
  };
  return html`<${Modal} onClose=${onClose}>
    <h2>Edit voter #${v.id}</h2>
    <form onSubmit=${submit}><input type="hidden" name="id" value=${v.id}/>
      <label for="e-name">Full name</label><input id="e-name" name="name" maxLength="39" defaultValue=${v.n} required/>
      <label for="e-age">Age</label><input id="e-age" name="age" type="number" min="18" max="120" defaultValue=${v.a} required/>
      <label for="e-ph">Mobile (10 digits)</label><input id="e-ph" name="phone" pattern="[0-9]{10}" maxLength="10" defaultValue=${v.p} required/>
      <label for="e-ad">Address</label><input id="e-ad" name="address" maxLength="79" defaultValue=${v.ad} required/>
      <div class="row" style=${{ marginTop: '16px' }}><button class="btn ok" type="submit">Save</button><button class="btn ghost" type="button" onClick=${onClose}>Cancel</button></div>
    </form><//>`;
}

export function Voters({ S, L, openRegister }) {
  const [q, setQ] = useState(''), [f, setF] = useState('all'), [mask, setMask] = useState(true), [edit, setEdit] = useState(null);
  const rows = useMemo(() => L.voters.filter(v => {
    if (f === 'voted' && !v.v) return false;
    if (f === 'pending' && v.v) return false;
    if (f === 'nofp' && (v.fp === 'O' || v.fp === 'U')) return false;
    return !q || (v.id + ' ' + v.n + ' ' + v.p + ' ' + v.ad).toLowerCase().includes(q.toLowerCase());
  }), [L.voters, q, f]);
  const onAct = (a, v) => {
    if (a === 'edit') setEdit(v);
    else if (a === 'rescan') { if (confirm('Replace the fingerprint of ' + v.n + '? The voter will be asked to scan twice.')) act('/api/enroll', { id: v.id, rescan: 1 }); }
    else if (confirm('Delete ' + v.n + ' (ID ' + v.id + ') and their fingerprint?')) act('/api/voter/delete', { id: v.id });
  };
  return html`<${Card} title="Voter database" right=${html`<span class="mut sm">${L.voters.length} registered, ${L.voters.filter(v => v.v).length} voted</span>`}>
    <div class="tb">
      <input type="search" aria-label="Search voters" placeholder="Search name, ID, phone, address" value=${q} onInput=${e => setQ(e.target.value)}/>
      <select aria-label="Filter" value=${f} onChange=${e => setF(e.target.value)}><option value="all">All voters</option><option value="voted">Voted</option><option value="pending">Not voted</option><option value="nofp">Fingerprint problem</option></select>
      <label class="chk" style=${{ margin: 0 }}><input type="checkbox" checked=${mask} onChange=${e => setMask(e.target.checked)}/> Mask phones</label>
      <span class="grow"></span>
      <button class="btn sm ok" onClick=${openRegister}>+ Register voter</button>
      <a class="btn sm ghost" href="/api/export.csv" download="voters.csv">Export CSV</a></div>
    <div class="tw"><table><thead><tr><th>ID</th><th>Name</th><th>Age</th><th>Phone</th><th>Address</th><th>Finger</th><th>Status</th><th></th></tr></thead>
      <tbody>${rows.length ? rows.map(v => {
        const b = FP[v.fp] || ['', '?'];
        return html`<tr key=${v.id}><td>${v.id}</td><td><b>${v.n}</b></td><td>${v.a}</td><td class="mono">${mask ? maskPhone(v.p) : v.p}</td><td>${v.ad}</td>
          <td><${Badge} kind=${b[0]}>${b[1]}<//></td>
          <td>${v.v ? html`<${Badge} kind="ok">Voted<//><div class="mut sm">${fmtDT(v.vt)}</div>` : html`<${Badge} kind="warn">Not voted<//>`}</td>
          <td style=${{ whiteSpace: 'nowrap' }}><button class="btn sm ghost" onClick=${() => onAct('edit', v)}>Edit</button> <button class="btn sm ghost" onClick=${() => onAct('rescan', v)}>Re-scan</button> <button class="btn sm err" onClick=${() => onAct('del', v)}>Delete</button></td></tr>`;
      }) : html`<tr><td colSpan="8" class="mut">No voters match.</td></tr>`}</tbody></table></div>
    ${edit ? html`<${EditModal} v=${edit} onClose=${() => setEdit(null)}/>` : null}
  <//>`;
}
