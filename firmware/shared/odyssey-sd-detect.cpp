// Odyssey SD storage.
// C3 owns SD through ESP-IDF SDSPI + FAT/VFS; S3 keeps its legacy one-shot detection.
// Hardware pins remain device-profile controlled and are never remapped here.
#if !SYNAP_CHAKSHU
#include <SPI.h>
#include <SD.h>
#if CONFIG_IDF_TARGET_ESP32C3
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_err.h"
#endif

#if CONFIG_IDF_TARGET_ESP32C3
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT
#error Odyssey C3 SD uses GPIO20/21: enable USB CDC On Boot to keep Serial off UART0 pins
#endif
#elif !CONFIG_IDF_TARGET_ESP32S3
#error Unsupported Odyssey SD target
#endif

constexpr int ODYSSEY_SD_CS=SYNAP_SD_CS_PIN, ODYSSEY_SD_SCK=SYNAP_SD_SCK_PIN;
constexpr int ODYSSEY_SD_MOSI=SYNAP_SD_MOSI_PIN, ODYSSEY_SD_MISO=SYNAP_SD_MISO_PIN;
constexpr bool odysseySdPinAvailable(int pin) {
  return pin != I2S_BCLK_PIN && pin != I2S_WS_PIN && pin != I2S_DATA_IN_PIN &&
    pin != TOUCH_INPUT_PIN && pin != BATTERY_ADC_PIN && pin != RGB_LED_PIN;
}
static_assert(odysseySdPinAvailable(ODYSSEY_SD_CS) &&
  odysseySdPinAvailable(ODYSSEY_SD_SCK) &&
  odysseySdPinAvailable(ODYSSEY_SD_MOSI) &&
  odysseySdPinAvailable(ODYSSEY_SD_MISO), "SD pin overlaps an existing peripheral");
static_assert(ODYSSEY_SD_CS != ODYSSEY_SD_SCK && ODYSSEY_SD_CS != ODYSSEY_SD_MOSI &&
  ODYSSEY_SD_CS != ODYSSEY_SD_MISO && ODYSSEY_SD_SCK != ODYSSEY_SD_MOSI &&
  ODYSSEY_SD_SCK != ODYSSEY_SD_MISO && ODYSSEY_SD_MOSI != ODYSSEY_SD_MISO,
  "SD pins must be distinct");

// Capability-compatible storage state:
// detection 0=not checked, 1=mounted/ready, 2=initialization or mount failed, 3=no card.
// probe 0=not checked, 1=SPI bus setup failed, 2=card protocol init failed,
//       3=FAT mount failed, 4=VFS validation failed, 6=ready.
static std::atomic<uint8_t> odysseySdBootState{0};
static std::atomic<uint8_t> odysseySdProbeStage{0};
#if CONFIG_IDF_TARGET_ESP32C3
// Touch requests recovery without blocking the control/gesture loop.
static std::atomic<bool> odysseySdRecoveryRequested{false};
void odysseySdRequestRecovery() { odysseySdRecoveryRequested=true; }
bool odysseySdConsumeRecoveryRequest() { return odysseySdRecoveryRequested.exchange(false); }
#endif
uint8_t odysseySdDetectionState() { return odysseySdBootState.load(); }
uint8_t odysseySdProbeState() { return odysseySdProbeStage.load(); }

#if CONFIG_IDF_TARGET_ESP32C3
static constexpr const char* ODYSSEY_SD_MOUNT_POINT="/odyssey-sd";
static constexpr const char* ODYSSEY_SD_RECORDING_DIR="/odyssey-sd/synap";
static constexpr uint32_t ODYSSEY_SD_INIT_FREQ_HZ=400000u;
static constexpr uint32_t ODYSSEY_SD_RESCUE_FREQ_HZ=100000u;
static constexpr uint32_t ODYSSEY_SD_RESCUE_BUSY_MS=3000u;
static constexpr uint32_t ODYSSEY_SD_STARTUP_SETTLE_MS=3000u;
static constexpr uint8_t ODYSSEY_SD_BOOT_ATTEMPTS=1;
static constexpr uint8_t ODYSSEY_SD_RECOVERY_ATTEMPTS=1;
static constexpr size_t ODYSSEY_SD_MAX_OPEN_FILES=8;

