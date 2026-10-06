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
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT
#error Odyssey C3 SD uses GPIO20/21: enable USB CDC On Boot to keep Serial off UART0 pins
#endif

constexpr int ODYSSEY_SD_CS=SYNAP_SD_CS_PIN, ODYSSEY_SD_SCK=SYNAP_SD_SCK_PIN;
constexpr int ODYSSEY_SD_MOSI=SYNAP_SD_MOSI_PIN, ODYSSEY_SD_MISO=SYNAP_SD_MISO_PIN;
static constexpr spi_host_device_t ODYSSEY_SD_HOST=SPI2_HOST;
static constexpr uint32_t ODYSSEY_SD_SPI_KHZ=400u;
static constexpr uint32_t ODYSSEY_SD_WAV_RATE=8000u;
static constexpr uint16_t ODYSSEY_SD_WAV_SAMPLES_PER_FRAME=SAMPLES_PER_FRAME/2u;
static constexpr size_t ODYSSEY_SD_WRITE_BUFFER_BYTES=4096u;
static constexpr size_t ODYSSEY_SD_WRITE_CHUNK_BYTES=512u;
static constexpr uint32_t ODYSSEY_SD_FLUSH_MS=5000u;
static constexpr char ODYSSEY_SD_MOUNT_POINT[]="/odyssey-sd";
static constexpr char ODYSSEY_SD_DIRECTORY[]="/odyssey-sd/synap";
static_assert((ODYSSEY_SD_WRITE_BUFFER_BYTES%ODYSSEY_SD_WRITE_CHUNK_BYTES)==0,
  "native C3 write buffer must contain complete sectors");

static sdmmc_card_t* odysseyNativeCard=nullptr;
static bool odysseyNativeBusInitialized=false;
static bool odysseyNativeMounted=false;
static esp_err_t odysseyNativeLastError=ESP_OK;
static uint8_t odysseyCleanWriteBuffer[ODYSSEY_SD_WRITE_BUFFER_BYTES];

static void odysseyCleanWavHeader(uint8_t* h,uint32_t pcmBytes) {
  memset(h,0,44);
  memcpy(h,"RIFF",4);
  put32le(h+4,pcmBytes+36u);
  memcpy(h+8,"WAVEfmt ",8);
  put32le(h+16,16u);
  h[20]=1;h[22]=1;
  put32le(h+24,ODYSSEY_SD_WAV_RATE);
  put32le(h+28,ODYSSEY_SD_WAV_RATE*2u);
  h[32]=2;h[34]=16;
  memcpy(h+36,"data",4);
  put32le(h+40,pcmBytes);
}

static void odysseyNativeResetPins() {
  pinMode(ODYSSEY_SD_CS,OUTPUT);digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_SCK,OUTPUT);digitalWrite(ODYSSEY_SD_SCK,LOW);
  pinMode(ODYSSEY_SD_MOSI,OUTPUT);digitalWrite(ODYSSEY_SD_MOSI,HIGH);
  pinMode(ODYSSEY_SD_MISO,INPUT);
  // SD SPI requires pull-ups on CMD/DAT lines. External pull-ups remain
  // preferable, but enable the C3's internal pulls as an electrical-safety aid.
  gpio_pullup_en(static_cast<gpio_num_t>(ODYSSEY_SD_CS));
  gpio_pullup_en(static_cast<gpio_num_t>(ODYSSEY_SD_MOSI));
  gpio_pullup_en(static_cast<gpio_num_t>(ODYSSEY_SD_MISO));
}

static void odysseyNativeUnmount(bool clearProbe=true) {
  if (odysseyNativeMounted && odysseyNativeCard) {
    const esp_err_t err=esp_vfs_fat_sdcard_unmount(ODYSSEY_SD_MOUNT_POINT,odysseyNativeCard);
    if (err!=ESP_OK) Serial.printf("[SD-IDF] unmount failed err=%s (0x%x)\n",esp_err_to_name(err),unsigned(err));
  }
  odysseyNativeMounted=false;
  odysseyNativeCard=nullptr;
  if (odysseyNativeBusInitialized) {
    const esp_err_t err=spi_bus_free(ODYSSEY_SD_HOST);
    if (err!=ESP_OK) Serial.printf("[SD-IDF] spi_bus_free failed err=%s (0x%x)\n",esp_err_to_name(err),unsigned(err));
  }
  odysseyNativeBusInitialized=false;
  odysseyNativeResetPins();
  if (clearProbe) {
    odysseySdBootState=0;
    odysseySdProbeStage=0;
    odysseyNativeLastError=ESP_OK;
  }
}

