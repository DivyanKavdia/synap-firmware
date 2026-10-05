// Odyssey C3 clean-room offline recorder.
// Deliberately excludes catalogue/read/delete/sync, preallocation, journals,
// segmentation and background recovery. One recording task owns the entire SD
// session: resync -> mount -> create/write/finalize WAV -> unmount.
#if !SYNAP_CHAKSHU
#include <SPI.h>
#include <SD.h>

static std::atomic<uint8_t> odysseySdBootState{0};
static std::atomic<uint8_t> odysseySdProbeStage{0};
uint8_t odysseySdDetectionState() { return odysseySdBootState.load(); }
uint8_t odysseySdProbeState() { return odysseySdProbeStage.load(); }

#if CONFIG_IDF_TARGET_ESP32C3
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT
#error Odyssey C3 SD uses GPIO20/21: enable USB CDC On Boot to keep Serial off UART0 pins
#endif

constexpr int ODYSSEY_SD_CS=SYNAP_SD_CS_PIN, ODYSSEY_SD_SCK=SYNAP_SD_SCK_PIN;
constexpr int ODYSSEY_SD_MOSI=SYNAP_SD_MOSI_PIN, ODYSSEY_SD_MISO=SYNAP_SD_MISO_PIN;
static constexpr uint32_t ODYSSEY_SD_SPI_HZ=400000u;
static constexpr size_t ODYSSEY_SD_WRITE_BUFFER_BYTES=8000u;
static constexpr uint32_t ODYSSEY_SD_FLUSH_MS=5000u;
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

static uint8_t odysseyCleanRawByte(uint8_t out) {
  uint8_t in=0;
  for (uint8_t bit=0;bit<8;++bit) {
    digitalWrite(ODYSSEY_SD_MOSI,(out&0x80u)?HIGH:LOW);
    delayMicroseconds(2);
    digitalWrite(ODYSSEY_SD_SCK,HIGH);
    delayMicroseconds(2);
    in=uint8_t((in<<1)|(digitalRead(ODYSSEY_SD_MISO)==HIGH?1u:0u));
    digitalWrite(ODYSSEY_SD_SCK,LOW);
    delayMicroseconds(2);
    out<<=1;
  }
  return in;
}

static void odysseyCleanIdleClocks(uint8_t bytes=20) {
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  for (uint8_t i=0;i<bytes;++i) (void)odysseyCleanRawByte(0xFF);
}

static uint8_t odysseyCleanCmd0() {
  digitalWrite(ODYSSEY_SD_CS,LOW);
  const uint8_t frame[6]={0x40,0,0,0,0,0x95};
  for (uint8_t b:frame) (void)odysseyCleanRawByte(b);
  uint8_t r1=0xFF;
  for (uint8_t i=0;i<32 && (r1&0x80u);++i) r1=odysseyCleanRawByte(0xFF);
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  (void)odysseyCleanRawByte(0xFF);
  return r1;
}

