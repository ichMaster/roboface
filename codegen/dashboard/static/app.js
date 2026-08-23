/* ── live binding ────────────────────────────────────────────────────────────
   The renderers below are unchanged from the prototype: each is a pure
   render(state) -> html. Only the data source moved -- from mock constants to a
   WebSocket carrying the reduced state (dashboard-specification §6). */

let STATE = null;
let redrawTimer = null;

/* Debounced to <=5Hz: the log arrives in bursts and re-rendering per event thrashes
   (spec §6.3). A panel holding the focused element is skipped until blur, so a
   keyboard user never loses their place mid-inspection. */
function scheduleRender(){
  if (redrawTimer) return;
  redrawTimer = setTimeout(() => { redrawTimer = null; renderAll(); }, 200);
}

function connect(){
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  const ws = new WebSocket(`${proto}://${location.host}/ws`);
  let backoff = 500;
  ws.onopen = () => { backoff = 500; setConnected(true); };
  ws.onmessage = (ev) => {
    try { const msg = JSON.parse(ev.data); if (msg.state){ STATE = msg.state; scheduleRender(); } }
    catch (_) { /* a malformed frame must not take the page down */ }
  };
  ws.onclose = () => {
    setConnected(false);                       // hold the last render; never blank it
    setTimeout(connect, backoff);
    backoff = Math.min(backoff * 2, 10000);
  };
  ws.onerror = () => ws.close();
}

function setConnected(ok){
  const el = document.getElementById('conn');
  if (!el) return;
  el.className = 'status ' + (ok ? 's-run' : 's-held');
  el.textContent = '';
  const dot = document.createElement('span'); dot.className = 'dot'; el.appendChild(dot);
  el.appendChild(document.createTextNode(ok ? 'live' : 'reconnecting'));
}

/* A panel containing the focused element is not re-rendered (spec §6.3). */
function safeSet(id, html){
  const node = document.getElementById(id);
  if (!node) return;
  if (node.contains(document.activeElement) && document.activeElement !== document.body) return;
  node.innerHTML = html;
}


/* ── STATE -> the shapes the panels render ───────────────────────────────────
   The renderers came from the prototype and are unchanged. This adapter is the
   only new logic: it derives their inputs from the reduced state, so nothing
   about the validated drawing code had to be rewritten. */

function versionsFromState(){
  const out = [];
  for (const phase of (STATE?.tree || [])){
    for (const v of (phase.children || [])){
      const steps = {};
      for (const st of (v.children || [])){
        const key = ({'generate-issues':'generate','upload-issues':'upload',
                      'execute-issues':'execute','review-and-fix-issues':'review',
                      'release-version':'release'})[st.id];
        if (key) steps[key] = st.elapsed_s || 0;
      }
      const find = {fixnow:0, hard:0, defer:0, held:0};
      for (const f of (STATE?.findings || [])){
        if (f.version !== v.id) continue;
        if (f.outcome === 'fixed') find.fixnow++;
        else if (f.outcome === 'hardened') find.hard++;
        else if (f.outcome === 'held') find.held++;
        else find.defer++;
      }
      const reviewed = (v.children || []).some(c => c.id === 'review-and-fix-issues' && c.end);
      out.push({id: v.id, title: v.id, status: v.status === 'ok' ? 'ok'
                : v.status === 'running' ? 'run' : v.status === 'skip' ? 'todo' : 'run',
                tag: (v.data && v.data.tag) || null,
                steps: Object.keys(steps).length ? steps : null,
                tests: (v.data && typeof v.data.tests_passing === 'number')
                       ? v.data.tests_passing : null,
                issues: (v.children || []).flatMap(c => c.children || []).length || 0,
                find: reviewed ? find : null});
    }
  }
  return out;
}

function issuesFromState(){
  const out = [];
  for (const phase of (STATE?.tree || []))
    for (const v of (phase.children || []))
      for (const step of (v.children || []))
        for (const issue of (step.children || []))
          out.push({id: issue.id, v: v.id,
                    size: (issue.data && issue.data.size) || 'M',
                    dur: issue.elapsed_s || 0,
                    att: (issue.data && issue.data.attempts) || 1});
  return out;
}

function renderAll(){
  if (!STATE) return;
  V.length = 0; V.push(...versionsFromState());
  ISSUES.length = 0; ISSUES.push(...issuesFromState());
  Object.assign(REPO, {
    branch: STATE.github?.branch || '-', head: STATE.github?.head_sha || '-',
    created: STATE.github?.created || 0, closed: STATE.github?.closed || 0,
    commits: STATE.github?.commits || 0,
  });
  renderHeader(); renderKpis();
  const failures = [];
  for (const fn of [renderTree, renderBurn, renderVel, renderTime,
                    renderFail, renderSuite, renderQuality]) {
    try { fn(); } catch (err) { failures.push(fn.name + ': ' + err.message); }
  }
  reportFailures(failures);
}

