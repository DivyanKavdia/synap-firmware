// Odyssey C3 clean-room offline recorder.
// Deliberately excludes catalogue/read/delete/sync, preallocation, journals,
// segmentation and background recovery. One recording task owns the entire SD
// session: initialize -> mount -> create/write/finalize WAV -> unmount.
#if !SYNAP_CHAKSHU
#include <SPI.h>
#include <SD.h>

static std::atomic<uint8_t> odysseySdBootState{0};
static std::atomic<uint8_t> odysseySdProbeStage{0};
uint8_t odysseySdDetectionState() { return odysseySdBootState.load(); }
uint8_t odysseySdProbeState() { return odysseySdProbeStage.load(); }

#if CONFIG_IDF_TARGET_ESP32C3
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT
#error Odyssey C3 SD uses GPIO20/21: enable USB CDC On Boot to keep Serial off UART0 pins
#endif

constexpr int ODYSSEY_SD_CS=SYNAP_SD_CS_PIN, ODYSSEY_SD_SCK=SYNAP_SD_SCK_PIN;
constexpr int ODYSSEY_SD_MOSI=SYNAP_SD_MOSI_PIN, ODYSSEY_SD_MISO=SYNAP_SD_MISO_PIN;
static constexpr uint32_t ODYSSEY_SD_SPI_HZ=1000000u;
static constexpr size_t ODYSSEY_SD_WRITE_BUFFER_BYTES=4096u;
static constexpr size_t ODYSSEY_SD_WRITE_CHUNK_BYTES=512u;
static constexpr uint32_t ODYSSEY_SD_FLUSH_MS=5000u;
static_assert((ODYSSEY_SD_WRITE_BUFFER_BYTES%ODYSSEY_SD_WRITE_CHUNK_BYTES)==0,
  "clean C3 write buffer must contain complete SD sectors");
static SPIClass odysseySdSpi(FSPI);
static std::atomic<bool> odysseyCleanSdMounted{false};
static uint8_t odysseyCleanWriteBuffer[ODYSSEY_SD_WRITE_BUFFER_BYTES];

static void odysseyCleanWavHeader(uint8_t* h,uint32_t pcmBytes) {
  memset(h,0,44);
  memcpy(h,"RIFF",4);
  put32le(h+4,pcmBytes+36u);
  memcpy(h+8,"WAVEfmt ",8);
  put32le(h+16,16u);
  h[20]=1;h[22]=1;
  put32le(h+24,SAMPLE_RATE);
  put32le(h+28,SAMPLE_RATE*2u);
  h[32]=2;h[34]=16;
  memcpy(h+36,"data",4);
  put32le(h+40,pcmBytes);
}

static void odysseyCleanPrepareHost() {
  // The pinned Arduino core receives Espressif's newer SD-SPI initializer in
  // CI for the C3 compile. Keep the application layer out of card commands.
  SD.end();
  odysseySdSpi.end();
  odysseyCleanSdMounted=false;
  pinMode(ODYSSEY_SD_CS,OUTPUT);digitalWrite(ODYSSEY_SD_CS,HIGH);
  delay(2);
}

static void odysseyCleanUnmount() {
  if (odysseyCleanSdMounted.load()) SD.end();
  odysseySdSpi.end();
  odysseyCleanSdMounted=false;
  pinMode(ODYSSEY_SD_CS,OUTPUT);digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_SCK,OUTPUT);digitalWrite(ODYSSEY_SD_SCK,LOW);
  pinMode(ODYSSEY_SD_MOSI,OUTPUT);digitalWrite(ODYSSEY_SD_MOSI,HIGH);
  odysseySdBootState=0;
  odysseySdProbeStage=0;
}