// Restore the exact Arduino-ESP32 3.3.5 stock SD initialization used by the
// known-good Odyssey C3 build 1445. Runtime recording/sync continues to use
// the current guarded POSIX/VFS implementation after the mount succeeds.
static SPIClass odysseySdSpi(FSPI);
static std::atomic<int32_t> odysseySdLastMountError{ESP_OK};
static std::atomic<uint32_t> odysseySdMountAttempts{0};
static std::atomic<uint32_t> odysseySdBeginAttempts{0};
static std::atomic<int16_t> odysseySdLastCmd0{-2};
static std::atomic<int16_t> odysseySdLastCmd12{-2};
static std::atomic<uint8_t> odysseySdLastCmd12Ready{0};
static std::atomic<uint8_t> odysseySdLastCmdReady{0};
static std::atomic<uint8_t> odysseySdLastRescueReady{0};
static std::atomic<int16_t> odysseySdLastCsHighByte{-2};
static StaticSemaphore_t odysseySdMutexStorage;
static SemaphoreHandle_t odysseySdMutex=nullptr;

int32_t odysseySdLastError() { return odysseySdLastMountError.load(); }
uint32_t odysseySdAttemptCount() { return odysseySdMountAttempts.load(); }
uint32_t odysseySdBeginAttemptCount() { return odysseySdBeginAttempts.load(); }
int16_t odysseySdLastCmd0Response() { return odysseySdLastCmd0.load(); }
int16_t odysseySdLastCmd12Response() { return odysseySdLastCmd12.load(); }
uint8_t odysseySdLastCmd12ReadyState() { return odysseySdLastCmd12Ready.load(); }
uint8_t odysseySdLastCmdReadyState() { return odysseySdLastCmdReady.load(); }
uint8_t odysseySdLastRescueReadyState() { return odysseySdLastRescueReady.load(); }
int16_t odysseySdLastCsHighResponse() { return odysseySdLastCsHighByte.load(); }
void odysseySdUseProbingClock() {}
void odysseySdMarkVfsFailure() {
  odysseySdBootState=2;odysseySdProbeStage=4;
  odysseySdLastMountError=ESP_FAIL;
  odysseySdRequestRecovery();
}

static void odysseySdEnsureMutex() {
  if (!odysseySdMutex) odysseySdMutex=xSemaphoreCreateMutexStatic(&odysseySdMutexStorage);
}
bool odysseySdTake(TickType_t timeout=portMAX_DELAY) {
  odysseySdEnsureMutex();
  return odysseySdMutex && xSemaphoreTake(odysseySdMutex,timeout)==pdTRUE;
}
void odysseySdGive() {
  if (odysseySdMutex) xSemaphoreGive(odysseySdMutex);
}
class OdysseySdGuard {
  bool held_;
public:
  explicit OdysseySdGuard(TickType_t timeout=portMAX_DELAY):held_(odysseySdTake(timeout)){}
  ~OdysseySdGuard(){ if(held_) odysseySdGive(); }
  explicit operator bool() const { return held_; }
};
bool odysseySdReady() {
  return odysseySdBootState.load()==1 && odysseySdProbeStage.load()==6;
}
const char* odysseySdMountPoint() { return ODYSSEY_SD_MOUNT_POINT; }
bool odysseySdPath(const char* logical,char* full,size_t capacity) {
  if (!logical || logical[0]!='/' || !full || capacity<2) return false;
  if (strstr(logical,"..")) return false;
  const int n=snprintf(full,capacity,"%s%s",ODYSSEY_SD_MOUNT_POINT,logical);
  return n>0 && size_t(n)<capacity;
}

void odysseySdHoldBusIdleEarly() {
  // The SD adapter remains powered across ESP resets. Establish a defined bus
  // state immediately on boot instead of leaving a continuously powered card
  // exposed to floating CS/clock/data during the startup settle interval.
  pinMode(ODYSSEY_SD_CS,OUTPUT);digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_SCK,OUTPUT);digitalWrite(ODYSSEY_SD_SCK,LOW);
  pinMode(ODYSSEY_SD_MOSI,OUTPUT);digitalWrite(ODYSSEY_SD_MOSI,HIGH);
  pinMode(ODYSSEY_SD_MISO,INPUT);
}

static void odysseySdReleaseLocked() {
  SD.end();
  odysseySdSpi.end();
  odysseySdHoldBusIdleEarly();
}

static void odysseyWaitForSdStartupSettle() {
  const uint32_t now=millis();
  if (now<ODYSSEY_SD_STARTUP_SETTLE_MS) {
    const uint32_t waitMs=ODYSSEY_SD_STARTUP_SETTLE_MS-now;
    Serial.printf("[SD] startup settle wait %lu ms before first transaction\n",
      static_cast<unsigned long>(waitMs));
    delay(waitMs);
  }
}

