#pragma once

// The welcome page phones get when they join ZeroDisplay's own Wi-Fi network (wireless/CaptivePortal).
// {{OS}}, {{RECEIVER}} are filled in per request; the watch button follows /status.
namespace cc::web {

inline constexpr const char kLandingPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ZeroDisplay</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 96 96'%3E%3Cpath d='M18 22H78M18 74H78' stroke='%230078D7' stroke-width='14' stroke-linecap='round'/%3E%3Cpath d='M74 27L22 69' stroke='%2360CDFF' stroke-width='8' stroke-linecap='round'/%3E%3C/svg%3E">
<style>
html,body{margin:0;background:#11141a;color:#eef;font:16px "Segoe UI",system-ui,sans-serif}
main{max-width:520px;margin:0 auto;padding:28px 20px 40px}
h1{font-size:26px;margin:0 0 4px;display:flex;align-items:center;gap:10px}
.logo{width:30px;height:30px}
h2{font-size:17px;font-weight:500;margin:30px 0 10px;border-left:3px solid #60cdff;padding-left:10px}
.dim{color:#9aa3b5}
a.btn{display:block;text-align:center;background:#0078d7;color:#fff;text-decoration:none;border-radius:4px;
 padding:16px;font-size:18px;font-weight:600;margin-top:18px}
a.btn.off{background:#2a2f3a;color:#9aa3b5;pointer-events:none}
.os{display:none;background:#1b1f28;border-radius:4px;padding:14px 16px;margin:10px 0;line-height:1.45}
.os b{display:block;margin-bottom:4px}
body.android .android,body.ios .ios,body.windows .windows,body.other .os{display:block}
.name{color:#60cdff;font-weight:600}
</style></head><body class="{{OS}}"><main>
<h1><svg class="logo" viewBox="0 0 96 96" aria-hidden="true"><defs><linearGradient id="zg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#8BE1FF"/><stop offset="1" stop-color="#0063B8"/></linearGradient></defs><path d="M18 22H78M18 74H78" stroke="url(#zg)" stroke-width="13" stroke-linecap="round" fill="none"/><path d="M74 27L22 69" stroke="#60CDFF" stroke-width="6" stroke-linecap="round"/></svg>ZeroDisplay</h1>
<div class="dim">You're on this PC's own Wi-Fi network.</div>
<h2>Watch</h2>
<a id="watch" class="btn off" href="#">Watch live</a>
<div id="hint" class="dim" style="margin-top:10px">The PC isn't sharing right now: turn on “Share on the local network” in ZeroDisplay.</div>
<h2>Cast your screen to the PC</h2>
<div class="os android"><b>Android</b>Open Quick Settings ▸ <b style="display:inline">Smart View</b> (or Cast / Screen mirroring) ▸ pick <span class="name">{{RECEIVER}}</span>.</div>
<div class="os windows"><b>Windows</b>Press Win+K and pick <span class="name">{{RECEIVER}}</span>.</div>
<div class="os ios"><b>iPhone / iPad</b>Apple devices only cast with AirPlay, which Windows can't receive. Use a Lightning or USB-C to HDMI adapter into the capture card instead.</div>
</main>
<script>
"use strict";
const w=document.getElementById('watch'), h=document.getElementById('hint');
function poll(){
  fetch('/status',{cache:'no-store'}).then(r=>r.json()).then(s=>{
    if(s.viewer){ w.href=s.viewer; w.classList.remove('off'); h.style.display='none'; }
    else { w.href='#'; w.classList.add('off'); h.style.display=''; }
  }).catch(()=>{}).finally(()=>setTimeout(poll,2000));
}
poll();
</script>
</body></html>
)HTML";

} // namespace cc::web
