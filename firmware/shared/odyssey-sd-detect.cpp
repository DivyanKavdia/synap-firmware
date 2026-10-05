// Odyssey SD storage.
// C3 uses the proven Arduino-ESP32 SPI/SD mount path at 400 kHz and FAT/VFS at runtime.
// S3 retains its legacy one-shot probe. Recording, recovery and sync stay POSIX/VFS based.
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
#include "esp_vfs_fat.h"
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
// probe 0=not checked, 1=SPI bus setup failed, 2=card/FAT mount failed,
//       3=explicit format failed, 4=VFS validation failed, 6=ready.
static std::atomic<uint8_t> odysseySdBootState{0};
static std::atomic<uint8_t> odysseySdProbeStage{0};
uint8_t odysseySdDetectionState() { return odysseySdBootState.load(); }
uint8_t odysseySdProbeState() { return odysseySdProbeStage.load(); }

#if CONFIG_IDF_TARGET_ESP32C3
static constexpr const char* ODYSSEY_SD_MOUNT_POINT="/odyssey-sd";
static constexpr const char* ODYSSEY_SD_RECORDING_DIR="/odyssey-sd/synap";
static constexpr uint32_t ODYSSEY_SD_STARTUP_SETTLE_MS=3000u;
// Builds 1631/1445 proved this exact C3/card/module combination on Arduino SD at 400 kHz.
// New offline recording uses one WAV descriptor with an inline recovery tail.
// Keep descriptor headroom for BLE reads, metadata, legacy recovery and maintenance.
static constexpr uint32_t ODYSSEY_SD_DATA_FREQ_HZ=400000u;
static constexpr size_t ODYSSEY_SD_MAX_OPEN_FILES=4;
static_assert(ODYSSEY_SD_MAX_OPEN_FILES>=2,"C3 SD maintenance requires descriptor headroom");

static StaticSemaphore_t odysseySdMutexStorage;
static SemaphoreHandle_t odysseySdMutex=nullptr;
static SPIClass odysseySdSpi(FSPI);
static std::atomic<bool> odysseySdHostMounted{false};
static std::atomic<int32_t> odysseySdLastMountError{ESP_OK};
static std::atomic<uint32_t> odysseySdMountAttempts{0};
static std::atomic<uint32_t> odysseySdBeginAttempts{0};
static std::atomic<uint32_t> odysseySdReleaseAttempts{0};
static std::atomic<uint8_t> odysseySdLastMountReason{0}; // 1=boot,2=op14,3=touch,4=probe,6=format
static std::atomic<int32_t> odysseySdLastIoErrno{0};
static std::atomic<int32_t> odysseySdLastReleaseError{ESP_OK};
static std::atomic<uint64_t> odysseySdLastFreeBytes{0};
static std::atomic<bool> odysseySdRecoveryRequested{false};

void odysseySdRequestRecovery() { odysseySdRecoveryRequested=true; }
bool odysseySdConsumeRecoveryRequest() { return odysseySdRecoveryRequested.exchange(false); }
int32_t odysseySdLastError() { return odysseySdLastMountError.load(); }
int32_t odysseySdLastIoError() { return odysseySdLastIoErrno.load(); }
int32_t odysseySdLastReleaseErrorCode() { return odysseySdLastReleaseError.load(); }
uint32_t odysseySdAttemptCount() { return odysseySdMountAttempts.load(); }
uint32_t odysseySdBeginAttemptCount() { return odysseySdBeginAttempts.load(); }
uint32_t odysseySdReleaseAttemptCount() { return odysseySdReleaseAttempts.load(); }
uint8_t odysseySdLastMountReasonCode() { return odysseySdLastMountReason.load(); }
uint64_t odysseySdLastFreeByteCount() { return odysseySdLastFreeBytes.load(); }

uint64_t odysseySdFreeBytesLocked() {
  if (!odysseySdHostMounted.load()) { odysseySdLastFreeBytes=0;return 0; }
  const uint64_t total=SD.totalBytes(),used=SD.usedBytes();
  const uint64_t freeBytes=total>=used?total-used:0;
  odysseySdLastFreeBytes=freeBytes;
  return freeBytes;
}

