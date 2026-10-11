// C3 native ESP-IDF SDSPI + FatFs/VFS storage backend.
// No Arduino SD/SPI implementation is linked to the Odyssey C3 mount path.
// S3 detection-only stays on Arduino SD/SPI; Chakshu is independent.
#if !SYNAP_CHAKSHU
#if CONFIG_IDF_TARGET_ESP32C3
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
uint8_t odysseyLastRecordFailureStage();
#else
#include <SPI.h>
#include <SD.h>
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
// SDSPI keeps its standard 400 kHz card initialization clock. Cap data
// transfers at 800 kHz rather than 1 MHz on the sealed C3+SD carrier:
// reduced signal-edge stress with enough theoretical bandwidth for 16 kHz
// mono PCM (32 kB/s) and metadata writes. Never change the FAT format.
static constexpr uint32_t ODYSSEY_SD_DATA_FREQ_HZ=800000u;
static constexpr spi_host_device_t ODYSSEY_SD_HOST=SPI2_HOST;
static sdmmc_card_t* odysseySdNativeCard=nullptr;
static bool odysseySdNativeBusInitialized=false;
static std::atomic<uint32_t> odysseyNativeIoFault{0};
// Fault layout 0x20SS00EE (native FatFs stage SS, errno EE).
// Keep the existing BLE/NVS diagnostic ABI; no Arduino SD driver is linked.
extern "C" uint32_t synapSdWriteFaultCode() { return odysseyNativeIoFault.load(); }
extern "C" void synapSdClearWriteFaultCode() { odysseyNativeIoFault=0; }
void synapSdNoteNativeWriteError(uint8_t stage,int ioError) {
  const uint32_t code=0x20000000u|(uint32_t(stage)<<16)|
    uint32_t(ioError>0?ioError&255:EIO);
  uint32_t expected=0;
  (void)odysseyNativeIoFault.compare_exchange_strong(expected,code);
}
// Preserve the known-good build-1445 lifecycle: one mount attempt per
// explicit action. Repeating SD.end()/SPI.end()/SD.begin() autonomously on a
// continuously powered card is itself a state mutation and obscures the first
// failure we need to diagnose.
static constexpr uint8_t ODYSSEY_SD_BOOT_ATTEMPTS=1;
static constexpr uint8_t ODYSSEY_SD_RECOVERY_ATTEMPTS=1;
// Stopping an open-ended CMD18 read costs about one 512-byte block of clocking.
// Budget generously but never let a reset path stall on a card that will not
// answer: a healthy idle card breaks out of the drain within a millisecond.
static constexpr uint32_t ODYSSEY_SD_QUIESCE_BUDGET_MS=250u;
// Match the last independently observed healthy build (1445) exactly.
static constexpr size_t ODYSSEY_SD_MAX_OPEN_FILES=1;

// Native SPI2 SDSPI host owns the bus; FAT keeps the existing POSIX paths.
static std::atomic<int32_t> odysseySdLastMountError{ESP_OK};
static std::atomic<uint32_t> odysseySdMountAttempts{0};
static std::atomic<uint32_t> odysseySdBeginAttempts{0};
static std::atomic<uint8_t> odysseySdLastMountReason{0}; // 1=boot,2=op14,3=touch,4=probe
// VFS stages: 0=not run; 1=root; 2=directory stat; 3=directory type;
// 4=opendir; 5=closedir; 11=read-verified; 13=missing /synap (valid empty FAT).
// A boot mount NEVER writes metadata; user-initiated recording tests write.
// Diagnostic only: these fields must not change the proven SD mount path.
static std::atomic<uint8_t> odysseySdVfsStep{0};
static std::atomic<int32_t> odysseySdVfsErrno{0};
static std::atomic<int16_t> odysseySdBitBangCsHigh{-2};
static std::atomic<int16_t> odysseySdBitBangCsLow{-2};
static std::atomic<uint8_t> odysseySdBitBangStopState{0}; // 1=released,2=still-busy,3=no-ready
static std::atomic<int16_t> odysseySdBitBangCmd12{-2};
static std::atomic<uint8_t> odysseySdBitBangCmd12Ready{0}; // 1=sustained idle after CMD12,2=no idle window
static std::atomic<uint32_t> odysseySdBitBangDrainBytes{0};
static std::atomic<uint16_t> odysseySdRawZero{0};
static std::atomic<uint16_t> odysseySdRawFF{0};
static std::atomic<uint16_t> odysseySdRawFE{0};
static std::atomic<uint16_t> odysseySdRawOther{0};
static std::atomic<uint16_t> odysseySdRawMaxFFRun{0};
static std::atomic<int16_t> odysseySdBitBangCmd0{-2};
static std::atomic<int16_t> odysseySdBitBangCmd8{-2};
static std::atomic<uint32_t> odysseySdBitBangR7{0};
static StaticSemaphore_t odysseySdMutexStorage;
static SemaphoreHandle_t odysseySdMutex=nullptr;

