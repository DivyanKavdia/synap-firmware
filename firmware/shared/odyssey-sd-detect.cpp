// Odyssey SD storage.
// C3 owns SD through ESP-IDF SDSPI + FAT/VFS; S3 keeps its legacy one-shot detection.
// Hardware pins remain device-profile controlled and are never remapped here.
#if !SYNAP_CHAKSHU
#if CONFIG_IDF_TARGET_ESP32C3
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
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
// Always complete protocol discovery + FAT mount at the SD probing clock.
 // Only after the card is fully in SPI mode and VFS is live do we promote
 // the bus to a conservative runtime frequency.
static constexpr uint32_t ODYSSEY_SD_INIT_FREQ_KHZ=SDMMC_FREQ_PROBING;
// 1 MHz gives the C3 SD carrier headroom over 16 kHz PCM while avoiding
// marginal 4 MHz SPI wiring. Fall back to the 400 kHz probe clock after I/O faults.
static constexpr uint32_t ODYSSEY_SD_RUN_FREQ_KHZ=1000u;
static constexpr uint8_t ODYSSEY_SD_BOOT_ATTEMPTS=2;
static constexpr uint8_t ODYSSEY_SD_RECOVERY_ATTEMPTS=3;
static constexpr uint32_t ODYSSEY_SD_RETRY_BACKOFF_MS=250u;
static constexpr size_t ODYSSEY_SD_MAX_OPEN_FILES=8;
static std::atomic<bool> odysseySdProbingClockOnly{false};
void odysseySdUseProbingClock() { odysseySdProbingClockOnly=true; }
void odysseySdMarkVfsFailure() {
  odysseySdBootState=2; odysseySdProbeStage=4;
  odysseySdRequestRecovery();
}

static sdmmc_card_t* odysseySdCard=nullptr;
static bool odysseySdBusInitialized=false;
static StaticSemaphore_t odysseySdMutexStorage;
static SemaphoreHandle_t odysseySdMutex=nullptr;

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
  return odysseySdBootState.load()==1 && odysseySdProbeStage.load()==6 && odysseySdCard!=nullptr;
}
const char* odysseySdMountPoint() { return ODYSSEY_SD_MOUNT_POINT; }
bool odysseySdPath(const char* logical,char* full,size_t capacity) {
  if (!logical || logical[0]!='/' || !full || capacity<2) return false;
  if (strstr(logical,"..")) return false;
  const int n=snprintf(full,capacity,"%s%s",ODYSSEY_SD_MOUNT_POINT,logical);
  return n>0 && size_t(n)<capacity;
}

static uint32_t odysseySdPromoteClockLocked() {
  sdmmc_card_t* card=odysseySdCard;
  if (!card || !card->host.set_card_clk || odysseySdProbingClockOnly.load()) {
    if (odysseySdProbingClockOnly.load()) Serial.println("[SD] retaining 400 kHz probe clock after media I/O fault");
    return ODYSSEY_SD_INIT_FREQ_KHZ;
  }
  const int result=int(card->host.set_card_clk(card->host.slot,ODYSSEY_SD_RUN_FREQ_KHZ));
  if (result!=ESP_OK) {
    Serial.printf("[SD] runtime clock promotion failed: %s (%d); retaining %u kHz\n",
      esp_err_to_name(result),result,unsigned(ODYSSEY_SD_INIT_FREQ_KHZ));
    return ODYSSEY_SD_INIT_FREQ_KHZ;
  }
  const int status=int(sdmmc_get_status(card));
  if (status!=ESP_OK) {
    const int fallback=int(card->host.set_card_clk(card->host.slot,ODYSSEY_SD_INIT_FREQ_KHZ));
    Serial.printf("[SD] %u kHz validation failed: %s (%d); fallback=%s (%d)\n",
      unsigned(ODYSSEY_SD_RUN_FREQ_KHZ),esp_err_to_name(status),status,
      esp_err_to_name(fallback),fallback);
    return ODYSSEY_SD_INIT_FREQ_KHZ;
  }
  int realFreq=0;
  if (card->host.get_real_freq) card->host.get_real_freq(card->host.slot,&realFreq);
  Serial.printf("[SD] runtime clock promoted to %d kHz after successful mount\n",
    realFreq>0?realFreq:int(ODYSSEY_SD_RUN_FREQ_KHZ));
  return realFreq>0?uint32_t(realFreq):ODYSSEY_SD_RUN_FREQ_KHZ;
}

