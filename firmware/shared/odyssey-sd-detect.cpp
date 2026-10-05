// Odyssey SD storage.
// C3 owns SPI through ESP-IDF SDSPI and FAT/VFS. S3 retains its legacy one-shot probe.
#if !SYNAP_CHAKSHU
#if CONFIG_IDF_TARGET_ESP32S3
#include <SPI.h>
#include <SD.h>
#endif
#if CONFIG_IDF_TARGET_ESP32C3
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
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
// probe 0=not checked, 1=SPI bus setup failed, 2=SDSPI/card/FAT mount failed,
//       3=explicit format failed, 4=VFS validation failed, 6=ready.
static std::atomic<uint8_t> odysseySdBootState{0};
static std::atomic<uint8_t> odysseySdProbeStage{0};
uint8_t odysseySdDetectionState() { return odysseySdBootState.load(); }
uint8_t odysseySdProbeState() { return odysseySdProbeStage.load(); }

#if CONFIG_IDF_TARGET_ESP32C3
static constexpr const char* ODYSSEY_SD_MOUNT_POINT="/odyssey-sd";
static constexpr const char* ODYSSEY_SD_RECORDING_DIR="/odyssey-sd/synap";
static constexpr uint32_t ODYSSEY_SD_STARTUP_SETTLE_MS=3000u;
static constexpr uint32_t ODYSSEY_SD_MAX_FREQ_KHZ=4000u;
static constexpr size_t ODYSSEY_SD_MAX_OPEN_FILES=4;
static constexpr size_t ODYSSEY_SD_MAX_TRANSFER_BYTES=4096;

static StaticSemaphore_t odysseySdMutexStorage;
static SemaphoreHandle_t odysseySdMutex=nullptr;
static sdmmc_card_t* odysseySdCard=nullptr;
static sdmmc_host_t odysseySdHost{};
static std::atomic<bool> odysseySdBusInitialized{false};
static std::atomic<bool> odysseySdHostMounted{false};
static std::atomic<int32_t> odysseySdLastMountError{ESP_OK};
static std::atomic<uint32_t> odysseySdMountAttempts{0};
static std::atomic<uint32_t> odysseySdBeginAttempts{0};
static std::atomic<uint8_t> odysseySdLastMountReason{0}; // 1=boot,2=op14,3=touch,4=probe,6=format
static std::atomic<bool> odysseySdRecoveryRequested{false};

void odysseySdRequestRecovery() { odysseySdRecoveryRequested=true; }
bool odysseySdConsumeRecoveryRequest() { return odysseySdRecoveryRequested.exchange(false); }
int32_t odysseySdLastError() { return odysseySdLastMountError.load(); }
uint32_t odysseySdAttemptCount() { return odysseySdMountAttempts.load(); }
uint32_t odysseySdBeginAttemptCount() { return odysseySdBeginAttempts.load(); }
uint8_t odysseySdLastMountReasonCode() { return odysseySdLastMountReason.load(); }

void odysseySdUseProbingClock() {}
void odysseySdMarkVfsFailure() {
  // Preserve the actual errno separately at the failing call site; never remount implicitly.
  odysseySdBootState=2;
  odysseySdProbeStage=4;
  odysseySdLastMountError=ESP_FAIL;
}

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
    odysseySdLastMountError=err;
    Serial.printf("[SD] contiguous preallocation failed err=%s (0x%lx) size=%llu\n",
      esp_err_to_name(err),static_cast<unsigned long>(err),static_cast<unsigned long long>(size));
    return false;
  }
  return true;
}

