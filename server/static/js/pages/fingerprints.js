import { html, useState } from '../lib.js';
import { Card, Badge, Banner } from '../ui.js';
import { act, maskPhone } from '../api.js';

const INFO = {
  O: 'Voter registered and fingerprint present.',
  M: 'Voter is registered but the sensor has no template - re-scan the finger.',
  X: 'Template on the sensor but no voter record (orphan). Delete it or register a voter on this ID.',
  U: 'Voter registered, template not verified yet. Run "Verify templates".',
  '.': 'Free slot.',
};

export function Fingerprints({ S, L, openRegister }) {
  const [sel, setSel] = useState(0);
  const fp = L.fp, slots = fp ? fp.slots : '.'.repeat(S.maxid);
  const v = L.voters.find(x => x.id === sel), code = sel ? slots[sel - 1] : '.';
  return html`<div>
    <div class="grid2">
      <${Card} title="Sensor">
        ${fp ? html`<div class="kv"><span>Sensor</span><span><${Badge} kind=${fp.ok ? 'ok' : 'err'}>${fp.ok ? 'online' : 'offline'}<//></span>
          <span>Templates stored</span><span>${fp.cnt} / ${fp.cap}</span><span>Security level</span><span>${fp.sec} (1 loose - 5 strict)</span>
          <span>Missing templates</span><span>${fp.miss}</span><span>Orphan templates</span><span>${fp.orph}</span></div>` : html`<div class="mut">Loading...</div>`}
        <div class="row" style=${{ marginTop: '12px' }}>
          <button class="btn sm" onClick=${() => act('/api/fp/verify')}>Verify templates</button>
          <button class="btn sm ok" onClick=${() => act('/api/fp/identify')}>Test a finger</button>
          <button class="btn sm err" onClick=${() => confirm('Delete ALL fingerprint templates from the sensor? Voters stay in the database but cannot authenticate until re-scanned.') && act('/api/fp/clear')}>Clear sensor</button></div>
        <div style=${{ marginTop: '12px' }}>
          ${S.ver.run ? html`<div><${Banner} kind="info">Verifying slots ${S.ver.pos}/${S.ver.max}<//><div class="bar"><i style=${{ width: Math.round(S.ver.pos * 100 / S.ver.max) + '%', background: 'var(--acc)' }}></i></div></div>` : null}
          ${!S.ver.run && S.idn.msg ? html`<${Banner} kind="info">${S.idn.msg}<//>` : null}</div>
      <//>
      <${Card} title="Slot details">
        ${!sel ? html`<div class="mut">Select a slot below.</div>` : html`<div>
          <div class="big"><b>Slot ${sel}</b></div><p class="mut" style=${{ margin: '6px 0' }}>${INFO[code] || ''}</p>
          ${v ? html`<div><div class="kv"><span>Voter</span><span>${v.n}</span><span>Phone</span><span class="mono">${maskPhone(v.p)}</span><span>Voted</span><span>${v.v ? 'yes' : 'no'}</span></div>
              <div class="row" style=${{ marginTop: '10px' }}><button class="btn sm" onClick=${() => act('/api/enroll', { id: sel, rescan: 1 })}>Re-scan finger</button></div></div>`
            : code === 'X' ? html`<div class="row"><button class="btn sm err" onClick=${() => confirm('Delete template ' + sel + '?') && act('/api/fp/delete', { id: sel })}>Delete orphan template</button></div>`
            : html`<div class="row"><button class="btn sm ok" onClick=${() => openRegister(sel)}>Register voter here</button></div>`}</div>`}
      <//>
    </div>
    <${Card} title=${'Template map (IDs 1-' + S.maxid + ')'}>
      <div class="slots">${[...slots].map((c, i) => html`<button key=${i} class=${'slot ' + (c === '.' ? '' : c) + (sel === i + 1 ? ' sel' : '')} onClick=${() => setSel(i + 1)}
        title=${L.voters.find(x => x.id === i + 1) ? L.voters.find(x => x.id === i + 1).n : 'free'} aria-label=${'Slot ' + (i + 1) + ', ' + (INFO[c] || '')}>${i + 1}</button>`)}</div>
      <div class="leg"><span><${Badge} kind="ok">OK<//> voter + template</span><span><${Badge} kind="err">M<//> template missing</span><span><${Badge} kind="warn">X<//> orphan template</span><span><${Badge} kind="info">U<//> not verified</span><span><${Badge}>.<//> free</span></div>
    <//>
  </div>`;
}
