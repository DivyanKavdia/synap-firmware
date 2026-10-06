// Odyssey C3 clean-room offline recorder.
// Deliberately excludes catalogue/read/delete/sync, preallocation, journals,
// segmentation and background recovery. One recording task owns the entire SD
// session: initialize -> mount -> create/write/finalize WAV -> unmount.
#if !SYNAP_CHAKSHU
static std::atomic<uint8_t> odysseySdBootState{0};
static std::atomic<uint8_t> odysseySdProbeStage{0};
uint8_t odysseySdDetectionState() { return odysseySdBootState.load(); }
uint8_t odysseySdProbeState() { return odysseySdProbeStage.load(); }

#if CONFIG_IDF_TARGET_ESP32C3
#include <SPI.h>
#include <SD.h>
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT
#error Odyssey C3 SD uses GPIO20/21: enable USB CDC On Boot to keep Serial off UART0 pins
#endif

constexpr int ODYSSEY_SD_CS=SYNAP_SD_CS_PIN, ODYSSEY_SD_SCK=SYNAP_SD_SCK_PIN;
constexpr int ODYSSEY_SD_MOSI=SYNAP_SD_MOSI_PIN, ODYSSEY_SD_MISO=SYNAP_SD_MISO_PIN;
static constexpr uint32_t ODYSSEY_SD_DATA_FREQ_HZ=400000u;
static SPIClass odysseySdSpi(FSPI);

static void odysseyLegacyRelease(bool clearProbe=true) {
  SD.end();
  odysseySdSpi.end();
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  if (clearProbe) {
    odysseySdBootState=0;
    odysseySdProbeStage=0;
  }
}

static bool odysseyLegacyMount(const char* why) {
  // Exact lifecycle restored from production build 1481 / the proven 1445 baseline:
  // tear down only when a mount is actually needed, then keep a successful
  // filesystem mounted across BLE idle and successive offline recordings.
  odysseySdBootState=0;
  odysseySdProbeStage=0;
  SD.end();
  odysseySdSpi.end();
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  odysseySdSpi.begin(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS);
  Serial.printf("[SD-1481] %s probe CS=%d SCK=%d MOSI=%d MISO=%d clock=%lu\n",
    why?why:"mount",ODYSSEY_SD_CS,ODYSSEY_SD_SCK,ODYSSEY_SD_MOSI,ODYSSEY_SD_MISO,
    static_cast<unsigned long>(ODYSSEY_SD_DATA_FREQ_HZ));
  const bool mounted=SD.begin(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_DATA_FREQ_HZ,
    "/odyssey-sd",1,false);
  if (mounted) {
    const uint8_t type=SD.cardType();
    if (type!=CARD_NONE) {
      odysseySdBootState=1;
      odysseySdProbeStage=6;
      markOdysseySdBatteryDividerPresent();
      const char* label=type==CARD_MMC?"MMC":type==CARD_SD?"SDSC":type==CARD_SDHC?"SDHC/SDXC":"unknown";
      Serial.printf("[SD-1481] mounted %s %llu MiB and retained\n",label,
        static_cast<unsigned long long>(SD.cardSize()/(1024ULL*1024ULL)));
      return true;
    }
    odysseySdBootState=3;
    odysseySdProbeStage=2;
    Serial.println("[SD-1481] mounted host but card type is NONE");
  } else {
    odysseySdBootState=2;
    odysseySdProbeStage=2;
    Serial.println("[SD-1481] SD.begin failed");
  }
  SD.end();
  odysseySdSpi.end();
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  return false;
}

static void odysseyLegacyWavHeader(uint8_t* h,uint32_t bytes) {
  memset(h,0,44);
  memcpy(h,"RIFF",4); put32le(h+4,bytes+36u);
  memcpy(h+8,"WAVEfmt ",8); put32le(h+16,16u);
  h[20]=1; h[22]=1; put32le(h+24,SAMPLE_RATE);
  put32le(h+28,SAMPLE_RATE*2u); h[32]=2; h[34]=16;
  memcpy(h+36,"data",4); put32le(h+40,bytes);
}

