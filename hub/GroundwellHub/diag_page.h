// GROUNDWELL hub diagnostics page (v0.12; v0.12.1 adds the crash on record): http://plants.local/diag, data from /api/diag every 2 s.
// v0.12: a health check of the whole chain (nodes -> Bluetooth -> hub -> Wi-Fi -> cloud) worked out in the page
// from the raw numbers in /api/diag and what the firmware expects ("expect"), plus the hub's own sensors.
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
.off,.l2{background:#c9573b}.warn,.l1{background:#d9a23b}.l0{background:#6f8f5e}.ln{background:#c8b8a6}
.small{font-size:.8rem;color:#8a7461}
.wide{background:#fff;border:1px solid #e6d6c4;border-radius:3px;overflow-x:auto}
.det td{background:#fbf5ee}
button{padding:.35rem .7rem;background:#6f8f5e;color:#fff;border:0;border-radius:3px;font-size:.85rem;cursor:pointer}
button.ghost{background:#fff;color:#6f8f5e;border:1px solid #6f8f5e}
#err{color:#c9573b}
.log td:first-child{white-space:nowrap;color:#8a7461}
.verdict{background:#fff;border:1px solid #e6d6c4;border-left:6px solid #6f8f5e;border-radius:3px;padding:.7rem .9rem;font-size:1.1rem}
.verdict.v1{border-left-color:#d9a23b}.verdict.v2{border-left-color:#c9573b}
.chain{display:flex;flex-wrap:wrap;align-items:stretch;gap:.3rem;margin:.6rem 0}
.link{flex:1 1 8.5rem;background:#fff;border:1px solid #e6d6c4;border-top:4px solid #6f8f5e;border-radius:3px;padding:.4rem .55rem}
.link.v1{border-top-color:#d9a23b}.link.v2{border-top-color:#c9573b}.link b{display:block;font-size:.85rem}
.arrow{align-self:center;color:#c8b8a6}
.fx td:nth-child(2){white-space:nowrap;color:#8a7461}
</style></head><body><main>
<div class=top><h1>groundwell hub · diagnostics</h1>
<span class=small><a href="/">Dashboard</a> · <a href="/update">Update</a> · <a href="/setup">Settings</a> · <button class=ghost onclick="snapshot()">Download snapshot</button></span></div>
<div class=now>Now: <b id=now>loading…</b> <span id=err class=small></span></div>
<h2>Health</h2>
<div id=verdict class=verdict>Checking…</div>
<div id=chain class=chain></div>
<div class=wide><table id=findings class=fx></table></div>
<p class=small>Timing, dropped packets and copies heard cover the last hour (<span id=win></span> so far). A node is expected to send every <span id=eivl></span> s; this hub carries node firmware <b id=efw></b>.</p>
<div class=grid>
<div class=card><h3>Hub</h3><div id=hub></div></div>
<div class=card><h3>Sensors wired to this hub</h3><div id=sens></div></div>
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
<p class=small>Updates every 2 s. "Download snapshot" saves everything on this page, the health findings included, as one file for debugging.</p>
</main><script>
const $=id=>document.getElementById(id);
const esc=x=>String(x==null?'':x).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
let last=null,health=null,samples=[],open={},paused=0;
function ago(s){if(s==null||s<0)return'never';if(s<90)return s+' s ago';if(s<5400)return Math.round(s/60)+' min ago';if(s<172800)return(s/3600).toFixed(1)+' h ago';return Math.round(s/86400)+' d ago'}
function dur(s){if(s==null||s<0)return'?';const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return(d?d+' d ':'')+(d||h?h+' h ':'')+m+' min'}
function mins(s){return s<90?Math.round(s)+' s':Math.round(s/60)+' min'}
function inS(s){return s<=0?'when it next sends':'in '+(s<5400?Math.round(s/60)+' min':(s/3600).toFixed(1)+' h')}
function kb(b){return(b/1024).toFixed(0)+' KB'}
function dot(ok,warn){return'<span class="dot'+(ok?(warn?' warn':''):' off')+'"></span>'}
function ld(l){return'<span class="dot l'+(l<0?'n':l)+'"></span>'}
function kv(rows){return'<table class=kv>'+rows.filter(r=>r).map(([k,v])=>'<tr><td>'+k+'</td><td>'+v+'</td></tr>').join('')+'</table>'}
function when(e,d){if(e.epoch)return new Date(e.epoch*1000).toLocaleTimeString();return'up '+dur(d.uptimeS-e.ageS)}
function rateN(key){if(samples.length<2)return null;const a=samples[0],b=samples[samples.length-1],dt=(b.t-a.t)/60;return dt>0?Math.round((b[key]-a[key])/dt):null}
function rate(key){const r=rateN(key);return r==null?'…':r+'/min'}
function pd(s){const o={};(s||'').split(';').forEach(p=>{const i=p.indexOf('=');if(i>0)o[p.slice(0,i)]=p.slice(i+1)});return o}
function vcmp(a,b){a=String(a).split(/[.-]/).map(x=>parseInt(x)||0);b=String(b).split(/[.-]/).map(x=>parseInt(x)||0);for(let i=0;i<3;i++)if((a[i]||0)!=(b[i]||0))return(a[i]||0)<(b[i]||0)?-1:1;return 0}
const DIAG={up:['Uptime',v=>dur(+v)],rst:['Last restart',v=>v],n:['Readings since start',v=>v],air:['Air sensor',v=>v],airfail:['Air read failures',v=>v],
 light:['Light sensor',v=>v],cal:['Factory calibration',v=>v=='1'?'yes':'no'],link:['Linked to a hub',v=>v=='1'?'yes':'no'],
 win:['Update window',v=>+v>0?Math.ceil(v/60)+' min left':'closed'],tx:['Radio power',v=>v+' dBm'],ivl:['Reading every',v=>v+' s'],
 con:['Connections since start',v=>v],authfail:['Failed link-key attempts',v=>v]};
function diagRows(n){
 if(!n.diag)return'<i>Not reported yet. Nodes on 0.11 or later report this at each check; press Refresh.</i>';
 const rows=n.diag.split(';').map(p=>{const i=p.indexOf('='),k=p.slice(0,i),v=p.slice(i+1);const f=DIAG[k];return f?[f[0],esc(f[1](v))]:[esc(k),esc(v)]});
 return kv(rows)+'<div class=small>Reported '+ago(n.diagS)+(n.diagS>=0?' (uptime then; add the time since)':'')+'</div>';
}
// ---- health: each check gives a level (0 fine, 1 needs attention, 2 problem; -1 a note) and what to do ----
function nodeHealth(n,d){
 const E=d.expect,f=[],g=pd(n.diag),loc=n.local,who=loc?'Hub sensors':'Node '+n.mac.slice(-5);
 const add=(l,what,todo)=>f.push({l,who,what,todo:todo||''});
 const med=n.dtMed/1000,mn=n.dtMin/1000,mx=n.dtMax/1000;
 let exp=E.nodeIvlS,five=false;
 if(loc&&d.hub.sens.fast)exp=5;
 if(!loc&&n.dtN>=3&&med>4&&med<6.5){exp=5;five=g.ivl!=='5'}       // 5 s: FAST mode, or firmware before 0.9
 if(!n.online)add(2,'Offline: nothing heard '+ago(n.seenS),loc?'The hub stopped reading its own sensors. Restart the hub; if it happens again, download a snapshot.':'Check its power and distance to the hub, then press its reset button and watch the event log for it.');
 else if(n.freshS>3*exp)add(2,'Heard, but no new reading for '+mins(n.freshS)+' (expected every '+exp+' s)','Press its reset button.');
 if(n.dtN>=3){
  const dev=Math.abs(med-exp)/exp,t='Readings every '+med.toFixed(1)+' s (middle of the last '+n.dtN+'), expected '+exp+' s';
  if(dev>.25)add(2,t,'The node is running a different interval than the hub expects: update it, or check REPORT_MS.');else if(dev>.1)add(1,t,'Its clock or schedule drifts: worth a reset.');
  if(mn<.75*exp)add(1,'Two readings came only '+mn.toFixed(1)+' s apart','It sent early or restarted: see its restarts and the event log.');
  if(mx>1.9*exp)add(1,'Longest gap '+mx.toFixed(0)+' s, about '+Math.round(mx/exp-1)+' readings missed',loc?'The hub was busy or restarting.':'See dropped readings and the link checks.');
 }
 const win=Math.min(E.hourS,n.firstS>=0?n.firstS:0),expN=win/exp,got=n.hRx,lost=n.hLost;
 // Percentages need enough readings to mean something (v0.12.2): under 20 expected, a miss or two is only a note.
 const few=expN<20,sure=(l)=>few?Math.min(l,got+lost>=6&&lost>=3?1:-1):l;
 if(expN>=4){const p=100*got/expN,t='Received '+got+' of about '+Math.round(expN)+' readings in the last '+mins(win)+' ('+Math.min(100,Math.round(p))+'%)';
  if(p<80)add(sure(2),t,loc?'Hub sensor readings are going missing: download a snapshot.':'Readings are going missing: weak link (below), a flat battery, or the node restarting.');else if(p<95)add(sure(1),t,'A few readings missing: watch the link checks.')}
 if(lost){const lp=100*lost/(got+lost);add(sure(lp>5?2:lp>1?1:-1),lost+' reading'+(lost>1?'s':'')+' dropped in the last '+mins(win)+' ('+lp.toFixed(1)+'%)'+(few?', early days':''),'Packets the hub never heard. Move the node or hub closer, raise TX_POWER_DBM to 8 on the node, or fit the hub\'s external antenna.')}
 if(n.hStale)add(n.hStale>got?1:-1,n.hStale+' old packet'+(n.hStale>1?'s':'')+' heard late in the last '+mins(win)+' (not counted as readings)','Normal for a node in its link window, which advertises nonstop. Outside the window it points at a radio repeating itself: worth a reset.');
 if(!loc&&got>=3){
  const cp=n.hAdv/got;
  if(cp<2)add(2,'Only '+cp.toFixed(1)+' copies of each reading heard (a burst sends '+E.copies+')','The link is at its edge: one more wall and readings drop. Move the node or hub closer, or raise TX_POWER_DBM.');
  else if(cp<5)add(1,'Thin link margin: '+cp.toFixed(1)+' copies of each reading heard (a burst sends '+E.copies+')','Fine for now; moving the node or hub closer gives it headroom.');
  if(n.hRssi<=-90)add(2,'Weak signal: '+n.hRssi+' dBm on average','Move the node or hub closer, or fit the hub\'s external antenna.');else if(n.hRssi<=-82)add(1,'Signal '+n.hRssi+' dBm on average: on the weak side','Fine for now; closer is better.');
 }
 if(!loc&&n.mv){const v=(n.mv/1000).toFixed(2);if(n.mv<E.minMv)add(2,'Battery '+v+' V: below '+(E.minMv/1000).toFixed(1)+' V, so firmware updates wait','Replace the coin cell.');else if(n.mv<2900)add(1,'Battery '+v+' V: getting low','Replace the coin cell soon.')}
 if(!loc){
  if(!n.conn)add(-1,'Broadcast-only test node: the hub can\'t check or update its firmware','Flash it over USB'+(E.nodeFw?' to '+E.nodeFw:'')+'.');
  else if(!n.fw||n.fw=='?'){if(d.uptimeS>900)add(1,'Firmware not checked yet','Press Refresh in its details, or its reset button.')}
  else if(E.bundled){const c=vcmp(n.fw,E.nodeFw);if(c<0)add(1,'Firmware '+n.fw+', expected '+E.nodeFw+(n.upd?' (updating now)':''),n.upd?'Leave it in range until it finishes.':'The hub updates it at its next check; see its Updates column.');else if(c>0)add(1,'Firmware '+n.fw+' is newer than the '+E.nodeFw+' this hub carries','Update the hub so the two match.')}
  if(five)add(1,'Sends every 5 s: firmware before 0.9, or in FAST mode','Update it; 0.9 and later send every 30 s, 0.13 and later every 60 s, and last far longer on a battery.');
  const st=n.st||'';if(/fail|stuck|press its reset|refused|no update service/.test(st))add(/stuck|update failed/.test(st)?2:1,'Update status: '+st,/press its reset/.test(st)?'Press the node\'s reset button once; the hub links it within a minute.':'See the event log.');
  if(n.restarts)add(n.restarts>3?1:-1,'Restarted '+n.restarts+' time'+(n.restarts>1?'s':'')+' since the hub started','Repeated restarts usually mean a sagging coin cell or a loose battery.');
 }
 if(n.diag){
  const N=+g.n||0,af=+g.airfail||0;
  if(g.air&&g.air!='none'&&N>=4){if(af>=N-1)add(2,'Air sensor ('+g.air+') not answering','Check its DATA, + and - wires; a bare 4-pin DHT11 needs a 10 kOhm pull-up to 3V3.');else if(af/N>.1)add(1,'Air sensor fails '+Math.round(100*af/N)+'% of reads','A DHT11 does this near radios; an SHT40 on I2C is steadier.')}
  if(g.cal==='0')add(1,'Not factory calibrated: sends raw counts','Run tools/factory_cal.py on it.');
  if(!loc&&g.link==='0')add(1,'Not linked to a hub','Press its reset button; the hub links it within a minute.');
  if(+g.authfail>0)add(g.link==='1'&&/linked to this hub|up to date|updat/.test(n.st||'')?-1:1,g.authfail+' refused link-key proof'+(+g.authfail>1?'s':'')+' since it started','It holds a key this hub can\'t prove: another hub linked it, or this hub\'s key changed. Press its reset button: in the 10 minutes after, this hub writes its own key.');
  if(/watchdog|lockup/.test(g.rst||''))add(1,'It last restarted from a '+g.rst,'A hang on the node: download a snapshot and report it.');
  if(g.air&&g.air!='none'&&(n.airS<0||n.airS>5*exp)&&N>=4&&af<N-1)add(1,'No air reading has reached the hub '+(n.airS<0?'yet':'for '+mins(n.airS)),'Readings are being held back as glitches: check the air sensor.');
 }
 if(d.cloud.on&&d.cloud.clock&&n.online){
  if(n.cloudS<0){if(got>0&&d.uptimeS>3*E.uploadS+60)add(1,'None of its readings has reached the cloud yet','See the cloud check.')}
  else if(n.cloudS>Math.max(900,5*exp))add(2,'Newest reading in the cloud is '+ago(n.cloudS),'See the cloud check.');
  else if(n.cloudS>3*E.uploadS+exp)add(1,'Newest reading in the cloud is '+ago(n.cloudS),'Uploads are running behind.');
 }
 return{l:f.reduce((m,x)=>Math.max(m,x.l),0),f,exp,med,mn,mx,win,expN,got,lost};
}
function evaluate(d){
 const E=d.expect,h=d.hub,s=h.sens,w=d.wifi,b=d.ble,c=d.cloud,out=[],nh={},tiles=[];
 const add=(l,who,what,todo)=>out.push({l,who,what,todo:todo||''});
 d.nodes.forEach(n=>{const r=nodeHealth(n,d);nh[n.i]=r;out.push(...r.f)});
 const radio=d.nodes.filter(n=>!n.local),wired=d.nodes.find(n=>n.local);
 // hub sensors
 if(s.on){
  if(!wired){if(d.uptimeS>60)add(2,'Hub sensors','Switched on, but no reading has come through','Restart the hub; if it stays this way, download a snapshot.')}
  if(s.cap<50)add(2,'Hub sensors','Capacitive probe reads '+s.cap+' (near 0)','Check its yellow wire on D0, red on D1 and black on GND.');
  else if(s.cap>4050)add(2,'Hub sensors','Capacitive probe reads full scale ('+s.cap+')','Probe unplugged, or its timer chip is an NE555 (needs a TLC555 at 3.3 V).');
  if(s.i2c&&!s.i2cFound)add(1,'Hub sensors','I2C ticked on the setup page, but no sensor answered on D4/D5','Check SDA on D4, SCL on D5, 3V3 and GND, then restart the hub (sensors are found at start).');
  if(s.dht&&s.air!='dht11'&&s.air!='none')add(-1,'Hub sensors','DHT11 ticked, but the '+s.air+' on I2C is used for air','');
 }
 if(wired){const r=nh[wired.i];r.f.push(...out.filter(x=>x.who=='Hub sensors'&&r.f.indexOf(x)<0));r.l=r.f.reduce((m,x)=>Math.max(m,x.l),0)}
 // Bluetooth
 const all=rateN('all');
 paused=b.scanning||b.job||/update/.test(d.now)?0:paused+1;
 if(paused>=3)add(2,'Bluetooth','Scan stopped and not resuming','Restart the hub. If it happens again, download a snapshot.');
 if(d.uptimeS>120&&all===0&&b.scanning)add(2,'Bluetooth','Hears nothing at all, from any device','The radio isn\'t receiving: check the antenna setting (USE_EXTERNAL_ANTENNA) and restart the hub.');
 if(d.uptimeS>120&&!radio.length)add(s.on?-1:1,'Bluetooth','No node heard yet',s.on?'Fine if the hub\'s own sensors are all you have.':'Power a node near the hub and press its reset button.');
 // hub
 if(!E.bundled)add(1,'Hub','This hub build carries no node firmware, so it can\'t update nodes','Use the GitHub build, or bundle it with tools/make_node_fw_h.py.');
 if(h.heapBlock<20480)add(2,'Hub','Largest free memory block '+kb(h.heapBlock)+': too small for a secure upload','Restart the hub and download a snapshot if it comes back.');
 else if(h.heapMin<50000)add(1,'Hub','Free memory dipped to '+kb(h.heapMin)+' (uploads need about 50 KB)','Watch it; a steady fall means a leak worth reporting.');
 if(/crash|watchdog|brownout/.test(h.reset))add(1,'Hub','Last restart: '+h.reset,h.reset=='brownout'?'The power supply sagged: try another USB cable or charger.':'Download a snapshot and report it.');
 if(h.trial)add(-1,'Hub','Firmware '+h.fw+' is on trial','It is kept after 10 minutes on Wi-Fi with a reading heard and an upload accepted.');
 if(h.crash&&h.crashMine)add(/crash|watchdog/.test(h.reset)?1:-1,'Hub','This firmware has a crash on record: '+h.crash.split(';')[0],'Download a snapshot and send it: it holds the crash addresses.');
 // Wi-Fi
 if(!w.up)add(2,'Wi-Fi',w.ssid?'Down'+(w.downS>=0?' since '+ago(w.downS):''):'No network saved','Readings queue in memory meanwhile. '+(w.ssid?'Check the router; the hub retries every 30 s.':'Set it up on the setup page.'));
 else if(w.rssi<=-88)add(2,'Wi-Fi','Weak signal: '+w.rssi+' dBm','Move the hub closer to a Wi-Fi point, or fit the external antenna.');
 else if(w.rssi<=-80)add(1,'Wi-Fi','Signal '+w.rssi+' dBm: on the weak side','Uploads may fail now and then.');
 if(w.ups>1)add(-1,'Wi-Fi','Reconnected '+(w.ups-1)+' time'+(w.ups>2?'s':'')+' since start','Normal if the router restarts on a schedule.');
 if(!w.updatePw)add(1,'Wi-Fi','No update password set','Set one on the setup page; updates stay off until you do.');
 // cloud
 if(!c.clock)add(2,'Cloud','Clock not set (no internet time yet)','Readings wait until the time is known: check the internet connection.');
 if(!c.on)add(1,'Cloud','Upload is off (no cloud URL)','Fill in the cloud URL on the setup page.');
 else{
  const why=c.code==401?' (wrong ingest key)':c.err?' ('+c.err+')':c.code&&c.code!=200?' (HTTP '+c.code+')':'';
  if(c.okS<0){if(d.uptimeS>3*E.uploadS)add(2,'Cloud','No upload has worked since the hub started'+why,c.code==401?'The ingest key on the setup page must match INGEST_KEY in Cloudflare.':'Check the cloud URL and the internet connection.')}
  else if(c.okS>900||c.streak>=3)add(2,'Cloud','Last upload that worked: '+ago(c.okS)+', '+c.streak+' failed since'+why,c.code==401?'The ingest key must match INGEST_KEY in Cloudflare.':'Check the internet connection and the Worker.');
  else if(c.okS>3*E.uploadS)add(1,'Cloud','Last upload that worked: '+ago(c.okS)+why,'One or two failures are normal; more is worth a look.');
  if(c.q>.9*c.qMax)add(2,'Cloud','Queue '+Math.round(100*c.q/c.qMax)+'% full: the oldest readings are about to be dropped','Get uploads working again.');
  else if(c.q>.5*c.qMax)add(1,'Cloud','Queue '+Math.round(100*c.q/c.qMax)+'% full','Uploads are falling behind.');
  if(c.oldestS>1800)add(2,'Cloud','The cloud is '+mins(c.oldestS)+' behind (oldest reading waiting)','Get uploads working again.');else if(c.oldestS>600)add(1,'Cloud','The cloud is '+mins(c.oldestS)+' behind','Uploads are falling behind.');
  if(c.ms>8000)add(1,'Cloud','The last upload took '+(c.ms/1000).toFixed(1)+' s','Slow internet or Worker; the Bluetooth scan pauses while it runs.');
 }
 const worst=k=>out.filter(x=>x.who==k||(k=='Nodes'&&/^Node /.test(x.who))).reduce((m,x)=>Math.max(m,x.l),0);
 const on=radio.filter(n=>n.online).length,rx=radio.reduce((a,n)=>a+nh[n.i].got,0),ex=radio.reduce((a,n)=>a+Math.max(nh[n.i].expN,0),0);
 tiles.push(['Nodes',radio.length?worst('Nodes'):(s.on?-1:1),radio.length?on+' of '+radio.length+' online'+(ex>=4?', '+Math.min(100,Math.round(100*rx/ex))+'% of readings in':''):'none heard']);
 tiles.push(['Bluetooth',worst('Bluetooth'),(b.scanning?'listening':b.job?'paused for a node':'paused')+(all!=null?', '+all+' adverts/min':'')]);
 if(s.on)tiles.push(['Hub sensors',worst('Hub sensors'),wired?'reading '+ago(s.lastS):'no reading yet']);
 tiles.push(['Hub',worst('Hub'),h.fw+', '+kb(h.heap)+' free']);
 tiles.push(['Wi-Fi',worst('Wi-Fi'),w.up?w.rssi+' dBm':'down']);
 tiles.push(['Cloud',worst('Cloud'),!c.on?'off':c.okS>=0?'upload OK '+ago(c.okS):'no upload yet']);
 out.sort((a,b)=>b.l-a.l);
 const l=out.reduce((m,x)=>Math.max(m,x.l),0),n1=out.filter(x=>x.l==1).length,n2=out.filter(x=>x.l==2).length;
 return{l,out,nh,tiles,text:l==0?'Healthy: every link from the nodes to the cloud is working as expected.':l==1?'Needs attention: '+n1+' thing'+(n1>1?'s':'')+' below, nothing broken.':'Problem: '+n2+' thing'+(n2>1?'s':'')+' not working'+(n1?', and '+n1+' to keep an eye on':'')+'.'};
}
function render(d){
 last=d;
 samples.push({t:d.uptimeS,all:d.ble.all,gw:d.ble.gw,fresh:d.ble.fresh});
 while(samples.length>2&&samples[samples.length-1].t-samples[0].t>60)samples.shift();
 $('now').textContent=d.now;
 const H=health=evaluate(d),E=d.expect;
 $('verdict').className='verdict v'+H.l;$('verdict').textContent=H.text;
 $('chain').innerHTML=H.tiles.map(([k,l,t],i)=>(i?'<span class=arrow>›</span>':'')+'<div class="link v'+Math.max(l,0)+'"><b>'+ld(l)+k+'</b><span class=small>'+esc(t)+'</span></div>').join('');
 const fs=H.out.filter(x=>x.l!=0);
 $('findings').innerHTML=fs.length?'<tr><th></th><th>Where</th><th>What</th><th>What to do</th></tr>'+fs.map(x=>'<tr><td>'+ld(x.l)+'</td><td>'+esc(x.who)+'</td><td>'+esc(x.what)+'</td><td class=small>'+esc(x.todo)+'</td></tr>').join(''):'<tr><td>Nothing to report.</td></tr>';
 $('win').textContent=mins(E.hourS);$('eivl').textContent=E.nodeIvlS;$('efw').textContent=E.bundled?E.nodeFw:'none';
 const h=d.hub,s=h.sens,w=d.wifi,b=d.ble,c=d.cloud;
 $('hub').innerHTML=kv([['Firmware',esc(h.fw)+(h.trial?' (on trial)':'')],['Slot',esc(h.slot)+', '+kb(h.sketch)+' of '+kb(h.slotSize)],
  ['Uptime',dur(d.uptimeS)],['Last restart',esc(h.reset)],['Free memory',kb(h.heap)+' (lowest '+kb(h.heapMin)+', largest block '+kb(h.heapBlock)+')'],
  ['Node firmware carried',h.nodeFw?esc(h.nodeFw)+' ('+kb(h.nodeFwLen)+')':'none'],h.note?['Last update',esc(h.note)]:null,
  h.crash?['Crash on record',esc(h.crash)+' <span class=small>('+(h.crashMine?'this firmware':'another build')+')</span>']:null,
  ['Clock',c.clock?new Date(d.epoch*1000).toLocaleString():dot(false)+'not set yet (no internet time)']]);
 $('sens').innerHTML=s.on?kv([['Address',esc(s.mac)+' (shown as a node)'],['Fitted','capacitive probe'+(s.dht?', DHT11':'')+(s.ldr?', photoresistor':'')+(s.i2c?', I2C bus':'')],
  ['Air',esc(s.air)+(s.airFail?' · '+s.airFail+' failed reads':'')],['Light',esc(s.light)+(s.uv?' + UV':'')+(s.light=='ldr'?' (adc '+s.ldrAdc+')':'')],
  ['Capacitive probe','raw '+s.cap],['Factory calibration',s.cal?'air '+s.calAir+', water '+s.calWater:dot(false,true)+'not yet (tools/factory_cal.py)'],
  ['Readings',s.n+', last '+ago(s.lastS)+(s.dtMs?' ('+(s.dtMs/1000).toFixed(1)+' s after the one before)':'')+(s.fast?' · FAST':'')]]):'<span class=small>None. Tick them on the <a href="/setup">setup page</a> if probes are wired to the hub.</span>';
 $('wifi').innerHTML=kv([['State',w.up?dot(true)+'connected':dot(false)+(w.downS>=0?'down since '+ago(w.downS):'not connected')],['Network',esc(w.ssid)||'none saved'],
  w.up?['Address',esc(w.ip)+' · '+esc(w.host)+(w.mdns?'':' (mDNS off)')]:null,w.up?['Signal',w.rssi+' dBm, channel '+w.ch]:null,
  w.up?['Access point',esc(w.bssid)]:null,['Hub MAC',esc(w.mac)],['Connections since start',w.ups],
  ['Setup hotspot',w.hotspot?dot(true,true)+esc(w.hsName)+', '+w.hsClients+' connected':'closed'],
  ['Updates',(w.updatePw?'password set':dot(false)+'no update password yet')+(w.arduinoOta?' · Arduino IDE port on':'')]]);
 $('ble').innerHTML=kv([['Scan',b.scanning?dot(true)+'listening (100 ms interval, 50 ms window)':dot(false,true)+'paused'+(b.job?' for a node job':'')],
  b.job?['Node job','node '+esc(b.jobNode)+', running '+b.jobS+' s']:null,['Adverts, all devices',b.all+' · '+rate('all')],
  ['GROUNDWELL packets',b.gw+' · '+rate('gw')],['New readings',b.fresh+' · '+rate('fresh')],
  ['Nodes',d.nodes.filter(n=>!n.local).length+' heard, '+d.nodes.filter(n=>!n.local&&n.online).length+' online'],
  ['Device list','emptied every 10 s ('+b.clears+' times)']]);
 $('cloud').innerHTML=kv([['Upload',c.on?esc(c.url):dot(false)+'off (no URL)'],
  c.on?['Last try',c.lastS<0?'not yet':(c.code==200?dot(true):dot(false))+'HTTP '+c.code+', '+ago(c.lastS)+' ('+c.ms+' ms)']:null,
  c.on?['Last success',ago(c.okS)+(c.streak?' · '+c.streak+' failed since':'')]:null,c.err?['Last error',esc(c.err)]:null,['Uploads',c.ok+' OK, '+c.fail+' failed'],
  ['Readings sent',c.sent],['Waiting to send',c.q+' soil (of '+c.qMax+'), '+c.c+' air, '+c.l+' light'],
  ['Behind by',c.oldestS>=0?mins(c.oldestS)+' (oldest waiting reading)':c.q?'?':'nothing waiting']]);
 $('ncount').textContent='('+d.nodes.length+')';
 let t='<tr><th>Node</th><th>Health</th><th>State</th><th>Last hour</th><th>Timing</th><th>Link</th><th>Battery</th><th>Firmware</th><th>Updates</th><th></th></tr>';
 if(!d.nodes.length)t+='<tr><td colspan=10>No nodes heard yet.</td></tr>';
 d.nodes.forEach(n=>{
  const r=H.nh[n.i],top=r.f.filter(x=>x.l>0).sort((a,b)=>b.l-a.l)[0];
  t+='<tr><td><span class=mono>'+esc(n.mac)+'</span><div class=small>'+esc(n.type)+(n.conn?', connectable':'')+'<br>'+esc(n.probes)+'</div></td>'+
   '<td>'+ld(r.l)+(r.l==2?'problem':r.l==1?'needs attention':'healthy')+(top?'<div class=small>'+esc(top.what)+'</div>':'')+'</td>'+
   '<td>'+dot(n.online)+(n.online?'online':'offline')+'<div class=small>heard '+ago(n.seenS)+'<br>new reading '+ago(n.freshS)+'<br>first heard '+ago(n.firstS)+(n.cloudS>=0?'<br>in the cloud '+ago(n.cloudS):'')+'</div></td>'+
   '<td>'+(r.expN>=1?n.hRx+' of ~'+Math.round(r.expN):n.hRx)+' readings<div class=small>'+n.hLost+' dropped'+(n.hStale?', '+n.hStale+' old':'')+(n.local?'':'<br>'+(n.hRx?(n.hAdv/n.hRx).toFixed(1):'?')+' copies heard each')+'<br>since start: '+n.rx+' in, '+n.lost+' lost ('+(n.lossX10/10).toFixed(1)+'%)</div></td>'+
   '<td>'+(n.dtN?r.med.toFixed(1)+' s':'?')+'<div class=small>expected '+r.exp+' s'+(n.dtN?'<br>range '+r.mn.toFixed(1)+' to '+r.mx.toFixed(1)+' s<br>last '+(n.dtLast/1000).toFixed(1)+' s':'')+'</div></td>'+
   '<td>'+(n.local?'wired':n.hRssi+' dBm')+(n.local?'':'<div class=small>avg, last hour<br>'+n.rssiMin+' to '+n.rssiMax+' since start</div>')+'</td>'+
   '<td>'+(n.local?'USB':n.mv?(n.mv/1000).toFixed(2)+' V':'?')+'<div class=small>'+(n.air?'air ':'')+(n.lux?'light ':'')+(n.uvOk?'UV':'')+'</div></td>'+
   '<td>'+(esc(n.fw)||'?')+'<div class=small>'+(n.local?'the hub\'s own':d.expect.bundled?'expected '+esc(d.expect.nodeFw):'')+'</div></td>'+
   '<td>'+(esc(n.st)||'not checked yet')+(n.upd?' ('+n.pct+'%)':'')+(n.local?'':'<div class=small>'+n.checks+' checks, last '+ago(n.checkS)+'<br>next '+inS(n.nextS)+(n.fails?'<br>'+n.fails+' failures in a row':'')+(n.small?'<br>20-byte packets':'')+'</div>')+'</td>'+
   '<td><button class=ghost onclick="tog('+n.i+')">'+(open[n.i]?'Hide':'Details')+'</button></td></tr>';
  if(open[n.i])t+='<tr class=det><td colspan=10><b>Health checks</b>'+(r.f.length?'<table class=fx>'+r.f.map(x=>'<tr><td>'+ld(x.l)+'</td><td>'+esc(x.what)+'</td><td class=small>'+esc(x.todo)+'</td></tr>').join('')+'</table>':'<div>All fine.</div>')+
   '<b>What the node reports about itself</b> '+(n.local?'':'<button onclick="refresh('+n.i+')">Refresh</button> ')+'<span class=small id=rf'+n.i+'></span>'+diagRows(n)+'</td></tr>';
 });
 $('nodes').innerHTML=t;
 let p='<tr><th>Probe</th><th>Node</th><th>Kind</th><th>Raw</th><th>Moisture</th><th>Dry / wet</th><th>Last stored</th></tr>';
 if(!d.probes.length)p+='<tr><td colspan=7>No probes yet.</td></tr>';
 d.probes.forEach(q=>{const s=q.fcal?100:1;p+='<tr><td>'+esc(q.name)+'</td><td class=mono>'+esc(q.node.slice(-8))+'</td><td>'+esc(q.kind)+'</td><td>'+q.raw+(q.fcal?' (probe %, x100)':'')+'</td><td>'+q.pct+'%</td><td>'+(q.dry/s)+' / '+(q.wet/s)+'</td><td>'+ago(q.qS)+'</td></tr>'});
 $('probes').innerHTML=p;
 $('log').innerHTML=d.log.length?d.log.map(e=>'<tr><td>'+when(e,d)+'</td><td>'+esc(e.t)+'</td></tr>').join(''):'<tr><td>Nothing yet.</td></tr>';
}
function tog(i){open[i]=!open[i];if(last)render(last)}
async function refresh(i){const s=$('rf'+i);try{const r=await fetch('/api/diag/check',{method:'POST',body:new URLSearchParams({i})});if(s)s.textContent=r.ok?'The hub checks this node at its next burst (within about a minute).':'Refused ('+r.status+')'}catch(e){if(s)s.textContent='No answer'}}
function snapshot(){if(!last)return;const a=document.createElement('a');a.href=URL.createObjectURL(new Blob([JSON.stringify({diag:last,health:health&&{verdict:health.text,findings:health.out}},null,1)],{type:'application/json'}));a.download='groundwell-diag-'+new Date().toISOString().replace(/[:.]/g,'-')+'.json';document.body.appendChild(a);a.click();a.remove()}
async function tick(){try{const r=await fetch('/api/diag',{cache:'no-store'});if(!r.ok)throw'HTTP '+r.status;render(await r.json());$('err').textContent=''}catch(e){$('err').textContent='hub not answering ('+e+'), retrying'}setTimeout(tick,2000)}
tick();
</script></body></html>)rawliteral";
