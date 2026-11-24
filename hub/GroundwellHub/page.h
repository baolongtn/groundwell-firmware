// Hub dashboard page (served at /). Plain HTML + JS, no external files.
#pragma once
const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Groundwell Hub</title>
<style>
:root{--bg:#f3f1ea;--card:#fffdf8;--ink:#1f2a1f;--mute:#6b7466;--line:#e2ddd2;--track:#ebe6db;--grid:#ebe7dd;
--ok:#3f8f4f;--low:#c27323;--dry:#b8432f;--wet:#2f6fb8;
--s0:#2f7d5b;--s1:#c4572f;--s2:#3b6fb6;--s3:#a37a1c;--s4:#8a4fa8;--s5:#1f8f9e;--s6:#b03a6a;--s7:#5d6b2a}
@media (prefers-color-scheme:dark){:root{--bg:#121612;--card:#1b211b;--ink:#e8ede4;--mute:#97a191;
--line:#2c352c;--track:#283028;--grid:#252d25;--ok:#6cc27d;--low:#e3a05a;--dry:#e2735f;--wet:#6aa5e8;
--s0:#5fc08f;--s1:#ec8a63;--s2:#79a6ec;--s3:#d9b04e;--s4:#b98ad4;--s5:#55c3cf;--s6:#e07aa3;--s7:#a9b86a}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{display:flex;justify-content:space-between;align-items:baseline;flex-wrap:wrap;gap:6px 14px;padding:22px 16px 6px;max-width:1100px;margin:auto}
h1{font-size:22px;margin:0;letter-spacing:-.01em}
.stat{font-size:13px;color:var(--mute);font-variant-numeric:tabular-nums}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--ok);margin-right:6px;vertical-align:1px}
.dot.off{background:var(--dry)}.dot.warn{background:var(--low)}
.panel{max-width:1100px;margin:8px auto 0;padding:0 16px}
.box{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px 16px 10px}
.ctop{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px}
.ctop h2{font-size:15px;margin:0}
.ctrls{display:flex;gap:8px;flex-wrap:wrap}
.seg{display:inline-flex;border:1px solid var(--line);border-radius:9px;overflow:hidden}
.seg button{border:0;border-radius:0;background:transparent;padding:5px 11px;color:var(--mute)}
.seg button.on{background:var(--ink);color:var(--card)}
.legend{display:flex;flex-wrap:wrap;gap:6px 16px;font-size:13px;margin-top:10px}
.legend span{display:inline-flex;align-items:center;gap:6px;cursor:pointer;user-select:none}
.legend span.off{opacity:.35}
.sw{width:14px;height:3px;border-radius:2px;display:inline-block;flex:none}
#chartwrap{position:relative;margin-top:6px}
#chart{width:100%;height:280px;display:block;touch-action:pan-y}
.axis{font-size:11px;fill:var(--mute);font-variant-numeric:tabular-nums}
#tip{position:absolute;top:8px;pointer-events:none;background:var(--card);border:1px solid var(--line);border-radius:8px;padding:6px 9px;font-size:12px;box-shadow:0 4px 14px rgba(0,0,0,.12);display:none;white-space:nowrap;font-variant-numeric:tabular-nums}
#tip b{display:block;margin-bottom:3px;font-weight:600}
#tip div{display:flex;align-items:center;gap:6px}
main{display:grid;grid-template-columns:repeat(auto-fill,minmax(270px,1fr));gap:14px;padding:14px 16px 32px;max-width:1100px;margin:auto}
.empty-msg{grid-column:1/-1;color:var(--mute);text-align:center;padding:30px 0}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px}
.card.stale{opacity:.6}
.top{display:flex;justify-content:space-between;align-items:center;gap:8px}
.nm{display:flex;align-items:center;gap:8px;min-width:0}
.name{font-weight:600;font-size:16px;cursor:pointer;border-bottom:1px dashed transparent;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.name:hover{border-bottom-color:var(--mute)}
.chip{font-size:12px;font-weight:600;padding:2px 9px;border-radius:99px;color:var(--card);white-space:nowrap}
.pct{font-size:46px;font-weight:700;letter-spacing:-.02em;margin:6px 0 4px;font-variant-numeric:tabular-nums;line-height:1.1}
.pct small{font-size:18px;color:var(--mute);font-weight:500;margin-left:2px}
.bar{position:relative;height:10px;background:var(--track);border-radius:99px}
.fill{height:100%;border-radius:99px;transition:width .6s,background .6s}
.mark{position:absolute;top:-3px;bottom:-3px;width:2px;background:var(--ink);opacity:.45}
.meta{display:flex;justify-content:space-between;gap:8px;font-size:12px;color:var(--mute);margin-top:7px;font-variant-numeric:tabular-nums}
.node{font-size:12px;color:var(--mute);margin-top:4px;font-variant-numeric:tabular-nums}
details{margin-top:10px;font-size:13px;border-top:1px solid var(--line);padding-top:8px}
summary{cursor:pointer;color:var(--mute)}
.row{display:flex;gap:8px;align-items:center;margin-top:8px;flex-wrap:wrap}
button{font:inherit;font-size:13px;padding:6px 10px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--ink);cursor:pointer}
button:hover{border-color:var(--mute)}
input{font:inherit;font-size:13px;width:70px;padding:5px 6px;border-radius:8px;border:1px solid var(--line);background:var(--bg);color:var(--ink)}
.hint{color:var(--mute);font-size:12px;margin-top:8px}
.tabs{max-width:1100px;margin:6px auto 0;padding:0 16px;display:flex;gap:4px;border-bottom:1px solid var(--line)}
.tabs button{border:0;border-bottom:2px solid transparent;border-radius:0;background:transparent;padding:8px 14px;font-size:14px;color:var(--mute);margin-bottom:-1px}
.tabs button.on{color:var(--ink);border-bottom-color:var(--ink);font-weight:600}
body.user .adm{display:none!important}
</style></head><body>
<header><h1>Groundwell</h1>
  <div><div class="stat" id="status"><span class="dot off"></span>Connecting...</div><div class="stat" id="cloud"></div></div></header>
