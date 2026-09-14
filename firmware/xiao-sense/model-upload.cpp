// The control task owns this bounded SD transaction. BLE callbacks only enqueue.
#include <mbedtls/sha256.h>
namespace ChakshuModel {
constexpr char FINAL_PATH[]="/synap/models/srmodels.bin";
constexpr char PART_PATH[]="/synap/models/srmodels.part";
constexpr char BACKUP_PATH[]="/synap/models/srmodels.bak";
std::atomic<bool> reserved{false};
bool busy(){return reserved.load();}
bool restoreBackup(){
  return SD.exists(FINAL_PATH)||!SD.exists(BACKUP_PATH)||SD.rename(BACKUP_PATH,FINAL_PATH);
}
class SdBackend : public Backend {
  File file;
public:
  Error begin()override{
    if(otaBusy()||streamingEnabled.load()||remoteStandby)return BUSY;
    bool expected=false;
    if(!ChakshuMedia::busy.compare_exchange_strong(expected,true))return BUSY;
    reserved=true;
    if(!ChakshuStorage::ready){abort();return NO_SD;}
    ChakshuStorage::refresh();
    if(ChakshuStorage::freeBytes<MODEL_BYTES+ChakshuStorage::RESERVE_BYTES){abort();return NO_SPACE;}
    if((!SD.exists("/synap/models")&&!SD.mkdir("/synap/models"))||!restoreBackup()||
       (SD.exists(PART_PATH)&&!SD.remove(PART_PATH))){abort();return IO_ERROR;}
    file=SD.open(PART_PATH,FILE_WRITE);
    if(!file){abort();return IO_ERROR;}
    return OK;
  }
  bool write(const uint8_t* bytes,size_t size)override{return file&&file.write(bytes,size)==size;}
  Error finish()override{
    file.flush();file.close();
    File check=SD.open(PART_PATH,FILE_READ);
    if(!check||check.size()!=MODEL_BYTES){check.close();return IO_ERROR;}
    mbedtls_sha256_context hash;mbedtls_sha256_init(&hash);
    int result=mbedtls_sha256_starts(&hash,0);
    uint8_t bytes[2048],digest[32];size_t count=0;
    while(!result&&count<MODEL_BYTES){
      const size_t size=std::min(sizeof(bytes),MODEL_BYTES-count);
      if(check.read(bytes,size)!=int(size)){result=-1;break;}
      result=mbedtls_sha256_update(&hash,bytes,size);count+=size;vTaskDelay(1);
    }
    if(!result)result=mbedtls_sha256_finish(&hash,digest);
    mbedtls_sha256_free(&hash);check.close();
    if(result)return IO_ERROR;
    char hex[65]{};for(int i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",digest[i]);
    if(strcmp(hex,MODEL_SHA256))return HASH_MISMATCH;
    // Keep the previous valid file until the new file is verified. Boot recovers
    // the backup if power was lost between these two FAT renames.
    if(SD.exists(BACKUP_PATH)&&!SD.remove(BACKUP_PATH))return IO_ERROR;
    if(SD.exists(FINAL_PATH)&&!SD.rename(FINAL_PATH,BACKUP_PATH))return IO_ERROR;
    if(!SD.rename(PART_PATH,FINAL_PATH)){restoreBackup();return IO_ERROR;}
    reserved=false;ChakshuMedia::busy=false;return OK;
  }
  void abort()override{
    if(file)file.close();
    // The staging file can be overwritten on retry. Never delete recordings.
    if(reserved.exchange(false))ChakshuMedia::busy=false;
  }
};
SdBackend backend;Upload upload(backend);
struct Message {uint32_t connection;uint16_t length;uint8_t bytes[489];};
QueueHandle_t queue=nullptr;
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
uint8_t snapshot[20]{};
uint32_t rebootAt=0;
void publish(){
  uint8_t p[20]{};p[0]=0xCE;p[1]=1;p[2]=upload.state;p[3]=upload.error;
  put32le(p+4,upload.session);put32le(p+8,upload.offset);put32le(p+12,MODEL_BYTES);
  const uint16_t mtu=deviceConnected.load()?bleServer->getPeerMTU(bleServer->getConnId()):23;
  const uint16_t data=std::min(uint16_t(480),uint16_t(std::max(uint16_t(23),mtu)-12));
  p[16]=data&255;p[17]=data>>8;p[18]=ChakshuStorage::ready?1:0;p[19]=queue?1:0;
  portENTER_CRITICAL(&mux);memcpy(snapshot,p,20);portEXIT_CRITICAL(&mux);
}
void tick(){
  if(rebootAt){if(uint32_t(millis()-rebootAt)>1500u)ESP.restart();return;}
  upload.tick(millis());
  Message message;
  for(uint8_t i=0;queue&&i<4&&xQueueReceive(queue,&message,0)==pdTRUE;++i){
    if(!deviceConnected.load()||message.connection!=connectionGeneration.load())continue;
    if(message.length==5&&message.bytes[0]==4&&Upload::u32(message.bytes+1)==upload.session&&upload.state==INSTALLED){
      bool expected=false;
      if(!otaBusy()&&!streamingEnabled.load()&&ChakshuMedia::busy.compare_exchange_strong(expected,true)){
        reserved=true;rebootAt=millis();upload.state=RESTARTING;upload.error=OK;publish();return;
      }else upload.error=BUSY;
      continue;
    }
    upload.packet(message.bytes,message.length,message.connection,millis());
  }
  publish(); // Expose VERIFYING while the SD file is reread.
  upload.verify();publish();
}
void initialize(){
  if(ChakshuStorage::ready)restoreBackup();
  queue=xQueueCreate(8,sizeof(Message));publish();
}
class WriteCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c)override{
    const String value=c->getValue();
    if(value.length()<5||value.length()>489||!queue)return;
    Message message{};message.connection=connectionGeneration.load();message.length=value.length();
    memcpy(message.bytes,value.c_str(),message.length);
    // On overflow no bytes are acknowledged. The client resumes at the actual offset.
    xQueueSend(queue,&message,0);
  }
};
class ReadCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* c)override{
    uint8_t p[20];portENTER_CRITICAL(&mux);memcpy(p,snapshot,20);portEXIT_CRITICAL(&mux);c->setValue(p,20);
  }
};
void ble(BLEService* service){
  auto* write=service->createCharacteristic("4fa12358-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_WRITE);
  write->setCallbacks(new WriteCallbacks());
  auto* read=service->createCharacteristic("4fa12359-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_READ);
  read->setCallbacks(new ReadCallbacks());
}
}