static bool odysseyCleanMountForTake() {
  odysseySdBootState=0;
  odysseySdProbeStage=0;
  odysseyCleanPrepareHost();
  if (odysseyStopRequested.load()) { odysseyCleanUnmount();return false; }
  if (!odysseySdSpi.begin(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS)) {
    odysseyCleanUnmount();
    odysseySdBootState=2;odysseySdProbeStage=1;
    Serial.println("[SD] clean mount: SPI.begin failed");
    return false;
  }
  if (!SD.begin(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_SPI_HZ,"/odyssey-sd",2,false)) {
    odysseySdBootState=2;odysseySdProbeStage=2;
    Serial.println("[SD] clean mount: SD.begin failed");
    odysseyCleanUnmount();
    odysseySdBootState=2;odysseySdProbeStage=2;
    return false;
  }
  odysseyCleanSdMounted=true;
  if (SD.cardType()==CARD_NONE) {
    odysseySdBootState=3;odysseySdProbeStage=2;
    Serial.println("[SD] clean mount: no card");
    odysseyCleanUnmount();
    odysseySdBootState=3;odysseySdProbeStage=2;
    return false;
  }
  if (!SD.exists("/synap") && !SD.mkdir("/synap")) {
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.println("[SD] clean mount: /synap create failed");
    odysseyCleanUnmount();
    odysseySdBootState=2;odysseySdProbeStage=4;
    return false;
  }
  odysseySdBootState=1;
  odysseySdProbeStage=6;
  markOdysseySdBatteryDividerPresent();
  Serial.printf("[SD] clean mount ready type=%u size=%lluMB clock=%luHz\n",
    unsigned(SD.cardType()),static_cast<unsigned long long>(SD.cardSize()/(1024ull*1024ull)),
    static_cast<unsigned long>(ODYSSEY_SD_SPI_HZ));
  return true;
}

static bool odysseyCleanWriteAll(int fd,const uint8_t* bytes,size_t size) {
  while (size) {
    const ssize_t n=write(fd,bytes,size);
    if (n<0 && errno==EINTR) continue;
    if (n<=0) { if (!n) errno=EIO;return false; }
    bytes+=n;size-=size_t(n);
  }
  return true;
}

static bool odysseyCleanRecordTake() {
  uint8_t failureStage=0;
  if (odysseyStopRequested.load()) return true;
  if (!odysseyCleanMountForTake()) return odysseyStopRequested.load();
  if (odysseyStopRequested.load()) { odysseyCleanUnmount();return true; }

  // Exclusive creation preserves every previous take, including improbable
  // random-name collisions. .part is never advertised as a completed WAV.
  char path[112]{},finished[112]{};
  int file=-1;
  for (uint8_t attempt=0;attempt<8;++attempt) {
    snprintf(finished,sizeof(finished),"/odyssey-sd/synap/odyssey_audio_%08lx_%08lx.wav",
      static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));
    struct stat existing{};
    errno=0;
    if (stat(finished,&existing)==0) continue;
    if (errno!=ENOENT) break;
    snprintf(path,sizeof(path),"%s.part",finished);
    file=open(path,O_CREAT|O_EXCL|O_WRONLY,0644);
    if (file>=0 || errno!=EEXIST) break;
  }
  if (file<0) {
    failureStage=3;
    Serial.printf("[SD] clean record: exclusive create failed errno=%d\n",errno);
    odysseyCleanUnmount();odysseySdBootState=2;odysseySdProbeStage=failureStage;return false;
  }

  uint8_t header[44];
  odysseyCleanWavHeader(header,0);
  bool ok=odysseyCleanWriteAll(file,header,sizeof(header));
  if (!ok) failureStage=3;
  if (ok && fsync(file)!=0) { ok=false;failureStage=5; }
  int failureErrno=ok?0:(errno?errno:EIO);
  uint32_t pcmBytes=0;
  size_t buffered=0;
  uint32_t lastFlush=millis();
  auto drain=[&](bool finalDrain) -> bool {
    while (buffered) {
      const size_t fileOffset=44u+size_t(pcmBytes);
      const size_t sectorOffset=fileOffset&(ODYSSEY_SD_WRITE_CHUNK_BYTES-1u);
      size_t chunk=0;
      if (sectorOffset) {
        const size_t toBoundary=ODYSSEY_SD_WRITE_CHUNK_BYTES-sectorOffset;
        if (!finalDrain && buffered<toBoundary) return true;
        chunk=std::min(buffered,toBoundary);
      } else if (buffered>=ODYSSEY_SD_WRITE_CHUNK_BYTES) {
        // One sector per VFS write keeps normal audio off CMD25 multi-block writes.
        chunk=ODYSSEY_SD_WRITE_CHUNK_BYTES;
      } else if (finalDrain) chunk=buffered;
      else return true;
      if (!odysseyCleanWriteAll(file,odysseyCleanWriteBuffer,chunk)) { failureStage=4;return false; }
      pcmBytes+=uint32_t(chunk);buffered-=chunk;
      if (buffered) memmove(odysseyCleanWriteBuffer,odysseyCleanWriteBuffer+chunk,buffered);
    }
    return true;
  };

