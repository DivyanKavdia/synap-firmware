// Synap pendant firmware for ESP32-S3FH4R2. Wiring and build settings: README.md.
#include <Arduino.h>
#include <BLEDevice.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <Preferences.h>
#if CONFIG_IDF_TARGET_ESP32S3
#include <driver/rtc_io.h>
#endif
#include <BLEServer.h>
#include <BLEUtils.h>
#if defined(CONFIG_BLUEDROID_ENABLED)
#include <BLE2902.h>
#endif
#include <Adafruit_NeoPixel.h>
#include <atomic>

#ifndef USE_REAL_I2S_MIC
#define USE_REAL_I2S_MIC 1
#endif
#if USE_REAL_I2S_MIC
#include <ESP_I2S.h>
#include <freertos/semphr.h>
I2SClass microphoneI2S;
bool microphoneReady = false;
std::atomic<bool> microphoneValidated{false};
StaticSemaphore_t microphoneMutexStorage;
SemaphoreHandle_t microphoneMutex = nullptr;

// Capture recovery calls start/stop while already holding this lock.
class MicrophoneGuard {
 public:
  MicrophoneGuard() { xSemaphoreTakeRecursive(microphoneMutex, portMAX_DELAY); }
  ~MicrophoneGuard() { xSemaphoreGiveRecursive(microphoneMutex); }
  MicrophoneGuard(const MicrophoneGuard&) = delete;
  MicrophoneGuard& operator=(const MicrophoneGuard&) = delete;
};
#else
#include <math.h>
#endif

#define DEVICE_NAME "synap"
#define DEVICE_ID_UUID "4fa1234c-0000-1000-8000-00805f9b34fb"
#define DIAGNOSTICS_UUID "4fa1234d-0000-1000-8000-00805f9b34fb"
// Public board identity, independent of firmware version, NVS and OTA authorization.
char synapDeviceId[19] = {};
#define SERVICE_UUID "4fa12345-0000-1000-8000-00805f9b34fb"
#define AUDIO_CHAR_UUID "4fa12346-0000-1000-8000-00805f9b34fb"
#define CONTROL_CHAR_UUID "4fa12347-0000-1000-8000-00805f9b34fb"
#define EVENT_CHAR_UUID "4fa1234e-0000-1000-8000-00805f9b34fb"

constexpr uint8_t PROTOCOL_VERSION = 2;
constexpr uint8_t AUDIO_PACKET_MAGIC = 0xA5;
constexpr uint8_t AUDIO_PROTOCOL_VERSION = 3;
constexpr uint8_t AUDIO_CODEC_IMA_ADPCM = 1;
constexpr uint8_t STATUS_PACKET_MAGIC = 0x5A;
constexpr uint8_t DIAGNOSTICS_MAGIC = 0xD6;
constexpr uint8_t DIAGNOSTICS_VERSION = 1;
constexpr uint8_t CMD_STOP = 0x00;
constexpr uint8_t CMD_START = 0x01;
constexpr uint8_t CMD_GET_STATUS = 0x02;
constexpr uint8_t CMD_STANDBY = 0x03;
constexpr uint8_t CMD_WAKE = 0x04;
constexpr uint8_t POWER_EVENT_MAGIC = 0xE2;
constexpr uint8_t POWER_EVENT_VERSION = 1;
constexpr uint8_t POWER_STATE_AWAKE = 1;
constexpr uint8_t POWER_STATE_STANDBY = 2;
constexpr uint8_t POWER_STATE_DEEP_SLEEP = 3;
constexpr uint32_t SYNAP_DEEP_SLEEP_MARKER = 0x53594E50u;
constexpr uint16_t WAKE_TAP_MIN_MS = 60;
constexpr uint16_t WAKE_TAP_MAX_MS = 500;
constexpr uint16_t WAKE_TAP_GAP_MS = 550;
constexpr uint16_t WAKE_TRIPLE_WINDOW_MS = 1600;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint16_t FRAME_DURATION_MS = 50;
constexpr uint16_t SAMPLES_PER_FRAME = 800;
constexpr uint16_t AUDIO_BYTES_PER_FRAME = 1600;
constexpr uint16_t ADPCM_HEADER_BYTES = 4;
constexpr uint16_t ADPCM_BYTES_PER_FRAME = ADPCM_HEADER_BYTES + (SAMPLES_PER_FRAME / 2);
constexpr uint16_t TRANSPORT_BYTES_PER_FRAME = ADPCM_BYTES_PER_FRAME;
constexpr uint8_t AUDIO_HEADER_BYTES = 8;
static_assert(SAMPLES_PER_FRAME % 2 == 0, "ADPCM frame requires an even PCM sample count");
static_assert(ADPCM_BYTES_PER_FRAME == 404, "Synap protocol-v3 ADPCM frame size");
constexpr uint8_t MIN_CHUNKS_PER_FRAME = 1;
constexpr uint8_t MAX_CHUNKS_PER_FRAME = 20;
constexpr uint16_t MIN_REQUIRED_MTU = 32;
constexpr uint16_t REQUESTED_MTU = 517;
constexpr uint16_t MAX_AUDIO_PAYLOAD_BYTES = 500;
constexpr uint8_t RGB_LED_PIN = 48;
#ifndef SYNAP_TOUCH_PIN
#define SYNAP_TOUCH_PIN 13
#endif
#ifndef SYNAP_TOUCH_ACTIVE_LEVEL
#define SYNAP_TOUCH_ACTIVE_LEVEL HIGH
#endif
#ifndef SYNAP_BATTERY_ADC_PIN
#define SYNAP_BATTERY_ADC_PIN 8
#endif
constexpr uint8_t TOUCH_INPUT_PIN = SYNAP_TOUCH_PIN;
constexpr uint8_t TOUCH_ACTIVE_LEVEL = SYNAP_TOUCH_ACTIVE_LEVEL;
constexpr uint8_t BATTERY_ADC_PIN = SYNAP_BATTERY_ADC_PIN;
constexpr uint16_t TOUCH_DEBOUNCE_MS = 35;
constexpr uint32_t AUTO_SLEEP_DISCONNECTED_MS = 300000u;
constexpr uint32_t BATTERY_SAMPLE_MS = 15000u;
constexpr uint32_t BATTERY_DIVIDER_TOP_OHMS = 1000000u;
constexpr uint32_t BATTERY_DIVIDER_BOTTOM_OHMS = 470000u;
constexpr uint16_t BATTERY_LOW_MV = 3600;
constexpr uint16_t BATTERY_CRITICAL_MV = 3400;
constexpr uint8_t BATTERY_EVENT_MAGIC = 0xB7;
constexpr uint8_t BATTERY_EVENT_VERSION = 2;
// TTP223 OUT is a digital, active-high push-pull signal by default.
// Battery sensing assumes B+ -> 1 MOhm -> GPIO8 -> 470 kOhm -> GND, with
// 100 nF from GPIO8 to GND. Implausible readings are reported but marked unavailable.
// Status LED is intentionally off most of the time. Short, dim pulses make the
// state visible without turning the onboard WS2812 into a material battery load.
constexpr uint8_t LED_DIM = 4;
constexpr int8_t I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6;
#if CONFIG_IDF_TARGET_ESP32S3
constexpr uint32_t IDLE_CPU_MHZ = 80, ACTIVE_CPU_MHZ = 240;
#elif CONFIG_IDF_TARGET_ESP32C3
constexpr uint32_t IDLE_CPU_MHZ = 80, ACTIVE_CPU_MHZ = 160;
#else
constexpr uint32_t IDLE_CPU_MHZ = 80, ACTIVE_CPU_MHZ = 160;
#endif