static void odysseyLegacyRecordTask(void*) {
  bool failed=false;
  uint8_t failureStage=0;
  File file;
  uint32_t bytes=0;
  char path[64]{};

  // Build 1481 admitted a take from an already-mounted filesystem.
  if (odysseySdBootState.load()!=1 || SD.cardType()==CARD_NONE) {
    failed=true; failureStage=2;
  }
  if (!failed && !SD.exists("/synap") && !SD.mkdir("/synap")) {
    failed=true; failureStage=3;
  }
  if (!failed) {
    for (uint8_t attempt=0;attempt<16;++attempt) {
      snprintf(path,sizeof(path),"/synap/odyssey_audio_%08lx_%08lx.wav",
        static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));
      if (!SD.exists(path)) { file=SD.open(path,FILE_WRITE); break; }
    }
    if (!file) { failed=true; failureStage=4; }
  }

  uint8_t header[44];
  odysseyLegacyWavHeader(header,0);
  if (!failed && file.write(header,sizeof(header))!=sizeof(header)) {
    failed=true; failureStage=4;
  }

#if USE_REAL_I2S_MIC
  if (!failed && !odysseyStopRequested.load()) {
    MicrophoneGuard guard;
    if (!startMicrophone()) {
      failed=true; failureStage=6;
    } else {
      int32_t raw[SAMPLES_PER_FRAME];
      int16_t pcm[SAMPLES_PER_FRAME];
      uint32_t checkpointAt=millis();
      bool captureConfirmed=false;
      while (!failed && !odysseyStopRequested.load()) {
        size_t received=0;
        uint8_t emptyReads=0;
        while (received<sizeof(raw) && !odysseyStopRequested.load()) {
          const size_t count=microphoneI2S.readBytes(
            reinterpret_cast<char*>(raw)+received,sizeof(raw)-received);
          if (!count) {
            if (++emptyReads>=3) { failed=true; failureStage=6; break; }
          } else {
            received+=count; emptyReads=0;
          }
        }
        if (failed || odysseyStopRequested.load()) break;
        for (uint16_t i=0;i<SAMPLES_PER_FRAME;++i)
          pcm[i]=static_cast<int16_t>(raw[i]>>16);
        if (bytes>0xffffff00u-sizeof(pcm)) break;

        const size_t written=file.write(reinterpret_cast<const uint8_t*>(pcm),sizeof(pcm));
        bytes+=written & ~size_t(1);
        if (written!=sizeof(pcm)) { failed=true; failureStage=4; break; }

        if (!captureConfirmed) {
          captureConfirmed=true;
          odysseyRecordingStartedAt=millis();
          odysseyCaptureActive=true;
          odysseySdRecoveryActive=false;
          updateStatusLed(true);
          Serial.printf("[SD-1481] PCM capture active path=%s\n",path);
        }

        // This is the exact recoverability cadence used by the known-good path:
        // rewrite the WAV header every 2 s, then File.flush().
        if (uint32_t(millis()-checkpointAt)>=2000u) {
          odysseyLegacyWavHeader(header,bytes);
          if (!file.seek(0) || file.write(header,44)!=44 || !file.seek(44+bytes)) {
            failed=true; failureStage=4; break;
          }
          file.flush();
          checkpointAt=millis();
        }
      }
      odysseyCaptureActive=false;
      updateStatusLed(true);
      stopMicrophone();
    }
  }
#else
  failed=true; failureStage=6;