#if USE_REAL_I2S_MIC
  if (ok && !odysseyStopRequested.load()) {
    MicrophoneGuard micGuard;
    if (!startMicrophone()) { ok=false;failureErrno=EIO;failureStage=6; }
    else {
      int32_t raw[SAMPLES_PER_FRAME];
      int16_t pcm[SAMPLES_PER_FRAME];
      while (ok && !odysseyStopRequested.load()) {
        size_t received=0;
        uint8_t emptyReads=0;
        while (received<sizeof(raw) && !odysseyStopRequested.load()) {
          const size_t n=microphoneI2S.readBytes(reinterpret_cast<char*>(raw)+received,sizeof(raw)-received);
          if (!n) {
            if (++emptyReads>=3) { ok=false;failureErrno=EIO;failureStage=6;break; }
          } else { received+=n;emptyReads=0; }
        }
        if (!ok || odysseyStopRequested.load()) break;
        for (uint16_t i=0;i<SAMPLES_PER_FRAME;++i) pcm[i]=static_cast<int16_t>(raw[i]>>16);
        // WAV RIFF length and FAT32 file size must never wrap on a long take.
        if (uint64_t(pcmBytes)+buffered+sizeof(pcm)>0xffff0000ull) break;
        if (buffered+sizeof(pcm)>sizeof(odysseyCleanWriteBuffer) && !drain(false)) {
          ok=false;failureErrno=errno?errno:EIO;break;
        }
        memcpy(odysseyCleanWriteBuffer+buffered,pcm,sizeof(pcm));
        buffered+=sizeof(pcm);
        if (!drain(false)) { ok=false;failureErrno=errno?errno:EIO;break; }
        if (!odysseyCaptureActive.load()) {
          // First real frame is written and synced before purple is asserted.
          if (fsync(file)!=0) { ok=false;failureErrno=errno?errno:EIO;failureStage=5;break; }
          odysseyRecordingStartedAt=millis();
          odysseyCaptureActive=true;odysseySdRecoveryActive=false;
          updateStatusLed(true);
          Serial.printf("[SD] clean PCM capture active path=%s\n",path);
        }
        if (uint32_t(millis()-lastFlush)>=ODYSSEY_SD_FLUSH_MS) {
          if (!drain(false)) { ok=false;failureErrno=errno?errno:EIO;if(!failureStage)failureStage=4;break; }
          if (fsync(file)!=0) { ok=false;failureErrno=errno?errno:EIO;failureStage=5;break; }
          lastFlush=millis();
        }
      }
      odysseyCaptureActive=false;updateStatusLed(true);stopMicrophone();
    }
  }
#else
  ok=false;failureErrno=ENOSYS;
#endif

  if (ok && !drain(true)) { ok=false;failureErrno=errno?errno:EIO;if(!failureStage)failureStage=4; }
  if (ok && pcmBytes) {
    odysseyCleanWavHeader(header,pcmBytes);
    if (fsync(file)!=0) {
      ok=false;failureErrno=errno?errno:EIO;failureStage=5;
    } else if (lseek(file,0,SEEK_SET)!=0 ||
               !odysseyCleanWriteAll(file,header,sizeof(header))) {
      ok=false;failureErrno=errno?errno:EIO;failureStage=3;
    } else if (fsync(file)!=0) {
      ok=false;failureErrno=errno?errno:EIO;failureStage=5;
    }
  }
  if (close(file)!=0) { ok=false;if (!failureErrno) failureErrno=errno?errno:EIO;if(!failureStage)failureStage=5; }
  if (ok && pcmBytes && rename(path,finished)!=0) { ok=false;failureErrno=errno?errno:EIO;failureStage=5; }
  // Preserve any failed take for later recovery. Only an empty, cleanly
  // cancelled take is removed; no writes are retried after a storage error.
  if (ok && !pcmBytes && unlink(path)!=0) { ok=false;failureErrno=errno?errno:EIO; }
  delay(20);
  odysseyCleanUnmount();
  if (ok && pcmBytes) {
    odysseySdBootState=1;odysseySdProbeStage=6;
    Serial.printf("[SD] clean WAV saved path=%s pcmBytes=%lu\n",finished,static_cast<unsigned long>(pcmBytes));
  } else if (!ok) {
    odysseySdBootState=2;odysseySdProbeStage=failureStage?failureStage:3;
    Serial.printf("[SD] clean WAV failed; partial retained path=%s pcmBytes=%lu errno=%d stage=%u\n",
      path,static_cast<unsigned long>(pcmBytes),failureErrno,unsigned(odysseySdProbeStage.load()));
  }
  return ok;
}

