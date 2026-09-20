'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');

test('short supervision is corrected once outside connect; busy retries and rejected requests are bounded',()=>{
  const read=name=>fs.readFileSync('firmware/xiao-sense/'+name+'.cpp','utf8');
  const result=nativeTest(`
#include <atomic>
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <functional>
constexpr uint16_t BLE_HS_CONN_HANDLE_NONE=65535;
namespace ChakshuTransfer {std::atomic<uint16_t> subscribedConnection{65535};std::atomic<uint32_t> cancelWindow{0},localEpoch{0};}
namespace ChakshuVoice {void linkConnected(){} void linkDisconnected(){}}
constexpr int BLE_MIN_INTERVAL=12,BLE_MAX_INTERVAL=24,BLE_SLAVE_LATENCY=0,BLE_SUPERVISION_TIMEOUT=600;
constexpr int BLE_HS_EALREADY=2,BLE_HS_EBUSY=15,BLE_GAP_INITIAL_CONN_MIN_CE_LEN=0,BLE_GAP_INITIAL_CONN_MAX_CE_LEN=0;
uint32_t clockMs=1000;uint32_t millis(){return clockMs;}
std::atomic<bool> deviceConnected{false},chakshuAudioSubscribed{false},streamingEnabled{false};
std::atomic<bool> recoveryWaiting{false},recoveryEnabled{false},connectionEventPending{false};
std::atomic<uint32_t> connectionGeneration{0},recoveryWaitingAt{0},linkDisconnects{0},lastDisconnectAt{0};
std::atomic<uint16_t> chakshuConnectionHandle{65535},lastDisconnectReason{65535};
struct NimBLEConnInfo {
  uint16_t handle=7,interval=24,latency=0,timeout=72;
  uint16_t getConnHandle()const{return handle;}uint16_t getConnInterval()const{return interval;}
  uint16_t getConnLatency()const{return latency;}uint16_t getConnTimeout()const{return timeout;}
};
struct NimBLEServer {void disconnect(uint16_t){}};
struct NimBLEServerCallbacks {
  virtual void onConnect(NimBLEServer*,NimBLEConnInfo&){}
  virtual void onDisconnect(NimBLEServer*,NimBLEConnInfo&,int){}
  virtual void onConnParamsUpdate(NimBLEConnInfo&){}
};
struct ble_gap_upd_params {uint16_t itvl_min,itvl_max,latency,supervision_timeout,min_ce_len,max_ce_len;};
int requests=0,returnCode=0;std::function<void()> duringRequest;
int ble_gap_update_params(uint16_t handle,const ble_gap_upd_params* p){
  assert(deviceConnected && handle==chakshuConnectionHandle);
  assert(p->itvl_min==12 && p->itvl_max==24 && p->latency==0 && p->supervision_timeout==600);
  assert(p->min_ce_len==0 && p->max_ce_len==0);++requests;
  if(duringRequest)duringRequest();
  return returnCode;
}
${read('ble-health')}
${read('ble-link')}
${read('ble-server')}
unsigned u16(const uint8_t* b,unsigned o){return unsigned(b[o])|(unsigned(b[o+1])<<8);}
int main(){
  NimBLEServer server;ServerCallbacks implementation;NimBLEServerCallbacks& cb=implementation;
  NimBLEConnInfo peer,other;other.handle=8;
  auto connect=[&](unsigned timeout){
    if(deviceConnected)cb.onDisconnect(&server,peer,0x208);
    clockMs+=2000;peer.timeout=timeout;cb.onConnect(&server,peer);
  };
  auto tick=[](unsigned ms){clockMs+=ms;serviceChakshuLink();};
  connect(72);assert(requests==0);tick(999);assert(requests==0);tick(1);assert(requests==1);
  assert(ChakshuLink::timeout==72 && ChakshuLink::paramRequestCode==0); // submission is not acceptance
  for(int i=0;i<100;++i)tick(1000);
  assert(requests==1);
  peer.timeout=600;cb.onConnParamsUpdate(peer);assert(ChakshuLink::timeout==600);
  other.timeout=72;cb.onConnParamsUpdate(other);assert(ChakshuLink::timeout==600);
  uint8_t b[84]={};ChakshuLink::append(b,true,false,false,clockMs);
  assert(u16(b,72)==24 && u16(b,74)==0 && u16(b,76)==600 && b[78]==1 && u16(b,80)==0);
  peer.timeout=72;cb.onConnParamsUpdate(peer);tick(5000);assert(requests==1); // central wins, no tug of war
  cb.onDisconnect(&server,peer,0x208);tick(5000);assert(requests==1);
  ChakshuLink::append(b,false,false,false,clockMs);
  assert(u16(b,64)==72 && u16(b,76)==0 && b[79]==1 && u16(b,82)==0);
  connect(600);tick(5000);assert(requests==1 && ChakshuLink::paramRequests==0);
  connect(1800);tick(5000);assert(requests==1); // already robust central settings stay untouched
  connect(72);returnCode=BLE_HS_EBUSY;tick(1000);assert(requests==2);
  tick(999);assert(requests==2);tick(1);assert(requests==3);
  returnCode=0;tick(1000);assert(requests==4 && ChakshuLink::paramRequests==3);
  tick(5000);assert(requests==4);
  connect(72);returnCode=BLE_HS_EALREADY;
  for(int i=0;i<100;++i)tick(1000);
  assert(requests==7 && ChakshuLink::paramRequests==3);
  connect(72);returnCode=0x23b;tick(1000);tick(10000);assert(requests==8); // permanent rejection
  connect(72);returnCode=BLE_HS_EBUSY;tick(1000);peer.timeout=600;cb.onConnParamsUpdate(peer);
  tick(5000);assert(requests==9); // central update completes while our retry waits
  connect(72);tick(999);cb.onDisconnect(&server,peer,0x208);
  cb.onConnect(&server,peer);tick(1);assert(requests==9); // recycled handle gets a fresh delay
  tick(999);assert(requests==10);
  duringRequest=[&](){cb.onDisconnect(&server,peer,0x208);cb.onConnect(&server,peer);};
  connect(72);returnCode=0;tick(1000);duringRequest=nullptr;
  assert(requests==11 && ChakshuLink::paramRequests==0 && ChakshuLink::paramRequestCode==0xffff);
  tick(999);assert(requests==11);tick(1);assert(requests==12);
  cb.onDisconnect(&server,peer,0x208);clockMs=0xfffffff0;peer.timeout=72;cb.onConnect(&server,peer);
  tick(999);assert(requests==12);tick(1);assert(requests==13); // clock rollover
  puts("PASS supervision delay, central acceptance, bounded retry and stale-link ownership");
}`);
  assert.match(result,/PASS supervision delay/);
});