enum class DeviceState : uint8_t { DISCONNECTED=0, CONNECTED_IDLE=1, STREAMING=2, ERROR=3 };
enum class ErrorCode : uint8_t {
  NONE=0, MTU_TOO_SMALL=1, AUDIO_NOT_SUBSCRIBED=2,
  AUDIO_SOURCE_FAILED=3, PROTOCOL_MISMATCH=4, BAD_COMMAND=5, TRANSPORT_CHANGED=6
};
enum class EventType : uint8_t { COMMAND, STREAM_ERROR };
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
BLECharacteristic* eventCharacteristic = nullptr;
BLECharacteristic* diagnosticsCharacteristic = nullptr;
#if defined(CONFIG_BLUEDROID_ENABLED)
BLE2902* audioCccd = nullptr;
#endif
QueueHandle_t audioFrameQueue = nullptr, controlQueue = nullptr;
TaskHandle_t captureTaskHandle = nullptr;
std::atomic<bool> deviceConnected{false}, streamingEnabled{false};
std::atomic<bool> connectionEventPending{false}, transmitterActive{false};
std::atomic<uint32_t> connectionGeneration{0}, streamGeneration{0};
std::atomic<uint32_t> capturedFrames{0}, captureDrops{0}, notifyRejected{0}, controlDrops{0};
DeviceState deviceState = DeviceState::DISCONNECTED;
ErrorCode errorCode = ErrorCode::NONE;
std::atomic<uint16_t> peerMtu{23}, attValueCapacity{20}, audioPayloadBytes{0};
std::atomic<uint8_t> chunksPerFrame{0};
#if !USE_REAL_I2S_MIC
float tonePhase = 0;
#endif
uint32_t disconnectedAt = 0;
bool restartAdvertising = false;
bool remoteStandby = false;
bool sleepPending = false;
RTC_DATA_ATTR uint32_t synapDeepSleepMarker = 0;
RTC_DATA_ATTR uint32_t synapSleepRequestCounter = 0;
RTC_DATA_ATTR uint8_t synapLastSleepStage = 0;
bool bootSleepWasLocked = false;
esp_sleep_wakeup_cause_t bootWakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
constexpr char SYNAP_POWER_NAMESPACE[] = "synap-power";
constexpr char SYNAP_SLEEP_LOCK_KEY[] = "sleep-lock";
constexpr uint8_t SLEEP_STAGE_REQUESTED = 1;
constexpr uint8_t SLEEP_STAGE_LOCKED = 2;
constexpr uint8_t SLEEP_STAGE_GPIO_RELEASED = 3;
constexpr uint8_t SLEEP_STAGE_WAKE_ARMED = 4;
constexpr uint8_t SLEEP_STAGE_ENTERING = 5;
constexpr uint8_t SLEEP_STAGE_RESET_RECOVERY = 6;
constexpr uint8_t SLEEP_STAGE_WAKE_VALIDATING = 7;
constexpr uint8_t SLEEP_STAGE_WAKE_CONFIRMED = 8;
constexpr uint8_t SLEEP_STAGE_ABORTED = 9;

bool readDurableSleepLock() {
  Preferences prefs;
  if (!prefs.begin(SYNAP_POWER_NAMESPACE,true)) return false;
  const bool locked=prefs.getBool(SYNAP_SLEEP_LOCK_KEY,false);
  prefs.end();
  return locked;
}

bool writeDurableSleepLock(bool locked) {
  Preferences prefs;
  if (!prefs.begin(SYNAP_POWER_NAMESPACE,false)) return false;
  const bool ok=prefs.putBool(SYNAP_SLEEP_LOCK_KEY,locked)==1u;
  prefs.end();
  return ok;
}
esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;
uint32_t touchPressedAt = 0;
bool touchRawState = false, touchStableState = false;
uint32_t touchChangedAt = 0;
uint32_t lastLedPattern = UINT32_MAX;
uint32_t lastBatterySampleAt = 0, lastBatteryPublishAt = 0;
uint16_t batteryMillivolts = 0, batteryAdcMillivolts = 0, batteryAdcRaw = 0;
uint8_t batteryPercent = 0, batteryValidSamples = 0, batteryCriticalSamples = 0;
bool batteryAvailable = false;

