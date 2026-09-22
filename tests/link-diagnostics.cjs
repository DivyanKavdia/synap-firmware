'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
for(const target of ['esp32s3-fh4r2-qspi-4m','esp32c3-supermini-4m','xiao-esp32s3-sense-8m']) {
  test(`${target} diagnostics retain link failure evidence across recording starts`,()=>{
    const code=materialize(source,target);
    const chakshu=target==='xiao-esp32s3-sense-8m';
    const health=chakshu?fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/ble-health.cpp'),'utf8'):'';
    const forward=chakshu?(code.match(/^extern std::atomic<bool> recoveryWaiting;$/m)?.[0]||''):'';
    const diagnostics=code.slice(code.indexOf('void updateDiagnosticsCharacteristic() {'),code.indexOf('// Optional recovery protocol.'));
    const version=code.slice(code.indexOf('constexpr uint8_t DIAGNOSTICS_MAGIC'),code.indexOf('constexpr uint8_t CMD_STOP'));
    const encode=code.slice(code.indexOf('static void put32le('),code.indexOf('// Transport-independent protocol engine'));
    assert.match(nativeTest(`
#include <atomic>
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>
#define USE_REAL_I2S_MIC 1
${health}
${forward}
${chakshu?`std::atomic<bool> chakshuAudioSubscribed{true};
constexpr uint8_t TOUCH_INPUT_PIN=1,TOUCH_ACTIVE_LEVEL=1,BATTERY_ADC_PIN=2,RGB_LED_PIN=5;
bool touchStableState=false,remoteStandby=false;std::atomic<bool> batteryAvailable{true};
uint8_t batteryPercent=77;uint16_t batteryAdcMillivolts=1200,batteryAdcRaw=2500,batteryMillivolts=3756;
uint32_t lastLedPattern=0x010203;std::atomic<uint32_t> touchTransitions{4},touchActions{2};std::atomic<uint16_t> touchLastHoldMs{120};
enum class DeviceState:uint8_t{DISCONNECTED=0,CONNECTED_IDLE=1,STREAMING=2,ERROR=3};DeviceState deviceState=DeviceState::STREAMING;
int digitalRead(uint8_t pin){assert(pin==TOUCH_INPUT_PIN);return 1;}`:''}
${version}
std::atomic<bool> deviceConnected{true},streamingEnabled{true},otaBusySnapshot{false},pcmTransport{false};
bool bootSleepWasLocked=false;
uint8_t bootResetReason=9;
std::atomic<uint32_t> capturedFrames{2345},captureDrops{2},notifyRejected{7},controlDrops{1};
std::atomic<uint32_t> linkDisconnects{3},lastDisconnectAt{0xFEDCBA98},lastNotifyError{0x103};
std::atomic<uint16_t> lastDisconnectReason{8},lastNotifyStatus{7};
struct {uint32_t getFreeHeap(){return 95000;}uint32_t getMinFreeHeap(){return 82000;}} ESP;
uint32_t millis(){return 123000;}
struct Characteristic {std::vector<uint8_t> bytes;void setValue(uint8_t* data,unsigned n){bytes.assign(data,data+n);}} characteristic;
auto* diagnosticsCharacteristic=&characteristic;
${encode}
${diagnostics}
// Match production ordering: the recovery storage follows the encoder.
${chakshu?'std::atomic<bool> recoveryWaiting{false};':''}
uint32_t u32(unsigned o){const auto& b=characteristic.bytes;return uint32_t(b[o])|(uint32_t(b[o+1])<<8)|(uint32_t(b[o+2])<<16)|(uint32_t(b[o+3])<<24);}
int main(){
  updateDiagnosticsCharacteristic();const auto& b=characteristic.bytes;
  assert(b.size()==${chakshu?112:48} && b[0]==0xD6 && b[1]==${chakshu?5:2} && b[2]==0x47 && b[3]==9);
  assert(u32(4)==2345 && u32(8)==2 && u32(12)==7 && u32(16)==1);
  assert(u32(20)==95000 && u32(24)==82000 && u32(28)==123);
  assert(b[32]==8 && b[33]==0 && b[34]==7 && b[35]==0);
  assert(u32(36)==3 && u32(40)==0xFEDCBA98 && u32(44)==0x103);
  ${chakshu?'assert(b[67]==4);assert(b[84]==1&&b[85]==0&&b[86]==1&&b[87]==77);assert(b[88]==0xB0&&b[89]==0x04);assert(b[97]==1&&b[98]==2&&b[99]==5);assert(u32(100)==4&&u32(104)==2);recoveryWaiting=true;updateDiagnosticsCharacteristic();assert(b[67]==2);recoveryWaiting=false;':''}
  pcmTransport=true;updateDiagnosticsCharacteristic();assert(b[2]==0xC7);
  capturedFrames=0;notifyRejected=0;streamingEnabled=false;updateDiagnosticsCharacteristic();
  assert(u32(4)==0 && u32(12)==0 && u32(36)==3 && b[32]==8 && u32(44)==0x103);
  lastDisconnectReason=0xFFFF;updateDiagnosticsCharacteristic();assert(b[32]==255 && b[33]==255);
  std::cout<<"PASS link diagnostics";
}`),/PASS link diagnostics/);
  });
}
