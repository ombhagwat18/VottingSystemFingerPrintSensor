import { html } from '../lib.js';
import { Card, Stat, Badge, SmsBadge } from '../ui.js';
import { act, fmtTime } from '../api.js';

export function Sms({ S, L }) {
  return html`<div>
    <div class="grid">
      <${Stat} value=${S.sms.pend} label="Queued / retrying"/><${Stat} value=${S.sms.sent} label="Sent (since start)"/>
      <${Stat} value=${S.sms.fail} label="Failed (since start)"/><${Stat} value=${L.sms.length} label="In log"/></div>
    ${!S.sms.key ? html`<div class="alertbar" role="alert">No CircuitDigest API key is configured, so messages are logged as "skipped". Add it to server/config.json (or run setup.bat again).</div>` : null}
    <${Card} title="Message log" right=${html`<button class="btn sm" onClick=${() => act('/api/sms/test')}>Send test SMS</button><button class="btn sm ghost" onClick=${() => act('/api/sms/clear')}>Clear finished</button>`}>
      <div class="tw"><table><thead><tr><th>Time</th><th>Type</th><th>To</th><th>Tpl</th><th>Message variables</th><th>Status</th><th></th></tr></thead>
        <tbody>${L.sms.length ? L.sms.map(m => html`<tr key=${m.i}><td>${fmtTime(m.t)}</td><td><${Badge}>${m.k}<//></td><td class="mono">${m.to}</td><td>${m.tp}</td>
          <td>${m.v1} <span class="mut">/</span> ${m.v2}</td>
          <td><${SmsBadge} s=${m.s}/>${m.a ? html`<div class="mut sm">try ${m.a}${m.h ? ' · HTTP ' + m.h : ''}</div>` : null}</td>
          <td>${m.s === 'failed' || m.s === 'skipped' ? html`<button class="btn sm ghost" onClick=${() => act('/api/sms/resend', { id: m.i })}>Resend</button>` : null}</td></tr>`)
          : html`<tr><td colSpan="7" class="mut">No messages yet.</td></tr>`}</tbody></table></div>
      <div class="mut sm" style=${{ marginTop: '10px' }}>Last API response: <span class="mono">${S.sms.last || '-'}</span></div>
      <details style=${{ marginTop: '12px' }}><summary class="sm" style=${{ cursor: 'pointer' }}>Why does a voter's SMS fail?</summary>
        <p class="sm mut" style=${{ marginTop: '6px' }}>The CircuitDigest free plan sends only to <b>India</b> numbers that are <b>linked</b> to your account (maximum 5) and allows 100 SMS per month. The admin number works because it is linked; a voter number that is not linked is refused (HTTP 400/403). HTTP 401 means the API key is wrong.</p></details>
    <//>
  </div>`;
}
