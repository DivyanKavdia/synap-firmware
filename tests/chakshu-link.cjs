'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const {assemble}=require('../tools/assemble-source.cjs');
const {materialize}=require('../tools/materialize-target.cjs');

test('the pinned NimBLE host already negotiates data length after the connect callback',
  {skip:!process.env.SYNAP_NIMBLE_SRC},()=>{
  const source=fs.readFileSync(process.env.SYNAP_NIMBLE_SRC+'/nimble/nimble/host/src/ble_gap.c','utf8');
  const start=source.indexOf('void\nble_gap_event_connect_call('),end=source.indexOf('\nvoid\nble_gap_rx_rd_rem_sup_feat_complete',start);
  assert(start>=0&&end>start);
  assert.match(nativeTest(`
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
#define NIMBLE_BLE_CONNECT 1
#define MYNEWT_VAL(x) 0
#undef le16toh
#define le16toh(n) (n)
constexpr int BLE_GAP_EVENT_CONNECT=1,BLE_GAP_EVENT_LINK_ESTAB=2;
constexpr int BLE_HCI_SUGG_DEF_DATALEN_TX_OCTETS_MAX=251,BLE_HCI_SUGG_DEF_DATALEN_TX_TIME_MAX=2120;
struct ble_gap_event {int type;struct {int status;uint16_t conn_handle;} connect,link_estab;};
int connects=0,requests=0;bool callbackRequestsData=false;
void ble_gap_event_listener_call(ble_gap_event*){}
void ble_hs_hci_util_set_data_len(uint16_t handle,int length,int time){
  assert(handle==7 && length==251 && time==2120);++requests;
}
void ble_gap_call_conn_event_cb(ble_gap_event* event,uint16_t handle){
  if(event->type==BLE_GAP_EVENT_CONNECT){
    ++connects;
    if(callbackRequestsData)ble_hs_hci_util_set_data_len(handle,251,2120);
  }
}
${source.slice(start,end)}
int main(){
  callbackRequestsData=true;ble_gap_event_connect_call(7,0);assert(connects==1 && requests==2);
  callbackRequestsData=false;connects=requests=0;
  ble_gap_event_connect_call(7,0);assert(connects==1 && requests==1);
  puts("PASS one host-owned data-length request");
}`),/PASS one host-owned/);
});