static bool odysseySdWaitReadyLocked(uint32_t timeoutMs,uint8_t& lastByte) {
  const uint32_t started=millis();
  do {
    lastByte=odysseySdSpi.transfer(0xFF);
    if (lastByte==0xFF) return true;
  } while (uint32_t(millis()-started)<timeoutMs);
  return false;
}

static bool odysseySdStopWriteLocked(uint32_t timeoutMs,uint8_t& lastByte) {
  // A multi-block write accepts 0xFD only after the previous block's busy
  // period ends. Repeat the token at a low rate so a continuously powered card
  // can be caught as soon as it becomes receptive after a host reset.
  const uint32_t started=millis();
  do {
    lastByte=odysseySdSpi.transfer(0xFD);
    for (uint8_t i=0;i<4;++i) {
      lastByte=odysseySdSpi.transfer(0xFF);
      if (lastByte==0xFF) return true;
    }
    delay(5);
  } while (uint32_t(millis()-started)<timeoutMs);
  return false;
}

static uint8_t odysseySdCommandLocked(uint8_t command,uint32_t argument,uint8_t crc,bool skipStuffByte=false) {
  odysseySdSpi.transfer(uint8_t(0x40u|command));
  odysseySdSpi.transfer(uint8_t(argument>>24));
  odysseySdSpi.transfer(uint8_t(argument>>16));
  odysseySdSpi.transfer(uint8_t(argument>>8));
  odysseySdSpi.transfer(uint8_t(argument));
  odysseySdSpi.transfer(crc);
  // CMD12 (STOP_TRANSMISSION) has one mandatory stuff byte before its R1 response in SPI mode.
  if (skipStuffByte) (void)odysseySdSpi.transfer(0xFF);
  uint8_t response=0xFF;
  for (uint8_t wait=0;wait<32;++wait) {
    response=odysseySdSpi.transfer(0xFF);
    if ((response&0x80u)==0) break;
  }
  return response;
}

static uint8_t odysseySdRearmProtocolLocked(const char* reason) {
  // A host reset can leave a continuously powered card inside a multi-block
  // read/write transaction. First run a recovery-only sequence at 100 kHz:
  // deselected clocks, write STOP token, CMD12, a bounded busy drain, then
  // two CMD0 chances. Healthy mounts never enter this path.
  odysseySdReleaseLocked();
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  odysseySdSpi.begin(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS);
  odysseySdSpi.beginTransaction(SPISettings(ODYSSEY_SD_RESCUE_FREQ_HZ,MSBFIRST,SPI_MODE0));

  uint8_t csHighByte=0x00;
  for (uint8_t i=0;i<64;++i) csHighByte=odysseySdSpi.transfer(0xFF);
  odysseySdLastCsHighByte=int16_t(csHighByte);

  digitalWrite(ODYSSEY_SD_CS,LOW);
  // CMD12 terminates a stranded multi-block read. It uses a valid CRC because
  // the previous session may have enabled command CRC before the host reset.
  const uint8_t cmd12=odysseySdCommandLocked(12u,0u,0x61u,true);
  odysseySdLastCmd12=int16_t(cmd12);
  uint8_t cmd12Byte=0x00;
  const bool cmd12Ready=odysseySdWaitReadyLocked(500u,cmd12Byte);
  odysseySdLastCmd12Ready=cmd12Ready?1u:2u;

  // 0xFD terminates a stranded multi-block write. If the card is still busy
  // programming its last block, repeat the stop token until that busy period
  // ends, bounded so a bad card cannot stall the device indefinitely.
  uint8_t rescueByte=cmd12Byte;
  const bool stopReady=odysseySdStopWriteLocked(ODYSSEY_SD_RESCUE_BUSY_MS,rescueByte);
  const bool rescueReady=stopReady && odysseySdWaitReadyLocked(500u,rescueByte);
  odysseySdLastRescueReady=rescueReady?1u:2u;
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  for (uint8_t i=0;i<20;++i) (void)odysseySdSpi.transfer(0xFF);
  delay(20);

  uint8_t response=0xFF;
  odysseySdLastCmdReady=0;
  for (uint8_t attempt=0;attempt<2 && response!=0x01;++attempt) {
    digitalWrite(ODYSSEY_SD_CS,LOW);
    uint8_t readyByte=0x00;
    const bool ready=odysseySdWaitReadyLocked(500u,readyByte);
    odysseySdLastCmdReady=ready?1u:2u;
    // Send GO_IDLE_STATE even when the card still reports busy; this matches
    // the stock 3.3.5 initializer and is our last software reset primitive.
    response=odysseySdCommandLocked(0u,0u,0x95u,false);
    digitalWrite(ODYSSEY_SD_CS,HIGH);
    for (uint8_t i=0;i<20;++i) (void)odysseySdSpi.transfer(0xFF);
    if (response!=0x01) delay(20);
  }

  odysseySdSpi.endTransaction();
  odysseySdSpi.end();
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  odysseySdLastCmd0=int16_t(response);
  if (response==0x00 || response==0x01) markOdysseySdBatteryDividerPresent();
  Serial.printf("[SD] %s powered-card rescue %lu Hz csHigh=0x%02X CMD12=0x%02X cmd12Ready=%u rescueReady=%u cmdReady=%u CMD0=0x%02X\n",
    reason,static_cast<unsigned long>(ODYSSEY_SD_RESCUE_FREQ_HZ),
    unsigned(uint8_t(odysseySdLastCsHighByte.load())),unsigned(cmd12),
    unsigned(odysseySdLastCmd12Ready.load()),unsigned(odysseySdLastRescueReady.load()),
    unsigned(odysseySdLastCmdReady.load()),unsigned(response));
  return response;
}