void odysseySdUseProbingClock() {}
void odysseySdMarkVfsFailure(int error) {
  // A VFS failure means the current mount is no longer trusted. Keep the host
  // owned until explicit recovery/power-down so teardown happens under the SD mutex.
  odysseySdLastIoErrno=error?error:EIO;
  odysseySdBootState=2;
  odysseySdProbeStage=4;
  odysseySdLastMountError=ESP_FAIL;
}
void odysseySdMarkVfsFailure() { odysseySdMarkVfsFailure(errno); }

static void odysseySdEnsureMutex() {
  if (!odysseySdMutex) odysseySdMutex=xSemaphoreCreateMutexStatic(&odysseySdMutexStorage);
}
bool odysseySdTake(TickType_t timeout=portMAX_DELAY) {
  odysseySdEnsureMutex();
  return odysseySdMutex && xSemaphoreTake(odysseySdMutex,timeout)==pdTRUE;
}
void odysseySdGive() { if (odysseySdMutex) xSemaphoreGive(odysseySdMutex); }
class OdysseySdGuard {
  bool held_;
public:
  explicit OdysseySdGuard(TickType_t timeout=portMAX_DELAY):held_(odysseySdTake(timeout)){}
  ~OdysseySdGuard(){ if(held_) odysseySdGive(); }
  explicit operator bool() const { return held_; }
};

bool odysseySdReady() { return odysseySdBootState.load()==1 && odysseySdProbeStage.load()==6; }
const char* odysseySdMountPoint() { return ODYSSEY_SD_MOUNT_POINT; }
bool odysseySdPath(const char* logical,char* full,size_t capacity) {
  if (!logical || logical[0]!='/' || !full || capacity<2 || strstr(logical,"..")) return false;
  const int n=snprintf(full,capacity,"%s%s",ODYSSEY_SD_MOUNT_POINT,logical);
  return n>0 && size_t(n)<capacity;
}
// Last completed failure survives a manual power cycle; write once per failed take.
static uint32_t odysseyStoredStage=0,odysseyStoredBytes=0,odysseyStoredBuild=0;
static int32_t odysseyStoredErrno=0;
static void odysseyLoadRecordFailure() {
  Preferences prefs;
  if (!prefs.begin("sd-failure",true)) return;
  uint32_t record[5]{};
  if (prefs.getBytesLength("record")==sizeof(record) &&
      prefs.getBytes("record",record,sizeof(record))==sizeof(record) && record[0]==1) {
    odysseyStoredStage=record[1];odysseyStoredErrno=int32_t(record[2]);
    odysseyStoredBytes=record[3];odysseyStoredBuild=record[4];
  }
  prefs.end();
}
void odysseySaveRecordFailure(uint8_t stage,int error,uint32_t bytes) {
  odysseyStoredStage=stage;odysseyStoredErrno=error;
  odysseyStoredBytes=bytes;odysseyStoredBuild=stage?SYNAP_BUILD:0;
  Preferences prefs;
  if (!prefs.begin("sd-failure",false)) return;
  const uint32_t record[]={stage?1u:0u,stage,uint32_t(error),bytes,stage?uint32_t(SYNAP_BUILD):0u};
  if (prefs.putBytes("record",record,sizeof(record))!=sizeof(record))
    Serial.println("[SD] could not persist recording failure");
  prefs.end();
}
bool odysseySdPreallocateFile(const char* fullPath,uint64_t size) {
  const size_t mountLength=strlen(ODYSSEY_SD_MOUNT_POINT);
  if (!fullPath || strncmp(fullPath,ODYSSEY_SD_MOUNT_POINT,mountLength)!=0 || fullPath[mountLength]!='/')
    return false;
  // Reserve the name exclusively before the IDF helper opens it with FA_OPEN_ALWAYS.
  const int reserved=open(fullPath,O_CREAT|O_EXCL|O_RDWR,0644);
  if (reserved<0) return false;
  if (close(reserved)!=0) return false;
  const esp_err_t err=esp_vfs_fat_create_contiguous_file(
    ODYSSEY_SD_MOUNT_POINT,fullPath,size,true);
  if (err!=ESP_OK) {
    const int saved=errno?errno:EIO;
    // No contiguous extent is not an I/O fault. Keep the exclusively reserved
    // empty file and allocate clusters sequentially, but never retry EIO.
    if (saved==ENOSPC || saved==EACCES) {
      struct stat st{};
      if (stat(fullPath,&st)==0 && st.st_size==0) { errno=0;return true; }
    }
    errno=saved;
    odysseySdLastMountError=err;
    Serial.printf("[SD] contiguous preallocation failed err=%s (0x%lx) size=%llu\n",
      esp_err_to_name(err),static_cast<unsigned long>(err),static_cast<unsigned long long>(size));
    return false;
  }
  errno=0;
  return true;
}

