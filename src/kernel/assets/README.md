# Kernel-embedded assets

## volume-pop.s16le

The volume-change feedback sound. The window manager plays it once at the new
level after a user volume change (`src/kernel/src/volume_feedback.h`,
`WmVolumeFeedbackTick`). It is embedded with `.incbin` by
`src/kernel/src/volume_pop.cpp`.

- **Format:** raw PCM, 44.1 kHz, stereo, signed 16-bit little-endian, 3540
  frames (80 ms), 14,160 bytes.
- **SHA-256:** `7754a085879c0b6ef6394e18619aeb8e1d3f4f8f15fd812645fdd19122d3b192`.
- **Source:** "Sound Effect by linhmitto
  (https://pixabay.com/users/linhmitto-46708238/) from Pixabay
  (https://pixabay.com/sound-effects/)", item 254773, under the Pixabay
  Content License. The original MP3 has SHA-256 `5b534a1541aeba6bbe43080a960f9f83164ce72f0cf917caec81aa3bdeea0cd9`.
- **Conversion:**
  1. Decode the MP3 to 44.1 kHz stereo s16le.
  2. Trim the 105 ms leading silence and the MP3 decoder delay.
  3. Apply a 2 ms linear fade-in and a 10 ms fade-out.
  4. Attenuate by −6 dB.

  The peak is 15314 (about −6.6 dBFS).