// Minimal pre-mount resynchronization only. This does not own filesystem state
// and never runs while a WAV is open. 160+ clocks and two CMD0 opportunities
// mirror the stronger initialization behavior in newer Arduino-ESP32 SD code.
static void odysseyCleanResyncBeforeMount() {
  SD.end();
  odysseySdSpi.end();
  odysseyCleanSdMounted=false;
  pinMode(ODYSSEY_SD_CS,OUTPUT);digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_SCK,OUTPUT);digitalWrite(ODYSSEY_SD_SCK,LOW);
  pinMode(ODYSSEY_SD_MOSI,OUTPUT);digitalWrite(ODYSSEY_SD_MOSI,HIGH);
  pinMode(ODYSSEY_SD_MISO,INPUT_PULLUP);
  delay(2);
  odysseyCleanIdleClocks(20);
  uint8_t r1=odysseyCleanCmd0();
  if (r1!=0x01) {
    delay(20);
    odysseyCleanIdleClocks(20);
    r1=odysseyCleanCmd0();
  }
  Serial.printf("[SD] clean pre-mount resync CMD0=0x%02X\n",unsigned(r1));
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  digitalWrite(ODYSSEY_SD_SCK,LOW);
  digitalWrite(ODYSSEY_SD_MOSI,HIGH);
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
  odysseyCleanResyncBeforeMount();
  if (!odysseySdSpi.begin(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS)) {
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

static bool odysseyCleanDrain(File& file,size_t& buffered,uint32_t& pcmBytes) {
  if (!buffered) return true;
  const size_t written=file.write(odysseyCleanWriteBuffer,buffered);
  if (written!=buffered) {
    Serial.printf("[SD] clean write short requested=%u written=%u\n",unsigned(buffered),unsigned(written));
    return false;
  }
  pcmBytes+=uint32_t(written);
  buffered=0;
  return true;
}

static bool odysseyCleanRecordTake() {
  if (!odysseyCleanMountForTake()) return false;

  char path[64];
  snprintf(path,sizeof(path),"/synap/odyssey_audio_%08lx_%08lx.wav",
    static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));
  File file=SD.open(path,FILE_WRITE);
  if (!file) {
    Serial.println("[SD] clean record: file create failed");
    odysseyCleanUnmount();
    return false;
  }

  uint8_t header[44];
  odysseyCleanWavHeader(header,0);
  if (file.write(header,sizeof(header))!=sizeof(header)) {
    Serial.println("[SD] clean record: WAV header create failed");
    file.close();SD.remove(path);odysseyCleanUnmount();
    return false;
  }
  file.flush();

  bool ok=true;
  uint32_t pcmBytes=0;
  size_t buffered=0;
  uint32_t lastFlush=millis();

#if USE_REAL_I2S_MIC
  {
    MicrophoneGuard micGuard;
    if (!startMicrophone()) {
      ok=false;
      Serial.println("[SD] clean record: microphone start failed");
    } else {
      odysseyRecordingStartedAt=millis();
      odysseyCaptureActive=true;
      odysseySdRecoveryActive=false;
      updateStatusLed(true);
      Serial.printf("[SD] clean PCM capture active path=%s\n",path);

      int32_t raw[SAMPLES_PER_FRAME];
      int16_t pcm[SAMPLES_PER_FRAME];
      while (ok && !odysseyStopRequested.load()) {
        size_t received=0;
        uint8_t emptyReads=0;
        while (received<sizeof(raw) && !odysseyStopRequested.load()) {
          const size_t n=microphoneI2S.readBytes(reinterpret_cast<char*>(raw)+received,sizeof(raw)-received);
          if (!n) {
            if (++emptyReads>=3) { ok=false;break; }
          } else {
            received+=n;
            emptyReads=0;
          }
        }
        if (!ok || odysseyStopRequested.load()) break;
        for (uint16_t i=0;i<SAMPLES_PER_FRAME;++i) pcm[i]=static_cast<int16_t>(raw[i]>>16);

        if (buffered+sizeof(pcm)>sizeof(odysseyCleanWriteBuffer) &&
            !odysseyCleanDrain(file,buffered,pcmBytes)) {
          ok=false;break;
        }
        memcpy(odysseyCleanWriteBuffer+buffered,pcm,sizeof(pcm));
        buffered+=sizeof(pcm);

        if (uint32_t(millis()-lastFlush)>=ODYSSEY_SD_FLUSH_MS) {
          if (!odysseyCleanDrain(file,buffered,pcmBytes)) { ok=false;break; }
          file.flush();
          lastFlush=millis();
        }
      }
      odysseyCaptureActive=false;
      updateStatusLed(true);
      stopMicrophone();
    }
  }
#else
  ok=false;
#endif

  if (ok && !odysseyCleanDrain(file,buffered,pcmBytes)) ok=false;
  if (ok && pcmBytes) {
    odysseyCleanWavHeader(header,pcmBytes);
    if (!file.seek(0,SeekSet) || file.write(header,sizeof(header))!=sizeof(header)) ok=false;
    file.flush();
  }

  file.close();
  if (!ok || !pcmBytes) {
    (void)SD.remove(path);
    ok=false;
  }
  // A take never leaves the filesystem mounted. The next double tap starts
  // from the same deterministic bus/card initialization sequence.
  delay(20);
  odysseyCleanUnmount();

  if (ok) Serial.printf("[SD] clean WAV saved path=%s pcmBytes=%lu\n",path,static_cast<unsigned long>(pcmBytes));
  else Serial.printf("[SD] clean WAV failed path=%s pcmBytes=%lu\n",path,static_cast<unsigned long>(pcmBytes));
  return ok;
}

static void odysseyCleanRecordTask(void*) {
  const bool ok=odysseyCleanRecordTake();
  const uint32_t finishedAt=millis();
  odysseyCaptureActive=false;
  odysseySdRecoveryActive=false;
  odysseyStopRequested=false;
  odysseyRecording=false;
  odysseySdSleepGuardUntil=finishedAt+1500u;
  disconnectedAt=finishedAt;
  applyCpuPowerProfile(false);
  if (!ok) odysseyRecordFaultAt=finishedAt;
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
  if (deviceConnected.load() || streamingEnabled.load() || otaBusy() || sleepPending) return;
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