static bool odysseySdReleaseLocked() {
  // All callers hold the storage mutex. FatFs close() always releases its VFS
  // descriptor slot even when f_close reports an I/O error, so teardown must
  // continue instead of leaving a poisoned mount registered forever.
  ++odysseySdReleaseAttempts;
  bool teardownOk=true;
  errno=0;
  if (!odysseySdCloseReadLocked()) {
    const int closeError=errno?errno:EIO;
    odysseySdLastIoErrno=closeError;
    Serial.printf("[SD] cached read close failed errno=%d; continuing teardown\n",closeError);
  }

  const bool hadHost=odysseySdHostMounted.load();
  SD.end(); // Arduino sends GO_IDLE, unmounts FatFs and unregisters its VFS path.

  // SDFS::end() discards sdcard_uninit()'s return code. Verify there is no
  // residual VFS registration; if Arduino left one behind, remove it here
  // before another SD.begin() can encounter ESP_ERR_INVALID_STATE.
  const esp_err_t residual=esp_vfs_fat_unregister_path(ODYSSEY_SD_MOUNT_POINT);
  if (residual==ESP_OK) {
    odysseySdLastReleaseError=ESP_OK;
    Serial.println("[SD] reclaimed residual FAT VFS registration after SD.end");
  } else if (residual!=ESP_ERR_INVALID_STATE) {
    teardownOk=false;
    odysseySdLastReleaseError=residual;
    Serial.printf("[SD] FAT VFS release verification failed err=%s (0x%lx)\n",
      esp_err_to_name(residual),static_cast<unsigned long>(residual));
  } else {
    odysseySdLastReleaseError=ESP_OK;
  }

  odysseySdSpi.end();
  // Preserve the proven bus-idle ownership: deselect the card after SPI detaches.
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  odysseySdHostMounted=false;
  if (hadHost) delay(2);
  return teardownOk;
}

static bool odysseySdBeginLocked(bool formatIfMountFailed=false) {
  ++odysseySdBeginAttempts;
  if (!odysseySdReleaseLocked()) {
    odysseySdLastMountError=odysseySdLastReleaseError.load();
    odysseySdBootState=2;odysseySdProbeStage=1;
    return false;
  }
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  if (!odysseySdSpi.begin(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS)) {
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=1;
    odysseySdSpi.end();
    digitalWrite(ODYSSEY_SD_CS,HIGH);pinMode(ODYSSEY_SD_CS,OUTPUT);
    Serial.println("[SD] custom-pin SPI bus start failed");
    return false;
  }
  const bool mounted=SD.begin(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_DATA_FREQ_HZ,
    ODYSSEY_SD_MOUNT_POINT,ODYSSEY_SD_MAX_OPEN_FILES,formatIfMountFailed);
  if (!mounted) {
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=formatIfMountFailed?3:2;
    // SD.begin() normally cleans its own failed mount, but use the same
    // verified teardown here so a residual VFS registration cannot poison the
    // next explicit recovery attempt.
    (void)odysseySdReleaseLocked();
    return false;
  }
  odysseySdHostMounted=true;
  odysseySdLastIoErrno=0;
  odysseySdLastReleaseError=ESP_OK;
  markOdysseySdBatteryDividerPresent();
  return true;
}