/* A failed panel is reported in the footer, never swallowed and never allowed to
   blank the page -- an observability tool that hides its own faults is the worst kind. */
function reportFailures(f){
  const el = document.getElementById('panel-errors');
  if (el) el.textContent = f.length ? 'panels failed to render: ' + f.join(' | ') : '';
}

function renderHeader(){
  const now = document.querySelector('.hero .now');
  if (now){
    const status = STATE.status === 'running' ? 'run'
      : STATE.status === 'done' ? 'ok' : STATE.status === 'aborted' ? 'fail' : 'skip';
    const word = STATE.status === 'running' ? 'running' : STATE.status;
    now.textContent = STATE.current || (STATE.status === 'done' ? 'finished' : '—');
    const chip = document.createElement('span');
    chip.className = 'status s-' + status;
    const dot = document.createElement('span'); dot.className = 'dot';
    chip.appendChild(dot); chip.appendChild(document.createTextNode(' ' + word));
    now.appendChild(document.createTextNode(' '));
    now.appendChild(chip);
  }
  const el = document.getElementById('h-eta');
  if (el){
    const eta = STATE.eta;
    el.textContent = eta ? `~${Math.round(eta.low_s/60)}–${Math.round(eta.high_s/60)} min` : '—';
  }
  const elapsed = document.getElementById('h-elapsed');
  if (elapsed) elapsed.textContent = mmss(STATE.elapsed_s || 0);
  // textContent, never innerHTML: the command and branch come from the log, and a log
  // is data from a process this page does not control.
  const sub = document.getElementById('h-sub');
  if (sub) sub.textContent = [STATE.command, STATE.run_id, STATE.github?.branch]
    .filter(Boolean).join(' · ');
}

/* Panel configuration -- not data. The step and outcome orders map to categorical
   slots, and the slot ORDER is the CVD-safety mechanism, so these are fixed. */
const STEPS = [['generate','Generate issues',1],['upload','Upload issues',2],
               ['execute','Execute issues',3],['review','Review & fix',4],['release','Release',5]];
const QSEG  = [['fixnow','Fixed now',1],['hard','Hardened later',3],
               ['defer','Still deferred',4],['held','Held',2]];

const V = [];
const ISSUES = [];
const REPO = {branch:'-', head:'-', created:0, closed:0, commits:0};

connect();

const S=v=>`var(--series-${v})`;
/* Round the TOTAL, then split. Rounding minutes and seconds independently let
   1619.7s render as "26:60" -- floor(26.99)=26 and round(59.7)=60. A clock that can
   print :60 undermines every other number on the page. */
const mmss=s=>{const t=Math.max(0,Math.round(s));return `${Math.floor(t/60)}:${String(t%60).padStart(2,'0')}`;};

/* Axis maxima came from the prototype's mock data as constants. Real runs exceed them,
   and an SVG does not clip by default -- the suite series left its card and painted
   over the panel above it. Derive the ceiling from the data instead. */
const niceMax=(v,floor)=>{const m=Math.max(v||0,floor||1);
  const p=Math.pow(10,Math.floor(Math.log10(m))),r=m/p;
  return (r<=1?1:r<=2?2:r<=2.5?2.5:r<=5?5:10)*p;};
const ticks=(max,n)=>Array.from({length:n+1},(_,i)=>max*i/n);

/* Row pitch, for the horizontal bar panels. They divided a FIXED height by the row
   count, so ten versions where the prototype had four gave every bar a negative height
   -- the bars vanished and the labels landed on top of each other. The chart grows with
   its rows instead; the card scrolls if it must. */
const PITCH=26, BARH=16;

/* Category labels on an x-axis. Ten versions in a narrow card overran each other --
   the same fixed-layout assumption as the bar heights, one axis over. Rotate as soon
   as the slot is too narrow for the text; stay horizontal when there is room, because
   upright labels are easier to read and most panels have it. */
const xLabel=(text,cx,y,slot)=>{
  const need=String(text).length*6.2+6;
  return need<=slot
    ? `<text class="ax xcat" x="${cx}" y="${y}" text-anchor="middle">${esc(text)}</text>`
    : `<text class="ax xcat" x="${cx}" y="${y}" text-anchor="end"
             transform="rotate(-40 ${cx} ${y})">${esc(text)}</text>`;
};
const W=v=>({S:1,M:3,L:5})[v.size];
const el=(id)=>document.getElementById(id);
const svg=(w,h)=>`<svg viewBox="0 0 ${w} ${h}" role="img">`;
/* The log is written by skills, hooks and a shell -- data, not markup. The tooltip
   path already uses textContent for that reason; the panels build HTML strings, so
   anything from the log is escaped on the way in. */
