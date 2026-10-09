import { html, useState } from '../lib.js';
import { Card, EventRow } from '../ui.js';

function download(rows) {
  const text = rows.map(r => new Date(r.t * 1000).toISOString() + '  ' + r.m).join('\n');
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], { type: 'text/plain' }));
  a.download = 'activity.txt';
  a.click();
}

export function Activity({ L }) {
  const [lvl, setLvl] = useState('all');
  const rows = L.log.filter(e => lvl === 'all' || (lvl === 'issues' ? e.y >= 2 : e.y === Number(lvl)));
  return html`<${Card} title="Activity log" right=${html`<select aria-label="Filter events" value=${lvl} onChange=${e => setLvl(e.target.value)} style=${{ width: 'auto' }}>
      <option value="all">All events</option><option value="issues">Warnings and errors</option><option value="1">Success</option></select>
      <button class="btn sm ghost" onClick=${() => download(rows)}>Download</button>`}>
    <p class="mut sm" style=${{ marginBottom: '8px' }}>Last ${L.log.length} events. The full history is also written to server/data/voting_log.txt. Votes are logged without the candidate.</p>
    <div class="feed">${rows.length ? rows.map(e => html`<${EventRow} key=${e.i} e=${e}/>`) : html`<div class="mut">No events</div>`}</div>
  <//>`;
}
