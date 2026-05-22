# Voice — TTS prompt-read + STT decision (v1.4.0)

Speaker reads the `dash prompt` hint aloud; mic captures "yes" / "no"
/ "explain" / "no, not that one". Same decision flow as the buttons,
but hands-free — useful when the user is across the desk or hands are
busy.

## Hardware

The Waveshare ESP32-S3-Touch-AMOLED-2.16 already has an onboard mic
(MEMS, I2S) and a small speaker driven via the audio codec. v1.4.0
uses the existing aurora-harness audio path (used in Aurora for the
audio scene).

## TTS

Two engines, in order of preference:

1. **On-device** (`esp_tts` component, available in IDF v6). Tiny
   English voice. ~500ms per prompt. Privacy-preserving.
2. **Bridge fallback**: bridge generates WAV via the host's TTS
   (e.g. `pyttsx3`) and pushes via `dash audio_play <wav-bytes>`.
   Used when the device asks for a higher-quality voice or non-English.

## STT

Local-first via tiny wake-word + 4-class command classifier:
- "approve" / "deny" / "explain" / "cancel"
Trained on 1k samples per command, INT8 ~50KB model. Inference ~80ms.
The model file ships in the OTA partition (v0.6.0 already partitions
flash for this).

If confidence < 0.85, fallback to "ask the user to repeat" + show
visual buttons. Never silently mis-decide a permission prompt.

## Wire commands

- `dash voice_say "<text>"` — TTS the text on device speaker.
- `dash voice_listen <timeout_s>` — listen for one command word;
  emit `EVT: voice_decision word=approve|deny|explain|cancel`.

## Scaffold status

Spec + stubs (`main/audio/voice_tts.{c,h}`, `main/audio/voice_stt.{c,h}`)
land in v1.4.0. Full TTS + STT implementation lands in v1.4.x once
the model training pipeline is ready. Falls back to v1 buttons
seamlessly.