int32_t odysseySdLastError() { return odysseySdLastMountError.load(); }
uint32_t odysseySdAttemptCount() { return odysseySdMountAttempts.load(); }
uint32_t odysseySdBeginAttemptCount() { return odysseySdBeginAttempts.load(); }
uint8_t odysseySdLastMountReasonCode() { return odysseySdLastMountReason.load(); }
uint8_t odysseySdVfsStepValue() { return odysseySdVfsStep.load(); }
int32_t odysseySdVfsErrnoValue() { return odysseySdVfsErrno.load(); }
int16_t odysseySdBitBangCsHighState() { return odysseySdBitBangCsHigh.load(); }
int16_t odysseySdBitBangCsLowState() { return odysseySdBitBangCsLow.load(); }
uint8_t odysseySdBitBangStopStateValue() { return odysseySdBitBangStopState.load(); }
int16_t odysseySdBitBangCmd12Response() { return odysseySdBitBangCmd12.load(); }
uint8_t odysseySdBitBangCmd12ReadyState() { return odysseySdBitBangCmd12Ready.load(); }
uint32_t odysseySdBitBangDrainByteCount() { return odysseySdBitBangDrainBytes.load(); }
uint16_t odysseySdRawZeroCount() { return odysseySdRawZero.load(); }
// A card that still drives MISO LOW while deselected cannot be recovered
// by repeatedly restarting the ESP32 SPI peripheral. Keep explicit recovery.
bool odysseySdBusStuckLow() {
  return odysseySdBootState.load()!=1 &&
    odysseySdBitBangCsHigh.load()==0 && odysseySdRawZero.load()>=900u;
}
uint16_t odysseySdRawFFCount() { return odysseySdRawFF.load(); }
uint16_t odysseySdRawFECount() { return odysseySdRawFE.load(); }
uint16_t odysseySdRawOtherCount() { return odysseySdRawOther.load(); }
uint16_t odysseySdRawMaxFFRunCount() { return odysseySdRawMaxFFRun.load(); }
int16_t odysseySdBitBangCmd0Response() { return odysseySdBitBangCmd0.load(); }
int16_t odysseySdBitBangCmd8Response() { return odysseySdBitBangCmd8.load(); }
uint32_t odysseySdBitBangR7Response() { return odysseySdBitBangR7.load(); }
void odysseySdUseProbingClock() {}
void odysseySdMarkVfsFailure() {
  // Observation only. Catalogue/read errors must never schedule a destructive
  // remount behind the user's back. Explicit PWA Check SD (op14) or a physical
  // disconnected double-tap may request recovery.
  odysseySdVfsStep=11;odysseySdVfsErrno=errno?errno:EIO;
  odysseySdBootState=2;odysseySdProbeStage=4;
  odysseySdLastMountError=ESP_FAIL;
  // A failed CMD18/read may leave an always-powered SD card streaming even
  // though the VFS now reports "not ready". Require a validated recovery
  // before deep sleep; do not treat a failed read as a safely absent card.
  odysseySdUnsafeToSleep=true;
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
static bool odysseySdReleaseLocked() {
  // Only called while holding OdysseySdGuard. Never unmount while the
  // microphone recorder or an SD media transfer owns an open FILE*.
  if (odysseySdNativeCard) {
    const esp_err_t result=esp_vfs_fat_sdcard_unmount(
      ODYSSEY_SD_MOUNT_POINT,odysseySdNativeCard);
    if (result!=ESP_OK) {
      odysseySdLastMountError=result;
      odysseySdUnsafeToSleep=true;
      Serial.printf("[SD] native unmount rejected err=%s\\n",esp_err_to_name(result));
      return false;
    }
    odysseySdNativeCard=nullptr;
  }
  if (odysseySdNativeBusInitialized) {
    const esp_err_t busResult=spi_bus_free(ODYSSEY_SD_HOST);
    if (busResult!=ESP_OK) {
      odysseySdLastMountError=busResult;
      odysseySdUnsafeToSleep=true;
      Serial.printf("[SD] SPI2 bus teardown rejected err=%s\\n",esp_err_to_name(busResult));
      return false;
    }
    odysseySdNativeBusInitialized=false;
  }
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  return true;
}

static uint8_t odysseySdBitBangTransfer(uint8_t out) {
  uint8_t in=0;
  for (uint8_t bit=0;bit<8;++bit) {
    digitalWrite(ODYSSEY_SD_MOSI,(out&0x80u)?HIGH:LOW);
    delayMicroseconds(4);
    digitalWrite(ODYSSEY_SD_SCK,HIGH);
    delayMicroseconds(4);
    in=uint8_t((in<<1)|(digitalRead(ODYSSEY_SD_MISO)==HIGH?1u:0u));
    digitalWrite(ODYSSEY_SD_SCK,LOW);
    delayMicroseconds(4);
    out<<=1;
  }
  return in;
}

static bool odysseySdBitBangWaitReady(uint32_t timeoutMs,uint8_t& lastByte) {
  const uint32_t started=millis();
  do {
    lastByte=odysseySdBitBangTransfer(0xFF);
    if (lastByte==0xFF) return true;
  } while (uint32_t(millis()-started)<timeoutMs);
  return false;
}

static uint8_t odysseySdBitBangCommand(uint8_t command,uint32_t argument,uint8_t crc,
    uint8_t* tail=nullptr,size_t tailSize=0,bool skipStuffByte=false) {
  digitalWrite(ODYSSEY_SD_CS,LOW);
  odysseySdBitBangTransfer(uint8_t(0x40u|command));
  odysseySdBitBangTransfer(uint8_t(argument>>24));
  odysseySdBitBangTransfer(uint8_t(argument>>16));
  odysseySdBitBangTransfer(uint8_t(argument>>8));
  odysseySdBitBangTransfer(uint8_t(argument));
  odysseySdBitBangTransfer(crc);
  if (skipStuffByte) (void)odysseySdBitBangTransfer(0xFF);
  uint8_t response=0xFF;
  for (uint8_t i=0;i<32;++i) {
    response=odysseySdBitBangTransfer(0xFF);
    if ((response&0x80u)==0) break;
  }
  if ((response&0x80u)==0 && tail) {
    for (size_t i=0;i<tailSize;++i) tail[i]=odysseySdBitBangTransfer(0xFF);
  }
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  (void)odysseySdBitBangTransfer(0xFF);
  return response;
}

static uint8_t odysseySdBitBangStopReadLocked(uint8_t& responseCandidate,uint32_t& drainedBytes,
    uint32_t budgetMs=0) {
  // CMD12 may be issued while CMD18 data is still flowing. Bytes returned
  // immediately after the mandatory stuff byte can therefore be payload and
  // must not be classified as R1 merely because bit 7 is clear. Keep CS low,
  // continue clocking, and require a sustained 0xFF idle window before
  // considering the read stream quiesced.
  digitalWrite(ODYSSEY_SD_CS,LOW);
  odysseySdBitBangTransfer(0x4Cu); // CMD12
  odysseySdBitBangTransfer(0x00);
  odysseySdBitBangTransfer(0x00);
  odysseySdBitBangTransfer(0x00);
  odysseySdBitBangTransfer(0x00);
  odysseySdBitBangTransfer(0x61u);
  (void)odysseySdBitBangTransfer(0xFF); // mandatory CMD12 stuff byte

  responseCandidate=0xFF;
  drainedBytes=0;
  uint16_t idleRun=0;
  static constexpr uint32_t MAX_DRAIN_BYTES=8192u;
  static constexpr uint16_t REQUIRED_IDLE_BYTES=64u;
  bool idle=false;
  // A reset path cannot afford the full byte cap, so callers that are on their
  // way to esp_restart()/deep sleep pass a wall-clock budget instead.
  const uint32_t deadlineStarted=millis();
  for (;drainedBytes<MAX_DRAIN_BYTES;++drainedBytes) {
    if (budgetMs && uint32_t(millis()-deadlineStarted)>=budgetMs) break;
    const uint8_t value=odysseySdBitBangTransfer(0xFF);
    if (responseCandidate==0xFF && (value&0x80u)==0) responseCandidate=value;
    if (value==0xFF) {
      if (++idleRun>=REQUIRED_IDLE_BYTES) {
        idle=true;
        ++drainedBytes;
        break;
      }
    } else {
      idleRun=0;
    }
  }
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  for (uint8_t i=0;i<10;++i) (void)odysseySdBitBangTransfer(0xFF);
  return idle?1:2;
}


static uint8_t odysseySdBitBangGoIdleLocked() {
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  for (uint8_t i=0;i<20;++i) (void)odysseySdBitBangTransfer(0xFF);
  uint8_t cmd0=0xFF;
  for (uint8_t attempt=0;attempt<2 && cmd0!=0x01;++attempt) {
    cmd0=odysseySdBitBangCommand(0u,0u,0x95u);
    if (cmd0!=0x01) {
      digitalWrite(ODYSSEY_SD_CS,HIGH);
      for (uint8_t i=0;i<20;++i) (void)odysseySdBitBangTransfer(0xFF);
      delay(20);
    }
  }
  return cmd0;
}

static uint8_t odysseySdBitBangStopWriteLocked(uint32_t readyMs=1500u,uint32_t releaseMs=3000u) {
  // If a reset interrupted CMD25, the card can be waiting for another data
  // token rather than a command. Wait through any program-busy interval, then
  // send the SPI multi-block write stop token 0xFD.
  digitalWrite(ODYSSEY_SD_CS,LOW);
  uint8_t last=0;
  if (!odysseySdBitBangWaitReady(readyMs,last)) {
    digitalWrite(ODYSSEY_SD_CS,HIGH);
    (void)odysseySdBitBangTransfer(0xFF);
    return 3;
  }
  (void)odysseySdBitBangTransfer(0xFD);
  const bool released=odysseySdBitBangWaitReady(releaseMs,last);
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  (void)odysseySdBitBangTransfer(0xFF);
  return released?1:2;
}

static void odysseySdSampleRawLocked() {
  // Passive classification before sending any recovery command. Clock 1024
  // bytes with MOSI high and record what DO is actually doing. An idle command
  // bus is overwhelmingly 0xFF; a program-busy/stuck-low card is overwhelmingly
  // 0x00; an active CMD18 stream produces varied bytes and often 0xFE tokens.
  uint16_t zero=0,ff=0,fe=0,other=0,maxFFRun=0,ffRun=0;
  digitalWrite(ODYSSEY_SD_CS,LOW);
  for (uint16_t i=0;i<1024u;++i) {
    const uint8_t value=odysseySdBitBangTransfer(0xFF);
    if (value==0x00) ++zero;
    else if (value==0xFF) ++ff;
    else if (value==0xFE) ++fe;
    else ++other;
    if (value==0xFF) {
      if (++ffRun>maxFFRun) maxFFRun=ffRun;
    } else {
      ffRun=0;
    }
  }
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  (void)odysseySdBitBangTransfer(0xFF);
  odysseySdRawZero=zero;
  odysseySdRawFF=ff;
  odysseySdRawFE=fe;
  odysseySdRawOther=other;
  odysseySdRawMaxFFRun=maxFFRun;
}

static uint8_t odysseySdBitBangRecoverLocked(const char* reason) {
  // Runs only after the exact known-good 1445 SD.begin() has failed. The
  // sequence deliberately bypasses SPIClass/SD/FatFS so it can recover a card
  // whose previous host reset happened inside CMD18/CMD25.
  odysseySdReleaseLocked();
  pinMode(ODYSSEY_SD_CS,OUTPUT);digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_SCK,OUTPUT);digitalWrite(ODYSSEY_SD_SCK,LOW);
  pinMode(ODYSSEY_SD_MOSI,OUTPUT);digitalWrite(ODYSSEY_SD_MOSI,HIGH);
  // A deselected SD DO line is allowed to float. Pull it high weakly so a
  // tri-stated bus is distinguishable from a card actively driving busy-low.
  pinMode(ODYSSEY_SD_MISO,INPUT_PULLUP);
  delay(1);
  odysseySdBitBangCsHigh=digitalRead(ODYSSEY_SD_MISO)==HIGH?1:0;
  for (uint8_t i=0;i<32;++i) (void)odysseySdBitBangTransfer(0xFF);

  digitalWrite(ODYSSEY_SD_CS,LOW);delayMicroseconds(20);
  odysseySdBitBangCsLow=digitalRead(ODYSSEY_SD_MISO)==HIGH?1:0;
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  (void)odysseySdBitBangTransfer(0xFF);

  odysseySdSampleRawLocked();

  // Observed in the field after stage 66: MISO is HIGH when CS is released,
  // but ~1023/1024 response bytes are 0x00 while selected. This is a card
  // continuously asserting BUSY, NOT an idle SPI command interface. Injecting
  // CMD12/CMD0/0xFD at this point cannot safely reset a card programming its
  // own NAND. Preserve the media and request a real SD power-rail cycle.
  if (odysseySdRawZero.load()>=1000u && odysseySdRawFF.load()<=2u) {
    odysseySdBitBangCmd12=0xFF;
    odysseySdBitBangCmd12Ready=0;
    odysseySdBitBangDrainBytes=0;
    odysseySdBitBangStopState=3;
    odysseySdBitBangCmd0=0xFF;
    odysseySdBitBangCmd8=0xFF;
    odysseySdBitBangR7=0;
    odysseySdUnsafeToSleep=true;
    digitalWrite(ODYSSEY_SD_CS,HIGH);
    digitalWrite(ODYSSEY_SD_SCK,LOW);
    digitalWrite(ODYSSEY_SD_MOSI,HIGH);
    Serial.printf("[SD] %s sustained selected-busy 0x00 (%u/1024); suppress CMD12/CMD0, cycle SD rail\\n",
      reason,unsigned(odysseySdRawZero.load()));
    return 0xFF;
  }

  // The historical failure was first observed after catalogue/read activity.
  // Drain the open-ended CMD18 stream after CMD12 instead of trying to parse
  // an R1 byte out of data that may still be arriving from the card.
  uint8_t cmd12=0xFF;
  uint32_t drainBytes=0;
  const uint8_t cmd12Ready=odysseySdBitBangStopReadLocked(cmd12,drainBytes);
  odysseySdBitBangCmd12=int16_t(cmd12);
  odysseySdBitBangCmd12Ready=cmd12Ready;
  odysseySdBitBangDrainBytes=drainBytes;

  uint8_t cmd0=odysseySdBitBangGoIdleLocked();

  // If CMD12 did not restore command mode, the other recoverable state is an
  // interrupted CMD25 multi-block write. A waiting writer accepts 0xFD only
  // after it is no longer program-busy; then try GO_IDLE_STATE again.
  uint8_t stopState=0;
  if (cmd0!=0x01) {
    stopState=odysseySdBitBangStopWriteLocked();
    if (stopState==1) cmd0=odysseySdBitBangGoIdleLocked();
  }
  odysseySdBitBangStopState=stopState;
  odysseySdBitBangCmd0=int16_t(cmd0);

  uint8_t r7[4]{};
  uint8_t cmd8=0xFF;
  if (cmd0==0x01) cmd8=odysseySdBitBangCommand(8u,0x1AAu,0x87u,r7,sizeof(r7));
  odysseySdBitBangCmd8=int16_t(cmd8);
  odysseySdBitBangR7=(uint32_t(r7[0])<<24)|(uint32_t(r7[1])<<16)|(uint32_t(r7[2])<<8)|uint32_t(r7[3]);

  digitalWrite(ODYSSEY_SD_CS,HIGH);
  digitalWrite(ODYSSEY_SD_SCK,LOW);
  digitalWrite(ODYSSEY_SD_MOSI,HIGH);
  Serial.printf("[SD] %s GPIO recovery high=%d low=%d raw0=%u rawFF=%u rawFE=%u rawOther=%u rawMaxFF=%u CMD12candidate=0x%02X readIdle=%u drain=%lu stop=%u CMD0=0x%02X CMD8=0x%02X R7=0x%08lX\n",
    reason,int(odysseySdBitBangCsHigh.load()),int(odysseySdBitBangCsLow.load()),
    unsigned(odysseySdRawZero.load()),unsigned(odysseySdRawFF.load()),
    unsigned(odysseySdRawFE.load()),unsigned(odysseySdRawOther.load()),
    unsigned(odysseySdRawMaxFFRun.load()),unsigned(cmd12),unsigned(cmd12Ready),
    static_cast<unsigned long>(drainBytes),unsigned(stopState),unsigned(cmd0),unsigned(cmd8),
    static_cast<unsigned long>(odysseySdBitBangR7.load()));
  return cmd0;
}

