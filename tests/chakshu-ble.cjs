'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {assemble}=require('../tools/assemble-source.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const {nativeTest}=require('./support/native.cjs');

test('Chakshu alone uses native callbacks, owned commands and synchronous audio results',()=>{
  const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
  assert.match(source,/#include <NimBLEDevice.h>/);
  assert.doesNotMatch(source,/#include <BLE|audioCharacteristic->notify\(|getData\(/);
  assert.match(source,/void onRead\(NimBLECharacteristic\* c, NimBLEConnInfo&\)override/);
  assert.match(source,/memcpy\(message.data,written.data\(\),size\)/);
  assert.match(source,/sendChakshuAudio\(packet,AUDIO_HEADER_BYTES\+length\)/);
  const boot=source.slice(source.indexOf('void setup() {'));
  assert(boot.indexOf('ChakshuVoice::initialize();')<boot.indexOf('initializeBLE();'));
  assert(boot.indexOf('xTaskCreatePinnedToCore(acquisitionTask')<boot.indexOf('initializeBLE();'));
  for(const target of ['esp32s3-fh4r2-qspi-4m','esp32c3-supermini-4m']) {
    const other=materialize(assemble(),target);
    assert.match(other,/#include <BLEDevice.h>/);
    assert.doesNotMatch(other,/NimBLEDevice.h|sendChakshuAudio/);
  }
});

test('Chakshu audio checks subscription, owns mbufs, and reports allocation/congestion failures',()=>{
  const source=fs.readFileSync('firmware/xiao-sense/ble-audio.cpp','utf8');
  const fixture=`#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <vector>
constexpr uint16_t BLE_HS_CONN_HANDLE_NONE=65535;constexpr int BLE_HS_ENOMEM=6;
std::atomic<uint16_t> chakshuConnectionHandle{7},lastNotifyStatus{0};
std::atomic<bool> deviceConnected{true},chakshuAudioSubscribed{false};
std::atomic<uint32_t> lastNotifyError{0},notifyRejected{0};
struct NimBLEConnInfo {uint16_t id;uint16_t getConnHandle(){return id;}};
struct NimBLECharacteristic {uint16_t getHandle(){return 42;}} characteristic;
auto* audioCharacteristic=&characteristic;
struct NimBLECharacteristicCallbacks {virtual void onSubscribe(NimBLECharacteristic*,NimBLEConnInfo&,uint16_t){}};
struct os_mbuf {std::vector<uint8_t> bytes;};
bool allocationFails=false;int allocations=0,submits=0,live=0,result=0;
std::vector<uint8_t> sent;
os_mbuf* ble_hs_mbuf_from_flat(const uint8_t* bytes,size_t length){
 ++allocations;if(allocationFails)return nullptr;++live;return new os_mbuf{{bytes,bytes+length}};
}
int ble_gattc_notify_custom(uint16_t connection,uint16_t handle,os_mbuf* packet){
 assert(connection==7&&handle==42&&packet);++submits;sent=packet->bytes;delete packet;--live;return result;
}
${source}
int main(){
 AudioCallbacks callback;NimBLECharacteristicCallbacks& base=callback;NimBLEConnInfo peer{7},stranger{8};
 uint8_t packet[408];for(size_t i=0;i<sizeof(packet);++i)packet[i]=uint8_t(i);
 assert(!sendChakshuAudio(packet,sizeof(packet))&&allocations==0);
 base.onSubscribe(&characteristic,stranger,1);assert(!chakshuAudioSubscribed);
 base.onSubscribe(&characteristic,peer,2);assert(!chakshuAudioSubscribed);
 base.onSubscribe(&characteristic,peer,1);assert(chakshuAudioSubscribed);
 assert(sendChakshuAudio(packet,sizeof(packet))&&sent==std::vector<uint8_t>(packet,packet+408));
 allocationFails=true;assert(!sendChakshuAudio(packet,sizeof(packet))&&submits==1&&notifyRejected==1&&lastNotifyError==6);
 allocationFails=false;result=6;assert(!sendChakshuAudio(packet,sizeof(packet))&&notifyRejected==2&&live==0);
 result=0;for(int i=0;i<10000;++i)assert(sendChakshuAudio(packet,sizeof(packet)));
 assert(live==0&&notifyRejected==2);
 deviceConnected=false;const int before=allocations;assert(!sendChakshuAudio(packet,sizeof(packet))&&allocations==before);
 deviceConnected=true;chakshuConnectionHandle=65535;assert(!sendChakshuAudio(packet,sizeof(packet))&&allocations==before);
 puts("PASS native audio ownership");
}`;
  assert.match(nativeTest(fixture),/PASS native audio ownership/);
});

test('Chakshu native submission preserves PCM, frame pacing, retries and cancellation across all MTUs',()=>{
  const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
  const codec=source.slice(source.indexOf('static const uint16_t IMA_STEP_TABLE'),source.indexOf('void transmitterTask(void* parameter) {'));
  const transport=source.slice(source.indexOf('bool configureTransportFromPeerMtu() {'),source.indexOf('void startStreaming(uint8_t version) {'));
  let fixture=fs.readFileSync('tests/audio-runtime.cpp','utf8');
  // The host enqueue implementation is covered above; exercise the generated
  // packetizer against a host that accepts, delays or rejects each exact packet.
  const host=`std::atomic<uint16_t> chakshuConnectionHandle{0};
bool sendChakshuAudio(const uint8_t* bytes,size_t length){
  const auto before=notifyRejected.load();
  characteristic.setValue(bytes,length);characteristic.notify();
  return before==notifyRejected.load();
}
`;
  fixture=fixture.replace('// INSERT CODEC AND TRANSPORT',host+transport+codec);
  assert.match(nativeTest(fixture),/PASS runtime; codec golden=3749bea1db6af550/);
});

test('pinned NimBLE handles short reads and consecutive writes with live values',{skip:!process.env.SYNAP_NIMBLE_SRC},()=>{
  const dir=process.env.SYNAP_NIMBLE_SRC;
  assert.match(fs.readFileSync(path.join(dir,'../library.properties'),'utf8'),/^version=2\.3\.6$/m);
  const source=fs.readFileSync(path.join(dir,'NimBLEServer.cpp'),'utf8');
  const start=source.indexOf('int NimBLEServer::handleGattEvent(');
  const end=source.indexOf('} // handleGattEvent',start);
  assert(start>=0&&end>start);
  const handler=source.slice(start,end+1);
  const fixture=fs.readFileSync('tests/chakshu-gatt.cpp','utf8').replace('// PINNED GATT HANDLER',handler);
  assert.match(nativeTest(fixture,['-Wno-unused-parameter']),/PASS live GATT values/);
});
