/* ArduinoDroid single-file edition of Synap firmware 1.0.0 / automated OTA builds.
 * All project headers are embedded below; no separate .h files are needed.
 * Requires Arduino-ESP32 3.3.5 and Adafruit NeoPixel.
 * Select your actual ESP32-S3 board and an OTA-capable partition scheme.
 * Default is simulated audio (USE_REAL_I2S_MIC=0).
 * OTA protocol 3 uses the public device ID as the update target, with no owner key.
 * Device IDs and SHA-256 are not authentication; nearby BLE clients can update.
 * Source/protocol checked; not compiled or tested on physical ESP32-S3 here.
 */

/*
 * Synap Pendant 1.0.0 — ESP32-S3FH4R2 / Arduino-ESP32 3.3.5
 * Binary BLE Protocol v2; 16 kHz mono PCM16, 50 ms frames.
 * Default: generated 440 Hz tone. No microphone is needed.
 * INMP441: 3V3, GND, BCLK=4, WS=5, SD=6, L/R=GND.
 * GPIO48 RGB: red offline, blue idle, green recording, purple error.
 * BLE callbacks only queue events. The control task owns all state and LED writes.
 */
#include <Arduino.h>
#include <BLEDevice.h>
#include <esp_mac.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#if defined(CONFIG_BLUEDROID_ENABLED)
#include <BLE2902.h>
#endif
#include <Adafruit_NeoPixel.h>
#include <atomic>
#include <math.h>

#ifndef USE_REAL_I2S_MIC
#define USE_REAL_I2S_MIC 0
#endif
#if USE_REAL_I2S_MIC
#include <ESP_I2S.h>
I2SClass microphoneI2S;
bool microphoneReady = false;
#endif

#define DEVICE_NAME "dk-pendant"
#define DEVICE_ID_UUID "4fa1234c-0000-1000-8000-00805f9b34fb"
// Public board identity, independent of firmware version, NVS and OTA authorization.
char synapDeviceId[19] = {};
#define SERVICE_UUID "4fa12345-0000-1000-8000-00805f9b34fb"
#define AUDIO_CHAR_UUID "4fa12346-0000-1000-8000-00805f9b34fb"
#define CONTROL_CHAR_UUID "4fa12347-0000-1000-8000-00805f9b34fb"

constexpr uint8_t PROTOCOL_VERSION = 2;
constexpr uint8_t AUDIO_PACKET_MAGIC = 0xA5;
constexpr uint8_t STATUS_PACKET_MAGIC = 0x5A;
constexpr uint8_t CMD_STOP = 0x00;
constexpr uint8_t CMD_START = 0x01;
constexpr uint8_t CMD_GET_STATUS = 0x02;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint16_t FRAME_DURATION_MS = 50;
constexpr uint16_t SAMPLES_PER_FRAME = 800;
constexpr uint16_t AUDIO_BYTES_PER_FRAME = 1600;
constexpr uint8_t AUDIO_HEADER_BYTES = 8;
constexpr uint8_t MIN_CHUNKS_PER_FRAME = 10;
constexpr uint8_t MAX_CHUNKS_PER_FRAME = 20;
constexpr uint16_t MIN_REQUIRED_MTU = 91;
constexpr uint16_t REQUESTED_MTU = 185;
constexpr uint16_t MAX_AUDIO_PAYLOAD_BYTES = 160;
constexpr uint8_t RGB_LED_PIN = 48;
constexpr int8_t I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6;

enum class DeviceState : uint8_t { DISCONNECTED=0, CONNECTED_IDLE=1, STREAMING=2, ERROR=3 };
enum class ErrorCode : uint8_t {
  NONE=0, MTU_TOO_SMALL=1, AUDIO_NOT_SUBSCRIBED=2,
  AUDIO_SOURCE_FAILED=3, PROTOCOL_MISMATCH=4, BAD_COMMAND=5, TRANSPORT_CHANGED=6
};
enum class EventType : uint8_t { CONNECTED, DISCONNECTED, COMMAND, STREAM_ERROR };
struct ControlMessage {
  EventType type;
  uint8_t command, version;
  uint32_t connection, stream;
};
struct AudioFrame {
  uint32_t generation;
  uint16_t sequence; // Assigned at capture so queue drops remain visible on the wire.
  int16_t samples[SAMPLES_PER_FRAME];
};
static_assert(sizeof(AudioFrame::samples) == AUDIO_BYTES_PER_FRAME, "PCM frame size");

