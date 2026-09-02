#pragma once
#include "OtaSession.h"
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#ifndef OTA_BUTTON_PIN
#define OTA_BUTTON_PIN 0 // BOOT on ESP32-S3 SuperMini. Verify against your board schematic.
#endif
#define OTA_WRITE_UUID "4fa12348-0000-1000-8000-00805f9b34fb"
#define OTA_STATUS_UUID "4fa12349-0000-1000-8000-00805f9b34fb"
constexpr uint16_t SYNAP_FIRMWARE_BUILD = 501;
// Compatibility marker, NOT a cryptographic signature. Only install trusted local binaries.
static const char SYNAP_PRODUCT[] = "SYNAP-ESP32S3-OTA-V1";

class EspOtaBackend : public Synap::OtaBackend {
 public:
  const esp_partition_t* target=nullptr;
  bool begin(uint32_t size, const uint8_t* hash) override {
    abort();
    target=esp_ota_get_next_update_partition(nullptr);
    if (!target || target==esp_ota_get_running_partition() || size>target->size) return false;
    memcpy(expected,hash,32);markerPosition=0;markerFound=false;
    mbedtls_sha256_init(&sha);hashActive=true;
    if (mbedtls_sha256_starts(&sha,0)!=0) { abort();return false; }
    if (esp_ota_begin(target,OTA_WITH_SEQUENTIAL_WRITES,&handle)!=ESP_OK) { abort();return false; }
    handleActive=true;return true;
  }
  bool write(const uint8_t* data, size_t size) override {
    if (!handleActive || esp_ota_write(handle,data,size)!=ESP_OK ||
        mbedtls_sha256_update(&sha,data,size)!=0) return false;
    for (size_t i=0;i<size && !markerFound;++i) {
      if (data[i]==uint8_t(SYNAP_PRODUCT[markerPosition])) ++markerPosition;
      else markerPosition=data[i]==uint8_t(SYNAP_PRODUCT[0]) ? 1 : 0;
      if (markerPosition==sizeof(SYNAP_PRODUCT)-1) markerFound=true;
    }
    return true;
  }
  Synap::OtaError finish() override {
    uint8_t digest[32];
    if (!hashActive || mbedtls_sha256_finish(&sha,digest)!=0) return Synap::HASH_MISMATCH;
    mbedtls_sha256_free(&sha);hashActive=false;
    if (memcmp(digest,expected,32)!=0) return Synap::HASH_MISMATCH;
    if (!markerFound) return Synap::INVALID_IMAGE;
    // ESP-IDF validates the full image, chip/revision and signatures if enabled.
    handleActive=false; // esp_ota_end frees the handle even on error.
    return esp_ota_end(handle)==ESP_OK ? Synap::OK : Synap::INVALID_IMAGE;
  }
  bool commit() override { return target && esp_ota_set_boot_partition(target)==ESP_OK; }
  void abort() override {
    if (handleActive) { esp_ota_abort(handle);handleActive=false; }
    if (hashActive) { mbedtls_sha256_free(&sha);hashActive=false; }
  }
 private:
  esp_ota_handle_t handle=0;
  mbedtls_sha256_context sha{};
  bool handleActive=false,hashActive=false,markerFound=false;
  size_t markerPosition=0;
  uint8_t expected[32]{};
};

EspOtaBackend otaBackend;
Synap::OtaSession otaSession(otaBackend);
BLECharacteristic* otaStatusCharacteristic=nullptr;
QueueHandle_t otaQueue=nullptr;
struct OtaMessage { uint32_t connection;uint16_t length;uint8_t data[Synap::OtaSession::PACKET_MAX]; };
std::atomic<bool> otaOverflow{false};
bool otaBusy() { return otaSession.busy(); } // Control task only.

class OtaWriteCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    OtaMessage message{};
    const size_t size=characteristic->getLength();
    if (!size || size>sizeof(message.data) || !characteristic->getData()) return;
    message.connection=connectionGeneration.load();message.length=size;
    memcpy(message.data,characteristic->getData(),size);
    if (xQueueSend(otaQueue,&message,0)!=pdTRUE) otaOverflow.store(true);
  }
};

