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

WEBGL = b"""<!doctype html><title>gpu probe webgl</title><body style=margin:0><canvas id=c width=1280 height=720></canvas><script>
const g=document.getElementById('c').getContext('webgl');let t=[];
const vs='attribute vec2 p;uniform float a;void main(){float c=cos(a),s=sin(a);gl_Position=vec4(mat2(c,-s,s,c)*p,0,1);}';
const fs='precision mediump float;uniform float a;void main(){gl_FragColor=vec4(.5+.5*sin(a+gl_FragCoord.x/40.),.5+.5*cos(a*1.3+gl_FragCoord.y/30.),.6,1);}';
function sh(k,s){const x=g.createShader(k);g.shaderSource(x,s);g.compileShader(x);return x;}
const pr=g.createProgram();g.attachShader(pr,sh(g.VERTEX_SHADER,vs));g.attachShader(pr,sh(g.FRAGMENT_SHADER,fs));g.linkProgram(pr);g.useProgram(pr);
const b=g.createBuffer();g.bindBuffer(g.ARRAY_BUFFER,b);g.bufferData(g.ARRAY_BUFFER,new Float32Array([-.8,-.8,.8,-.8,0,.9]),g.STATIC_DRAW);
const l=g.getAttribLocation(pr,'p');g.enableVertexAttribArray(l);g.vertexAttribPointer(l,2,g.FLOAT,false,0,0);const u=g.getUniformLocation(pr,'a');
function f(now){t.push(now);g.clearColor(.1,.1,.2,1);g.clear(g.COLOR_BUFFER_BIT);g.uniform1f(u,now/500);g.drawArrays(g.TRIANGLES,0,3);requestAnimationFrame(f);}
requestAnimationFrame(f);
window.probe=()=>{const d=t.slice(1).map((x,i)=>x-t[i]).sort((a,b)=>a-b);const q=p=>d[Math.floor(p*(d.length-1))];
return {webgl:!!g,renderer:g&&g.getParameter(g.RENDERER),frames:t.length,p50:q(.5),p90:q(.9),p99:q(.99),mean:d.reduce((a,b)=>a+b,0)/d.length};};
</script>"""

VIDEO = b"""<!doctype html><title>gpu probe video</title><body style=margin:0;background:#000>
<video id=v src=/clip.webm autoplay muted loop playsinline width=1280 height=720></video><script>
const v=document.getElementById('v');let t=[],start=null;
function cb(now,m){t.push(now);if(start===null)start={wall:now,media:m.mediaTime};v.requestVideoFrameCallback(cb);}
if(v.requestVideoFrameCallback)v.requestVideoFrameCallback(cb);
window.probe=()=>{const d=t.slice(1).map((x,i)=>x-t[i]).sort((a,b)=>a-b);const q=p=>d[Math.floor(p*(d.length-1))];
const pq=v.getVideoPlaybackQuality?v.getVideoPlaybackQuality():{};
return {presented:t.length,p50:q(.5),p90:q(.9),p99:q(.99),currentTime:v.currentTime,paused:v.paused,error:v.error&&v.error.code,
 total:pq.totalVideoFrames,dropped:pq.droppedVideoFrames,mediaRate:start&&t.length>1?(v.currentTime-start.media)/((performance.now()-start.wall)/1000):null};};
</script>"""

PAGES = {'anim': PAGE, 'webgl': WEBGL, 'video': VIDEO}
