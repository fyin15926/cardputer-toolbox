# 2026-05-24 stable sleep + playback fade baseline

Backed up file:

- `toolbox_20260524_stable_sleep_playback_fade.ino`

Observed state before the next recording-stop experiment:

- Sleep idle no longer appears to produce sudden random pops after removing the 30s timer wakeup.
- Playback start pop is basically gone after applying an 80ms fade-in to the actual PCM data.
- Recording begin/end still may pop, but recording/playback reliability is back to the stable baseline.
- Do not reintroduce the previous mic-hot / no-Mic.end experiment on the main version; it caused low volume, missing audio, and severe second-recording pops.

Next experiment:

- Keep the stable recording lifecycle.
- Before `Mic.end()` at recording stop, only disconnect the output path (`0x32=0x00`, `0x13=0x00`, optionally keep DAC power untouched) and then run the existing `Mic.end()` recovery sequence.
