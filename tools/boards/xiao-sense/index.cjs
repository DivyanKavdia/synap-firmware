'use strict';
const {replaceOnce,readTemplate}=require('../../target-source.cjs');
const {materializeBle}=require('./ble.cjs');
function materializeChakshu(source,target) {
  let out=source;
  const replace=(before,after,label)=>out=replaceOnce(out,before,after,label);
  replace('#include <atomic>','#include <atomic>\n'+readTemplate('xiao-sense','ownership.cpp'),'Resource admission gate');
  replace('bool remoteStandby = false;','std::atomic<bool> remoteStandby{false};','Cross-task standby state');
  replace('  if (mediaBusy()) { updateStatusCharacteristic(true);return; }',
    '  ChakshuResources::Lease admission;\n  if (!admission) { updateStatusCharacteristic(true);return; }','Reserve START transition');
  replace('    otaSession.packet(message.data,message.length,millis(),generation,',
    '    ChakshuResources::Lease admission;\n    otaSession.packet(message.data,message.length,millis(),generation,','Reserve OTA transition');
  replace('      || mediaBusy()','      || !admission','OTA admission result');
  const stopStart=out.indexOf('void stopStreaming(ErrorCode reason) {');
  const stopEnd=out.indexOf('bool configureTransportFromPeerMtu() {',stopStart);
  if(stopStart<0 || stopEnd<0)throw Error('Missing BLE stop boundary');
  const stop=out.slice(stopStart,stopEnd).replace('  stopMicrophone();','  if (!mediaBusy() && !ChakshuVoice::active()) stopMicrophone();');
  out=out.slice(0,stopStart)+stop+out.slice(stopEnd);
  replace('applyCpuPowerProfile(streamingEnabled.load() || otaNeedsActiveCpu());',
    'applyCpuPowerProfile(streamingEnabled.load() || otaNeedsActiveCpu() || mediaBusy() || ChakshuVoice::active());','Camera/voice CPU profile');
  replace('    microphoneI2S.setPins(I2S_BCLK_PIN, I2S_WS_PIN, -1, I2S_DATA_IN_PIN);',
    `    microphoneI2S.setPinsPdmRx(${target.hardware.clock},${target.hardware.data});`,'Onboard PDM pins');
  replace('microphoneReady=microphoneI2S.begin(I2S_MODE_STD, SAMPLE_RATE,\n      I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);',
    'microphoneReady=microphoneI2S.begin(I2S_MODE_PDM_RX, SAMPLE_RATE,\n      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);','PDM PCM16 driver');
  replace('  static int32_t raw[SAMPLES_PER_FRAME];','  static int16_t raw[SAMPLES_PER_FRAME];','Native PCM16 capture buffer');
  replace('    const int32_t sample=raw[i] >> 16;','    const int32_t sample=raw[i];','Preserve onboard PCM samples');
  replace('// SYNAP_BOARD_FEATURES',
    ['voice-contract.cpp','tiny-voice-model.h','camera.cpp','sd-storage.cpp','media.cpp','media-buffers.cpp','sd-recording.cpp','wifi-downloads.cpp','media-transfer.cpp','voice.cpp'].map(name=>readTemplate('xiao-sense',name)).join('\n'),'Camera, SD and local voice drivers');
  replace('std::atomic<bool> batteryAvailable{false};',
    'std::atomic<bool> batteryAvailable{false};\nstd::atomic<uint32_t> touchTransitions{0},touchActions{0};\nstd::atomic<uint16_t> touchLastHoldMs{0};',
    'Retain Chakshu hardware input evidence');
  // SD owns the PDM reader while offline recording. Feed the recognizer the
  // exact PCM copy already captured for the WAV, never a competing microphone read.
  replace('if(slot->size){s.capturedBytes.fetch_add(slot->size);s.audio.publish();}',
    'if(slot->size){ChakshuVoice::feed(reinterpret_cast<const int16_t*>(slot->bytes),slot->size/2);s.capturedBytes.fetch_add(slot->size);s.audio.publish();}',
    'Copy SD PCM to command recognizer');
  // The normal BLE recording path remains authoritative. Voice gets only a
  // copy of completed PCM frames and cannot alter transport bytes.
  replace('  return true;\n}\nvoid acquisitionTask',
    '  ChakshuVoice::feed(frame.samples,SAMPLES_PER_FRAME);\n  return true;\n}\nvoid acquisitionTask','Copy streamed PCM to command recognizer');
  replace("  pinMode(TOUCH_INPUT_PIN, INPUT);","  pinMode(TOUCH_INPUT_PIN, INPUT_PULLDOWN);","Stabilize TTP223 input after boot");
  replace("  statusLed.begin();\n  statusLed.clear();\n  statusLed.show();","  pinMode(RGB_LED_PIN,OUTPUT);\n  digitalWrite(RGB_LED_PIN,LOW);\n  delay(2);\n  statusLed.begin();\n  statusLed.clear();\n  statusLed.show();\n  delay(1);\n  statusLed.clear();\n  statusLed.show();","Force external NeoPixel dark at startup");
  replace("  statusLed.setPixelColor(0,statusLed.Color(r,g,b));\n  statusLed.show();","  statusLed.clear();\n  if(pattern)statusLed.setPixelColor(0,statusLed.Color(r,g,b));\n  statusLed.show();","Clear stale NeoPixel state before every pattern");
  replace("  } else if (batteryAvailable && batteryMillivolts<=BATTERY_LOW_MV) {","  } else if (mediaBusy()) {\n    if (now%900u<90u) g=LED_DIM+2;\n  } else if (batteryAvailable && batteryMillivolts<=BATTERY_LOW_MV) {","Show Chakshu media activity on NeoPixel");
  replace("  (void)analogRead(BATTERY_ADC_PIN);\n  delayMicroseconds(1200);\n  uint32_t mvTotal=0, rawTotal=0;\n  for (uint8_t i=0;i<16;++i) {","  for(uint8_t warmup=0;warmup<4;++warmup){(void)analogRead(BATTERY_ADC_PIN);delayMicroseconds(500);}\n  delayMicroseconds(3000);\n  constexpr uint8_t BATTERY_SAMPLE_COUNT=24;\n  uint32_t mvTotal=0, rawTotal=0;\n  for (uint8_t i=0;i<BATTERY_SAMPLE_COUNT;++i) {","Settle high-impedance Chakshu battery divider");
  replace("  const uint32_t adcMv=mvTotal/16u;\n  const uint32_t adcRaw=rawTotal/16u;","  const uint32_t adcMv=mvTotal/BATTERY_SAMPLE_COUNT;\n  const uint32_t adcRaw=rawTotal/BATTERY_SAMPLE_COUNT;","Average settled Chakshu ADC samples");
  replace("  if (raw!=touchRawState) { touchRawState=raw; touchChangedAt=now; }","  if (raw!=touchRawState) { touchRawState=raw; touchChangedAt=now; ++touchTransitions; Serial.printf(\"[TOUCH] gpio=%u raw=%u transitions=%lu\\\\n\",unsigned(TOUCH_INPUT_PIN),raw?1u:0u,(unsigned long)touchTransitions.load()); }","Track TTP223 transitions");
  replace("    const uint32_t held=touchPressedAt ? uint32_t(now-touchPressedAt) : 0;\n    touchPressedAt=0;","    const uint32_t held=touchPressedAt ? uint32_t(now-touchPressedAt) : 0;\n    touchLastHoldMs=uint16_t(held>65535u?65535u:held);\n    touchPressedAt=0;","Track TTP223 hold time");
  replace("      Serial.println(\"[TOUCH] long press -> DEEP SLEEP\");","      ++touchActions;\n      Serial.println(\"[TOUCH] long press -> DEEP SLEEP\");","Count long-press touch action");
  replace("    touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;\n    if (streamingEnabled.load()) {","    touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;\n    ++touchActions;\n    if (streamingEnabled.load()) {","Count double-tap touch action");
  replace("    } else if (deviceConnected.load()) {\n      Serial.println(remoteStandby ? \"[TOUCH] double tap standby -> START\" : \"[TOUCH] double tap -> START\");\n      queueEvent(EventType::COMMAND,CMD_START,PROTOCOL_VERSION,streamGeneration.load());\n    }","    } else if (deviceConnected.load()) {\n      Serial.println(remoteStandby ? \"[TOUCH] double tap standby -> START\" : \"[TOUCH] double tap -> START\");\n      queueEvent(EventType::COMMAND,CMD_START,PROTOCOL_VERSION,streamGeneration.load());\n    } else if (ChakshuVoice::touchAudioToggle()) {\n      Serial.println(\"[TOUCH] double tap -> OFFLINE SD AUDIO TOGGLE\");\n    }","Route disconnected touch to SD audio");
  if (!out.includes('#define SYNAP_TOUCH_PIN 1') ||
      !out.includes('#define SYNAP_BATTERY_ADC_PIN 2') ||
      !out.includes('constexpr uint8_t RGB_LED_PIN = 5;') ||
      !out.includes('Adafruit_NeoPixel statusLed(1, RGB_LED_PIN') ||
      !out.includes('pinMode(TOUCH_INPUT_PIN, INPUT_PULLDOWN)') ||
      !out.includes('analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_6db)') ||
      !out.includes('esp_deep_sleep_start()'))
    throw Error('Chakshu touch, battery, NeoPixel or sleep materialization is incomplete');
  replace('  ChakshuMedia::initialize();',
    '  const uint32_t mediaStarted=millis();\n  ChakshuMedia::initialize();\n  ChakshuLink::mediaBootMs=millis()-mediaStarted;\n  // TinyML remains deferred: BLE/OTA/media initialize first; no external model is loaded.','Measure media boot cost without blocking BLE on local voice');
  replace('    ChakshuMedia::tick();','    ChakshuMedia::tick();\n    ChakshuVoice::tick();','Dispatch local voice commands');
  replace('  if (!deviceConnected.load() && !streamingEnabled.load() && !otaBusy() &&\n      disconnectedAt && uint32_t(millis()-disconnectedAt)>=AUTO_SLEEP_DISCONNECTED_MS) {',
    '  if (!deviceConnected.load() && !streamingEnabled.load() && !otaBusy() &&\n      !ChakshuVoice::active() &&\n      disconnectedAt && uint32_t(millis()-disconnectedAt)>=AUTO_SLEEP_DISCONNECTED_MS) {',
    'Keep standalone Hey Snap awake');
  out=materializeBle(out);
  replace('  ChakshuTransfer::ble(service);','  ChakshuTransfer::ble(service);\n  ChakshuVoice::ble(service);','Register local voice service');
  replace('  p[14]=ChakshuTransfer::requests?1:0;','  p[14]=ChakshuTransfer::requests?1:0;\n  p[15]=2;','Advertise voice protocol v2');
  replace('bool sendRecoveryFrame() {',
    'bool ChakshuTransfer::chakshuAudioHasBacklog() {\n  if (!recoveryMutex) return false;\n  RecoveryGuard guard;\n  return recoveryFinishing.load() || recoveryRing.count-recoveryRing.cursor>2;\n}\nbool sendRecoveryFrame() {','Prioritize audio over camera notifications');
  replace('void controlTask(void* parameter) {',
    'void controlTask(void* parameter) {\n  while (!ChakshuResources::runtimeReady.load()) vTaskDelay(1);','Wait for complete BLE initialization');
  replace('  initializeBLE();','  initializeBLE();\n  ChakshuLink::bootReadyMs=millis();\n  ChakshuResources::runtimeReady.store(true);\n  // TinyML remains optional and starts only after the proven BLE/OTA/media boot path is healthy.\n  ChakshuVoice::scheduleInitialize();\n  Serial.printf("[CHAKSHU] ready_ms=%lu media_ms=%lu heap=%lu psram=%lu voice=%u\\n",(unsigned long)ChakshuLink::bootReadyMs.load(),(unsigned long)ChakshuLink::mediaBootMs.load(),(unsigned long)ESP.getFreeHeap(),(unsigned long)ESP.getFreePsram(),unsigned(ChakshuVoice::active()));','Publish initialized runtime before TinyML');
  return out;
}
module.exports={materializeChakshu};