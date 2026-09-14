#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <vector>
#define NIMBLE_LOGD(...) ((void)0)
#define SLIST_NEXT(p,unused) ((p)->next)
constexpr int BLE_HS_CONN_HANDLE_NONE=65535;
constexpr int BLE_GATT_ACCESS_OP_READ_CHR=1,BLE_GATT_ACCESS_OP_READ_DSC=2;
constexpr int BLE_GATT_ACCESS_OP_WRITE_CHR=3,BLE_GATT_ACCESS_OP_WRITE_DSC=4;
constexpr int BLE_ATT_ERR_INSUFFICIENT_RES=17,BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN=13,BLE_ATT_ERR_UNLIKELY=14;
struct os_mbuf {uint16_t om_len,om_pkthdr_len;uint8_t* om_data;os_mbuf* next=nullptr;};
struct ble_gatt_access_ctxt {int op;os_mbuf* om;};
struct NimBLEConnInfo {int m_desc=0;};
void ble_gap_conn_find(uint16_t,int*){}
void ble_npl_hw_enter_critical(){}
void ble_npl_hw_exit_critical(int){}
std::vector<uint8_t> response;
int os_mbuf_append(os_mbuf*,const uint8_t* bytes,size_t size){response.assign(bytes,bytes+size);return 0;}
struct NimBLEAttValue {
  std::vector<uint8_t> bytes;
  const uint8_t* data()const{return bytes.data();}size_t size()const{return bytes.size();}
  uint16_t max_size()const{return 512;}
};
struct NimBLELocalValueAttribute {
  NimBLEAttValue value;int reads=0;std::vector<std::vector<uint8_t>> writes;
  const NimBLEAttValue& getAttVal(){return value;}
  void readEvent(NimBLEConnInfo&){++reads;value.bytes={0xCD,1,uint8_t(reads),1};}
  void writeEvent(const uint8_t* bytes,uint16_t len,NimBLEConnInfo&){writes.emplace_back(bytes,bytes+len);}
};
struct NimBLEServer {static int handleGattEvent(uint16_t,uint16_t,ble_gatt_access_ctxt*,void*);};
// PINNED GATT HANDLER
int main(){
  NimBLELocalValueAttribute attribute;
  // A normal ATT read has one response-opcode byte. The packet-header length
  // is 8: Arduino BLE 3.3.5's >8 guard incorrectly suppresses this callback.
  uint8_t opcode=0x0b;os_mbuf buffer{1,8,&opcode};
  ble_gatt_access_ctxt request{BLE_GATT_ACCESS_OP_READ_CHR,&buffer};
  assert(NimBLEServer::handleGattEvent(7,42,&request,&attribute)==0);
  assert(attribute.reads==1&&response==std::vector<uint8_t>({0xCD,1,1,1}));
  assert(NimBLEServer::handleGattEvent(7,42,&request,&attribute)==0&&attribute.reads==2&&response[2]==2);
  buffer.om_len=0;assert(NimBLEServer::handleGattEvent(7,42,&request,&attribute)==0&&attribute.reads==2);
  buffer.om_len=1;assert(NimBLEServer::handleGattEvent(65535,42,&request,&attribute)==0&&attribute.reads==2);
  uint8_t first[]={0xcc,1,1},second[]={0xcc,1,0};request.op=BLE_GATT_ACCESS_OP_WRITE_CHR;
  buffer.om_len=3;buffer.om_data=first;assert(NimBLEServer::handleGattEvent(7,42,&request,&attribute)==0);
  buffer.om_data=second;assert(NimBLEServer::handleGattEvent(7,42,&request,&attribute)==0);
  assert(attribute.writes==std::vector<std::vector<uint8_t>>({{0xcc,1,1},{0xcc,1,0}}));
  // Long writes arrive in a chain and must be copied once, in order.
  os_mbuf tail{3,0,second};buffer.next=&tail;
  assert(NimBLEServer::handleGattEvent(7,42,&request,&attribute)==0);
  assert(attribute.writes.back()==std::vector<uint8_t>({0xcc,1,0,0xcc,1,0}));
  const size_t count=attribute.writes.size();buffer.om_len=513;
  assert(NimBLEServer::handleGattEvent(7,42,&request,&attribute)==13&&attribute.writes.size()==count);
  puts("PASS live GATT values");
}
