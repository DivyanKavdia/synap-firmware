// Media extension v1. Small responses share the existing BLE link with audio.
// Only this worker touches its file/frame buffers. BLE callbacks copy requests/results.
#include <img_converters.h>
#include <jpeg_decoder.h>
namespace ChakshuTransfer {
struct Request { uint32_t connection,id,offset;uint8_t operation;char path[64];bool local=false;uint32_t localEpoch=0,windowEpoch=0; };
QueueHandle_t requests=nullptr;
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
uint8_t response[496]{};size_t responseSize=16;uint32_t responseConnection=0;
uint8_t* buffer=nullptr;size_t bufferSize=0;
char selectedPath[64]{},originalPath[64]{};
uint32_t selectedConnection=0;
std::atomic<bool> offline{false},stopRequested{false},photoRequested{false};
std::atomic<uint8_t> offlineMode{0};
std::atomic<uint32_t> localEpoch{0};
ChakshuMedia::Snapshot offlineStatus;

void reply(uint32_t id,uint8_t error,uint32_t total=0,uint32_t offset=0,const uint8_t* bytes=nullptr,size_t size=0,uint32_t connection=connectionGeneration.load()) {
  uint8_t value[496]{};value[0]=0xCB;value[1]=1;value[2]=error?2:1;value[3]=error;
  put32le(value+4,id);put32le(value+8,total);put32le(value+12,offset);
  size=std::min(size,size_t(480));if(size)memcpy(value+16,bytes,size);
  portENTER_CRITICAL(&mux);
  if(connection==connectionGeneration.load()) {
    memcpy(response,value,16+size);responseSize=16+size;responseConnection=connection;
  }
  portEXIT_CRITICAL(&mux);
}
void replyFor(const Request& request,uint8_t error,uint32_t total=0,uint32_t offset=0,const uint8_t* bytes=nullptr,size_t size=0) {
  if(request.local){
    if(error||request.operation==11)ChakshuVoice::mediaCompleted(request.operation,error);
    return;
  }
  reply(request.id,error,total,offset,bytes,size,request.connection);
}
void readResponse(uint8_t* value,size_t& size) {
  portENTER_CRITICAL(&mux);
  if(responseConnection==connectionGeneration.load()) {size=responseSize;memcpy(value,response,size);}
  else {size=16;memset(value,0,size);value[0]=0xCB;value[1]=1;}
  portEXIT_CRITICAL(&mux);
}
void saveOffline(const ChakshuMedia::Snapshot& value) {
  portENTER_CRITICAL(&mux);offlineStatus=value;portEXIT_CRITICAL(&mux);
}
void clearSelection() {
  selectedPath[0]=0;ChakshuStorage::clearProtection();
  free(buffer);buffer=nullptr;bufferSize=0;
}
// File handles never outlive a resource lease: a hardware check can remount SD
// between BLE reads. Retain the path and reopen only while the gate is held.
uint8_t selectFile(const char* path,uint32_t& total) {
  if(!ChakshuStorage::ready)return ChakshuMedia::NO_SD;
  File file=SD.open(path,FILE_READ);
  if(!file||file.isDirectory()){file.close();return ChakshuMedia::NO_SD;}
  total=file.size();file.close();snprintf(selectedPath,sizeof(selectedPath),"%s",path);
  ChakshuStorage::protect(path);return 0;
}
uint8_t readSelection(uint32_t offset,uint32_t& total,uint8_t* bytes,size_t& size) {
  if(selectedPath[0]) {
    if(!ChakshuStorage::ready)return ChakshuMedia::NO_SD;
    File file=SD.open(selectedPath,FILE_READ);
    if(!file||file.isDirectory()){file.close();return ChakshuMedia::IO_ERROR;}
    total=file.size();
    uint8_t error=ChakshuMedia::IO_ERROR;
    if(offset<total){
      size=std::min(size_t(480),size_t(total-offset));
      if(file.seek(offset)&&file.read(bytes,size)==int(size))error=0;
    }
    file.close();return error;
  }
  total=bufferSize;
  if(!buffer||offset>=total)return ChakshuMedia::IO_ERROR;
  size=std::min(size_t(480),size_t(total-offset));memcpy(bytes,buffer+offset,size);return 0;
}
bool validPath(const char* path) {
  const size_t n=strlen(path);if(n<28||n>31||strncmp(path,"/synap/",7)||path[15]!='-'||path[24]!='.')return false;
  for(size_t i=7;i<24;++i)if(i!=15 && !((path[i]>='0'&&path[i]<='9')||(path[i]>='a'&&path[i]<='f')))return false;
  return !strcmp(path+25,"jpg")||!strcmp(path+25,"wav")||!strcmp(path+25,"mjpeg")||!strcmp(path+25,"json");
}
uint8_t captureFrame(bool preview=false) {
  clearSelection();if(!ChakshuCamera::ready)return ChakshuMedia::NO_CAMERA;
  if(!ChakshuCamera::configure(preview))return ChakshuMedia::CAPTURE_ERROR;
  // Discard the old buffer so the PWA timestamps a newly captured frame.
  camera_fb_t* frame=esp_camera_fb_get();if(frame)esp_camera_fb_return(frame);
  frame=esp_camera_fb_get();if(!frame)return ChakshuMedia::CAPTURE_ERROR;
  uint8_t error=ChakshuMedia::OK;
  if(frame->format!=PIXFORMAT_JPEG||frame->len<4||frame->len>250000)error=ChakshuMedia::CAPTURE_ERROR;
  else {
    buffer=static_cast<uint8_t*>(ps_malloc(frame->len));
    if(!buffer)error=ChakshuMedia::CAPTURE_ERROR;
    else {memcpy(buffer,frame->buf,frame->len);bufferSize=frame->len;}
  }
  esp_camera_fb_return(frame);return error;
}
struct PreviewJpeg { uint8_t* bytes;size_t size=0;bool failed=false; };
size_t previewChunk(void* argument,size_t offset,const void* bytes,size_t size) {
  auto& jpeg=*static_cast<PreviewJpeg*>(argument);
  if(!bytes)return 0;
  if(offset>96000 || size>96000-offset){jpeg.failed=true;return 0;}
  memcpy(jpeg.bytes+offset,bytes,size);jpeg.size=offset+size;return size;
}
bool makePreviewFromOriginal() {
  if(!buffer||bufferSize<4)return false;
  esp_jpeg_image_cfg_t config{};
  config.indata=buffer;config.indata_size=bufferSize;
  config.out_format=JPEG_IMAGE_FORMAT_RGB565;config.out_scale=JPEG_IMAGE_SCALE_1_8;
  config.flags.swap_color_bytes=1;
  esp_jpeg_image_output_t info{};
  if(esp_jpeg_get_image_info(&config,&info)!=ESP_OK || !info.width || !info.height ||
     info.width>320 || info.height>240)return false;
  const size_t rgbBytes=size_t(info.width)*info.height*2u;
  uint8_t* rgb=static_cast<uint8_t*>(ps_malloc(rgbBytes));
  PreviewJpeg jpeg{static_cast<uint8_t*>(ps_malloc(96000))};
  if(!rgb||!jpeg.bytes){free(rgb);free(jpeg.bytes);return false;}
  config.outbuf=rgb;config.outbuf_size=rgbBytes;
  esp_jpeg_image_output_t decoded{};
  const bool ok=esp_jpeg_decode(&config,&decoded)==ESP_OK && decoded.width==info.width &&
    decoded.height==info.height && decoded.output_len==rgbBytes &&
    fmt2jpg_cb(rgb,rgbBytes,info.width,info.height,PIXFORMAT_RGB565,70,previewChunk,&jpeg) &&
    !jpeg.failed && jpeg.size>4 && jpeg.size<bufferSize;
  free(rgb);
  if(ok){free(buffer);buffer=jpeg.bytes;bufferSize=jpeg.size;jpeg.bytes=nullptr;}
  free(jpeg.bytes);return ok;
}
uint8_t captureSavedPreview() {
  clearSelection();originalPath[0]=0;
  if(!ChakshuStorage::ready)return ChakshuMedia::NO_SD;
  if(!ChakshuCamera::beginOriginal())return ChakshuMedia::CAPTURE_ERROR;
  // Discard the stale frame after the mode switch, then retain one full-quality exposure.
  camera_fb_t* frame=esp_camera_fb_get();if(frame)esp_camera_fb_return(frame);
  frame=esp_camera_fb_get();
  uint8_t error=ChakshuMedia::OK;
  if(!frame||frame->format!=PIXFORMAT_JPEG||frame->len<4||frame->len>2u*1024u*1024u)error=ChakshuMedia::CAPTURE_ERROR;
  else {
    buffer=static_cast<uint8_t*>(ps_malloc(frame->len));
    if(!buffer)error=ChakshuMedia::CAPTURE_ERROR;
    else {memcpy(buffer,frame->buf,frame->len);bufferSize=frame->len;}
  }
  if(frame)esp_camera_fb_return(frame);
  if(error){ChakshuCamera::endOriginal();return error;}
  if(!ChakshuStorage::ensureSpace(bufferSize)){ChakshuCamera::endOriginal();clearSelection();return ChakshuMedia::NO_SPACE;}
  File original=ChakshuStorage::create(originalPath,sizeof(originalPath),"jpg");
  if(!original){ChakshuCamera::endOriginal();clearSelection();originalPath[0]=0;return ChakshuMedia::IO_ERROR;}
  ChakshuStorage::protect(originalPath);
  const bool saved=original.write(buffer,bufferSize)==bufferSize;
  original.flush();original.close();ChakshuStorage::clearProtection();
  if(!saved){ChakshuStorage::ready=false;originalPath[0]=0;ChakshuCamera::endOriginal();clearSelection();return ChakshuMedia::IO_ERROR;}
  // Derive the preview from this exact full-resolution exposure. The original
  // stays on SD until the app durably verifies and acknowledges its move.
  makePreviewFromOriginal();
  ChakshuCamera::endOriginal();return 0;
}
uint8_t catalogue() {
  clearSelection();if(!ChakshuStorage::ready)return ChakshuMedia::NO_SD;
  File directory=SD.open("/synap");if(!directory)return ChakshuMedia::IO_ERROR;
  String json="[";unsigned count=0;
  for(File entry=directory.openNextFile();entry;entry=directory.openNextFile()) {
    String path=entry.path();
    if(!entry.isDirectory()&&validPath(path.c_str())&&(path.endsWith(".jpg")||path.endsWith(".mjpeg")||(path.endsWith(".wav")&&!SD.exists(path.substring(0,path.length()-4)+".mjpeg")))) {
      if(count++)json+=",";
      json+="{\"path\":\""+path+"\",\"bytes\":"+String(entry.size())+"}";
    }
    entry.close();if(count>=100)break;
  }
  directory.close();json+="]";
  buffer=static_cast<uint8_t*>(ps_malloc(json.length()));if(!buffer)return ChakshuMedia::CAPTURE_ERROR;
  bufferSize=json.length();memcpy(buffer,json.c_str(),bufferSize);return 0;
}

void recordOffline(bool withVideo=true) {
  ChakshuMedia::Snapshot s;
  {portENTER_CRITICAL(&mux);s=offlineStatus;portEXIT_CRITICAL(&mux);}
  ChakshuRecorder::record(s,withVideo,stopRequested,saveOffline);
  ChakshuMedia::refresh(s);saveOffline(s);
  photoRequested=false;offline=false;offlineMode=0;
}

bool chakshuAudioHasBacklog();
BLECharacteristic* streamCharacteristic=nullptr;
std::atomic<uint16_t> subscribedConnection{BLE_HS_CONN_HANDLE_NONE};
std::atomic<uint32_t> cancelWindow{0};
bool sendMediaPacket(const Request& request,uint8_t kind,uint8_t error,uint32_t total,uint32_t offset,const uint8_t* bytes,size_t size) {
  const uint16_t connection=chakshuConnectionHandle.load();
  if(!deviceConnected.load() || request.connection!=connectionGeneration.load() ||
     subscribedConnection.load()!=connection || !streamCharacteristic)return false;
  const uint16_t mtu=bleServer->getPeerMTU(connection);
  if(mtu<19 || size>480 || size>size_t(mtu-19))return false;
  uint8_t packet[496]{};packet[0]=0xCC;packet[1]=1;packet[2]=kind;packet[3]=error;
  put32le(packet+4,request.id);put32le(packet+8,total);put32le(packet+12,offset);
  if(size)memcpy(packet+16,bytes,size);
  constexpr int reserve=8;
  if(os_msys_num_free()<=reserve)return false;
  os_mbuf* mbuf=ble_hs_mbuf_from_flat(packet,16+size);
  if(!mbuf)return false;
  if(os_msys_num_free()<reserve){os_mbuf_free_chain(mbuf);return false;}
  return ble_gattc_notify_custom(connection,streamCharacteristic->getHandle(),mbuf)==0;
}
void streamWindow(const Request& request) {
  const uint32_t started=millis(),cancel=request.windowEpoch;
  uint32_t offset=request.offset,total=0;uint8_t bytes[480],error=0;
  const uint16_t mtu=bleServer->getPeerMTU(chakshuConnectionHandle.load());
  const size_t capacity=mtu>19?std::min(size_t(480),size_t(mtu-19)):0;
  if(!capacity){replyFor(request,ChakshuMedia::BAD_COMMAND);return;}
  for(unsigned count=0;count<8 && millis()-started<2000u;) {
    if(cancel!=cancelWindow.load() || request.connection!=connectionGeneration.load() || !deviceConnected.load())return;
    if(streamingEnabled.load() && chakshuAudioHasBacklog()){vTaskDelay(pdMS_TO_TICKS(20));continue;}
    size_t size=0;error=readSelection(offset,total,bytes,size);if(error)break;
    size=std::min(size,capacity);
    if(sendMediaPacket(request,1,0,total,offset,bytes,size)){offset+=size;++count;if(offset>=total)break;}
    vTaskDelay(pdMS_TO_TICKS(streamingEnabled.load()?15:4));
  }
  // Missing notifications are requested again by byte offset. Never retry the
  // photo exposure or queue an unbounded stream ahead of audio and Stop.
  for(unsigned n=0;n<8 && cancel==cancelWindow.load();++n) {
    if(sendMediaPacket(request,2,error,total,offset,nullptr,0))break;
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}
void worker(void*) {
  Request request{};
  for(;;) {
    if(xQueueReceive(requests,&request,portMAX_DELAY)!=pdTRUE)continue;
    if(request.local&&request.localEpoch!=localEpoch.load())continue;
    if(!request.local&&(request.connection!=connectionGeneration.load()||!deviceConnected.load()))continue;
    ChakshuResources::Lease admission;
    if(!admission||otaBusySnapshot.load()) {replyFor(request,1);continue;}
    if(selectedConnection!=request.connection){clearSelection();originalPath[0]=0;selectedConnection=request.connection;}
    if(request.operation==5||request.operation==10) {
      if(streamingEnabled.load()||remoteStandby){replyFor(request,1);continue;}
      ChakshuCamera::VideoProfile profile;
      const uint32_t seconds=request.operation==5?(request.offset>>8):request.offset;
      if(seconds>600u){replyFor(request,ChakshuMedia::BAD_COMMAND);continue;}
      if(request.operation==5 && !ChakshuCamera::videoProfile(request.offset&255,profile)){replyFor(request,ChakshuMedia::BAD_COMMAND);continue;}
      clearSelection();stopRequested.store(false);photoRequested.store(false);offlineMode.store(request.operation);offline.store(true);
      ChakshuMedia::Snapshot s;s.state=1;s.operation=request.operation==5?4:3;
      if(request.operation==5){s.videoProfile=uint8_t(request.offset&255);s.width=profile.width;s.height=profile.height;s.targetFps=profile.fps;s.clipLimitMs=(seconds?seconds:10u)*1000u;}
      else s.clipLimitMs=(seconds?seconds:600u)*1000u;
      saveOffline(s);
      replyFor(request,0);recordOffline(request.operation==5);
      if(request.local){ChakshuMedia::Snapshot done;portENTER_CRITICAL(&mux);done=offlineStatus;portEXIT_CRITICAL(&mux);ChakshuVoice::mediaCompleted(request.operation,done.error);}
      continue;
    }
    if(request.operation==20) {
      if(streamingEnabled.load()||remoteStandby){replyFor(request,1);continue;}
      if(!ChakshuStorage::ready){replyFor(request,ChakshuMedia::NO_SD);continue;}
      clearSelection();
      if(!ChakshuWifi::begin()){replyFor(request,10);continue;}
      char json[256];const size_t size=ChakshuWifi::encode(json,sizeof(json));
      replyFor(request,0,size,0,reinterpret_cast<const uint8_t*>(json),size);
      ChakshuWifi::serve();continue;
    }
    uint8_t error=0;uint32_t total=0;size_t size=0;uint8_t bytes[480];
    switch(request.operation) {
      // Offset 1 is an optional QVGA video hint; legacy offset 0 retains VGA photos.
      case 1:error=captureFrame(request.offset==1);total=bufferSize;break;
      case 12:streamWindow(request);if(!streamingEnabled.load())stopMicrophone();continue;
      case 13:error=captureSavedPreview();total=bufferSize;break;
      case 15:total=size=strlen(originalPath);memcpy(bytes,originalPath,size);break;
      case 11: {
        ChakshuMedia::Snapshot s;
        if(!ChakshuStorage::ready)error=ChakshuMedia::NO_SD;
        else if(!ChakshuCamera::beginOriginal())error=ChakshuMedia::NO_CAMERA;
        else {
          File file=ChakshuStorage::create(s.path,sizeof(s.path),"jpg");
          camera_fb_t* frame=esp_camera_fb_get();if(frame)esp_camera_fb_return(frame);
          frame=esp_camera_fb_get();
          if(!file||!frame||frame->format!=PIXFORMAT_JPEG||frame->len<4)error=ChakshuMedia::CAPTURE_ERROR;
          else if(!ChakshuStorage::ensureSpace(frame->len)||file.write(frame->buf,frame->len)!=frame->len)error=ChakshuMedia::IO_ERROR;
          if(frame)esp_camera_fb_return(frame);if(file){file.flush();file.close();}
          ChakshuCamera::endOriginal();
        }
        s.operation=2;s.state=error?3:2;s.error=error;ChakshuMedia::refresh(s);ChakshuMedia::save(s);break;
      }
      case 2:case 4:
        error=readSelection(request.offset,total,bytes,size);
        break;
      case 3:
        clearSelection();
        if(!validPath(request.path)){error=2;break;}
        error=selectFile(request.path,total);
        break;
      case 14: {
        if(streamingEnabled.load()){error=ChakshuMedia::BUSY;break;}
        error=ChakshuStorage::begin(true)?0:ChakshuMedia::NO_SD;
        ChakshuMedia::Snapshot s;ChakshuMedia::copy(s);ChakshuMedia::refresh(s);ChakshuMedia::save(s);
        break;
      }
      case 17: {
        if(!validPath(request.path)){error=ChakshuMedia::BAD_COMMAND;break;}
        clearSelection();
        error=ChakshuStorage::removeCapture(request.path)?0:ChakshuMedia::IO_ERROR;
        break;
      }
      case 18: {
        if(streamingEnabled.load()){error=ChakshuMedia::BUSY;break;}
        clearSelection();
        total=ChakshuStorage::clearCaptures();
        break;
      }
      case 7:error=catalogue();total=bufferSize;break;
      case 8:total=bufferSize;break;
      default:error=2;
    }
    replyFor(request,error,total,request.offset,bytes,error?0:size);
    if(!streamingEnabled.load())stopMicrophone();
  }
}
class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const String value=characteristic->getValue();
    if(value.length()<10||value.length()>73||uint8_t(value[0])!=0xCA)return;
    const uint8_t* p=reinterpret_cast<const uint8_t*>(value.c_str());Request request{};
    request.operation=p[1];memcpy(&request.id,p+2,4);memcpy(&request.offset,p+6,4);
    request.connection=connectionGeneration.load();request.windowEpoch=cancelWindow.load();memcpy(request.path,p+10,value.length()-10);
    if(request.operation==6){stopRequested.store(true);replyFor(request,0);return;}
    if(request.operation==16){++cancelWindow;replyFor(request,0);return;}
    if(request.operation==21 || request.operation==22) {
      if(request.operation==22)ChakshuWifi::stopRequested=true;
      char json[256];const size_t size=ChakshuWifi::encode(json,sizeof(json));
      replyFor(request,0,size,0,reinterpret_cast<const uint8_t*>(json),size);return;
    }
    if(request.operation==9) {
      ChakshuMedia::Snapshot s;portENTER_CRITICAL(&mux);s=offlineStatus;portEXIT_CRITICAL(&mux);
      char json[384];const int size=snprintf(json,sizeof(json),"{\"active\":%s,\"state\":%u,\"error\":%u,\"progress\":%u,\"path\":\"%s\",\"audioMs\":%lu,\"frames\":%lu,\"droppedFrames\":%lu,\"width\":%u,\"height\":%u,\"targetFps\":%u,\"videoProfile\":%u,\"clipLimitMs\":%lu}",offline.load()?"true":"false",s.state,s.error,s.progress,s.path,(unsigned long)s.audioMs,(unsigned long)s.frames,(unsigned long)s.droppedFrames,s.width,s.height,s.targetFps,s.videoProfile,(unsigned long)s.clipLimitMs);
      replyFor(request,0,size,0,reinterpret_cast<const uint8_t*>(json),size);return;
    }
    if(offline.load()||!requests||xQueueSend(requests,&request,0)!=pdTRUE)replyFor(request,1);
  }
};
class DataCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* characteristic) override {
    uint8_t value[496];size_t size;
    readResponse(value,size);
    characteristic->setValue(value,size);
  }
};
class StreamCallbacks : public BLECharacteristicCallbacks {
  void onSubscribe(BLECharacteristic*,NimBLEConnInfo& peer,uint16_t flags) override {
    if(peer.getConnHandle()==chakshuConnectionHandle.load())
      subscribedConnection=flags&1?peer.getConnHandle():BLE_HS_CONN_HANDLE_NONE;
  }
};
void initialize() {
  requests=xQueueCreate(2,sizeof(Request));
  if(!requests||xTaskCreatePinnedToCore(worker,"chakshu-transfer",8192,nullptr,1,nullptr,1)!=pdPASS){if(requests)vQueueDelete(requests);requests=nullptr;}
  reply(0,0);
}
void ble(BLEService* service) {
  auto* command=service->createCharacteristic("4fa12354-0000-1000-8000-00805f9b34fb",
    BLECharacteristic::PROPERTY_WRITE|BLECharacteristic::PROPERTY_WRITE_NR);
  command->setCallbacks(new CommandCallbacks());
  auto* data=service->createCharacteristic("4fa12355-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_READ);
  data->setCallbacks(new DataCallbacks());
  streamCharacteristic=service->createCharacteristic("4fa1235a-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_NOTIFY);
  streamCharacteristic->setCallbacks(new StreamCallbacks());
}
} // namespace ChakshuTransfer