test('Chakshu keeps connect callbacks passive and retains early-disconnect evidence across repeated links',()=>{
  const health=fs.readFileSync('firmware/xiao-sense/ble-health.cpp','utf8');
  const callbacks=fs.readFileSync('firmware/xiao-sense/ble-server.cpp','utf8');
  const result=nativeTest(`
#include <atomic>
#include <cstdint>
#include <cassert>
#include <cstdio>
constexpr uint16_t BLE_HS_CONN_HANDLE_NONE=65535;
namespace ChakshuTransfer {std::atomic<uint16_t> subscribedConnection{65535};std::atomic<uint32_t> cancelWindow{0};}
namespace ChakshuVoice {void linkConnected(){} void linkDisconnected(){}}
constexpr int BLE_MIN_INTERVAL=12,BLE_MAX_INTERVAL=24,BLE_SLAVE_LATENCY=0,BLE_SUPERVISION_TIMEOUT=600;
uint32_t clockMs=1000;uint32_t millis(){return clockMs;}
std::atomic<bool> deviceConnected{false},chakshuAudioSubscribed{false},streamingEnabled{false};
std::atomic<bool> recoveryWaiting{false},recoveryEnabled{false},connectionEventPending{false};
std::atomic<uint32_t> connectionGeneration{0},recoveryWaitingAt{0},linkDisconnects{0},lastDisconnectAt{0};
std::atomic<uint16_t> chakshuConnectionHandle{65535},lastDisconnectReason{65535};
struct NimBLEConnInfo {
  uint16_t handle,interval=24,latency=0,timeout=600;
  uint16_t getConnHandle()const{return handle;}
  uint16_t getConnInterval()const{return interval;}
  uint16_t getConnLatency()const{return latency;}
  uint16_t getConnTimeout()const{return timeout;}
};
struct NimBLEServer {
  int updates=0,dataRequests=0,rejected=0;
  void disconnect(uint16_t){++rejected;}
  void updateConnParams(uint16_t,int,int,int,int){++updates;}
  void setDataLen(uint16_t,int){++dataRequests;}
};
struct NimBLEServerCallbacks {
  virtual void onConnect(NimBLEServer*,NimBLEConnInfo&){}
  virtual void onDisconnect(NimBLEServer*,NimBLEConnInfo&,int){}
  virtual void onConnParamsUpdate(NimBLEConnInfo&){}
};
${health}
${callbacks}
uint32_t u32(const uint8_t* b,unsigned o){return uint32_t(b[o])|(uint32_t(b[o+1])<<8)|(uint32_t(b[o+2])<<16)|(uint32_t(b[o+3])<<24);}
int main(){
  NimBLEServer server;ServerCallbacks implementation;NimBLEServerCallbacks& cb=implementation;
  ChakshuLink::bootReadyMs=940;ChakshuLink::mediaBootMs=410;
  for(unsigned i=0;i<30;++i){
    NimBLEConnInfo peer{uint16_t(i)},other{uint16_t(i+100)};
    cb.onConnect(&server,peer);
    assert(deviceConnected && connectionEventPending && chakshuConnectionHandle==i);
    assert(server.updates==0 && server.dataRequests==0); // no optional LL/L2CAP procedures at startup
    assert(!ChakshuLink::statusSeen && !chakshuAudioSubscribed);
    cb.onConnect(&server,other);assert(server.rejected==int(i+1));
    cb.onDisconnect(&server,other,0x208);assert(deviceConnected && linkDisconnects==i);
    clockMs+=7250;
    chakshuAudioSubscribed=true;
    if(i%3)ChakshuLink::statusSeen=true;
    if(i%3==2){streamingEnabled=true;recoveryEnabled=true;}
    const uint8_t stage=i%3==0?2:i%3==1?3:4;
    cb.onDisconnect(&server,peer,0x213);
    assert(!deviceConnected && !chakshuAudioSubscribed && chakshuConnectionHandle==65535);
    assert(lastDisconnectReason==0x213 && linkDisconnects==i+1);
    assert(ChakshuLink::lastDurationMs==7250 && ChakshuLink::lastStage==stage);
    uint8_t bytes[84]={};ChakshuLink::append(bytes,false,false,false,clockMs);
    assert(u32(bytes,48)==940 && u32(bytes,52)==410 && u32(bytes,56)==7250);
    assert(bytes[60]==24 && bytes[62]==0 && bytes[64]==88 && bytes[65]==2);
    assert(bytes[66]==stage && bytes[67]==0 && u32(bytes,68)==0);
    if(i%3==2)assert(recoveryWaiting && streamingEnabled); // same-journal recovery is preserved
    recoveryWaiting=false;recoveryEnabled=false;streamingEnabled=false;recoveryWaitingAt=0;
  }
  // Millisecond rollover must not turn a short connection into a huge duration.
  NimBLEConnInfo peer{80};clockMs=0xfffffff0;cb.onConnect(&server,peer);clockMs=64;
  cb.onDisconnect(&server,peer,0x208);assert(ChakshuLink::lastDurationMs==80);
  puts("PASS passive startup and retained link evidence");
}`);
  assert.match(result,/PASS passive startup/);
});

test('only Chakshu extends diagnostics and uses a fixed 20 ms advertising interval',()=>{
  const source=materialize(assemble(),'xiao-esp32s3-sense-8m');
  assert.match(source,/DIAGNOSTICS_VERSION = 4/);
  assert.match(source,/uint8_t value\[84\] = \{\}/);
  assert.match(source,/reconcileConnection\(\);\s+serviceChakshuLink\(\);/);
  assert.match(source,/ChakshuLink::append\(value,deviceConnected.load\(\)/);
  assert.match(source,/command==CMD_GET_STATUS && version==PROTOCOL_VERSION\) ChakshuLink::statusSeen=true/);
  assert.match(source,/setMinInterval\(32\)/);assert.match(source,/setMaxInterval\(32\)/);
  for(const target of ['esp32s3-fh4r2-qspi-4m','esp32c3-supermini-4m']){
    const other=materialize(assemble(),target);
    assert.match(other,/DIAGNOSTICS_VERSION = 2/);
    assert.match(other,/uint8_t value\[48\] = \{\}/);
    assert.doesNotMatch(other,/ChakshuLink::|setMinInterval\(32\)/);
  }
});
