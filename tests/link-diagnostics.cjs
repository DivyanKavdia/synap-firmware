'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const source=fs.readFileSync(path.join(__dirname,'../synap_esp32s3/synap_esp32s3.ino'),'utf8');
for(const target of ['esp32s3-fh4r2-qspi-4m','esp32c3-supermini-4m']) {
  test(`${target} diagnostics retain link failure evidence across recording starts`,()=>{
    const code=materialize(source,target);
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
uint32_t u32(unsigned o){const auto& b=characteristic.bytes;return uint32_t(b[o])|(uint32_t(b[o+1])<<8)|(uint32_t(b[o+2])<<16)|(uint32_t(b[o+3])<<24);}
int main(){
  updateDiagnosticsCharacteristic();const auto& b=characteristic.bytes;
  assert(b.size()==48 && b[0]==0xD6 && b[1]==2 && b[2]==0x47 && b[3]==9);
  assert(u32(4)==2345 && u32(8)==2 && u32(12)==7 && u32(16)==1);
  assert(u32(20)==95000 && u32(24)==82000 && u32(28)==123);
  assert(b[32]==8 && b[33]==0 && b[34]==7 && b[35]==0);
  assert(u32(36)==3 && u32(40)==0xFEDCBA98 && u32(44)==0x103);
  pcmTransport=true;updateDiagnosticsCharacteristic();assert(b[2]==0xC7);
  capturedFrames=0;notifyRejected=0;streamingEnabled=false;updateDiagnosticsCharacteristic();
  assert(u32(4)==0 && u32(12)==0 && u32(36)==3 && b[32]==8 && u32(44)==0x103);
  lastDisconnectReason=0xFFFF;updateDiagnosticsCharacteristic();assert(b[32]==255 && b[33]==255);
  std::cout<<"PASS link diagnostics";
}`),/PASS link diagnostics/);
  });
}
