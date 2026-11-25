// GROUNDWELL hub diagnostics page (v0.11): http://plants.local/diag, data from /api/diag every 2 s.
#pragma once
static const char DIAG_PAGE[] PROGMEM = R"rawliteral(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>groundwell hub · diagnostics</title>
<style>
body{font-family:system-ui,sans-serif;background:#fdf6ee;color:#5a4636;margin:0;padding:1rem;font-size:14px}
main{max-width:72rem;margin:0 auto}
h1{font-size:1.35rem;color:#6f8f5e;font-weight:600;margin:.2rem 0}
h2{font-size:1rem;color:#6f8f5e;margin:1.4rem 0 .4rem;font-weight:600}
a{color:#6f8f5e}
.top{display:flex;flex-wrap:wrap;gap:.6rem;align-items:baseline;justify-content:space-between}
.now{background:#fff;border:1px solid #e6d6c4;border-radius:3px;padding:.6rem .8rem;margin:.6rem 0;font-size:1.05rem}
.now b{color:#6f8f5e}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(17rem,1fr));gap:.8rem}
.card{background:#fff;border:1px solid #e6d6c4;border-radius:3px;padding:.5rem .7rem}
.card h3{margin:.1rem 0 .3rem;font-size:.85rem;text-transform:uppercase;letter-spacing:.04em;color:#8a7461}
table{border-collapse:collapse;width:100%}
td,th{padding:.25rem .45rem;text-align:left;vertical-align:top;border-bottom:1px solid #f1e6da}
th{font-weight:600;color:#8a7461;font-size:.8rem}
.kv td:first-child{color:#8a7461;white-space:nowrap;width:40%}
.mono{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:.85rem}
.dot{display:inline-block;width:.55rem;height:.55rem;border-radius:50%;margin-right:.35rem;background:#6f8f5e}
.off{background:#c9573b}.warn{background:#d9a23b}
.small{font-size:.8rem;color:#8a7461}
.wide{background:#fff;border:1px solid #e6d6c4;border-radius:3px;overflow-x:auto}
.det td{background:#fbf5ee}
button{padding:.35rem .7rem;background:#6f8f5e;color:#fff;border:0;border-radius:3px;font-size:.85rem;cursor:pointer}
button.ghost{background:#fff;color:#6f8f5e;border:1px solid #6f8f5e}
#err{color:#c9573b}
.log td:first-child{white-space:nowrap;color:#8a7461}
</style></head><body><main>
<div class=top><h1>groundwell hub · diagnostics</h1>
<span class=small><a href="/">Dashboard</a> · <a href="/update">Update</a> · <a href="/setup">Settings</a> · <button class=ghost onclick="snapshot()">Download snapshot</button></span></div>
<div class=now>Now: <b id=now>loading…</b> <span id=err class=small></span></div>
<div class=grid>
<div class=card><h3>Hub</h3><div id=hub></div></div>
<div class=card><h3>Wi-Fi</h3><div id=wifi></div></div>
<div class=card><h3>Bluetooth</h3><div id=ble></div></div>
<div class=card><h3>Cloud</h3><div id=cloud></div></div>
</div>
<h2>Nodes <span id=ncount class=small></span></h2>
<div class=wide><table id=nodes></table></div>
<h2>Probes</h2>
<div class=wide><table id=probes></table></div>
<h2>Event log <span class=small>(newest first, last 60)</span></h2>
<div class=wide><table id=log class=log></table></div>
<p class=small>Updates every 2 s. "Download snapshot" saves everything on this page as one file, for debugging.</p>
</main><script>
const $=id=>document.getElementById(id);
const esc=x=>String(x==null?'':x).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
let last=null,samples=[],open={};
function ago(s){if(s==null||s<0)return'never';if(s<90)return s+' s ago';if(s<5400)return Math.round(s/60)+' min ago';if(s<172800)return(s/3600).toFixed(1)+' h ago';return Math.round(s/86400)+' d ago'}
function dur(s){if(s==null||s<0)return'?';const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return(d?d+' d ':'')+(d||h?h+' h ':'')+m+' min'}
function inS(s){return s<=0?'when it next sends':'in '+(s<5400?Math.round(s/60)+' min':(s/3600).toFixed(1)+' h')}
function kb(b){return(b/1024).toFixed(0)+' KB'}
function dot(ok,warn){return'<span class="dot'+(ok?(warn?' warn':''):' off')+'"></span>'}
function kv(rows){return'<table class=kv>'+rows.filter(r=>r).map(([k,v])=>'<tr><td>'+k+'</td><td>'+v+'</td></tr>').join('')+'</table>'}
function when(e,d){if(e.epoch)return new Date(e.epoch*1000).toLocaleTimeString();return'up '+dur(d.uptimeS-e.ageS)}
function rate(key){if(samples.length<2)return'…';const a=samples[0],b=samples[samples.length-1],dt=(b.t-a.t)/60;return dt>0?Math.round((b[key]-a[key])/dt)+'/min':'…'}
const DIAG={up:['Uptime',v=>dur(+v)],rst:['Last restart',v=>v],n:['Readings since start',v=>v],air:['Air sensor',v=>v],airfail:['Air read failures',v=>v],
 light:['Light sensor',v=>v],cal:['Factory calibration',v=>v=='1'?'yes':'no'],link:['Linked to a hub',v=>v=='1'?'yes':'no'],
 win:['Update window',v=>+v>0?Math.ceil(v/60)+' min left':'closed'],tx:['Radio power',v=>v+' dBm'],ivl:['Reading every',v=>v+' s'],
 con:['Connections since start',v=>v],authfail:['Failed link-key attempts',v=>v]};
function diagRows(n){
 if(!n.diag)return'<i>Not reported yet. Nodes on 0.11 or later report this at each check; press Refresh.</i>';
 const rows=n.diag.split(';').map(p=>{const i=p.indexOf('='),k=p.slice(0,i),v=p.slice(i+1);const f=DIAG[k];return f?[f[0],esc(f[1](v))]:[esc(k),esc(v)]});
 return kv(rows)+'<div class=small>Reported '+ago(n.diagS)+(n.diagS>=0?' (uptime then; add the time since)':'')+'</div>';
}
function render(d){
 last=d;
 samples.push({t:d.uptimeS,all:d.ble.all,gw:d.ble.gw,fresh:d.ble.fresh});
 while(samples.length>2&&samples[samples.length-1].t-samples[0].t>60)samples.shift();
 $('now').textContent=d.now;
 const h=d.hub,w=d.wifi,b=d.ble,c=d.cloud;
 $('hub').innerHTML=kv([['Firmware',esc(h.fw)+(h.trial?' (on trial)':'')],['Slot',esc(h.slot)+', '+kb(h.sketch)+' of '+kb(h.slotSize)],
  ['Uptime',dur(d.uptimeS)],['Last restart',esc(h.reset)],['Free memory',kb(h.heap)+' (lowest '+kb(h.heapMin)+', largest block '+kb(h.heapBlock)+')'],
  ['Node firmware carried',h.nodeFw?esc(h.nodeFw)+' ('+kb(h.nodeFwLen)+')':'none'],h.note?['Last update',esc(h.note)]:null,
  ['Clock',c.clock?new Date(d.epoch*1000).toLocaleString():dot(false)+'not set yet (no internet time)']]);
 $('wifi').innerHTML=kv([['State',w.up?dot(true)+'connected':dot(false)+(w.downS>=0?'down since '+ago(w.downS):'not connected')],['Network',esc(w.ssid)||'none saved'],
  w.up?['Address',esc(w.ip)+' · '+esc(w.host)+(w.mdns?'':' (mDNS off)')]:null,w.up?['Signal',w.rssi+' dBm, channel '+w.ch]:null,
  w.up?['Access point',esc(w.bssid)]:null,['Hub MAC',esc(w.mac)],['Connections since start',w.ups],
  ['Setup hotspot',w.hotspot?dot(true,true)+esc(w.hsName)+', '+w.hsClients+' connected':'closed'],
  ['Updates',(w.updatePw?'password set':dot(false)+'no update password yet')+(w.arduinoOta?' · Arduino IDE port on':'')]]);
 $('ble').innerHTML=kv([['Scan',b.scanning?dot(true)+'listening (100 ms interval, 50 ms window)':dot(false,true)+'paused'+(b.job?' for a node job':'')],
  b.job?['Node job','node '+esc(b.jobNode)+', running '+b.jobS+' s']:null,['Adverts, all devices',b.all+' · '+rate('all')],
  ['GROUNDWELL packets',b.gw+' · '+rate('gw')],['New readings',b.fresh+' · '+rate('fresh')],
  ['Nodes',d.nodes.length+' heard, '+d.nodes.filter(n=>n.online).length+' online']]);
 $('cloud').innerHTML=kv([['Upload',c.on?esc(c.url):dot(false)+'off (no URL)'],
  c.on?['Last try',c.lastS<0?'not yet':(c.code==200?dot(true):dot(false))+'HTTP '+c.code+', '+ago(c.lastS)+' ('+c.ms+' ms)']:null,
  c.on?['Last success',ago(c.okS)]:null,c.err?['Last error',esc(c.err)]:null,['Uploads',c.ok+' OK, '+c.fail+' failed'],
  ['Readings sent',c.sent],['Waiting to send',c.q+' soil (of '+c.qMax+'), '+c.c+' air, '+c.l+' light']]);
 $('ncount').textContent='('+d.nodes.length+')';
 let t='<tr><th>Node</th><th>State</th><th>Traffic</th><th>Every</th><th>Signal</th><th>Battery</th><th>Firmware</th><th>Updates</th><th></th></tr>';
 if(!d.nodes.length)t+='<tr><td colspan=9>No nodes heard yet.</td></tr>';
 d.nodes.forEach(n=>{
  const stale=n.online&&n.freshS>Math.max(90,3*n.ivlMs/1000);
  t+='<tr><td><span class=mono>'+esc(n.mac)+'</span><div class=small>'+esc(n.type)+(n.conn?', connectable':'')+'<br>'+esc(n.probes)+'</div></td>'+
   '<td>'+dot(n.online,stale)+(n.online?'online':'offline')+'<div class=small>heard '+ago(n.seenS)+'<br>new reading '+ago(n.freshS)+'<br>first heard '+ago(n.firstS)+'</div></td>'+
   '<td>'+n.rx+' readings<div class=small>'+n.adv+' adverts<br>'+n.lost+' lost ('+(n.lossX10/10).toFixed(1)+'%)<br>packet #'+n.pid+', '+n.restarts+' restarts</div></td>'+
   '<td>'+(n.ivlMs?(n.ivlMs/1000).toFixed(1)+' s':'?')+'</td>'+
   '<td>'+n.rssi+' dBm<div class=small>'+n.rssiMin+' to '+n.rssiMax+'</div></td>'+
   '<td>'+(n.mv?(n.mv/1000).toFixed(2)+' V':'?')+'<div class=small>'+(n.air?'air ':'')+(n.lux?'light ':'')+(n.uvOk?'UV':'')+'</div></td>'+
   '<td>'+(esc(n.fw)||'?')+'</td>'+
   '<td>'+(esc(n.st)||'not checked yet')+(n.upd?' ('+n.pct+'%)':'')+'<div class=small>'+n.checks+' checks, last '+ago(n.checkS)+'<br>next '+inS(n.nextS)+(n.fails?'<br>'+n.fails+' failures in a row':'')+(n.small?'<br>20-byte packets':'')+'</div></td>'+
   '<td><button class=ghost onclick="tog('+n.i+')">'+(open[n.i]?'Hide':'Details')+'</button></td></tr>';
  if(open[n.i])t+='<tr class=det><td colspan=9><b>What the node reports about itself</b> <button onclick="refresh('+n.i+')">Refresh</button> <span class=small id=rf'+n.i+'></span>'+diagRows(n)+'</td></tr>';
 });
 $('nodes').innerHTML=t;
 let p='<tr><th>Probe</th><th>Node</th><th>Kind</th><th>Raw</th><th>Moisture</th><th>Dry / wet</th><th>Last stored</th></tr>';
 if(!d.probes.length)p+='<tr><td colspan=7>No probes yet.</td></tr>';
 d.probes.forEach(q=>{const s=q.fcal?100:1;p+='<tr><td>'+esc(q.name)+'</td><td class=mono>'+esc(q.node.slice(-8))+'</td><td>'+esc(q.kind)+'</td><td>'+q.raw+(q.fcal?' (probe %, x100)':'')+'</td><td>'+q.pct+'%</td><td>'+(q.dry/s)+' / '+(q.wet/s)+'</td><td>'+ago(q.qS)+'</td></tr>'});
 $('probes').innerHTML=p;
 $('log').innerHTML=d.log.length?d.log.map(e=>'<tr><td>'+when(e,d)+'</td><td>'+esc(e.t)+'</td></tr>').join(''):'<tr><td>Nothing yet.</td></tr>';
}
function tog(i){open[i]=!open[i];if(last)render(last)}
async function refresh(i){const s=$('rf'+i);try{const r=await fetch('/api/diag/check',{method:'POST',body:new URLSearchParams({i})});if(s)s.textContent=r.ok?'The hub checks this node at its next burst (within about 30 s).':'Refused ('+r.status+')'}catch(e){if(s)s.textContent='No answer'}}
function snapshot(){if(!last)return;const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([JSON.stringify(last,null,1)],{type:'application/json'}));a.download='groundwell-diag-'+new Date().toISOString().replace(/[:.]/g,'-')+'.json';document.body.appendChild(a);a.click();a.remove()}
async function tick(){try{const r=await fetch('/api/diag',{cache:'no-store'});if(!r.ok)throw'HTTP '+r.status;render(await r.json());$('err').textContent=''}catch(e){$('err').textContent='hub not answering ('+e+'), retrying'}setTimeout(tick,2000)}
tick();
</script></body></html>)rawliteral";