void otaPublish(bool notify) {
  if (!otaStatusCharacteristic) return;
  uint8_t value[20];otaSession.status(value,SYNAP_FIRMWARE_BUILD);
  otaStatusCharacteristic->setValue(value,sizeof(value));
  if (notify && deviceConnected.load()) otaStatusCharacteristic->notify();
}
void otaInitialize(BLEService* service) {
  pinMode(OTA_BUTTON_PIN,INPUT_PULLUP);
  otaQueue=xQueueCreate(4,sizeof(OtaMessage));
  if (!otaQueue) fatalSetup("[OTA] queue allocation failed");
  auto* command=service->createCharacteristic(OTA_WRITE_UUID,BLECharacteristic::PROPERTY_WRITE);
  command->setCallbacks(new OtaWriteCallbacks());
  otaStatusCharacteristic=service->createCharacteristic(OTA_STATUS_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
#if defined(CONFIG_BLUEDROID_ENABLED)
  otaStatusCharacteristic->addDescriptor(new BLE2902());
#endif
  otaPublish(false);
  Serial.printf("[OTA] %s build=%u; hold BOOT 2 seconds then release while idle\n",SYNAP_PRODUCT,SYNAP_FIRMWARE_BUILD);
}
void otaTick() {
  if (!otaQueue || !otaStatusCharacteristic) return;
  static uint32_t configuredConnection=UINT32_MAX,pressedAt=0,rebootAt=0;
  static bool buttonReleased=false,pressing=false;
  const uint32_t now=millis(),generation=connectionGeneration.load();
  const bool connected=deviceConnected.load();
  const Synap::OtaState previous=otaSession.state;
  otaSession.tick(now,generation,connected);
  if (connected && !otaBusy()) {
    const uint16_t mtu=bleServer->getPeerMTU(bleServer->getConnId());
    const uint16_t packet=mtu>185 ? 182 : (mtu>=23 ? mtu-3 : 20);
    const uint16_t data=packet>9 ? packet-9 : 0;
    if (configuredConnection!=generation || otaSession.maxData!=data) {
      if (configuredConnection!=generation) { pressing=false;buttonReleased=false; }
      const auto* partition=esp_ota_get_next_update_partition(nullptr);
      const auto* metadata=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_OTA,nullptr);
      const auto* slot0=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,nullptr);
      const auto* slot1=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,nullptr);
      otaSession.configure(metadata && slot0 && slot1 && partition &&
        partition->address!=esp_ota_get_running_partition()->address ? partition->size : 0,data);
      configuredConnection=generation;otaPublish(true);
    }
  }
  // Require a release since startup and a fresh press in this connection.
  const bool down=digitalRead(OTA_BUTTON_PIN)==LOW;
  if (!down) {
    if (pressing && uint32_t(now-pressedAt)>=2000 && connected && !streamingEnabled.load()) {
      otaSession.arm(now,generation);otaPublish(true);
    }
    pressing=false;buttonReleased=true;
  } else if (connected && !otaBusy() && !streamingEnabled.load() && buttonReleased && !pressing) {
    pressedAt=now;pressing=true;buttonReleased=false;
  }
  if (!connected || streamingEnabled.load()) { pressing=false;buttonReleased=!down; }
  if (otaOverflow.exchange(false) && otaBusy() && otaSession.state!=Synap::COMMITTED) otaSession.fail(Synap::BAD_PACKET);
  OtaMessage message;
  if (xQueueReceive(otaQueue,&message,0)==pdTRUE && connected && message.connection==generation) {
    otaSession.packet(message.data,message.length,now,generation,streamingEnabled.load());
    // A disconnect during flash work invalidates the session before any further packet.
    otaSession.tick(millis(),connectionGeneration.load(),deviceConnected.load());
    otaPublish(true);
  }
  if (otaSession.state!=previous) {
    otaPublish(true);
    if (otaSession.state==Synap::ARMED || otaBusy()) {
      statusLed.setPixelColor(0,statusLed.Color(24,12,0));statusLed.show();
    } else setDeviceState(deviceConnected.load() ? DeviceState::CONNECTED_IDLE : DeviceState::DISCONNECTED,ErrorCode::NONE);
  }
  if (otaSession.state==Synap::COMMITTED) {
    if (!rebootAt) rebootAt=millis();
    if (uint32_t(millis()-rebootAt)>1500) ESP.restart();
  }
}