// Stop whatever the card is doing before the host goes away.
//
// SD.end() tears down the *host*: it sends no clocks and no command. So a reset
// taken while a CMD18 multi-block read or a CMD25 multi-block write is open
// leaves the card still driving DO on a rail that is never cycled, and the next
// boot meets a bus that answers 0x00 to every command. That is precisely the
// state the GPIO recovery above has to dig out of, and recovery is far less
// reliable than simply not creating the mess: CMD12 plus a drain to idle on the
// way out costs about one block of clocking and leaves the card addressable.
//
// Returns 1 when the bus reached a sustained idle window, 2 otherwise.
static uint8_t odysseySdQuiesceLocked(uint32_t budgetMs) {
  (void)budgetMs;
  // Native SDSPI knows whether the card has completed the last write.
  // A direct CMD13 check is safer than injecting legacy CMD12/STOP tokens
  // into a healthy native transaction stream during OTA/restart.
  if (odysseySdNativeCard) {
    const esp_err_t status=sdmmc_get_status(odysseySdNativeCard);
    if (status!=ESP_OK) {
      odysseySdLastMountError=status;
      odysseySdUnsafeToSleep=true;
      Serial.printf("[SD] native CMD13 idle status failed err=%s\\n",esp_err_to_name(status));
      return 2;
    }
  }
  if (!odysseySdReleaseLocked()) return 2;
  Serial.println("[SD] native SDSPI clean unmount before power transition");
  return 1;
}