static bool odysseySdBeginLocked() {
  ++odysseySdBeginAttempts;
  odysseySdSpi.begin(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS);
  const bool mounted=SD.begin(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_INIT_FREQ_HZ,
    ODYSSEY_SD_MOUNT_POINT,ODYSSEY_SD_MAX_OPEN_FILES,false);
  if (mounted) markOdysseySdBatteryDividerPresent();
  else odysseySdReleaseLocked();
  return mounted;
}

static bool odysseySdValidateVfsLocked(const char* reason,uint8_t attempt) {
  struct stat root{};
  if (stat(ODYSSEY_SD_MOUNT_POINT,&root)!=0 || !S_ISDIR(root.st_mode)) {
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u VFS root unavailable errno=%d\n",reason,unsigned(attempt),errno);
    return false;
  }
  struct stat recordings{};
  if (stat(ODYSSEY_SD_RECORDING_DIR,&recordings)!=0) {
    if (errno!=ENOENT || mkdir(ODYSSEY_SD_RECORDING_DIR,0755)!=0) {
      odysseySdLastMountError=ESP_FAIL;
      odysseySdBootState=2;odysseySdProbeStage=4;
      Serial.printf("[SD] %s attempt %u recording directory unavailable errno=%d\n",
        reason,unsigned(attempt),errno);
      return false;
    }
  } else if (!S_ISDIR(recordings.st_mode)) {
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u /synap is not a directory\n",reason,unsigned(attempt));
    return false;
  }

  DIR* verified=opendir(ODYSSEY_SD_RECORDING_DIR);
  if (!verified) {
    const int saved=errno;
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u recordings opendir failed errno=%d\n",
      reason,unsigned(attempt),saved);
    return false;
  }
  if (closedir(verified)!=0) {
    const int saved=errno;
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u recordings closedir failed errno=%d\n",
      reason,unsigned(attempt),saved);
    return false;
  }

  const char* probePath="/odyssey-sd/synap/.synap-media-probe.tmp";
  FILE* probe=fopen(probePath,"wb");
  if (!probe) {
    const int saved=errno;
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u recordings not writable errno=%d\n",
      reason,unsigned(attempt),saved);
    return false;
  }
  const bool writeOk=fputc('S',probe)!=EOF && fflush(probe)==0;
  const int writeErrno=errno;
  const bool closeOk=fclose(probe)==0;
  const bool removeOk=unlink(probePath)==0;
  if (!writeOk || !closeOk || !removeOk) {
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u write/readiness probe failed errno=%d\n",
      reason,unsigned(attempt),writeOk?errno:writeErrno);
    return false;
  }
  return true;
}