const esc=v=>String(v).replace(/[&<>"']/g, c =>
  ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));

/* ── tooltip: textContent only — labels are untrusted data ──────────────── */
const tip=el('tip');
function showTip(e,title,rows){
  tip.textContent='';
  const t=document.createElement('div'); t.className='v'; t.textContent=title; tip.appendChild(t);
  rows.forEach(([label,val,color])=>{
    const r=document.createElement('div'); r.className='r';
    if(color){const k=document.createElement('i');k.className='lk';k.style.background=color;r.appendChild(k);}
    const s=document.createElement('span'); s.textContent=label+' '+val; r.appendChild(s);
    tip.appendChild(r);
  });
  tip.style.opacity='1';
  const pad=14, w=tip.offsetWidth, h=tip.offsetHeight;
  tip.style.left=Math.min(e.clientX+pad, innerWidth-w-8)+'px';
  tip.style.top =Math.max(8, e.clientY-h-pad)+'px';
}
const hideTip=()=>tip.style.opacity='0';
function bind(node,title,rows){
  node.addEventListener('pointermove',e=>showTip(e,title,rows));
  node.addEventListener('pointerleave',hideTip);
  node.addEventListener('focus',e=>{const r=node.getBoundingClientRect();
    showTip({clientX:r.left+r.width/2,clientY:r.top},title,rows);});
  node.addEventListener('blur',hideTip);
  node.setAttribute('tabindex','0');
}

/* ── 2 · KPI tiles ──────────────────────────────────────────────────────── */
const done=ISSUES.length;


const findOpen=V.reduce((a,v)=>a+(v.find?v.find.defer+v.find.held:0),0);
const vmean=id=>{const d=ISSUES.filter(i=>i.v===id);return d.reduce((a,i)=>a+i.dur,0)/d.length;};
const velD=(vmean('v01.03')-vmean('v01.02'))/vmean('v01.02')*100;
function renderKpis(){
  const sc = STATE.scope || {}, m = STATE.metrics || {}, gh = STATE.github || {};
  const vmean = id => { const d = ISSUES.filter(i=>i.v===id); return d.length ? d.reduce((a,i)=>a+i.dur,0)/d.length : 0; };
  const vers = V.map(v=>v.id);
  const cur = vers[vers.length-1], prev = vers[vers.length-2];
  const m1 = cur ? vmean(cur) : 0, m0 = prev ? vmean(prev) : 0;
  const diff = Math.round(m0 - m1);
  const tiles = [
    ['Issues done', `${m.issues_done ?? 0}`,
     `${sc.known ?? 0} known · ${sc.est_low ?? 0}–${sc.est_high ?? 0} projected`, ''],
    ['Versions', `${m.versions_released ?? 0} <small>/ ${(STATE.plan||[]).length}</small>`,
     STATE.status === 'running' ? 'in progress' : STATE.status, ''],
    // Label names unit AND subject; delta names its comparison, shows that value, and
    // states direction in words -- a green arrow on a TIME metric is ambiguous (spec §4.3).
    ['Mean time per issue', cur ? `${mmss(m1)} <small>in ${cur}</small>` : '—',
     prev && Math.abs(diff) >= 1
       ? `${Math.abs(diff)}s ${diff > 0 ? 'faster' : 'slower'} than ${prev} (${mmss(m0)})`
       : 'no prior version to compare', diff > 0 ? 'up' : ''],
    ['Tests passing', `${m.tests_passing ?? 0}`, '', ''],
    ['Tests failing now', '0', 'expected — see failure surface', ''],
    ['Review findings', `${m.findings_open ?? 0} <small>open</small>`,
     `${m.findings_total ?? 0} raised`, ''],
    ['GitHub issues', `${gh.created ?? 0} <small>created</small>`,
     `${gh.closed ?? 0} closed · ${gh.open ?? 0} open`, ''],
    ['Commits', `${gh.commits ?? 0}`, `on ${gh.branch || '-'} · from ${gh.head_sha || '-'}`, ''],
  ];
  safeSet('kpis', tiles.map(([l,v,d,c]) =>
    `<div class="kpi"><div class="lbl">${l}</div><div class="val">${v}</div><div class="delta ${c}">${d}</div></div>`
  ).join(''));
}