// Explicit prototypes prevent Arduino's auto-prototyper from duplicating defaults.
void setDeviceState(DeviceState state, ErrorCode error);
void updateStatusLed(bool force = false);
void publishBatteryEvent(bool force = false);
void sampleBattery(bool force = false);
bool batteryCritical();
void enterDeepSleep(const char* reason);
void powerTick();
void pollTouchControl();
void updateStatusCharacteristic(bool notify);
void updateDiagnosticsCharacteristic();
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
  static constexpr size_t PACKET_MAX = 512;
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
    backend.abort();state=FAILED;error=reason;lastLength=0;orphanedAt=0;
  }
  void tick(uint32_t now, uint32_t connection, bool connected) {
    if (state==COMMITTED) return; // Boot selection is already committed; never claim cancellation.
    if (busy()) {
      // Preserve the flash handle and rolling hash across a short BLE interruption.
      // A matching RESUME packet explicitly binds a new BLE connection generation.
      if (!connected || connection!=owner) {
        if (!orphanedAt) orphanedAt=now ? now : 1;
        if (uint32_t(now-orphanedAt)>900000u) fail(LINK_LOST);
        return;
      }
      orphanedAt=0;
      // Screen lock/background suspension is expected on mobile. Keep the flash
      // handle and exact persisted offset long enough for the PWA to resume.
      if (uint32_t(now-last)>900000u) fail(TIMED_OUT);
    }
  }
  void packet(const uint8_t* p, size_t n, uint32_t now, uint32_t connection, bool recording) {
    if (state==COMMITTED) return;
    if (!p || n<5 || n>PACKET_MAX) { if (busy()) fail(BAD_PACKET);return; }
    const uint8_t command=p[0];const uint32_t id=u32(p+1);
    if (command==6) { // RESUME: same envelope as BEGIN; never erases or restarts flash.
      if (n!=59 || !busy() || state==COMMITTED || !id || id!=session ||
          u32(p+5)!=size || memcmp(p+9,expectedHash,32)!=0 ||
          !backend.matchesDevice(p+41)) return;
      owner=connection;last=now;orphanedAt=0;error=OK;return;
    }
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
      memcpy(expectedHash,p+9,32);owner=connection;orphanedAt=0;
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
  uint32_t owner=0,last=0,size=0,lastOffset=0,orphanedAt=0;
  size_t lastLength=0;
  uint8_t expectedHash[32]{};
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
// Unpublished USB builds use 0; CI supplies the monotonically increasing release build.
#define SYNAP_BUILD 0
#endif
static_assert(SYNAP_BUILD >= 0 && SYNAP_BUILD <= 65535, "OTA build must fit the protocol counter");
constexpr uint16_t SYNAP_FIRMWARE_BUILD = SYNAP_BUILD;
#define SYNAP_STRING_INNER(x) #x
#define SYNAP_STRING(x) SYNAP_STRING_INNER(x)
// Kept in the image and exposed over BLE for release/board verification.
static const char SYNAP_FIRMWARE_ID[] =
  "SYNAP-FW:esp32s3-fh4r2-qspi-4m:1.0.0:" SYNAP_STRING(SYNAP_BUILD);
// Target marker; the PWA verifies publisher authenticity using GitHub provenance.
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
std::atomic<bool> otaBusySnapshot{false};
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
  otaBusySnapshot.store(otaSession.busy());
  if (!otaStatusCharacteristic) return;
  uint8_t value[20];otaSession.status(value,SYNAP_FIRMWARE_BUILD);
  otaStatusCharacteristic->setValue(value,sizeof(value));
  if (notify && deviceConnected.load()) otaStatusCharacteristic->notify();
}
void otaInitialize(BLEService* service) {
  otaQueue=xQueueCreate(16,sizeof(OtaMessage));
  if (!otaQueue) fatalSetup("[OTA] queue allocation failed");
  auto* command=service->createCharacteristic(OTA_WRITE_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  command->setCallbacks(new OtaWriteCallbacks());
  otaStatusCharacteristic=service->createCharacteristic(OTA_STATUS_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  auto* identity=service->createCharacteristic("4fa1234b-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_READ);
  identity->setValue(SYNAP_FIRMWARE_ID);
#if defined(CONFIG_BLUEDROID_ENABLED)
  otaStatusCharacteristic->addDescriptor(new BLE2902());
#endif
  otaPublish(false);
}
void otaTick() {
  if (!otaQueue || !otaStatusCharacteristic) return;
  static uint32_t configuredConnection=UINT32_MAX,rebootAt=0;
  const uint32_t now=millis(),generation=connectionGeneration.load();
  const bool connected=deviceConnected.load();
  const Synap::OtaState previous=otaSession.state;
  otaSession.tick(now,generation,connected);
  if (batteryCritical() && otaBusy() && otaSession.state!=Synap::COMMITTED) {
    otaSession.fail(Synap::BUSY);
  }
  if (connected && !otaBusy()) {
    const uint16_t mtu=bleServer->getPeerMTU(bleServer->getConnId());
    const uint16_t packet=mtu>515 ? 512 : (mtu>=23 ? mtu-3 : 20);
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
  // Drain a short burst each control-loop iteration so cumulative-ACK windows do not
  // spend most of their time waiting in RAM. Flash writes remain strictly ordered.
  for (uint8_t drained=0;drained<4 && xQueueReceive(otaQueue,&message,0)==pdTRUE;drained++) {
    if (!connected || message.connection!=generation) continue;
    // Treat a confirmed critically-low battery like another busy condition: never
    // start or continue a new flash transaction when brownout margin is inadequate.
    otaSession.packet(message.data,message.length,millis(),generation,
      streamingEnabled.load() || batteryCritical());
    otaSession.tick(millis(),connectionGeneration.load(),deviceConnected.load());
    otaPublish(true);
    if (otaSession.state==Synap::FAILED || otaSession.state==Synap::COMMITTED) break;
  }
  if (otaSession.state!=previous) {
    otaPublish(true);
    if (otaBusy()) updateStatusLed(true);
    else if (!streamingEnabled.load())
      setDeviceState(deviceConnected.load() ? DeviceState::CONNECTED_IDLE : DeviceState::DISCONNECTED,ErrorCode::NONE);
  }
  if (otaSession.state==Synap::COMMITTED) {
    if (!rebootAt) rebootAt=millis();
    if (uint32_t(millis()-rebootAt)>1500) ESP.restart();
  }
}
// END EMBEDDED SynapOTA.h

static void put32le(uint8_t* p, uint32_t value) {
  p[0]=value&255;p[1]=(value>>8)&255;p[2]=(value>>16)&255;p[3]=(value>>24)&255;
}

void updateStatusLed(bool force) {
  const uint32_t now = millis();
  uint8_t r=0,g=0,b=0;
  if (otaBusy()) {
    const uint32_t phase=now%1400u;
    if (phase<55u || (phase>=180u && phase<235u)) { r=LED_DIM; g=2; }
  } else if (batteryAvailable && batteryMillivolts<=BATTERY_LOW_MV) {
    const uint32_t phase=now%5000u;
    if (phase<40u || (phase>=180u && phase<220u)) r=LED_DIM;
  } else if (deviceState == DeviceState::DISCONNECTED) {
    if (now%5000u<35u) r=LED_DIM;
  } else if (remoteStandby) {
    // BLE stays connected while mic and LED are off.
  } else if (deviceState == DeviceState::CONNECTED_IDLE) {
    if (now%6000u<30u) b=LED_DIM;
  } else if (deviceState == DeviceState::STREAMING) {
    if (now%1800u<45u) g=LED_DIM+1;
  } else {
    if (now%1200u<70u) { r=LED_DIM; b=LED_DIM; }
  }
  const uint32_t pattern=(uint32_t(r)<<16)|(uint32_t(g)<<8)|b;
  if (!force && pattern==lastLedPattern) return;
  lastLedPattern=pattern;
  statusLed.setPixelColor(0,statusLed.Color(r,g,b));
  statusLed.show();
}

void setDeviceState(DeviceState state, ErrorCode error) {
  deviceState = state;
  errorCode = error;
  updateStatusLed(true);
}

void applyCpuPowerProfile(bool active) {
  static uint32_t appliedMHz = 0;
  const uint32_t targetMHz = active ? ACTIVE_CPU_MHZ : IDLE_CPU_MHZ;
  if (appliedMHz == targetMHz) return;
  if (setCpuFrequencyMhz(targetMHz)) {
    appliedMHz = targetMHz;
    Serial.printf("[POWER] cpu=%luMHz mode=%s\n",
      static_cast<unsigned long>(targetMHz),active?"active":"idle");
  } else {
    Serial.printf("[POWER] cpu profile change to %luMHz failed\n",
      static_cast<unsigned long>(targetMHz));
  }
}

bool startMicrophone() {
#if USE_REAL_I2S_MIC
  MicrophoneGuard guard;
  if (microphoneReady) return true;
  constexpr uint8_t MIC_START_ATTEMPTS=3;
  for (uint8_t attempt=1; attempt<=MIC_START_ATTEMPTS; ++attempt) {
    if (attempt>1) {
      microphoneI2S.end();
      vTaskDelay(pdMS_TO_TICKS(25u*attempt));
    }
    microphoneI2S.setPins(I2S_BCLK_PIN, I2S_WS_PIN, -1, I2S_DATA_IN_PIN);
    microphoneI2S.setTimeout(80);
    microphoneReady=microphoneI2S.begin(I2S_MODE_STD, SAMPLE_RATE,
      I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO, I2S_STD_SLOT_LEFT);
    if (microphoneReady) {
      microphoneValidated=true;
      Serial.printf("[POWER] microphone I2S on attempt=%u\n",unsigned(attempt));
      return true;
    }
    Serial.printf("[MIC] initialization failed attempt=%u/%u\n",unsigned(attempt),unsigned(MIC_START_ATTEMPTS));
  }
  microphoneReady=false;
  return false;
#else
  return true;
#endif
}

void stopMicrophone() {
#if USE_REAL_I2S_MIC
  MicrophoneGuard guard;
  if (!microphoneReady) return;
  microphoneI2S.end();
  microphoneReady=false;
  Serial.println("[POWER] microphone I2S off");
#endif
}

uint8_t batteryPercentFromMillivolts(uint16_t mv) {
  // Production calibration: DMM 4.13 V, ADC 1.32 V, raw 1544 = full charge.
  // Interpolate between LiPo discharge anchors rather than using a linear scale.
  if (mv>=4130) return 100;
  if (mv>=4050) return 90 + uint32_t(mv-4050)*10/80;
  if (mv>=3950) return 80 + uint32_t(mv-3950)*10/100;
  if (mv>=3850) return 70 + uint32_t(mv-3850)*10/100;
  if (mv>=3780) return 60 + uint32_t(mv-3780)*10/70;
  if (mv>=3720) return 50 + uint32_t(mv-3720)*10/60;
  if (mv>=3680) return 40 + uint32_t(mv-3680)*10/40;
  if (mv>=3620) return 30 + uint32_t(mv-3620)*10/60;
  if (mv>=3550) return 20 + uint32_t(mv-3550)*10/70;
  if (mv>=3450) return 10 + uint32_t(mv-3450)*10/100;
  if (mv>=3300) return uint32_t(mv-3300)*10/150;
  return 0;
}

#ifndef SYNAP_BATTERY_MONITOR_ENABLE
#if CONFIG_IDF_TARGET_ESP32S3
#define SYNAP_BATTERY_MONITOR_ENABLE 1
#else
#define SYNAP_BATTERY_MONITOR_ENABLE 0
#endif
#endif
bool batteryCritical() {
#if SYNAP_BATTERY_MONITOR_ENABLE
  return batteryAvailable && batteryValidSamples>=3 && batteryCriticalSamples>=2 &&
    batteryMillivolts<=BATTERY_CRITICAL_MV;
#else
  return false;
#endif
}

void publishBatteryEvent(bool force) {
  if (!controlCharacteristic || !deviceConnected.load()) return;
  const uint32_t now=millis();
  if (!force && uint32_t(now-lastBatteryPublishAt)<BATTERY_SAMPLE_MS) return;
  // Include raw ADC measurements even when cell voltage is outside the trusted range.
  uint8_t value[12] = {BATTERY_EVENT_MAGIC, BATTERY_EVENT_VERSION, batteryPercent, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  if (batteryAvailable) value[3]|=0x01;
  if (batteryAvailable && batteryMillivolts<=BATTERY_LOW_MV) value[3]|=0x02;
  if (batteryCritical()) value[3]|=0x04;
  value[4]=batteryMillivolts&255;value[5]=batteryMillivolts>>8;
  value[6]=BATTERY_LOW_MV&255;value[7]=BATTERY_LOW_MV>>8;
  value[8]=batteryAdcMillivolts&255;value[9]=batteryAdcMillivolts>>8;
  value[10]=batteryAdcRaw&255;value[11]=batteryAdcRaw>>8;
  if (eventCharacteristic) {
    eventCharacteristic->setValue(value,sizeof(value));
    eventCharacteristic->notify();
  }
  // Control subscribers also receive battery telemetry.
  controlCharacteristic->setValue(value,sizeof(value));
  controlCharacteristic->notify();
  // Let the 12-byte battery notification leave before restoring the status value.
  vTaskDelay(pdMS_TO_TICKS(20));
  updateStatusCharacteristic(false);
  lastBatteryPublishAt=now;
}

void sampleBattery(bool force) {
#if !SYNAP_BATTERY_MONITOR_ENABLE
  (void)force;
  batteryAvailable=false;batteryValidSamples=0;batteryCriticalSamples=0;
  batteryMillivolts=0;batteryPercent=0;
  return;
#else
  const uint32_t now=millis();
  if (!force && uint32_t(now-lastBatterySampleAt)<BATTERY_SAMPLE_MS) return;
  lastBatterySampleAt=now;
  // High-value divider needs settling time. Throw away one conversion, then
  // average both calibrated millivolts and raw ADC counts over 16 samples.
  (void)analogRead(BATTERY_ADC_PIN);
  delayMicroseconds(1200);
  uint32_t mvTotal=0, rawTotal=0;
  for (uint8_t i=0;i<16;++i) {
    rawTotal+=analogRead(BATTERY_ADC_PIN);
    mvTotal+=analogReadMilliVolts(BATTERY_ADC_PIN);
    delayMicroseconds(250);
  }
  const uint32_t adcMv=mvTotal/16u;
  const uint32_t adcRaw=rawTotal/16u;
  batteryAdcMillivolts=uint16_t(adcMv>65535u?65535u:adcMv);
  batteryAdcRaw=uint16_t(adcRaw>65535u?65535u:adcRaw);
  // Calibrate the divider from the measured full-charge point: 1.32 V ADC =
  // 4.13 V cell (raw 1544). This is only a 0.04% correction versus the ideal
  // 1M/470k divider ratio, but makes the real hardware full point exact.
  constexpr uint32_t BATTERY_CAL_ADC_MV = 1320u;
  constexpr uint32_t BATTERY_CAL_CELL_MV = 4130u;
  const uint32_t cellMv=(adcMv*BATTERY_CAL_CELL_MV + BATTERY_CAL_ADC_MV/2u)/BATTERY_CAL_ADC_MV;
  if (cellMv>=2800u && cellMv<=4350u) {
    batteryMillivolts=uint16_t(cellMv);
    batteryPercent=batteryPercentFromMillivolts(batteryMillivolts);
    if (batteryValidSamples<255) ++batteryValidSamples;
    // A single averaged conversion is sufficient for UI availability. Critical
    // actions still require multiple corroborating samples via batteryCritical().
    batteryAvailable=batteryValidSamples>=1;
    if (batteryMillivolts<=BATTERY_CRITICAL_MV) {
      if (batteryCriticalSamples<255) ++batteryCriticalSamples;
    } else batteryCriticalSamples=0;
  } else {
    // Preserve the reconstructed voltage even when it is outside the expected
    // LiPo range. The PWA can then distinguish bad wiring/ADC from missing BLE.
    batteryAvailable=false;batteryValidSamples=0;batteryCriticalSamples=0;
    batteryMillivolts=uint16_t(cellMv>65535u?65535u:cellMv);batteryPercent=0;
  }
  Serial.printf("[BATTERY] gpio=%u raw=%u adc=%umV cell=%umV available=%u percent=%u\n",
    static_cast<unsigned>(BATTERY_ADC_PIN),static_cast<unsigned>(batteryAdcRaw),static_cast<unsigned>(batteryAdcMillivolts),
    static_cast<unsigned>(batteryMillivolts),batteryAvailable?1u:0u,static_cast<unsigned>(batteryPercent));
  if (!streamingEnabled.load()) publishBatteryEvent(true);
  updateStatusLed(true);
#endif
}

bool armTouchWakeSource() {
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_err_t wakeError=ESP_FAIL;
#if CONFIG_IDF_TARGET_ESP32S3
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  rtc_gpio_init(static_cast<gpio_num_t>(TOUCH_INPUT_PIN));
  rtc_gpio_set_direction(static_cast<gpio_num_t>(TOUCH_INPUT_PIN), RTC_GPIO_MODE_INPUT_ONLY);
  // TTP223 drives GPIO13 push-pull. Do not bias the line from the ESP while asleep.
  rtc_gpio_pullup_dis(static_cast<gpio_num_t>(TOUCH_INPUT_PIN));
  rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(TOUCH_INPUT_PIN));
  wakeError=esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(TOUCH_INPUT_PIN),1);
#elif CONFIG_IDF_TARGET_ESP32C3
  wakeError=esp_deep_sleep_enable_gpio_wakeup(1ULL<<TOUCH_INPUT_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);
#else
#error Unsupported Synap sleep target
#endif
  if (wakeError!=ESP_OK) {
    Serial.printf("[POWER] failed to arm touch wake err=%d\n",int(wakeError));
    return false;
  }
  synapLastSleepStage=SLEEP_STAGE_WAKE_ARMED;
  return true;
}

void armTouchWakeAndSleep() {
  synapDeepSleepMarker=SYNAP_DEEP_SLEEP_MARKER;
  sleepPending=true;
  if (!armTouchWakeSource()) {
    synapLastSleepStage=SLEEP_STAGE_ABORTED;
    Serial.println("[POWER] fail-closed wake arm failed; rebooting with sleep lock retained");
    delay(250);
    ESP.restart();
    return;
  }
  synapLastSleepStage=SLEEP_STAGE_ENTERING;
  Serial.printf("[POWER] deep sleep now request=%u gpio=%u\n",
    unsigned(synapSleepRequestCounter),unsigned(digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL));
  esp_deep_sleep_start();
  Serial.println("[POWER] deep sleep returned unexpectedly; rebooting fail-closed");
  delay(250);
  ESP.restart();
}

bool confirmTouchWakeTripleTap() {
  const bool durableLock=readDurableSleepLock();
  const bool sleepResume=durableLock || (synapDeepSleepMarker==SYNAP_DEEP_SLEEP_MARKER) || bootSleepWasLocked;
  const esp_sleep_wakeup_cause_t cause=esp_sleep_get_wakeup_cause();
  bootWakeCause=cause;
  bootSleepWasLocked=sleepResume;
  Serial.printf("[POWER] wake cause=%u sleepLock=%u rtcMarker=%u stage=%u request=%u\n",
    unsigned(cause),durableLock?1u:0u,
    synapDeepSleepMarker==SYNAP_DEEP_SLEEP_MARKER?1u:0u,
    unsigned(synapLastSleepStage),unsigned(synapSleepRequestCounter));
  if (!sleepResume) return true;

  bool touchWake=false;
#if CONFIG_IDF_TARGET_ESP32S3
  touchWake=(cause==ESP_SLEEP_WAKEUP_EXT0);
#elif CONFIG_IDF_TARGET_ESP32C3
  touchWake=(cause==ESP_SLEEP_WAKEUP_GPIO);
#endif
  if (!touchWake) {
    // A reset/brownout/watchdog during the shutdown path is not permission to boot BLE.
    synapLastSleepStage=SLEEP_STAGE_RESET_RECOVERY;
    synapDeepSleepMarker=SYNAP_DEEP_SLEEP_MARKER;
    sleepPending=true;
    Serial.println("[POWER] sleep lock survived a non-touch reset; returning to deep sleep before BLE");
    delay(30);
    armTouchWakeAndSleep();
    return false;
  }

  // The electrical touch wake counts as tap 1. Keep both durable and RTC locks set
  // until taps 2 and 3 are validated, so any reset during this window still fails closed.
  synapLastSleepStage=SLEEP_STAGE_WAKE_VALIDATING;
  Serial.println("[TOUCH] deep-sleep wake: tap 1/3; waiting for taps 2 and 3");
  const uint32_t firstPressedAt=millis();
  while (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) {
    if (uint32_t(millis()-firstPressedAt)>WAKE_TAP_MAX_MS+250u) {
      Serial.println("[TOUCH] wake tap too long; returning to deep sleep");
      delay(30);armTouchWakeAndSleep();return false;
    }
    delay(5);
  }

  uint8_t taps=1;
  const uint32_t windowStarted=millis();
  while (taps<3 && uint32_t(millis()-windowStarted)<WAKE_TRIPLE_WINDOW_MS) {
    const uint32_t waitStarted=millis();
    while (digitalRead(TOUCH_INPUT_PIN)!=TOUCH_ACTIVE_LEVEL) {
      if (uint32_t(millis()-waitStarted)>WAKE_TAP_GAP_MS ||
          uint32_t(millis()-windowStarted)>=WAKE_TRIPLE_WINDOW_MS) {
        Serial.printf("[TOUCH] wake sequence incomplete at %u/3; returning to deep sleep\n",unsigned(taps));
        delay(30);armTouchWakeAndSleep();return false;
      }
      delay(5);
    }

    const uint32_t pressedAt=millis();
    while (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL &&
        uint32_t(millis()-pressedAt)<=WAKE_TAP_MAX_MS) delay(5);
    const uint32_t held=uint32_t(millis()-pressedAt);
    if (held<WAKE_TAP_MIN_MS || held>WAKE_TAP_MAX_MS ||
        digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) {
      Serial.println("[TOUCH] invalid wake tap; returning to deep sleep");
      while (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) delay(5);
      delay(30);armTouchWakeAndSleep();return false;
    }
    ++taps;
    Serial.printf("[TOUCH] wake tap %u/3\n",unsigned(taps));
  }

  if (taps!=3) {
    delay(30);armTouchWakeAndSleep();return false;
  }
  if (!writeDurableSleepLock(false)) {
    Serial.println("[POWER] could not clear durable sleep lock; refusing BLE boot");
    delay(30);armTouchWakeAndSleep();return false;
  }
  synapDeepSleepMarker=0;
  sleepPending=false;
  synapLastSleepStage=SLEEP_STAGE_WAKE_CONFIRMED;
  Serial.println("[TOUCH] triple tap wake confirmed; sleep lock cleared; continuing normal boot");
  touchRawState=false;touchStableState=false;touchPressedAt=0;
  touchChangedAt=millis();
  return true;
}

void publishPowerEvent(uint8_t powerState) {
  if (!eventCharacteristic || !deviceConnected.load()) return;
  uint8_t value[6] = {POWER_EVENT_MAGIC, POWER_EVENT_VERSION, powerState,
    static_cast<uint8_t>(deviceState),
    static_cast<uint8_t>(SYNAP_FIRMWARE_BUILD & 255),
    static_cast<uint8_t>(SYNAP_FIRMWARE_BUILD >> 8)};
  eventCharacteristic->setValue(value,sizeof(value));
  eventCharacteristic->notify();
}

bool exitRemoteStandby() {
  if (!remoteStandby || sleepPending) return !sleepPending;
  if (otaBusy()) return false;
  remoteStandby=false;
  applyCpuPowerProfile(false);
  setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
  configureTransportFromPeerMtu();
  updateStatusCharacteristic(true);
  publishPowerEvent(POWER_STATE_AWAKE);
  Serial.println("[POWER] remote standby -> awake; microphone remains off until START");
  return true;
}

void enterRemoteStandby() {
  if (sleepPending) return;
  if (otaBusy()) { updateStatusCharacteristic(true); return; }
  if (streamingEnabled.load()) stopStreaming();
  remoteStandby=true;
#if USE_REAL_I2S_MIC
  stopMicrophone();
#endif
  applyCpuPowerProfile(false);
  setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
  updateStatusCharacteristic(true);
  publishPowerEvent(POWER_STATE_STANDBY);
  statusLed.clear();statusLed.show();
  Serial.println("[POWER] remote standby; BLE available, mic/I2S off");
}

void enterDeepSleep(const char* reason) {
  if (otaBusy() || streamingEnabled.load() || sleepPending) return;
  if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) return;

  const uint32_t initialReleaseAt=millis();
  while (uint32_t(millis()-initialReleaseAt)<300u) {
    if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) {
      Serial.println("[TOUCH] sleep cancelled: touch line was not released");
      return;
    }
    delay(10);
  }

  remoteStandby=false;
  sleepPending=true;
  ++synapSleepRequestCounter;
  synapLastSleepStage=SLEEP_STAGE_REQUESTED;
  synapDeepSleepMarker=SYNAP_DEEP_SLEEP_MARKER;

  // The durable lock is committed before any operation that could reset or drop BLE.
  if (!writeDurableSleepLock(true)) {
    synapLastSleepStage=SLEEP_STAGE_ABORTED;
    synapDeepSleepMarker=0;
    sleepPending=false;
    Serial.println("[POWER] durable sleep lock write failed; staying awake");
    return;
  }
  synapLastSleepStage=SLEEP_STAGE_LOCKED;
  Serial.printf("[POWER] sleep lock committed request=%u reason=%s\n",
    unsigned(synapSleepRequestCounter),reason?reason:"idle");

#if USE_REAL_I2S_MIC
  stopMicrophone();
#endif
  applyCpuPowerProfile(false);

  // Recheck the TTP223 immediately before arming the level wake source.
  const uint32_t finalReleaseAt=millis();
  while (uint32_t(millis()-finalReleaseAt)<300u) {
    if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) {
      writeDurableSleepLock(false);
      synapDeepSleepMarker=0;
      synapLastSleepStage=SLEEP_STAGE_ABORTED;
      sleepPending=false;
      Serial.println("[TOUCH] sleep cancelled: touch input changed before wake arm");
      return;
    }
    delay(10);
  }
  synapLastSleepStage=SLEEP_STAGE_GPIO_RELEASED;

  if (!armTouchWakeSource()) {
    writeDurableSleepLock(false);
    synapDeepSleepMarker=0;
    synapLastSleepStage=SLEEP_STAGE_ABORTED;
    sleepPending=false;
    Serial.println("[POWER] wake source could not be armed; sleep cancelled");
    return;
  }

  // Tell the app only after the durable lock and wake source are ready. No BLE deinit
  // is performed here; deep sleep itself tears down the radio without a reset window.
  Serial.println("[POWER] sleep pending; BLE commands locked; wake source armed");
  publishPowerEvent(POWER_STATE_DEEP_SLEEP);
  if (deviceConnected.load()) delay(90);

  // Any edge after wake arming is handled fail-closed by the boot gate.
  statusLed.clear();statusLed.show();
  synapLastSleepStage=SLEEP_STAGE_ENTERING;
  Serial.printf("[POWER] entering deep sleep request=%u battery=%umV\n",
    unsigned(synapSleepRequestCounter),unsigned(batteryMillivolts));
  esp_deep_sleep_start();

  // Deep sleep should not return. If it does, retain fail-closed semantics.
  Serial.println("[POWER] deep sleep returned unexpectedly; rebooting with sleep lock retained");
  delay(250);
  ESP.restart();
}