static bool odysseySdMountOnceLocked(const char* reason,uint8_t attempt) {
  odysseySdBootState=0;
  odysseySdProbeStage=0;
  ++odysseySdMountAttempts;
  odysseySdReleaseLocked();

  pinMode(ODYSSEY_SD_CS,OUTPUT);
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  Serial.printf("[SD] %s attempt %u Arduino SPI init at %lu Hz, pins CS=%d SCK=%d MOSI=%d MISO=%d\n",
    reason,unsigned(attempt),static_cast<unsigned long>(ODYSSEY_SD_INIT_FREQ_HZ),
    ODYSSEY_SD_CS,ODYSSEY_SD_SCK,ODYSSEY_SD_MOSI,ODYSSEY_SD_MISO);

  bool mounted=odysseySdBeginLocked();
  if (!mounted) {
    const uint8_t cmd0=odysseySdRearmProtocolLocked(reason);
    delay(20);
    mounted=odysseySdBeginLocked();
    if (!mounted) {
      odysseySdLastMountError=ESP_FAIL;
      odysseySdBootState=2;odysseySdProbeStage=2;
      Serial.printf("[SD] %s attempt %u SD.begin failed after re-arm CMD0=0x%02X\n",
        reason,unsigned(attempt),unsigned(cmd0));
      odysseySdReleaseLocked();
      return false;
    }
  }

  const uint8_t type=SD.cardType();
  if (type==CARD_NONE) {
    odysseySdLastMountError=ESP_ERR_NOT_FOUND;
    odysseySdBootState=3;odysseySdProbeStage=0;
    Serial.printf("[SD] %s attempt %u mounted bus but card type is NONE\n",reason,unsigned(attempt));
    odysseySdReleaseLocked();
    return false;
  }

  if (!odysseySdValidateVfsLocked(reason,attempt)) {
    odysseySdReleaseLocked();
    return false;
  }

  odysseySdLastMountError=ESP_OK;
  odysseySdBootState=1;
  odysseySdProbeStage=6;
  const char* label=type==CARD_MMC?"MMC":type==CARD_SD?"SDSC":type==CARD_SDHC?"SDHC/SDXC":"unknown";
  Serial.printf("[SD] ready via proven Arduino SPI path: %s, %llu MiB, %lu Hz\n",
    label,static_cast<unsigned long long>(SD.cardSize()/(1024ULL*1024ULL)),
    static_cast<unsigned long>(ODYSSEY_SD_INIT_FREQ_HZ));
  return true;
}

static bool odysseySdMountLocked(const char* reason,uint8_t attempts) {
  if (odysseySdReady()) return true;
  for (uint8_t attempt=1;attempt<=attempts;++attempt) {
    if (odysseySdMountOnceLocked(reason,attempt)) return true;
  }
  Serial.printf("[SD] %s failed after %u attempt(s), state=%u stage=%u\n",
    reason,unsigned(attempts),unsigned(odysseySdBootState.load()),unsigned(odysseySdProbeStage.load()));
  return false;
}

void odysseyDetectSdCard() {
  OdysseySdGuard guard;
  if (!guard) { odysseySdBootState=2;odysseySdProbeStage=1; return; }
  odysseySdMountLocked("probe",1);
}
bool odysseyInitializeSdCardBeforeBle() {
  // A powered SD adapter can be less tolerant than the C3 itself during boot.
  // Preserve the proven 3 s first-transaction settle window, but keep the
  // mount before BLE so catalogue state is deterministic at connection time.
  odysseyWaitForSdStartupSettle();
  OdysseySdGuard guard;
  if (!guard) { odysseySdBootState=2;odysseySdProbeStage=1; return false; }
  const bool ready=odysseySdMountLocked("boot",ODYSSEY_SD_BOOT_ATTEMPTS);
  Serial.printf("[SD] boot initialization complete state=%u stage=%u before BLE\n",
    unsigned(odysseySdBootState.load()),unsigned(odysseySdProbeStage.load()));
  return ready;
}
bool odysseyRecoverSdCard() {
  OdysseySdGuard guard(pdMS_TO_TICKS(2000));
  if (!guard) return false;
  odysseySdBootState=0;odysseySdProbeStage=0;
  odysseySdReleaseLocked();
  return odysseySdMountLocked("recovery",ODYSSEY_SD_RECOVERY_ATTEMPTS);
}

bool odysseyPrepareSdForPowerTransition(uint32_t timeoutMs) {
  OdysseySdGuard guard(pdMS_TO_TICKS(timeoutMs));
  if (!guard) {
    Serial.println("[SD] power transition deferred: storage busy");
    return false;
  }
  if (odysseyRecording.load()) {
    Serial.println("[SD] power transition deferred: local recording active");
    return false;
  }
  const bool wasReady=odysseySdReady();
  odysseySdBootState=0;odysseySdProbeStage=0;
  odysseySdReleaseLocked();
  const uint8_t cmd0=odysseySdRearmProtocolLocked("power-transition");
  odysseySdBootState=0;odysseySdProbeStage=0;
  Serial.printf("[SD] power transition prepared ready=%u CMD0=0x%02X\n",
    wasReady?1u:0u,unsigned(cmd0));
  return true;
}
#else
// Odyssey S3 remains detection-only and retains the existing Arduino SD probe.
static SPIClass odysseySdSpi(FSPI);
void odysseyDetectSdCard() {
  odysseySdBootState=0;odysseySdProbeStage=0;
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
#endif
#endif