Adafruit_NeoPixel statusLed(1, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);
BLEServer* bleServer = nullptr;
BLECharacteristic* audioCharacteristic = nullptr;
BLECharacteristic* controlCharacteristic = nullptr;
#if defined(CONFIG_BLUEDROID_ENABLED)
BLE2902* audioCccd = nullptr;
#endif
QueueHandle_t audioFrameQueue = nullptr, controlQueue = nullptr;
std::atomic<bool> deviceConnected{false}, streamingEnabled{false};
std::atomic<uint32_t> connectionGeneration{0}, streamGeneration{0};
std::atomic<uint32_t> capturedFrames{0}, captureDrops{0}, notifyAccepted{0}, notifyRejected{0}, controlDrops{0};
DeviceState deviceState = DeviceState::DISCONNECTED;
ErrorCode errorCode = ErrorCode::NONE;
std::atomic<uint16_t> peerMtu{23}, attValueCapacity{20}, audioPayloadBytes{0};
std::atomic<uint8_t> chunksPerFrame{0};
float tonePhase = 0;
uint32_t disconnectedAt = 0;
bool restartAdvertising = false;

// Explicit prototypes prevent Arduino's auto-prototyper from duplicating defaults.
void setDeviceState(DeviceState state, ErrorCode error);
void updateStatusCharacteristic(bool notify);
void stopStreaming(ErrorCode reason = ErrorCode::NONE);
bool configureTransportFromPeerMtu();
void startStreaming(uint8_t version);
void queueEvent(EventType type, uint8_t command, uint8_t version, uint32_t stream);
void requestStreamError(ErrorCode error, uint32_t generation);
void processCommand(uint8_t command, uint8_t version);
void controlTask(void* parameter);
void acquisitionTask(void* parameter);
void transmitterTask(void* parameter);
bool acquireAudioFrame(AudioFrame& frame);
bool sendAudioFrame(const AudioFrame& frame, uint16_t sequence);
void initializeBLE();
void fatalSetup(const char* message);

// Explicit OTA prototypes for Arduino sketch preprocessing.
bool otaBusy();
void otaPublish(bool notify);
void otaInitialize(BLEService* service);
void otaTick();