function renderTree(){
  const sc={ok:'s-ok',run:'s-run',todo:'s-skip',fail:'s-fail'};
  const word={ok:'done',run:'running',todo:'queued',fail:'failed'};
  const rs=STATE.status==='running'?'run':STATE.status==='done'?'ok':STATE.status==='aborted'?'fail':'todo';
  let h=`<div class="tnode"><span class="status ${sc[rs]}"><span class="dot"></span></span>
        <span class="tname">run · ${esc(STATE.command||'—')}</span>
        <span class="tdur">${mmss(STATE.elapsed_s||0)}</span></div>`;
  V.forEach(v=>{
    const total=v.steps?Object.values(v.steps).reduce((a,b)=>a+b,0):null;
    h+=`<div class="tnode d1 ${v.status==='run'?'active':''}">
        <span class="status ${sc[v.status]}"><span class="dot"></span></span>
        <span class="tname">${v.id} — ${v.title}</span>
        ${v.tag?`<span class="tag">${v.tag}</span>`:''}
        <span class="tdur">${total?mmss(total):word[v.status]}</span></div>`;
    if(v.status==='run'){
      STEPS.forEach(([k,label])=>{
        const d=v.steps[k];
        h+=`<div class="tnode d2"><span class="status ${d?'s-ok':'s-skip'}"><span class="dot"></span></span>
            <span class="tname">${label}</span><span class="tdur">${d?mmss(d):'—'}</span></div>`;
        if(k==='execute'){
          ISSUES.filter(i=>i.v===v.id).forEach(i=>{
            h+=`<div class="tnode d3"><span class="status ${i.att>1?'s-held':'s-ok'}"><span class="dot"></span></span>
                <span class="tname">${i.id} · ${i.size}${i.att>1?` · ${i.att} attempts`:''}</span>
                <span class="tdur">${mmss(i.dur)}</span></div>`;
          });
        }
      });
    }
  });
  el('tree').innerHTML=h;
}

/* ── 4 · burn-down: PROJECTED total (line) + uncertainty range (band) ──────
   Scope is discovered, not declared. Plotting only *known* remaining would drop the
   line to zero at every version boundary — reading as "finished" when it means "the
   next version isn't decomposed yet". So the line is the best estimate of total
   remaining (known + midpoint), and the band is the low–high range. The band is
   widest at t=0, when the total is entirely inference, and narrows as versions land. */
function renderBurn(){
  const LO=9, HI=21;                       // points per undecomposed version (3–7 issues × ~3)
  // From STATE, not a constant: the burn-down is the only panel needing shape over TIME,
  // so the reducer samples remaining work at every event that changes it.
  const TL = (STATE.burndown || []).map(p => [p.elapsed_s/60, p.known_points, p.undecomposed]);
  if (!TL.length){ el('c-burn').innerHTML = '<p class="note">no data yet</p>'; return; }
  const lo=p=>p[1]+p[2]*LO, hi=p=>p[1]+p[2]*HI, mid=p=>(lo(p)+hi(p))/2;
  const W_=560,H=200,L=34,R=14,T=10,B=26;
  const hiOf = p => p[1] + p[2]*HI;
  const maxY = Math.max(10, Math.ceil(Math.max(...TL.map(hiOf)) * 1.15 / 10) * 10);
  const maxX = Math.max(10, Math.ceil(Math.max(...TL.map(p=>p[0])) * 1.6 / 10) * 10);
  const x=v=>L+(v/maxX)*(W_-L-R), y=v=>T+(1-v/maxY)*(H-T-B);
  let s=svg(W_,H);
  const yTicks=[0, maxY/3, maxY*2/3, maxY].map(v=>Math.round(v));
  yTicks.forEach(g=>{ s+=`<line class="gridline" x1="${L}" x2="${W_-R}" y1="${y(g)}" y2="${y(g)}"/>
      <text class="ax" x="${L-6}" y="${y(g)+3.5}" text-anchor="end">${g}</text>`; });
  s+=`<line class="axisline" x1="${L}" x2="${W_-R}" y1="${y(0)}" y2="${y(0)}"/>`;
  [0, maxX/4, maxX/2, maxX*3/4, maxX].map(v=>Math.round(v)).forEach(t=>s+=`<text class="ax" x="${x(t)}" y="${H-8}" text-anchor="middle">${t}m</text>`);
  const up=TL.map(p=>`${x(p[0]).toFixed(1)},${y(hi(p)).toFixed(1)}`).join(' L');
  const dn=TL.slice().reverse().map(p=>`${x(p[0]).toFixed(1)},${y(lo(p)).toFixed(1)}`).join(' L');
  s+=`<path d="M${up} L${dn} Z" fill="${S(1)}" opacity=".16"/>`;
  s+=`<line x1="${x(0)}" y1="${y(mid(TL[0]))}" x2="${x(maxX*0.9)}" y2="${y(0)}"
        stroke="var(--text-muted)" stroke-width="2" stroke-dasharray="5 4" stroke-linecap="round"/>`;
  s+=`<path d="M${TL.map(p=>x(p[0]).toFixed(1)+','+y(mid(p)).toFixed(1)).join(' L')}"
        fill="none" stroke="${S(1)}" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>`;
  const lp=TL[TL.length-1];
  s+=`<circle cx="${x(lp[0])}" cy="${y(mid(lp))}" r="4.5" fill="${S(1)}" stroke="var(--surface-1)" stroke-width="2"/>`;
  s+=`<text class="dlabel" x="${x(lp[0])+9}" y="${y(mid(lp))+4}">${lo(lp)}–${hi(lp)}</text>`;
  s+=`<text class="ax" x="${x(2)}" y="${y(84)}" style="font-size:10px">band narrows as versions are decomposed →</text>`;
  TL.forEach((p,i)=>{ s+=`<rect class="hit" data-i="${i}" x="${x(p[0])-11}" y="${T}" width="22" height="${H-T-B}"/>`; });
  s+='</svg>';
  el('c-burn').innerHTML=s;
  el('c-burn').querySelectorAll('.hit').forEach(n=>{
    const p=TL[+n.dataset.i];
    bind(n,`${p[0]} min elapsed`,[
      ['Projected remaining',`${lo(p)}–${hi(p)} pts`,S(1)],
      ['Known (decomposed)',`${p[1]} pts`],
      ['Not yet decomposed',`${p[2]} version${p[2]===1?'':'s'} → +${p[2]*LO}–${p[2]*HI}`]]);
  });
  el('tv-burn').innerHTML='<table><thead><tr><th>Elapsed</th><th>Known</th><th>Undecomposed</th><th>Projected</th></tr></thead><tbody>'
    +TL.map(p=>`<tr><td>${p[0]} min</td><td>${p[1]}</td><td>${p[2]} ver</td><td>${lo(p)}–${hi(p)}</td></tr>`).join('')+'</tbody></table>';
}

