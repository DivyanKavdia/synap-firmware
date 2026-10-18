// Chakshu native ESP-IDF SDSPI / FatFs backend.
// Keeps Arduino fs::File/FS convenience API for camera, WAV and BLE media,
// but never uses Arduino SD.h, sd_diskio or generic SPIClass bus ownership.
// Matches Odyssey C3's native SPI2 mount, non-formatting and timeout quarantine.
#include <FS.h>
#include <vfs_api.h>
#include <SPI.h>
#include <esp_vfs_fat.h>
#include <driver/spi_master.h>
#include <driver/sdspi_host.h>
#include <sdmmc_cmd.h>
#include <errno.h>

enum : uint8_t { CARD_NONE=0, CARD_MMC=1, CARD_SD=2, CARD_SDHC=3, CARD_UNKNOWN=4 };

class ChakshuNativeSD final : public fs::FS {
  sdmmc_card_t* card_=nullptr;
  bool bus_=false, uncertain_=false;
  uint32_t lastCommand_=UINT32_MAX, lastError_=0;
  const char* mountedAt_="/sd";

  static ChakshuNativeSD* active_;
  static esp_err_t transaction(int slot,sdmmc_command_t* cmd) {
    esp_err_t result=sdspi_host_do_transaction(slot,cmd);
    // Same limited init-only CRC enable retry used by Odyssey C3. Never retry
    // a possibly completed file write, data read or unbounded CMD25.
    if(cmd && cmd->opcode==59 && cmd->arg==1 &&
       result==ESP_ERR_NOT_SUPPORTED) {
      vTaskDelay(pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1);
      result=sdspi_host_do_transaction(slot,cmd);
    }
    if(active_ && cmd) {
      const esp_err_t error=result!=ESP_OK?result:cmd->error;
      if(error!=ESP_OK && active_->lastCommand_==UINT32_MAX &&
         cmd->opcode!=5 && cmd->opcode!=52) {
        active_->lastCommand_=cmd->opcode;
        active_->lastError_=uint32_t(error);
      }
      if((error==ESP_ERR_TIMEOUT || error==ESP_ERR_INVALID_RESPONSE) &&
         (cmd->opcode==17 || cmd->opcode==18 || cmd->opcode==24 ||
          cmd->opcode==25 || cmd->opcode==13))
        active_->uncertain_=true;
    }
    return result;
  }