static void odysseySdReleaseLocked() {
  if (odysseySdCard) {
    const int result=int(esp_vfs_fat_sdcard_unmount(ODYSSEY_SD_MOUNT_POINT,odysseySdCard));
    if (result!=ESP_OK) Serial.printf("[SD] unmount result=%s (%d)\n",esp_err_to_name(result),result);
    odysseySdCard=nullptr;
  }
  if (odysseySdBusInitialized) {
    const int result=int(spi_bus_free(SPI2_HOST));
    if (result!=ESP_OK) Serial.printf("[SD] bus free result=%s (%d)\n",esp_err_to_name(result),result);
    odysseySdBusInitialized=false;
  }
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  digitalWrite(ODYSSEY_SD_CS,HIGH);
}

static int odysseySdMountOnceLocked(const char* reason,uint8_t attempt) {
  // Publish INITIALIZING before teardown so capability reads never observe a
  // stale ready state while VFS/card/bus ownership is being recycled.
  odysseySdBootState=0;
  odysseySdProbeStage=0;
  odysseySdReleaseLocked();

  // Return all SD pins to a known GPIO state before every fresh bus attach.
  // This matters after failed SDSPI attempts because GPIO-matrix ownership is
  // torn down independently from the card's internal SPI-mode state.
  gpio_reset_pin(static_cast<gpio_num_t>(ODYSSEY_SD_SCK));
  gpio_reset_pin(static_cast<gpio_num_t>(ODYSSEY_SD_MOSI));
  gpio_reset_pin(static_cast<gpio_num_t>(ODYSSEY_SD_MISO));
  gpio_reset_pin(static_cast<gpio_num_t>(ODYSSEY_SD_CS));
  gpio_set_pull_mode(static_cast<gpio_num_t>(ODYSSEY_SD_MISO),GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(static_cast<gpio_num_t>(ODYSSEY_SD_MOSI),GPIO_PULLUP_ONLY);
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  delay(20);

  sdmmc_host_t host=SDSPI_HOST_DEFAULT();
  host.max_freq_khz=ODYSSEY_SD_INIT_FREQ_KHZ;

  spi_bus_config_t bus{};
  bus.mosi_io_num=ODYSSEY_SD_MOSI;
  bus.miso_io_num=ODYSSEY_SD_MISO;
  bus.sclk_io_num=ODYSSEY_SD_SCK;
  bus.quadwp_io_num=-1;
  bus.quadhd_io_num=-1;
  bus.max_transfer_sz=4096;

  int result=int(spi_bus_initialize(static_cast<spi_host_device_t>(host.slot),&bus,SDSPI_DEFAULT_DMA));
  if (result!=ESP_OK) {
    odysseySdBootState=2;odysseySdProbeStage=1;
    Serial.printf("[SD] %s attempt %u bus init failed: %s (%d)\n",
      reason,unsigned(attempt),esp_err_to_name(result),result);
    return result;
  }
  odysseySdBusInitialized=true;

  sdspi_device_config_t slot=SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.host_id=static_cast<spi_host_device_t>(host.slot);
  slot.gpio_cs=static_cast<gpio_num_t>(ODYSSEY_SD_CS);
  slot.gpio_cd=SDSPI_SLOT_NO_CD;
  slot.gpio_wp=SDSPI_SLOT_NO_WP;

  esp_vfs_fat_mount_config_t mount=VFS_FAT_MOUNT_DEFAULT_CONFIG();
  mount.format_if_mount_failed=false;
  mount.max_files=ODYSSEY_SD_MAX_OPEN_FILES;
  mount.allocation_unit_size=16*1024;
  mount.disk_status_check_enable=false;

  sdmmc_card_t* card=nullptr;
  result=int(esp_vfs_fat_sdspi_mount(ODYSSEY_SD_MOUNT_POINT,&host,&slot,&mount,&card));
  if (result!=ESP_OK || !card) {
    odysseySdBootState=(result==ESP_ERR_NOT_FOUND)?3:2;
    odysseySdProbeStage=(result==ESP_FAIL)?3:2;
    Serial.printf("[SD] %s attempt %u card/FAT init failed: %s (%d), stage=%u\n",
      reason,unsigned(attempt),esp_err_to_name(result),result,unsigned(odysseySdProbeStage.load()));
    // The IDF mount helper removes a partially attached SD device on failure;
    // this owner still owns the SPI bus and must release it before retrying.
    if (odysseySdBusInitialized) {
      const int freeResult=int(spi_bus_free(static_cast<spi_host_device_t>(host.slot)));
      if (freeResult!=ESP_OK)
        Serial.printf("[SD] failed-attempt bus free: %s (%d)\n",esp_err_to_name(freeResult),freeResult);
      odysseySdBusInitialized=false;
    }
    digitalWrite(ODYSSEY_SD_CS,HIGH);
    return result==ESP_OK?ESP_FAIL:result;
  }
  odysseySdCard=card;

  struct stat root{};
  if (stat(ODYSSEY_SD_MOUNT_POINT,&root)!=0 || !S_ISDIR(root.st_mode)) {
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u VFS validation failed errno=%d\n",reason,unsigned(attempt),errno);
    odysseySdReleaseLocked();
    return ESP_FAIL;
  }

  struct stat recordings{};
  if (stat(ODYSSEY_SD_RECORDING_DIR,&recordings)!=0) {
    if (errno!=ENOENT || mkdir(ODYSSEY_SD_RECORDING_DIR,0755)!=0) {
      odysseySdBootState=2;odysseySdProbeStage=4;
      Serial.printf("[SD] %s attempt %u recording directory unavailable errno=%d\n",
        reason,unsigned(attempt),errno);
      odysseySdReleaseLocked();
      return ESP_FAIL;
    }
  } else if (!S_ISDIR(recordings.st_mode)) {
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u /synap is not a directory\n",reason,unsigned(attempt));
    odysseySdReleaseLocked();
    return ESP_FAIL;
  }

  // A successful mount and stat() do not prove opendir() works: the failed
  // C3 logs report stage=6 yet every catalogue returns IO_ERROR. Validate
  // the exact /synap directory and a disposable write before claiming READY.
  DIR* verified=opendir(ODYSSEY_SD_RECORDING_DIR);
  if (!verified) {
    const int saved=errno;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u recordings opendir failed errno=%d\\n",
      reason,unsigned(attempt),saved);
    odysseySdReleaseLocked();return ESP_FAIL;
  }
  if (closedir(verified)!=0) {
    const int saved=errno;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u recordings closedir failed errno=%d\\n",
      reason,unsigned(attempt),saved);
    odysseySdReleaseLocked();return ESP_FAIL;
  }
  const char* probePath="/odyssey-sd/synap/.synap-media-probe.tmp";
  FILE* probe=fopen(probePath,"wb");
  if (!probe) {
    const int saved=errno;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u recordings not writable errno=%d\\n",
      reason,unsigned(attempt),saved);
    odysseySdReleaseLocked();return ESP_FAIL;
  }
  const bool writeOk=fputc('S',probe)!=EOF && fflush(probe)==0;
  const int writeErrno=errno;
  const bool closeOk=fclose(probe)==0;
  const bool removeOk=unlink(probePath)==0;
  if (!writeOk || !closeOk || !removeOk) {
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u write/readiness probe failed errno=%d\\n",
      reason,unsigned(attempt),writeOk?(closeOk?errno:errno):writeErrno);
    odysseySdReleaseLocked();return ESP_FAIL;
  }

  const uint32_t runtimeFreq=odysseySdPromoteClockLocked();
  odysseySdBootState=1;
  odysseySdProbeStage=6;
  const unsigned long long bytes=uint64_t(card->csd.capacity)*uint64_t(card->csd.sector_size);
  Serial.printf("[SD] ready via ESP-IDF SDSPI: %llu MiB, init=%u kHz runtime=%u kHz, pins CS=%d SCK=%d MOSI=%d MISO=%d\n",
    bytes/(1024ULL*1024ULL),unsigned(ODYSSEY_SD_INIT_FREQ_KHZ),unsigned(runtimeFreq),
    ODYSSEY_SD_CS,ODYSSEY_SD_SCK,ODYSSEY_SD_MOSI,ODYSSEY_SD_MISO);
  return ESP_OK;
}

static bool odysseySdMountLocked(const char* reason,uint8_t attempts) {
  if (odysseySdReady()) return true;
  for (uint8_t attempt=1;attempt<=attempts;++attempt) {
    if (odysseySdMountOnceLocked(reason,attempt)==ESP_OK) return true;
    if (attempt<attempts) delay(ODYSSEY_SD_RETRY_BACKOFF_MS*attempt);
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
  const bool ready=odysseySdMountLocked("boot",ODYSSEY_SD_BOOT_ATTEMPTS);
  Serial.printf("[SD] boot initialization complete state=%u stage=%u before BLE\n",
    unsigned(odysseySdBootState.load()),unsigned(odysseySdProbeStage.load()));
  return ready;
}
bool odysseyRecoverSdCard() {
  OdysseySdGuard guard(pdMS_TO_TICKS(2000));
  if (!guard) return false;
  // Recovery is a deliberate full lifecycle reset: publish INITIALIZING, then
  // VFS/card -> SPI device -> SPI bus -> fresh mount attempts.
  odysseySdBootState=0;odysseySdProbeStage=0;
  odysseySdReleaseLocked();
  return odysseySdMountLocked("recovery",ODYSSEY_SD_RECOVERY_ATTEMPTS);
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
