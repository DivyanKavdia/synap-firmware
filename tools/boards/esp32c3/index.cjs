'use strict';
const {PRIMARY_TARGET}=require('../../targets.cjs');
const {replaceOnce}=require('../../target-source.cjs');
const led=require('./led.cjs'),battery=require('./battery.cjs'),touch=require('./touch.cjs');

function materializeC3(source,target){
  let out=source;
  out=out.replace(/ESP32-S3FH4R2/g,'ESP32-C3 SuperMini');
  out=out.split(PRIMARY_TARGET).join(target.id);
  out=out.split('SYNAP-ESP32S3-OTA-ID-V3').join(target.productMarker);
  out=replaceOnce(out,'p[21]!=9 || p[22]!=0','p[21]!=5 || p[22]!=0','ESP image chip ID');
  out=replaceOnce(out,'constexpr uint8_t RGB_LED_PIN = 48;','constexpr uint8_t RGB_LED_PIN = 8;','C3 status LED pin');
  out=replaceOnce(out,'#define SYNAP_TOUCH_PIN 13','#define SYNAP_TOUCH_PIN 3','C3 touch/wake pin');
  out=replaceOnce(out,'#define SYNAP_BATTERY_ADC_PIN 8','#define SYNAP_BATTERY_ADC_PIN 1','C3 battery ADC pin');
  out=out.replace(/GPIO8/g,'GPIO1');

  out=led.apply(out);
  out=battery.apply(out);
  out=touch.apply(out);

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
  if(!out.includes(`SYNAP-FW:${target.id}:1.0.0:`))throw Error('C3 firmware identity was not materialized');
  if(!out.includes(target.productMarker))throw Error('C3 OTA marker was not materialized');
  if(!out.includes('p[21]!=5 || p[22]!=0'))throw Error('C3 chip image check was not materialized');
  if(!out.includes('esp_deep_sleep_enable_gpio_wakeup'))throw Error('C3 GPIO deep-sleep wake is unavailable');
  if(!out.includes('C3 long press -> DEEP SLEEP'))throw Error('C3 long-press power gesture was not materialized');
  if(!out.includes('C3 double tap -> START'))throw Error('C3 double-tap recording gesture was not materialized');
  if(out.includes('triple tap -> DEEP SLEEP'))throw Error('C3 source still contains the S3 triple-tap power gesture');
  return out;
}

module.exports={materializeC3};
