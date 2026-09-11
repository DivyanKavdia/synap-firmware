# Audio conditioning and firmware cleanup

## Findings and choice

The microphone path converts signed, left-aligned I2S slots to PCM16 using `raw >> 16` at unity digital gain. Software amplification cannot recover microphone signal-to-noise ratio and reduces clipping headroom.

The INMP441 specifies 24-bit I2S words in 32-bit slots, a natural acoustic high-pass corner around 60 Hz, and an additional internal digital high-pass whose 3.7 Hz corner at 48 kHz scales with sample rate. These built-in filters do not provide speech-band noise suppression. [TDK INMP441 datasheet, pages 10–14](https://product.tdk.com/system/files/dam/doc/product/sw_piezo/mic/mems-mic/data_sheet/inmp441.pdf).

The firmware now adds a modest 70 Hz first-order high-pass between PCM conversion and ADPCM encoding. This reduces low frequency rumble before a lossy codec spends its range representing that rumble. It does not remove nearby conversation, traffic in the speech band or broad background hiss. There is no automatic gain boost, noise gate, voice detector or dropped silence.

Stronger enhancement belongs initially in an optional local PWA processing path that retains the received recording for comparison. ESP-SR provides a firmware audio front end, but integrating its model selection, buffering and feed/fetch tasks into this Arduino/BLE design needs a separate resource and device benchmark, particularly for the C3 target without PSRAM. It has not been added here. [Espressif AFE API](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/audio_front_end/README.html).

## Exact processing and measured response

The `SynapAudio::SpeechHighPass` class in the production sketch contains the tested implementation. At 16 kHz:

```text
a = 31880 / 32768
b = 32324 / 32768
y[n] = a*y[n-1] + b*(x[n]-x[n-1])
```

It uses Q8 state, 64-bit intermediate products, symmetric PCM rounding and saturating output. The first sample initializes the input history and produces zero, preventing a stale offset from the previous recording. Filter history persists across 800-sample frames, and resets on a new recording generation or a successful I2S driver recovery. Only the capture task owns that history.

Native tests of the exact embedded implementation measured these steady-state sine responses at a PCM amplitude of 10,000:

| Frequency | Added filter gain |
| ---: | ---: |
| 20 Hz | −11.217 dB |
| 50 Hz | −4.709 dB |
| 70 Hz | −3.007 dB |
| 100 Hz | −1.730 dB |
| 300 Hz | −0.229 dB |
| 1 kHz | −0.021 dB |
| 6 kHz | approximately 0 dB |

These are synthetic filter measurements, not evidence of improved transcription accuracy or real-world speech quality. The microphone's own response remains in addition to this response; low-pitched voices need listening comparison.

The filter needs 12 bytes of state plus a 4-byte generation counter, no heap allocation and no extra audio frame. There is no additional buffering delay; a causal high-pass still has frequency-dependent phase delay. The 50 ms frame duration, 800 samples, 404-byte independent ADPCM frame, BLE protocol versions and queue capacity are unchanged. No model or floating-point operation is added to the real microphone path.

The hardware acceptance budget is less than 1 ms of added processing per 50 ms frame on each target, with no increase in capture drops or notification rejections during a sustained recording. This budget has not been measured on a pendant. Host throughput is printed by the native test only as a regression aid and must not be presented as ESP32 timing.

## Runtime and source structure

The production sketch is compiled without logic-rewriting patches. Tests inspect that source directly, and the C3 generator changes only hardware-specific configuration. The asynchronous event characteristic carries battery and power events. Diagnostic tone state is excluded from microphone builds.

Capture blocks on a task notification while idle and wakes on START. ADPCM packing initializes each output byte as it writes the low nibble, avoiding a redundant 400-byte clear per frame. The transmitter preserves its 45 ms pacing window and yields during the final fragment's remaining wait. These changes reduce polling and redundant work; actual device speed and power savings require measurement.

## Verification and release gate

Run `node --test tests/*.cjs`. Native C++ tests compile with warnings as errors and undefined-behavior sanitization. They exercise rumble response, speech-band and quiet-tone retention, DC settling, full-scale polarity reversals, reset determinism and exact bypass for every signed PCM16 value. A fake I2S backend compiles the exact production capture function and checks split byte reads, frame continuity, new recordings, driver recovery and stop cancellation.

Prepare the exact production sketches with:

```sh
node tools/prepare-production.cjs synap_esp32s3/synap_esp32s3.ino prepared/synap_esp32s3/synap_esp32s3.ino
node tools/materialize-target.cjs esp32c3-supermini-4m prepared/synap_esp32s3/synap_esp32s3.ino prepared/synap_esp32c3/synap_esp32c3.ino
```

The pull-request workflow must compile both targets using the pinned production Arduino/ESP32 toolchain. Local native tests do not substitute for those board builds. For an otherwise identical A/B firmware build, add `-DSYNAP_MIC_HPF_ENABLE=0` alongside the existing real-I2S/build flags.

Before firmware release, compare enabled/bypass recordings with low and high voices, quiet speech, a fan, walking/clothing noise and silence. Check onset after boot/standby, long-recording CPU/drop counters, double/triple taps, OTA and reconnect. Actual speech quality, transcription improvement and device timing require physical measurements.
