import { html, useState, useEffect } from '../lib.js';
import { Card, Badge } from '../ui.js';
import { act, fmtUp } from '../api.js';

export function Settings({ S, L, refresh }) {
  const s = L.set;
  const [seeC, setSeeC] = useState(false);
  useEffect(() => { if (s) setSeeC(!!s.seeC); }, [s]);
  const submit = async e => {
    e.preventDefault();
    const f = new FormData(e.target), o = {};
    f.forEach((v, k) => { o[k] = v; });
    ['sms', 'smsV', 'smsR', 'smsA', 'seeC'].forEach(k => { o[k] = f.has(k) ? 1 : 0; });
    if (await act('/api/settings', o)) refresh();
  };
  const ck = (n, l, v, on) => html`<label class="chk"><input type="checkbox" name=${n} defaultChecked=${!!v} onChange=${on}/> <span>${l}</span></label>`;
  return html`<div>
    <${Card} title="Settings">
      ${!s ? html`<div class="mut">Loading...</div>` : html`<form key=${JSON.stringify(s)} onSubmit=${submit}>
        <div class="grid2">
          <div><h3>Candidates (buttons 1-4)</h3>${s.cand.map((c, i) => html`<div key=${i}><label for=${'c' + i}>Button ${i + 1}</label><input id=${'c' + i} name=${'c' + i} maxLength="20" defaultValue=${c} required/></div>`)}</div>
          <div><h3>Voting</h3>
            <label for="vt">Vote timeout after authentication (10-120 s)</label><input id="vt" name="vt" type="number" min="10" max="120" defaultValue=${s.vt}/>
            <label for="mc">Minimum match confidence (10-200)</label><input id="mc" name="mc" type="number" min="10" max="200" defaultValue=${s.mc}/>
            <label for="sec">Sensor security level (1-5)</label><input id="sec" name="sec" type="number" min="1" max="5" defaultValue=${s.sec}/></div>
          <div><h3>SMS notifications</h3>
            ${ck('sms', 'Enable SMS sending', s.sms)}${ck('smsV', 'SMS the voter after voting', s.smsV)}${ck('smsR', 'SMS the voter after registration', s.smsR)}${ck('smsA', 'SMS the admin on every vote', s.smsA)}
            ${ck('seeC', 'Include the candidate in the admin SMS', s.seeC, e => setSeeC(e.target.checked))}
            ${seeC ? html`<div class="banner warn sm" role="alert">This breaks ballot secrecy: the admin would learn how each voter voted. Leave it off unless the election rules allow it.</div>` : null}
            <label for="admin">Admin phone (10 digits)</label><input id="admin" name="admin" defaultValue=${s.admin} maxLength="12"/>
            <div class="hint">Must be linked in your CircuitDigest account.</div></div>
        </div>
        <div class="row" style=${{ marginTop: '16px' }}><button class="btn ok" type="submit">Save settings</button></div></form>`}
    <//>
    <div class="grid2">
      <${Card} title="System">
        <div class="kv">
          <span>Dashboard address</span><span class="mono">http://${S.host}:${S.port}</span>
          <span>Server uptime</span><span>${fmtUp(S.up)}</span>
          <span>Voting hardware</span><span><${Badge} kind=${S.bridge.ok ? 'ok' : 'err'}>${S.bridge.ok ? 'connected' : 'offline'}<//> ${S.bridge.port || ''} ${S.bridge.fw ? 'fw v' + S.bridge.fw : ''}</span>
          <span>Link status</span><span>${S.bridge.msg}</span>
          <span>Fingerprint sensor</span><span><${Badge} kind=${S.sensor.ok ? 'ok' : 'err'}>${S.sensor.ok ? 'online' : 'offline'}<//></span>
          <span>SMS API key</span><span>${S.sms.key ? 'configured' : 'missing'}</span>
          <span>Booth</span><span>${S.booth}</span></div>
        <div class="row" style=${{ marginTop: '14px' }}><button class="btn sm warn" onClick=${() => confirm('Restart the voting hardware (NodeMCU)? Voter data stays on this PC.') && act('/api/reboot')}>Restart hardware</button></div>
      <//>
      <${Card} title="Danger zone">
        <p class="mut sm" style=${{ marginBottom: '12px' }}>Reset clears every tally and lets all voters vote again. Erase removes all voters, votes and fingerprint templates. Export the voter list first.</p>
        <div class="row"><button class="btn sm warn" onClick=${() => confirm('Reset ALL votes to zero and mark every voter as not voted?') && act('/api/votes/reset')}>Reset votes</button>
          <button class="btn sm err" onClick=${() => { const t = prompt('This deletes ALL voters, votes and sensor templates.\nType ERASE to confirm.'); if (t === 'ERASE') act('/api/factory', { confirm: 'ERASE' }); }}>Erase all data</button></div>
      <//>
    </div>
  </div>`;
}
