// Odyssey SD storage.
// C3 mounts through the stock Arduino-ESP32 SD SPI path and uses FAT/VFS at runtime; S3 keeps its legacy one-shot detection.
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

// Restore the exact Arduino-ESP32 3.3.5 stock SD initialization used by the
// known-good Odyssey C3 build 1445. Runtime recording/sync continues to use
// the current guarded POSIX/VFS implementation after the mount succeeds.
static SPIClass odysseySdSpi(FSPI);
static std::atomic<int32_t> odysseySdLastMountError{ESP_OK};
static std::atomic<uint32_t> odysseySdMountAttempts{0};
static std::atomic<uint32_t> odysseySdBeginAttempts{0};
static std::atomic<uint8_t> odysseySdLastMountReason{0}; // 1=boot,2=op14,3=touch,4=probe
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
int16_t odysseySdBitBangCsHighState() { return odysseySdBitBangCsHigh.load(); }
int16_t odysseySdBitBangCsLowState() { return odysseySdBitBangCsLow.load(); }
uint8_t odysseySdBitBangStopStateValue() { return odysseySdBitBangStopState.load(); }
int16_t odysseySdBitBangCmd12Response() { return odysseySdBitBangCmd12.load(); }
uint8_t odysseySdBitBangCmd12ReadyState() { return odysseySdBitBangCmd12Ready.load(); }
uint32_t odysseySdBitBangDrainByteCount() { return odysseySdBitBangDrainBytes.load(); }
uint16_t odysseySdRawZeroCount() { return odysseySdRawZero.load(); }
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
  odysseySdBootState=2;odysseySdProbeStage=4;
  odysseySdLastMountError=ESP_FAIL;
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

static void odysseySdReleaseLocked() {
  SD.end();
  odysseySdSpi.end();
  // Exact 1445 teardown: only deassert CS. Do not preconfigure SCK/MOSI/MISO
  // before the next SPIClass::begin(), because this diagnostic build is
  // intentionally testing the old known-good ownership sequence.
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_CS,OUTPUT);
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
  odysseySdReleaseLocked();
  digitalWrite(ODYSSEY_SD_CS,HIGH);pinMode(ODYSSEY_SD_CS,OUTPUT);
  pinMode(ODYSSEY_SD_SCK,OUTPUT);digitalWrite(ODYSSEY_SD_SCK,LOW);
  pinMode(ODYSSEY_SD_MOSI,OUTPUT);digitalWrite(ODYSSEY_SD_MOSI,HIGH);
  pinMode(ODYSSEY_SD_MISO,INPUT_PULLUP);
  delayMicroseconds(50);

  const uint32_t started=millis();
  const int csHigh=digitalRead(ODYSSEY_SD_MISO)==HIGH?1:0;
  for (uint8_t i=0;i<16;++i) (void)odysseySdBitBangTransfer(0xFF);

  uint8_t candidate=0xFF;
  uint32_t drained=0;
  uint8_t state=odysseySdBitBangStopReadLocked(candidate,drained,budgetMs);

  // A card left inside CMD25 ignores CMD12 and waits for a data token instead.
  // Only reachable if budget remains, and with timeouts scaled to it.
  uint8_t writeStop=0;
  const uint32_t spent=uint32_t(millis()-started);
  if (state!=1 && spent<budgetMs) {
    const uint32_t remaining=budgetMs-spent;
    writeStop=odysseySdBitBangStopWriteLocked(remaining/2u+1u,remaining/2u+1u);
    if (writeStop==1) state=1;
  }

  // Park the bus the way a deselected card expects to find it.
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  for (uint8_t i=0;i<10;++i) (void)odysseySdBitBangTransfer(0xFF);
  digitalWrite(ODYSSEY_SD_SCK,LOW);
  digitalWrite(ODYSSEY_SD_MOSI,HIGH);

  Serial.printf("[SD] quiesce idle=%u csHigh=%d drain=%lu writeStop=%u in %lums\n",
    unsigned(state),csHigh,static_cast<unsigned long>(drained),unsigned(writeStop),
    static_cast<unsigned long>(millis()-started));
  return state;
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
  odysseySdReleaseLocked();

  // digitalWrite BEFORE pinMode, which is the order build 1445 used. The
  // reverse drives the pin from the output register's reset value - low - for
  // the microseconds until the digitalWrite lands, so every mount attempt
  // opened with a CS glitch the known-good lifecycle never produced.
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  Serial.printf("[SD] %s attempt %u Arduino SPI init at %lu Hz, pins CS=%d SCK=%d MOSI=%d MISO=%d\n",
    reason,unsigned(attempt),static_cast<unsigned long>(ODYSSEY_SD_INIT_FREQ_HZ),
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
      odysseySdLastMountError=ESP_FAIL;
      odysseySdBootState=2;odysseySdProbeStage=2;
      Serial.printf("[SD] %s attempt %u exact-1445 SD.begin failed; GPIO CMD0=0x%02X\n",
        reason,unsigned(attempt),unsigned(bitBangCmd0));
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
    // A card that has just been dragged out of a stranded transfer needs a
    // moment before it will answer CMD0 cleanly. Retrying instantly mostly
    // reproduces the same failure.
    if (attempt<attempts) delay(ODYSSEY_SD_ATTEMPT_SETTLE_MS);
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
  // Regression diagnostic: reproduce build 1445 timing and first transaction.
  // No startup delay and no raw command is issued before the first SD.begin().
  OdysseySdGuard guard;
  if (!guard) { odysseySdBootState=2;odysseySdProbeStage=1; return false; }
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
