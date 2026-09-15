// Audio acquisition never waits for the camera or filesystem. The writer owns
// every SD handle; camera congestion drops visual frames with real timestamps.
namespace ChakshuRecorder {
constexpr uint32_t AUDIO_LIMIT=1920000, VIDEO_LIMIT=32u*1024u*1024u, VIDEO_INTERVAL_MS=100, JPEG_LIMIT=96000;
struct Pcm { size_t size;uint8_t bytes[1600]; };
struct Jpeg { size_t size;uint32_t atMs;uint8_t bytes[JPEG_LIMIT]; };
struct Session {
  ChakshuBuffers::Queue<Pcm,40> audio;
  ChakshuBuffers::Queue<Jpeg,2> video;
  std::atomic<bool> running{true},audioDone{false},videoDone{true};
  std::atomic<uint8_t> error{0};
  std::atomic<uint32_t> capturedBytes{0},droppedFrames{0};
  uint32_t started=0;
};
void audioTask(void* argument) {
  auto& s=*static_cast<Session*>(argument);
  while(s.running.load() && s.capturedBytes.load()<AUDIO_LIMIT) {
    Pcm* slot=s.audio.reserve();
    if(!slot){s.error=9;break;}
    slot->size=0;
    while(s.running.load() && slot->size<sizeof(slot->bytes)) {
      MicrophoneGuard guard;
      const size_t n=microphoneI2S.readBytes(reinterpret_cast<char*>(slot->bytes)+slot->size,sizeof(slot->bytes)-slot->size);
      if(!n || (n&1)){s.error=ChakshuMedia::CAPTURE_ERROR;break;}
      slot->size+=n;
    }
    if(slot->size){s.capturedBytes.fetch_add(slot->size);s.audio.publish();}
    if(s.error.load())break;
  }
  s.audioDone=true;
  vTaskDelete(nullptr);
}
void cameraTask(void* argument) {
  auto& s=*static_cast<Session*>(argument);
  TickType_t wake=xTaskGetTickCount();
  while(s.running.load() && !s.audioDone.load()) {
    Jpeg* slot=s.video.reserve();
    if(!slot)++s.droppedFrames;
    else {
      camera_fb_t* frame=esp_camera_fb_get();
      if(!frame){s.error=ChakshuMedia::CAPTURE_ERROR;break;}
      const uint32_t captured=uint32_t(uint64_t(frame->timestamp.tv_sec)*1000u+frame->timestamp.tv_usec/1000u);
      if(frame->format!=PIXFORMAT_JPEG || frame->len<4 || frame->len>JPEG_LIMIT) {
        esp_camera_fb_return(frame);s.error=ChakshuMedia::CAPTURE_ERROR;break;
      }
      if(int32_t(captured-s.started)>=0) {
        slot->size=frame->len;
        slot->atMs=std::min(captured-s.started,s.capturedBytes.load()/32u);
        memcpy(slot->bytes,frame->buf,frame->len);s.video.publish();
      }
      esp_camera_fb_return(frame);
    }
    vTaskDelayUntil(&wake,pdMS_TO_TICKS(VIDEO_INTERVAL_MS));
  }
  s.videoDone=true;
  vTaskDelete(nullptr);
}
void record(ChakshuMedia::Snapshot& status,bool withVideo,std::atomic<bool>& stop,
            void (*progress)(const ChakshuMedia::Snapshot&)) {
  using namespace ChakshuMedia;
  Session s;
  File audio,video,index;
  uint8_t header[44],error=0;
  bool audioCreated=false;
  uint32_t audioBytes=0,videoBytes=0,frames=0,nextProgress=0;
  char audioPath[64]{},indexPath[64]{};
  if(!ChakshuStorage::ready)error=NO_SD;
  else if(withVideo && !ChakshuCamera::ready)error=NO_CAMERA;
  else if(withVideo && !ChakshuCamera::configure(true))error=CAPTURE_ERROR;
  if(!error) {
    s.audio.slots=static_cast<Pcm*>(ps_malloc(sizeof(Pcm)*40));
    if(withVideo)s.video.slots=static_cast<Jpeg*>(ps_malloc(sizeof(Jpeg)*2));
    if(!s.audio.slots || (withVideo&&!s.video.slots))error=CAPTURE_ERROR;
  }
  if(!error && !startMicrophone())error=NO_MIC;
  if(!error) {
    File primary=ChakshuStorage::create(status.path,sizeof(status.path),withVideo?"mjpeg":"wav");
    if(!primary)error=ChakshuStorage::freeBytes<ChakshuStorage::RESERVE_BYTES?NO_SPACE:IO_ERROR;
    else if(withVideo) {
      video=primary;
      snprintf(audioPath,sizeof(audioPath),"%.*s.wav",int(strlen(status.path)-6),status.path);
      snprintf(indexPath,sizeof(indexPath),"%.*s.json",int(strlen(status.path)-6),status.path);
      audio=SD.open(audioPath,FILE_WRITE);index=SD.open(indexPath,FILE_WRITE);
      if(!audio||!index)error=IO_ERROR;
      else {
        constexpr char prefix[]="{\"schema\":1,\"frameTimesMs\":[";
        if(index.print(prefix)!=sizeof(prefix)-1)error=IO_ERROR;
      }
    } else {audio=primary;snprintf(audioPath,sizeof(audioPath),"%s",status.path);}
    if(!error){ChakshuStorage::wavHeader(header,0);if(audio.write(header,44)!=44)error=IO_ERROR;}
  }
  s.started=millis();
  if(!error) {
    audioCreated=xTaskCreatePinnedToCore(audioTask,"sd-pcm",4096,&s,3,nullptr,0)==pdPASS;
    if(!audioCreated)error=CAPTURE_ERROR;
    if(!error && withVideo) {
      s.videoDone=false;
      if(xTaskCreatePinnedToCore(cameraTask,"sd-camera",4096,&s,1,nullptr,1)!=pdPASS){s.videoDone=true;error=CAPTURE_ERROR;}
    }
  }
  if(!audioCreated)s.audioDone=true;
  uint64_t remaining=ChakshuStorage::freeBytes;
  while(!error) {
    if(stop.load() || s.error.load() || s.audioDone.load() || millis()-s.started>=65000u)s.running=false;
    bool wrote=false;
    while(Pcm* slot=s.audio.peek()) {
      if(remaining<slot->size+ChakshuStorage::RESERVE_BYTES){error=NO_SPACE;break;}
      const size_t n=audio.write(slot->bytes,slot->size);
      audioBytes+=uint32_t(n&~size_t(1));remaining-=n;
      const bool complete=n==slot->size;s.audio.release();wrote=true;
      if(!complete){error=IO_ERROR;break;}
    }
    if(error)break;
    if(Jpeg* slot=s.video.peek()) {
      // Keep every SD clip within the companion's per-file import limit. Stop
      // producers and drain PCM while discarding any visual tail beyond it.
      if(videoBytes+slot->size>VIDEO_LIMIT) {
        s.running=false;++s.droppedFrames;s.video.release();continue;
      }
      if(remaining<slot->size+ChakshuStorage::RESERVE_BYTES){error=NO_SPACE;break;}
      const size_t n=video.write(slot->bytes,slot->size);
      if(n!=slot->size){error=IO_ERROR;break;}
      videoBytes+=n;remaining-=n;
      const uint32_t at=std::min(slot->atMs,audioBytes/32u);
      if((frames && index.print(',')!=1) || !index.print(at)){error=IO_ERROR;break;}
      ++frames;s.video.release();wrote=true;
    }
    if(millis()-nextProgress>=250u) {
      nextProgress=millis();status.bytes=withVideo?videoBytes:audioBytes;
      status.audioMs=audioBytes/32u;status.frames=frames;status.droppedFrames=s.droppedFrames;
      status.progress=uint8_t(std::min<uint32_t>(audioBytes/19200u,99));progress(status);
    }
    if(s.audioDone.load() && s.videoDone.load() && !s.audio.peek() && !s.video.peek())break;
    if(!wrote)vTaskDelay(1);
  }
  s.running=false;
  // Tasks own pointers into this stack/session. Never free their buffers before
  // their bounded microphone/camera calls have returned and published done.
  while(!s.audioDone.load() || !s.videoDone.load())vTaskDelay(1);
  if(!error)error=s.error.load();
  if(audio) {
    ChakshuStorage::wavHeader(header,audioBytes);
    if(!audio.seek(0)||audio.write(header,44)!=44)error=IO_ERROR;
    audio.flush();audio.close();
  }
  if(index) {
    if(!index.printf("],\"durationMs\":%lu,\"audio\":\"%s\",\"droppedFrames\":%lu}",
       (unsigned long)(audioBytes/32u),audioPath,(unsigned long)s.droppedFrames.load()))error=IO_ERROR;
    index.flush();index.close();
  }
  if(video){video.flush();video.close();}
  stopMicrophone();free(s.audio.slots);free(s.video.slots);
  if(!error && (!audioBytes || (withVideo&&!frames)))error=CAPTURE_ERROR;
  if(error==IO_ERROR || error==NO_SD)ChakshuStorage::ready=false;
  status.bytes=withVideo?videoBytes:audioBytes;status.audioMs=audioBytes/32u;
  status.frames=frames;status.droppedFrames=s.droppedFrames;
  status.error=error;status.state=error?3:2;status.progress=error?status.progress:100;
}
}
