#!/usr/bin/env python3
"""Generate a local, click-to-play WAV page; output routing is external."""
import argparse
import base64
import io
import math
import struct
import wave
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
args = parser.parse_args()
pcm = b"".join(struct.pack("<h", int(1000 * math.sin(2 * math.pi * 440 * i / 44100)))
               for i in range(44100))
buffer = io.BytesIO()
with wave.open(buffer, "wb") as wav:
    wav.setnchannels(1)
    wav.setsampwidth(2)
    wav.setframerate(44100)
    wav.writeframes(pcm)
source = base64.b64encode(buffer.getvalue()).decode("ascii")
html = '''<!doctype html>
<meta charset="utf-8">
<title>Brook media: ready</title>
<style>body { font: 32px sans-serif; margin: 30px; } button { display: block; font: 40px sans-serif; padding: 40px; margin-bottom: 20px; }</style>
<h1>Local media check</h1>
<button id="play">Start media audio</button>
<button id="graph">Start Web Audio</button>
<p id="status">Waiting for click</p>
<audio id="audio" loop src="data:audio/wav;base64,%s"></audio>
<script>
const audio = document.querySelector("#audio");
function phase(s) {
  document.title = "Brook media: " + s;
  document.querySelector("#status").textContent = s;
}
for (const event of ["playing", "waiting", "stalled", "error", "pause"])
  audio.addEventListener(event, () => phase("event " + event));
document.querySelector("#play").addEventListener("click", () => {
  phase("clicked");
  audio.play().then(() => phase("playing promise"))
    .catch(error => phase("play error " + error));
});
let graph;
document.querySelector("#graph").addEventListener("click", () => {
  try {
    const context = new AudioContext();
    const oscillator = context.createOscillator();
    const gain = context.createGain();
    oscillator.frequency.value = 440;
    gain.gain.value = 0.05;
    oscillator.connect(gain);
    gain.connect(context.destination);
    oscillator.start();
    graph = {context, oscillator, gain};
    phase("graph resume " + context.state);
    context.onstatechange = () => phase("graph state " + context.state);
    context.resume().then(() => phase("graph resumed " + context.state))
      .catch(error => phase("graph error " + error));
  } catch (error) {
    phase("graph error " + error);
  }
});
</script>
''' % source
args.output.write_text(html, encoding="utf-8")