// Recover reserved recording parts before making them visible to catalogue reads.
static bool odysseySdRecoverRecordingPartsLocked() {
  DIR* dir=opendir(ODYSSEY_SD_RECORDING_DIR);
  if (!dir) return false;
  bool ok=true;
  for (;;) {
    errno=0;dirent* entry=readdir(dir);
    if (!entry) { if (errno) ok=false;break; }
    if (strncmp(entry->d_name,"odyssey_audio_",14)!=0) continue;
    const size_t length=strlen(entry->d_name);
    if (length<8 || strcmp(entry->d_name+length-4,".wav")!=0 || !strstr(entry->d_name,"_p")) continue;
    char path[128];
    const int n=snprintf(path,sizeof(path),"%s/%s",ODYSSEY_SD_RECORDING_DIR,entry->d_name);
    if (n<=0 || size_t(n)>=sizeof(path)) { ok=false;errno=ENAMETOOLONG;break; }
    if (!odysseyRecoverWav(path)) { ok=false;break; }
  }
  if (closedir(dir)!=0) ok=false;
  return ok;
}

static bool odysseySdValidateVfsLocked(const char* reason,uint8_t attempt) {
  int failureErrno=0;
  struct stat root{};
  struct stat recordings{};
  errno=0;
  if (stat(ODYSSEY_SD_MOUNT_POINT,&root)!=0 || !S_ISDIR(root.st_mode)) {
    failureErrno=errno?errno:ENODEV;goto failed;
  }
  errno=0;
  if (stat(ODYSSEY_SD_RECORDING_DIR,&recordings)!=0) {
    const int statError=errno;
    if (statError!=ENOENT) { failureErrno=statError?statError:EIO;goto failed; }
    errno=0;
    if (mkdir(ODYSSEY_SD_RECORDING_DIR,0755)!=0) { failureErrno=errno?errno:EIO;goto failed; }
  } else if (!S_ISDIR(recordings.st_mode)) {
    failureErrno=ENOTDIR;goto failed;
  }

  errno=0;
  if (!odysseySdRecoverRecordingPartsLocked()) {
    failureErrno=errno?errno:EIO;goto failed;
  }

  {
    errno=0;
    DIR* verified=opendir(ODYSSEY_SD_RECORDING_DIR);
    if (!verified) { failureErrno=errno?errno:EIO;goto failed; }
    if (closedir(verified)!=0) { failureErrno=errno?errno:EIO;goto failed; }
  }
  {
    const char* probePath="/odyssey-sd/synap/.synap-media-probe.tmp";
    errno=0;
    FILE* probe=fopen(probePath,"wb");
    if (!probe) { failureErrno=errno?errno:EIO;goto failed; }
    bool ok=true;
    if (fwrite("SD",1,2,probe)!=2 || fflush(probe)!=0 || fsync(fileno(probe))!=0) {
      failureErrno=errno?errno:EIO;ok=false;
    }
    if (fclose(probe)!=0) {
      if (!failureErrno) failureErrno=errno?errno:EIO;
      ok=false;
    }
    char readback[2]{};
    FILE* verify=nullptr;
    if (ok) {
      errno=0;
      verify=fopen(probePath,"rb");
      if (!verify) { failureErrno=errno?errno:EIO;ok=false; }
    }
    if (ok && fread(readback,1,sizeof(readback),verify)!=sizeof(readback)) {
      failureErrno=(ferror(verify) && errno)?errno:EIO;ok=false;
    }
    if (ok && memcmp(readback,"SD",sizeof(readback))!=0) {
      failureErrno=EIO;ok=false;
    }
    if (verify && fclose(verify)!=0) {
      if (!failureErrno) failureErrno=errno?errno:EIO;
      ok=false;
    }
    errno=0;
    if (unlink(probePath)!=0 && errno!=ENOENT) {
      if (!failureErrno) failureErrno=errno?errno:EIO;
      ok=false;
    }
    if (!ok) goto failed;
  }
  odysseySdLastIoErrno=0;
  return true;

failed:
  if (!failureErrno) failureErrno=errno?errno:EIO;
  errno=failureErrno;
  odysseySdLastIoErrno=failureErrno;
  odysseySdLastMountError=ESP_FAIL;
  odysseySdBootState=2;odysseySdProbeStage=4;
  Serial.printf("[SD] %s attempt %u VFS check failed errno=%d\n",
    reason,unsigned(attempt),failureErrno);
  return false;
}