static bool odysseySdReleaseLocked() {
  // All callers hold the storage mutex; no descriptor may survive unmount.
  if (!odysseySdCloseReadLocked()) return false;
  if (odysseySdHostMounted.load()) {
    const esp_err_t err=esp_vfs_fat_sdcard_unmount(ODYSSEY_SD_MOUNT_POINT,odysseySdCard);
    if (err!=ESP_OK) {
      odysseySdLastMountError=err;
      Serial.printf("[SD] FatFs unmount failed err=%s (0x%lx); keeping SPI bus owned\n",
        esp_err_to_name(err),static_cast<unsigned long>(err));
      return false;
    }
    odysseySdCard=nullptr;
    odysseySdHostMounted=false;
  }
  if (odysseySdBusInitialized.load()) {
    const esp_err_t err=spi_bus_free(SPI2_HOST);
    if (err!=ESP_OK) {
      odysseySdLastMountError=err;
      Serial.printf("[SD] SPI bus free failed err=%s (0x%lx)\n",
        esp_err_to_name(err),static_cast<unsigned long>(err));
      return false;
    }
    odysseySdBusInitialized=false;
  }
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  return true;
}

static bool odysseySdBeginLocked(bool formatIfMountFailed=false) {
  ++odysseySdBeginAttempts;
  if (!odysseySdReleaseLocked()) return false;
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_CS,OUTPUT);

  spi_bus_config_t bus{};
  bus.mosi_io_num=ODYSSEY_SD_MOSI;
  bus.miso_io_num=ODYSSEY_SD_MISO;
  bus.sclk_io_num=ODYSSEY_SD_SCK;
  bus.quadwp_io_num=-1;
  bus.quadhd_io_num=-1;
  bus.max_transfer_sz=ODYSSEY_SD_MAX_TRANSFER_BYTES;
  esp_err_t err=spi_bus_initialize(SPI2_HOST,&bus,SDSPI_DEFAULT_DMA);
  if (err!=ESP_OK) {
    odysseySdLastMountError=err;
    odysseySdBootState=2;odysseySdProbeStage=1;
    Serial.printf("[SD] SPI bus init failed err=%s (0x%lx)\n",esp_err_to_name(err),static_cast<unsigned long>(err));
    return false;
  }
  odysseySdBusInitialized=true;

  odysseySdHost=SDSPI_HOST_DEFAULT();
  odysseySdHost.slot=SPI2_HOST;
  odysseySdHost.max_freq_khz=ODYSSEY_SD_MAX_FREQ_KHZ;
  sdspi_device_config_t slot=SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.host_id=SPI2_HOST;
  slot.gpio_cs=static_cast<gpio_num_t>(ODYSSEY_SD_CS);
  esp_vfs_fat_mount_config_t config{};
  config.format_if_mount_failed=false;
  config.max_files=ODYSSEY_SD_MAX_OPEN_FILES;
  config.allocation_unit_size=0; // Keep the existing FAT32 volume's cluster geometry.
  config.disk_status_check_enable=true;
  config.use_one_fat=false;
  // The only caller allowed to opt in is the explicit user Format SD action.
  config.format_if_mount_failed=formatIfMountFailed;
  err=esp_vfs_fat_sdspi_mount(ODYSSEY_SD_MOUNT_POINT,&odysseySdHost,&slot,
    &config,&odysseySdCard);
  if (err!=ESP_OK) {
    odysseySdLastMountError=err;
    odysseySdBootState=(err==ESP_ERR_NOT_FOUND)?3:2;
    odysseySdProbeStage=(err==ESP_ERR_NOT_FOUND)?0:2;
    odysseySdCard=nullptr; // The convenience mount helper cleans its card/host on failure.
    Serial.printf("[SD] SDSPI/FatFs mount failed err=%s (0x%lx)\n",esp_err_to_name(err),static_cast<unsigned long>(err));
    (void)odysseySdReleaseLocked();
    return false;
  }
  odysseySdHostMounted=true;
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
  struct stat root{};
  struct stat recordings{};
  if (stat(ODYSSEY_SD_MOUNT_POINT,&root)!=0 || !S_ISDIR(root.st_mode)) goto failed;
  if (stat(ODYSSEY_SD_RECORDING_DIR,&recordings)!=0) {
    if (errno!=ENOENT || mkdir(ODYSSEY_SD_RECORDING_DIR,0755)!=0) goto failed;
  } else if (!S_ISDIR(recordings.st_mode)) { errno=ENOTDIR; goto failed; }

  if (!odysseySdRecoverRecordingPartsLocked()) goto failed;

  {
    DIR* verified=opendir(ODYSSEY_SD_RECORDING_DIR);
    if (!verified) goto failed;
    if (closedir(verified)!=0) goto failed;
  }
  {
    const char* probePath="/odyssey-sd/synap/.synap-media-probe.tmp";
    FILE* probe=fopen(probePath,"wb");
    if (!probe) goto failed;
    bool ok=fwrite("SD",1,2,probe)==2 && fflush(probe)==0 && fsync(fileno(probe))==0;
    const int saved=errno;
    if (fclose(probe)!=0) ok=false;
    char readback[2]{};
    FILE* verify=ok?fopen(probePath,"rb"):nullptr;
    if (!verify || fread(readback,1,sizeof(readback),verify)!=sizeof(readback) ||
        memcmp(readback,"SD",sizeof(readback))!=0) ok=false;
    if (verify && fclose(verify)!=0) ok=false;
    if (unlink(probePath)!=0) ok=false;
    if (!ok) { errno=saved?saved:EIO; goto failed; }
  }
  return true;

