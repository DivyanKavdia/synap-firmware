'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const battery=fs.readFileSync('firmware/shared/battery.cpp','utf8');
const boot=fs.readFileSync('firmware/shared/boot.cpp','utf8');
const detect=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');

test('C3 SD battery divider identity persists independently of a failed future SD probe',()=>{
  const start=battery.indexOf('static std::atomic<bool> odysseySdBatteryDividerObserved');
  const end=battery.indexOf('// Field Odyssey C3+SD',start);
  assert(start>0 && end>start);
  const code=battery.slice(start,end);
  const output=nativeTest(`
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdint>
#include <iostream>
static bool persisted=false,allowNvs=true;
static int writes=0;
struct FakeSerial { void println(const char*){} } Serial;
struct Preferences {
  bool begin(const char* ns,bool ro) { (void)ro;assert(!strcmp(ns,"synap-c3-sd")); return allowNvs; }
  bool getBool(const char* key,bool value) { assert(!strcmp(key,"adc-div")); (void)value; return persisted; }
  size_t putBool(const char* key,bool value) { assert(!strcmp(key,"adc-div"));persisted=value;++writes;return 1; }
  void end(){}
};
${code}
int main() {
  // A new standard C3 without an SD mount is left in its original x2 mode.
  assert(!odysseySdBatteryDividerPresent());
  restoreOdysseySdBatteryDividerProfile();
  assert(!odysseySdBatteryDividerPresent() && !persisted);
  markOdysseySdBatteryDividerPresent();
  persistOdysseySdBatteryDividerProfile();
  assert(persisted && writes==1);
  persistOdysseySdBatteryDividerProfile();
  assert(writes==1); // no flash write on every boot
  odysseySdBatteryDividerObserved=false; // simulate full cold power loss
  restoreOdysseySdBatteryDividerProfile();
  assert(odysseySdBatteryDividerPresent()); // profile survives failed SD mount
  allowNvs=false;odysseySdBatteryDividerObserved=false;
  restoreOdysseySdBatteryDividerProfile();
  assert(!odysseySdBatteryDividerPresent()); // read failure stays fail-closed
  std::cout<<"PASS C3 persisted ADC profile\\n";
}
`);
  assert.match(output,/PASS C3 persisted ADC profile/);
});

test('C3 boot restores divider profile BEFORE initial battery read and SD.begin',()=>{
  const pos=boot.indexOf('restoreOdysseySdBatteryDividerProfile();');
  assert(pos>0);
  const bootFlow=boot.slice(boot.indexOf('OdysseyTransfer::initialize();'),boot.indexOf('initializeBLE();'));
  assert(bootFlow.indexOf('restoreOdysseySdBatteryDividerProfile();')>=0);
  assert(bootFlow.indexOf('restoreOdysseySdBatteryDividerProfile();')<bootFlow.indexOf('sampleBattery(true);'));
  assert(bootFlow.indexOf('restoreOdysseySdBatteryDividerProfile();')<bootFlow.indexOf('odysseyInitializeSdCardBeforeBle();'));
  assert.match(detect,/if \(mounted\) \{\s*markOdysseySdBatteryDividerPresent\(\)/);
  const okay=detect.indexOf('persistOdysseySdBatteryDividerProfile();');
  assert(okay>0 && okay<detect.indexOf('odysseySdLastMountError=ESP_OK;',okay));
  assert(detect.indexOf('if (!odysseySdValidateVfsLocked(')<okay);
});