static uint8_t odysseySdMountReasonCode(const char* reason) {
  if (!strcmp(reason,"boot")) return 1;
  if (!strcmp(reason,"op14")) return 2;
  if (!strcmp(reason,"touch")) return 3;
  if (!strcmp(reason,"format")) return 6;
  return 4;
}

static bool odysseySdMountOnceLocked(const char* reason,uint8_t attempt) {
  odysseySdBootState=0;odysseySdProbeStage=0;
  odysseySdLastMountReason=odysseySdMountReasonCode(reason);
  ++odysseySdMountAttempts;
  Serial.printf("[SD] %s attempt %u proven Arduino SPI mount at %lu Hz pins CS=%d SCK=%d MOSI=%d MISO=%d\n",
    reason,unsigned(attempt),static_cast<unsigned long>(ODYSSEY_SD_DATA_FREQ_HZ),
    ODYSSEY_SD_CS,ODYSSEY_SD_SCK,ODYSSEY_SD_MOSI,ODYSSEY_SD_MISO);
  if (!odysseySdBeginLocked()) return false;
  const uint8_t type=SD.cardType();
  const uint64_t cardBytes=SD.cardSize();
  const size_t sectorBytes=SD.sectorSize();
  if (type==CARD_NONE) {
    odysseySdLastMountError=ESP_ERR_NOT_FOUND;
    odysseySdBootState=3;odysseySdProbeStage=0;
    (void)odysseySdReleaseLocked();
    return false;
  }
  if (!cardBytes || sectorBytes!=512u) {
    odysseySdLastMountError=ESP_FAIL;
    odysseySdLastIoErrno=ENODEV;
    odysseySdBootState=2;odysseySdProbeStage=2;
    Serial.printf("[SD] %s attempt %u card geometry invalid bytes=%llu sector=%u\n",
      reason,unsigned(attempt),static_cast<unsigned long long>(cardBytes),unsigned(sectorBytes));
    (void)odysseySdReleaseLocked();
    return false;
  }
  if (!odysseySdValidateVfsLocked(reason,attempt)) {
    (void)odysseySdReleaseLocked();
    return false;
  }
  odysseySdLastMountError=ESP_OK;
  odysseySdBootState=1;odysseySdProbeStage=6;
  const char* label=type==CARD_MMC?"MMC":type==CARD_SD?"SDSC":type==CARD_SDHC?"SDHC/SDXC":"unknown";
  odysseySdRecoveryRequested=false;
  const uint64_t freeBytes=odysseySdFreeBytesLocked();
  Serial.printf("[SD] ready via proven Arduino SPI path: %s, %llu MiB, free=%llu MiB, %lu Hz\n",
    label,static_cast<unsigned long long>(cardBytes/(1024ULL*1024ULL)),
    static_cast<unsigned long long>(freeBytes/(1024ULL*1024ULL)),
    static_cast<unsigned long>(ODYSSEY_SD_DATA_FREQ_HZ));
  return true;
}

static bool odysseySdMountLocked(const char* reason,uint8_t attempts) {
  if (odysseySdReady()) return true;
  for (uint8_t attempt=1;attempt<=attempts;++attempt) {
    if (odysseySdMountOnceLocked(reason,attempt)) return true;
    if (attempt<attempts) delay(250u);
  }
  Serial.printf("[SD] %s failed after %u attempt(s), state=%u stage=%u err=%ld\n",
    reason,unsigned(attempts),unsigned(odysseySdBootState.load()),unsigned(odysseySdProbeStage.load()),
    static_cast<long>(odysseySdLastMountError.load()));
  return false;
}