failed:
  odysseySdLastMountError=ESP_FAIL;
  odysseySdBootState=2;odysseySdProbeStage=4;
  Serial.printf("[SD] %s attempt %u VFS check failed errno=%d\n",reason,unsigned(attempt),errno);
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
  Serial.printf("[SD] %s attempt %u IDF SDSPI mount max=%u kHz pins CS=%d SCK=%d MOSI=%d MISO=%d\n",
    reason,unsigned(attempt),unsigned(ODYSSEY_SD_MAX_FREQ_KHZ),ODYSSEY_SD_CS,ODYSSEY_SD_SCK,ODYSSEY_SD_MOSI,ODYSSEY_SD_MISO);
  if (!odysseySdBeginLocked()) return false;
  if (!odysseySdValidateVfsLocked(reason,attempt)) {
    (void)odysseySdReleaseLocked();
    return false;
  }
  odysseySdLastMountError=ESP_OK;
  odysseySdBootState=1;odysseySdProbeStage=6;
  Serial.printf("[SD] ready via IDF SDSPI/FatFs, capacity=%llu MiB, max=%u kHz\n",
    static_cast<unsigned long long>(odysseySdCard->csd.capacity) * odysseySdCard->csd.sector_size / (1024ULL*1024ULL),
    unsigned(ODYSSEY_SD_MAX_FREQ_KHZ));
  return true;
}

static bool odysseySdMountLocked(const char* reason,uint8_t attempts) {
  if (odysseySdReady()) return true;
  for (uint8_t attempt=1;attempt<=attempts;++attempt)
    if (odysseySdMountOnceLocked(reason,attempt)) return true;
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
  if (!guard || !odysseySdReleaseLocked()) return false;
  odysseySdBootState=0;odysseySdProbeStage=0;
  return odysseySdMountLocked(reason?reason:"op14",1);
}

bool odysseyFormatSdCard() {
  OdysseySdGuard guard(pdMS_TO_TICKS(15000));
  if (!guard || !odysseySdCloseReadLocked()) return false;
  if (!odysseySdHostMounted.load() && !odysseySdBeginLocked(true)) {
    odysseySdBootState=2;odysseySdProbeStage=3;
    Serial.println("[SD] explicit format could not mount card/FAT");
    return false;
  }
  odysseySdLastMountReason=odysseySdMountReasonCode("format");
  const esp_err_t err=esp_vfs_fat_sdcard_format(ODYSSEY_SD_MOUNT_POINT,odysseySdCard);
  if (err!=ESP_OK) {
    odysseySdLastMountError=err;odysseySdBootState=2;odysseySdProbeStage=3;
    Serial.printf("[SD] explicit FAT format failed err=%s (0x%lx)\n",esp_err_to_name(err),static_cast<unsigned long>(err));
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
  if (!odysseySdReleaseLocked()) return false;
  const bool wasReady=odysseySdReady();
  odysseySdBootState=0;odysseySdProbeStage=0;
  Serial.printf("[SD] power transition prepared; unmounted ready=%u\n",wasReady?1u:0u);
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
