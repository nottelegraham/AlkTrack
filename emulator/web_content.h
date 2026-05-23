#pragma once

static const char WEB_HTML[] = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AlkTrack Monitor</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,system-ui,sans-serif;background:#0a1628;color:#e0e8f0;min-height:100vh;padding:24px}
.container{max-width:720px;margin:0 auto}
h1{font-size:1.1rem;color:#64748b;font-weight:500;margin-bottom:24px;letter-spacing:0.5px}
.result-card{background:#111d33;border:1px solid #1e3a5f;border-radius:12px;padding:32px;text-align:center;margin-bottom:20px}
.result-value{font-size:4rem;font-weight:700;color:#22d3ee;line-height:1}
.result-unit{font-size:1.2rem;color:#64748b;margin-top:4px}
.phase{display:inline-block;padding:4px 12px;border-radius:20px;font-size:0.8rem;font-weight:600;text-transform:uppercase;letter-spacing:0.5px;margin-top:12px}
.phase-sleeping{background:#1e293b;color:#94a3b8}
.phase-ref_aerate,.phase-tank_aerate{background:#0c4a6e;color:#38bdf8;animation:pulse 2s infinite}
.phase-startup{background:#422006;color:#fb923c}
.phase-idle{background:#14532d;color:#4ade80}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.6}}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:20px}
.stat{background:#111d33;border:1px solid #1e3a5f;border-radius:8px;padding:16px}
.stat-label{font-size:0.75rem;color:#64748b;text-transform:uppercase;letter-spacing:0.5px}
.stat-value{font-size:1.4rem;font-weight:600;color:#e2e8f0;margin-top:4px}
.chart-card{background:#111d33;border:1px solid #1e3a5f;border-radius:8px;padding:16px;margin-bottom:20px}
.chart-title{font-size:0.75rem;color:#64748b;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:12px}
svg{width:100%;height:80px;display:block}
.controls{background:#111d33;border:1px solid #1e3a5f;border-radius:8px;padding:16px}
.btn-row{display:flex;gap:8px;flex-wrap:wrap}
button{background:#1e3a5f;border:1px solid #2d5a8e;color:#e0e8f0;padding:8px 16px;border-radius:6px;cursor:pointer;font-size:0.85rem;font-weight:500;transition:background 0.15s}
button:hover{background:#2d5a8e}
button.danger{background:#7f1d1d;border-color:#991b1b}
button.danger:hover{background:#991b1b}
.cal-row{display:flex;gap:8px;margin-top:10px}
input{background:#0a1628;border:1px solid #1e3a5f;color:#e0e8f0;padding:8px 12px;border-radius:6px;width:100px;font-size:0.85rem}
input:focus{outline:none;border-color:#22d3ee}
.meta{font-size:0.7rem;color:#475569;text-align:center;margin-top:16px}
</style>
</head>
<body>
<div class="container">
<h1>AlkTrack Alkalinity Monitor</h1>
<div class="result-card">
  <div class="result-value" id="result">--</div>
  <div class="result-unit">dKH</div>
  <span class="phase phase-startup" id="phase">startup</span>
</div>
<div class="grid">
  <div class="stat"><div class="stat-label">Ref pH</div><div class="stat-value" id="refph">--</div></div>
  <div class="stat"><div class="stat-label">Tank pH</div><div class="stat-value" id="tankph">--</div></div>
  <div class="stat"><div class="stat-label">Temperature</div><div class="stat-value" id="temp">--</div></div>
  <div class="stat"><div class="stat-label">Anchor</div><div class="stat-value" id="anchor">--</div></div>
</div>
<div class="chart-card">
  <div class="chart-title">Trend (last 24 readings)</div>
  <svg id="chart" viewBox="0 0 680 80" preserveAspectRatio="none"></svg>
</div>
<div class="controls">
  <div class="btn-row">
    <button onclick="sendCmd('RUN')">Run Cycle</button>
    <button onclick="sendCmd('STATUS')">Status</button>
    <button onclick="sendCmd('PRIME')">Prime</button>
    <button class="danger" onclick="sendCmd('PUMPS_OFF')">PUMPS OFF</button>
  </div>
  <div class="cal-row">
    <input type="number" id="calval" step="0.1" min="3" max="20" placeholder="dKH">
    <button onclick="sendCal()">Set Anchor</button>
  </div>
</div>
<div class="meta">Auto-refreshes every 2s | Simulation mode</div>
</div>
<script>
function update(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('result').textContent=d.lastResult>0?d.lastResult.toFixed(2):'--';
    document.getElementById('refph').textContent=d.refPH>0?d.refPH.toFixed(4):'--';
    document.getElementById('tankph').textContent=d.tankPH>0?d.tankPH.toFixed(4):'--';
    document.getElementById('temp').textContent=d.temp>0?d.temp.toFixed(1)+' C':'--';
    document.getElementById('anchor').textContent=d.anchor>0?d.anchor.toFixed(2)+' dKH':'--';
    var el=document.getElementById('phase');
    el.textContent=d.phase.replace('_',' ');
    el.className='phase phase-'+d.phase;
    drawChart(d.history);
  }).catch(()=>{});
}
function drawChart(data){
  var svg=document.getElementById('chart');
  if(!data||data.length<2){svg.innerHTML='<text x="340" y="45" text-anchor="middle" fill="#475569" font-size="12">Waiting for data...</text>';return;}
  var min=Math.min(...data)-0.5,max=Math.max(...data)+0.5;
  if(max-min<1){min-=0.5;max+=0.5;}
  var w=680,h=80,pts=[];
  for(var i=0;i<data.length;i++){
    var x=(i/(data.length-1))*w;
    var y=h-((data[i]-min)/(max-min))*h;
    pts.push(x.toFixed(1)+','+y.toFixed(1));
  }
  svg.innerHTML='<polyline points="'+pts.join(' ')+'" fill="none" stroke="#22d3ee" stroke-width="2" stroke-linejoin="round"/>';
  for(var i=0;i<data.length;i++){
    var x=(i/(data.length-1))*w;
    var y=h-((data[i]-min)/(max-min))*h;
    svg.innerHTML+='<circle cx="'+x.toFixed(1)+'" cy="'+y.toFixed(1)+'" r="3" fill="#22d3ee"/>';
  }
}
function sendCmd(cmd){fetch('/api/command',{method:'POST',body:cmd}).then(()=>setTimeout(update,500));}
function sendCal(){var v=document.getElementById('calval').value;if(v)sendCmd('CAL '+v);}
setInterval(update,2000);
update();
</script>
</body>
</html>
)rawhtml";