static bool odysseySdBeginLocked() {
  // ESP-IDF SDSPI host, not Arduino SD.begin(). The card is never formatted
  // or repartitioned, including after failed mount or watchdog reset.
  ++odysseySdBeginAttempts;
  if (!odysseySdReleaseLocked()) return false;
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  pinMode(ODYSSEY_SD_MISO,INPUT_PULLUP); // idle-high during deselection
  spi_bus_config_t bus{};
  bus.mosi_io_num=ODYSSEY_SD_MOSI;
  bus.miso_io_num=ODYSSEY_SD_MISO;
  bus.sclk_io_num=ODYSSEY_SD_SCK;
  bus.quadwp_io_num=-1;
  bus.quadhd_io_num=-1;
  bus.max_transfer_sz=4096+512;
  esp_err_t result=spi_bus_initialize(ODYSSEY_SD_HOST,&bus,SDSPI_DEFAULT_DMA);
  if (result!=ESP_OK) {
    odysseySdLastMountError=result;
    Serial.printf("[SD] native SPI2 bus init failed err=%s\\n",esp_err_to_name(result));
    return false;
  }
  odysseySdNativeBusInitialized=true;
  sdmmc_host_t host=SDSPI_HOST_DEFAULT();
  host.slot=ODYSSEY_SD_HOST;
  host.max_freq_khz=ODYSSEY_SD_DATA_FREQ_HZ/1000u;
  sdspi_device_config_t device=SDSPI_DEVICE_CONFIG_DEFAULT();
  device.host_id=ODYSSEY_SD_HOST;
  device.gpio_cs=static_cast<gpio_num_t>(ODYSSEY_SD_CS);
  esp_vfs_fat_sdmmc_mount_config_t mount{};
  mount.format_if_mount_failed=false;
  mount.max_files=ODYSSEY_SD_MAX_OPEN_FILES;
  mount.allocation_unit_size=4096; // ignored for an existing FAT filesystem
  // IDF FAT diskio normally trusts cached card readiness; enable the real
  // SD status check on this unstable removable-media path. Detect a failed
  // controller earlier instead of issuing additional FAT metadata writes.
  mount.disk_status_check_enable=true;
  result=esp_vfs_fat_sdspi_mount(ODYSSEY_SD_MOUNT_POINT,
    &host,&device,&mount,&odysseySdNativeCard);
  if (result!=ESP_OK) {
    odysseySdLastMountError=result;
    odysseySdNativeCard=nullptr; // mount helper releases its own card/device
    Serial.printf("[SD] native SDSPI mount failed err=%s, no format\\n",
      esp_err_to_name(result));
    (void)odysseySdReleaseLocked(); // free bus after mount's cleanup
    return false;
  }
  markOdysseySdBatteryDividerPresent();
  sampleBattery(true); // telemetry only, NEVER SD admission
  return true;
}

