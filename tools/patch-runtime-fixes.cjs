const fs=require('node:fs');

function replaceOnce(source,before,after,label){
  const i=source.indexOf(before);
  if(i<0)throw new Error(`Missing firmware anchor: ${label}`);
  if(source.indexOf(before,i+before.length)>=0)throw new Error(`Ambiguous firmware anchor: ${label}`);
  return source.slice(0,i)+after+source.slice(i+before.length);
}

function patch(source){
  let out=source;
  out=replaceOnce(out,
`bool batteryCritical() {
  return batteryAvailable && batteryValidSamples>=3 && batteryCriticalSamples>=2 &&
    batteryMillivolts<=BATTERY_CRITICAL_MV;
}`,
`#ifndef SYNAP_BATTERY_MONITOR_ENABLE
#define SYNAP_BATTERY_MONITOR_ENABLE 0
#endif
bool batteryCritical() {
#if SYNAP_BATTERY_MONITOR_ENABLE
  return batteryAvailable && batteryValidSamples>=3 && batteryCriticalSamples>=2 &&
    batteryMillivolts<=BATTERY_CRITICAL_MV;
#else
  return false;
#endif
}`,'battery critical guard');

  out=replaceOnce(out,
`void sampleBattery(bool force) {
  const uint32_t now=millis();`,
`void sampleBattery(bool force) {
#if !SYNAP_BATTERY_MONITOR_ENABLE
  (void)force;
  batteryAvailable=false;batteryValidSamples=0;batteryCriticalSamples=0;
  batteryMillivolts=0;batteryPercent=0;
  return;
#else
  const uint32_t now=millis();`,'battery sample guard start');

  out=replaceOnce(out,
`  publishBatteryEvent(true);
  updateStatusLed(true);
}

void enterDeepSleep`,
`  publishBatteryEvent(true);
  updateStatusLed(true);
#endif
}

void enterDeepSleep`,'battery sample guard end');

  return out;
}

if(require.main===module){
  const file=process.argv[2];
  if(!file)throw new Error('Usage: node tools/patch-runtime-fixes.cjs <sketch>');
  const source=fs.readFileSync(file,'utf8');
  const out=patch(source);
  fs.writeFileSync(file,out);
  console.log('Patched Synap runtime safeguards');
}
module.exports={patch};