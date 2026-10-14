#pragma once

// The browser viewer, served at "/". Kept dependency-free (no libraries, no external requests).
namespace cc::web {

inline constexpr const char kViewerPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>ZeroDisplay</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 96 96'%3E%3Cpath d='M18 22H78M18 74H78' stroke='%230078D7' stroke-width='14' stroke-linecap='round'/%3E%3Cpath d='M74 27L22 69' stroke='%2360CDFF' stroke-width='8' stroke-linecap='round'/%3E%3C/svg%3E">
<style>
html,body{margin:0;height:100%;background:#000;color:#eef;font:14px "Segoe UI",system-ui,sans-serif;overflow:hidden}
video,canvas{position:absolute;inset:0;width:100%;height:100%;object-fit:contain;background:#000}
canvas{display:none}
#bar{position:absolute;left:0;right:0;top:0;padding:10px 14px;display:flex;gap:14px;align-items:center;
 background:linear-gradient(#000d,#0000);transition:opacity .4s}
#bar.hide{opacity:0}
.dim{color:#9aa3b5}
.logo{width:20px;height:20px;flex:none}
button{background:#0078d7;color:#fff;border:0;border-radius:4px;padding:8px 14px;font:inherit;cursor:pointer}
#tap{position:absolute;left:50%;bottom:12%;transform:translateX(-50%);font-size:16px;padding:12px 22px}
#pin{position:absolute;inset:0;display:none;align-items:center;justify-content:center;background:#000c}
#pin form{background:#1b1e26;border-radius:6px;padding:20px 22px;max-width:320px;margin:16px}
#pin input{font:22px monospace;letter-spacing:6px;width:9ch;padding:8px 10px;border-radius:6px;border:1px solid #445;background:#0d0f14;color:#fff}
</style></head><body>
<video id="v" muted playsinline autoplay disableremoteplayback></video>
<canvas id="c"></canvas>
<div id="bar"><svg class="logo" viewBox="0 0 96 96" aria-hidden="true"><defs><linearGradient id="zg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#8BE1FF"/><stop offset="1" stop-color="#0063B8"/></linearGradient></defs><path d="M18 22H78M18 74H78" stroke="url(#zg)" stroke-width="13" stroke-linecap="round" fill="none"/><path d="M74 27L22 69" stroke="#60CDFF" stroke-width="6" stroke-linecap="round"/></svg><b>ZeroDisplay</b><span id="name" class="dim"></span><span id="st" class="dim">Connecting…</span>
<span style="flex:1"></span><button id="fs">Full screen</button></div>
<button id="tap">&#x1F50A; Tap for sound</button>
<div id="pin"><form id="pf"><b>PIN</b><p id="pm" class="dim">Type the PIN shown in ZeroDisplay on the sharing PC (Settings, Network).</p>
<input id="pi" inputmode="numeric" autocomplete="off" maxlength="6" pattern="[0-9]*"> <button>Watch</button></form></div>
<script>
"use strict";
const $=id=>document.getElementById(id), v=$('v'), c=$('c');
const useWC = window.isSecureContext && 'VideoDecoder' in window && 'AudioWorkletNode' in window;
let ws, cfg=null, ms=null, vsb=null, asb=null, vq=[], aq=[], dec=null, g=null, needKey=true;
let actx=null, anode=null, soundOn=false, lastData=0, frames=0, fps=0, lastFpsAt=performance.now();
// Latency: clock offset to the server (best = smallest round trip), capture time of what is on screen.
let clk=null, bestRtt=1e9, baseMs=null, lat=[], latShown='';
const srvNow=()=>clk===null?null:performance.now()+clk;
function addLat(captureMs,shownAt){ if(clk===null||captureMs==null)return; const l=shownAt+clk-captureMs; if(l>-50&&l<10000)lat.push(l); }
const status=t=>$('st').textContent=t;

// The sharing PIN: remembered in this browser for this PC (its address); a PIN in the link (#pin=, from the QR code
// on the PC's screen) is kept and taken out of the address bar.
const getPin=()=>{try{return localStorage.getItem('ccpin')||'';}catch(e){return '';}};
const setPin=p=>{try{if(p)localStorage.setItem('ccpin',p);else localStorage.removeItem('ccpin');}catch(e){}};
{const h=/[#&]pin=(\d{6})/.exec(location.hash);if(h){setPin(h[1]);history.replaceState(null,'',location.pathname+location.search);}}
function askPin(msg){ $('pm').textContent=msg; $('pin').style.display='flex'; $('tap').style.display='none'; $('pi').value=''; $('pi').focus(); }
$('pf').onsubmit=e=>{e.preventDefault(); const p=$('pi').value.trim(); if(!/^\d{6}$/.test(p)){$('pm').textContent='The PIN has 6 digits.';return;}
  setPin(p); typed=true; $('pin').style.display='none'; status('Checking the PIN…'); if(ws&&ws.readyState===1)ws.send('pin:'+p); };
let typed=false;   // the PIN being checked was typed now (else: the remembered one)
function onPin(m){
  if(m.ok){ $('pin').style.display='none'; if(!soundOn)$('tap').style.display=''; ws.send(useWC?'webcodecs':'mse'); status('Waiting for video…'); bestRtt=1e9; clk=null; for(let i=0;i<5;i++)setTimeout(ping,200*i); return; }
  if(m.need==='wait'){ status('Too many wrong PINs, trying again in a minute'); $('pin').style.display='none'; setTimeout(()=>{if(ws&&ws.readyState===1)ws.send('pin:'+getPin());},60000); return; }
  const wrong=m.need==='wrong', mine=typed; typed=false;
  if(wrong) setPin('');
  status('PIN needed'); askPin((wrong?(mine?'Wrong PIN. ':'The sharing PC has a new PIN. '):'')+'Type the PIN shown in ZeroDisplay on the sharing PC (Settings, Network).');
}
function connect(){
  ws=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws');
  ws.binaryType='arraybuffer';
  ws.onopen=()=>{ws.send('pin:'+getPin());status('Connecting…');};
  ws.onclose=()=>{status('Disconnected, reconnecting…');reset();setTimeout(connect,1000);};
  ws.onmessage=e=>{
    if(typeof e.data==='string'){const m=JSON.parse(e.data);
      if(m.type==='pin')onPin(m);
      else if(m.type==='config')onConfig(m);
      else if(m.type==='pong'){const now=performance.now(),rtt=now-m.c;if(rtt<bestRtt){bestRtt=rtt;clk=m.s-(m.c+rtt/2);}}
      else if(m.type==='base')baseMs=m.ms;
      return;}
    onBinary(new Uint8Array(e.data));
  };
}
function ping(){ if(ws&&ws.readyState===1)ws.send('ping '+performance.now().toFixed(3)); }
function onShown(now,meta){   // MSE: every frame the compositor shows
  if(baseMs!==null)addLat(baseMs+meta.mediaTime*1000,meta.expectedDisplayTime||now);
  if(v.requestVideoFrameCallback)v.requestVideoFrameCallback(onShown);
}
if(v.requestVideoFrameCallback)v.requestVideoFrameCallback(onShown);
function reset(){
  if(dec){try{dec.close()}catch(e){}} dec=null; needKey=true;
  if(ms){try{v.removeAttribute('src');v.load()}catch(e){}} ms=vsb=asb=null; vq=[]; aq=[]; cfg=null;
}
function onConfig(m){
  const changed=!cfg||cfg.codec!==m.codec||cfg.width!==m.width||cfg.height!==m.height;
  cfg=m; $('name').textContent=m.name||'';
  if(changed){ if(useWC) setupWC(); else setupMSE(); }
}

// ---- WebCodecs (secure contexts): decode each frame the moment it arrives ----
function setupWC(){
  if(dec){try{dec.close()}catch(e){}}
  v.style.display='none'; c.style.display='block'; g=c.getContext('2d',{alpha:false,desynchronized:true});
  dec=new VideoDecoder({output:f=>{
      if(c.width!==f.displayWidth||c.height!==f.displayHeight){c.width=f.displayWidth;c.height=f.displayHeight;}
      g.drawImage(f,0,0); addLat(f.timestamp/1000,performance.now()); f.close(); frames++;
    }, error:e=>{status('Decoder error: '+e.message);needKey=true;setTimeout(setupWC,100);}});
  dec.configure({codec:cfg.codec,optimizeForLatency:true,hardwareAcceleration:'prefer-hardware'});
  needKey=true; ws.send('key');
}
function wcVideo(b){
  if(!dec||dec.state!=='configured')return;
  const key=(b[1]&1)===1; if(needKey&&!key)return;
  if(dec.decodeQueueSize>3){needKey=true;ws.send('key');return;}   // falling behind: skip to the next keyframe
  needKey=false;
  const ts=Number(new DataView(b.buffer,b.byteOffset,b.byteLength).getBigInt64(2,true));
  dec.decode(new EncodedVideoChunk({type:key?'key':'delta',timestamp:ts,data:b.subarray(10)}));
}
const worklet=`class P extends AudioWorkletProcessor{
 constructor(){super();this.q=[];this.pos=0;this.n=0;this.port.onmessage=e=>{this.q.push(e.data);this.n+=e.data.length/2;}}
 process(i,o){const L=o[0][0],R=o[0][1]||L,max=sampleRate*0.06;
  while(this.n>max&&this.q.length>1){const d=this.q.shift();this.n-=(d.length-this.pos)/2;this.pos=0;}
  for(let k=0;k<L.length;k++){
   if(!this.q.length){L[k]=R[k]=0;continue;}
   const d=this.q[0];L[k]=d[this.pos];R[k]=d[this.pos+1];this.pos+=2;this.n--;
   if(this.pos>=d.length){this.q.shift();this.pos=0;}
  }return true;}}
registerProcessor('pcm',P);`;
async function startWCAudio(){
  actx=new AudioContext({latencyHint:'interactive',sampleRate:cfg&&cfg.audioRate||48000});
  await actx.audioWorklet.addModule(URL.createObjectURL(new Blob([worklet],{type:'text/javascript'})));
  anode=new AudioWorkletNode(actx,'pcm',{outputChannelCount:[2]}); anode.connect(actx.destination);
}
function wcAudio(b){
  if(!anode)return;
  const s=new Int16Array(b.buffer.slice(b.byteOffset+10,b.byteOffset+b.byteLength)), f=new Float32Array(s.length);
  for(let i=0;i<s.length;i++)f[i]=s[i]/32768; anode.port.postMessage(f,[f.buffer]);
}

// ---- Media Source Extensions (plain http): fragmented MP4, kept at the live edge ----
function setupMSE(){
  const keep=cfg; reset(); cfg=keep;
  const MS=window.ManagedMediaSource||window.MediaSource;
  if(!MS){status('This browser cannot play the stream');return;}
  ms=new MS(); v.src=URL.createObjectURL(ms);
  ms.addEventListener('sourceopen',()=>{
    try{vsb=ms.addSourceBuffer('video/mp4; codecs="'+cfg.codec+'"');}catch(e){status('Unsupported video: '+cfg.codec);return;}
    vsb.mode='segments'; vsb.addEventListener('updateend',pump);
    if(cfg.audioRate){try{asb=ms.addSourceBuffer('audio/mp4; codecs="mp4a.40.2"');asb.mode='segments';asb.addEventListener('updateend',pump);}catch(e){asb=null;}}
    ws.send('key'); pump();
  });
}
function pump(){
  try{
    if(vsb&&!vsb.updating&&vq.length)vsb.appendBuffer(vq.shift());
    if(asb&&!asb.updating&&aq.length)asb.appendBuffer(aq.shift());
  }catch(e){status('Playback error, resyncing');vq=[];aq=[];ws.send('key');}
}
function onBinary(b){
  lastData=performance.now();
  switch(b[0]){
    case 1: case 2: if(vsb||ms){vq.push(b.subarray(1));pump();if(b[0]===2)frames++;} break;
    case 3: case 4: if(asb){aq.push(b.subarray(1));pump();} break;
    case 5: wcVideo(b); break;
    case 6: wcAudio(b); break;
  }
}
const Q=new URLSearchParams(location.search), qn=(k,d)=>{const x=parseFloat(Q.get(k));return isFinite(x)?x:d;};
const T={seek:qn('seek',0.35),hi:qn('hi',0.12),lo:qn('lo',0.03),fast:qn('fast',1.08)};
let lastTrim=0;
setInterval(()=>{
  const now=performance.now();
  if(now-lastFpsAt>=1000){fps=Math.round(frames*1000/(now-lastFpsAt));frames=0;lastFpsAt=now;}
  if(useWC){ if(dec)status(fps+' fps · WebCodecs'+latShown); return; }
  if(!vsb||!v.buffered.length)return;
  const end=v.buffered.end(v.buffered.length-1), lag=end-v.currentTime;
  if(v.paused)v.play().catch(()=>{});
  // Stay right behind the newest data: jump if far behind, otherwise nudge the playback rate.
  // Stay just behind the newest frame: nudge the playback rate, jump only when far behind (a jump restarts
  // decoding at the previous keyframe). Tunable for tests: ?seek=0.35&hi=0.12&lo=0.03&fast=1.08
  if(lag>T.seek||v.currentTime<v.buffered.start(0))v.currentTime=Math.max(v.buffered.start(0),end-0.04);
  else v.playbackRate=lag>T.hi?T.fast:(lag<T.lo?0.98:1.0);
  if(now-lastTrim>10000&&!vsb.updating&&v.currentTime>8){lastTrim=now;try{vsb.remove(0,v.currentTime-4);if(asb&&!asb.updating)asb.remove(0,v.currentTime-4);}catch(e){}}
  status(fps+' fps · '+Math.round(lag*1000)+' ms buffered'+latShown);
},100);
// Every 2 s: report what this viewer sees to the PC's log (fps, latency from capture to screen, drops).
let lastDropped=0;
setInterval(()=>{
  ping();
  if(!lat.length){latShown='';return;}
  const s=lat.slice().sort((a,b)=>a-b), avg=s.reduce((a,b)=>a+b,0)/s.length;
  latShown=' · '+Math.round(avg)+' ms latency';
  const q=v.getVideoPlaybackQuality?v.getVideoPlaybackQuality():null, dropped=q?q.droppedVideoFrames-lastDropped:0; if(q)lastDropped=q.droppedVideoFrames;
  const buf=(!useWC&&v.buffered.length)?Math.round((v.buffered.end(v.buffered.length-1)-v.currentTime)*1000):0;
  if(ws&&ws.readyState===1)ws.send('stats '+JSON.stringify({path:useWC?'webcodecs':'mse',fps,lat:Math.round(avg),min:Math.round(s[0]),
    max:Math.round(s[s.length-1]),p95:Math.round(s[Math.floor(s.length*0.95)]),n:s.length,dropped,buf,rtt:Math.round(bestRtt),
    w:(useWC?c.width:v.videoWidth),h:(useWC?c.height:v.videoHeight),ua:navigator.userAgent.slice(0,120)}));
  lat=[];
},2000);

$('tap').onclick=async()=>{
  $('tap').style.display='none'; soundOn=true;
  if(useWC){ if(!actx) await startWCAudio(); else actx.resume(); }
  else { v.muted=false; v.play().catch(()=>{}); }
};
$('fs').onclick=()=>{const d=document.documentElement;(d.requestFullscreen||d.webkitRequestFullscreen).call(d);};
let idle=0; document.addEventListener('pointermove',()=>{$('bar').classList.remove('hide');idle=performance.now();});
setInterval(()=>{if(performance.now()-idle>3000)$('bar').classList.add('hide');},500);
document.addEventListener('dblclick',()=>$('fs').onclick());
connect();
</script></body></html>
)HTML";

} // namespace cc::web
