#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#define CONFIG_NIMBLE_ENABLED 1
#define log_d(...) ((void)0)
#define SLIST_NEXT(p, unused) ((p)->next)
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
constexpr int BLE_HS_CONN_HANDLE_NONE=65535, BLE_ATT_ATTR_MAX_LEN=512;
constexpr int BLE_GATT_ACCESS_OP_READ_CHR=1, BLE_GATT_ACCESS_OP_WRITE_CHR=3;
constexpr int BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN=13, BLE_ATT_ERR_INSUFFICIENT_RES=17, BLE_ATT_ERR_UNLIKELY=14;
struct ble_uuid_t {};
struct UUID { struct { ble_uuid_t u; } native; auto* getNative(){return &native;} std::string toString(){return "control";} };
int ble_uuid_cmp(const ble_uuid_t*,const ble_uuid_t*){return 0;}
struct ble_gap_conn_desc {};
int ble_gap_conn_find(uint16_t,ble_gap_conn_desc*){return 0;}
struct os_mbuf {size_t om_len,om_pkthdr_len; uint8_t* om_data; os_mbuf* next=nullptr;};
struct Chr {ble_uuid_t* uuid;};
struct ble_gatt_access_ctxt {int op;os_mbuf* om;Chr* chr;};
std::vector<uint8_t> response;
int os_mbuf_append(os_mbuf*,uint8_t* data,size_t size){response.assign(data,data+size);return 0;}
struct Value {std::string value; std::string getValue(){return value;}};
struct BLECharacteristic;
struct BLECharacteristicCallbacks {
  virtual void onRead(BLECharacteristic*){}
  virtual void onRead(BLECharacteristic* c,ble_gap_conn_desc*){onRead(c);}
  virtual void onWriteValue(BLECharacteristic*,ble_gap_conn_desc*,const uint8_t*,size_t){}
};
struct BLECharacteristic {
  Value m_value;BLECharacteristicCallbacks* m_pCallbacks=nullptr;
  UUID getUUID(){return {};}
  void setValue(const uint8_t* data,size_t size){m_value.value.assign(reinterpret_cast<const char*>(data),size);}
  static int handleGATTServerEvent(uint16_t,uint16_t,ble_gatt_access_ctxt*,void*);
};
BLECharacteristic control;
std::atomic<uint32_t> streamGeneration{9};
enum class EventType { COMMAND };
std::vector<std::vector<uint8_t>> commands;
void queueEvent(EventType,uint8_t command,uint8_t version,uint32_t generation){
  assert(generation==9);commands.push_back({command,version});
}
int reads=0;
void updateStatusCharacteristic(bool notify){
  assert(!notify);++reads;
  uint8_t value[16]={0x5a,2,1,0};control.setValue(value,sizeof(value));
}
// INSERT CALLBACKS
// INSERT HANDLER
int main(){
  ControlCallbacks callbacks;control.m_pCallbacks=&callbacks;
  uint8_t opcode=0x0b;os_mbuf buffer{1,8,&opcode};ble_uuid_t uuid;Chr chr{&uuid};
  ble_gatt_access_ctxt request{BLE_GATT_ACCESS_OP_READ_CHR,&buffer,&chr};
  // A normal short ATT read has an 8-byte packet header; it still needs onRead.
  assert(BLECharacteristic::handleGATTServerEvent(7,42,&request,&control)==0);
  assert(reads==1 && response.size()==16 && response[0]==0x5a);
  assert(BLECharacteristic::handleGATTServerEvent(7,42,&request,&control)==0 && reads==2);
  buffer.om_len=0;
  assert(BLECharacteristic::handleGATTServerEvent(7,42,&request,&control)==0 && reads==2);
  buffer.om_len=1;
  assert(BLECharacteristic::handleGATTServerEvent(65535,42,&request,&control)==0 && reads==2);
  // Consecutive writes retain their bytes even with an intervening status read.
  request.op=BLE_GATT_ACCESS_OP_WRITE_CHR;
  for(uint8_t command:{2,1,0,1,0}){
    uint8_t bytes[]={command,2};buffer.om_data=bytes;buffer.om_len=2;
    assert(BLECharacteristic::handleGATTServerEvent(7,42,&request,&control)==0);
    assert(commands.back()==std::vector<uint8_t>({command,2}));
    assert(control.m_value.value.size()==16 && uint8_t(control.m_value.value[0])==0x5a);
    updateStatusCharacteristic(false);
  }
  assert(commands.size()==5);
  // Long-write chains are copied in order; invalid lengths stay invalid.
  uint8_t first=1,second=2;os_mbuf tail{1,0,&second};
  buffer.om_data=&first;buffer.om_len=1;buffer.next=&tail;
  assert(BLECharacteristic::handleGATTServerEvent(7,42,&request,&control)==0);
  assert(commands.back()==std::vector<uint8_t>({1,2}));
  buffer.om_len=513;const size_t count=commands.size();
  assert(BLECharacteristic::handleGATTServerEvent(7,42,&request,&control)==13 && commands.size()==count);
  puts("PASS pinned S3/C3 live reads and immutable command writes");
}