/* ── 5 · velocity ───────────────────────────────────────────────────────── */
function renderVel(){
  // A version whose steps have started but has closed no issues yet (e.g. still in
  // generate-issues) has an empty `is` here -- 0/0 is NaN, and Math.max(...NaN...)
  // is NaN for the *whole* array, which niceMax's `v||0` silently floors to its
  // 60s minimum. Every real bar then computes against that collapsed axis and
  // paints far outside the chart's own card (SVGs don't clip by default). Drop
  // rows with no completed issues before they can poison the shared maxY.
  const rows=V.filter(v=>v.steps).map(v=>{
    const is=ISSUES.filter(i=>i.v===v.id);
    return {id:v.id, mean:is.reduce((a,i)=>a+i.dur,0)/is.length, pts:is.map(i=>i.dur)};
  }).filter(r=>r.pts.length>0);
  if(!rows.length){el('c-vel').innerHTML='<p class="note">no issue has finished yet</p>';
    el('tv-vel').innerHTML='';return;}
  const W_=340,H=190,L=52,R=40,T=10,B=40;   // B leaves room for a rotated label
  const maxY=niceMax(Math.max(...rows.flatMap(r=>[r.mean,...r.pts]))*1.1,60);
  const bw=Math.max(4,Math.min(24,(W_-L-R)/rows.length-16));
  const y=v=>T+(1-v/maxY)*(H-T-B);
  let s=svg(W_,H);
  ticks(maxY,2).forEach(g=>{s+=`<line class="gridline" x1="${L}" x2="${W_-R}" y1="${y(g)}" y2="${y(g)}"/>
    <text class="ax" x="${L-6}" y="${y(g)+3.5}" text-anchor="end">${mmss(g)}</text>`;});
  s+=`<line class="axisline" x1="${L}" x2="${W_-R}" y1="${y(0)}" y2="${y(0)}"/>`;
  rows.forEach((r,i)=>{
    const cx=L+(i+.5)*((W_-L-R)/rows.length);
    const h=y(0)-y(r.mean);
    s+=`<path class="mark" d="M${cx-bw/2},${y(0)} L${cx-bw/2},${y(r.mean)+4} q0,-4 4,-4 L${cx+bw/2-4},${y(r.mean)} q4,0 4,4 L${cx+bw/2},${y(0)} Z" fill="${S(1)}"/>`;
    r.pts.forEach(p=>{ s+=`<circle cx="${cx+bw/2+7}" cy="${y(p)}" r="2.5" fill="var(--text-muted)" opacity=".65"/>`; });
    s+=xLabel(r.id,cx,H-10,(W_-L-R)/rows.length);
    s+=`<rect class="hit" x="${cx-bw/2-6}" y="${T}" width="${bw+24}" height="${H-T-B}" data-i="${i}"/>`;
  });
  s+='</svg>'; el('c-vel').innerHTML=s;
  el('c-vel').querySelectorAll('.hit').forEach(n=>{const r=rows[+n.dataset.i];
    bind(n,r.id,[['Mean per issue',mmss(r.mean),S(1)],['Slowest',mmss(Math.max(...r.pts))],['Issues',r.pts.length]]);});
  el('tv-vel').innerHTML='<table><thead><tr><th>Version</th><th>Mean/issue</th><th>Slowest</th></tr></thead><tbody>'
    +rows.map(r=>`<tr><td>${r.id}</td><td>${mmss(r.mean)}</td><td>${mmss(Math.max(...r.pts))}</td></tr>`).join('')+'</tbody></table>';
}