// BEGIN EMBEDDED OtaSession.h
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Transport-independent protocol engine; only the control task calls these methods.
namespace Synap {
enum OtaState : uint8_t { OTA_DISABLED, AVAILABLE, RESERVED, RECEIVING, READY, COMMITTED, FAILED };
enum OtaError : uint8_t { OK, NOT_AVAILABLE, BAD_PACKET, BAD_SIZE, BAD_OFFSET, FLASH_ERROR,
  INVALID_IMAGE, HASH_MISMATCH, LINK_LOST, TIMED_OUT, CANCELLED, BUSY, DEVICE_MISMATCH };
struct OtaBackend {
  virtual ~OtaBackend() = default;
  virtual bool matchesDevice(const uint8_t* deviceId) = 0;
  virtual bool begin(uint32_t size, const uint8_t* hash) = 0;
  virtual bool write(const uint8_t* data, size_t size) = 0;
  virtual OtaError finish() = 0;
  virtual bool commit() = 0;
  virtual void abort() = 0;
};
class OtaSession {
 public:
  static constexpr size_t PACKET_MAX = 182;
  OtaState state = OTA_DISABLED;
  OtaError error = OK;
  uint32_t session = 0, offset = 0, capacity = 0;
  uint16_t maxData = 0;
  explicit OtaSession(OtaBackend& backend) : backend(backend) {}
  static uint32_t u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
  }
  static void put32(uint8_t* p, uint32_t n) { for (int i=0;i<4;++i) p[i]=uint8_t(n>>(8*i)); }
  bool busy() const { return state==RECEIVING || state==READY || state==COMMITTED; }
  void configure(uint32_t bytes, uint16_t data) {
    capacity=bytes;maxData=data;
    if (!busy()) { state=capacity && maxData>=64 ? AVAILABLE : OTA_DISABLED;error=OK;session=offset=0; }
  }
  void fail(OtaError reason) {
    backend.abort();state=FAILED;error=reason;lastLength=0;
  }
  void tick(uint32_t now, uint32_t connection, bool connected) {
    if (state==COMMITTED) return; // Boot selection is already committed; never claim cancellation.
    if (busy()) {
      if (!connected || connection!=owner) { fail(LINK_LOST);return; }
      if (uint32_t(now-last)>45000u) fail(TIMED_OUT);
    }
  }
  void packet(const uint8_t* p, size_t n, uint32_t now, uint32_t connection, bool recording) {
    if (state==COMMITTED) return;
    if (!p || n<5 || n>PACKET_MAX) { if (busy()) fail(BAD_PACKET);return; }
    const uint8_t command=p[0];const uint32_t id=u32(p+1);
    if (command==1) { // BEGIN: command, session, length, sha256, 18-byte public device ID.
      if (busy()) return;
      session=id;offset=0;
      if (!capacity || maxData<64) { state=OTA_DISABLED;error=BAD_SIZE;return; }
      if (recording) { error=BUSY;return; }
      if (n!=59 || !id) { error=BAD_PACKET;return; }
      const uint32_t bytes=u32(p+5);
      if (bytes<36 || bytes>capacity) { error=BAD_SIZE;return; }
      if (!backend.matchesDevice(p+41)) { state=AVAILABLE;error=DEVICE_MISMATCH;return; }
      session=id;offset=0;size=bytes;error=OK;lastLength=0;
      owner=connection;
      if (!backend.begin(bytes,p+9)) { fail(FLASH_ERROR);return; }
      state=RECEIVING;last=now;return;
    }
    if (!busy() || connection!=owner || id!=session) return;
    if (command==5 && n==5) { fail(CANCELLED);return; }
    if (command==2 && state==RECEIVING && n>9 && n-9<=maxData) {
      const uint32_t position=u32(p+5);const size_t bytes=n-9;
      // Exactly one previous packet may be repeated after a lost application ACK.
      if (lastLength && position==lastOffset && bytes==lastLength &&
          memcmp(lastData,p+9,bytes)==0) { last=now;return; }
      if (position!=offset || bytes>size-offset) { fail(BAD_OFFSET);return; }
      if (offset==0 && (bytes<36 || p[9]!=0xE9 || p[21]!=9 || p[22]!=0 ||
          u32(p+9+32)!=0xABCD5432)) { fail(INVALID_IMAGE);return; }
      if (!backend.write(p+9,bytes)) { fail(FLASH_ERROR);return; }
      lastOffset=position;lastLength=bytes;memcpy(lastData,p+9,bytes);
      offset+=bytes;last=now;return;
    }
    if (command==3 && n==5 && state==RECEIVING && offset==size) {
      const OtaError result=backend.finish();
      if (result!=OK) { fail(result);return; }
      state=READY;last=now;return;
    }
    if (command==4 && n==5 && state==READY) {
      if (!backend.commit()) { fail(FLASH_ERROR);return; }
      state=COMMITTED;last=now;return;
    }
    fail(BAD_PACKET);
  }
  void status(uint8_t* p, uint16_t build) const {
    memset(p,0,20);p[0]=0xD7;p[1]=3;p[2]=state;p[3]=error;
    put32(p+4,session);put32(p+8,offset);put32(p+12,capacity);
    p[16]=maxData&255;p[17]=maxData>>8;p[18]=build&255;p[19]=build>>8;
  }
 private:
  OtaBackend& backend;
  uint32_t owner=0,last=0,size=0,lastOffset=0;
  size_t lastLength=0;
  uint8_t lastData[PACKET_MAX-9]{};
};
}
// END EMBEDDED OtaSession.h

// BEGIN EMBEDDED SynapOTA.h
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

#define OTA_WRITE_UUID "4fa12348-0000-1000-8000-00805f9b34fb"
#define OTA_STATUS_UUID "4fa12349-0000-1000-8000-00805f9b34fb"
#ifndef SYNAP_BUILD
#define SYNAP_BUILD 506
#endif
static_assert(SYNAP_BUILD > 503 && SYNAP_BUILD <= 65535, "OTA build must fit the protocol counter");
constexpr uint16_t SYNAP_FIRMWARE_BUILD = SYNAP_BUILD;
#define SYNAP_STRING_INNER(x) #x
#define SYNAP_STRING(x) SYNAP_STRING_INNER(x)
// Kept in the image and exposed over BLE for release/board verification.
static const char SYNAP_FIRMWARE_ID[] =
  "SYNAP-FW:esp32s3-fh4r2-qspi-4m:1.0.0:" SYNAP_STRING(SYNAP_BUILD);
// Compatibility marker, NOT a signature. No secret or publisher authentication.
static const char SYNAP_PRODUCT[] = "SYNAP-ESP32S3-OTA-ID-V3";
static const char SYNAP_TARGET_MARKER[] = "SYNAP-FW:esp32s3-fh4r2-qspi-4m:";

