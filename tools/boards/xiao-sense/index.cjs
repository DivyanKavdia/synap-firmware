'use strict';
const {replaceOnce,replaceFunctionBlock,readTemplate}=require('../../target-source.cjs');
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
  replace('#include <Adafruit_NeoPixel.h>','','No external LED');
  replace('Adafruit_NeoPixel statusLed(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);','','No camera-pin LED');
  out=out.split('statusLed.clear();statusLed.show();').join('/* No external LED on Chakshu. */');
  out=replaceFunctionBlock(out,'void updateStatusLed(bool force) {','void setDeviceState(',
    'void updateStatusLed(bool force) { (void)force; }\n\n','No external indicator');
  const stopStart=out.indexOf('void stopStreaming(ErrorCode reason) {');
  const stopEnd=out.indexOf('bool configureTransportFromPeerMtu() {',stopStart);
  if(stopStart<0 || stopEnd<0)throw Error('Missing BLE stop boundary');
  const stop=out.slice(stopStart,stopEnd).replace('  stopMicrophone();','  if (!mediaBusy()) stopMicrophone();');
  out=out.slice(0,stopStart)+stop+out.slice(stopEnd);
  replace('applyCpuPowerProfile(streamingEnabled.load() || otaNeedsActiveCpu());',
    'applyCpuPowerProfile(streamingEnabled.load() || otaNeedsActiveCpu() || mediaBusy());','Camera CPU profile');
  // Remove touch wake/sleep implementations, including durable wake gates from another board.
  out=replaceFunctionBlock(out,'bool armTouchWakeSource() {','void publishPowerEvent(',
    'bool armTouchWakeSource() { return false; }\nvoid armTouchWakeAndSleep() {}\nbool confirmTouchWakeGesture() { return true; }\n\n','Always-awake boot');
  out=replaceFunctionBlock(out,'void enterDeepSleep(const char* reason) {','void updateStatusCharacteristic(',
    'void enterDeepSleep(const char* reason) { (void)reason; }\nvoid powerTick() {}\nvoid pollTouchControl() {}\n\n','No touch or automatic sleep');
  // Initialization must not touch GPIO8 (SD MISO), GPIO13 (camera clock), or GPIO48 (camera data).
  const bootStart=out.indexOf('  bootSleepWasLocked=readDurableSleepLock()');
  const bootEnd=out.indexOf('  disconnectedAt=millis();',bootStart);
  if (bootStart<0 || bootEnd<0) throw Error('Missing Chakshu hardware setup boundary');
  out=out.slice(0,bootStart)+'  bootSleepWasLocked=false;\n  delay(400);\n'+out.slice(bootEnd);
  replace('    microphoneI2S.setPins(I2S_BCLK_PIN, I2S_WS_PIN, -1, I2S_DATA_IN_PIN);',
    `    microphoneI2S.setPinsPdmRx(${target.hardware.clock},${target.hardware.data});`,'Onboard PDM pins');
  replace('microphoneReady=microphoneI2S.begin(I2S_MODE_STD, SAMPLE_RATE,\n      I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);',
    'microphoneReady=microphoneI2S.begin(I2S_MODE_PDM_RX, SAMPLE_RATE,\n      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);','PDM PCM16 driver');
  replace('  static int32_t raw[SAMPLES_PER_FRAME];','  static int16_t raw[SAMPLES_PER_FRAME];','Native PCM16 capture buffer');
  replace('    const int32_t sample=raw[i] >> 16;','    const int32_t sample=raw[i];','Preserve onboard PCM samples');
  replace('// SYNAP_BOARD_FEATURES',
    ['camera.cpp','sd-storage.cpp','media.cpp','media-transfer.cpp'].map(name=>readTemplate('xiao-sense',name)).join('\n'),'Camera and SD drivers');
  if (out.includes('statusLed.') || out.includes('pinMode(TOUCH_INPUT_PIN') ||
      out.includes('analogSetPinAttenuation(') || out.includes('esp_deep_sleep_start()'))
    throw Error('Chakshu still accesses absent hardware');
  replace('  ChakshuMedia::initialize();',
    '  const uint32_t mediaStarted=millis();\n  ChakshuMedia::initialize();\n  const uint32_t mediaBootMs=millis()-mediaStarted;','Measure media boot cost');
  out=materializeBle(out);
  replace('void controlTask(void* parameter) {',
    'void controlTask(void* parameter) {\n  while (!ChakshuResources::runtimeReady.load()) vTaskDelay(1);','Wait for complete BLE initialization');
  replace('  initializeBLE();','  initializeBLE();\n  ChakshuResources::runtimeReady.store(true);\n  Serial.printf("[CHAKSHU] ready_ms=%lu media_ms=%lu heap=%lu psram=%lu voice=off\\n",(unsigned long)millis(),(unsigned long)mediaBootMs,(unsigned long)ESP.getFreeHeap(),(unsigned long)ESP.getFreePsram());','Publish initialized runtime');
  return out;
}
module.exports={materializeChakshu};