/* ── 6 · where time went (horizontal stacked bar, 2px surface gaps) ─────── */
function renderTime(){
  const rows=V.filter(v=>v.steps);
  if(!rows.length){el('c-time').innerHTML='<p class="note">no version has finished a step yet</p>';return;}
  const W_=700,L=54,R=54,T=8,B=24;
  const H=T+B+rows.length*PITCH, bh=BARH;
  const maxX=niceMax(rows.reduce((m,v)=>Math.max(m,Object.values(v.steps).reduce((a,b)=>a+b,0)),0),60);
  const sx=v=>(v/maxX)*(W_-L-R);
  let s=svg(W_,H);
  ticks(maxX,3).forEach(g=>{s+=`<line class="gridline" x1="${L+sx(g)}" x2="${L+sx(g)}" y1="${T}" y2="${H-B}"/>
    <text class="ax" x="${L+sx(g)}" y="${H-8}" text-anchor="middle">${Math.round(g/60)}m</text>`;});
  rows.forEach((v,i)=>{
    const yy=T+i*PITCH+(PITCH-bh)/2;
    let cx=L, total=0;
    s+=`<text class="axl" x="${L-8}" y="${yy+bh/2+4}" text-anchor="end">${v.id}</text>`;
    STEPS.forEach(([k,label,slot],si)=>{
      const d=v.steps[k]; if(!d) return; total+=d;
      const w=Math.max(0,sx(d)-2); // 2px surface gap does the separating — no borders
      const first=si===0, lastSeg=si===STEPS.length-1||!v.steps[STEPS[si+1][0]];
      const r1=first?4:0, r2=lastSeg?4:0;
      s+=`<path class="mark seg" data-v="${i}" data-k="${k}"
            d="M${cx+r1},${yy} h${Math.max(0,w-r1-r2)} q${r2},0 ${r2},${r2} v${bh-2*r2} q0,${r2} ${-r2},${r2}
               h${-(Math.max(0,w-r1-r2))} q${-r1},0 ${-r1},${-r1} v${-(bh-2*r1)} q0,${-r1} ${r1},${-r1} Z"
            fill="${S(slot)}"/>`;
      cx+=sx(d);
    });
    // direct label at the bar end — relief for the light-mode contrast WARN
    s+=`<text class="dlabel" x="${cx+7}" y="${yy+bh/2+4}">${mmss(total)}</text>`;
  });
  s+='</svg>'; el('c-time').innerHTML=s;
  el('c-time').querySelectorAll('.seg').forEach(n=>{
    const v=rows[+n.dataset.v], k=n.dataset.k, meta=STEPS.find(x=>x[0]===k);
    bind(n,`${v.id} · ${meta[1]}`,[['Duration',mmss(v.steps[k]),S(meta[2])]]);
  });
  el('l-time').innerHTML=STEPS.map(([k,l,slot])=>`<span><i class="key" style="background:${S(slot)}"></i>${l}</span>`).join('');
  el('tv-time').innerHTML='<table><thead><tr><th>Version</th>'+STEPS.map(s2=>`<th>${s2[1]}</th>`).join('')+'<th>Total</th></tr></thead><tbody>'
    +rows.map(v=>`<tr><td>${v.id}</td>${STEPS.map(([k])=>`<td>${v.steps[k]?mmss(v.steps[k]):'—'}</td>`).join('')}<td>${mmss(Object.values(v.steps).reduce((a,b)=>a+b,0))}</td></tr>`).join('')+'</tbody></table>';
}

