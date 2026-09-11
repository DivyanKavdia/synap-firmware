'use strict';
const fs=require('node:fs'),path=require('node:path');
const {PRIMARY_TARGET,getTarget}=require('./targets.cjs');

function replaceOnce(source,before,after,label){
  const first=source.indexOf(before);
  if(first<0)throw Error(`Missing target materialization anchor: ${label}`);
  if(source.indexOf(before,first+before.length)>=0)throw Error(`Ambiguous target materialization anchor: ${label}`);
  return source.slice(0,first)+after+source.slice(first+before.length);
}

function materialize(source,targetId){
  const target=getTarget(targetId);
  if(target.id===PRIMARY_TARGET)return source;
  if(target.family!=='esp32c3')throw Error(`No materializer for ${target.id}`);

  let out=source;
  out=out.replace(/ESP32-S3FH4R2/g,'ESP32-C3 SuperMini');
  out=out.split(PRIMARY_TARGET).join(target.id);
  out=out.split('SYNAP-ESP32S3-OTA-ID-V3').join(target.productMarker);
  out=replaceOnce(out,'p[21]!=9 || p[22]!=0','p[21]!=5 || p[22]!=0','ESP image chip ID');
  out=replaceOnce(out,'constexpr uint8_t RGB_LED_PIN = 48;','constexpr uint8_t RGB_LED_PIN = 8;','C3 status LED pin');
  out=replaceOnce(out,'#define SYNAP_TOUCH_PIN 13','#define SYNAP_TOUCH_PIN 3','C3 touch/wake pin');
  out=replaceOnce(out,'#define SYNAP_BATTERY_ADC_PIN 8','#define SYNAP_BATTERY_ADC_PIN 1','C3 battery ADC pin');
  out=out.replace(/GPIO8/g,'GPIO1');

  const taskPrefix=`  if (xTaskCreatePinnedToCore(controlTask, "control", 8192, nullptr, 3, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(acquisitionTask, "capture", 4096, nullptr, 2, &captureTaskHandle, 0) != pdPASS ||
      xTaskCreatePinnedToCore(transmitterTask, "transmit", `;
  const taskSuffix=`, nullptr, 2, nullptr, 1) != pdPASS) {`;
  const taskStart=out.indexOf(taskPrefix);
  if(taskStart<0)throw Error('Missing target materialization anchor: single-core task creation');
  if(out.indexOf(taskPrefix,taskStart+taskPrefix.length)>=0)throw Error('Ambiguous target materialization anchor: single-core task creation');
  const stackStart=taskStart+taskPrefix.length,stackEnd=out.indexOf(taskSuffix,stackStart);
  if(stackEnd<0)throw Error('Missing transmitter stack value in task creation');
  const transmitterStack=out.slice(stackStart,stackEnd);
  if(transmitterStack!=='8192')throw Error(`Unsupported transmitter stack ${transmitterStack}`);
  const taskBefore=taskPrefix+transmitterStack+taskSuffix;
  const taskAfter=`  // ESP32-C3 is single-core. Keep the same priority ordering without pinning to
  // non-existent core 1; preserve the validated transmitter stack.
  if (xTaskCreate(controlTask, "control", 8192, nullptr, 3, nullptr) != pdPASS ||
      xTaskCreate(acquisitionTask, "capture", 4096, nullptr, 2, &captureTaskHandle) != pdPASS ||
      xTaskCreate(transmitterTask, "transmit", ${transmitterStack}, nullptr, 2, nullptr) != pdPASS) {`;
  out=replaceOnce(out,taskBefore,taskAfter,'single-core task creation');

  if(out.includes(PRIMARY_TARGET))throw Error('C3 source still contains the S3 target identity');
  if(out.includes('SYNAP-ESP32S3-OTA-ID-V3'))throw Error('C3 source still contains the S3 product marker');
  if(out.includes('esp_sleep_enable_ext1_wakeup'))throw Error('C3 source still contains unsupported EXT1 wake');
  if(!out.includes(`SYNAP-FW:${target.id}:1.0.0:`))throw Error('C3 firmware identity was not materialized');
  if(!out.includes(target.productMarker))throw Error('C3 OTA marker was not materialized');
  if(!out.includes('p[21]!=5 || p[22]!=0'))throw Error('C3 chip image check was not materialized');
  if(!out.includes('esp_deep_sleep_enable_gpio_wakeup'))throw Error('C3 GPIO deep-sleep wake is unavailable');
  return out;
}

if(require.main===module){
  const args=process.argv.slice(2),check=args[0]==='--check';
  if(check)args.shift();
  const [targetId,input,output]=args;
  if(!targetId||!input)throw Error('Usage: node tools/materialize-target.cjs [--check] <target> <prepared-source> [output]');
  const source=fs.readFileSync(input,'utf8'),result=materialize(source,targetId);
  if(check){console.log(`PASS: materialized ${targetId}`);process.exit(0);}
  if(!output)throw Error('Output path is required unless --check is used');
  fs.mkdirSync(path.dirname(output),{recursive:true});
  fs.writeFileSync(output,result);
  console.log(`Materialized ${targetId} -> ${output}`);
}

module.exports={materialize,replaceOnce};
