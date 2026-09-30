'use strict';
const {PRIMARY_TARGET}=require('../../targets.cjs');
const {replaceOnce}=require('../../target-source.cjs');
function materializeC3(source,target){
  let out=replaceOnce(source,'p[21]!=9 || p[22]!=0',`p[21]!=${target.chip} || p[22]!=0`,'ESP image chip ID');
  out=replaceOnce(out,'analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_6db);',
    `analogSetPinAttenuation(BATTERY_ADC_PIN, ${target.hardware.batteryAttenuation});`,'ADC input range');

  const taskBefore=`  if (xTaskCreatePinnedToCore(controlTask, "control", 8192, nullptr, 3, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(acquisitionTask, "capture", 4096, nullptr, 2, &captureTaskHandle, 0) != pdPASS ||
      xTaskCreatePinnedToCore(transmitterTask, "transmit", 8192, nullptr, 2, nullptr, 1) != pdPASS) {`;
  const taskAfter=`  // ESP32-C3 has one core; retain task priorities and stack sizes without pinning.
  if (xTaskCreate(controlTask, "control", 8192, nullptr, 3, nullptr) != pdPASS ||
      xTaskCreate(acquisitionTask, "capture", 4096, nullptr, 2, &captureTaskHandle) != pdPASS ||
      xTaskCreate(transmitterTask, "transmit", 8192, nullptr, 2, nullptr) != pdPASS) {`;
  out=replaceOnce(out,taskBefore,taskAfter,'single-core task creation');

  if(out.includes(PRIMARY_TARGET))throw Error('C3 source still contains the S3 target identity');
  if(out.includes('SYNAP-ESP32S3-OTA-ID-V3'))throw Error('C3 source still contains the S3 product marker');
  if(out.includes('esp_sleep_enable_ext1_wakeup'))throw Error('C3 source still contains unsupported EXT1 wake');
  if(!out.includes(`"SYNAP-FW:${target.id}:" SYNAP_VERSION`))throw Error('C3 firmware identity was not materialized');
  if(!out.includes(target.productMarker))throw Error('C3 OTA marker was not materialized');
  if(!out.includes('p[21]!=5 || p[22]!=0'))throw Error('C3 chip image check was not materialized');
  if(!out.includes('esp_deep_sleep_enable_gpio_wakeup'))throw Error('C3 GPIO deep-sleep wake is unavailable');
  if(!out.includes('long press -> DEEP SLEEP'))throw Error('C3 long-press power gesture was not materialized');
  if(!out.includes('double tap -> START'))throw Error('C3 double-tap recording gesture was not materialized');
  return out;
}

module.exports={materializeC3};
