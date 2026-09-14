'use strict';
const {PRIMARY_TARGET}=require('../../targets.cjs');
const {replaceOnce,replaceFunctionBlock,readTemplate}=require('../../target-source.cjs');
function materializeChakshu(source,target) {
  let out=source.split(PRIMARY_TARGET).join(target.id)
    .split('SYNAP-ESP32S3-OTA-ID-V3').join(target.productMarker)
    .split('ESP32-S3FH4R2').join('Chakshu / XIAO ESP32S3 Sense');
  const replace=(before,after,label)=>out=replaceOnce(out,before,after,label);
  replace('#define SYNAP_CHAKSHU 0','#define SYNAP_CHAKSHU 1','Chakshu build profile');
  replace('#define SYNAP_MODULE_ID 1','#define SYNAP_MODULE_ID 3','Chakshu board identity');
  replace('#define DEVICE_NAME "synap"','#define DEVICE_NAME "synap-Chakshu"','Chakshu advertising name');
  replace('#include <Adafruit_NeoPixel.h>','','No external LED');
  replace('Adafruit_NeoPixel statusLed(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);','','No camera-pin LED');
  out=out.split('statusLed.clear();statusLed.show();').join('/* No external LED on Chakshu. */');
  out=replaceFunctionBlock(out,'void updateStatusLed(bool force) {','void setDeviceState(',
    'void updateStatusLed(bool force) { (void)force; }\n\n','No external indicator');
  replace('  if (microphoneValidated) stopMicrophone();',
    '  // Keep the onboard PDM microphone initialized for bring-up.','Always-on microphone');
  const stopStart=out.indexOf('void stopStreaming(ErrorCode reason) {');
  const stopEnd=out.indexOf('bool configureTransportFromPeerMtu() {',stopStart);
  if(stopStart<0 || stopEnd<0)throw Error('Missing BLE stop boundary');
  const stop=out.slice(stopStart,stopEnd).replace('  stopMicrophone();','  if (!mediaBusy()) stopMicrophone();');
  out=out.slice(0,stopStart)+stop+out.slice(stopEnd);
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
  replace('#define SYNAP_BATTERY_MONITOR_ENABLE 1','#define SYNAP_BATTERY_MONITOR_ENABLE 0','No battery divider');
  replace('    microphoneI2S.setPins(I2S_BCLK_PIN, I2S_WS_PIN, -1, I2S_DATA_IN_PIN);',
    '    microphoneI2S.setPinsPdmRx(42,41);','Onboard PDM pins');
  replace('microphoneReady=microphoneI2S.begin(I2S_MODE_STD, SAMPLE_RATE,\n      I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);',
    'microphoneReady=microphoneI2S.begin(I2S_MODE_PDM_RX, SAMPLE_RATE,\n      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);','PDM PCM16 driver');
  replace('  static int32_t raw[SAMPLES_PER_FRAME];','  static int16_t raw[SAMPLES_PER_FRAME];','Native PCM16 capture buffer');
  replace('    const int32_t sample=raw[i] >> 16;','    const int32_t sample=raw[i];','Preserve onboard PCM samples');
  replace('// SYNAP_BOARD_FEATURES',
    ['camera.cpp','sd-storage.cpp','media.cpp'].map(name=>readTemplate('xiao-sense',name)).join('\n'),'Camera and SD drivers');
  if (out.includes('statusLed.') || out.includes('pinMode(TOUCH_INPUT_PIN') ||
      out.includes('analogSetPinAttenuation(') || out.includes('esp_deep_sleep_start()'))
    throw Error('Chakshu still accesses absent hardware');
  return out;
}
module.exports={materializeChakshu};
