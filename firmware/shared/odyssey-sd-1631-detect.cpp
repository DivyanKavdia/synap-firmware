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
uint32_t odysseyLastRecordFailureBytes();
void odysseySdInvalidateHistoricalFaultResolution();
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
// Formatting is strictly a user-confirmed BLE operation. It is never a mount
// or recovery fallback and must not overlap a disconnected touch recording.
static std::atomic<bool> odysseySdFormatInProgress{false};
bool odysseySdFormatting() { return odysseySdFormatInProgress.load(); }
static std::atomic<uint32_t> odysseyNativeIoFault{0};
// ESP-IDF 5.5.1 sdmmc_write_sectors_dma() sends CMD13 after CMD24/25.
// POSIX EIO loses the originating command and ESP error; capture the first
// ACTUAL low-level failure without sending additional commands or retrying
// writes. These variables are read after the take and persisted separately.
static std::atomic<uint32_t> odysseyWireCmdPlusOne{0};
static std::atomic<uint32_t> odysseyWireError{0};
static std::atomic<uint32_t> odysseyWireArg{0};
static std::atomic<uint32_t> odysseyWireResponse{0};
static std::atomic<uint32_t> odysseyWireLastCmd13{0};
// Field-facing context for the FIRST native transaction failure. All fields
// are observed in the existing command callback; no extra card I/O or retries.
static std::atomic<uint32_t> odysseyWireElapsedMs{0};
static std::atomic<uint32_t> odysseyWireBudgetMs{0};
static std::atomic<uint32_t> odysseyWireDataLength{0};
static std::atomic<uint32_t> odysseyWirePriorCmd{UINT32_MAX};
static std::atomic<uint32_t> odysseyWirePriorSuccessCmd{UINT32_MAX};
static std::atomic<uint32_t> odysseyWireGoodWrites{0};
static std::atomic<uint32_t> odysseyWireWritesAtFailure{0};
// A timed-out or invalid-response CMD24/CMD25 (or following CMD13) can leave the
// always-powered SD controller busy/programming. No further filesystem I/O,
// unmount or same-rail recovery is permitted before a real power cycle.
static std::atomic<bool> odysseySdTimedOutWriteQuarantined{false};
// A CMD17/CMD18 timeout during an already-mounted session may leave the
// continuously-powered SDSPI controller in an unknown data-transfer state.
// Do not treat a returned POSIX errno as evidence of card-side idle.
static std::atomic<bool> odysseySdTimedOutReadQuarantined{false};
bool odysseySdWriteTimeoutQuarantined() {
  return odysseySdTimedOutWriteQuarantined.load();
}
bool odysseySdReadTimeoutQuarantined() {
  return odysseySdTimedOutReadQuarantined.load();
}
void odysseySdClearWireFault() {
  odysseyWireCmdPlusOne=0;
  odysseyWireError=0;
  odysseyWireArg=0;
  odysseyWireResponse=0;
  odysseyWireLastCmd13=0;
  odysseyWireElapsedMs=0;
  odysseyWireBudgetMs=0;
  odysseyWireDataLength=0;
  odysseyWirePriorCmd=UINT32_MAX;
  odysseyWirePriorSuccessCmd=UINT32_MAX;
  odysseyWireGoodWrites=0;
  odysseyWireWritesAtFailure=0;
}
uint32_t odysseySdWireFaultCmd() {
  const uint32_t code=odysseyWireCmdPlusOne.load();
  return code?code-1u:UINT32_MAX;
}
uint32_t odysseySdWireFaultError() { return odysseyWireError.load(); }
uint32_t odysseySdWireFaultArg() { return odysseyWireArg.load(); }
uint32_t odysseySdWireFaultResponse() { return odysseyWireResponse.load(); }
uint32_t odysseySdLastCmd13Response() { return odysseyWireLastCmd13.load(); }
uint32_t odysseySdWireFaultElapsedMs() { return odysseyWireElapsedMs.load(); }
uint32_t odysseySdWireFaultBudgetMs() { return odysseyWireBudgetMs.load(); }
uint32_t odysseySdWireFaultLength() { return odysseyWireDataLength.load(); }
uint32_t odysseySdWireFaultPreviousCmd() { return odysseyWirePriorSuccessCmd.load(); }
uint32_t odysseySdWireFaultGoodWrites() { return odysseyWireWritesAtFailure.load(); }
// Bind the diagnostic interceptor as a local lambda during mount. Arduino's
// .ino preprocessor otherwise emits a forward prototype before sdmmc types
// are in scope, even though the original source includes the right headers.
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
// Do not lose the first native mount-helper return code when subsequent
// GPIO recovery or cleanup fails. ESP_FAIL often means FAT mount failure,
// whereas SPI/card protocol failures return their own ESP error codes.
static std::atomic<int32_t> odysseySdFirstMountFailure{ESP_OK};
int32_t odysseySdFirstMountFailureError() { return odysseySdFirstMountFailure.load(); }
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
// Field signature: CS-high DO releases normally, but the selected card
// drives 0x00 continuously (~1023/1024 bytes) and cannot accept CMD0.
// Refuse repeated mount/recover attempts until an actual card rail reset.
bool odysseySdControllerBusyLow() {
  return odysseySdBootState.load()!=1 &&
    odysseySdBitBangCsHigh.load()==1 &&
    odysseySdBitBangCsLow.load()==1 &&
    odysseySdRawZero.load()>=1000u && odysseySdRawFF.load()<=2u;
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
  // Read-side EIO revokes any prior cold-mount witness. On a subsequent
  // software/OTA reset, this newer unresolved SD failure must stay protected.
  // This touches ESP internal NVS only; no additional SD command is issued.
  odysseySdInvalidateHistoricalFaultResolution();
  // Observation only. Catalogue/read errors must never schedule a destructive
  // remount behind the user's back. Explicit recovery still refuses any
  // timed-out CMD17/CMD18 until the SD card receives a genuine power cycle.
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
// Explicit read-only FAT capacity query: no writes, remounts or recovery.
esp_err_t odysseySdGetCapacity(uint64_t& totalBytes,uint64_t& freeBytes,uint64_t& physicalBytes) {
  totalBytes=0;freeBytes=0;physicalBytes=0;
  OdysseySdGuard guard(pdMS_TO_TICKS(1000));
  if (!guard || !odysseySdReady() || !odysseySdNativeCard ||
      odysseySdTimedOutReadQuarantined.load() ||
      odysseySdTimedOutWriteQuarantined.load() ||
      odysseyRecording.load() || streamingEnabled.load() || otaBusy() || sleepPending)
    return ESP_ERR_INVALID_STATE;
  physicalBytes=uint64_t(odysseySdNativeCard->csd.capacity)*
    uint64_t(odysseySdNativeCard->csd.sector_size);
  const esp_err_t result=esp_vfs_fat_info(ODYSSEY_SD_MOUNT_POINT,&totalBytes,&freeBytes);
  if (result!=ESP_OK || !totalBytes || freeBytes>totalBytes)
    return result==ESP_OK?ESP_FAIL:result;
  return ESP_OK;
}
// Operator-initiated, once-per-boot raw sector classification for SD that
// will not mount after a genuine power cycle. NO FAT calls or card writes.
// This is not a repair, filesystem scan, or backup of the original card.
struct OdysseySdRawProbeResult {
  uint8_t status=0; // 0=not run,1=sector0 read,2=unsafe,3=busy-low,
                    // 4=bus error,5=host error,6=device error,
                    // 7=card protocol error,8=raw sector read error
  int32_t error=0;
  uint32_t sectors=0;
  uint32_t partitionLba=0;
  uint16_t sectorBytes=0;
  uint8_t signature=0; // 0=no 0x55AA boot signature,1=present
  uint8_t fatHint=0;   // 0=unknown,1=FAT VBR,2=MBR partition entry
  uint8_t partitionType=0;  // MBR type: 0x0b/0x0c FAT32, 0x04/0x06/0x0e FAT16
  uint8_t partitionIndex=255;
  uint8_t partitionBounds=0; // 1 only for a FAT partition within card capacity
  uint32_t partitionSectors=0;
  uint8_t vbrRead=0;        // 0=not attempted, 1=read, 2=read error
  int32_t vbrError=0;
  uint8_t vbrSignature=0;
  uint8_t bpbPlausible=0;   // BPB arithmetic only; NOT FAT table validation
  uint8_t fatVariant=0;     // 12/16/32 based on computed cluster count
  uint16_t bpbBytesPerSector=0;
  uint8_t bpbSectorsPerCluster=0;
  uint32_t bpbTotalSectors=0;
};
static bool odysseySdRawProbeAttempted=false;
static OdysseySdRawProbeResult odysseySdRawProbeCached{};
void odysseySdRawProbeOnce() {
  OdysseySdGuard guard(pdMS_TO_TICKS(1000));
  OdysseySdRawProbeResult result{};
  if (!guard || odysseySdReady() || odysseySdNativeCard ||
      odysseySdNativeBusInitialized || odysseyRecording.load() ||
      streamingEnabled.load() || otaBusy() || sleepPending || odysseySdFormatting()) {
    result.status=2;result.error=ESP_ERR_INVALID_STATE;odysseySdRawProbeCached=result;return;
  }
  if (odysseySdControllerBusyLow() || odysseySdBusStuckLow()) {
    result.status=3;result.error=ESP_ERR_TIMEOUT;odysseySdRawProbeCached=result;return;
  }
  if (odysseySdRawProbeAttempted) return;
  odysseySdRawProbeAttempted=true;
  result.sectorBytes=512;
  // The unmounted FAT driver has released the native device and SPI bus.
  // Initialize exactly one isolated, read-only SDSPI session at 400 kHz.
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  pinMode(ODYSSEY_SD_MISO,INPUT_PULLUP);
  spi_bus_config_t bus{};
  bus.mosi_io_num=ODYSSEY_SD_MOSI;
  bus.miso_io_num=ODYSSEY_SD_MISO;
  bus.sclk_io_num=ODYSSEY_SD_SCK;
  bus.quadwp_io_num=-1;
  bus.quadhd_io_num=-1;
  bus.max_transfer_sz=1024;
  esp_err_t err=spi_bus_initialize(ODYSSEY_SD_HOST,&bus,SDSPI_DEFAULT_DMA);
  if (err!=ESP_OK) {
    result.status=4;result.error=err;
  } else {
    const esp_err_t hostErr=sdspi_host_init();
    if (hostErr!=ESP_OK) {
      result.status=5;result.error=hostErr;
    } else {
      sdspi_device_config_t device=SDSPI_DEVICE_CONFIG_DEFAULT();
      device.host_id=ODYSSEY_SD_HOST;
      device.gpio_cs=static_cast<gpio_num_t>(ODYSSEY_SD_CS);
      sdspi_dev_handle_t handle=-1;
      const esp_err_t deviceErr=sdspi_host_init_device(&device,&handle);
      if (deviceErr!=ESP_OK) {
        result.status=6;result.error=deviceErr;
      } else {
        sdmmc_host_t host=SDSPI_HOST_DEFAULT();
        host.slot=handle;
        host.max_freq_khz=400;
        sdmmc_card_t card{};
        const esp_err_t cardErr=sdmmc_card_init(&host,&card);
        if (cardErr!=ESP_OK) {
          result.status=7;result.error=cardErr;
        } else {
          result.sectors=card.csd.capacity;
          // The DMA-aligned buffer can hold exactly one 512-byte sector.
          // Reject unexpected logical sizes instead of risking overflow.
          if (card.csd.sector_size!=512u) {
            result.status=8;result.error=ESP_ERR_NOT_SUPPORTED;
          } else {
          // A successful raw read despite FAT mount failure strongly
          // suggests on-card filesystem/partition metadata trouble.
          alignas(4) uint8_t bootBlock[512]{};
          const esp_err_t readErr=sdmmc_read_sectors(&card,bootBlock,0,1);
          if (readErr!=ESP_OK) {
            result.status=8;result.error=readErr;
          } else {
            result.status=1;
            result.signature=(bootBlock[510]==0x55 && bootBlock[511]==0xAA)?1:0;
            const bool fatVbr=(memcmp(bootBlock+54,"FAT",3)==0 ||
              memcmp(bootBlock+82,"FAT",3)==0);
            if (fatVbr) result.fatHint=1;
            else if (result.signature) {
              // All four MBR entries are inspected. The first sector's
              // "MBR hint" alone does not establish a valid FAT partition.
              const auto u16=[&](size_t off)->uint16_t {
                return uint16_t(bootBlock[off]) |
                  (uint16_t(bootBlock[off+1])<<8);
              };
              const auto u32=[&](size_t off)->uint32_t {
                return uint32_t(bootBlock[off]) |
                  (uint32_t(bootBlock[off+1])<<8) |
                  (uint32_t(bootBlock[off+2])<<16) |
                  (uint32_t(bootBlock[off+3])<<24);
              };
              for (unsigned entry=0;entry<4;++entry) {
                const size_t off=446u+16u*entry;
                const uint8_t type=bootBlock[off+4];
                if (type!=0) result.fatHint=2;
                const uint32_t lba=u32(off+8), count=u32(off+12);
                const bool fatType=type==0x01 || type==0x04 ||
                  type==0x06 || type==0x0b || type==0x0c || type==0x0e;
                const bool bounds=fatType && lba!=0 && count!=0 &&
                  uint64_t(lba)+uint64_t(count)<=uint64_t(result.sectors);
                if (!bounds) continue;
                result.partitionType=type;
                result.partitionIndex=uint8_t(entry);
                result.partitionLba=lba;
                result.partitionSectors=count;
                result.partitionBounds=1;
                break;
              }
              if (result.partitionBounds) {
                // A second, read-only CMD17 examines only the validated
                // FAT partition's volume boot record (VBR). Never try a
                // guessed address or touch FAT allocation tables.
                const esp_err_t vbrErr=sdmmc_read_sectors(
                  &card,bootBlock,result.partitionLba,1);
                if (vbrErr!=ESP_OK) {
                  result.vbrRead=2;result.vbrError=vbrErr;
                } else {
                  result.vbrRead=1;
                  result.vbrSignature=(bootBlock[510]==0x55 &&
                    bootBlock[511]==0xAA)?1:0;
                  const uint16_t bytesPerSector=u16(11);
                  const uint8_t sectorsPerCluster=bootBlock[13];
                  const uint16_t reserved=u16(14);
                  const uint8_t fats=bootBlock[16];
                  const uint16_t rootEntries=u16(17);
                  const uint32_t total=u16(19)?uint32_t(u16(19)):u32(32);
                  const uint32_t fat16=u16(22),fat32=u32(36);
                  const uint32_t fatSectors=fat16?fat16:fat32;
                  const bool jump=bootBlock[0]==0xE9 ||
                    (bootBlock[0]==0xEB && bootBlock[2]==0x90);
                  const bool powerOfTwo=sectorsPerCluster &&
                    (sectorsPerCluster&(sectorsPerCluster-1u))==0 &&
                    sectorsPerCluster<=128;
                  const uint32_t rootDirSectors=
                    (uint32_t(rootEntries)*32u+511u)/512u;
                  const uint64_t overhead=uint64_t(reserved)+
                    uint64_t(fats)*fatSectors+rootDirSectors;
                  result.bpbBytesPerSector=bytesPerSector;
                  result.bpbSectorsPerCluster=sectorsPerCluster;
                  result.bpbTotalSectors=total;
                  if (result.vbrSignature && jump &&
                      bytesPerSector==512 && powerOfTwo &&
                      reserved>=1 && (fats==1 || fats==2) &&
                      fatSectors!=0 && total!=0 &&
                      total<=result.partitionSectors && overhead<total) {
                    const uint32_t clusters=uint32_t(
                      (uint64_t(total)-overhead)/sectorsPerCluster);
                    const uint8_t variant=clusters<4085u?12:
                      clusters<65525u?16:32;
                    const bool variantOk=variant==32
                      ? (fat16==0 && fat32!=0 && rootEntries==0 &&
                          u32(44)>=2u && u32(44)<uint64_t(clusters)+2u)
                      : (fat16!=0 && rootEntries!=0);
                    if (variantOk) {
                      result.bpbPlausible=1;
                      result.fatVariant=variant;
                    }
                  }
                }
              }
            }
          }
          }
        }
        (void)sdspi_host_remove_device(handle);
      }
      (void)sdspi_host_deinit();
    }
    const esp_err_t freeErr=spi_bus_free(ODYSSEY_SD_HOST);
    if (freeErr!=ESP_OK) {
      odysseySdUnsafeToSleep=true;
      if (result.status==1) { result.status=4;result.error=freeErr; }
    }
  }
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  odysseySdUnsafeToSleep=true; // do not deep sleep an unverified SD controller
  odysseySdRawProbeCached=result;
  Serial.printf("[SD] raw sector0 diagnostic status=%u err=%ld sectors=%lu signature=%u FAT-hint=%u (READ ONLY)\\n",
    unsigned(result.status),static_cast<long>(result.error),
    static_cast<unsigned long>(result.sectors),unsigned(result.signature),unsigned(result.fatHint));
  return;
}
const char* odysseySdMountPoint() { return ODYSSEY_SD_MOUNT_POINT; }
// Canonical root capture name is exactly 8.3: R + 7 uppercase hex + .WAV.
// This avoids the fragile mkdir(/synap) CMD24 and long filename writes on
// first use of a freshly formatted FAT card. Historical /synap files remain
// addressable without ever moving or deleting them.
bool odysseySdRootWavName(const char* name) {
  if (!name || strlen(name)!=12u || name[0]!='R' ||
      strcmp(name+8,".WAV")!=0) return false;
  for (size_t i=1;i<8;++i) {
    const char ch=name[i];
    if (!((ch>='0'&&ch<='9')||(ch>='A'&&ch<='F'))) return false;
  }
  return true;
}
bool odysseySdPath(const char* logical,char* full,size_t capacity) {
  if (!logical || logical[0]!='/' || !full || capacity<2) return false;
  if (strstr(logical,"..")) return false;
  // Keep the same stable media-v1 wire path used by Synap's receipts and
  // BLE/Wi-Fi transfer while only changing the *physical* FAT directory.
  if (strncmp(logical,"/synap/",7)==0 &&
      odysseySdRootWavName(logical+7)) {
    const int n=snprintf(full,capacity,"%s/%s",ODYSSEY_SD_MOUNT_POINT,logical+7);
    return n>0 && size_t(n)<capacity;
  }
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

static bool odysseySdBeginLocked(bool explicitlyProvisionFat=false) {
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
  // Only observes original Espressif commands, without retries or extra I/O.
  host.do_transaction=[](int slot,sdmmc_command_t* cmd)->esp_err_t {
  // Stopwatch surrounds the EXACT original native transaction. Duration
  // near cmd->timeout_ms suggests write-program busy; a short failure may be
  // command/data-response related. No extra SD command or delay is added.
  const uint32_t startedAt=millis();
  const esp_err_t result=sdspi_host_do_transaction(slot,cmd);
  const uint32_t elapsed=uint32_t(millis()-startedAt);
  if (cmd && cmd->opcode==13u) odysseyWireLastCmd13=cmd->response[0];
  const esp_err_t error=result!=ESP_OK?result:(cmd?cmd->error:ESP_FAIL);
  const uint32_t prior=odysseyWirePriorCmd.load();
  if (cmd && (error==ESP_ERR_TIMEOUT || error==ESP_ERR_INVALID_RESPONSE) &&
      (cmd->opcode==24u || cmd->opcode==25u ||
       (cmd->opcode==13u && (prior==24u || prior==25u)))) {
    odysseySdTimedOutWriteQuarantined=true;
    odysseySdUnsafeToSleep=true;
  }
  // Field build 2206: BLE sync lost CMD17 after 3072/4096 read bytes.
  // A read during FAT VFS validation can time out before bootState becomes
  // 1. A non-null native card here proves the mount helper finished. Do not
  // quarantine optional card initialization probes when the mount has not
  // returned a card yet.
  if (cmd && error==ESP_ERR_TIMEOUT &&
      (odysseySdBootState.load()==1 || odysseySdNativeCard!=nullptr) &&
      (cmd->opcode==17u || cmd->opcode==18u)) {
    odysseySdTimedOutReadQuarantined=true;
    odysseySdUnsafeToSleep=true;
  }
  // ESP-IDF probes for optional SDIO functionality during card init:
  // CMD52 (IO reset) and CMD5 (IO_SEND_OP_COND). An ordinary SD memory card
  // may reject either; sdmmc_init_io() explicitly treats CMD5 rejection as
  // "not an IO card" and proceeds with the memory-card mount. Do not let
  // these EXPECTED probe results occupy the first real SD read/write fault.
  // This is diagnostics-only: every original transaction and return value
  // is left unchanged. In particular, no timeout recovery or extra CMD is
  // introduced during an existing recording or WAV transfer.
  const bool expectedIoProbe = cmd &&
    (cmd->opcode==52u || cmd->opcode==5u) &&
    (error==ESP_ERR_TIMEOUT || error==ESP_ERR_NOT_SUPPORTED || error==ESP_ERR_INVALID_CRC);
  if (cmd && error!=ESP_OK && !expectedIoProbe) {
    uint32_t expected=0;
    if (odysseyWireCmdPlusOne.compare_exchange_strong(expected,cmd->opcode+1u)) {
      odysseyWireError=uint32_t(error);
      odysseyWireArg=cmd->arg;
      odysseyWireResponse=cmd->response[0];
      odysseyWireElapsedMs=elapsed;
      odysseyWireBudgetMs=cmd->timeout_ms;
      odysseyWireDataLength=cmd->datalen;
      odysseyWirePriorSuccessCmd=odysseyWirePriorCmd.load();
      odysseyWireWritesAtFailure=odysseyWireGoodWrites.load();
    }
  } else if (cmd && error==ESP_OK && !odysseyWireCmdPlusOne.load()) {
    // Count completed commands, NOT guaranteed durable sectors. CMD13/FatFs
    // still own post-program validation. Stop updating on first failure.
    odysseyWirePriorCmd=cmd->opcode;
    if (cmd->opcode==24u || cmd->opcode==25u) ++odysseyWireGoodWrites;
  }
  return result;
};
  sdspi_device_config_t device=SDSPI_DEVICE_CONFIG_DEFAULT();
  device.host_id=ODYSSEY_SD_HOST;
  device.gpio_cs=static_cast<gpio_num_t>(ODYSSEY_SD_CS);
  esp_vfs_fat_sdmmc_mount_config_t mount{};
  mount.format_if_mount_failed=false; // Normal boot/recovery remains read-only
  if (explicitlyProvisionFat) mount.format_if_mount_failed=true; // operator-only emergency FAT rescue
  mount.max_files=ODYSSEY_SD_MAX_OPEN_FILES;
  mount.allocation_unit_size=4096; // ignored for an existing FAT filesystem
  // FatFs disk_status_check_enable issues a *separate* CMD13 before ordinary
  // filesystem operations. A transient post-write/program-busy CMD13 can
  // return STA_NOINIT and poison reads/catalogue even when the previous
  // SD sector operation was successful. The native SDSPI read/write paths
  // already check SD responses and report actual I/O failures. Keep FatFs'
  // disk status gate off; check CMD13 only at explicit power transitions.
  mount.disk_status_check_enable=false;
  result=esp_vfs_fat_sdspi_mount(ODYSSEY_SD_MOUNT_POINT,
    &host,&device,&mount,&odysseySdNativeCard);
  if (result!=ESP_OK) {
    int32_t expected=ESP_OK;
    (void)odysseySdFirstMountFailure.compare_exchange_strong(expected,int32_t(result));
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
    // A responding SD card with an unmountable FAT filesystem is NOT the
    // same as failed SD SPI initialization. Never hammer CMD0 to repair FAT.
    if (odysseySdLastMountError.load()==ESP_FAIL) {
      odysseySdBootState=2;odysseySdProbeStage=3;
      Serial.println("[SD] card initialized, FAT mount failed; preserving files");
      return false;
    }
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
    // A timed-out VFS root/directory read is *not* permission to unmount,
    // trigger a new CMD12/CMD0, or reinitialize the same powered SD card.
    // Preserve the mounted driver for a real rail power cycle.
    if (odysseySdTimedOutReadQuarantined.load() ||
        odysseySdTimedOutWriteQuarantined.load()) {
      odysseySdUnsafeToSleep=true;
      Serial.printf("[SD] VFS validation failed after native timeout step=%u errno=%ld; no SPI teardown or retry\n",
        unsigned(odysseySdVfsStep.load()),static_cast<long>(odysseySdVfsErrno.load()));
      return false;
    }
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
  odysseySdTimedOutWriteQuarantined=false; // only a verified fresh mount clears quarantine
  odysseySdTimedOutReadQuarantined=false;  // never cleared by an ordinary BLE read
  const uint64_t bytes=uint64_t(odysseySdNativeCard->csd.capacity)*
    uint64_t(odysseySdNativeCard->csd.sector_size);
  Serial.printf("[SD] native SDSPI FAT ready capacity=%llu MiB clock=%lu Hz\\n",
    static_cast<unsigned long long>(bytes/(1024ULL*1024ULL)),
    static_cast<unsigned long>(ODYSSEY_SD_DATA_FREQ_HZ));
  return true;
}

static bool odysseySdMountLocked(const char* reason,uint8_t attempts) {
  if (odysseySdReady()) return true;
  if (odysseySdTimedOutReadQuarantined.load() ||
      odysseySdTimedOutWriteQuarantined.load()) {
    odysseySdUnsafeToSleep=true;
    Serial.println("[SD] same-rail remount refused after SD transfer timeout; cycle SD power");
    return false;
  }
  for (uint8_t attempt=1;attempt<=attempts;++attempt) {
    if (odysseySdMountOnceLocked(reason,attempt)) return true;
    if (odysseySdProbeStage.load()==3) break; // Do not repeatedly probe an unmountable FAT
    if (odysseySdTimedOutReadQuarantined.load() ||
        odysseySdTimedOutWriteQuarantined.load()) {
      Serial.println("[SD] stop mount attempts: timed-out SD read/write requires rail power cycle");
      break;
    }
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
// A cold power-on followed by a verified FAT mount establishes that a
// RETAINED diagnostic describes a past failure, not today's SD health.
// Keep the original first-fault journal intact for field diagnostics.
// New recording faults revoke this separate recovery witness BEFORE journaling.
static constexpr uint32_t ODYSSEY_SD_RECOVERY_WITNESS=0x53445231u;
bool odysseySdHistoricalFaultResolved() {
  const uint8_t stage=odysseyLastRecordFailureStage();
  if (stage<44u || stage==48u) return false;
  Preferences prefs;
  if (!prefs.begin("sd-recstate",true)) return false;
  uint32_t witness[3]{};
  const bool read=prefs.getBytesLength("resolved")==sizeof(witness) &&
    prefs.getBytes("resolved",witness,sizeof(witness))==sizeof(witness);
  prefs.end();
  return read && witness[0]==ODYSSEY_SD_RECOVERY_WITNESS &&
    witness[1]==uint32_t(stage) && witness[2]==odysseyLastRecordFailureBytes();
}
static void odysseySdMarkHistoricalFaultResolved() {
  const uint8_t stage=odysseyLastRecordFailureStage();
  if (stage<44u || stage==48u) return;
  const uint32_t witness[3]={ODYSSEY_SD_RECOVERY_WITNESS,
    uint32_t(stage),odysseyLastRecordFailureBytes()};
  Preferences prefs;
  if (!prefs.begin("sd-recstate",false)) return;
  (void)prefs.putBytes("resolved",witness,sizeof(witness));
  prefs.end();
}
void odysseySdInvalidateHistoricalFaultResolution() {
  const uint32_t witness[3]={0u,0u,0u};
  Preferences prefs;
  if (!prefs.begin("sd-recstate",false)) return;
  (void)prefs.putBytes("resolved",witness,sizeof(witness));
  prefs.end();
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
  const bool previousStorageFault=previousRecordStage>=44u && previousRecordStage!=48u &&
    !odysseySdHistoricalFaultResolved();
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
  // Only an actual cold card power-on + successful VFS validation can clear
  // this historical boot veto. Software/OTA reset alone is insufficient.
  if (ready && bootResetReason==ESP_RST_POWERON)
    odysseySdMarkHistoricalFaultResolved();
  Serial.printf("[SD] boot initialization complete state=%u stage=%u before BLE\n",
    unsigned(odysseySdBootState.load()),unsigned(odysseySdProbeStage.load()));
  return ready;
}
bool odysseyRecoverSdCard(const char* reason) {
  // A timed-out read or write is not proof that the card returned to SPI
  // idle. Never attempt CMD0/CMD12, remount or FAT access on that same rail;
  // require actual card power cycling, retaining all original recordings.
  if (odysseySdWriteTimeoutQuarantined() ||
      odysseySdReadTimeoutQuarantined()) {
    odysseySdUnsafeToSleep=true;
    Serial.println("[SD] recovery refused after timed-out CMD17/18 or CMD24/25: SD rail power-cycle required; recordings retained");
    return false;
  }
  OdysseySdGuard guard(pdMS_TO_TICKS(5000));
  if (!guard) return false;
  if (odysseySdControllerBusyLow()) {
    odysseySdUnsafeToSleep=true;
    Serial.println("[SD] recovery refused: selected SD still drives sustained busy-low, cycle SD power");
    return false;
  }
  // A healthy card needs no unmount/reinit on an explicit Retry SD.
  if (odysseySdReady()) {
    odysseySdMarkHistoricalFaultResolved();
    return true;
  }
  odysseySdBootState=0;odysseySdProbeStage=0;
  if (!odysseySdReleaseLocked()) return false;
  const bool recovered=odysseySdMountLocked(reason?reason:"op14",ODYSSEY_SD_RECOVERY_ATTEMPTS);
  // The operator explicitly validated a fresh FAT mount. Resolve only the
  // boot veto, retaining the original CMD24/CMD25 journal for diagnostics.
  if (recovered && !odysseySdWriteTimeoutQuarantined() &&
      !odysseySdReadTimeoutQuarantined())
    odysseySdMarkHistoricalFaultResolved();
  return recovered;
}

// An explicit emergency FAT rescue may provision a NEW filesystem only when
// a physically cold-booted card completed SD SPI initialization but FAT failed.
// This can erase ALL original recordings and is NEVER used during normal boot.
// An MCU software reset does not power-cycle an always-powered SD controller.
static std::atomic<bool> odysseySdEmergencyProvisionAttempted{false};
esp_err_t odysseySdEmergencyProvisionFat() {
  OdysseySdGuard guard(pdMS_TO_TICKS(5000));
  if (!guard || odysseySdEmergencyProvisionAttempted.load() ||
      esp_reset_reason()!=ESP_RST_POWERON ||
      odysseySdReady() || odysseySdProbeStage.load()!=3 ||
      odysseySdLastMountError.load()!=ESP_FAIL ||
      odysseySdNativeCard || odysseySdNativeBusInitialized ||
      odysseySdTimedOutReadQuarantined.load() ||
      odysseySdTimedOutWriteQuarantined.load() ||
      odysseySdControllerBusyLow() ||
      odysseyRecording.load() || streamingEnabled.load() ||
      otaBusy() || sleepPending || odysseySdFormatting())
    return ESP_ERR_INVALID_STATE;

  // One attempt per physical boot: never loop a potentially partial format.
  odysseySdEmergencyProvisionAttempted=true;
  odysseySdFormatInProgress=true;
  odysseySdBootState=0;odysseySdProbeStage=0;
  Serial.println("[SD] EXPLICIT emergency FAT rescue may erase ALL recordings");
  const bool initialized=odysseySdBeginLocked(true);
  const bool verified=initialized &&
    odysseySdValidateVfsLocked("emergency-format",1);
  if (!verified) {
    odysseySdBootState=2;odysseySdProbeStage=3;
    odysseySdUnsafeToSleep=true;
    odysseySdFormatInProgress=false;
    Serial.println("[SD] emergency FAT rescue FAILED; do not repeat this boot");
    return ESP_FAIL;
  }
  odysseySdBootState=1;odysseySdProbeStage=6;
  odysseySdLastMountError=ESP_OK;
  odysseySdUnsafeToSleep=false;
  odysseySdSleepGuardUntil=millis()+5000u;
  odysseySdMarkHistoricalFaultResolved(); // retain first-fault journal
  odysseySdFormatInProgress=false;
  Serial.println("[SD] emergency FAT rescue verified; inspect before recording");
  return ESP_OK;
}

// Explicit media-v1 operation 19 only. The SD must ALREADY be FAT-mounted;
// Espressif's documented formatter formats the mounted card and remounts FAT.
// Never run this from boot, a failed mount, a probe, or automatic recovery.
// The worker owns BLE-side exclusivity; this guard owns all FAT operations.
esp_err_t odysseyFormatSdCard() {
  OdysseySdGuard guard(pdMS_TO_TICKS(1000));
  if (!guard || !odysseySdReady() || !odysseySdNativeCard ||
      odysseySdUnsafeToSleep.load() || odysseyRecording.load() ||
      streamingEnabled.load() || otaBusy() || sleepPending ||
      odysseySdFormatInProgress.exchange(true)) return ESP_ERR_INVALID_STATE;

  Serial.println("[SD] explicit BLE format: permanently erasing ALL card files");
  const esp_err_t result=esp_vfs_fat_sdcard_format(
    ODYSSEY_SD_MOUNT_POINT,odysseySdNativeCard);
  if (result!=ESP_OK) {
    // A failed format may have modified FAT; never claim old recordings
    // survived, and never probe/remount automatically across an unknown state.
    odysseySdBootState=2;odysseySdProbeStage=3;
    odysseySdLastMountError=result;
    odysseySdUnsafeToSleep=true;
    Serial.printf("[SD] explicit format FAILED err=%s; leave card untouched until manual recovery\\n",
      esp_err_to_name(result));
    odysseySdFormatInProgress=false;
    return result;
  }

  // esp_vfs_fat_sdcard_format remounts; verify the mounted FAT root before
  // reporting success. /synap is intentionally not recreated until recording.
  if (!odysseySdValidateVfsLocked("format",1)) {
    odysseySdBootState=2;odysseySdProbeStage=4;
    odysseySdUnsafeToSleep=true;
    odysseySdFormatInProgress=false;
    Serial.println("[SD] format returned OK but FAT validation failed");
    return ESP_FAIL;
  }
  odysseySdBootState=1;odysseySdProbeStage=6;
  odysseySdLastMountError=ESP_OK;
  odysseySdUnsafeToSleep=false;
  odysseySdSleepGuardUntil=millis()+5000u;
  odysseySdFormatInProgress=false;
  Serial.println("[SD] explicit format VERIFIED: FAT mounted and ready");
  return ESP_OK;
}

bool odysseyPrepareSdForPowerTransition(uint32_t timeoutMs) {
  OdysseySdGuard guard(pdMS_TO_TICKS(timeoutMs));
  if (!guard) {
    Serial.println("[SD] power transition deferred: storage busy");
    return false;
  }
  if (odysseyRecording.load() || odysseySdFormatting()) {
    Serial.println("[SD] power transition deferred: local recording or format active");
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
