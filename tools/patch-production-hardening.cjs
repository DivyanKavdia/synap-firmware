'use strict';
const fs=require('node:fs');
function replaceOnce(source,before,after,label){const i=source.indexOf(before);if(i<0)throw Error(`Missing production hardening anchor: ${label}`);if(source.indexOf(before,i+before.length)>=0)throw Error(`Ambiguous production hardening anchor: ${label}`);return source.slice(0,i)+after+source.slice(i+before.length)}
function patch(source){let out=source;
  // Make the prepared source itself describe the production S3 wiring. This removes
  // the hidden compiler-only GPIO13 override and lets release source hashes reproduce
  // the actual binary configuration.
  out=replaceOnce(out,'#define SYNAP_TOUCH_PIN 7','#define SYNAP_TOUCH_PIN 13','S3 production touch pin');
  // A 404-byte ADPCM frame fits one notification at large MTU. Let the negotiated
  // capacity choose 1..20 chunks instead of forcing four and wasting radio events.
  out=replaceOnce(out,'constexpr uint8_t MIN_CHUNKS_PER_FRAME = 4;','constexpr uint8_t MIN_CHUNKS_PER_FRAME = 1;','compressed minimum chunks');
  // Touch timing and gesture semantics are fully owned by patch-touch-reliability.cjs.
  // Keep production hardening focused on cross-cutting power/audio/OTA safeguards.
  // Never enter deep sleep while the active-high TTP223 wake source is already
  // asserted; doing so creates an immediate wake/sleep loop.
  out=replaceOnce(out,'void enterDeepSleep(const char* reason) {\n  if (otaBusy() || streamingEnabled.load()) return;','void enterDeepSleep(const char* reason) {\n  if (otaBusy() || streamingEnabled.load()) return;\n  if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) return;','deep sleep release guard');

  // OTA/deep-sleep reboots exercise a begin -> end -> begin lifecycle. Make the
  // on-demand start self-healing instead of trusting one driver allocation attempt.
  out=replaceOnce(out,`bool startMicrophone() {\n#if USE_REAL_I2S_MIC\n  if (microphoneReady) return true;\n  microphoneI2S.setPins(I2S_BCLK_PIN, I2S_WS_PIN, -1, I2S_DATA_IN_PIN);\n  microphoneI2S.setTimeout(80);\n  microphoneReady=microphoneI2S.begin(I2S_MODE_STD, SAMPLE_RATE,\n    I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);\n  if (microphoneReady) {\n    microphoneValidated=true;\n    Serial.println("[POWER] microphone I2S on");\n  } else Serial.println("[MIC] initialization failed");\n  return microphoneReady;\n#else\n  return true;\n#endif\n}`,
`bool startMicrophone() {\n#if USE_REAL_I2S_MIC\n  if (microphoneReady) return true;\n  constexpr uint8_t MIC_START_ATTEMPTS=3;\n  for (uint8_t attempt=1; attempt<=MIC_START_ATTEMPTS; ++attempt) {\n    if (attempt>1) {\n      microphoneI2S.end();\n      vTaskDelay(pdMS_TO_TICKS(25u*attempt));\n    }\n    microphoneI2S.setPins(I2S_BCLK_PIN, I2S_WS_PIN, -1, I2S_DATA_IN_PIN);\n    microphoneI2S.setTimeout(80);\n    microphoneReady=microphoneI2S.begin(I2S_MODE_STD, SAMPLE_RATE,\n      I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);\n    if (microphoneReady) {\n      microphoneValidated=true;\n      Serial.printf("[POWER] microphone I2S on attempt=%u\\n",unsigned(attempt));\n      return true;\n    }\n    Serial.printf("[MIC] initialization failed attempt=%u/%u\\n",unsigned(attempt),unsigned(MIC_START_ATTEMPTS));\n  }\n  microphoneReady=false;\n  return false;\n#else\n  return true;\n#endif\n}`,'microphone initialization retries');

  // Reliability takes precedence over the small idle-power saving from repeatedly
  // tearing down I2S. Keep the microphone clocked for the whole awake session.
  // Deep sleep still shuts I2S down, and a genuine audio-source failure tears it
  // down so the next START gets a clean retry path.
  out=replaceOnce(out,
`  if (audioFrameQueue) xQueueReset(audioFrameQueue);\n#if USE_REAL_I2S_MIC\n  if (microphoneReady) { vTaskDelay(pdMS_TO_TICKS(90)); stopMicrophone(); }\n#endif\n  applyCpuPowerProfile(false);`,
`  if (audioFrameQueue) xQueueReset(audioFrameQueue);\n#if USE_REAL_I2S_MIC\n  if (reason==ErrorCode::AUDIO_SOURCE_FAILED && microphoneReady) stopMicrophone();\n#endif\n  applyCpuPowerProfile(false);`,
  'keep microphone alive after normal stream stop');

  out=replaceOnce(out,
`      setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);\n      vTaskDelay(pdMS_TO_TICKS(90));\n      stopMicrophone();\n      applyCpuPowerProfile(false);\n      updateStatusCharacteristic(true);`,
`      setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);\n      vTaskDelay(pdMS_TO_TICKS(60));\n      applyCpuPowerProfile(false);\n      updateStatusCharacteristic(true);`,
  'keep microphone alive after explicit stop');

  out=replaceOnce(out,
`#if USE_REAL_I2S_MIC\n  microphoneValidated=startMicrophone();\n  if (microphoneValidated) stopMicrophone();\n#endif\n  applyCpuPowerProfile(false);`,
`#if USE_REAL_I2S_MIC\n  microphoneValidated=startMicrophone();\n#endif\n  applyCpuPowerProfile(false);`,
  'keep microphone alive after boot validation');

  // One scheduler/I2S timeout is not evidence that the microphone failed. Allow a
  // bounded read window and one controlled driver restart before surfacing failure.
  out=replaceOnce(out,`  size_t received=0;\n  while (received < sizeof(raw)) {\n    if (!streamingEnabled.load() || frame.generation != streamGeneration.load()) return false;\n    const size_t count = microphoneI2S.readBytes(\n      reinterpret_cast<char*>(raw)+received, sizeof(raw)-received);\n    if (!count) return false;\n    received += count;\n  }`,`  size_t received=0;\n  uint8_t emptyReads=0;\n  bool microphoneRecoveryUsed=false;\n  while (received < sizeof(raw)) {\n    if (!streamingEnabled.load() || frame.generation != streamGeneration.load()) return false;\n    const size_t count = microphoneI2S.readBytes(\n      reinterpret_cast<char*>(raw)+received, sizeof(raw)-received);\n    if (!count) {\n      if (++emptyReads < 3) continue;\n      if (!microphoneRecoveryUsed) {\n        microphoneRecoveryUsed=true;\n        Serial.println("[MIC] empty I2S reads; restarting capture driver");\n        stopMicrophone();\n        vTaskDelay(pdMS_TO_TICKS(35));\n        if (!streamingEnabled.load() || frame.generation != streamGeneration.load()) return false;\n        if (startMicrophone()) { received=0; emptyReads=0; continue; }\n      }\n      return false;\n    }\n    emptyReads=0;\n    received += count;\n  }`,'I2S transient timeout recovery');
  // Mobile stacks may complete MTU negotiation after START. An increase is always
  // safe for the already-selected packet size. A decrease is also safe while the
  // current audio packet still fits; only a genuinely incompatible capacity stops.
  out=replaceOnce(out,`    if (deviceConnected.load() && streamingEnabled.load() &&\n        bleServer->getPeerMTU(bleServer->getConnId()) != peerMtu) {\n      stopStreaming(ErrorCode::TRANSPORT_CHANGED);\n    }`,`    if (deviceConnected.load() && streamingEnabled.load()) {\n      uint16_t liveMtu=bleServer->getPeerMTU(bleServer->getConnId());\n      if (liveMtu<23) liveMtu=23;\n      if (liveMtu!=peerMtu) {\n        const uint16_t liveCapacity=liveMtu-3;\n        if (liveCapacity < uint16_t(AUDIO_HEADER_BYTES+audioPayloadBytes.load())) {\n          stopStreaming(ErrorCode::TRANSPORT_CHANGED);\n        } else {\n          peerMtu=liveMtu;attValueCapacity=liveCapacity;\n          updateStatusCharacteristic(true);\n        }\n      }\n    }`,'nonfatal compatible MTU change');
  out=replaceOnce(out,'  // GPIO8 sees up to about 1.04 V from a 4.2 V cell through the 1M/330k divider.','  // GPIO8 is calibrated at 1.32 V ADC for a 4.13 V cell on the 1M/470k divider.','battery calibration comment');
  // If a previously healthy cell becomes critically low mid-update, stop further
  // flash writes rather than only rejecting a new BEGIN command.
  out=replaceOnce(out,'  otaSession.tick(now,generation,connected);\n  if (connected && !otaBusy()) {','  otaSession.tick(now,generation,connected);\n  if (batteryCritical() && otaBusy() && otaSession.state!=Synap::COMMITTED) {\n    otaSession.fail(Synap::BUSY);\n  }\n  if (connected && !otaBusy()) {','critical battery OTA abort');
  return out;
}
if(require.main===module){const file=process.argv[2];if(!file)throw Error('Usage: node tools/patch-production-hardening.cjs <sketch>');fs.writeFileSync(file,patch(fs.readFileSync(file,'utf8')));console.log('Applied Synap end-to-end production hardening')}
module.exports={patch,replaceOnce};