static bool odysseyNativeMountForTake() {
  odysseySdBootState=0;
  odysseySdProbeStage=0;
  odysseyNativeLastError=ESP_OK;
  odysseyNativeUnmount(true);
  if (odysseyStopRequested.load()) return false;

  spi_bus_config_t busCfg={};
  busCfg.mosi_io_num=ODYSSEY_SD_MOSI;
  busCfg.miso_io_num=ODYSSEY_SD_MISO;
  busCfg.sclk_io_num=ODYSSEY_SD_SCK;
  busCfg.quadwp_io_num=-1;
  busCfg.quadhd_io_num=-1;
  busCfg.max_transfer_sz=4096;

  esp_err_t err=spi_bus_initialize(ODYSSEY_SD_HOST,&busCfg,SDSPI_DEFAULT_DMA);
  if (err!=ESP_OK) {
    odysseyNativeLastError=err;
    odysseyNativeResetPins();
    odysseySdBootState=2;odysseySdProbeStage=1;
    Serial.printf("[SD-IDF] spi_bus_initialize failed err=%s (0x%x)\n",esp_err_to_name(err),unsigned(err));
    return false;
  }
  odysseyNativeBusInitialized=true;

  sdmmc_host_t host=SDSPI_HOST_DEFAULT();
  host.slot=ODYSSEY_SD_HOST;
  host.max_freq_khz=ODYSSEY_SD_SPI_KHZ;

  sdspi_device_config_t slot=SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.host_id=ODYSSEY_SD_HOST;
  slot.gpio_cs=static_cast<gpio_num_t>(ODYSSEY_SD_CS);
  // Give a marginal MISO line the maximum supported ready-high settling time.
  slot.wait_for_miso=127;

  esp_vfs_fat_sdmmc_mount_config_t mount=VFS_FAT_MOUNT_DEFAULT_CONFIG();
  mount.format_if_mount_failed=false;
  mount.max_files=2;
  mount.allocation_unit_size=0;
  mount.disk_status_check_enable=false;

  err=esp_vfs_fat_sdspi_mount(ODYSSEY_SD_MOUNT_POINT,&host,&slot,&mount,&odysseyNativeCard);
  if (err!=ESP_OK) {
    odysseyNativeLastError=err;
    // esp_vfs_fat_sdspi_mount releases the SDSPI device on failure; the bus
    // remains caller-owned and must be released here.
    if (odysseyNativeBusInitialized) {
      const esp_err_t freeErr=spi_bus_free(ODYSSEY_SD_HOST);
      if (freeErr!=ESP_OK) Serial.printf("[SD-IDF] failed-mount spi_bus_free err=%s (0x%x)\n",esp_err_to_name(freeErr),unsigned(freeErr));
    }
    odysseyNativeBusInitialized=false;
    odysseyNativeCard=nullptr;
    odysseyNativeResetPins();
    odysseySdBootState=2;
    odysseySdProbeStage=(err==ESP_FAIL)?3:2;
    Serial.printf("[SD-IDF] native mount failed stage=%u err=%s (0x%x)\n",
      unsigned(odysseySdProbeStage.load()),esp_err_to_name(err),unsigned(err));
    return false;
  }
  odysseyNativeMounted=true;

  struct stat directory{};
  if (stat(ODYSSEY_SD_DIRECTORY,&directory)!=0) {
    if (errno!=ENOENT || mkdir(ODYSSEY_SD_DIRECTORY,0755)!=0) {
      const int savedErrno=errno?errno:EIO;
      Serial.printf("[SD-IDF] /synap create failed errno=%d\n",savedErrno);
      odysseyNativeUnmount(false);
      errno=savedErrno;
      odysseySdBootState=2;odysseySdProbeStage=3;
      return false;
    }
  } else if (!S_ISDIR(directory.st_mode)) {
    Serial.println("[SD-IDF] /synap exists but is not a directory");
    odysseyNativeUnmount(false);
    odysseySdBootState=2;odysseySdProbeStage=3;
    errno=ENOTDIR;
    return false;
  }

  odysseySdBootState=1;
  odysseySdProbeStage=6;
  markOdysseySdBatteryDividerPresent();
  const uint64_t bytes=uint64_t(odysseyNativeCard->csd.capacity)*odysseyNativeCard->csd.sector_size;
  Serial.printf("[SD-IDF] native SDSPI mounted size=%lluMB sector=%u clock=%lukHz\n",
    static_cast<unsigned long long>(bytes/(1024ull*1024ull)),
    unsigned(odysseyNativeCard->csd.sector_size),
    static_cast<unsigned long>(ODYSSEY_SD_SPI_KHZ));
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
  if (!odysseyNativeMountForTake()) return odysseyStopRequested.load();
  if (odysseyStopRequested.load()) { odysseyNativeUnmount();return true; }

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
    const int savedErrno=errno?errno:EIO;
    failureStage=4;
    Serial.printf("[SD-IDF] exclusive create failed errno=%d\n",savedErrno);
    odysseyNativeUnmount(false);
    errno=savedErrno;
    odysseySdBootState=2;odysseySdProbeStage=failureStage;
    return false;
  }

  uint8_t header[44];
  odysseyCleanWavHeader(header,0);
  bool ok=odysseyCleanWriteAll(file,header,sizeof(header));
  if (!ok) failureStage=4;
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
        chunk=ODYSSEY_SD_WRITE_CHUNK_BYTES;
      } else if (finalDrain) chunk=buffered;
      else return true;
      if (!odysseyCleanWriteAll(file,odysseyCleanWriteBuffer,chunk)) {
        failureStage=4;
        return false;
      }
      pcmBytes+=uint32_t(chunk);
      buffered-=chunk;
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
      int16_t pcm[ODYSSEY_SD_WAV_SAMPLES_PER_FRAME];
      while (ok && !odysseyStopRequested.load()) {
        size_t received=0;
        uint8_t emptyReads=0;
        while (received<sizeof(raw) && !odysseyStopRequested.load()) {
          const size_t n=microphoneI2S.readBytes(reinterpret_cast<char*>(raw)+received,sizeof(raw)-received);
          if (!n) {
            if (++emptyReads>=3) { ok=false;failureErrno=EIO;failureStage=6;break; }
          } else {
            received+=n;
            emptyReads=0;
          }
        }
        if (!ok || odysseyStopRequested.load()) break;
        // Offline safe mode stores 8 kHz PCM16. Average adjacent 16 kHz
        // microphone samples before downsampling to avoid a hard decimation edge.
        for (uint16_t i=0;i<ODYSSEY_SD_WAV_SAMPLES_PER_FRAME;++i) {
          const int32_t a=raw[2u*i]>>16;
          const int32_t b=raw[2u*i+1u]>>16;
          pcm[i]=static_cast<int16_t>((a+b)/2);
        }
        if (uint64_t(pcmBytes)+buffered+sizeof(pcm)>0xffff0000ull) break;
        if (buffered+sizeof(pcm)>sizeof(odysseyCleanWriteBuffer) && !drain(false)) {
          ok=false;failureErrno=errno?errno:EIO;break;
        }
        memcpy(odysseyCleanWriteBuffer+buffered,pcm,sizeof(pcm));
        buffered+=sizeof(pcm);
        if (!drain(false)) { ok=false;failureErrno=errno?errno:EIO;break; }

        if (!odysseyCaptureActive.load()) {
          if (fsync(file)!=0) { ok=false;failureErrno=errno?errno:EIO;failureStage=5;break; }
          odysseyRecordingStartedAt=millis();
          odysseyCaptureActive=true;
          odysseySdRecoveryActive=false;
          updateStatusLed(true);
          Serial.printf("[SD-IDF] PCM capture active path=%s\n",path);
        }
        if (uint32_t(millis()-lastFlush)>=ODYSSEY_SD_FLUSH_MS) {
          if (!drain(false)) { ok=false;failureErrno=errno?errno:EIO;if(!failureStage)failureStage=4;break; }
          if (fsync(file)!=0) { ok=false;failureErrno=errno?errno:EIO;failureStage=5;break; }
          lastFlush=millis();
        }
      }
      odysseyCaptureActive=false;
      updateStatusLed(true);
      stopMicrophone();
    }
  }