static bool odysseySdValidateVfsLocked(const char* reason,uint8_t attempt) {
  // Power-loss-safe BOOT contract: validate the FAT volume and existing
  // directories using ONLY reads. Never create a test file, force fsync,
  // unlink, format or mkdir during mount. Battery ADC is not consulted.
  // Explicit recording creates /synap and verifies its own writes.
  odysseySdVfsStep=0;odysseySdVfsErrno=0;
  struct stat root{};
  if (stat(ODYSSEY_SD_MOUNT_POINT,&root)!=0 || !S_ISDIR(root.st_mode)) {
    odysseySdVfsStep=1;odysseySdVfsErrno=errno?errno:ENOTDIR;
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u FAT root not readable errno=%d\\n",
      reason,unsigned(attempt),int(odysseySdVfsErrno.load()));
    return false;
  }
  struct stat recordings{};
  errno=0;
  if (stat(ODYSSEY_SD_RECORDING_DIR,&recordings)!=0) {
    const int directoryErrno=errno;
    if (directoryErrno==ENOENT) {
      // A formatted or recovered SD with no recordings is still healthy.
      // Directory creation is reserved for explicit double-tap capture.
      odysseySdVfsStep=13;odysseySdVfsErrno=ENOENT;
      Serial.printf("[SD] %s FAT root verified, /synap absent; mount ready without writes\\n",reason);
      return true;
    }
    odysseySdVfsStep=2;odysseySdVfsErrno=directoryErrno?directoryErrno:EIO;
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u /synap stat I/O fault errno=%d\\n",
      reason,unsigned(attempt),int(odysseySdVfsErrno.load()));
    return false;
  }
  if (!S_ISDIR(recordings.st_mode)) {
    odysseySdVfsStep=3;odysseySdVfsErrno=ENOTDIR;
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u /synap is not a directory\\n",reason,unsigned(attempt));
    return false;
  }
  DIR* verified=opendir(ODYSSEY_SD_RECORDING_DIR);
  if (!verified) {
    odysseySdVfsStep=4;odysseySdVfsErrno=errno?errno:EIO;
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u /synap opendir failed errno=%d\\n",
      reason,unsigned(attempt),int(odysseySdVfsErrno.load()));
    return false;
  }
  if (closedir(verified)!=0) {
    odysseySdVfsStep=5;odysseySdVfsErrno=errno?errno:EIO;
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u /synap closedir failed errno=%d\\n",
      reason,unsigned(attempt),int(odysseySdVfsErrno.load()));
    return false;
  }
  odysseySdVfsStep=11;odysseySdVfsErrno=0; // read-verified, no boot-time FAT mutation
  Serial.printf("[SD] %s FAT directory verified; no boot write probe, no ADC admission\\n",reason);
  return true;
}