#endif

  if (file) {
    odysseyLegacyWavHeader(header,bytes);
    if (!file.seek(0) || file.write(header,44)!=44) {
      failed=true;
      if (!failureStage) failureStage=4;
    }
    file.flush();
    file.close();
  }

  if (failed) {
    odysseySdBootState=2;
    odysseySdProbeStage=failureStage?failureStage:4;
    // Match the legacy failure behavior: release the SD filesystem; a later
    // touch may run the proven mount sequence again.
    SD.end();
  } else if (bytes) {
    odysseySdBootState=1;
    odysseySdProbeStage=6;
  }

  Serial.printf("[SD-1481] local audio %s: %s, %lu PCM bytes stage=%u\n",
    failed?"failed":"saved",path,static_cast<unsigned long>(bytes),
    unsigned(odysseySdProbeStage.load()));

  const uint32_t finishedAt=millis();
  odysseyCaptureActive=false;
  odysseySdRecoveryActive=false;
  odysseyStopRequested=false;
  odysseySdSleepGuardUntil=finishedAt+1500u;
  disconnectedAt=finishedAt;
  applyCpuPowerProfile(false);
  if (failed || !bytes) odysseyRecordFaultAt=finishedAt;
  odysseyRecording=false;
  updateStatusLed(true);
  vTaskDelete(nullptr);
}

void odysseyInitializeSdCardBeforeBle() {
  odysseyCaptureActive=false;
  odysseySdRecoveryActive=true;
  odysseyRecording=false;
  odysseyStopRequested=false;
  const bool mounted=odysseyLegacyMount("boot");
  odysseySdRecoveryActive=false;
  if (!mounted) odysseyRecordFaultAt=0;
  Serial.printf("[SD-1481] boot mount %s; retained before BLE\n",mounted?"ready":"unavailable");
}

void odysseyToggleRecording() {
  if (odysseyRecording.load()) {
    odysseyStopRequested=true;
    updateStatusLed(true);
    Serial.println("[TOUCH] double tap -> 1481 SD audio STOP");
    return;
  }
  if (deviceConnected.load() || streamingEnabled.load() || otaBusy() || sleepPending || batteryCritical()) return;

  odysseyRecordFaultAt=0;
  odysseyStopRequested=false;
  odysseyCaptureActive=false;
  odysseySdRecoveryActive=true;
  updateStatusLed(true);

  // The defining 1481 behavior: reuse a healthy retained mount. Do not
  // SD.end()/SD.begin() before every take.
  if (odysseySdBootState.load()!=1 || SD.cardType()==CARD_NONE) {
    if (!odysseyLegacyMount("touch")) {
      odysseySdRecoveryActive=false;
      odysseyRecordFaultAt=millis();
      updateStatusLed(true);
      return;
    }
  }

  odysseyRecording=true;
  applyCpuPowerProfile(true);
  if (xTaskCreate(odysseyLegacyRecordTask,"sd-audio",8192,nullptr,2,nullptr)!=pdPASS) {
    odysseyRecording=false;
    odysseySdRecoveryActive=false;
    odysseyRecordFaultAt=millis();
    applyCpuPowerProfile(false);
    updateStatusLed(true);
    Serial.println("[SD-1481] local audio task allocation failed");
    return;
  }
  Serial.println("[TOUCH] double tap -> retained-mount SD audio START");
}

bool odysseyPrepareForConnectedStreaming(uint32_t timeoutMs) {
  if (!odysseyRecording.load()) return true;
  odysseyStopRequested=true;
  const uint32_t started=millis();
  while (odysseyRecording.load() && uint32_t(millis()-started)<timeoutMs) delay(10);
  return !odysseyRecording.load();
}

bool odysseyPrepareSdForPowerTransition(uint32_t timeoutMs) {
  if (odysseyRecording.load()) {
    odysseyStopRequested=true;
    const uint32_t started=millis();
    while (odysseyRecording.load() && uint32_t(millis()-started)<timeoutMs) delay(10);
    if (odysseyRecording.load()) return false;
  }
  odysseyLegacyRelease(true);
  return true;
}

namespace OdysseyTransfer {
void initialize() {}
void ble(BLEService*) {}
bool available() { return false; }
}

#elif CONFIG_IDF_TARGET_ESP32S3
#include <SPI.h>
#include <SD.h>
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
