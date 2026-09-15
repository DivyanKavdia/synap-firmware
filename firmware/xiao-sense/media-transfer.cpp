// Media extension v1. Small responses share the existing BLE link with audio.
// Only this worker touches its file/frame buffers. BLE callbacks copy requests/results.
namespace ChakshuTransfer {
struct Request { uint32_t connection,id,offset;uint8_t operation;char path[64];bool local=false;uint32_t localEpoch=0; };
QueueHandle_t requests=nullptr;
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
uint8_t response[496]{};size_t responseSize=16;uint32_t responseConnection=0;
uint8_t* buffer=nullptr;size_t bufferSize=0;
char selectedPath[64]{};
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
void clearSelection() { selectedPath[0]=0;free(buffer);buffer=nullptr;bufferSize=0; }
// File handles never outlive a resource lease: a hardware check can remount SD
// between BLE reads. Retain the path and reopen only while the gate is held.
uint8_t selectFile(const char* path,uint32_t& total) {
  if(!ChakshuStorage::ready)return ChakshuMedia::NO_SD;
  File file=SD.open(path,FILE_READ);
  if(!file||file.isDirectory()){file.close();return ChakshuMedia::NO_SD;}
  total=file.size();file.close();snprintf(selectedPath,sizeof(selectedPath),"%s",path);return 0;
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
uint8_t captureFrame() {
  clearSelection();if(!ChakshuCamera::ready)return ChakshuMedia::NO_CAMERA;
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
  using namespace ChakshuMedia;
  Snapshot s;{portENTER_CRITICAL(&mux);s=offlineStatus;portEXIT_CRITICAL(&mux);}
  uint8_t error=0;File video,audio,index;uint32_t audioBytes=0,videoBytes=0,frames=0;
  char audioPath[64]{},indexPath[64]{};uint8_t header[44];
  if(!ChakshuStorage::ready)error=NO_SD;
  else if(withVideo&&!ChakshuCamera::ready)error=NO_CAMERA;
  else if(!startMicrophone())error=NO_MIC;
  if(!error) {
    if(!withVideo) {
      audio=ChakshuStorage::create(s.path,sizeof(s.path),"wav");
      if(!audio)error=IO_ERROR;
      else {ChakshuStorage::wavHeader(header,0);if(audio.write(header,44)!=44)error=IO_ERROR;}
    } else {
    video=ChakshuStorage::create(s.path,sizeof(s.path),"mjpeg");
    if(!video)error=IO_ERROR;
    else {
      snprintf(audioPath,sizeof(audioPath),"%.*s.wav",int(strlen(s.path)-6),s.path);
      snprintf(indexPath,sizeof(indexPath),"%.*s.json",int(strlen(s.path)-6),s.path);
      audio=SD.open(audioPath,FILE_WRITE);index=SD.open(indexPath,FILE_WRITE);
      if(!audio||!index)error=IO_ERROR;
      else {
        ChakshuStorage::wavHeader(header,0);
        if(audio.write(header,44)!=44)error=IO_ERROR;
        index.print("{\"schema\":1,\"frameTimesMs\":[");
      }
    }
  }
  }
  const uint32_t started=millis();uint32_t nextFrame=0,nextSpaceCheck=0;alignas(int16_t) uint8_t pcm[1600];
  // Finite 60-second takes remain bounded after BLE/phone disconnection.
  {
    MicrophoneGuard guard;
    while(!error&&!stopRequested.load()&&audioBytes<1920000u&&millis()-started<65000u) {
      const size_t count=microphoneI2S.readBytes(reinterpret_cast<char*>(pcm),sizeof(pcm));
      if(!count || (count&1)){error=CAPTURE_ERROR;break;}
      ChakshuVoice::feed(reinterpret_cast<const int16_t*>(pcm),count/2);
      if(audio.write(pcm,count)!=count){error=IO_ERROR;break;}
      audioBytes+=count;
      if(withVideo&&audioBytes/32>=nextFrame) {
        camera_fb_t* frame=esp_camera_fb_get();if(frame)esp_camera_fb_return(frame);
        frame=esp_camera_fb_get();
        if(!frame||frame->format!=PIXFORMAT_JPEG){if(frame)esp_camera_fb_return(frame);error=CAPTURE_ERROR;break;}
        if(video.write(frame->buf,frame->len)!=frame->len)error=IO_ERROR;
        else videoBytes+=frame->len;
        esp_camera_fb_return(frame);
        if(frames++)index.print(',');index.print(audioBytes/32);nextFrame=audioBytes/32+500;
      }
      if(photoRequested.exchange(false)) {
        Snapshot image;image.operation=2;
        image.error=ChakshuCamera::ready?captureCamera(image,false):NO_CAMERA;
        image.state=image.error?3:2;refresh(image);save(image);
      }
      if(audioBytes>=nextSpaceCheck){ChakshuStorage::refresh();if(ChakshuStorage::freeBytes<ChakshuStorage::RESERVE_BYTES)error=NO_SPACE;nextSpaceCheck=audioBytes+16000;}
      s.bytes=withVideo?videoBytes:audioBytes;s.progress=uint8_t(audioBytes/19200);saveOffline(s);
      vTaskDelay(1);
    }
  }
  if(audio){ChakshuStorage::wavHeader(header,audioBytes);if(!audio.seek(0)||audio.write(header,44)!=44)error=IO_ERROR;audio.flush();audio.close();}
  if(index){index.printf("],\"durationMs\":%lu,\"audio\":\"%s\"}",static_cast<unsigned long>(audioBytes/32),audioPath);index.flush();index.close();}
  if(video){video.flush();if(video.size()!=videoBytes)error=IO_ERROR;video.close();}
  if(!error && ((withVideo&&!frames)||!audioBytes))error=CAPTURE_ERROR;
  s.bytes=withVideo?videoBytes:audioBytes;s.error=error;s.state=error?3:2;s.progress=error?s.progress:100;
  refresh(s);saveOffline(s);photoRequested.store(false);offline.store(false);offlineMode.store(0);
}
void worker(void*) {
  Request request{};
  for(;;) {
    if(xQueueReceive(requests,&request,portMAX_DELAY)!=pdTRUE)continue;
    if(request.local&&request.localEpoch!=localEpoch.load())continue;
    if(!request.local&&(request.connection!=connectionGeneration.load()||!deviceConnected.load()))continue;
    ChakshuResources::Lease admission;
    if(!admission||otaBusySnapshot.load()) {replyFor(request,1);continue;}
    if(selectedConnection!=request.connection){clearSelection();selectedConnection=request.connection;}
    if(request.operation==5||request.operation==10) {
      if(streamingEnabled.load()||remoteStandby){replyFor(request,1);continue;}
      clearSelection();stopRequested.store(false);photoRequested.store(false);offlineMode.store(request.operation);offline.store(true);
      ChakshuMedia::Snapshot s;s.state=1;s.operation=request.operation==5?4:3;saveOffline(s);
      replyFor(request,0);recordOffline(request.operation==5);continue;
    }
    uint8_t error=0;uint32_t total=0;size_t size=0;uint8_t bytes[480];
    switch(request.operation) {
      case 1:error=captureFrame();total=bufferSize;break;
      case 11: {
        ChakshuMedia::Snapshot s;
        error=!ChakshuStorage::ready?ChakshuMedia::NO_SD:!ChakshuCamera::ready?ChakshuMedia::NO_CAMERA:ChakshuMedia::captureCamera(s,false);
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
      case 7:error=catalogue();total=bufferSize;break;
      case 8:total=bufferSize;break;
      default:error=2;
    }
    replyFor(request,error,total,request.offset,bytes,error?0:size);
  }
}
class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const String value=characteristic->getValue();
    if(value.length()<10||value.length()>73||uint8_t(value[0])!=0xCA)return;
    const uint8_t* p=reinterpret_cast<const uint8_t*>(value.c_str());Request request{};
    request.operation=p[1];memcpy(&request.id,p+2,4);memcpy(&request.offset,p+6,4);
    request.connection=connectionGeneration.load();memcpy(request.path,p+10,value.length()-10);
    if(request.operation==6){stopRequested.store(true);replyFor(request,0);return;}
    if(request.operation==9) {
      ChakshuMedia::Snapshot s;portENTER_CRITICAL(&mux);s=offlineStatus;portEXIT_CRITICAL(&mux);
      char json[180];const int size=snprintf(json,sizeof(json),"{\"active\":%s,\"state\":%u,\"error\":%u,\"progress\":%u,\"path\":\"%s\"}",offline.load()?"true":"false",s.state,s.error,s.progress,s.path);
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
}
} // namespace ChakshuTransfer