static uint8_t odysseySdMountReasonCode(const char* reason) {
  if (!strcmp(reason,"boot")) return 1;
  if (!strcmp(reason,"op14")) return 2;
  if (!strcmp(reason,"touch")) return 3;
  if (!strcmp(reason,"rearm")) return 5;
  return 4;
}

static bool odysseySdMountOnceLocked(const char* reason,uint8_t attempt) {
  odysseySdBootState=0;
  odysseySdProbeStage=0;
  odysseySdLastMountReason=odysseySdMountReasonCode(reason);
  ++odysseySdMountAttempts;
  odysseySdVfsStep=0;odysseySdVfsErrno=0;
  if (!odysseySdReleaseLocked()) return false;
  Serial.printf("[SD] %s attempt %u native ESP-IDF SDSPI at %lu Hz, pins CS=%d SCK=%d MOSI=%d MISO=%d\\n",
    reason,unsigned(attempt),static_cast<unsigned long>(ODYSSEY_SD_DATA_FREQ_HZ),
    ODYSSEY_SD_CS,ODYSSEY_SD_SCK,ODYSSEY_SD_MOSI,ODYSSEY_SD_MISO);

  bool mounted=odysseySdBeginLocked();
  if (!mounted) {
    const uint8_t bitBangCmd0=odysseySdBitBangRecoverLocked(reason);
    // If direct GPIO recovery returns the card to SPI idle, retry the exact
    // stock Arduino path once. No formatting or media mutation is performed.
    if (bitBangCmd0==0x01) {
      delay(20);
      mounted=odysseySdBeginLocked();
    }
    if (!mounted) {
      // MISO driven LOW even with CS HIGH is an unsafe bus, not proven absence.
      // Keep sleep veto through reset/retry until a verified mount succeeds.
      if (odysseySdBitBangCsHigh.load()==0 && odysseySdRawZero.load()>=900u) {
        odysseySdUnsafeToSleep=true;
        Serial.println("[SD] unsafe-to-sleep: MISO driven low after failed mount");
      }
      odysseySdLastMountError=ESP_FAIL;
      odysseySdBootState=2;odysseySdProbeStage=2;
      Serial.printf("[SD] %s attempt %u exact-1445 SD.begin failed; GPIO CMD0=0x%02X\n",
        reason,unsigned(attempt),unsigned(bitBangCmd0));
      odysseySdReleaseLocked();
      return false;
    }
  }

  if (!odysseySdNativeCard) {
    odysseySdLastMountError=ESP_ERR_NOT_FOUND;
    odysseySdBootState=3;odysseySdProbeStage=0;
    (void)odysseySdReleaseLocked();
    return false;
  }

  if (!odysseySdValidateVfsLocked(reason,attempt)) {
    odysseySdReleaseLocked();
    return false;
  }

  // Persist the field C3+SD divider only after a genuine mounted and
  // VFS-validated SD session. This survives failed SD probes and cold boots.
  persistOdysseySdBatteryDividerProfile();
  odysseySdLastMountError=ESP_OK;
  odysseySdBootState=1;
  odysseySdProbeStage=6;
  // A complete mount plus writable VFS validation is the explicit recovery
  // that makes a previously failed sleep quiesce safe to attempt again.
  odysseySdUnsafeToSleep=false;
  const uint64_t bytes=uint64_t(odysseySdNativeCard->csd.capacity)*
    uint64_t(odysseySdNativeCard->csd.sector_size);
  Serial.printf("[SD] native SDSPI FAT ready capacity=%llu MiB clock=%lu Hz\\n",
    static_cast<unsigned long long>(bytes/(1024ULL*1024ULL)),
    static_cast<unsigned long>(ODYSSEY_SD_DATA_FREQ_HZ));
  return true;
}