void powerTick() {
  sampleBattery(false);
  if (batteryCritical() && !streamingEnabled.load() && !otaBusy()) {
    enterDeepSleep("critical-battery");
    return;
  }
  if (!deviceConnected.load() && !streamingEnabled.load() && !otaBusy() &&
      disconnectedAt && uint32_t(millis()-disconnectedAt)>=AUTO_SLEEP_DISCONNECTED_MS) {
    enterDeepSleep("disconnected-timeout");
  }
}

void pollTouchControl() {
  constexpr uint16_t TOUCH_TAP_MIN_MS = 80;
  constexpr uint16_t TOUCH_TAP_MAX_MS = 450;
  constexpr uint16_t TOUCH_STATE_LOCKOUT_MS = 650;
  constexpr uint16_t AWAKE_TRIPLE_TAP_GAP_MS = 500;
  constexpr uint16_t AWAKE_TRIPLE_WINDOW_MS = 1400;
  static uint32_t touchRearmAt = 0;
  static bool lastConnectedState = false;
  static bool lastStreamingState = false;
  static bool lastStandbyState = false;
  static bool standbyAfterStop = false;
  static bool deepSleepAfterStop = false;
  static uint8_t tapCount = 0;
  static uint32_t tapSequenceStartedAt = 0;
  static uint32_t lastTapAt = 0;
  static uint32_t pendingDoubleAt = 0;
  const uint32_t now=millis();
  const bool connected=deviceConnected.load();
  const bool streaming=streamingEnabled.load();
  const bool standby=remoteStandby;
  const bool raw=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;

  if (deepSleepAfterStop && !streaming && !raw && !otaBusy()) {
    deepSleepAfterStop=false;
    enterDeepSleep("touch-triple-after-stop");
    return;
  }
  if (standbyAfterStop && !streaming && !raw && !otaBusy()) {
    standbyAfterStop=false;
    enterRemoteStandby();
    return;
  }

  if (connected!=lastConnectedState || streaming!=lastStreamingState || standby!=lastStandbyState) {
    lastConnectedState=connected;
    lastStreamingState=streaming;
    lastStandbyState=standby;
    touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
    touchPressedAt=0;
    tapCount=0;
    tapSequenceStartedAt=0;
    lastTapAt=0;
    pendingDoubleAt=0;
  }

  // A double tap remains START/STOP, but wait briefly for a possible third tap.
  if (pendingDoubleAt && !raw && uint32_t(now-pendingDoubleAt)>AWAKE_TRIPLE_TAP_GAP_MS) {
    pendingDoubleAt=0;
    tapCount=0;
    tapSequenceStartedAt=0;
    lastTapAt=0;
    touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
    if (streamingEnabled.load()) {
      standbyAfterStop=true;
      Serial.println("[TOUCH] double tap -> STOP + POWER SAVER");
      queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
    } else if (deviceConnected.load()) {
      Serial.println(remoteStandby ? "[TOUCH] double tap standby -> START" : "[TOUCH] double tap -> START");
      queueEvent(EventType::COMMAND,CMD_START,PROTOCOL_VERSION,streamGeneration.load());
    }
    return;
  }

  if (tapCount==1 && lastTapAt && uint32_t(now-lastTapAt)>AWAKE_TRIPLE_TAP_GAP_MS) {
    tapCount=0;tapSequenceStartedAt=0;lastTapAt=0;
  }

  if (raw!=touchRawState) { touchRawState=raw; touchChangedAt=now; }
  if (raw!=touchStableState && uint32_t(now-touchChangedAt)>=TOUCH_DEBOUNCE_MS) {
    touchStableState=raw;
    if (touchStableState) {
      if (static_cast<int32_t>(now-touchRearmAt)<0) {
        touchPressedAt=0;
        tapCount=0;tapSequenceStartedAt=0;lastTapAt=0;pendingDoubleAt=0;
        return;
      }
      touchPressedAt=now;
      return;
    }

    const uint32_t held=touchPressedAt ? uint32_t(now-touchPressedAt) : 0;
    touchPressedAt=0;
    if (!held) return;

    if (held<TOUCH_TAP_MIN_MS || held>TOUCH_TAP_MAX_MS || otaBusy()) {
      tapCount=0;tapSequenceStartedAt=0;lastTapAt=0;pendingDoubleAt=0;
      return;
    }

    if (!tapCount || !lastTapAt ||
        uint32_t(now-lastTapAt)>AWAKE_TRIPLE_TAP_GAP_MS ||
        uint32_t(now-tapSequenceStartedAt)>AWAKE_TRIPLE_WINDOW_MS) {
      tapCount=1;
      tapSequenceStartedAt=now;
      lastTapAt=now;
      pendingDoubleAt=0;
      return;
    }

    ++tapCount;
    lastTapAt=now;
    if (tapCount==2) {
      pendingDoubleAt=now;
      return;
    }

    if (tapCount>=3 && uint32_t(now-tapSequenceStartedAt)<=AWAKE_TRIPLE_WINDOW_MS) {
      tapCount=0;tapSequenceStartedAt=0;lastTapAt=0;pendingDoubleAt=0;
      touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
      Serial.println("[TOUCH] triple tap -> DEEP SLEEP");
      if (streamingEnabled.load()) {
        deepSleepAfterStop=true;
        queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
      } else {
        enterDeepSleep("touch-triple");
      }
    }
  }
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
void updateDiagnosticsCharacteristic() {
  if (!diagnosticsCharacteristic) return;
  uint8_t value[32] = {};
  value[0]=DIAGNOSTICS_MAGIC;value[1]=DIAGNOSTICS_VERSION;
  uint8_t flags=0;
#if USE_REAL_I2S_MIC
  flags|=0x01;
#endif
  if (deviceConnected.load()) flags|=0x02;
  if (streamingEnabled.load()) flags|=0x04;
  // The GATT read callback must not inspect the control task's mutable OTA engine.
  if (otaBusySnapshot.load()) flags|=0x08;
  if (bootSleepWasLocked) flags|=0x10;
#if CONFIG_IDF_TARGET_ESP32S3
  if (bootWakeCause==ESP_SLEEP_WAKEUP_EXT0) flags|=0x20;
#elif CONFIG_IDF_TARGET_ESP32C3
  if (bootWakeCause==ESP_SLEEP_WAKEUP_GPIO) flags|=0x20;
#endif
  value[2]=flags;value[3]=static_cast<uint8_t>(bootResetReason);
  put32le(value+4,capturedFrames.load());
  put32le(value+8,captureDrops.load());
  put32le(value+12,notifyRejected.load());
  put32le(value+16,controlDrops.load());
  put32le(value+20,ESP.getFreeHeap());
  put32le(value+24,ESP.getMinFreeHeap());
  put32le(value+28,millis()/1000u);
  diagnosticsCharacteristic->setValue(value,sizeof(value));
}
void stopStreaming(ErrorCode reason) {
  streamingEnabled.store(false);
  ++streamGeneration; // Invalidates queued AND already-in-flight old task work.
  if (audioFrameQueue) xQueueReset(audioFrameQueue);
#if USE_REAL_I2S_MIC
  stopMicrophone();
#endif
  // Acknowledge STOP only after the final in-flight notification has returned.
  while (transmitterActive.load()) vTaskDelay(1);
  applyCpuPowerProfile(false);
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
  chunksPerFrame = (TRANSPORT_BYTES_PER_FRAME + bounded - 1) / bounded;
  if (chunksPerFrame < MIN_CHUNKS_PER_FRAME) chunksPerFrame = MIN_CHUNKS_PER_FRAME;
  if (chunksPerFrame > MAX_CHUNKS_PER_FRAME) return false;
  audioPayloadBytes = (TRANSPORT_BYTES_PER_FRAME + chunksPerFrame - 1) / chunksPerFrame;
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
  if (!configureTransportFromPeerMtu()) { stopStreaming(ErrorCode::MTU_TOO_SMALL); return; }
  applyCpuPowerProfile(true);
#if USE_REAL_I2S_MIC
  if (!startMicrophone()) { stopStreaming(ErrorCode::AUDIO_SOURCE_FAILED); return; }
#endif
  xQueueReset(audioFrameQueue);
  ++streamGeneration;
  capturedFrames=0; captureDrops=0; notifyRejected=0;
  setDeviceState(DeviceState::STREAMING, ErrorCode::NONE);
  streamingEnabled.store(true);
  if (captureTaskHandle) xTaskNotifyGive(captureTaskHandle);
  updateStatusCharacteristic(true);
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
    connectionEventPending.store(true);
  }
  void onDisconnect(BLEServer* server) override {
    (void)server;
    deviceConnected.store(false);
    streamingEnabled.store(false);
    ++connectionGeneration;
    connectionEventPending.store(true);
  }
};
class ControlCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const size_t length = characteristic->getLength();
    const uint8_t* data = characteristic->getData();
    const uint8_t command = length == 2 && data ? data[0] : 0xFF;
    const uint8_t version = length == 2 && data ? data[1] : 0;
    // Defer work so synchronous GATT writes return promptly.
    queueEvent(EventType::COMMAND, command, version, streamGeneration.load());
  }
};
class AudioCallbacks : public BLECharacteristicCallbacks {
  void onStatus(BLECharacteristic* characteristic, Status status, uint32_t code) override {
    (void)characteristic; (void)code;
    if (status != SUCCESS_NOTIFY) ++notifyRejected;
  }
};
class DiagnosticsCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* characteristic) override {
    (void)characteristic;
    updateDiagnosticsCharacteristic();
  }
};

