// Bounded hardware checks. BLE callbacks only copy requests; work runs off the control task.
namespace ChakshuMedia {
enum Error : uint8_t { OK=0, BUSY=1, BAD_COMMAND=2, NO_SD=3, NO_CAMERA=4,
  NO_MIC=5, NO_SPACE=6, IO_ERROR=7, CAPTURE_ERROR=8 };
struct Request { uint32_t connection;uint8_t operation,id; };
struct Snapshot {
  uint8_t operation=0,id=0,state=0,error=0,ready=0,progress=0;
  uint32_t totalMiB=0,freeMiB=0,bytes=0,connection=0;
  uint16_t sensor=0;
  char path[64]{};
};
portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
Snapshot status;
std::atomic<bool>& busy=ChakshuResources::media;
QueueHandle_t requests=nullptr,jobs=nullptr;
void copy(Snapshot& out) {
  portENTER_CRITICAL(&mux);out=status;portEXIT_CRITICAL(&mux);
}
void save(const Snapshot& value) {
  portENTER_CRITICAL(&mux);status=value;portEXIT_CRITICAL(&mux);
}
void copyForConnection(Snapshot& out) {
  copy(out);
  if(out.connection && out.connection!=connectionGeneration.load()) {
    out.operation=out.id=out.state=out.error=out.progress=0;
    out.bytes=0;out.path[0]=0;
  }
}
void refresh(Snapshot& s) {
  ChakshuStorage::refresh();
  s.ready=(microphoneValidated.load()?1:0)|(ChakshuCamera::ready?2:0)|(ChakshuStorage::ready?4:0);
  s.sensor=ChakshuCamera::sensorPid;
  s.totalMiB=uint32_t(ChakshuStorage::capacity/(1024u*1024u));
  s.freeMiB=uint32_t(ChakshuStorage::freeBytes/(1024u*1024u));
}
void encode(uint8_t* p) {
  Snapshot s;copyForConnection(s);memset(p,0,20);
  p[0]=0xC9;p[1]=1;p[2]=s.operation;p[3]=s.id;p[4]=s.state;
  p[5]=s.error;p[6]=s.ready;p[7]=s.progress;
  put32le(p+8,s.totalMiB);put32le(p+12,s.freeMiB);put32le(p+16,s.bytes);
}
uint8_t recordWav(Snapshot& s) {
  if (!startMicrophone()) return NO_MIC;
  MicrophoneGuard guard;
  File file=ChakshuStorage::create(s.path,sizeof(s.path),"wav");
  if (!file) return ChakshuStorage::freeBytes<ChakshuStorage::RESERVE_BYTES?NO_SPACE:IO_ERROR;
  uint8_t header[44];ChakshuStorage::wavHeader(header,0);
  uint8_t error=file.write(header,44)==44?OK:IO_ERROR;
  int16_t samples[800];
  const uint32_t deadline=millis()+15000u;
  while (error==OK && s.bytes<320000u) {
    if (int32_t(millis()-deadline)>=0) { error=CAPTURE_ERROR;break; }
    size_t received=0;
    while (received<sizeof(samples)) {
      const size_t n=microphoneI2S.readBytes(reinterpret_cast<char*>(samples)+received,sizeof(samples)-received);
      if (!n || int32_t(millis()-deadline)>=0) { error=CAPTURE_ERROR;break; }
      received+=n;
    }
    if (error!=OK) break;
    const size_t written=file.write(reinterpret_cast<const uint8_t*>(samples),sizeof(samples));
    s.bytes+=written;
    if (written!=sizeof(samples)) error=IO_ERROR;
    s.progress=uint8_t(s.bytes*100u/320000u);save(s);
    taskYIELD();
  }
  // Even a failed take retains a correctly sized header for the samples actually written.
  ChakshuStorage::wavHeader(header,s.bytes&~1u);
  if (!file.seek(0) || file.write(header,44)!=44) error=IO_ERROR;
  file.flush();
  if (file.size()!=44u+s.bytes) error=IO_ERROR;
  file.close();
  return error;
}
uint8_t captureCamera(Snapshot& s,bool video) {
  if (!ChakshuCamera::configure(false)) return CAPTURE_ERROR;
  File file=ChakshuStorage::create(s.path,sizeof(s.path),video?"mjpeg":"jpg");
  if (!file) return ChakshuStorage::freeBytes<ChakshuStorage::RESERVE_BYTES?NO_SPACE:IO_ERROR;
  uint8_t error=OK;
  const uint8_t frames=video?20:1;
  const uint32_t deadline=millis()+15000u;
  TickType_t wake=xTaskGetTickCount();
  for (uint8_t i=0;i<frames;++i) {
    if (int32_t(millis()-deadline)>=0) { error=CAPTURE_ERROR;break; }
    // With one framebuffer, return a stale queued frame before taking the next exposure.
    camera_fb_t* stale=esp_camera_fb_get();
    if (stale) esp_camera_fb_return(stale);
    camera_fb_t* frame=esp_camera_fb_get();
    if (!frame || frame->format!=PIXFORMAT_JPEG || frame->len<4) {
      if (frame) esp_camera_fb_return(frame);
      error=CAPTURE_ERROR;break;
    }
    if (frame->len>ChakshuStorage::RESERVE_BYTES-s.bytes) {
      esp_camera_fb_return(frame);error=NO_SPACE;break;
    }
    const size_t written=file.write(frame->buf,frame->len);
    const bool complete=written==frame->len;
    s.bytes+=written;esp_camera_fb_return(frame);
    if (!complete) { error=IO_ERROR;break; }
    s.progress=uint8_t((i+1)*100u/frames);save(s);
    if (video) vTaskDelayUntil(&wake,pdMS_TO_TICKS(500));
  }
  file.flush();file.close();
  return error;
}
void worker(void*) {
  Request request;
  for (;;) {
    if (xQueueReceive(jobs,&request,portMAX_DELAY)!=pdTRUE) continue;
    Snapshot s;copy(s);
    if(!deviceConnected.load() || request.connection!=connectionGeneration.load()) {
      s.state=3;s.error=BAD_COMMAND;save(s);busy.store(false);continue;
    }
    uint8_t error=OK;
    if (request.operation==1) {
      microphoneValidated=startMicrophone();
      ChakshuCamera::begin();ChakshuStorage::begin(true);
    } else if (!ChakshuStorage::ready) error=NO_SD;
    else if (request.operation==2 || request.operation==4) {
      error=ChakshuCamera::ready?captureCamera(s,request.operation==4):NO_CAMERA;
    } else if (request.operation==3) error=recordWav(s);
    else error=BAD_COMMAND;
    if (error==IO_ERROR || error==NO_SD) ChakshuStorage::ready=false;
    if (request.operation==3 && error==CAPTURE_ERROR) { stopMicrophone();microphoneValidated=false; }
    // The module remains awake after checks.
    refresh(s);s.error=error;s.state=error?3:2;
    if (!error) s.progress=100;
    save(s);
    Serial.printf("[CHAKSHU] op=%u result=%u bytes=%lu file=%s\n",
      unsigned(s.operation),unsigned(error),(unsigned long)s.bytes,s.path);
    stopMicrophone();
    busy.store(false);
  }
}
class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const auto value=characteristic->getValue();
    if (value.length()!=4 || uint8_t(value[0])!=0xC8 || uint8_t(value[1])!=1 ||
        uint8_t(value[2])<1 || uint8_t(value[2])>4 || !uint8_t(value[3])) return;
    Request request{connectionGeneration.load(),uint8_t(value[2]),uint8_t(value[3])};
    if (requests) xQueueSend(requests,&request,0);
  }
};
class StatusCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* characteristic) override {
    uint8_t value[20];encode(value);characteristic->setValue(value,sizeof(value));
  }
};
class PathCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* characteristic) override {
    Snapshot s;copyForConnection(s);characteristic->setValue(s.path);
  }
};
void initialize() {
  ChakshuCamera::begin();ChakshuStorage::begin(false);
  Snapshot s;refresh(s);save(s);
  requests=xQueueCreate(4,sizeof(Request));jobs=xQueueCreate(1,sizeof(Request));
  if (!requests || !jobs ||
      xTaskCreatePinnedToCore(worker,"chakshu-media",8192,nullptr,1,nullptr,1)!=pdPASS)
    fatalSetup("[CHAKSHU] media worker allocation failed");
}
void ble(BLEService* service) {
  auto* command=service->createCharacteristic("4fa12351-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_WRITE);
  command->setCallbacks(new CommandCallbacks());
  auto* result=service->createCharacteristic("4fa12352-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_READ);
  result->setCallbacks(new StatusCallbacks());
  auto* path=service->createCharacteristic("4fa12353-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_READ);
  path->setCallbacks(new PathCallbacks());
}
void tick() {
  Request request;
  if (!requests || xQueueReceive(requests,&request,0)!=pdTRUE ||
      !deviceConnected.load() || request.connection!=connectionGeneration.load()) return;
  bool expected=false;
  if (!busy.compare_exchange_strong(expected,true)) return; // Claim the camera/SD worker atomically.
  Snapshot s;copy(s);
  // A retried command has the same ID; never capture twice after an ACK loss.
  if (s.connection==request.connection && s.id==request.id && s.operation==request.operation && s.error!=BUSY) {busy.store(false);return;}
  s.connection=request.connection;s.id=request.id;s.operation=request.operation;s.progress=0;s.bytes=0;s.path[0]=0;
  if (otaBusy() || streamingEnabled.load() || remoteStandby) {
    s.state=3;s.error=BUSY;save(s);busy.store(false);return;
  }
  s.state=1;s.error=OK;save(s);
  if (xQueueSend(jobs,&request,0)!=pdTRUE) {
    s.state=3;s.error=BUSY;save(s);busy.store(false);
  }
}
}
bool mediaBusy() { return ChakshuMedia::busy.load(); }
