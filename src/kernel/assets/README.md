# Kernel-embedded assets

## volume-pop.s16le

The volume-change feedback sound. The window manager plays it once at the new
level after a user volume change (`src/kernel/src/volume_feedback.h`,
`WmVolumeFeedbackTick`). It is embedded with `.incbin` by
`src/kernel/src/volume_pop.cpp`.

- **Format:** raw PCM, 44.1 kHz, stereo, signed 16-bit little-endian, 3540
  frames (80 ms), 14,160 bytes.
- **SHA-256:** `a5a85788a58763428a63470b8e19bda8231a2b71aa77f67ba458c792d05aa95a`.
- **Source:** "Sound Effect by linhmitto
  (https://pixabay.com/users/linhmitto-46708238/) from Pixabay
  (https://pixabay.com/sound-effects/)", item 254773, under the Pixabay
  Content License. The original MP3 has SHA-256 `5b534a1541aeba6bbe43080a960f9f83164ce72f0cf917caec81aa3bdeea0cd9`.
- **Conversion:** decode the MP3 to 44.1 kHz stereo s16le (`raw.pcm`), then:
  1. Take 3540 frames starting at frame 4626, which skips the 105 ms leading
     silence and the MP3 decoder delay.
  2. Apply a linear 2 ms (88-frame) fade-in and 10 ms (441-frame) fade-out.
  3. Normalise the peak to −0.1 dBFS (×1.0576; source peak 30629, peak 32392).

  It is played at full scale on purpose, so that the pop represents the new
  volume level. The mixer's master volume, applied in hardware on the Dell,
  sets how loud it is.