class EspOtaBackend : public Synap::OtaBackend {
 public:
  const esp_partition_t* target=nullptr;
  bool matchesDevice(const uint8_t* deviceId) override {
    return memcmp(deviceId,synapDeviceId,18)==0;
  }
  bool begin(uint32_t size, const uint8_t* hash) override {
    abort();
    target=esp_ota_get_next_update_partition(nullptr);
    if (!target || target==esp_ota_get_running_partition() || size>target->size) return false;
    memcpy(expected,hash,32);markerPosition=targetPosition=0;markerFound=targetFound=false;
    mbedtls_sha256_init(&sha);hashActive=true;
    if (mbedtls_sha256_starts(&sha,0)!=0) { abort();return false; }
    if (esp_ota_begin(target,OTA_WITH_SEQUENTIAL_WRITES,&handle)!=ESP_OK) { abort();return false; }
    handleActive=true;return true;
  }
  bool write(const uint8_t* data, size_t size) override {
    if (!handleActive || esp_ota_write(handle,data,size)!=ESP_OK ||
        mbedtls_sha256_update(&sha,data,size)!=0) return false;
    for (size_t i=0;i<size && (!markerFound || !targetFound);++i) {
      if (!markerFound) {
        if (data[i]==uint8_t(SYNAP_PRODUCT[markerPosition])) ++markerPosition;
        else markerPosition=data[i]==uint8_t(SYNAP_PRODUCT[0]) ? 1 : 0;
        if (markerPosition==sizeof(SYNAP_PRODUCT)-1) markerFound=true;
      }
      if (!targetFound) {
        if (data[i]==uint8_t(SYNAP_TARGET_MARKER[targetPosition])) ++targetPosition;
        else targetPosition=data[i]==uint8_t(SYNAP_TARGET_MARKER[0]) ? 1 : 0;
        if (targetPosition==sizeof(SYNAP_TARGET_MARKER)-1) targetFound=true;
      }
    }
    return true;
  }
  Synap::OtaError finish() override {
    uint8_t digest[32];
    if (!hashActive || mbedtls_sha256_finish(&sha,digest)!=0) return Synap::HASH_MISMATCH;
    mbedtls_sha256_free(&sha);hashActive=false;
    if (memcmp(digest,expected,32)!=0) return Synap::HASH_MISMATCH;
    if (!markerFound || !targetFound) return Synap::INVALID_IMAGE;
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
  bool handleActive=false,hashActive=false,markerFound=false,targetFound=false;
  size_t markerPosition=0,targetPosition=0;
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
  otaQueue=xQueueCreate(4,sizeof(OtaMessage));
  if (!otaQueue) fatalSetup("[OTA] queue allocation failed");
  auto* command=service->createCharacteristic(OTA_WRITE_UUID,BLECharacteristic::PROPERTY_WRITE);
  command->setCallbacks(new OtaWriteCallbacks());
  otaStatusCharacteristic=service->createCharacteristic(OTA_STATUS_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  auto* identity=service->createCharacteristic("4fa1234b-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_READ);
  identity->setValue(SYNAP_FIRMWARE_ID);
#if defined(CONFIG_BLUEDROID_ENABLED)
  otaStatusCharacteristic->addDescriptor(new BLE2902());
#endif
  otaPublish(false);
  Serial.printf("[OTA] %s build=%u; target=%s; no update key required.\n",
    SYNAP_PRODUCT,SYNAP_FIRMWARE_BUILD,synapDeviceId);
}
void otaTick() {
  if (!otaQueue || !otaStatusCharacteristic) return;
  static uint32_t configuredConnection=UINT32_MAX,rebootAt=0;
  const uint32_t now=millis(),generation=connectionGeneration.load();
  const bool connected=deviceConnected.load();
  const Synap::OtaState previous=otaSession.state;
  otaSession.tick(now,generation,connected);
  if (connected && !otaBusy()) {
    const uint16_t mtu=bleServer->getPeerMTU(bleServer->getConnId());
    const uint16_t packet=mtu>185 ? 182 : (mtu>=23 ? mtu-3 : 20);
    const uint16_t data=packet>9 ? packet-9 : 0;
    if (configuredConnection!=generation || otaSession.maxData!=data) {
      const auto* partition=esp_ota_get_next_update_partition(nullptr);
      const auto* metadata=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_OTA,nullptr);
      const auto* slot0=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,nullptr);
      const auto* slot1=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,nullptr);
      otaSession.configure(metadata && slot0 && slot1 && partition &&
        partition->address!=esp_ota_get_running_partition()->address ? partition->size : 0,data);
      configuredConnection=generation;otaPublish(true);
    }
  }
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
    if (otaBusy()) {
      statusLed.setPixelColor(0,statusLed.Color(24,12,0));statusLed.show();
    } else setDeviceState(deviceConnected.load() ? DeviceState::CONNECTED_IDLE : DeviceState::DISCONNECTED,ErrorCode::NONE);
  }
  if (otaSession.state==Synap::COMMITTED) {
    if (!rebootAt) rebootAt=millis();
    if (uint32_t(millis()-rebootAt)>1500) ESP.restart();
  }
}
// END EMBEDDED SynapOTA.h


