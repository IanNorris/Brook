"""Shared local animation page for the browser GPU probes."""
PAGE = b"""<!doctype html><title>gpu probe</title>
<style>body{margin:0;background:#123}#b{width:300px;height:300px;background:linear-gradient(45deg,#f80,#08f);
animation:s 2s linear infinite;border-radius:40px}@keyframes s{to{transform:rotate(360deg) scale(1.3)}}</style>
<div id=b></div><canvas id=c width=1280 height=720></canvas><script>
const c=document.getElementById('c').getContext('2d');let t=[];
function f(now){t.push(now);for(let i=0;i<200;i++){c.fillStyle=`hsl(${(now/10+i*7)%360},80%,50%)`;
c.fillRect((i*37+now/3)%1280,(i*53)%720,60,60);}requestAnimationFrame(f);}requestAnimationFrame(f);
window.probe=()=>{const d=t.slice(1).map((x,i)=>x-t[i]).sort((a,b)=>a-b);const q=p=>d[Math.floor(p*(d.length-1))];
return {frames:t.length,p50:q(.5),p90:q(.9),p99:q(.99),mean:d.reduce((a,b)=>a+b,0)/d.length};};
</script>"""