static void odysseyCleanRecordTask(void*) {
  const bool ok=odysseyCleanRecordTake();
  const uint32_t finishedAt=millis();
  odysseyCaptureActive=false;
  odysseySdRecoveryActive=false;
  odysseyStopRequested=false;
  odysseySdSleepGuardUntil=finishedAt+1500u;
  disconnectedAt=finishedAt;
  applyCpuPowerProfile(false);
  if (!ok) odysseyRecordFaultAt=finishedAt;
  odysseyRecording=false;
  updateStatusLed(true);
  vTaskDelete(nullptr);
}

void odysseyInitializeSdCardBeforeBle() {
  // Intentionally do not mount at boot. A recording gesture owns the complete
  // SD lifetime so BLE, idle and deep sleep cannot inherit an open card state.
  odysseyCleanUnmount();
  odysseySdRecoveryActive=false;
  odysseyCaptureActive=false;
  odysseyRecording=false;
  odysseyStopRequested=false;
  Serial.println("[SD] clean-room C3 recorder ready; mount deferred to offline double tap");
}

void odysseyToggleRecording() {
  if (odysseyRecording.load()) {
    odysseyStopRequested=true;
    updateStatusLed(true);
    Serial.println("[TOUCH] double tap -> clean SD audio STOP");
    return;
  }
  if (deviceConnected.load() || streamingEnabled.load() || otaBusy() || sleepPending || batteryCritical()) return;
  odysseyRecordFaultAt=0;
  odysseyStopRequested=false;
  odysseyCaptureActive=false;
  odysseySdRecoveryActive=true;
  odysseyRecording=true;
  applyCpuPowerProfile(true);
  updateStatusLed(true);
  if (xTaskCreate(odysseyCleanRecordTask,"sd-clean-audio",8192,nullptr,2,nullptr)!=pdPASS) {
    odysseyRecording=false;
    odysseySdRecoveryActive=false;
    odysseyRecordFaultAt=millis();
    applyCpuPowerProfile(false);
    updateStatusLed(true);
    Serial.println("[SD] clean record task create failed");
    return;
  }
  Serial.println("[TOUCH] double tap -> clean SD audio START");
}

bool odysseyPrepareForConnectedStreaming(uint32_t timeoutMs) {
  if (!odysseyRecording.load()) return true;
  odysseyStopRequested=true;
  const uint32_t started=millis();
  while (odysseyRecording.load() && uint32_t(millis()-started)<timeoutMs) delay(10);
  if (odysseyRecording.load()) {
    Serial.println("[SD] clean recorder did not finalize before BLE handoff");
    return false;
  }
  return true;
}

bool odysseyPrepareSdForPowerTransition(uint32_t timeoutMs) {
  if (odysseyRecording.load()) {
    odysseyStopRequested=true;
    const uint32_t started=millis();
    while (odysseyRecording.load() && uint32_t(millis()-started)<timeoutMs) delay(10);
    if (odysseyRecording.load()) return false;
  }
  odysseyCleanUnmount();
  return true;
}

namespace OdysseyTransfer {
void initialize() {}
void ble(BLEService*) {}
bool available() { return false; }
}

#elif CONFIG_IDF_TARGET_ESP32S3
constexpr int ODYSSEY_SD_CS=SYNAP_SD_CS_PIN, ODYSSEY_SD_SCK=SYNAP_SD_SCK_PIN;
constexpr int ODYSSEY_SD_MOSI=SYNAP_SD_MOSI_PIN, ODYSSEY_SD_MISO=SYNAP_SD_MISO_PIN;
static SPIClass odysseySdSpi(FSPI);

void odysseyDetectSdCard() {
  odysseySdBootState=0;
  odysseySdProbeStage=0;
  SD.end();odysseySdSpi.end();
  digitalWrite(ODYSSEY_SD_CS,HIGH);pinMode(ODYSSEY_SD_CS,OUTPUT);
  odysseySdSpi.begin(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS);
  const bool mounted=SD.begin(ODYSSEY_SD_CS,odysseySdSpi,400000,"/odyssey-sd",1,false);
  if (mounted && SD.cardType()!=CARD_NONE) {
    odysseySdBootState=1;odysseySdProbeStage=6;
    Serial.println("[SD] Odyssey S3 detection succeeded");
  } else if (mounted) {
    odysseySdBootState=3;
    Serial.println("[SD] Odyssey S3 no card reported");
  } else {
    odysseySdBootState=2;
    Serial.println("[SD] Odyssey S3 detection/mount failed");
  }
  SD.end();odysseySdSpi.end();digitalWrite(ODYSSEY_SD_CS,HIGH);
}
#else
#error Unsupported Odyssey SD target
#endif
#endif