void odysseyDetectSdCard() {
  OdysseySdGuard guard;
  if (!guard) { odysseySdBootState=2;odysseySdProbeStage=1; return; }
  odysseySdMountLocked("probe",1);
}
bool odysseyInitializeSdCardBeforeBle() {
  odysseyLoadRecordFailure();
  const uint32_t now=millis();
  if (now<ODYSSEY_SD_STARTUP_SETTLE_MS) {
    const uint32_t waitMs=ODYSSEY_SD_STARTUP_SETTLE_MS-now;
    Serial.printf("[SD] startup settle %lu ms before first transaction\n",static_cast<unsigned long>(waitMs));
    delay(waitMs);
  }
  OdysseySdGuard guard;
  if (!guard) { odysseySdBootState=2;odysseySdProbeStage=1; return false; }
  const bool ready=odysseySdMountLocked("boot",1);
  Serial.printf("[SD] boot initialization complete state=%u stage=%u before BLE\n",
    unsigned(odysseySdBootState.load()),unsigned(odysseySdProbeStage.load()));
  return ready;
}
bool odysseyRecoverSdCard(const char* reason) {
  OdysseySdGuard guard(pdMS_TO_TICKS(5000));
  if (!guard) return false;
  // Physical offline recovery gets two bounded attempts. Connected op14 stays
  // single-shot so the PWA remains responsive and can report the exact failure.
  const char* why=reason?reason:"op14";
  const uint8_t attempts=(!strcmp(why,"touch") || !strcmp(why,"post-record"))?2u:1u;
  odysseySdBootState=0;odysseySdProbeStage=0;
  return odysseySdMountLocked(why,attempts);
}

bool odysseyFormatSdCard() {
  OdysseySdGuard guard(pdMS_TO_TICKS(15000));
  if (!guard) return false;
  // A failed close still releases the FatFs VFS descriptor slot. Record the
  // error but continue with the user's explicit destructive recovery request.
  errno=0;
  if (!odysseySdCloseReadLocked()) odysseySdLastIoErrno=errno?errno:EIO;

  // Formatting remains explicit. If a volume is already mounted, invalidate
  // sector zero first so Arduino FatFs actually creates a fresh filesystem.
  if (odysseySdHostMounted.load() && SD.cardType()!=CARD_NONE) {
    uint8_t blankSector[512]{};
    if (!SD.writeRAW(blankSector,0)) {
      odysseySdBootState=2;odysseySdProbeStage=3;odysseySdLastMountError=ESP_FAIL;
      Serial.println("[SD] explicit format refused: could not invalidate sector 0");
      return false;
    }
  }
  odysseySdLastMountReason=odysseySdMountReasonCode("format");
  odysseySdBootState=0;odysseySdProbeStage=0;
  if (!odysseySdBeginLocked(true)) {
    odysseySdBootState=2;odysseySdProbeStage=3;odysseySdLastMountError=ESP_FAIL;
    Serial.println("[SD] explicit format could not create/mount FAT");
    return false;
  }
  if (SD.cardType()==CARD_NONE) {
    odysseySdBootState=3;odysseySdProbeStage=0;odysseySdLastMountError=ESP_ERR_NOT_FOUND;
    (void)odysseySdReleaseLocked();
    return false;
  }
  (void)mkdir(ODYSSEY_SD_RECORDING_DIR,0755);
  if (!odysseySdValidateVfsLocked("format",1)) {
    (void)odysseySdReleaseLocked();
    return false;
  }
  odysseySdLastMountError=ESP_OK;odysseySdBootState=1;odysseySdProbeStage=6;
  Serial.println("[SD] explicit format complete; FAT/VFS ready");
  return true;
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
  if (!odysseySdReleaseLocked()) return false;
  odysseySdLastFreeBytes=0;
  odysseySdBootState=0;odysseySdProbeStage=0;
  Serial.printf("[SD] power transition prepared; Arduino SD host released ready=%u\n",wasReady?1u:0u);
  return true;
}
#else
// Odyssey S3 remains detection-only and retains its existing Arduino SD probe.
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