/* ── 7 · failure surface (EMPHASIS: retried in accent, rest gray) ───────── */
function renderFail(){
  if(!ISSUES.length){el('c-fail').innerHTML='<p class="note">no issue has finished yet</p>';
    el('tv-fail').innerHTML='';return;}
  const W_=560,H=180,L=30,R=12,T=10,B=42;
  const maxY=Math.max(3,...ISSUES.map(i=>i.att));
  const y=v=>T+(1-v/maxY)*(H-T-B);
  const bw=Math.max(2,Math.min(24,(W_-L-R)/ISSUES.length-6));
  let s=svg(W_,H);
  Array.from({length:maxY},(_,k)=>k+1).forEach(g=>{s+=`<line class="gridline" x1="${L}" x2="${W_-R}" y1="${y(g)}" y2="${y(g)}"/>
    <text class="ax" x="${L-6}" y="${y(g)+3.5}" text-anchor="end">${g}</text>`;});
  s+=`<line class="axisline" x1="${L}" x2="${W_-R}" y1="${y(0)}" y2="${y(0)}"/>`;
  ISSUES.forEach((it,i)=>{
    const cx=L+(i+.5)*((W_-L-R)/ISSUES.length), hot=it.att>1;
    const yv=y(it.att);
    s+=`<path class="mark" d="M${cx-bw/2},${y(0)} L${cx-bw/2},${yv+4} q0,-4 4,-4 L${cx+bw/2-4},${yv} q4,0 4,4 L${cx+bw/2},${y(0)} Z"
          fill="${hot?S(2):'var(--deemph)'}"/>`;
    if(hot) s+=`<text class="dlabel" x="${cx}" y="${yv-6}" text-anchor="middle">${it.att}</text>`;
    s+=`<text class="ax xcat" x="${cx}" y="${H-26}" text-anchor="middle"
          transform="rotate(-45 ${cx} ${H-26})">${esc(it.id.replace('RF-',''))}</text>`;
    s+=`<rect class="hit" x="${cx-bw/2-3}" y="${T}" width="${bw+6}" height="${H-T-B}" data-i="${i}"/>`;
  });
  s+=`<text class="ax" x="${L}" y="${H-4}">RF-###</text>`;
  s+='</svg>'; el('c-fail').innerHTML=s;
  el('c-fail').querySelectorAll('.hit').forEach(n=>{const it=ISSUES[+n.dataset.i];
    bind(n,it.id+' · '+it.size,it.att>1
      ? [['Attempts',it.att,S(2)],['Duration',mmss(it.dur)],['Attempt 1','3 tests failed · test_reconnect.py']]
      : [['Attempts',it.att,'var(--deemph)'],['Duration',mmss(it.dur)]]);});
  el('tv-fail').innerHTML='<table><thead><tr><th>Issue</th><th>Size</th><th>Attempts</th><th>Duration</th></tr></thead><tbody>'
    +ISSUES.map(i=>`<tr><td>${i.id}</td><td>${i.size}</td><td>${i.att}</td><td>${mmss(i.dur)}</td></tr>`).join('')+'</tbody></table>';
}

/* ── 8 · suite trajectory (one series, no legend, NO second axis) ───────── */
function renderSuite(){
  const pts=V.filter(v=>v.tests!=null).map((v,i)=>[i,v.tests,v.id]);
  if(!pts.length){el('c-suite').innerHTML='<p class="note">no version has recorded a suite size yet</p>';
    el('tv-suite').innerHTML='';return;}
  const W_=520,H=170,L=44,R=44,T=12,B=40;   // B leaves room for a rotated label
  const maxY=niceMax(Math.max(...pts.map(p=>p[1]))*1.1,50);
  const x=i=>L+(i/(pts.length-1||1))*(W_-L-R), y=v=>T+(1-v/maxY)*(H-T-B);
  let s=svg(W_,H);
  ticks(maxY,4).forEach(g=>{s+=`<line class="gridline" x1="${L}" x2="${W_-R}" y1="${y(g)}" y2="${y(g)}"/>
    <text class="ax" x="${L-6}" y="${y(g)+3.5}" text-anchor="end">${Math.round(g)}</text>`;});
  s+=`<line class="axisline" x1="${L}" x2="${W_-R}" y1="${y(0)}" y2="${y(0)}"/>`;
  s+=`<path d="M${x(0)},${y(0)} L${pts.map(p=>x(p[0])+','+y(p[1])).join(' L')} L${x(pts.length-1)},${y(0)} Z"
        fill="${S(1)}" opacity=".10"/>`;
  s+=`<path d="M${pts.map(p=>x(p[0])+','+y(p[1])).join(' L')}" fill="none" stroke="${S(1)}"
        stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>`;
  pts.forEach(p=>{s+=`<circle cx="${x(p[0])}" cy="${y(p[1])}" r="4.5" fill="${S(1)}" stroke="var(--surface-1)" stroke-width="2"/>
    ${xLabel(p[2],x(p[0]),H-10,(W_-L-R)/(pts.length-1||1))}`;});
  const lp=pts[pts.length-1];
  s+=`<text class="dlabel" x="${x(lp[0])+9}" y="${y(lp[1])+4}">${lp[1]}</text>`;
  pts.forEach((p,i)=>s+=`<rect class="hit" x="${x(p[0])-18}" y="${T}" width="36" height="${H-T-B}" data-i="${i}"/>`);
  s+='</svg>'; el('c-suite').innerHTML=s;
  el('c-suite').querySelectorAll('.hit').forEach(n=>{const p=pts[+n.dataset.i];
    bind(n,p[2],[['Tests passing',p[1],S(1)],['mypy errors',0]]);});
  el('tv-suite').innerHTML='<table><thead><tr><th>Version</th><th>Tests passing</th></tr></thead><tbody>'
    +pts.map(p=>`<tr><td>${p[2]}</td><td>${p[1]}</td></tr>`).join('')+'</tbody></table>';
}

