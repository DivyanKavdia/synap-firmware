bool acquireAudioFrame(AudioFrame& frame) {
#if USE_REAL_I2S_MIC
  MicrophoneGuard guard;
  static int32_t raw[SAMPLES_PER_FRAME];
  size_t received=0;
  uint8_t emptyReads=0;
  bool microphoneRecoveryUsed=false;
  while (received < sizeof(raw)) {
    if (!streamingEnabled.load() || frame.generation != streamGeneration.load()) return false;
    const size_t count = microphoneI2S.readBytes(
      reinterpret_cast<char*>(raw)+received, sizeof(raw)-received);
    if (!count) {
      if (++emptyReads < 3) continue;
      if (!microphoneRecoveryUsed) {
        microphoneRecoveryUsed=true;
        Serial.println("[MIC] empty I2S reads; restarting capture driver");
        stopMicrophone();
        vTaskDelay(pdMS_TO_TICKS(35));
        if (!streamingEnabled.load() || frame.generation != streamGeneration.load()) return false;
        if (startMicrophone()) { received=0; emptyReads=0; continue; }
      }
      return false;
    }
    emptyReads=0;
    received += count;
  }
  for (uint16_t i=0; i<SAMPLES_PER_FRAME; ++i) {
    const int32_t sample=raw[i] >> 16;
    // Format conversion only: retain the signed upper 16 bits of the I2S slot.
    // No filter, gain, gate or per-recording signal history precedes the codec.
    frame.samples[i]=static_cast<int16_t>(sample);
  }
#else
  const float increment=2.0f*PI*440.0f/SAMPLE_RATE;
  for (uint16_t i=0; i<SAMPLES_PER_FRAME; ++i) {
    frame.samples[i]=static_cast<int16_t>(sinf(tonePhase)*9000.0f);
    tonePhase+=increment;
    if (tonePhase >= 2.0f*PI) tonePhase-=2.0f*PI;
  }
#endif
  return true;
}
void acquisitionTask(void* parameter) {
  (void)parameter;
  AudioFrame frame;
  uint32_t generation=0;
  uint16_t nextSequence=0;
#if !USE_REAL_I2S_MIC
  TickType_t wake=xTaskGetTickCount();
#endif
  for (;;) {
    if (!streamingEnabled.load() || recoveryFinishing.load() || (!deviceConnected.load() && !recoveryEnabled.load())) {
      // START wakes capture immediately; idle recording needs no periodic polling.
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#if !USE_REAL_I2S_MIC
      wake=xTaskGetTickCount();
#endif
      continue;
    }
    frame.generation=streamGeneration.load();
    if (generation != frame.generation) { generation=frame.generation; nextSequence=0; }
    const bool acquired=acquireAudioFrame(frame);
    if (!streamingEnabled.load() || recoveryFinishing.load() || frame.generation != streamGeneration.load()) continue;
    if (!acquired) { requestStreamError(ErrorCode::AUDIO_SOURCE_FAILED, frame.generation); continue; }
    frame.sequence=nextSequence++;
    ++capturedFrames;
    retainRecoveryFrame(frame);
    if (xQueueSend(audioFrameQueue, &frame, 0) != pdTRUE && !recoveryEnabled.load()) ++captureDrops;
#if !USE_REAL_I2S_MIC
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(FRAME_DURATION_MS));
#endif
  }
}