void processCommand(uint8_t command, uint8_t version) {
  if (!deviceConnected.load() || sleepPending) return;
  if (otaBusy()) { updateStatusCharacteristic(true); return; }
  if (version != PROTOCOL_VERSION) { stopStreaming(ErrorCode::PROTOCOL_MISMATCH); return; }
  switch (command) {
    case CMD_START:
      if (remoteStandby && !exitRemoteStandby()) break;
      startStreaming(version);
      if (streamingEnabled.load()) {
        publishPowerEvent(POWER_STATE_AWAKE);
      }
      break;
    case CMD_STOP:
      if (remoteStandby) updateStatusCharacteristic(true);
      else stopStreaming();
      break;
    case CMD_GET_STATUS:
      if (remoteStandby) {
        // Standby remains CONNECTED_IDLE on protocol v2.
        setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
      } else if (!streamingEnabled.load()) {
        if (configureTransportFromPeerMtu()) setDeviceState(DeviceState::CONNECTED_IDLE, ErrorCode::NONE);
        else setDeviceState(DeviceState::ERROR, ErrorCode::MTU_TOO_SMALL);
      }
      updateStatusCharacteristic(true);
      sampleBattery(true);
      break;
    case CMD_STANDBY:
      if (!streamingEnabled.load()) enterRemoteStandby();
      else updateStatusCharacteristic(true);
      break;
    case CMD_WAKE:
      exitRemoteStandby();
      break;
    default:
      stopStreaming(ErrorCode::BAD_COMMAND);
      break;
  }
}
void reconcileConnection() {
  // Link transitions cannot be lost when the command queue is full. A newer
  // transition during this work leaves the flag set for the next control tick.
  if (!connectionEventPending.exchange(false)) return;
  if (deviceConnected.load()) {
    restartAdvertising=false;
    disconnectedAt=0;
    peerMtu=23; attValueCapacity=20; chunksPerFrame=0; audioPayloadBytes=0;
    stopStreaming();
    publishPowerEvent(remoteStandby ? POWER_STATE_STANDBY : POWER_STATE_AWAKE);
    sampleBattery(true);
  } else {
    stopStreaming();
    disconnectedAt=millis();
    restartAdvertising=true;
  }
}