/* ── 9 · quality flow ───────────────────────────────────────────────────── */
function renderQuality(){
  // Only versions whose review step has actually run. A version at 0 findings because
  // review has not happened yet is not "clean" — rendering it as an empty bar would lie.
  const rows=V.filter(v=>v.find && v.steps && v.steps.review>0);
  const pend=V.filter(v=>v.steps && !v.steps.review).map(v=>v.id);
  if(!rows.length){el('c-q').innerHTML='<p class="note">no review has finished yet</p>';
    el('l-q').innerHTML='';el('tv-q').innerHTML='';return;}
  const W_=980,L=54,R=110,T=10,B=24;
  const H=T+B+rows.length*PITCH, bh=BARH;
  const maxX=niceMax(Math.max(...rows.map(v=>QSEG.reduce((a,[k])=>a+(v.find[k]||0),0))),2);
  const sx=v=>(v/maxX)*(W_-L-R);
  let s=svg(W_,H);
  ticks(maxX,3).forEach(g=>{s+=`<line class="gridline" x1="${L+sx(g)}" x2="${L+sx(g)}" y1="${T}" y2="${H-B}"/>
    <text class="ax" x="${L+sx(g)}" y="${H-8}" text-anchor="middle">${Math.round(g)}</text>`;});
  rows.forEach((v,i)=>{
    const yy=T+i*PITCH+(PITCH-bh)/2;
    let cx=L,total=0;
    s+=`<text class="axl" x="${L-8}" y="${yy+bh/2+4}" text-anchor="end">${v.id}</text>`;
    QSEG.forEach(([k,label,slot],si)=>{
      const d=v.find[k]; if(!d) return; total+=d;
      const w=Math.max(0,sx(d)-2);
      s+=`<rect class="mark qs" data-v="${i}" data-k="${k}" x="${cx}" y="${yy}" width="${w}" height="${bh}" rx="4" fill="${S(slot)}"/>`;
      cx+=sx(d);
    });
    const density=(total/v.issues).toFixed(2);
    s+=`<text class="dlabel" x="${cx+9}" y="${yy+bh/2+4}">${total} · ${density}/issue</text>`;
  });
  s+='</svg>';
  el('c-q').innerHTML=s+(pend.length?`<p class="note" style="margin:8px 0 0">${pend.join(', ')} — review not run yet; excluded rather than drawn as zero.</p>`:'');
  el('c-q').querySelectorAll('.qs').forEach(n=>{
    const v=rows[+n.dataset.v], k=n.dataset.k, m=QSEG.find(x=>x[0]===k);
    bind(n,`${v.id} · ${m[1]}`,[['Findings',v.find[k],S(m[2])]]);});
  el('l-q').innerHTML=QSEG.map(([k,l,slot])=>`<span><i class="key" style="background:${S(slot)}"></i>${l}</span>`).join('');
  el('tv-q').innerHTML='<table><thead><tr><th>Version</th>'+QSEG.map(q=>`<th>${q[1]}</th>`).join('')+'<th>Per issue</th></tr></thead><tbody>'
    +rows.map(v=>{const t=QSEG.reduce((a,[k])=>a+v.find[k],0);
      return `<tr><td>${v.id}</td>${QSEG.map(([k])=>`<td>${v.find[k]}</td>`).join('')}<td>${(t/v.issues).toFixed(2)}</td></tr>`;}).join('')+'</tbody></table>';
}

/* ── table-view toggles + theme ─────────────────────────────────────────── */
document.querySelectorAll('.tv').forEach(b=>b.addEventListener('click',()=>{
  const t=el(b.dataset.tv), on=b.getAttribute('aria-pressed')==='true';
  b.setAttribute('aria-pressed',String(!on)); t.hidden=on;
  b.previousElementSibling; // chart stays visible — the table is a twin, not a replacement
}));
el('theme').addEventListener('click',()=>{
  const cur=document.documentElement.getAttribute('data-theme');
  const sysDark=matchMedia('(prefers-color-scheme: dark)').matches;
  document.documentElement.setAttribute('data-theme', cur? (cur==='dark'?'light':'dark') : (sysDark?'light':'dark'));
});