static bool odysseySdMountLocked(const char* reason,uint8_t attempts) {
  if (odysseySdReady()) return true;
  for (uint8_t attempt=1;attempt<=attempts;++attempt) {
    if (odysseySdMountOnceLocked(reason,attempt)) return true;
    // Do not hammer a deselected busy-low SD on a sealed C3.
    // A controller-only restart cannot physically power-cycle that card.
    if (reason && !strcmp(reason,"boot") &&
        odysseySdBitBangCsHigh.load()==0 && odysseySdRawZero.load()>=900u) {
      odysseySdUnsafeToSleep=true;
      Serial.println("[SD] boot: SD MISO held LOW; deferring more probes until recovery");
      break;
    }
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
  OdysseySdGuard guard;
  if (!guard) { odysseySdBootState=2;odysseySdProbeStage=1; return false; }

  // A retained FAT write EIO survives MCU reset but not necessarily a
  // real SD power reset. Warm/OTA reboot cannot reset the continuously-powered
  // card. Do not send CMD0/CMD12 or even a new mount while its controller may
  // still be programming. A true cold power-on may try one normal mount,
  // followed only by guarded/passive recovery. User op14 remains explicit.
  const uint8_t previousRecordStage=odysseyLastRecordFailureStage();
  const bool previousStorageFault=previousRecordStage>=44u && previousRecordStage!=48u;
  if (previousStorageFault) {
    odysseySdUnsafeToSleep=true;
    if (bootResetReason!=ESP_RST_POWERON) {
      odysseySdBootState=2;
      odysseySdProbeStage=previousRecordStage;
      Serial.printf("[SD] warm boot deferred: historical record stage=%u, SD rail not known to have reset\\n",
        unsigned(previousRecordStage));
      return false;
    }
    Serial.printf("[SD] cold CPU power-on: try one SD mount after retained stage=%u\\n",
      unsigned(previousRecordStage));
  }

  const bool ready=odysseySdMountLocked("boot",ODYSSEY_SD_BOOT_ATTEMPTS);
  Serial.printf("[SD] boot initialization complete state=%u stage=%u before BLE\n",
    unsigned(odysseySdBootState.load()),unsigned(odysseySdProbeStage.load()));
  return ready;
}
bool odysseyRecoverSdCard(const char* reason) {
  OdysseySdGuard guard(pdMS_TO_TICKS(5000));
  if (!guard) return false;
  odysseySdBootState=0;odysseySdProbeStage=0;
  odysseySdReleaseLocked();
  return odysseySdMountLocked(reason?reason:"op14",ODYSSEY_SD_RECOVERY_ATTEMPTS);
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
  if (odysseySdUnsafeToSleep.load()) {
    Serial.println("[SD] power transition deferred: SD idle/write failure requires recovery");
    return false;
  }
  const bool wasReady=odysseySdReady();
  const uint8_t state=odysseySdBootState.load();
  odysseySdBootState=0;odysseySdProbeStage=0;

  // Only a successfully mounted session can have an application-owned CMD18
  // or CMD25 transfer to close. If initialization already failed, do not inject
  // another raw recovery sequence on every reset/deep-sleep transition.
  if (!wasReady) {
    odysseySdReleaseLocked();
    Serial.printf("[SD] power transition prepared without quiesce state=%u\n",unsigned(state));
    return true;
  }

  const uint8_t idle=odysseySdQuiesceLocked(ODYSSEY_SD_QUIESCE_BUDGET_MS);
  odysseySdReleaseLocked();
  Serial.printf("[SD] power transition prepared ready=1 quiesced=%u\n",idle==1?1u:0u);
  // The card may remain powered when the C3 sleeps or resets. If it did not
  // reach the SPI idle window, never cut the host out from under a possible
  // unfinished CMD18/CMD25 transaction. Caller must cancel the transition.
  // Latch the failure so the next auto-sleep retry cannot bypass quiescence
  // merely because this attempt reset odysseySdBootState to "not checked".
  if (idle!=1) {
    odysseySdUnsafeToSleep=true;
    return false;
  }
  return true;
}
// A committed OTA has already selected its validated boot partition.
// Unlike deep sleep, a *historical* SD failure must not strand this boot.
// Hold the same SD mutex; never restart across an open recording or a mounted
// card that fails the existing CMD13/quiesce check.
bool odysseyPrepareSdForCommittedOtaRestart(uint32_t timeoutMs) {
  OdysseySdGuard guard(pdMS_TO_TICKS(timeoutMs));
  if (!guard || odysseyRecording.load()) {
    Serial.println("[OTA] restart deferred: live SD transaction");
    return false;
  }
  const bool ready=odysseySdReady();
  const uint8_t previous=odysseySdBootState.load();
  if (!ready) {
    // Boot/media probe already declared VFS/SD unavailable, so there is no
    // application-owned open file. Ignore the persisted sleep veto ONLY here.
    odysseySdReleaseLocked();
    Serial.printf("[OTA] restart permitted with unmounted SD previousState=%u unsafe=%u\n",
      unsigned(previous),odysseySdUnsafeToSleep.load()?1u:0u);
    return true;
  }
  const uint8_t idle=odysseySdQuiesceLocked(ODYSSEY_SD_QUIESCE_BUDGET_MS);
  odysseySdReleaseLocked();
  if (idle!=1) {
    odysseySdUnsafeToSleep=true;
    Serial.println("[OTA] restart deferred: mounted SD failed quiesce");
    return false;
  }
  Serial.println("[OTA] restart permitted: mounted SD quiesced");
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
