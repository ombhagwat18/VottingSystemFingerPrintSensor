import { html } from '../lib.js';
import { Card, Stat, Ring, CandidateBars, Donut, Timeline } from '../ui.js';

export function Results({ S, R }) {
  const tot = S.votes.reduce((a, b) => a + b, 0), pct = S.reg ? Math.round(S.voted * 100 / S.reg) : 0;
  const mx = Math.max(...S.votes), tied = S.votes.filter(v => v === mx).length;
  const verdict = !tot ? 'No votes have been cast yet.' : tied > 1 ? 'Currently a tie between ' + S.cand.filter((_, i) => S.votes[i] === mx).join(' and ') + '.'
    : S.cand[S.votes.indexOf(mx)] + ' is currently leading.';
  return html`<div>
    <div class="grid">
      <${Stat} value=${tot} label="Votes counted"/>
      <${Stat} value=${pct + '%'} label=${'Turnout (' + S.voted + ' of ' + S.reg + ')'}><${Ring} pct=${pct}/><//>
      <${Stat} value=${S.open ? 'Open' : 'Closed'} label="Election status"/>
    </div>
    <${Card} title="Result" right=${html`<span class="row noprint">
        <a class="btn sm ghost" href="/api/results.csv" download>Export CSV</a>
        <button class="btn sm ghost" onClick=${() => window.print()}>Print</button></span>`}>
      <div class="secret" role="note"><span aria-hidden="true">\u{1F512}</span><span><b>Ballot secrecy.</b> The system stores only totals per candidate. No voter record, log entry or message says how an individual voted, and administrators cannot look it up.</span></div>
      <p style=${{ fontWeight: 650, marginBottom: '14px' }}>${verdict}</p>
      <div class="grid2">
        <div><${CandidateBars} cand=${S.cand} votes=${S.votes}/></div>
        <div><${Donut} cand=${S.cand} votes=${S.votes}/></div>
      </div>
    <//>
    <${Card} title="Votes over time" right=${html`<span class="mut sm">totals only, no candidate shown</span>`}>
      <${Timeline} history=${R.history} reg=${S.reg}/>
    <//>
    <p class="mut sm" style=${{ marginTop: '12px' }}>Printed ${new Date().toLocaleString()} \u00b7 ${S.booth}</p>
  </div>`;
}