  bool setupBus(uint8_t cs) {
    pinMode(cs,OUTPUT);
    digitalWrite(cs,HIGH);
    pinMode(8,INPUT_PULLUP); // MISO must idle high when SD is deselected
    spi_bus_config_t bus{};
    bus.mosi_io_num=9;
    bus.miso_io_num=8;
    bus.sclk_io_num=7;
    bus.quadwp_io_num=-1;
    bus.quadhd_io_num=-1;
    bus.max_transfer_sz=4608;
    const esp_err_t result=spi_bus_initialize(SPI2_HOST,&bus,SDSPI_DEFAULT_DMA);
    bus_=result==ESP_OK;
    if(!bus_)lastError_=uint32_t(result);
    return bus_;
  }
  void freeBus() {
    if(bus_ && !uncertain_) {
      const esp_err_t result=spi_bus_free(SPI2_HOST);
      if(result==ESP_OK)bus_=false;
      else {lastError_=uint32_t(result);uncertain_=true;}
    }
  }
public:
  ChakshuNativeSD():fs::FS(fs::FSImplPtr(new VFSImpl())) {}
  bool mountUncertain()const{return uncertain_;}
  uint32_t lastWireCommand()const{return lastCommand_;}
  uint32_t lastWireError()const{return lastError_;}
  bool begin(uint8_t cs,SPIClass&,uint32_t frequency,const char* mountpoint,
             uint8_t max_files,bool format_if_empty) {
    if(format_if_empty || uncertain_ || cs!=21 || strcmp(mountpoint,"/sd"))return false;
    end();
    if(bus_ || !setupBus(cs))return false;
    active_=this;
    lastCommand_=UINT32_MAX;
    lastError_=0;
    sdmmc_host_t host=SDSPI_HOST_DEFAULT();
    host.slot=SPI2_HOST;
    host.max_freq_khz=std::max<uint32_t>(400u,frequency/1000u);
    host.do_transaction=&ChakshuNativeSD::transaction;
    sdspi_device_config_t device=SDSPI_DEVICE_CONFIG_DEFAULT();
    device.host_id=SPI2_HOST;
    device.gpio_cs=static_cast<gpio_num_t>(cs);
    esp_vfs_fat_sdmmc_mount_config_t cfg{};
    cfg.format_if_mount_failed=false;
    cfg.max_files=max_files;
    cfg.allocation_unit_size=4096;
    cfg.disk_status_check_enable=false;
    const esp_err_t result=esp_vfs_fat_sdspi_mount(mountpoint,&host,&device,&cfg,&card_);
    if(result!=ESP_OK) {
      card_=nullptr;
      lastError_=uint32_t(result);
      // If a CMD17/18 timed out during mount, do not reset a potentially
      // active controller or proceed through further speed fallback attempts.
      if(!uncertain_)freeBus();
      active_=nullptr;
      return false;
    }
    mountedAt_=mountpoint;
    _impl->mountpoint(mountedAt_);
    return true;
  }
  void end() {
    if(uncertain_)return; // C3-style no unmount after ambiguous sector I/O
    if(card_) {
      const esp_err_t error=esp_vfs_fat_sdcard_unmount(mountedAt_,card_);
      if(error!=ESP_OK){lastError_=uint32_t(error);uncertain_=true;return;}
      card_=nullptr;
      _impl->mountpoint(nullptr);
    }
    freeBus();
  }
  uint8_t cardType()const {
    if(!card_)return CARD_NONE;
    return card_->ocr & (1u<<30) ? CARD_SDHC : CARD_SD;
  }
  uint64_t totalBytes() {
    uint64_t total=0,free=0;
    return card_ && esp_vfs_fat_info(mountedAt_,&total,&free)==ESP_OK?total:0;
  }
  uint64_t usedBytes() {
    uint64_t total=0,free=0;
    return card_ && esp_vfs_fat_info(mountedAt_,&total,&free)==ESP_OK && total>=free
      ?total-free:0;
  }
  void probeReadOnly(uint8_t& type,bool& attempted,bool& sectorOk,
                     bool& signature,bool& filesystem) {
    type=CARD_NONE;attempted=false;sectorOk=false;signature=false;filesystem=false;
    if(bus_ || uncertain_ || !setupBus(21))return;
    const esp_err_t hostError=sdspi_host_init();
    if(hostError!=ESP_OK){lastError_=uint32_t(hostError);freeBus();return;}
    sdspi_device_config_t device=SDSPI_DEVICE_CONFIG_DEFAULT();
    device.host_id=SPI2_HOST;
    device.gpio_cs=GPIO_NUM_21;
    sdspi_dev_handle_t handle=-1;
    const esp_err_t deviceError=sdspi_host_init_device(&device,&handle);
    if(deviceError==ESP_OK) {
      sdmmc_host_t host=SDSPI_HOST_DEFAULT();
      host.slot=handle;
      host.max_freq_khz=400;
      host.do_transaction=&ChakshuNativeSD::transaction;
      sdmmc_card_t probe{};
      active_=this;
      const esp_err_t result=sdmmc_card_init(&host,&probe);
      if(result==ESP_OK) {
        type=(probe.ocr & (1u<<30))?CARD_SDHC:CARD_SD;
        uint8_t sector[512]{};
        attempted=true;
        const esp_err_t readResult=sdmmc_read_sectors(&probe,sector,0,1);
        sectorOk=readResult==ESP_OK;
        signature=sectorOk && sector[510]==0x55 && sector[511]==0xAA;
        if(!sectorOk)lastError_=uint32_t(readResult);
      } else {type=CARD_UNKNOWN;lastError_=uint32_t(result);}
      active_=nullptr;
      if(!uncertain_)(void)sdspi_host_remove_device(handle);
    } else lastError_=uint32_t(deviceError);
    if(!uncertain_) {
      (void)sdspi_host_deinit();
      freeBus();
    }
    // Do not mount, format, or inspect FAT after a failed native mount.
  }
};
ChakshuNativeSD* ChakshuNativeSD::active_=nullptr;
static ChakshuNativeSD SD;
