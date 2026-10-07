/*****************************************************
 * webui.h - single-page admin console (served from flash)
 * Talks to the JSON API defined in voting_with_sms.ino.
 *****************************************************/
#pragma once
#include <pgmspace.h>

const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Fingerprint Voting Console</title>
<style>
:root{--bg:#f2f7ff;--card:#fff;--ink:#0f2447;--mut:#5d7194;--line:#dce7f8;--acc:#1d6fe8;--acc2:#2f8cff;--ok:#12a36b;--warn:#d98a0b;--err:#e5484d;--info:#1d6fe8;--c0:#1d6fe8;--c1:#06b6d4;--c2:#6366f1;--c3:#64748b;--sh:0 4px 18px rgba(29,111,232,.10)}
*{box-sizing:border-box;margin:0;padding:0}
a{color:var(--acc);font-weight:600;text-decoration:none}a:hover{text-decoration:underline}
body{font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--ink);line-height:1.5;font-size:15px}
header{position:sticky;top:0;z-index:20;display:flex;align-items:center;justify-content:space-between;gap:12px;padding:12px 20px;background:linear-gradient(135deg,#1558c8,var(--acc2));color:#fff;flex-wrap:wrap;box-shadow:0 2px 12px rgba(21,88,200,.25)}
.brand b{font-size:1.15rem}.brand small{display:block;opacity:.8;font-size:.78rem}
.hr{display:flex;align-items:center;gap:10px}
.dot{width:10px;height:10px;border-radius:50%;background:#aaa;display:inline-block}.dot.on{background:#4ade80;box-shadow:0 0 0 4px rgba(74,222,128,.25)}.dot.off{background:#f87171}
.pill{padding:3px 12px;border-radius:99px;font-size:.78rem;font-weight:700;background:rgba(255,255,255,.2)}
.pill.open{background:#16a36a}.pill.closed{background:#6b7090}
nav{display:flex;gap:4px;padding:10px 20px 0;overflow-x:auto;max-width:1150px;margin:0 auto}
nav button{border:0;background:none;color:var(--mut);padding:9px 14px;border-radius:10px 10px 0 0;font:inherit;font-weight:600;cursor:pointer;white-space:nowrap}
nav button.on{background:var(--card);color:var(--acc);box-shadow:var(--sh);border-bottom:3px solid var(--acc)}
main{max-width:1150px;margin:0 auto;padding:0 20px 40px}
section{display:none}section.on{display:block}
.card{background:var(--card);border-radius:14px;padding:20px;margin-top:16px;box-shadow:var(--sh)}
.card h2{font-size:1.05rem;margin-bottom:12px;display:flex;align-items:center;justify-content:space-between;gap:8px}
.grid{display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(240px,1fr))}
.grid2{display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(340px,1fr))}
.stat{background:var(--card);border-radius:14px;padding:18px;box-shadow:var(--sh);display:flex;align-items:center;gap:14px}
.stat .v{font-size:2rem;font-weight:800;line-height:1.1}.stat .l{color:var(--mut);font-size:.78rem;text-transform:uppercase;letter-spacing:.04em}
.ring{--p:0;width:64px;height:64px;border-radius:50%;background:conic-gradient(var(--acc) calc(var(--p)*1%),var(--line) 0);display:grid;place-items:center;flex:none}
.ring span{width:48px;height:48px;border-radius:50%;background:var(--card);display:grid;place-items:center;font-weight:800;font-size:.85rem}
.btn{border:0;border-radius:10px;padding:9px 16px;font:inherit;font-weight:600;cursor:pointer;color:#fff;background:linear-gradient(135deg,var(--acc),var(--acc2));transition:transform .15s,opacity .15s}
.btn:hover{transform:translateY(-1px)}.btn:disabled{opacity:.5;cursor:not-allowed;transform:none}
.btn.ok{background:var(--ok)}.btn.err{background:var(--err)}.btn.warn{background:var(--warn)}.btn.ghost{background:none;color:var(--acc);border:1.5px solid var(--line)}
.btn.sm{padding:5px 10px;font-size:.8rem;border-radius:8px}
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
.bar{height:12px;background:var(--line);border-radius:99px;overflow:hidden}.bar i{display:block;height:100%;border-radius:99px;transition:width .5s}
.cand{margin-bottom:14px}.cand .t{display:flex;justify-content:space-between;margin-bottom:5px;font-weight:600}
.banner{padding:14px 16px;border-radius:12px;font-weight:600;margin-bottom:10px;background:var(--line)}
.banner.ok{background:rgba(22,163,106,.15);color:var(--ok)}.banner.err{background:rgba(229,72,77,.15);color:var(--err)}.banner.warn{background:rgba(217,138,11,.15);color:var(--warn)}.banner.info{background:rgba(47,143,224,.15);color:var(--info)}
.big{font-size:1.25rem}
table{width:100%;border-collapse:collapse;font-size:.88rem}
th,td{text-align:left;padding:9px 8px;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--mut);font-size:.72rem;text-transform:uppercase;letter-spacing:.04em}
.tw{overflow-x:auto}
.badge{display:inline-block;padding:2px 10px;border-radius:99px;font-size:.75rem;font-weight:700;background:var(--line);color:var(--mut);white-space:nowrap}
.badge.ok{background:rgba(22,163,106,.15);color:var(--ok)}.badge.err{background:rgba(229,72,77,.15);color:var(--err)}.badge.warn{background:rgba(217,138,11,.15);color:var(--warn)}.badge.info{background:rgba(47,143,224,.15);color:var(--info)}
input,select,textarea{width:100%;padding:9px 12px;border:1.5px solid var(--line);border-radius:10px;font:inherit;background:var(--bg);color:var(--ink)}
input:focus,select:focus,textarea:focus{outline:none;border-color:var(--acc)}
input[type=checkbox]{width:auto}
label{display:block;font-size:.8rem;font-weight:600;color:var(--mut);margin:10px 0 4px}
label.chk{display:flex;gap:8px;align-items:center;color:var(--ink);font-size:.92rem;font-weight:500}
.tb{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:12px}.tb input[type=search]{max-width:260px}.tb select{max-width:160px}
.slots{display:grid;grid-template-columns:repeat(auto-fill,minmax(38px,1fr));gap:6px}
.slot{aspect-ratio:1;border-radius:8px;display:grid;place-items:center;font-size:.72rem;font-weight:700;cursor:pointer;background:var(--line);color:var(--mut);border:2px solid transparent}
.slot.O{background:rgba(22,163,106,.2);color:var(--ok)}.slot.M{background:rgba(229,72,77,.2);color:var(--err)}.slot.X{background:rgba(217,138,11,.25);color:var(--warn)}.slot.U{background:rgba(47,143,224,.2);color:var(--info)}.slot.sel{border-color:var(--acc)}
.leg{display:flex;gap:14px;flex-wrap:wrap;font-size:.8rem;color:var(--mut);margin-top:12px}
.feed{max-height:520px;overflow-y:auto}.ev{display:flex;gap:10px;padding:6px 0;border-bottom:1px solid var(--line);font-size:.86rem}
.ev time{color:var(--mut);white-space:nowrap;min-width:78px}.ev.ok b{color:var(--ok)}.ev.warn b{color:var(--warn)}.ev.err b{color:var(--err)}.ev b{font-weight:500}
.ov{position:fixed;inset:0;background:rgba(10,12,30,.6);z-index:50;display:grid;place-items:center;padding:16px;overflow-y:auto}.ov[hidden]{display:none}
.modal{background:var(--card);border-radius:16px;padding:24px;width:100%;max-width:460px;box-shadow:var(--sh)}
.modal h2{margin-bottom:6px}
.steps{display:flex;gap:6px;margin:16px 0}.steps div{flex:1;text-align:center;font-size:.72rem;padding:8px 4px;border-radius:8px;background:var(--line);color:var(--mut);font-weight:600}
.steps .act{background:var(--acc);color:#fff;animation:pl 1s infinite}.steps .done{background:var(--ok);color:#fff}.steps .bad{background:var(--err);color:#fff}
@keyframes pl{50%{opacity:.6}}
#toasts{position:fixed;right:16px;bottom:16px;z-index:99;display:flex;flex-direction:column;gap:8px}
.toast{background:#22254a;color:#fff;padding:11px 16px;border-radius:10px;box-shadow:var(--sh);font-size:.9rem;max-width:320px}.toast.err{background:var(--err)}.toast.ok{background:var(--ok)}
.mut{color:var(--mut)}.sm{font-size:.82rem}.mono{font-family:ui-monospace,Consolas,monospace}
.kv{display:grid;grid-template-columns:auto 1fr;gap:6px 16px;font-size:.9rem}.kv span:nth-child(odd){color:var(--mut)}
@media(max-width:600px){header{padding:10px 14px}main{padding:0 12px 30px}nav{padding:8px 12px 0}.grid2{grid-template-columns:1fr}}
</style></head><body>
<header>
 <div class="brand"><b>&#x1F5F3; Fingerprint Voting Console</b><small id="clock">connecting...</small></div>
 <div class="hr"><span id="link" class="dot" title="Live connection"></span><span id="elpill" class="pill">--</span><button id="elbtn" class="btn sm ghost" style="color:#fff;border-color:rgba(255,255,255,.5)">--</button></div>
</header>
<nav id="nav">
 <button data-t="dash" class="on">Dashboard</button><button data-t="voters">Voters</button><button data-t="fp">Fingerprints</button><button data-t="sms">SMS</button><button data-t="log">Activity</button><button data-t="set">Settings</button>
</nav>
<main>
<section id="t-dash" class="on">
 <div class="grid" style="margin-top:16px" id="stats"></div>
 <div class="grid2">
  <div class="card"><h2>Live results <span class="mut sm" id="leader"></span></h2><div id="cands"></div></div>
  <div class="card"><h2>Voting station</h2><div id="station"></div>
   <div class="row" style="margin-top:8px"><button class="btn sm ghost" id="goReg">+ Register voter</button><button class="btn sm ghost" id="goFp">Manage fingerprints</button></div></div>
 </div>
 <div class="grid2">
  <div class="card"><h2>Recent activity <a href="#" class="sm" data-go="log">view all</a></h2><div id="dlog"></div></div>
  <div class="card"><h2>Recent SMS <a href="#" class="sm" data-go="sms">view all</a></h2><div id="dsms"></div></div>
 </div>
</section>

<section id="t-voters"><div class="card">
 <h2>Voter database <span class="mut sm" id="vcount"></span></h2>
 <div class="tb"><input type="search" id="vq" placeholder="Search name, ID, phone, address"><select id="vf"><option value="all">All voters</option><option value="voted">Voted</option><option value="pending">Not voted</option><option value="nofp">Fingerprint problem</option></select>
  <label class="chk" style="margin:0"><input type="checkbox" id="vmask" checked> Mask phones</label>
  <span style="flex:1"></span><button class="btn sm ok" id="vadd">+ Register voter</button><a class="btn sm ghost" href="/api/export.csv" download="voters.csv" style="text-decoration:none">Export CSV</a></div>
 <div class="tw"><table><thead><tr><th>ID</th><th>Name</th><th>Age</th><th>Phone</th><th>Address</th><th>Finger</th><th>Status</th><th></th></tr></thead><tbody id="vbody"></tbody></table></div>
</div></section>

<section id="t-fp">
 <div class="grid2">
  <div class="card"><h2>Sensor</h2><div id="fpinfo"></div>
   <div class="row" style="margin-top:12px"><button class="btn sm" id="fpver">Verify templates</button><button class="btn sm ok" id="fpid">Test a finger</button><button class="btn sm err" id="fpclr">Clear sensor</button></div>
   <div id="fplive" style="margin-top:12px"></div></div>
  <div class="card"><h2>Slot details</h2><div id="fpdetail" class="mut">Select a slot below.</div></div>
 </div>
 <div class="card"><h2>Template map (IDs 1-40)</h2><div class="slots" id="slots"></div>
  <div class="leg"><span><span class="badge ok">OK</span> voter + template</span><span><span class="badge err">M</span> template missing</span><span><span class="badge warn">X</span> orphan template</span><span><span class="badge info">U</span> not verified</span><span><span class="badge">.</span> free</span></div></div>
</section>

<section id="t-sms">
 <div class="grid" style="margin-top:16px" id="smsstat"></div>
 <div class="card"><h2>Message log <span class="row"><button class="btn sm" id="smstest">Send test SMS</button><button class="btn sm ghost" id="smsclr">Clear finished</button></span></h2>
  <div class="tw"><table><thead><tr><th>Time</th><th>Type</th><th>To</th><th>Tpl</th><th>Message variables</th><th>Status</th><th></th></tr></thead><tbody id="smsbody"></tbody></table></div>
  <div class="mut sm" style="margin-top:10px">Last API response: <span class="mono" id="smsresp">-</span></div></div>
</section>

<section id="t-log"><div class="card"><h2>Activity log <span class="mut sm">last 20 events (kept in RAM)</span></h2><div class="feed" id="logfeed"></div></div></section>

<section id="t-set">
 <div class="card"><h2>Settings</h2><form id="setform"><div class="mut">Loading...</div></form></div>
 <div class="grid2">
  <div class="card"><h2>System</h2><div class="kv" id="sys"></div><div class="row" style="margin-top:14px"><button class="btn sm warn" id="reboot">Restart device</button></div></div>
  <div class="card"><h2>Danger zone</h2>
   <p class="mut sm" style="margin-bottom:12px">Reset clears every tally and lets all voters vote again. Erase removes all voters, votes and fingerprint templates.</p>
   <div class="row"><button class="btn sm warn" id="rstv">Reset votes</button><button class="btn sm err" id="erase">Erase all data</button></div></div>
 </div>
</section>
</main>
<div id="ov" class="ov" hidden></div>
<div id="enov" class="ov" hidden></div>
<div id="toasts"></div>
<script>
const $=(s,r=document)=>r.querySelector(s),$$=(s,r=document)=>[...r.querySelectorAll(s)];
const esc=s=>String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const CC=['var(--c0)','var(--c1)','var(--c2)','var(--c3)'];
let S=null,L={voters:[],sms:[],log:[],fp:null,set:null},R={},tab='dash',lastOk=0,pt=null,dismiss=0,slotSel=0;
const fmtUp=ms=>{let s=Math.floor(ms/1000),h=Math.floor(s/3600),m=Math.floor(s%3600/60);s%=60;return(h?h+'h ':'')+(h||m?m+'m ':'')+s+'s'};
const fmtT=(t,ms)=>t?new Date(t*1000).toLocaleTimeString([],{hour:'2-digit',minute:'2-digit',second:'2-digit'}):'T+'+fmtUp(ms);
const fmtDT=t=>t?new Date(t*1000).toLocaleString([],{day:'2-digit',month:'short',hour:'2-digit',minute:'2-digit'}):'-';
const maskP=p=>$('#vmask').checked?p.slice(0,2)+'******'+p.slice(-2):p;

async function api(p,body){
 const o={headers:{'X-Req':'1'}};
 if(body!==undefined){o.method='POST';o.headers['Content-Type']='application/x-www-form-urlencoded';o.body=new URLSearchParams(body).toString()}
 const r=await fetch(p,o);let j={};try{j=await r.json()}catch(e){}
 if(!r.ok||j.ok===0)throw new Error(j.msg||('HTTP '+r.status));
 return j;
}
function toast(m,c){const d=document.createElement('div');d.className='toast '+(c||'');d.textContent=m;$('#toasts').appendChild(d);setTimeout(()=>d.remove(),3800)}
async function act(p,body,msg){try{const j=await api(p,body||{});toast(j.msg||msg||'Done','ok');poll();return j}catch(e){toast(e.message,'err');return null}}

/* ---------- polling ---------- */
function poll(){
 clearTimeout(pt);
 api('/api/state').then(d=>{S=d;lastOk=Date.now();onState()}).catch(()=>{}).finally(()=>{
  setLink();
  const busy=S&&(S.en.s!=='idle'||S.st.s==='auth'||S.idn.on||S.ver.run);
  pt=setTimeout(poll,busy?500:1500);
 });
}
function setLink(){const ok=Date.now()-lastOk<5000;$('#link').className='dot '+(ok?'on':'off');if(!ok)$('#clock').textContent='connection lost - retrying...'}
async function sync(){
 if(!S)return;const r=S.rev,j=[];
 if(r.l!==R.l)j.push(api('/api/log').then(d=>{L.log=d;R.l=r.l;renderLog()}));
 if(r.s!==R.s&&(tab==='sms'||tab==='dash'))j.push(api('/api/sms').then(d=>{L.sms=d;R.s=r.s;renderSms()}));
 if(r.v!==R.v&&(tab==='voters'||tab==='fp'))j.push(api('/api/voters').then(d=>{L.voters=d;R.v=r.v;renderVoters();renderSlots()}));
 if(r.f!==R.f&&tab==='fp')j.push(api('/api/fp').then(d=>{L.fp=d;R.f=r.f;renderVoters();renderSlots()}));
 if(r.c!==R.c&&tab==='set')j.push(api('/api/settings').then(d=>{L.set=d;R.c=r.c;renderSettings()}));
 try{await Promise.all(j)}catch(e){}
}
function onState(){
 $('#clock').textContent=(S.ep?new Date(S.ep*1000).toLocaleString():'clock not synced')+'  |  up '+fmtUp(S.up);
 const o=S.open;$('#elpill').textContent=o?'ELECTION OPEN':'ELECTION CLOSED';$('#elpill').className='pill '+(o?'open':'closed');
 $('#elbtn').textContent=o?'Close election':'Open election';
 renderDash();renderStation();renderEnroll();renderFpLive();renderSys();sync();
}
$('#elbtn').onclick=()=>{if(!S)return;const o=S.open;if(o&&!confirm('Close the election? Voters will not be able to vote.'))return;act('/api/election',{open:o?0:1})};

/* ---------- tabs ---------- */
function go(t){tab=t;$$('nav button').forEach(b=>b.classList.toggle('on',b.dataset.t===t));$$('section').forEach(s=>s.classList.toggle('on',s.id==='t-'+t));
 if(t==='voters'||t==='fp'||t==='sms'||t==='set'){R.v=R.s=R.f=R.c=undefined}if(S)onState()}
$('#nav').onclick=e=>{const b=e.target.closest('button');if(b)go(b.dataset.t)};
document.addEventListener('click',e=>{const a=e.target.closest('[data-go]');if(a){e.preventDefault();go(a.dataset.go)}});
$('#goReg').onclick=()=>openRegister();$('#goFp').onclick=()=>go('fp');

/* ---------- dashboard ---------- */
function renderDash(){
 const tot=S.votes.reduce((a,b)=>a+b,0),pct=S.reg?Math.round(tot*100/S.reg):0,mx=Math.max(...S.votes);
 $('#stats').innerHTML=
  `<div class="stat"><div class="ring" style="--p:${pct}"><span>${pct}%</span></div><div><div class="v">${tot}<span class="mut" style="font-size:1rem">/${S.reg}</span></div><div class="l">Votes / registered</div></div></div>`+
  `<div class="stat"><div><div class="v">${S.reg-S.voted}</div><div class="l">Still to vote</div></div></div>`+
  `<div class="stat"><div><div class="v">${S.sensor.cnt}</div><div class="l">Templates on sensor</div></div></div>`+
  `<div class="stat"><div><div class="v">${S.sms.pend}</div><div class="l">SMS pending (${S.sms.sent} sent, ${S.sms.fail} failed)</div></div></div>`;
 $('#cands').innerHTML=S.cand.map((n,i)=>{const v=S.votes[i],p=tot?Math.round(v*100/tot):0;
  return `<div class="cand"><div class="t"><span>${tot&&v===mx?'&#x1F451; ':''}${esc(n)}</span><span>${v} <span class="mut sm">(${p}%)</span></span></div><div class="bar"><i style="width:${p}%;background:${CC[i]}"></i></div></div>`}).join('');
 const lead=S.votes.filter(v=>v===mx).length===1&&mx>0?S.cand[S.votes.indexOf(mx)]:'';
 $('#leader').textContent=lead?'Leading: '+lead:(tot?'Tie':'No votes yet');
 $('#dlog').innerHTML=L.log.slice(0,7).map(evRow).join('')||'<div class="mut">No activity yet</div>';
 $('#dsms').innerHTML=L.sms.slice(0,5).map(m=>`<div class="ev"><time>${fmtT(m.t,m.ms)}</time><span>${smsBadge(m.s)} <b>${esc(m.k)}</b> ${esc(m.v1)} / ${esc(m.v2)}</span></div>`).join('')||'<div class="mut">No messages yet</div>';
}
const evRow=e=>`<div class="ev ${['','ok','warn','err'][e.y]}"><time>${fmtT(e.t,e.ms)}</time><b>${esc(e.m)}</b></div>`;
function renderStation(){
 const st=S.st;let b='',c='info';
 if(!S.sensor.ok){b='Fingerprint sensor offline - check wiring (see Settings > System)';c='err'}
 else if(['wait1','remove','wait2'].includes(S.en.s)){b='Enrollment in progress...';c='info'}
 else if(S.ver.run){b='Verifying sensor templates ('+S.ver.pos+'/'+S.ver.max+')';c='info'}
 else if(S.idn.on){b='Test mode: place any finger on the sensor';c='info'}
 else if(st.s==='auth'){b='<span class="big">'+esc(st.name)+'</span> (ID '+st.id+')<br>Authenticated - press candidate button 1-4. '+st.left+'s left';c='ok'}
 else if(st.s==='scan'){b='Ready - waiting for a voter to place a finger';c='ok'}
 else{b='Election closed - sensor idle. Open the election to start voting.';c='warn'}
 let h=`<div class="banner ${c}">${b}</div>`;
 if(st.msg)h+=`<div class="banner ${st.mt}">${esc(st.msg)}</div>`;
 if(S.idn.msg)h+=`<div class="banner info">${esc(S.idn.msg)}</div>`;
 $('#station').innerHTML=h;
}

/* ---------- enrollment overlay ---------- */
function renderEnroll(){
 const e=S.en,ov=$('#enov');
 if(e.s==='idle'||dismiss===e.seq){ov.hidden=true;return}
 const idx={wait1:0,remove:e.ph?3:1,wait2:2,wait3:4,ok:5,fail:-1}[e.s],names=['Place finger','Lift finger','Place again','Lift','Verify'];
 const st=names.map((n,i)=>`<div class="${e.s==='fail'?(i===0?'bad':''):(i<idx?'done':i===idx?'act':'')}">${i+1}. ${n}</div>`).join('');
 const run=idx>=0&&idx<5,cls=e.s==='ok'?'ok':e.s==='fail'?'err':'info';
 ov.hidden=false;
 ov.innerHTML=`<div class="modal"><h2>${e.resc?'Re-scan fingerprint':'Fingerprint enrollment'}</h2><div class="mut sm">Voter ID ${e.id}${e.try?' &middot; attempt '+(e.try+1)+'/3':''}</div><div class="steps">${st}</div>
  <div class="banner ${cls} big">${esc(e.msg)}</div>
  <div class="row">${run?'<button class="btn err" id="enx">Cancel</button>':'<button class="btn" id="enc">Close</button>'}</div></div>`;
 if(run)$('#enx').onclick=()=>act('/api/enroll/cancel',{});else $('#enc').onclick=()=>{dismiss=e.seq;ov.hidden=true};
}

/* ---------- voters ---------- */
function renderVoters(){
 const q=$('#vq').value.toLowerCase(),f=$('#vf').value;
 const rows=L.voters.filter(v=>{
  if(f==='voted'&&!v.v)return false;if(f==='pending'&&v.v)return false;if(f==='nofp'&&(v.fp==='O'||v.fp==='U'))return false;
  return !q||(v.id+' '+v.n+' '+v.p+' '+v.ad).toLowerCase().includes(q)});
 $('#vcount').textContent=L.voters.length+' registered, '+L.voters.filter(v=>v.v).length+' voted';
 const fp={O:['ok','OK'],M:['err','Missing'],U:['info','Unverified'],X:['warn','Orphan']};
 $('#vbody').innerHTML=rows.map(v=>{const b=fp[v.fp]||['','?'];
  return `<tr><td>${v.id}</td><td><b>${esc(v.n)}</b></td><td>${v.a}</td><td class="mono">${esc(maskP(v.p))}</td><td>${esc(v.ad)}</td><td><span class="badge ${b[0]}">${b[1]}</span></td>
  <td>${v.v?'<span class="badge ok">Voted</span><div class="mut sm">'+fmtDT(v.vt)+'</div>':'<span class="badge warn">Not voted</span>'}</td>
  <td style="white-space:nowrap"><button class="btn sm ghost" data-a="edit" data-id="${v.id}">Edit</button> <button class="btn sm ghost" data-a="rescan" data-id="${v.id}">Re-scan</button> <button class="btn sm err" data-a="del" data-id="${v.id}">Delete</button></td></tr>`}).join('')
  ||'<tr><td colspan="8" class="mut">No voters match.</td></tr>';
}
['vq','vf','vmask'].forEach(i=>$('#'+i).addEventListener('input',renderVoters));
$('#vbody').onclick=e=>{const b=e.target.closest('button');if(!b)return;const id=+b.dataset.id,v=L.voters.find(x=>x.id===id);if(!v)return;
 if(b.dataset.a==='edit')openEdit(v);
 else if(b.dataset.a==='rescan'){if(confirm('Replace the fingerprint of '+v.n+'? The voter will be asked to scan twice.'))act('/api/enroll',{id,rescan:1})}
 else if(confirm('Delete '+v.n+' (ID '+id+') and their fingerprint?'))act('/api/voter/delete',{id})};
$('#vadd').onclick=()=>openRegister();

function modal(h){const o=$('#ov');o.innerHTML='<div class="modal">'+h+'</div>';o.hidden=false;o.onclick=e=>{if(e.target===o)closeModal()};return o}
function closeModal(){$('#ov').hidden=true}
async function openRegister(){
 if(!L.voters.length){try{L.voters=await api('/api/voters')}catch(e){}}
 let id=1;const used=new Set(L.voters.map(v=>v.id));while(used.has(id)&&id<40)id++;
 modal(`<h2>Register voter</h2><div class="mut sm">After you submit, the sensor asks the voter to scan the same finger twice.</div>
 <form id="rf"><label>Voter ID (1-40, = sensor slot)</label><input name="id" type="number" min="1" max="40" value="${id}" required>
 <label>Full name</label><input name="name" maxlength="23" required>
 <label>Age (18+)</label><input name="age" type="number" min="18" max="120" required>
 <label>Mobile (10 digits)</label><input name="phone" inputmode="numeric" pattern="[0-9]{10}" maxlength="10" required>
 <label>Address</label><input name="address" maxlength="39" required>
 <div class="row" style="margin-top:16px"><button class="btn ok" type="submit">Start enrollment</button><button class="btn ghost" type="button" onclick="closeModal()">Cancel</button></div></form>`);
 $('#rf').onsubmit=async e=>{e.preventDefault();const r=await act('/api/enroll',Object.fromEntries(new FormData(e.target)));if(r)closeModal()};
}
function openEdit(v){
 modal(`<h2>Edit voter #${v.id}</h2><form id="ef"><input type="hidden" name="id" value="${v.id}">
 <label>Full name</label><input name="name" maxlength="23" value="${esc(v.n)}" required>
 <label>Age</label><input name="age" type="number" min="18" max="120" value="${v.a}" required>
 <label>Mobile (10 digits)</label><input name="phone" pattern="[0-9]{10}" maxlength="10" value="${esc(v.p)}" required>
 <label>Address</label><input name="address" maxlength="39" value="${esc(v.ad)}" required>
 <div class="row" style="margin-top:16px"><button class="btn ok" type="submit">Save</button><button class="btn ghost" type="button" onclick="closeModal()">Cancel</button></div></form>`);
 $('#ef').onsubmit=async e=>{e.preventDefault();const r=await act('/api/voter/update',Object.fromEntries(new FormData(e.target)));if(r)closeModal()};
}

/* ---------- fingerprints ---------- */
function renderSlots(){
 const m=L.fp?L.fp.slots:'.'.repeat(40),vm={};L.voters.forEach(v=>vm[v.id]=v);
 $('#slots').innerHTML=[...m].map((c,i)=>`<div class="slot ${c==='.'?'':c}${slotSel===i+1?' sel':''}" data-id="${i+1}" title="${vm[i+1]?esc(vm[i+1].n):'free'}">${i+1}</div>`).join('');
 const info=L.fp?`<div class="kv"><span>Sensor</span><span>${L.fp.ok?'<span class="badge ok">online</span>':'<span class="badge err">offline</span>'}</span><span>Templates stored</span><span>${L.fp.cnt} / ${L.fp.cap}</span><span>Security level</span><span>${L.fp.sec} (1 loose - 5 strict)</span><span>Missing templates</span><span>${L.fp.miss}</span><span>Orphan templates</span><span>${L.fp.orph}</span></div>`:'<div class="mut">Loading...</div>';
 $('#fpinfo').innerHTML=info;if(slotSel)showSlot(slotSel);
}
function showSlot(id){
 slotSel=id;const c=L.fp?L.fp.slots[id-1]:'.',v=L.voters.find(x=>x.id===id);let h=`<div class="big"><b>Slot ${id}</b></div>`;
 const t={O:'Voter registered and fingerprint present.',M:'Voter is registered but the sensor has no template - re-scan the finger.',X:'Template on the sensor but no voter record (orphan). Delete it or register a voter on this ID.',U:'Voter registered, template not verified yet. Run "Verify templates".','.':'Free slot.'};
 h+=`<p class="mut" style="margin:6px 0">${t[c]||''}</p>`;
 if(v)h+=`<div class="kv"><span>Voter</span><span>${esc(v.n)}</span><span>Phone</span><span class="mono">${esc(maskP(v.p))}</span><span>Voted</span><span>${v.v?'yes':'no'}</span></div><div class="row" style="margin-top:10px"><button class="btn sm" id="sr">Re-scan finger</button></div>`;
 else if(c==='X')h+=`<div class="row"><button class="btn sm err" id="sd">Delete orphan template</button></div>`;
 else h+=`<div class="row"><button class="btn sm ok" id="sn">Register voter here</button></div>`;
 $('#fpdetail').innerHTML=h;
 const g=i=>document.getElementById(i);
 if(g('sr'))g('sr').onclick=()=>act('/api/enroll',{id,rescan:1});
 if(g('sd'))g('sd').onclick=()=>confirm('Delete template '+id+'?')&&act('/api/fp/delete',{id});
 if(g('sn'))g('sn').onclick=async()=>{await openRegister();$('#rf').elements.id.value=id};
}
$('#slots').onclick=e=>{const s=e.target.closest('.slot');if(s){slotSel=+s.dataset.id;renderSlots()}};
$('#fpver').onclick=()=>act('/api/fp/verify',{});
$('#fpid').onclick=()=>act('/api/fp/identify',{});
$('#fpclr').onclick=()=>confirm('Delete ALL fingerprint templates from the sensor? Voters stay in the database but cannot authenticate until re-scanned.')&&act('/api/fp/clear',{});
function renderFpLive(){
 let h='';if(S.ver.run)h=`<div class="banner info">Verifying slots ${S.ver.pos}/${S.ver.max}</div><div class="bar"><i style="width:${Math.round(S.ver.pos*100/S.ver.max)}%;background:var(--acc)"></i></div>`;
 else if(S.idn.msg)h=`<div class="banner info">${esc(S.idn.msg)}</div>`;
 $('#fplive').innerHTML=h;
}

/* ---------- SMS ---------- */
const smsBadge=s=>`<span class="badge ${({sent:'ok',failed:'err',retry:'warn',queued:'info',skipped:''})[s]||''}">${s}</span>`;
function renderSms(){
 const c=k=>L.sms.filter(m=>m.s===k).length;
 $('#smsstat').innerHTML=[['Queued/retrying',S?S.sms.pend:0],['Sent (since boot)',S?S.sms.sent:0],['Failed (since boot)',S?S.sms.fail:0],['In log',L.sms.length]].map(a=>`<div class="stat"><div><div class="v">${a[1]}</div><div class="l">${a[0]}</div></div></div>`).join('');
 $('#smsbody').innerHTML=L.sms.map(m=>`<tr><td>${fmtT(m.t,m.ms)}</td><td><span class="badge">${esc(m.k)}</span></td><td class="mono">${esc(m.to)}</td><td>${esc(m.tp)}</td><td>${esc(m.v1)} <span class="mut">/</span> ${esc(m.v2)}</td>
  <td>${smsBadge(m.s)}${m.a?'<div class="mut sm">try '+m.a+(m.h?' &middot; HTTP '+m.h:'')+'</div>':''}</td><td>${m.s==='failed'||m.s==='skipped'?`<button class="btn sm ghost" data-id="${m.i}">Resend</button>`:''}</td></tr>`).join('')||'<tr><td colspan="7" class="mut">No messages yet.</td></tr>';
 if(S)$('#smsresp').textContent=S.sms.last||'-';
 if(S)renderDash();
}
$('#smsbody').onclick=e=>{const b=e.target.closest('button');if(b)act('/api/sms/resend',{id:b.dataset.id})};
$('#smstest').onclick=()=>act('/api/sms/test',{});
$('#smsclr').onclick=()=>act('/api/sms/clear',{});
function renderLog(){$('#logfeed').innerHTML=L.log.map(evRow).join('')||'<div class="mut">No events yet</div>';if(S)renderDash()}

/* ---------- settings ---------- */
function renderSettings(){
 const s=L.set;if(!s)return;const ck=(n,l,v)=>`<label class="chk"><input type="checkbox" name="${n}" ${v?'checked':''}> ${l}</label>`;
 $('#setform').innerHTML=`<div class="grid2"><div><h3 style="font-size:.95rem">Candidates (buttons 1-4)</h3>${s.cand.map((c,i)=>`<label>Button ${i+1}</label><input name="c${i}" maxlength="20" value="${esc(c)}" required>`).join('')}</div>
 <div><h3 style="font-size:.95rem">Voting</h3><label>Vote timeout after authentication (10-120 s)</label><input name="vt" type="number" min="10" max="120" value="${s.vt}">
 <label>Minimum match confidence (10-200)</label><input name="mc" type="number" min="10" max="200" value="${s.mc}">
 <label>Sensor security level (1-5)</label><input name="sec" type="number" min="1" max="5" value="${s.sec}"></div>
 <div><h3 style="font-size:.95rem">SMS notifications</h3>${ck('sms','Enable SMS sending',s.sms)}${ck('smsV','SMS voter after voting',s.smsV)}${ck('smsR','SMS voter after registration',s.smsR)}${ck('smsA','SMS admin on every vote',s.smsA)}${ck('seeC','Include candidate in admin SMS (breaks ballot secrecy)',s.seeC)}
 <label>Admin phone (10 digits or 91xxxxxxxxxx)</label><input name="admin" value="${esc(s.admin)}" maxlength="12"></div></div>
 <div class="row" style="margin-top:16px"><button class="btn ok" type="submit">Save settings</button></div>`;
}
$('#setform').onsubmit=async e=>{e.preventDefault();const f=new FormData(e.target),o={};f.forEach((v,k)=>o[k]=v);
 ['sms','smsV','smsR','smsA','seeC'].forEach(k=>o[k]=f.has(k)?1:0);const r=await act('/api/settings',o);if(r){R.c=undefined;sync()}};
function renderSys(){
 if(tab!=='set'||!S)return;
 $('#sys').innerHTML=[['Uptime',fmtUp(S.up)],['IP address',S.ip],['Wi-Fi',S.wifi?('connected ('+S.rssi+' dBm)'):'disconnected'],['Free heap',(S.heap/1024).toFixed(1)+' KB (largest block '+(S.blk/1024).toFixed(1)+' KB)'],['Heap fragmentation',S.frag+'%'],['Flash storage',(S.fsUsed/1024).toFixed(0)+' / '+(S.fsTotal/1024).toFixed(0)+' KB'],['Sensor',S.sensor.ok?'online':'OFFLINE'],['Clock',S.ep?'NTP synced':'not synced']].map(a=>`<span>${a[0]}</span><span>${esc(a[1])}</span>`).join('');
}
$('#reboot').onclick=()=>confirm('Restart the device? Data is saved in flash and survives.')&&act('/api/reboot',{});
$('#rstv').onclick=()=>confirm('Reset ALL votes to zero and mark every voter as not voted?')&&act('/api/votes/reset',{});
$('#erase').onclick=()=>{const t=prompt('This deletes ALL voters, votes and sensor templates.\nType ERASE to confirm.');if(t==='ERASE')act('/api/factory',{confirm:'ERASE'})};

poll();
if(location.hash)go(location.hash.slice(1));
</script></body></html>)HTML";
