'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {assemble}=require('../tools/assemble-source.cjs');
const {materialize}=require('../tools/materialize-target.cjs');
const {nativeTest}=require('./support/native.cjs');

test('Chakshu alone uses native callbacks, owned commands and synchronous audio results',()=>{
  const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
  assert.match(source,/#include <NimBLEDevice.h>/);
  assert.doesNotMatch(source,/#include <BLE|audioCharacteristic->notify\(|getData\(/);
  assert.match(source,/void onRead\(NimBLECharacteristic\* characteristic, NimBLEConnInfo&\) override/);
  assert.match(source,/memcpy\(message.data,written.data\(\),size\)/);
  assert.match(source,/sendChakshuAudio\(packet,AUDIO_HEADER_BYTES\+length\)/);
  const boot=source.slice(source.indexOf('void setup() {'));
  assert.match(source,/namespace ChakshuVoice/);
  assert.match(source,/namespace ChakshuTinyModel/);
  assert.match(source,/LEARNED_WEIGHT_BYTES/);
  assert.doesNotMatch(source,/esp_afe_sr|esp_mn_|model_path|SYNAP_EMBEDDED_SR_MODEL_START/);
  assert.match(source,/4fa12356-0000-1000-8000-00805f9b34fb/);
  assert.match(source,/4fa12357-0000-1000-8000-00805f9b34fb/);
  assert(boot.indexOf('xTaskCreatePinnedToCore(acquisitionTask')<boot.indexOf('initializeBLE();'));
  for(const target of ['esp32s3-fh4r2-qspi-4m','esp32c3-supermini-4m']) {
    const other=materialize(assemble(),target);
    assert.match(other,/#include <BLEDevice.h>/);
    assert.doesNotMatch(other,/NimBLEDevice.h|sendChakshuAudio|ChakshuVoice|ChakshuModel|esp_afe|esp_mn|srmodels|4fa1235[67]-/);
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
struct os_mbuf {std::vector<uint8_t> bytes;int blocks;};
bool allocationFails=false;int allocations=0,submits=0,live=0,result=0,freeBuffers=12,blocksPerPacket=2;
int os_msys_num_free(){return freeBuffers;}
std::vector<uint8_t> sent;
os_mbuf* ble_hs_mbuf_from_flat(const uint8_t* bytes,size_t length){
 ++allocations;if(allocationFails)return nullptr;++live;freeBuffers-=blocksPerPacket;return new os_mbuf{{bytes,bytes+length},blocksPerPacket};
}
void os_mbuf_free_chain(os_mbuf* p){freeBuffers+=p->blocks;delete p;--live;}
int ble_gattc_notify_custom(uint16_t connection,uint16_t handle,os_mbuf* packet){
 assert(connection==7&&handle==42&&packet);++submits;sent=packet->bytes;os_mbuf_free_chain(packet);return result;
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
 freeBuffers=8;assert(sendChakshuAudio(packet,sizeof(packet))&&freeBuffers==8);
 blocksPerPacket=5;const int submittedBefore=submits;
 assert(!sendChakshuAudio(packet,sizeof(packet))&&submits==submittedBefore&&live==0&&freeBuffers==8);
 freeBuffers=4;const int allocatedBefore=allocations;
 assert(!sendChakshuAudio(packet,sizeof(packet))&&allocations==allocatedBefore&&lastNotifyError==6);
 blocksPerPacket=2;
 freeBuffers=12;notifyRejected=0;
 allocationFails=true;assert(!sendChakshuAudio(packet,sizeof(packet))&&submits==2&&notifyRejected==1&&lastNotifyError==6);
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

test('Chakshu congested frame retries continue at the first unsent fragment and reset for a new owner',()=>{
  const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
  const codec=source.slice(source.indexOf('static const uint16_t IMA_STEP_TABLE'),source.indexOf('void transmitterTask(void* parameter) {'));
  const transport=source.slice(source.indexOf('bool configureTransportFromPeerMtu() {'),source.indexOf('void startStreaming(uint8_t version) {'));
  let fixture=fs.readFileSync('tests/audio-runtime.cpp','utf8').split('int main(){')[0];
  const host=`std::atomic<uint16_t> chakshuConnectionHandle{0};
int budget=2;
bool sendChakshuAudio(const uint8_t* bytes,size_t length){
 if(budget==0){++notifyRejected;return false;}--budget;
 characteristic.setValue(bytes,length);characteristic.notify();return true;
}
`;
  fixture=fixture.replace('// INSERT CODEC AND TRANSPORT',host+transport+codec)+`
int main(){
 server.mtu=517;assert(configureTransportFromPeerMtu());assert(chunksPerFrame==4);
 AudioFrame frame{1,7,{}};for(int i=0;i<800;++i)frame.samples[i]=i-400;
 assert(!sendAudioFrame(frame));assert(characteristic.packets.size()==2);
 for(int i=2;i<4;++i){budget=1;assert(sendAudioFrame(frame)==(i==3));}
 assert(characteristic.packets.size()==4);
 for(int i=0;i<4;++i){auto& p=characteristic.packets[i].bytes;assert(p[4]==i);
   assert(!memcmp(p.data()+8,reinterpret_cast<uint8_t*>(frame.samples)+i*400,400));}
 // Completed frames can be intentionally replayed in full.
 budget=1;assert(!sendAudioFrame(frame));assert(characteristic.packets.back().bytes[4]==0);
 ++audioReplayGeneration;budget=1;assert(!sendAudioFrame(frame));assert(characteristic.packets.back().bytes[4]==0);
 ++connectionGeneration;budget=1;assert(!sendAudioFrame(frame));assert(characteristic.packets.back().bytes[4]==0);
 ++streamGeneration;frame.generation=streamGeneration;budget=1;
 assert(!sendAudioFrame(frame));assert(characteristic.packets.back().bytes[4]==0);
 ++frame.sequence;budget=1;assert(!sendAudioFrame(frame));assert(characteristic.packets.back().bytes[4]==0);
 server.mtu=185;assert(configureTransportFromPeerMtu());budget=1;
 assert(!sendAudioFrame(frame));assert(characteristic.packets.back().bytes[4]==0);
 puts("PASS partial frame progress");
}`;
  assert.match(nativeTest(fixture),/PASS partial frame progress/);
});

test('Chakshu control writes enqueue original commands without replacing readable status',()=>{
  const code=fs.readFileSync('firmware/xiao-sense/ble-control.cpp','utf8');
  const fixture=`#include <atomic>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <vector>
#define CONTROL_CHAR_UUID "control"
constexpr int CMD_STOP=0,PROTOCOL_VERSION=2;
namespace ChakshuTransfer {std::atomic<uint32_t> cancelWindow{0};}
namespace NIMBLE_PROPERTY {constexpr int READ=1,WRITE=2,WRITE_NR=4,NOTIFY=8;}
struct NimBLEConnInfo {uint16_t handle;uint16_t getConnHandle(){return handle;}};
struct NimBLECharacteristic {
 std::vector<uint8_t> status;
 NimBLECharacteristic(const char*,int flags,int length){assert(flags==15&&length==16);}
 virtual void writeEvent(const uint8_t* bytes,uint16_t length,NimBLEConnInfo&){status.assign(bytes,bytes+length);}
};
std::atomic<bool> deviceConnected{true};
std::atomic<uint16_t> chakshuConnectionHandle{7};std::atomic<uint32_t> streamGeneration{9};
enum class EventType {COMMAND};
struct Command {uint8_t command,version;uint32_t generation;};std::vector<Command> commands;
void queueEvent(EventType,uint8_t command,uint8_t version,uint32_t generation){commands.push_back({command,version,generation});}
${code}
int main(){
 ChakshuControlCharacteristic control;NimBLECharacteristic& characteristic=control;
 // The last published status stays STREAMING during the entire recovery drain.
 characteristic.status={0x5a,2,2,0,5,2,2,2,4,8,128,62,32,3,144,1};
 const auto status=characteristic.status;NimBLEConnInfo owner{7},stranger{8};
 uint8_t stop[]={0,2};
 for(int i=0;i<8;++i){characteristic.writeEvent(stop,2,owner);assert(characteristic.status==status);}
 assert(commands.size()==8);for(const auto& c:commands)assert(c.command==0&&c.version==2&&c.generation==9);
 stop[0]=1;assert(commands.back().command==0);
 characteristic.writeEvent(stop,2,stranger);deviceConnected=false;characteristic.writeEvent(stop,2,owner);
 assert(commands.size()==8);deviceConnected=true;
 characteristic.writeEvent(nullptr,0,owner);assert(commands.back().command==255&&commands.back().version==0);
 assert(characteristic.status==status);
 characteristic.status[2]=1;characteristic.writeEvent(stop,2,owner);assert(characteristic.status[2]==1);
 puts("PASS isolated control commands");
}`;
  assert.match(nativeTest(fixture),/PASS isolated control commands/);
});

test('a connected replay of the in-flight frame is not advanced by its previous submission',()=>{
  const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
  const ring=source.slice(source.indexOf('namespace SynapRecovery {'),source.indexOf('SynapRecovery::Ring recoveryRing;'));
  const send=source.slice(source.indexOf('bool sendRecoveryFrame() {'),source.indexOf('void processRecoveryRequest() {'));
  const fixture=`#include <atomic>
#include <cstdint>
#include <cassert>
#include <cstdio>
struct AudioFrame {uint32_t generation;uint16_t sequence;int16_t samples[800];};
${ring}
SynapRecovery::Ring recoveryRing;
struct RecoveryGuard {};
std::atomic<uint32_t> connectionGeneration{1},streamGeneration{1},notifyRejected{0},audioReplayGeneration{0};
std::atomic<bool> recoveryWaiting{false},streamingEnabled{true},deviceConnected{true};
std::atomic<uint8_t> chunksPerFrame{4};
enum class ErrorCode{TRANSPORT_CHANGED};
void requestStreamError(ErrorCode,uint32_t){assert(false);}
int pdMS_TO_TICKS(int n){return n;}void vTaskDelay(int){}
bool replay=true;
bool sendCapturedFrame(const AudioFrame&,uint32_t){
 if(replay){replay=false;recoveryRing.after(9);++audioReplayGeneration;}return true;
}
${send}
int main(){
 AudioFrame frames[2]={{1,9,{}},{1,10,{}}};recoveryRing.frames=frames;recoveryRing.capacity=2;recoveryRing.count=2;recoveryRing.cursor=1;
 assert(sendRecoveryFrame()&&recoveryRing.cursor==1);
 assert(sendRecoveryFrame()&&recoveryRing.cursor==2);
 puts("PASS in-flight replay ownership");
}`;
  assert.match(nativeTest(fixture,['-Wno-unused-variable']),/PASS in-flight replay ownership/);
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

test('Chakshu text characteristics exclude C array terminators and padding with pinned NimBLE overloads',{skip:!process.env.SYNAP_NIMBLE_SRC},()=>{
  const dir=process.env.SYNAP_NIMBLE_SRC;
  assert.match(fs.readFileSync(path.join(dir,'../library.properties'),'utf8'),/^version=2\.3\.6$/m);
  const valueHeader=fs.readFileSync(path.join(dir,'NimBLEAttValue.h'),'utf8').replace('#include "nimconfig.h"','#define CONFIG_BT_ENABLED 1');
  const localHeader=fs.readFileSync(path.join(dir,'NimBLELocalValueAttribute.h'),'utf8');
  const methods=localHeader.slice(localHeader.indexOf('    void setValue(const uint8_t*'),localHeader.indexOf('  protected:'));
  assert(methods.includes('m_value.setValue<T>(val)'));
  const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
  // Extract production call sites, allowing the path's explicit byte-pointer cast.
  const deviceCall=source.match(/deviceIdentity->setValue\([^;]+;/)[0];
  const firmwareCall=source.match(/identity->setValue\(SYNAP_FIRMWARE_ID[^;]+;|identity->setValue\(reinterpret_cast[^;]+;/)[0];
  const pathCallback=source.slice(source.indexOf('class PathCallbacks'),source.indexOf('void initialize()',source.indexOf('class PathCallbacks')));
  const pathCall=pathCallback.match(/characteristic->setValue\([^;]+;/)[0];
  const fixture=`#include <cassert>
#include <cstdio>
${valueHeader}
std::vector<uint8_t> wire;
NimBLEAttValue::NimBLEAttValue(uint16_t,uint16_t) {}
NimBLEAttValue::~NimBLEAttValue() = default;
bool NimBLEAttValue::setValue(const uint8_t* data,uint16_t length) {wire.assign(data,data+length);return true;}
struct Characteristic { NimBLEAttValue m_value;
${methods}
};
int main(){
 Characteristic c;auto* deviceIdentity=&c;auto* identity=&c;auto* characteristic=&c;
 char synapDeviceId[19]="SYNAP-68EE8F4719A0";
 const char SYNAP_FIRMWARE_ID[]="SYNAP-FW:xiao-esp32s3-sense-8m:synap-os1-build1227:1227";
 struct {char path[64]{};} s;
 // Reproduce 1227 exactly: mutable arrays select the generic sizeof(T) overload.
 c.setValue(synapDeviceId);assert(wire.size()==19&&wire.back()==0);
 c.setValue(s.path);assert(wire.size()==64&&wire.front()==0);
 c.setValue(SYNAP_FIRMWARE_ID);assert(wire.size()==strlen(SYNAP_FIRMWARE_ID));
 ${deviceCall}
 assert(wire.size()==18&&std::string(wire.begin(),wire.end())=="SYNAP-68EE8F4719A0");
 ${firmwareCall}
 assert(wire.size()==strlen(SYNAP_FIRMWARE_ID)&&wire.back()=='7');
 ${pathCall}
 assert(wire.empty());
 strcpy(s.path,"/synap/abcdef01-00000001.jpg");
 ${pathCall}
 assert(std::string(wire.begin(),wire.end())==s.path);
 puts("PASS exact GATT text bytes");
}`;
  assert.match(nativeTest(fixture,['-Wno-unused-parameter']),/PASS exact GATT text bytes/);
});

for (const target of ['esp32s3-fh4r2-qspi-4m','esp32c3-supermini-4m','xiao-esp32s3-sense-8m']) test(target + ': ring eviction preserves partially sent PCM',()=>{
 const source=materialize(assemble(),target);
 const ring=source.slice(source.indexOf('namespace SynapRecovery {'),source.indexOf('SynapRecovery::Ring recoveryRing;'));
 const send=source.slice(source.indexOf('bool sendRecoveryFrame() {'),source.indexOf('void processRecoveryRequest() {'));
 const progress=source.slice(source.indexOf('class AudioSendProgress {'),source.indexOf('bool sendEncodedFrame(uint32_t generation,uint16_t sequence,const uint8_t* encoded,uint32_t paceUs,bool pcm) {'));
 const fixture=`#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
struct AudioFrame {uint32_t generation;uint16_t sequence;int16_t samples[800];};
${ring}
${progress}
SynapRecovery::Ring recoveryRing;
struct RecoveryGuard {};
std::atomic<uint32_t> connectionGeneration{1},streamGeneration{1},notifyRejected{0},audioReplayGeneration{0};
std::atomic<bool> recoveryWaiting{false},streamingEnabled{true},deviceConnected{true};
std::atomic<uint8_t> chunksPerFrame{4};
enum class ErrorCode {TRANSPORT_CHANGED};
void requestStreamError(ErrorCode,uint32_t){assert(false);}
int pdMS_TO_TICKS(int n){return n;}void vTaskDelay(int){}
std::vector<int16_t> delivered;
std::vector<uint16_t> sequences;
bool sendCapturedFrame(const AudioFrame& frame,uint32_t){
 const auto chunk=audioSendProgress.begin(frame.generation,connectionGeneration,audioReplayGeneration,frame.sequence,4,400,true);
 sequences.push_back(frame.sequence);
 delivered.insert(delivered.end(),frame.samples+chunk*200,frame.samples+(chunk+1)*200);
 audioSendProgress.accept(chunk);
 if(chunk==3){audioSendProgress.reset();return true;}
 ++notifyRejected;return false;
}
${send}
int main(){
 AudioFrame storage[2];recoveryRing.frames=storage;recoveryRing.capacity=2;
 AudioFrame frame{1,9,{}};for(int i=0;i<800;++i)frame.samples[i]=i-400;recoveryRing.push(frame);
 assert(!sendRecoveryFrame());
 for(int call=1;call<4;++call){
  // More than a ring's worth of fresh capture arrives during each BLE retry.
  for(int j=0;j<3;++j){++frame.sequence;for(int i=0;i<800;++i)frame.samples[i]=1234;recoveryRing.push(frame);}
  assert(sendRecoveryFrame()==(call==3));
 }
 assert(delivered.size()==800);
 for(int i=0;i<800;++i)assert(delivered[i]==i-400);
 for(auto sequence:sequences)assert(sequence==9);
 // Sending the evicted frame must not acknowledge unsent newer ring frames.
 assert(recoveryRing.cursor==0);
 assert(!sendRecoveryFrame()&&sequences.back()>9);
 const auto old=sequences.back();++audioReplayGeneration;recoveryRing.after(frame.sequence);
 assert(!sendRecoveryFrame());assert(sequences.back()==old); // Empty replay: no stale held-frame send.
 puts("PASS complete frame survives ring overflow");
}`;
 assert.match(nativeTest(fixture,['-Wno-unused-variable']),/PASS complete frame survives ring overflow/);
});