<nav class="tabs" id="modeTabs"><button data-v="user">User</button><button data-v="admin">Admin</button></nav>
<section class="panel"><div class="box">
  <div class="ctop"><h2>Moisture over time</h2>
    <div class="ctrls">
      <div class="seg" id="rangeSeg"><button data-v="live" class="on">Last hour</button><button data-v="day">24 h</button></div>
      <div class="seg" id="unitSeg"><button data-v="pct" class="on">%</button><button data-v="raw">Raw</button></div>
    </div></div>
  <div class="legend" id="legend"></div>
  <div id="chartwrap"><svg id="chart"></svg><div id="tip"></div></div>
</div></section>
<main id="grid"><div class="empty-msg">Waiting for the first node broadcast...</div></main>
<script>
const grid=document.getElementById('grid'),st=document.getElementById('status'),cl=document.getElementById('cloud'),svgC=document.getElementById('chart'),tip=document.getElementById('tip');
let cfg=[],byId={},chart=null,cards={},range='live',unit='pct',hidden={},geo=null,live=null,mode='user',pin='';
try{mode=localStorage.getItem('gwMode')||'user';pin=sessionStorage.getItem('gwPin')||''}catch(e){}
function setMode(m){mode=m;document.body.classList.toggle('user',m!=='admin');
  document.querySelectorAll('#modeTabs button').forEach(x=>x.classList.toggle('on',x.dataset.v===m));
  try{localStorage.setItem('gwMode',m)}catch(e){}}
document.querySelectorAll('#modeTabs button').forEach(b=>b.onclick=async()=>{
  if(b.dataset.v==='admin'&&live&&live.pin&&!pin){
    const p=prompt('Admin PIN');if(p===null)return;
    const r=await fetch('/api/admin',{method:'POST',body:new URLSearchParams({pin:p})}).catch(()=>null);
    if(!r||!r.ok){alert('Wrong PIN');return}
    pin=p;try{sessionStorage.setItem('gwPin',p)}catch(e){}
  }
  setMode(b.dataset.v)});