void setDeviceState(DeviceState state, ErrorCode error) {
  deviceState = state;
  errorCode = error;
  uint8_t r=0,g=0,b=0;
  if (state == DeviceState::DISCONNECTED) r=24;
  if (state == DeviceState::CONNECTED_IDLE) b=24;
  if (state == DeviceState::STREAMING) g=24;
  if (state == DeviceState::ERROR) { r=24; b=24; }
  statusLed.setPixelColor(0, statusLed.Color(r,g,b));
  statusLed.show();
}
void updateStatusCharacteristic(bool notify) {
  if (!controlCharacteristic) return;
  uint8_t value[16] = { STATUS_PACKET_MAGIC, PROTOCOL_VERSION,
    static_cast<uint8_t>(deviceState), static_cast<uint8_t>(errorCode) };
  value[4]=peerMtu & 255; value[5]=peerMtu >> 8;
  value[6]=attValueCapacity & 255; value[7]=attValueCapacity >> 8;
  value[8]=chunksPerFrame; value[9]=AUDIO_HEADER_BYTES;
  value[10]=SAMPLE_RATE & 255; value[11]=SAMPLE_RATE >> 8;
  value[12]=SAMPLES_PER_FRAME & 255; value[13]=SAMPLES_PER_FRAME >> 8;
  value[14]=audioPayloadBytes & 255; value[15]=audioPayloadBytes >> 8;
  controlCharacteristic->setValue(value, sizeof(value));
  if (notify && deviceConnected.load()) controlCharacteristic->notify();
}
void stopStreaming(ErrorCode reason) {
  streamingEnabled.store(false);
  ++streamGeneration; // Invalidates queued AND already-in-flight old task work.
  if (audioFrameQueue) xQueueReset(audioFrameQueue);
  if (!deviceConnected.load()) setDeviceState(DeviceState::DISCONNECTED, ErrorCode::NONE);
  else if (reason == ErrorCode::NONE) setDeviceState(DeviceState::CONNECTED_IDLE, reason);
  else setDeviceState(DeviceState::ERROR, reason);
  updateStatusCharacteristic(true);
}
bool configureTransportFromPeerMtu() {
  if (!deviceConnected.load() || !bleServer) return false;
  peerMtu = bleServer->getPeerMTU(bleServer->getConnId());
  if (peerMtu < 23) peerMtu = 23;
  attValueCapacity = peerMtu - 3;
  audioPayloadBytes = 0; chunksPerFrame = 0;
  if (peerMtu < MIN_REQUIRED_MTU) return false;
  const uint16_t available = attValueCapacity - AUDIO_HEADER_BYTES;
  const uint16_t bounded = available < MAX_AUDIO_PAYLOAD_BYTES ? available : MAX_AUDIO_PAYLOAD_BYTES;
  chunksPerFrame = (AUDIO_BYTES_PER_FRAME + bounded - 1) / bounded;
  if (chunksPerFrame < MIN_CHUNKS_PER_FRAME) chunksPerFrame = MIN_CHUNKS_PER_FRAME;
  if (chunksPerFrame > MAX_CHUNKS_PER_FRAME) return false;
  audioPayloadBytes = (AUDIO_BYTES_PER_FRAME + chunksPerFrame - 1) / chunksPerFrame;
  return audioPayloadBytes + AUDIO_HEADER_BYTES <= attValueCapacity;
}
void startStreaming(uint8_t version) {
  if (otaBusy()) { updateStatusCharacteristic(true); return; }
  if (!deviceConnected.load()) return;
  if (version != PROTOCOL_VERSION) { stopStreaming(ErrorCode::PROTOCOL_MISMATCH); return; }
  // Repeated START is idempotent; it must not reset an active take's sequence.
  if (streamingEnabled.load()) { updateStatusCharacteristic(true); return; }
#if defined(CONFIG_BLUEDROID_ENABLED)
  if (!audioCccd || !audioCccd->getNotifications()) {
    stopStreaming(ErrorCode::AUDIO_NOT_SUBSCRIBED); return;
  }
#endif
#if USE_REAL_I2S_MIC
  if (!microphoneReady) { stopStreaming(ErrorCode::AUDIO_SOURCE_FAILED); return; }
#endif
  if (!configureTransportFromPeerMtu()) { stopStreaming(ErrorCode::MTU_TOO_SMALL); return; }
  xQueueReset(audioFrameQueue);
  ++streamGeneration;
  capturedFrames=0; captureDrops=0; notifyAccepted=0; notifyRejected=0;
  setDeviceState(DeviceState::STREAMING, ErrorCode::NONE);
  streamingEnabled.store(true);
  updateStatusCharacteristic(true);
  Serial.printf("[START] generation=%lu MTU=%u chunks=%u payload=%u\n",
    static_cast<unsigned long>(streamGeneration.load()), peerMtu.load(), chunksPerFrame.load(), audioPayloadBytes.load());
}
void queueEvent(EventType type, uint8_t command, uint8_t version, uint32_t stream) {
  const ControlMessage message = {type, command, version, connectionGeneration.load(), stream};
  if (xQueueSend(controlQueue, &message, 0) != pdTRUE) ++controlDrops;
}
void requestStreamError(ErrorCode error, uint32_t generation) {
  queueEvent(EventType::STREAM_ERROR, static_cast<uint8_t>(error), PROTOCOL_VERSION, generation);
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    (void)server;
    ++connectionGeneration;
    streamingEnabled.store(false);
    deviceConnected.store(true);
    queueEvent(EventType::CONNECTED, 0, PROTOCOL_VERSION, streamGeneration.load());
  }
  void onDisconnect(BLEServer* server) override {
    (void)server;
    deviceConnected.store(false);
    streamingEnabled.store(false);
    ++connectionGeneration;
    queueEvent(EventType::DISCONNECTED, 0, PROTOCOL_VERSION, streamGeneration.load());
  }
};
class ControlCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const size_t length = characteristic->getLength();
    const uint8_t* data = characteristic->getData();
    const uint8_t command = length == 2 && data ? data[0] : 0xFF;
    const uint8_t version = length == 2 && data ? data[1] : 0;
    // NO notify, LED write, delay, or logging in this synchronous GATT callback.
    queueEvent(EventType::COMMAND, command, version, streamGeneration.load());
  }
};
class AudioCallbacks : public BLECharacteristicCallbacks {
  void onStatus(BLECharacteristic* characteristic, Status status, uint32_t code) override {
    (void)characteristic; (void)code;
    if (status == SUCCESS_NOTIFY) ++notifyAccepted; else ++notifyRejected;
  }
};

