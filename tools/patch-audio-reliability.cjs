const fs=require('node:fs');

function replaceOnce(source,before,after,label){
  const i=source.indexOf(before);
  if(i<0)throw new Error(`Missing firmware anchor: ${label}`);
  if(source.indexOf(before,i+before.length)>=0)throw new Error(`Ambiguous firmware anchor: ${label}`);
  return source.slice(0,i)+after+source.slice(i+before.length);
}

function patch(source){
  let out=source;

  // Let a large negotiated ATT MTU materially reduce notification pressure.
  // At MTU 517 this produces four ~400-byte chunks per 50 ms PCM frame instead
  // of the old hard minimum of ten 160-byte chunks. Smaller MTUs naturally use
  // more chunks and retain the existing 20-chunk safety ceiling.
  out=replaceOnce(out,
`constexpr uint8_t MIN_CHUNKS_PER_FRAME = 10;`,
`constexpr uint8_t MIN_CHUNKS_PER_FRAME = 4;`,
  'minimum audio chunks');
  out=replaceOnce(out,
`constexpr uint16_t MAX_AUDIO_PAYLOAD_BYTES = 160;`,
`constexpr uint16_t MAX_AUDIO_PAYLOAD_BYTES = 500;`,
  'maximum audio payload');

  // The microphone is no longer left clocked for the entire boot. Keep a separate
  // validation bit so OTA rollback can still prove the physical audio path once at
  // startup while microphoneReady means "I2S is actively running right now".
  out=replaceOnce(out,
`I2SClass microphoneI2S;\nbool microphoneReady = false;`,
`I2SClass microphoneI2S;\nbool microphoneReady = false;\nbool microphoneValidated = false;`,
  'microphone lifecycle state');

  out=replaceOnce(out,
`constexpr uint8_t LED_DIM = 4;\nconstexpr int8_t I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6;`,
`constexpr uint8_t LED_DIM = 4;\nconstexpr int8_t I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6;\n#if CONFIG_IDF_TARGET_ESP32S3\nconstexpr uint32_t IDLE_CPU_MHZ = 80, ACTIVE_CPU_MHZ = 240;\n#elif CONFIG_IDF_TARGET_ESP32C3\nconstexpr uint32_t IDLE_CPU_MHZ = 80, ACTIVE_CPU_MHZ = 160;\n#else\nconstexpr uint32_t IDLE_CPU_MHZ = 80, ACTIVE_CPU_MHZ = 160;\n#endif`,
  'power profile constants');

  out=replaceOnce(out,
`void setDeviceState(DeviceState state, ErrorCode error) {\n  deviceState = state;\n  errorCode = error;\n  updateStatusLed(true);\n}\n\nuint8_t batteryPercentFromMillivolts`,
`void setDeviceState(DeviceState state, ErrorCode error) {\n  deviceState = state;\n  errorCode = error;\n  updateStatusLed(true);\n}\n\nvoid applyCpuPowerProfile(bool active) {\n  static uint32_t appliedMHz = 0;\n  const uint32_t targetMHz = active ? ACTIVE_CPU_MHZ : IDLE_CPU_MHZ;\n  if (appliedMHz == targetMHz) return;\n  if (setCpuFrequencyMhz(targetMHz)) {\n    appliedMHz = targetMHz;\n    Serial.printf("[POWER] cpu=%luMHz mode=%s\\n",\n      static_cast<unsigned long>(targetMHz),active?"active":"idle");\n  } else {\n    Serial.printf("[POWER] cpu profile change to %luMHz failed\\n",\n      static_cast<unsigned long>(targetMHz));\n  }\n}\n\nbool startMicrophone() {\n#if USE_REAL_I2S_MIC\n  if (microphoneReady) return true;\n  microphoneI2S.setPins(I2S_BCLK_PIN, I2S_WS_PIN, -1, I2S_DATA_IN_PIN);\n  microphoneI2S.setTimeout(80);\n  microphoneReady=microphoneI2S.begin(I2S_MODE_STD, SAMPLE_RATE,\n    I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);\n  if (microphoneReady) {\n    microphoneValidated=true;\n    Serial.println("[POWER] microphone I2S on");\n  } else Serial.println("[MIC] initialization failed");\n  return microphoneReady;\n#else\n  return true;\n#endif\n}\n\nvoid stopMicrophone() {\n#if USE_REAL_I2S_MIC\n  if (!microphoneReady) return;\n  microphoneI2S.end();\n  microphoneReady=false;\n  Serial.println("[POWER] microphone I2S off");\n#endif\n}\n\nuint8_t batteryPercentFromMillivolts`,
  'idle power helpers');

  out=replaceOnce(out,
`  if (chunks < 10 || chunks > 20 || !payload || payload > 160) return false;\n  uint8_t packet[AUDIO_HEADER_BYTES+MAX_AUDIO_PAYLOAD_BYTES];`,
`  if (chunks < MIN_CHUNKS_PER_FRAME || chunks > MAX_CHUNKS_PER_FRAME ||\n      !payload || payload > MAX_AUDIO_PAYLOAD_BYTES) return false;\n  uint8_t packet[AUDIO_HEADER_BYTES+MAX_AUDIO_PAYLOAD_BYTES];`,
  'audio send bounds');

  // Finish each frame with transport headroom. The capture side remains clocked
  // by I2S at 50 ms/frame, while the transmitter has ~5 ms/frame to recover from
  // scheduler/BLE jitter instead of being permanently exactly at the deadline.
  out=replaceOnce(out,
`    const uint32_t target=started+static_cast<uint32_t>(index+1)*50000UL/chunks;`,
`    const uint32_t target=started+static_cast<uint32_t>(index+1)*45000UL/chunks;`,
  'audio transmit pacing');

  // One second of PCM frame buffering absorbs short browser/radio scheduling
  // stalls without immediately dropping captured frames. Sequence numbers still
  // make any true overflow visible to the PWA diagnostics/reconstruction layer.
  out=replaceOnce(out,
`  audioFrameQueue=xQueueCreate(4, sizeof(AudioFrame));`,
`  audioFrameQueue=xQueueCreate(20, sizeof(AudioFrame));`,
  'audio queue depth');

  // The INMP44x family provides 24-bit signed I2S data in a 32-bit slot. Convert
  // the signed slot to PCM16 first. The former >>14 applied ~12 dB hidden digital
  // gain and could hard-clip normal near-field speech before BLE transmission.
  out=replaceOnce(out,
`    int32_t sample=raw[i] >> 14;\n    if (sample > 32767) sample=32767;\n    if (sample < -32768) sample=-32768;\n    frame.samples[i]=static_cast<int16_t>(sample);`,
`    const int32_t sample=raw[i] >> 16;\n    frame.samples[i]=static_cast<int16_t>(sample);`,
  'microphone PCM normalization');

  // Battery telemetry is low priority while streaming. Sampling continues, but
  // defer its BLE notification until idle so audio never competes with periodic
  // battery/control traffic on constrained mobile links.
  out=replaceOnce(out,
`  publishBatteryEvent(true);\n  updateStatusLed(true);\n#endif\n}`,
`  if (!streamingEnabled.load()) publishBatteryEvent(true);\n  updateStatusLed(true);\n#endif\n}`,
  'defer battery notification during audio');

  // Any transition out of streaming first invalidates capture work, gives a pending
  // I2S read a short bounded window to return, then stops the I2S clocks. INMP44x
  // microphones enter their low-power state when those clocks disappear.
  out=replaceOnce(out,
`void stopStreaming(ErrorCode reason) {\n  streamingEnabled.store(false);\n  streamStartedAt=0;\n  ++streamGeneration; // Invalidates queued AND already-in-flight old task work.\n  if (audioFrameQueue) xQueueReset(audioFrameQueue);`,
`void stopStreaming(ErrorCode reason) {\n  streamingEnabled.store(false);\n  streamStartedAt=0;\n  ++streamGeneration; // Invalidates queued AND already-in-flight old task work.\n  if (audioFrameQueue) xQueueReset(audioFrameQueue);\n#if USE_REAL_I2S_MIC\n  if (microphoneReady) { vTaskDelay(pdMS_TO_TICKS(90)); stopMicrophone(); }\n#endif\n  applyCpuPowerProfile(false);`,
  'idle microphone shutdown');

  out=replaceOnce(out,
`#if USE_REAL_I2S_MIC\n  if (!microphoneReady) { stopStreaming(ErrorCode::AUDIO_SOURCE_FAILED); return; }\n#endif\n  if (!configureTransportFromPeerMtu()) { stopStreaming(ErrorCode::MTU_TOO_SMALL); return; }`,
`  applyCpuPowerProfile(true);\n#if USE_REAL_I2S_MIC\n  if (!startMicrophone()) { stopStreaming(ErrorCode::AUDIO_SOURCE_FAILED); return; }\n#endif\n  if (!configureTransportFromPeerMtu()) { stopStreaming(ErrorCode::MTU_TOO_SMALL); return; }`,
  'on-demand microphone startup');

  out=replaceOnce(out,
`    case CMD_STOP:\n      streamingEnabled.store(false);\n      streamStartedAt=0;\n      ++streamGeneration;\n      if (audioFrameQueue) xQueueReset(audioFrameQueue);\n      setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);\n      vTaskDelay(pdMS_TO_TICKS(60));\n      updateStatusCharacteristic(true);\n      break;`,
`    case CMD_STOP:\n      streamingEnabled.store(false);\n      streamStartedAt=0;\n      ++streamGeneration;\n      if (audioFrameQueue) xQueueReset(audioFrameQueue);\n      setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);\n      vTaskDelay(pdMS_TO_TICKS(90));\n      stopMicrophone();\n      applyCpuPowerProfile(false);\n      updateStatusCharacteristic(true);\n      break;`,
  'stop command power down');

  out=replaceOnce(out,
`  Serial.printf("[POWER] deep sleep: %s battery=%umV\\n", reason?reason:"idle", unsigned(batteryMillivolts));\n  statusLed.clear();statusLed.show();`,
`  Serial.printf("[POWER] deep sleep: %s battery=%umV\\n", reason?reason:"idle", unsigned(batteryMillivolts));\n  stopMicrophone();\n  applyCpuPowerProfile(false);\n  statusLed.clear();statusLed.show();`,
  'deep sleep peripheral shutdown');

  // Probe the microphone once after boot for hardware/rollback validation, then
  // immediately stop I2S. Actual capture restarts it on demand in startStreaming().
  out=replaceOnce(out,
`#if USE_REAL_I2S_MIC\n  microphoneI2S.setPins(I2S_BCLK_PIN, I2S_WS_PIN, -1, I2S_DATA_IN_PIN);\n  microphoneI2S.setTimeout(200);\n  microphoneReady=microphoneI2S.begin(I2S_MODE_STD, SAMPLE_RATE,\n    I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);\n  if (!microphoneReady) Serial.println("[MIC] initialization failed");\n#endif`,
`#if USE_REAL_I2S_MIC\n  microphoneValidated=startMicrophone();\n  if (microphoneValidated) stopMicrophone();\n#endif\n  applyCpuPowerProfile(false);`,
  'boot microphone probe');

  out=replaceOnce(out,
`#if USE_REAL_I2S_MIC\n      && microphoneReady\n#endif`,
`#if USE_REAL_I2S_MIC\n      && microphoneValidated\n#endif`,
  'OTA microphone validation state');

  // Keep capture task wakeups sparse while no recording is active. START latency is
  // still dominated by BLE/I2S setup, while idle scheduler churn drops materially.
  out=replaceOnce(out,
`    if (!streamingEnabled.load() || !deviceConnected.load()) {\n      vTaskDelay(pdMS_TO_TICKS(10));`,
`    if (!streamingEnabled.load() || !deviceConnected.load()) {\n      vTaskDelay(pdMS_TO_TICKS(40));`,
  'idle capture task cadence');

  // OTA temporarily needs full CPU speed too. This check is cheap and only changes
  // the clock when the requested profile changes.
  out=replaceOnce(out,
`    pollTouchControl();\n    otaTick();\n    powerTick();\n    updateStatusLed();`,
`    pollTouchControl();\n    otaTick();\n    powerTick();\n    applyCpuPowerProfile(streamingEnabled.load() || otaBusy());\n    updateStatusLed();`,
  'runtime CPU power profile');

  return out;
}

if(require.main===module){
  const file=process.argv[2];
  if(!file)throw new Error('Usage: node tools/patch-audio-reliability.cjs <sketch>');
  const source=fs.readFileSync(file,'utf8');
  fs.writeFileSync(file,patch(source));
  console.log('Patched Synap BLE audio reliability and idle power');
}
module.exports={patch};