setMode(mode);
const SC=i=>'var(--s'+(i%8)+')';
const tone=(p,t)=>p<t?'var(--dry)':p<t+15?'var(--low)':p>90?'var(--wet)':'var(--ok)';
const word=(p,t)=>p<t?'Thirsty':p<t+15?'Drying out':p>90?'Soaked':'Happy';
function pctF(raw,s){if(!s||s.dry===s.wet)return 0;return Math.max(0,Math.min(100,(s.dry-raw)*100/(s.dry-s.wet)))}
function pctOf(raw,s){return Math.round(pctF(raw,s))}
function ago(s){return s<60?s+' s ago':s<3600?Math.round(s/60)+' min ago':Math.round(s/3600)+' h ago'}
function fmtUp(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return d?d+'d '+h+'h':h?h+'h '+m+'m':m+'m'}
const clock=(t,sec)=>new Date(t).toLocaleTimeString([],sec?{hour:'numeric',minute:'2-digit',second:'2-digit'}:{hour:'numeric',minute:'2-digit'});
function esc(s){const d=document.createElement('div');d.textContent=s;return d.innerHTML}
function seg(id,cb){document.querySelectorAll('#'+id+' button').forEach(b=>b.onclick=()=>{document.querySelectorAll('#'+id+' button').forEach(x=>x.classList.toggle('on',x===b));cb(b.dataset.v)})}
seg('rangeSeg',v=>{range=v;loadChart()});
seg('unitSeg',v=>{unit=v;drawChart()});

