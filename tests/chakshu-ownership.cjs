'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {assemble}=require('../tools/assemble-source.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const {nativeTest}=require('./support/native.cjs');
const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
const ownership=fs.readFileSync('firmware/xiao-sense/ownership.cpp','utf8');

test('START reserves Chakshu resources through microphone initialization and publishes before release',()=>{
  const start=source.slice(source.indexOf('void startStreaming(uint8_t version) {'),source.indexOf('void queueEvent(EventType type, uint8_t command, uint8_t version, uint32_t stream) {'));
  const fixture=`#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <thread>
#define SYNAP_CHAKSHU 1
#define USE_REAL_I2S_MIC 1
${ownership}
enum class DeviceState {STREAMING};
enum class ErrorCode {NONE,PROTOCOL_MISMATCH,AUDIO_NOT_SUBSCRIBED,MTU_TOO_SMALL,AUDIO_SOURCE_FAILED};
constexpr uint8_t PROTOCOL_VERSION=2;
std::atomic<bool> deviceConnected{true},streamingEnabled{false},chakshuAudioSubscribed{true};
std::atomic<unsigned> streamGeneration{0},capturedFrames{0},captureDrops{0},notifyRejected{0};
bool updating=false,microphoneValid=true,transportValid=true;
unsigned starts=0,wakes=0;
int audioFrameQueue=1,captureTaskHandle=1;
bool otaBusy(){return updating;}
void updateStatusCharacteristic(bool){}
void stopStreaming(ErrorCode){streamingEnabled=false;}
bool configureTransportFromPeerMtu(){return transportValid;}
void applyCpuPowerProfile(bool){}
bool startMicrophone(){
  ++starts;
  // Schedule the competing offline worker before START publishes streamingEnabled.
  std::thread contender([]{ChakshuResources::Lease media;assert(!media);});contender.join();
  return microphoneValid;
}
void xQueueReset(int){}
void resetRecovery(){}
void setDeviceState(DeviceState,ErrorCode){}
void xTaskNotifyGive(int){++wakes;assert(streamingEnabled&&ChakshuResources::media.load());}
${start}
int main(){
 {ChakshuResources::Lease media;assert(media);startStreaming(2);assert(!starts&&!streamingEnabled);}
 updating=true;startStreaming(2);assert(!starts&&!ChakshuResources::media);
 updating=false;startStreaming(99);assert(!starts&&!ChakshuResources::media);
 transportValid=false;startStreaming(2);assert(!starts&&!ChakshuResources::media);
 transportValid=true;microphoneValid=false;startStreaming(2);assert(starts==1&&!streamingEnabled&&!ChakshuResources::media);
 microphoneValid=true;startStreaming(2);assert(starts==2&&wakes==1&&streamingEnabled&&!ChakshuResources::media);
 startStreaming(2);assert(starts==2&&wakes==1&&!ChakshuResources::media);
 {ChakshuResources::Lease media;assert(media&&streamingEnabled);}
 puts("PASS atomic START admission");
}`;
  assert.match(nativeTest(fixture,['-pthread']),/PASS atomic START admission/);
});

test('OTA admission excludes media through flash begin and publishes the worker snapshot before releasing',()=>{
  const engine=source.slice(source.indexOf('static void put32le('),source.indexOf('#include <esp_ota_ops.h>'));
  const publish=source.slice(source.indexOf('void otaPublish(bool notify) {'),source.indexOf('void otaInitialize(',source.indexOf('void otaPublish(bool notify) {')));
  const power=source.slice(source.indexOf('bool otaNeedsActiveCpu() {'),source.indexOf('class OtaWriteCallbacks'));
  const tick=source.slice(source.indexOf('void otaTick() {'),source.indexOf('// Model upload protocol 1'));
  let fixture=fs.readFileSync('tests/ota-runtime.cpp','utf8');
  fixture=fixture.replace('// INSERT ENGINE',`#define SYNAP_CHAKSHU 1\n${ownership}\n${engine}`)
    .replace('// INSERT POWER',power).replace('// INSERT PUBLISH',publish).replace('// INSERT TICK',tick)
    .replace('assert(cpuActive);++begins;', 'assert(cpuActive);ChakshuResources::Lease media;assert(!media);++begins;')
    .replace('uint32_t clockNow=100,', 'std::atomic<uint16_t> chakshuConnectionHandle{0};\nuint32_t clockNow=100,');
  fixture=fixture.slice(0,fixture.indexOf('int main(){'))+`
int main(){
 streamingEnabled=false;deviceState=DeviceState::CONNECTED_IDLE;
 {ChakshuResources::Lease media;assert(media);enqueueBegin();tick();
  assert(!backend.begins&&otaSession.error==Synap::BUSY&&!otaBusySnapshot&&ChakshuResources::media);}
 enqueueBegin();tick();assert(backend.begins==1&&otaBusySnapshot&&!ChakshuResources::media);
 {ChakshuResources::Lease media;assert(media&&otaBusySnapshot);}
 enqueueCommand(5);tick();assert(!otaBusySnapshot&&!ChakshuResources::media);
 enqueueBegin();tick();assert(backend.begins==2&&otaBusySnapshot&&!ChakshuResources::media);
 puts("PASS atomic OTA admission");
}`;
  assert.match(nativeTest(fixture),/PASS atomic OTA admission/);
});

test('workers use atomic OTA state and control waits until setup publishes all services',()=>{
  const voice=fs.readFileSync('firmware/xiao-sense/voice.cpp','utf8').split('void cleanup()')[0];
  const transfer=fs.readFileSync('firmware/xiao-sense/media-transfer.cpp','utf8');
  assert.doesNotMatch(voice+transfer,/otaBusy\(\)/);
  assert.match(source,/void controlTask\(void\* parameter\) \{\s+while \(!ChakshuResources::runtimeReady.load\(\)\) vTaskDelay\(1\);/);
  assert.match(source,/initializeBLE\(\);\s+ChakshuResources::runtimeReady.store\(true\);/);
  assert(source.indexOf('class Lease')<source.indexOf('void otaTick() {'));
});