#else
  ok=false;failureErrno=ENOSYS;failureStage=6;
#endif

  if (ok && !drain(true)) { ok=false;failureErrno=errno?errno:EIO;if(!failureStage)failureStage=4; }
  if (ok && pcmBytes) {
    odysseyCleanWavHeader(header,pcmBytes);
    if (fsync(file)!=0) {
      ok=false;failureErrno=errno?errno:EIO;failureStage=5;
    } else if (lseek(file,0,SEEK_SET)!=0 ||
               !odysseyCleanWriteAll(file,header,sizeof(header))) {
      ok=false;failureErrno=errno?errno:EIO;failureStage=4;
    } else if (fsync(file)!=0) {
      ok=false;failureErrno=errno?errno:EIO;failureStage=5;
    }
  }
  if (close(file)!=0) {
    ok=false;
    if (!failureErrno) failureErrno=errno?errno:EIO;
    if (!failureStage) failureStage=5;
  }
  if (ok && pcmBytes && rename(path,finished)!=0) {
    ok=false;failureErrno=errno?errno:EIO;failureStage=5;
  }
  if (ok && !pcmBytes && unlink(path)!=0) {
    ok=false;failureErrno=errno?errno:EIO;if(!failureStage)failureStage=5;
  }

  delay(20);
  odysseyNativeUnmount(false);
  if (ok && pcmBytes) {
    odysseySdBootState=1;odysseySdProbeStage=6;
    Serial.printf("[SD-IDF] WAV saved path=%s pcmBytes=%lu\n",finished,static_cast<unsigned long>(pcmBytes));
  } else if (!ok) {
    odysseySdBootState=2;odysseySdProbeStage=failureStage?failureStage:4;
    Serial.printf("[SD-IDF] WAV failed; partial retained path=%s pcmBytes=%lu errno=%d stage=%u esp=%s (0x%x)\n",
      path,static_cast<unsigned long>(pcmBytes),failureErrno,unsigned(odysseySdProbeStage.load()),
      esp_err_to_name(odysseyNativeLastError),unsigned(odysseyNativeLastError));
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
  odysseyNativeUnmount(true);
  odysseySdRecoveryActive=false;
  odysseyCaptureActive=false;
  odysseyRecording=false;
  odysseyStopRequested=false;
  Serial.println("[SD-IDF] native C3 electrical-safe recorder ready; 400kHz/8kPCM mount deferred to offline double tap");
}

void odysseyToggleRecording() {
  if (odysseyRecording.load()) {
    odysseyStopRequested=true;
    updateStatusLed(true);
    Serial.println("[TOUCH] double tap -> native SD audio STOP");
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
  if (xTaskCreate(odysseyCleanRecordTask,"sd-idf-audio",8192,nullptr,2,nullptr)!=pdPASS) {
    odysseyRecording=false;
    odysseySdRecoveryActive=false;
    odysseyRecordFaultAt=millis();
    applyCpuPowerProfile(false);
    updateStatusLed(true);
    Serial.println("[SD-IDF] recorder task create failed");
    return;
  }
  Serial.println("[TOUCH] double tap -> native SD audio START");
}

bool odysseyPrepareForConnectedStreaming(uint32_t timeoutMs) {
  if (!odysseyRecording.load()) return true;
  odysseyStopRequested=true;
  const uint32_t started=millis();
  while (odysseyRecording.load() && uint32_t(millis()-started)<timeoutMs) delay(10);
  if (odysseyRecording.load()) {
    Serial.println("[SD-IDF] recorder did not finalize before BLE handoff");
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
  odysseyNativeUnmount(true);
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
