#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
using String=std::string;
#define log_v(...) ((void)0)
#define log_w(...) ((void)0)
#define log_e(...) ((void)0)
#define ARDUHAL_LOG_LEVEL 0
#define ARDUHAL_LOG_LEVEL_VERBOSE 5
constexpr int BLE_HS_ENOMEM=6,ESP_OK=0,NIMBLE_SUB_NOTIFY=1,NIMBLE_SUB_INDICATE=2;
constexpr int BLE_GATT_CHR_F_READ_AUTHEN=4,BLE_GATT_CHR_F_READ_AUTHOR=8,BLE_GATT_CHR_F_READ_ENC=16;
struct ble_gap_conn_desc {struct {bool encrypted=true;} sec_state;};
int ble_gap_conn_find(int,ble_gap_conn_desc*){return 0;}
bool noBuffers=false;int submits=0,result=0,status=-1;uint32_t statusCode=0;
std::string delivered;
struct os_mbuf {std::string bytes;};
os_mbuf* ble_hs_mbuf_from_flat(uint8_t* bytes,size_t length){return noBuffers?nullptr:new os_mbuf{{reinterpret_cast<char*>(bytes),length}};}
void os_mbuf_free_chain(os_mbuf* packet){delete packet;}
int ble_gatts_notify_custom(int,int,os_mbuf* packet){
 ++submits;assert(packet && "null must not trigger a characteristic read");
 delivered=packet->bytes;delete packet;return result;
}
int ble_gatts_indicate_custom(int c,int h,os_mbuf* p){return ble_gatts_notify_custom(c,h,p);}
struct Server {int getConnectedCount(){return 1;}int getPeerMTU(int){return 517;}bool setIndicateWait(int){return true;}void clearIndicateWait(int){}} server;
struct BLEDevice {static Server* getServer(){return &server;}};
struct Service {Server* getServer(){return &server;}} service;
struct BLECharacteristic;
struct BLECharacteristicCallbacks {
 enum Status {ERROR_NO_CLIENT,ERROR_NO_SUBSCRIBER,ERROR_NOTIFY_DISABLED,ERROR_INDICATE_DISABLED,ERROR_GATT,ERROR_INDICATE_TIMEOUT,SUCCESS_INDICATE,ERROR_INDICATE_FAILURE,SUCCESS_NOTIFY};
 void onNotify(BLECharacteristic*){}
 void onStatus(BLECharacteristic*,Status value,uint32_t code){status=value;statusCode=code;}
} callbacks;
struct BLECharacteristic {
 static constexpr int PROPERTY_NOTIFY=32,PROPERTY_INDICATE=64;
 int m_properties=PROPERTY_NOTIFY,m_handle=42,indicationTimeout=100;
 BLECharacteristicCallbacks* m_pCallbacks=&callbacks;
 std::map<int,int> m_subscribedVec{{7,1}};
 struct Value {std::string bytes;std::string getValue(){return bytes;}} m_value;
 struct Semaphore {void take(const char*){}void give(){}bool timedWait(const char*,int){return true;}int value(){return 0;}} m_semaphoreConfEvt;
 Service* getService(){return &service;}String getValue(){return m_value.bytes;}
 void notify(bool);
};
// INSERT NOTIFY
int main(){
 BLECharacteristic audio;audio.m_value.bytes=std::string(408,'A');
 noBuffers=true;audio.notify(true);
 assert(submits==0 && status==BLECharacteristicCallbacks::ERROR_GATT && statusCode==6);
 noBuffers=false;audio.notify(true);
 assert(submits==1 && delivered==audio.m_value.bytes && status==BLECharacteristicCallbacks::SUCCESS_NOTIFY);
 result=6;audio.notify(true);
 assert(submits==2 && status==BLECharacteristicCallbacks::ERROR_GATT && statusCode==6);
 puts("PASS allocation failure cannot enqueue a null notification");
}