void processCommand(uint8_t command, uint8_t version) {
  if (!deviceConnected.load()) return;
  if (otaBusy()) { updateStatusCharacteristic(true); return; }
  if (version != PROTOCOL_VERSION) { stopStreaming(ErrorCode::PROTOCOL_MISMATCH); return; }
  switch (command) {
    case CMD_START: startStreaming(version); break;
    case CMD_STOP:
      stopStreaming();
      Serial.println("[STOP] acknowledged; LED=blue; state=idle");
      break;
    case CMD_GET_STATUS:
      if (!streamingEnabled.load()) {
        if (configureTransportFromPeerMtu()) setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
        else setDeviceState(DeviceState::ERROR, ErrorCode::MTU_TOO_SMALL);
      }
      updateStatusCharacteristic(true);
      break;
    default: stopStreaming(ErrorCode::BAD_COMMAND); break;
  }
}
void controlTask(void* parameter) {
  (void)parameter;
  ControlMessage message;
  for (;;) {
    if (xQueueReceive(controlQueue, &message, pdMS_TO_TICKS(10)) == pdTRUE) {
      if (message.connection != connectionGeneration.load()) continue;
      switch (message.type) {
        case EventType::CONNECTED:
          restartAdvertising=false;
          peerMtu=23; attValueCapacity=20; chunksPerFrame=0; audioPayloadBytes=0;
          stopStreaming();
          Serial.println("[BLE] connected; LED=blue");
          break;
        case EventType::DISCONNECTED:
          stopStreaming();
          disconnectedAt=millis(); restartAdvertising=true;
          Serial.println("[BLE] disconnected; LED=red");
          break;
        case EventType::COMMAND:
          processCommand(message.command, message.version);
          break;
        case EventType::STREAM_ERROR:
          if (streamingEnabled.load() && message.stream == streamGeneration.load()) {
            stopStreaming(static_cast<ErrorCode>(message.command));
          }
          break;
      }
    }
    // Recovery even if a disconnect event could not fit in the control queue.
    if (!deviceConnected.load() && deviceState != DeviceState::DISCONNECTED) {
      stopStreaming(); disconnectedAt=millis(); restartAdvertising=true;
    }
    if (deviceConnected.load() && streamingEnabled.load() &&
        bleServer->getPeerMTU(bleServer->getConnId()) != peerMtu) {
      stopStreaming(ErrorCode::TRANSPORT_CHANGED);
    }
#if defined(CONFIG_BLUEDROID_ENABLED)
    if (restartAdvertising && !deviceConnected.load() && millis()-disconnectedAt > 250) {
      restartAdvertising=false;
      bleServer->startAdvertising();
    }
#endif
    otaTick();
  }
}