function makeCard(s){
  const c=document.createElement('div');c.className='card';
  c.innerHTML='<div class="top"><span class="nm"><i class="sw" style="background:'+SC(s.id)+'"></i><span class="name" title="Click to rename"></span></span><span class="chip"></span></div>'+
    '<div class="pct"><span class="v">--</span><small>%</small></div>'+
    '<div class="bar"><div class="fill"></div><div class="mark" title="Thirsty level"></div></div>'+
    '<div class="meta"><span class="raw"></span><span class="kind"></span></div><div class="node air"></div><div class="node link"></div>'+
    '<details class="adm"><summary>Calibrate and alert level</summary>'+
    '<div class="row"><button data-a="dry">Set dry = now</button><button data-a="wet">Set wet = now</button></div>'+
    '<div class="row">Dry <input class="i-dry" type="number" step="any"> Wet <input class="i-wet" type="number" step="any"> <span class="unit-l">raw</span></div>'+
    '<div class="row">Thirsty below <input class="i-thr" type="number" min="0" max="100"> % <button data-a="save">Save</button></div>'+
    '<div class="hint">Dry: probe in air or bone-dry soil. Wet: soil right after a full watering. Wait one reading (5 s) after moving the probe.</div></details>';
  const q=k=>c.querySelector(k);
  q('.name').onclick=()=>{const n=prompt('Plant name',byId[s.id].name);if(n!==null&&n.trim())post({id:s.id,name:n.trim()})};
  c.querySelectorAll('button').forEach(b=>b.onclick=()=>{
    const a=b.dataset.a,cur=byId[s.id],nm=cur.name,sc=cur.fcal?100:1;
    if(a==='save')post({id:s.id,dry:Math.round(q('.i-dry').value*sc),wet:Math.round(q('.i-wet').value*sc),thr:q('.i-thr').value});
    else if(confirm('Set the '+a+' point for '+nm+' to the current reading ('+(cur.fcal?(cur.raw/100).toFixed(1)+' probe %':cur.raw)+')?'))post({id:s.id,action:a});
  });
  return c;
}
function legend(){
  const L=document.getElementById('legend');
  L.innerHTML=cfg.map(s=>'<span data-i="'+s.id+'" class="'+(hidden[s.id]?'off':'')+'"><i class="sw" style="background:'+SC(s.id)+'"></i>'+esc(s.name)+'</span>').join('');
  L.querySelectorAll('span').forEach(e=>e.onclick=()=>{const i=+e.dataset.i;hidden[i]=!hidden[i];legend();drawChart()});
}
function update(d){
  live=d;
  if(d.pin&&!pin&&mode==='admin')setMode('user');
  const sig=cfg.map(s=>s.id+':'+s.name).join('|'),nsig=d.sensors.map(s=>s.id+':'+s.name).join('|');
  cfg=d.sensors;byId={};cfg.forEach(s=>byId[s.id]=s);
  if(cfg.length&&grid.querySelector('.empty-msg'))grid.innerHTML='';
  cfg.forEach(s=>{if(!cards[s.id]){cards[s.id]=makeCard(s);grid.appendChild(cards[s.id])}});
  if(sig!==nsig)legend();
  cfg.forEach(s=>{
    const c=cards[s.id],q=k=>c.querySelector(k),col=tone(s.pct,s.thr),stale=s.ageS>150;
    c.classList.toggle('stale',stale);
    q('.name').textContent=s.name;q('.v').textContent=s.pct;
    q('.fill').style.width=s.pct+'%';q('.fill').style.background=col;
    q('.mark').style.left='calc('+s.thr+'% - 1px)';
    const ch=q('.chip');ch.textContent=stale?'No signal':word(s.pct,s.thr);ch.style.background=stale?'var(--mute)':col;
    q('.raw').textContent=s.fcal?'probe '+(s.raw/100).toFixed(1)+'%':'raw '+s.raw;q('.kind').textContent=s.fcal?'Capacitive probe, factory calibrated':s.kind+' probe';
    q('.unit-l').textContent=s.fcal?'probe %':'raw';
    q('.air').textContent=(s.air?'Air '+(s.temp*9/5+32).toFixed(1)+' °F ('+s.temp+' °C) · '+s.rh+'% RH':'Air: no reading')+(s.luxOk?' · Light '+Number(s.lux).toLocaleString()+' lux':'')+(s.uv!=null?' · UV '+s.uv:'');
    q('.link').textContent='Node '+s.node.slice(-5)+' · '+s.volt+' V · '+s.brssi+' dBm · '+s.loss+'% lost · '+ago(s.ageS);
    const open=q('details').open;
    const sc=s.fcal?100:1;
    [['.i-dry',s.dry/sc],['.i-wet',s.wet/sc],['.i-thr',s.thr]].forEach(([k,v])=>{const e=q(k);if(!open||e.value==='')e.value=v});
  });
  st.innerHTML='<span class="dot"></span>Hub '+d.ip+' · Wi-Fi '+d.rssi+' dBm · up '+fmtUp(d.uptime);
  const c=d.cloud;
  if(!c.enabled)cl.innerHTML='<span class="dot warn"></span>Cloud upload off';
  else if(c.ageS<0)cl.innerHTML='<span class="dot warn"></span>Cloud: waiting for first upload ('+c.queued+' queued, last HTTP '+c.code+')';
  else cl.innerHTML='<span class="dot'+(c.ageS>150?' off':'')+'"></span>Cloud: synced '+ago(c.ageS)+' · '+c.sent+' sent · '+c.queued+' queued';
}
function drawChart(){
  if(!chart||!cfg.length)return;
  const W=Math.max(svgC.clientWidth,280),H=280,L=unit==='pct'?40:48,R=12,T=10,B=26,pw=W-L-R,ph=H-T-B;
  svgC.setAttribute('viewBox','0 0 '+W+' '+H);
  const iv=chart.intervalMs,tEnd=chart.t-chart.ageMs,win=range==='live'?3600000:86400000,t0=tEnd-win;
  const big=cfg.some(s=>s.fcal&&!hidden[s.id]),ymax=unit==='pct'?100:big?10000:4095,Y=v=>T+ph-v/ymax*ph,X=t=>L+(t-t0)/win*pw;
  const val=(raw,s)=>unit==='pct'?pctF(raw,s):raw;
  let n=0,h='';
  (unit==='pct'?[0,25,50,75,100]:big?[0,2500,5000,7500,10000]:[0,1000,2000,3000,4000]).forEach(v=>{h+='<line x1="'+L+'" x2="'+(W-R)+'" y1="'+Y(v)+'" y2="'+Y(v)+'" stroke="var(--grid)"/><text class="axis" x="'+(L-6)+'" y="'+(Y(v)+4)+'" text-anchor="end">'+v+(unit==='pct'?'%':'')+'</text>'});
  const step=range==='live'?(W<500?1200000:600000):(W<500?21600000:14400000);
  for(let t=Math.ceil(t0/step)*step;t<=tEnd;t+=step){const x=X(t);h+='<line x1="'+x+'" x2="'+x+'" y1="'+T+'" y2="'+(T+ph)+'" stroke="var(--grid)"/><text class="axis" x="'+x+'" y="'+(H-7)+'" text-anchor="middle">'+clock(t)+'</text>'}
  cfg.forEach(s=>{const arr=chart.series[s.id]||[];n=Math.max(n,arr.length);if(hidden[s.id]||arr.length<2)return;
    const k0=arr.length;const pts=arr.map((raw,k)=>X(tEnd-(k0-1-k)*iv).toFixed(1)+','+Y(val(raw,s)).toFixed(1)).join(' ');
    h+='<polyline points="'+pts+'" fill="none" stroke="'+SC(s.id)+'" stroke-width="2" stroke-linejoin="round"/>'});
  if(n<2)h+='<text class="axis" x="'+(L+pw/2)+'" y="'+(T+ph/2)+'" text-anchor="middle">Collecting data...</text>';
  h+='<g id="hov"></g>';svgC.innerHTML=h;geo={L,pw,T,ph,iv,tEnd,t0,win,X,Y,val};
}
function hover(ev){
  if(!geo||!chart)return;const r=svgC.getBoundingClientRect(),x=ev.clientX-r.left,g=geo;
  if(x<g.L){unhover();return}
  const t=g.t0+(x-g.L)/g.pw*g.win,back=Math.round((g.tEnd-t)/g.iv),tx=g.X(g.tEnd-back*g.iv);
  let h='<line x1="'+tx+'" x2="'+tx+'" y1="'+g.T+'" y2="'+(g.T+g.ph)+'" stroke="var(--mute)" stroke-dasharray="3 3"/>',tt='<b>'+clock(g.tEnd-back*g.iv,range==='live')+'</b>',any=false;
  cfg.forEach(s=>{const arr=chart.series[s.id]||[],k=arr.length-1-back;if(hidden[s.id]||k<0||k>=arr.length)return;any=true;const v=g.val(arr[k],s);
    h+='<circle cx="'+tx+'" cy="'+g.Y(v)+'" r="3.5" fill="'+SC(s.id)+'" stroke="var(--card)" stroke-width="1.5"/>';
    tt+='<div><i class="sw" style="background:'+SC(s.id)+'"></i>'+esc(s.name)+': '+Math.round(v)+(unit==='pct'?'%':'')+'</div>'});
  if(!any){unhover();return}
  document.getElementById('hov').innerHTML=h;tip.innerHTML=tt;tip.style.display='block';
  const w=tip.offsetWidth;tip.style.left=(tx+12+w>r.width?tx-12-w:tx+12)+'px';
}
function unhover(){const g=document.getElementById('hov');if(g)g.innerHTML='';tip.style.display='none'}
svgC.addEventListener('pointermove',hover);svgC.addEventListener('pointerleave',unhover);
window.addEventListener('resize',()=>{drawChart();unhover()});
async function tick(){try{const r=await fetch('/api/live',{cache:'no-store'});update(await r.json());drawChart()}catch(e){st.innerHTML='<span class="dot off"></span>Hub offline, retrying...'}}
async function loadChart(){try{const r=await fetch('/api/history'+(range==='live'?'?range=live':''),{cache:'no-store'});const d=await r.json();d.t=Date.now();chart=d;drawChart()}catch(e){}}
async function post(o){if(pin)o.pin=pin;
  try{const r=await fetch('/api/config',{method:'POST',body:new URLSearchParams(o)});
    if(r.status===403){pin='';try{sessionStorage.removeItem('gwPin')}catch(e){}alert('Admin PIN needed: open the Admin tab again.');setMode('user')}}catch(e){}
  await tick();loadChart()}
(async()=>{await tick();loadChart()})();
setInterval(()=>{tick();loadChart()},5000);
</script></body></html>)HTML";
