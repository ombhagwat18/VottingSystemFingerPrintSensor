import { html } from '../lib.js';
import { Card, Stat, Ring, Banner, CandidateBars, EventRow, SmsBadge } from '../ui.js';
import { fmtTime, toast } from '../api.js';

export function stationBanner(S) {
  const st = S.st;
  if (!S.bridge.ok) return ['err', 'Voting hardware (NodeMCU) is not connected. ' + (S.bridge.msg || 'Check the USB cable.')];
  if (!S.sensor.ok) return ['err', 'Fingerprint sensor offline - check wiring (5V, GND, D1/D2) and watch the Activity tab'];
  if (['wait1', 'remove', 'wait2', 'wait3'].includes(S.en.s)) return ['info', 'Enrollment in progress...'];
  if (S.ver.run) return ['info', 'Verifying sensor templates (' + S.ver.pos + '/' + S.ver.max + ')'];
  if (S.idn.on) return ['info', 'Test mode: place any finger on the sensor'];
  if (st.s === 'auth') return ['ok', html`<span class="big">${st.name}</span> (ID ${st.id})<br/>Authenticated - press candidate button 1-4. ${st.left}s left`];
  if (st.s === 'scan') return ['ok', 'Ready - waiting for a voter to place a finger'];
  return ['warn', 'Election closed - sensor idle. Open the election to start voting.'];
}

export function Station({ S }) {
  const [kind, body] = stationBanner(S);
  return html`<div>
    <${Banner} kind=${kind}>${body}<//>
    ${S.st.msg ? html`<${Banner} kind=${S.st.mt}>${S.st.msg}<//>` : null}
    ${S.idn.msg ? html`<${Banner} kind="info">${S.idn.msg}<//>` : null}</div>`;
}

export function Dashboard({ S, L, go, openRegister }) {
  const tot = S.votes.reduce((a, b) => a + b, 0), pct = S.reg ? Math.round(S.voted * 100 / S.reg) : 0;
  const mx = Math.max(...S.votes);
  const lead = S.votes.filter(v => v === mx).length === 1 && mx > 0 ? S.cand[S.votes.indexOf(mx)] : '';
  const url = 'http://' + S.host + ':' + S.port;
  return html`<div>
    <div class="grid">
      <${Stat} value=${S.voted} sub=${'/' + S.reg} label="Voted / registered"><${Ring} pct=${pct}/><//>
      <${Stat} value=${S.reg - S.voted} label="Still to vote"/>
      <${Stat} value=${S.sensor.cnt} label="Templates on sensor"/>
      <${Stat} value=${S.sms.pend} label=${'SMS pending (' + S.sms.sent + ' sent, ' + S.sms.fail + ' failed)'}/>
    </div>
    <div class="grid2">
      <${Card} title="Live results" right=${html`<span class="mut sm">${lead ? 'Leading: ' + lead : (tot ? 'Tie' : 'No votes yet')}</span>`}>
        <${CandidateBars} cand=${S.cand} votes=${S.votes}/>
        <a href="#results" onClick=${e => { e.preventDefault(); go('results'); }} class="sm">Charts and export \u2192</a>
      <//>
      <${Card} title="Voting station">
        <${Station} S=${S}/>
        <div class="row" style=${{ marginTop: '8px' }}>
          <button class="btn sm ghost" onClick=${openRegister}>+ Register voter</button>
          <button class="btn sm ghost" onClick=${() => go('fp')}>Manage fingerprints</button></div>
      <//>
    </div>
    <div class="grid2">
      <${Card} title="Recent activity" right=${html`<a href="#log" class="sm" onClick=${e => { e.preventDefault(); go('log'); }}>view all</a>`}>
        ${L.log.length ? L.log.slice(0, 7).map(e => html`<${EventRow} key=${e.i} e=${e}/>`) : html`<div class="mut">No activity yet</div>`}
      <//>
      <${Card} title="Recent SMS" right=${html`<a href="#sms" class="sm" onClick=${e => { e.preventDefault(); go('sms'); }}>view all</a>`}>
        ${L.sms.length ? L.sms.slice(0, 5).map(m => html`<div class="ev" key=${m.i}><time>${fmtTime(m.t)}</time>
          <span><${SmsBadge} s=${m.s}/> <b>${m.k}</b> ${m.v1} / ${m.v2}</span></div>`) : html`<div class="mut">No messages yet</div>`}
      <//>
    </div>
    <${Card} title="Open this dashboard from another device">
      <p class="mut sm">Any phone or laptop on the same WiFi can open the address below and sign in. The NodeMCU does not need WiFi at all.</p>
      <div class="row" style=${{ marginTop: '8px' }}><code class="mono big">${url}</code>
        <button class="btn sm ghost" onClick=${() => { navigator.clipboard && navigator.clipboard.writeText(url).then(() => toast('Address copied', 'ok'), () => toast(url)); }}>Copy</button></div>
    <//>
  </div>`;
}