bool acquireAudioFrame(AudioFrame& frame) {
#if USE_REAL_I2S_MIC
  static int32_t raw[SAMPLES_PER_FRAME];
  size_t received=0;
  while (received < sizeof(raw)) {
    if (!streamingEnabled.load() || frame.generation != streamGeneration.load()) return false;
    const size_t count = microphoneI2S.readBytes(
      reinterpret_cast<char*>(raw)+received, sizeof(raw)-received);
    if (!count) return false;
    received += count;
  }
  for (uint16_t i=0; i<SAMPLES_PER_FRAME; ++i) {
    int32_t sample=raw[i] >> 14;
    if (sample > 32767) sample=32767;
    if (sample < -32768) sample=-32768;
    frame.samples[i]=static_cast<int16_t>(sample);
  }
#else
  const float increment=2.0f*PI*440.0f/SAMPLE_RATE;
  for (uint16_t i=0; i<SAMPLES_PER_FRAME; ++i) {
    frame.samples[i]=static_cast<int16_t>(sinf(tonePhase)*9000.0f);
    tonePhase+=increment;
    if (tonePhase >= 2.0f*PI) tonePhase-=2.0f*PI;
  }
#endif
  return true;
}
void acquisitionTask(void* parameter) {
  (void)parameter;
  AudioFrame frame;
  uint32_t generation=0;
  uint16_t nextSequence=0;
#if !USE_REAL_I2S_MIC
  TickType_t wake=xTaskGetTickCount();
#endif
  for (;;) {
    if (!streamingEnabled.load() || !deviceConnected.load()) {
      vTaskDelay(pdMS_TO_TICKS(10));
#if !USE_REAL_I2S_MIC
      wake=xTaskGetTickCount();
#endif
      continue;
    }
    frame.generation=streamGeneration.load();
    if (generation != frame.generation) { generation=frame.generation; nextSequence=0; }
    const bool acquired=acquireAudioFrame(frame);
    if (!streamingEnabled.load() || frame.generation != streamGeneration.load()) continue;
    if (!acquired) { requestStreamError(ErrorCode::AUDIO_SOURCE_FAILED, frame.generation); continue; }
    frame.sequence=nextSequence++;
    ++capturedFrames;
    if (xQueueSend(audioFrameQueue, &frame, 0) != pdTRUE) ++captureDrops;
#if !USE_REAL_I2S_MIC
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(FRAME_DURATION_MS));
#endif
  }
}
bool sendAudioFrame(const AudioFrame& frame, uint16_t sequence) {
  const uint8_t chunks=chunksPerFrame;
  const uint16_t payload=audioPayloadBytes, capacity=attValueCapacity;
  if (chunks < 10 || chunks > 20 || !payload || payload > 160) return false;
  uint8_t packet[AUDIO_HEADER_BYTES+MAX_AUDIO_PAYLOAD_BYTES];
  const uint8_t* pcm=reinterpret_cast<const uint8_t*>(frame.samples);
  const uint32_t started=micros();
  for (uint8_t index=0; index<chunks; ++index) {
    if (!streamingEnabled.load() || !deviceConnected.load() ||
        frame.generation != streamGeneration.load()) return false;
    const uint16_t offset=index*payload;
    const uint16_t remaining=AUDIO_BYTES_PER_FRAME-offset;
    const uint16_t length=remaining < payload ? remaining : payload;
    if (AUDIO_HEADER_BYTES+length > capacity) return false;
    packet[0]=AUDIO_PACKET_MAGIC; packet[1]=PROTOCOL_VERSION;
    packet[2]=sequence & 255; packet[3]=sequence >> 8;
    packet[4]=index; packet[5]=chunks; packet[6]=length & 255; packet[7]=length >> 8;
    memcpy(packet+AUDIO_HEADER_BYTES, pcm+offset, length);
    audioCharacteristic->setValue(packet, AUDIO_HEADER_BYTES+length);
    audioCharacteristic->notify();
    const uint32_t target=started+static_cast<uint32_t>(index+1)*50000UL/chunks;
    while (static_cast<int32_t>(target-micros()) > 1000) vTaskDelay(1);
    while (static_cast<int32_t>(target-micros()) > 0) delayMicroseconds(50);
  }
  return frame.generation == streamGeneration.load();
}
void transmitterTask(void* parameter) {
  (void)parameter;
  AudioFrame frame;
  uint32_t generation=0, sent=0;
  for (;;) {
    if (xQueueReceive(audioFrameQueue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    if (!streamingEnabled.load() || frame.generation != streamGeneration.load()) continue;
    if (generation != frame.generation) { generation=frame.generation; sent=0; }
    if (sendAudioFrame(frame, frame.sequence)) {
      ++sent;
      if (sent % 100 == 0) Serial.printf("[AUDIO] frames=%lu captured=%lu dropped=%lu notifyOK=%lu notifyFail=%lu controlDrop=%lu\n",
        static_cast<unsigned long>(sent), static_cast<unsigned long>(capturedFrames.load()),
        static_cast<unsigned long>(captureDrops.load()), static_cast<unsigned long>(notifyAccepted.load()),
        static_cast<unsigned long>(notifyRejected.load()), static_cast<unsigned long>(controlDrops.load()));
    } else if (streamingEnabled.load() && generation == streamGeneration.load()) {
      requestStreamError(ErrorCode::TRANSPORT_CHANGED, generation);
    }
  }
}

void initializeBLE() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(REQUESTED_MTU);
  bleServer=BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());
#if defined(CONFIG_NIMBLE_ENABLED)
  bleServer->advertiseOnDisconnect(true);
#endif
  // Audio/control + device ID + OTA/status/build identity exceed Bluedroid's default
  // 15-handle service reservation. NimBLE accepts this overload as well.
  BLEService* service=bleServer->createService(BLEUUID(SERVICE_UUID),32);
  audioCharacteristic=service->createCharacteristic(AUDIO_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  audioCharacteristic->setCallbacks(new AudioCallbacks());
  controlCharacteristic=service->createCharacteristic(CONTROL_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
  controlCharacteristic->setCallbacks(new ControlCallbacks());
#if defined(CONFIG_BLUEDROID_ENABLED)
  audioCccd=new BLE2902();
  audioCharacteristic->addDescriptor(audioCccd);
  controlCharacteristic->addDescriptor(new BLE2902());
#endif
  // NimBLE creates CCCDs itself. BLE2902::getNotifications() is NOT a
  // subscription test under NimBLE; do not use it to gate START.
  auto* deviceIdentity = service->createCharacteristic(DEVICE_ID_UUID, BLECharacteristic::PROPERTY_READ);
  deviceIdentity->setValue(synapDeviceId);
  updateStatusCharacteristic(false);
  otaInitialize(service);
  service->start();
  BLEAdvertising* advertising=BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  advertising->start();
  Serial.println("[BLE] dk-pendant advertising; release 1.0.0 / audio protocol 2 / device-ID OTA protocol 3");
}
void fatalSetup(const char* message) {
  Serial.println(message);
  setDeviceState(DeviceState::ERROR, ErrorCode::AUDIO_SOURCE_FAILED);
  for (;;) delay(1000);
}
void setup() {
  Serial.begin(115200);
  delay(400);
  statusLed.begin();
  setDeviceState(DeviceState::DISCONNECTED, ErrorCode::NONE);
#if USE_REAL_I2S_MIC
  microphoneI2S.setPins(I2S_BCLK_PIN, I2S_WS_PIN, -1, I2S_DATA_IN_PIN);
  microphoneI2S.setTimeout(200);
  microphoneReady=microphoneI2S.begin(I2S_MODE_STD, SAMPLE_RATE,
    I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);
  Serial.println(microphoneReady ? "[MIC] ready" : "[MIC] initialization failed");
#else
  Serial.println("[AUDIO] simulated 440 Hz; no microphone required");
#endif
  audioFrameQueue=xQueueCreate(4, sizeof(AudioFrame));
  controlQueue=xQueueCreate(12, sizeof(ControlMessage));
  if (!audioFrameQueue || !controlQueue) fatalSetup("[FATAL] queue allocation failed");
  uint8_t factoryMac[6];
  if (esp_efuse_mac_get_default(factoryMac) != ESP_OK) fatalSetup("[FATAL] device identity unavailable");
  snprintf(synapDeviceId, sizeof(synapDeviceId), "SYNAP-%02X%02X%02X%02X%02X%02X",
    factoryMac[0], factoryMac[1], factoryMac[2], factoryMac[3], factoryMac[4], factoryMac[5]);
  Serial.printf("[DEVICE] %s\n", synapDeviceId);
  initializeBLE();
  if (xTaskCreatePinnedToCore(controlTask, "control", 8192, nullptr, 3, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(acquisitionTask, "capture", 4096, nullptr, 2, nullptr, 0) != pdPASS ||
      xTaskCreatePinnedToCore(transmitterTask, "transmit", 4096, nullptr, 2, nullptr, 1) != pdPASS) {
    fatalSetup("[FATAL] task allocation failed");
  }
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  // Basic startup validation only. Stock Arduino bootloaders usually disable rollback.
#if USE_REAL_I2S_MIC
  if (microphoneReady)
#endif
  {
    const esp_err_t result=esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("[OTA] startup validation result=%d\n",int(result));
  }
#endif
}
void loop() { delay(20); }