void controlTask(void* parameter) {
  (void)parameter;
  ControlMessage message;
  for (;;) {
    const bool received=xQueueReceive(controlQueue, &message, pdMS_TO_TICKS(10)) == pdTRUE;
    reconcileConnection();
    if (received && message.connection == connectionGeneration.load()) {
      switch (message.type) {
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
    if (deviceConnected.load() && streamingEnabled.load()) {
      uint16_t liveMtu=bleServer->getPeerMTU(bleServer->getConnId());
      if (liveMtu<23) liveMtu=23;
      if (liveMtu!=peerMtu) {
        const uint16_t liveCapacity=liveMtu-3;
        if (liveCapacity < uint16_t(AUDIO_HEADER_BYTES+audioPayloadBytes.load())) {
          stopStreaming(ErrorCode::TRANSPORT_CHANGED);
        } else {
          peerMtu=liveMtu;attValueCapacity=liveCapacity;
          updateStatusCharacteristic(true);
        }
      }
    }
#if defined(CONFIG_BLUEDROID_ENABLED)
    if (restartAdvertising && !deviceConnected.load() && millis()-disconnectedAt > 250) {
      restartAdvertising=false;
      bleServer->startAdvertising();
    }
#endif
    pollTouchControl();
    otaTick();
    powerTick();
    applyCpuPowerProfile(streamingEnabled.load() || otaBusy());
    updateStatusLed();
  }
}

#ifndef SYNAP_AUDIO_CONDITIONING_H
#define SYNAP_AUDIO_CONDITIONING_H

#include <stdint.h>

// Set to 0 for an unfiltered PCM comparison build.
#ifndef SYNAP_MIC_HPF_ENABLE
#define SYNAP_MIC_HPF_ENABLE 1
#endif

namespace SynapAudio {

// First-order 70 Hz high-pass at 16 kHz, with unity Nyquist gain.
// a = exp(-2*pi*70/16000), b = (1+a)/2, quantized to Q15.
// Q8 state avoids integer limit-cycle noise on quiet signals. Products use
// int64_t so full-scale steps cannot overflow, including on the ESP32-C3.
// This removes rumble/DC, not speech-band noise. No gain, gate or VAD is used.
class SpeechHighPass {
 public:
  void reset() { previousInputQ8_=0; previousOutputQ8_=0; initialized_=false; }

  int16_t process(int16_t sample) {
#if SYNAP_MIC_HPF_ENABLE
    const int32_t inputQ8=int32_t(sample)*256;
    if (!initialized_) {
      previousInputQ8_=inputQ8;
      initialized_=true;
      return 0;
    }
    const int64_t nextQ8=(int64_t(previousOutputQ8_)*31880 +
      (int64_t(inputQ8)-previousInputQ8_)*32324)/32768;
    previousInputQ8_=inputQ8;
    previousOutputQ8_=static_cast<int32_t>(nextQ8);
    // Symmetric rounding avoids adding a negative DC bias to quiet audio.
    int32_t output=static_cast<int32_t>((nextQ8+(nextQ8<0 ? -128 : 128))/256);
    if (output>32767) output=32767;
    if (output<-32768) output=-32768;
    return static_cast<int16_t>(output);
#else
    return sample;
#endif
  }

 private:
  int32_t previousInputQ8_=0, previousOutputQ8_=0;
  bool initialized_=false;
};

} // namespace SynapAudio

#endif

static_assert(SAMPLE_RATE==16000, "Speech high-pass requires 16 kHz PCM");

bool acquireAudioFrame(AudioFrame& frame) {
#if USE_REAL_I2S_MIC
  MicrophoneGuard guard;
  static int32_t raw[SAMPLES_PER_FRAME];
  // Capture-task ownership avoids sharing filter state with the control task.
  static SynapAudio::SpeechHighPass highPass;
  static uint32_t highPassGeneration=0;
  if (highPassGeneration!=frame.generation) {
    highPass.reset();
    highPassGeneration=frame.generation;
  }
  size_t received=0;
  uint8_t emptyReads=0;
  bool microphoneRecoveryUsed=false;
  while (received < sizeof(raw)) {
    if (!streamingEnabled.load() || frame.generation != streamGeneration.load()) return false;
    const size_t count = microphoneI2S.readBytes(
      reinterpret_cast<char*>(raw)+received, sizeof(raw)-received);
    if (!count) {
      if (++emptyReads < 3) continue;
      if (!microphoneRecoveryUsed) {
        microphoneRecoveryUsed=true;
        Serial.println("[MIC] empty I2S reads; restarting capture driver");
        stopMicrophone();
        vTaskDelay(pdMS_TO_TICKS(35));
        if (!streamingEnabled.load() || frame.generation != streamGeneration.load()) return false;
        if (startMicrophone()) { highPass.reset(); received=0; emptyReads=0; continue; }
      }
      return false;
    }
    emptyReads=0;
    received += count;
  }
  for (uint16_t i=0; i<SAMPLES_PER_FRAME; ++i) {
    const int32_t sample=raw[i] >> 16;
    frame.samples[i]=highPass.process(static_cast<int16_t>(sample));
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
      // START wakes capture immediately; idle recording needs no periodic polling.
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
static const uint16_t IMA_STEP_TABLE[89] = {
  7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,
  34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
  157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,
  724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,
  2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
  10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static const int8_t IMA_INDEX_TABLE[8] = {-1,-1,-1,-1,2,4,6,8};

uint16_t encodeImaAdpcm(const int16_t* samples, uint8_t* output) {
  int32_t predictor=samples[0];
  int32_t index=0;
  output[0]=uint8_t(predictor&255);output[1]=uint8_t((predictor>>8)&255);
  output[2]=uint8_t(index);output[3]=AUDIO_CODEC_IMA_ADPCM;
  // Each low nibble assignment initializes its byte, including the final padding nibble.
  for (uint16_t sampleIndex=1;sampleIndex<SAMPLES_PER_FRAME;++sampleIndex) {
    const int32_t step=IMA_STEP_TABLE[index];
    int32_t difference=int32_t(samples[sampleIndex])-predictor;
    uint8_t code=0;
    if (difference<0) { code=8;difference=-difference; }
    int32_t delta=step>>3;
    if (difference>=step) { code|=4;difference-=step;delta+=step; }
    if (difference>=(step>>1)) { code|=2;difference-=step>>1;delta+=step>>1; }
    if (difference>=(step>>2)) { code|=1;delta+=step>>2; }
    predictor+=(code&8)?-delta:delta;
    if (predictor>32767) predictor=32767;
    if (predictor<-32768) predictor=-32768;
    index+=IMA_INDEX_TABLE[code&7];
    if (index<0) index=0;
    if (index>88) index=88;
    const uint16_t packedIndex=ADPCM_HEADER_BYTES+((sampleIndex-1)>>1);
    if ((sampleIndex-1)&1) output[packedIndex]|=uint8_t((code&15)<<4);
    else output[packedIndex]=uint8_t(code&15);
  }
  return ADPCM_BYTES_PER_FRAME;
}

bool sendAudioFrame(const AudioFrame& frame, uint16_t sequence) {
  const uint8_t chunks=chunksPerFrame;
  const uint16_t payload=audioPayloadBytes, capacity=attValueCapacity;
  if (chunks < MIN_CHUNKS_PER_FRAME || chunks > MAX_CHUNKS_PER_FRAME ||
      !payload || payload > MAX_AUDIO_PAYLOAD_BYTES) return false;
  static uint8_t packet[AUDIO_HEADER_BYTES+MAX_AUDIO_PAYLOAD_BYTES];
  static uint8_t encoded[ADPCM_BYTES_PER_FRAME];
  const uint16_t encodedBytes=encodeImaAdpcm(frame.samples,encoded);
  if (encodedBytes!=TRANSPORT_BYTES_PER_FRAME) return false;
  const uint32_t started=micros();
  for (uint8_t index=0; index<chunks; ++index) {
    if (!streamingEnabled.load() || !deviceConnected.load() ||
        frame.generation != streamGeneration.load()) return false;
    const uint16_t offset=index*payload;
    const uint16_t remaining=encodedBytes-offset;
    const uint16_t length=remaining < payload ? remaining : payload;
    if (AUDIO_HEADER_BYTES+length > capacity) return false;
    packet[0]=AUDIO_PACKET_MAGIC; packet[1]=AUDIO_PROTOCOL_VERSION;
    packet[2]=sequence & 255; packet[3]=sequence >> 8;
    packet[4]=index; packet[5]=chunks; packet[6]=length & 255; packet[7]=length >> 8;
    memcpy(packet+AUDIO_HEADER_BYTES, encoded+offset, length);
    audioCharacteristic->setValue(packet, AUDIO_HEADER_BYTES+length);
    audioCharacteristic->notify();
    const uint32_t target=started+static_cast<uint32_t>(index+1)*45000UL/chunks;
    if (index+1 == chunks) {
      // Keep the frame rate bounded during queue catch-up, yielding all remaining time.
      while (static_cast<int32_t>(target-micros()) > 0) vTaskDelay(1);
    } else {
      while (static_cast<int32_t>(target-micros()) > 1000) vTaskDelay(1);
      while (static_cast<int32_t>(target-micros()) > 0) delayMicroseconds(50);
    }
  }
  return frame.generation == streamGeneration.load();
}
void transmitterTask(void* parameter) {
  (void)parameter;
  AudioFrame frame;
  uint32_t generation=0;
  for (;;) {
    if (xQueueReceive(audioFrameQueue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    // Claim activity before checking the session so STOP cannot miss a pending send.
    transmitterActive.store(true);
    if (streamingEnabled.load() && frame.generation == streamGeneration.load()) {
      generation=frame.generation;
      if (!sendAudioFrame(frame, frame.sequence) &&
          streamingEnabled.load() && generation == streamGeneration.load()) {
        requestStreamError(ErrorCode::TRANSPORT_CHANGED, generation);
      }
    }
    transmitterActive.store(false);
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
  // Audio/control + device ID + OTA/status/build identity + diagnostics exceed
  // Bluedroid's default service reservation. NimBLE accepts this overload as well.
  BLEService* service=bleServer->createService(BLEUUID(SERVICE_UUID),36);
  audioCharacteristic=service->createCharacteristic(AUDIO_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  audioCharacteristic->setCallbacks(new AudioCallbacks());
  controlCharacteristic=service->createCharacteristic(CONTROL_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
  controlCharacteristic->setCallbacks(new ControlCallbacks());
  // Battery and power events have a dedicated notification channel.
  eventCharacteristic=service->createCharacteristic(EVENT_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
#if defined(CONFIG_BLUEDROID_ENABLED)
  audioCccd=new BLE2902();
  audioCharacteristic->addDescriptor(audioCccd);
  controlCharacteristic->addDescriptor(new BLE2902());
  eventCharacteristic->addDescriptor(new BLE2902());
#endif
  // NimBLE creates CCCDs itself. BLE2902::getNotifications() is NOT a
  // subscription test under NimBLE; do not use it to gate START.
  auto* deviceIdentity = service->createCharacteristic(DEVICE_ID_UUID, BLECharacteristic::PROPERTY_READ);
  deviceIdentity->setValue(synapDeviceId);
  diagnosticsCharacteristic=service->createCharacteristic(DIAGNOSTICS_UUID,BLECharacteristic::PROPERTY_READ);
  diagnosticsCharacteristic->setCallbacks(new DiagnosticsCallbacks());
  updateDiagnosticsCharacteristic();
  updateStatusCharacteristic(false);
  otaInitialize(service);
  service->start();
  BLEAdvertising* advertising=BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  advertising->start();
}
void fatalSetup(const char* message) {
  Serial.println(message);
  setDeviceState(DeviceState::ERROR, ErrorCode::AUDIO_SOURCE_FAILED);
  for (;;) delay(1000);
}
void setup() {
  Serial.begin(115200);
#if USE_REAL_I2S_MIC
  microphoneMutex=xSemaphoreCreateRecursiveMutexStatic(&microphoneMutexStorage);
  if (!microphoneMutex) fatalSetup("[FATAL] microphone lock unavailable");
#endif
  bootResetReason=esp_reset_reason();
  bootWakeCause=esp_sleep_get_wakeup_cause();
  bootSleepWasLocked=readDurableSleepLock() || (synapDeepSleepMarker==SYNAP_DEEP_SLEEP_MARKER);
  if (bootSleepWasLocked) delay(20);
  else delay(400);
#if CONFIG_IDF_TARGET_ESP32S3
  if (bootWakeCause==ESP_SLEEP_WAKEUP_EXT0 || bootSleepWasLocked) {
    rtc_gpio_deinit(static_cast<gpio_num_t>(TOUCH_INPUT_PIN));
  }
#endif
  pinMode(TOUCH_INPUT_PIN, INPUT);
  touchRawState=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;
  touchStableState=touchRawState;
  touchChangedAt=millis();
  pinMode(BATTERY_ADC_PIN, INPUT);
  analogReadResolution(12);
  // GPIO8 is calibrated at 1.32 V ADC for a 4.13 V cell on the 1M/470k divider.
  // 6 dB attenuation comfortably covers the expected range while retaining resolution.
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_6db);
  statusLed.begin();
  statusLed.clear();
  statusLed.show();
  if (!confirmTouchWakeTripleTap()) return;
  disconnectedAt=millis();
  setDeviceState(DeviceState::DISCONNECTED, ErrorCode::NONE);
  sampleBattery(true);
#if USE_REAL_I2S_MIC
  microphoneValidated=startMicrophone();
  if (microphoneValidated) stopMicrophone();
#endif
  applyCpuPowerProfile(false);
  audioFrameQueue=xQueueCreate(20, sizeof(AudioFrame));
  controlQueue=xQueueCreate(12, sizeof(ControlMessage));
  if (!audioFrameQueue || !controlQueue) fatalSetup("[FATAL] queue allocation failed");
  uint8_t factoryMac[6];
  if (esp_efuse_mac_get_default(factoryMac) != ESP_OK) fatalSetup("[FATAL] device identity unavailable");
  snprintf(synapDeviceId, sizeof(synapDeviceId), "SYNAP-%02X%02X%02X%02X%02X%02X",
    factoryMac[0], factoryMac[1], factoryMac[2], factoryMac[3], factoryMac[4], factoryMac[5]);
  Serial.printf("Synap %u %s reset=%u\n", SYNAP_FIRMWARE_BUILD, synapDeviceId, unsigned(bootResetReason));
  initializeBLE();
  if (xTaskCreatePinnedToCore(controlTask, "control", 8192, nullptr, 3, nullptr, 1) != pdPASS ||
      xTaskCreatePinnedToCore(acquisitionTask, "capture", 4096, nullptr, 2, &captureTaskHandle, 0) != pdPASS ||
      xTaskCreatePinnedToCore(transmitterTask, "transmit", 8192, nullptr, 2, nullptr, 1) != pdPASS) {
    fatalSetup("[FATAL] task allocation failed");
  }
}
void loop() {
#if defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) && CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  static bool bootValidated=false;
  // Leave a newly selected image in PENDING_VERIFY long enough to prove that
  // BLE, queues and (when fitted) the microphone survive early runtime startup.
  if (!bootValidated && millis()>5000 && bleServer && audioFrameQueue && controlQueue
#if USE_REAL_I2S_MIC
      && microphoneValidated
#endif
  ) {
    const esp_err_t result=esp_ota_mark_app_valid_cancel_rollback();
    if (result==ESP_OK || result==ESP_ERR_NOT_FOUND) bootValidated=true;
    Serial.printf("[OTA] delayed boot validation result=%d\n",int(result));
  }
#endif
  delay(20);
}
