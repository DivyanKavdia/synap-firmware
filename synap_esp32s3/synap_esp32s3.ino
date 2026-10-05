#include <Arduino.h>
// SYNAP_DEVICE_PROFILE_BEGIN
// Generated from devices/catalog.json; edit the catalog and reassemble.
#define SYNAP_CHAKSHU 0
#define SYNAP_MODULE_ID 1
#define DEVICE_NAME "synap"
#define SYNAP_SUPPORTED_CAPABILITIES 121
#define SYNAP_CAP_AUDIO 1
#define SYNAP_CAP_CAMERA 2
#define SYNAP_CAP_SD 4
#define SYNAP_CAP_SETTINGS 8
#define SYNAP_CAP_TOUCH 16
#define SYNAP_CAP_BATTERY 32
#define SYNAP_CAP_STANDBY 64
#define SYNAP_CAP_VIDEO 128
#define SYNAP_CAP_SDAUDIO 256
#define SYNAP_CAP_PHOTO 512
#define SYNAP_SD_CS_PIN 9
#define SYNAP_SD_SCK_PIN 12
#define SYNAP_SD_MOSI_PIN 10
#define SYNAP_SD_MISO_PIN 11
#ifndef SYNAP_TOUCH_PIN
#define SYNAP_TOUCH_PIN 13
#endif
#ifndef SYNAP_BATTERY_ADC_PIN
#define SYNAP_BATTERY_ADC_PIN 8
#endif
#ifndef SYNAP_BATTERY_MONITOR_ENABLE
#define SYNAP_BATTERY_MONITOR_ENABLE 1
#endif
#define SYNAP_BATTERY_ENFORCE 1
#define SYNAP_BATTERY_FULL_MV 4130
#define SYNAP_BATTERY_SCALE_NUMERATOR 4130
#define SYNAP_BATTERY_SCALE_DENOMINATOR 1320
constexpr uint8_t RGB_LED_PIN = 48;
constexpr int8_t I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6;
constexpr uint32_t IDLE_CPU_MHZ = 80, ACTIVE_CPU_MHZ = 240;
// SYNAP_DEVICE_PROFILE_END
#include <BLEDevice.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include <esp_sleep.h>
#include <Preferences.h>
#if CONFIG_IDF_TARGET_ESP32S3
#include <driver/rtc_io.h>
#endif
#include <BLEServer.h>
#if defined(CONFIG_BLUEDROID_ENABLED)
#include <BLE2902.h>
#endif
#include <Adafruit_NeoPixel.h>
#include <atomic>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifndef USE_REAL_I2S_MIC
#define USE_REAL_I2S_MIC 1
#endif
#if SYNAP_CHAKSHU && !USE_REAL_I2S_MIC
#error Chakshu hardware checks require the real onboard microphone
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

#define DEVICE_ID_UUID "4fa1234c-0000-1000-8000-00805f9b34fb"
#define DIAGNOSTICS_UUID "4fa1234d-0000-1000-8000-00805f9b34fb"
// Public board identity, independent of firmware version, NVS and OTA authorization.
char synapDeviceId[19] = {};
#define SERVICE_UUID "4fa12345-0000-1000-8000-00805f9b34fb"
#define AUDIO_CHAR_UUID "4fa12346-0000-1000-8000-00805f9b34fb"
#define CONTROL_CHAR_UUID "4fa12347-0000-1000-8000-00805f9b34fb"
#define RECOVERY_CHAR_UUID "4fa1234f-0000-1000-8000-00805f9b34fb"
#define EVENT_CHAR_UUID "4fa1234e-0000-1000-8000-00805f9b34fb"

constexpr uint8_t PROTOCOL_VERSION = 2;
constexpr uint8_t AUDIO_PACKET_MAGIC = 0xA5;
constexpr uint8_t AUDIO_PROTOCOL_VERSION = 3; // Compressed compatibility stream.
constexpr uint8_t PCM_AUDIO_PROTOCOL_VERSION = 2; // Existing uncompressed PCM wire format.
constexpr uint16_t PCM_MIN_MTU = 185; // At most ten PCM notifications per 50 ms frame.
constexpr uint8_t AUDIO_CODEC_IMA_ADPCM = 1;
constexpr uint8_t STATUS_PACKET_MAGIC = 0x5A;
constexpr uint8_t DIAGNOSTICS_MAGIC = 0xD6;
constexpr uint8_t DIAGNOSTICS_VERSION = 2;
constexpr uint8_t CMD_STOP = 0x00;
constexpr uint8_t CMD_START = 0x01;
constexpr uint8_t CMD_GET_STATUS = 0x02;
constexpr uint8_t CMD_STANDBY = 0x03;
constexpr uint8_t CMD_WAKE = 0x04;
constexpr uint8_t CMD_RESTART = 0x05;
constexpr uint8_t POWER_EVENT_MAGIC = 0xE2;
constexpr uint8_t POWER_EVENT_VERSION = 1;
constexpr uint8_t POWER_STATE_AWAKE = 1;
constexpr uint8_t POWER_STATE_STANDBY = 2;
constexpr uint8_t POWER_STATE_DEEP_SLEEP = 3;
constexpr uint32_t SYNAP_DEEP_SLEEP_MARKER = 0x53594E50u;
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint16_t FRAME_DURATION_MS = 50;
constexpr uint16_t SAMPLES_PER_FRAME = 800;
constexpr uint16_t AUDIO_BYTES_PER_FRAME = 1600;
constexpr uint16_t ADPCM_HEADER_BYTES = 4;
constexpr uint16_t ADPCM_BYTES_PER_FRAME = ADPCM_HEADER_BYTES + (SAMPLES_PER_FRAME / 2);
constexpr uint8_t AUDIO_HEADER_BYTES = 8;
static_assert(SAMPLES_PER_FRAME % 2 == 0, "ADPCM frame requires an even PCM sample count");
static_assert(ADPCM_BYTES_PER_FRAME == 404, "Synap protocol-v3 ADPCM frame size");
constexpr uint8_t MIN_CHUNKS_PER_FRAME = 1;
constexpr uint8_t MAX_CHUNKS_PER_FRAME = 20;
constexpr uint16_t MIN_REQUIRED_MTU = 32;
constexpr uint16_t REQUESTED_MTU = 517;
// 1.25 ms interval units; 10 ms timeout units. These also satisfy Apple QA1931.
constexpr uint16_t BLE_MIN_INTERVAL = 12, BLE_MAX_INTERVAL = 24; // 15–30 ms
constexpr uint16_t BLE_SLAVE_LATENCY = 0, BLE_SUPERVISION_TIMEOUT = 600; // 6 s
constexpr uint16_t MAX_AUDIO_PAYLOAD_BYTES = 500;
#ifndef SYNAP_TOUCH_ACTIVE_LEVEL
#define SYNAP_TOUCH_ACTIVE_LEVEL HIGH
#endif
constexpr uint8_t TOUCH_INPUT_PIN = SYNAP_TOUCH_PIN;
constexpr uint8_t TOUCH_ACTIVE_LEVEL = SYNAP_TOUCH_ACTIVE_LEVEL;
constexpr uint8_t BATTERY_ADC_PIN = SYNAP_BATTERY_ADC_PIN;
constexpr uint16_t TOUCH_DEBOUNCE_MS = 35;
constexpr uint32_t AUTO_SLEEP_DISCONNECTED_MS = 300000u;
constexpr uint32_t BATTERY_SAMPLE_MS = 15000u;
constexpr uint16_t BATTERY_LOW_MV = 3600;
constexpr uint16_t BATTERY_CRITICAL_MV = 3400;
constexpr uint8_t BATTERY_EVENT_MAGIC = 0xB7;
constexpr uint8_t BATTERY_EVENT_VERSION = 2;
// Short, dim status pulses limit the onboard WS2812's battery load.
constexpr uint8_t LED_DIM = 4;

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
std::atomic<bool> odysseySdRecoveryActive{false}, odysseyCaptureActive{false};
std::atomic<bool> connectionEventPending{false}, transmitterActive{false};
std::atomic<uint32_t> connectionGeneration{0}, streamGeneration{0};
std::atomic<uint32_t> audioReplayGeneration{0};
std::atomic<uint32_t> capturedFrames{0}, captureDrops{0}, notifyRejected{0}, controlDrops{0};
// Retained across recording starts/reconnects, cleared only by a device reboot.
std::atomic<uint32_t> linkDisconnects{0}, lastDisconnectAt{0}, lastNotifyError{0};
std::atomic<uint16_t> lastDisconnectReason{0xFFFF}, lastNotifyStatus{0};
// Capture/transmit faults must survive command queue pressure and reconnects.
portMUX_TYPE streamErrorMux = portMUX_INITIALIZER_UNLOCKED;
ErrorCode pendingStreamError = ErrorCode::NONE;
uint32_t pendingStreamErrorGeneration = 0;
DeviceState deviceState = DeviceState::DISCONNECTED;
ErrorCode errorCode = ErrorCode::NONE;
std::atomic<uint16_t> peerMtu{23}, attValueCapacity{20}, audioPayloadBytes{0};
std::atomic<uint8_t> chunksPerFrame{0};
std::atomic<bool> pcmTransport{false};
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
std::atomic<uint32_t> connectedLedAt{0};
uint32_t lastBatterySampleAt = 0;
uint16_t batteryMillivolts = 0, batteryAdcMillivolts = 0, batteryAdcRaw = 0;
uint8_t batteryPercent = 0, batteryValidSamples = 0, batteryCriticalSamples = 0;
std::atomic<bool> batteryAvailable{false};

#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
std::atomic<bool> odysseyRecording{false}, odysseyStopRequested{false};
std::atomic<uint32_t> odysseyRecordingStartedAt{0}, odysseyRecordFaultAt{0};
std::atomic<uint32_t> odysseySdSleepGuardUntil{0};
void odysseyToggleRecording();
bool odysseyPrepareForConnectedStreaming(uint32_t timeoutMs);
bool odysseyPrepareSdForPowerTransition(uint32_t timeoutMs);
namespace OdysseyTransfer {
void initialize();
void ble(BLEService* service);
bool available();
}
#endif

// Explicit prototypes prevent Arduino's auto-prototyper from duplicating defaults.
void setDeviceState(DeviceState state, ErrorCode error);
void updateStatusLed(bool force = false);
void publishBatteryEvent();
void sampleBattery(bool force = false);
bool batteryCritical();
void enterDeepSleep(const char* reason);
void powerTick();
void pollTouchControl();
void updateStatusCharacteristic(bool notify);
void updateDiagnosticsCharacteristic();
void applyCpuPowerProfile(bool active);
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
bool sendAudioFrame(const AudioFrame& frame);
bool sendCapturedFrame(const AudioFrame& frame, uint32_t paceUs);
void initializeBLE();
void fatalSetup(const char* message);

bool startMicrophone();
void stopMicrophone();
bool otaBusy();
void otaPublish(bool notify);
void otaInitialize(BLEService* service);
void otaTick();
#if SYNAP_CHAKSHU
bool mediaBusy();
#endif

static void put32le(uint8_t* p, uint32_t value) {
  p[0]=value&255;p[1]=(value>>8)&255;p[2]=(value>>16)&255;p[3]=(value>>24)&255;
}

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
      if (n!=59 || !busy() || !id || id!=session ||
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
      size=bytes;error=OK;lastLength=0;
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
    put32le(p+4,session);put32le(p+8,offset);put32le(p+12,capacity);
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
#define SYNAP_VERSION "synap-os1-build" SYNAP_STRING(SYNAP_BUILD)
// Kept in the image and exposed over BLE for release/board verification.
static const char SYNAP_FIRMWARE_ID[] =
  "SYNAP-FW:esp32s3-fh4r2-qspi-4m:" SYNAP_VERSION ":" SYNAP_STRING(SYNAP_BUILD);
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
uint32_t otaLastActivityAt=0;
bool otaBusy() { return otaSession.busy(); } // Control task only.

bool otaNeedsActiveCpu() {
  // Keep transfer bursts fast, then release the boost while the phone is paused or absent.
  return otaBusy() && deviceConnected.load() && uint32_t(millis()-otaLastActivityAt)<1000u;
}

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
  bool statusPending=otaSession.state!=previous;
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
      configuredConnection=generation;statusPending=true;
    }
  }
  if (otaOverflow.exchange(false) && otaBusy() && otaSession.state!=Synap::COMMITTED) {
    otaSession.fail(Synap::BAD_PACKET);statusPending=true;
  }
  OtaMessage message;
  // Drain a short burst each control-loop iteration so cumulative-ACK windows do not
  // spend most of their time waiting in RAM. Flash writes remain strictly ordered.
  for (uint8_t drained=0;drained<4 && xQueueReceive(otaQueue,&message,0)==pdTRUE;drained++) {
    if (!connected || message.connection!=generation) continue;
    applyCpuPowerProfile(true);
    otaLastActivityAt=millis();
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
    // OTA BEGIN is an explicit request to take the device offline. Seal a
    // currently running C3 SD WAV before acquiring the flash partition; a BLE
    // reconnect alone must still leave that recording untouched. Validate the
    // packet envelope and device ID before stopping any user recording.
    if (message.length==59 && message.data[0]==1 && !streamingEnabled.load() &&
        odysseyRecording.load() && !batteryCritical() &&
        otaSession.state==Synap::AVAILABLE && otaSession.capacity &&
        Synap::OtaSession::u32(message.data+1)!=0 &&
        Synap::OtaSession::u32(message.data+5)>=36 &&
        Synap::OtaSession::u32(message.data+5)<=otaSession.capacity &&
        otaBackend.matchesDevice(message.data+41)) {
      Serial.println("[OTA] C3 finalizing SD recording before update");
      if (!odysseyPrepareForConnectedStreaming(2500u))
        Serial.println("[OTA] SD recording still active; keeping flash locked");
    }
#endif
    // Treat a confirmed critically-low battery like another busy condition: never
    // start or continue a new flash transaction when brownout margin is inadequate.
    otaSession.packet(message.data,message.length,millis(),generation,
      streamingEnabled.load() || batteryCritical()
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
      || odysseyRecording.load()
#endif
#if SYNAP_CHAKSHU
      || mediaBusy()
#endif
    );
    otaSession.tick(millis(),connectionGeneration.load(),deviceConnected.load());
    // Every command, including a retry, needs an ACK; it also carries capability/state changes.
    otaPublish(true);
    statusPending=false;
    if (otaSession.state==Synap::FAILED || otaSession.state==Synap::COMMITTED) break;
  }
  if (statusPending) otaPublish(true);
  if (otaSession.state!=previous) {
    if (otaBusy()) updateStatusLed(true);
    else if (!streamingEnabled.load())
      setDeviceState(deviceConnected.load() ? DeviceState::CONNECTED_IDLE : DeviceState::DISCONNECTED,ErrorCode::NONE);
  }
  if (otaSession.state==Synap::COMMITTED) {
    if (!rebootAt) rebootAt=millis();
    if (uint32_t(millis()-rebootAt)>1500) {
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
      if (!odysseyPrepareSdForPowerTransition(1000u)) return;
#endif
      ESP.restart();
    }
  }
}

// SYNAP_BOARD_FEATURES
// Versioned 20-byte descriptor fits the default ATT payload; names are display-only.
#if !SYNAP_CHAKSHU
uint8_t odysseySdDetectionState();
uint8_t odysseySdProbeState();
#endif
void encodeModuleCapabilities(uint8_t* p) {
  memset(p,0,20);p[0]=0xC7;p[1]=1;p[2]=SYNAP_MODULE_ID;p[3]=1;
  const uint16_t supported=SYNAP_SUPPORTED_CAPABILITIES;
  uint16_t ready=supported & (SYNAP_CAP_SETTINGS|SYNAP_CAP_TOUCH|SYNAP_CAP_STANDBY);
  uint16_t sensor=0;
#if USE_REAL_I2S_MIC
  if (microphoneValidated.load()) ready|=SYNAP_CAP_AUDIO;
#endif
  if (batteryAvailable) ready|=SYNAP_CAP_BATTERY;
#if SYNAP_CHAKSHU
  ChakshuMedia::Snapshot status;ChakshuMedia::copy(status);
  ready|=status.ready;
  if (status.ready&SYNAP_CAP_CAMERA) ready|=SYNAP_CAP_VIDEO|SYNAP_CAP_PHOTO;
  if ((status.ready&(SYNAP_CAP_AUDIO|SYNAP_CAP_SD))==(SYNAP_CAP_AUDIO|SYNAP_CAP_SD)) ready|=SYNAP_CAP_SDAUDIO;
  sensor=status.sensor;
  p[14]=ChakshuTransfer::requests?1:0;
  // Additive media-v1 features: paced notifications, saved photo preview,
  // Wi-Fi downloads, independent SD workers, native SD video quality profiles.
  p[16]=ChakshuTransfer::requests?31:0;
#endif
#if !SYNAP_CHAKSHU
  // Odyssey keeps the boot-probe snapshot for diagnostics. C3 additionally
  // exposes the shared media-v1 catalogue/read/delete protocol when its worker
  // is alive; SD readiness still follows the actual mounted-card state.
  p[17]=1;
  p[18]=odysseySdDetectionState();
#if CONFIG_IDF_TARGET_ESP32C3
  p[19]=odysseySdProbeState();
#endif
#if CONFIG_IDF_TARGET_ESP32C3
  if (OdysseyTransfer::available()) {
    p[14]=1;
    // C3 media feature bit 2 advertises explicit destructive FAT formatting.
    p[16]|=4;
    if (odysseySdDetectionState()==1) {
      ready|=SYNAP_CAP_SD;
      if (ready&SYNAP_CAP_AUDIO) ready|=SYNAP_CAP_SDAUDIO;
    }
  }
#endif
#endif
  ready &= supported;
  p[4]=supported&255;p[5]=supported>>8;p[6]=ready&255;p[7]=ready>>8;
  p[8]=sensor&255;p[9]=sensor>>8;p[10]=SAMPLE_RATE&255;p[11]=SAMPLE_RATE>>8;
  p[12]=uint8_t(ESP.getFlashChipSize()/(1024u*1024u));
  p[13]=uint8_t(ESP.getPsramSize()/(1024u*1024u));
}
class ModuleCapabilitiesCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* characteristic) override {
    uint8_t value[20];encodeModuleCapabilities(value);characteristic->setValue(value,sizeof(value));
  }
};
void initializeModuleCapabilities(BLEService* service) {
  auto* capability=service->createCharacteristic("4fa12350-0000-1000-8000-00805f9b34fb",BLECharacteristic::PROPERTY_READ);
  capability->setCallbacks(new ModuleCapabilitiesCallbacks());
  uint8_t value[20];encodeModuleCapabilities(value);capability->setValue(value,sizeof(value));
}
void updateStatusLed(bool force) {
  const uint32_t now = millis();
  uint8_t r=0,g=0,b=0;
  if (otaBusy()) {
    const uint32_t phase=now%1400u;
    if (phase<55u || (phase>=180u && phase<235u)) { r=LED_DIM; g=2; }
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  } else if (odysseySdRecoveryActive.load()) {
    // Amber double-pulse means the same offline double-tap is recovering SD
    // before capture (or preparing storage after a failed take).
    const uint32_t phase=now%900u;
    if (phase<120u || (phase>=240u && phase<360u)) { r=LED_DIM+4; g=LED_DIM+2; }
  } else if (odysseyCaptureActive.load()) {
    // Purple means confirmed PCM capture only: microphone started and the WAV
    // recovery header is durable. Preparation/recovery remains amber.
    const uint32_t phase=uint32_t(now-odysseyRecordingStartedAt.load())%1800u;
    if (!odysseyStopRequested.load() && phase<260u) { r=LED_DIM+4; b=LED_DIM+6; }
  } else if (odysseyRecordFaultAt.load() &&
             uint32_t(now-odysseyRecordFaultAt.load())<6000u) {
    // Two red pulses distinguish missing SD / failed capture from active
    // purple recording. Resume normal LED state after six seconds.
    const uint32_t phase=uint32_t(now-odysseyRecordFaultAt.load())%900u;
    if (phase<140u || (phase>=260u && phase<400u)) r=LED_DIM+3;
#endif
  } else if (remoteStandby) {
    // Standby stays dark; battery telemetry remains available over BLE.
  } else if (batteryAvailable && batteryMillivolts<=BATTERY_LOW_MV) {
    const uint32_t phase=now%5000u;
    if (phase<40u || (phase>=180u && phase<220u)) r=LED_DIM;
  } else if (deviceState == DeviceState::DISCONNECTED) {
    if (now%5000u<35u) r=LED_DIM;
  } else if (deviceState == DeviceState::CONNECTED_IDLE) {
    // Three visible green acknowledgements confirm the PWA/BLE connection.
    // Connected idle stays dark after the burst to conserve battery.
    const uint32_t connectedAt=connectedLedAt.load();
    const uint32_t elapsed=uint32_t(now-connectedAt);
    if (connectedAt && elapsed<1500u && elapsed%500u<180u) g=LED_DIM+5;
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
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  active=active || odysseyRecording.load();
#endif
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

// SYNAP_BATTERY_RUNTIME_BEGIN
#if CONFIG_IDF_TARGET_ESP32C3
static std::atomic<bool> odysseySdBatteryDividerObserved{false};
void markOdysseySdBatteryDividerPresent() { odysseySdBatteryDividerObserved=true; }
bool odysseySdBatteryDividerPresent() { return odysseySdBatteryDividerObserved.load(); }
#if SYNAP_BATTERY_MONITOR_ENABLE
static bool detectOdysseySdBatteryDividerFromAdc(uint32_t adcMv) {
  if (odysseySdBatteryDividerObserved.load()) return true;
  const uint32_t standardMv=(adcMv*SYNAP_BATTERY_SCALE_NUMERATOR + SYNAP_BATTERY_SCALE_DENOMINATOR/2u)/SYNAP_BATTERY_SCALE_DENOMINATOR;
  const uint32_t sdMv=(adcMv*SYNAP_SD_BATTERY_SCALE_NUMERATOR + SYNAP_SD_BATTERY_SCALE_DENOMINATOR/2u)/SYNAP_SD_BATTERY_SCALE_DENOMINATOR;
  // A healthy LiPo cannot sustain the C3 below 2.8 V. If the legacy 2:1
  // reconstruction is therefore impossible while the 1 MOhm/470 kOhm
  // reconstruction lands in the normal LiPo window, the divider itself is
  // sufficient evidence of the SD-equipped hardware even when SD init fails.
  if (standardMv<2800u && sdMv>=3300u && sdMv<=4350u) {
    odysseySdBatteryDividerObserved=true;
    return true;
  }
  return false;
}
#endif
#else
void markOdysseySdBatteryDividerPresent() {}
bool odysseySdBatteryDividerPresent() { return false; }
#endif

uint16_t batteryFullMillivolts() {
#if CONFIG_IDF_TARGET_ESP32C3
  if (odysseySdBatteryDividerPresent()) return SYNAP_SD_BATTERY_FULL_MV;
#endif
  return SYNAP_BATTERY_FULL_MV;
}

uint32_t batteryCellMillivoltsFromAdc(uint32_t adcMv) {
  uint32_t numerator=SYNAP_BATTERY_SCALE_NUMERATOR;
  uint32_t denominator=SYNAP_BATTERY_SCALE_DENOMINATOR;
#if CONFIG_IDF_TARGET_ESP32C3
  if (odysseySdBatteryDividerPresent()) {
    numerator=SYNAP_SD_BATTERY_SCALE_NUMERATOR;
    denominator=SYNAP_SD_BATTERY_SCALE_DENOMINATOR;
  }
#endif
  return (adcMv*numerator + denominator/2u)/denominator;
}

uint8_t batteryPercentFromMillivolts(uint16_t mv) {
  // Standard C3 keeps its historical 2:1 calibration. Once SD hardware is
  // positively observed, the SD-equipped C3 uses its 1 MOhm / 470 kOhm divider.
  const uint16_t fullMv=batteryFullMillivolts();
  if (mv>=fullMv) return 100;
  if (mv>=4050) return 90 + uint32_t(mv-4050)*10/(fullMv-4050);
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

bool batteryCritical() {
#if SYNAP_BATTERY_MONITOR_ENABLE && SYNAP_BATTERY_ENFORCE
  return batteryAvailable && batteryValidSamples>=3 && batteryCriticalSamples>=2 &&
    batteryMillivolts<=BATTERY_CRITICAL_MV;
#else
  return false;
#endif
}

void publishBatteryEvent() {
  if (!controlCharacteristic || !deviceConnected.load()) return;
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
#if CONFIG_IDF_TARGET_ESP32C3
  const bool dividerWasObserved=odysseySdBatteryDividerPresent();
  if (!dividerWasObserved && detectOdysseySdBatteryDividerFromAdc(adcMv)) {
    Serial.printf("[BATTERY] inferred SD divider from adc=%lumV\n",static_cast<unsigned long>(adcMv));
  }
#endif
  const uint32_t cellMv=batteryCellMillivoltsFromAdc(adcMv);
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
  if (!streamingEnabled.load()) publishBatteryEvent();
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
  // TTP223 drives the configured touch GPIO push-pull. Do not bias the line from the ESP while asleep.
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
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
    odysseyPrepareSdForPowerTransition(500u);
#endif
    delay(250);
    ESP.restart();
    return;
  }
  synapLastSleepStage=SLEEP_STAGE_ENTERING;
  Serial.printf("[POWER] deep sleep now request=%u gpio=%u\n",
    unsigned(synapSleepRequestCounter),unsigned(digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL));
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  odysseyPrepareSdForPowerTransition(500u);
#endif
  esp_deep_sleep_start();
  Serial.println("[POWER] deep sleep returned unexpectedly; rebooting fail-closed");
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  odysseyPrepareSdForPowerTransition(500u);
#endif
  delay(250);
  ESP.restart();
}

bool confirmTouchWakeGesture() {
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
    synapLastSleepStage=SLEEP_STAGE_RESET_RECOVERY;
    synapDeepSleepMarker=SYNAP_DEEP_SLEEP_MARKER;
    sleepPending=true;
    Serial.println("[POWER] sleep lock survived a non-touch reset; returning to deep sleep before BLE");
    delay(30);armTouchWakeAndSleep();return false;
  }

  // Both boards confirm a deliberate hold after hardware wake. The TTP223
  // high level performs the hardware wake; firmware then confirms the hold.
  constexpr uint16_t TOUCH_WAKE_HOLD_MS = 4000;
  synapLastSleepStage=SLEEP_STAGE_WAKE_VALIDATING;
  const uint32_t pressedAt=millis();
  Serial.println("[TOUCH] wake touch detected; hold to power on");
  while (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL &&
      uint32_t(millis()-pressedAt)<TOUCH_WAKE_HOLD_MS) delay(5);

  if (uint32_t(millis()-pressedAt)<TOUCH_WAKE_HOLD_MS) {
    Serial.println("[TOUCH] wake press too short; returning to deep sleep");
    delay(30);armTouchWakeAndSleep();return false;
  }

  // Do not continue into normal touch handling until the wake press is released.
  uint32_t releasedAt=millis();
  while (uint32_t(millis()-releasedAt)<TOUCH_DEBOUNCE_MS) {
    if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) releasedAt=millis();
    delay(5);
  }
  if (!writeDurableSleepLock(false)) {
    Serial.println("[POWER] could not clear durable sleep lock; refusing BLE boot");
    delay(30);armTouchWakeAndSleep();return false;
  }
  synapDeepSleepMarker=0;
  sleepPending=false;
  synapLastSleepStage=SLEEP_STAGE_WAKE_CONFIRMED;
  Serial.println("[TOUCH] long-press wake confirmed; sleep lock cleared; continuing normal boot");
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
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  if (odysseyRecording.load()) return;
#endif
  if (sleepPending) return;
  if (otaBusy()) { updateStatusCharacteristic(true); return; }
#if SYNAP_CHAKSHU
  if (mediaBusy()) { updateStatusCharacteristic(true); return; }
#endif
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
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  if (odysseyRecording.load()) return;
  const uint32_t sdGuardUntil=odysseySdSleepGuardUntil.load();
  if (sdGuardUntil && static_cast<int32_t>(millis()-sdGuardUntil)<0) {
    Serial.println("[POWER] deep sleep deferred: C3 SD post-record settle");
    return;
  }
#endif
  if (otaBusy() || streamingEnabled.load() || sleepPending) return;
#if SYNAP_CHAKSHU
  if (mediaBusy()) return;
#endif
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
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  if (!odysseyPrepareSdForPowerTransition(1000u)) {
    writeDurableSleepLock(false);
    synapDeepSleepMarker=0;
    synapLastSleepStage=SLEEP_STAGE_ABORTED;
    sleepPending=false;
    Serial.println("[POWER] deep sleep cancelled: C3 SD storage did not quiesce");
    return;
  }
#endif
  esp_deep_sleep_start();

  // Deep sleep should not return. If it does, retain fail-closed semantics.
  Serial.println("[POWER] deep sleep returned unexpectedly; rebooting with sleep lock retained");
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  odysseyPrepareSdForPowerTransition(500u);
#endif
  delay(250);
  ESP.restart();
}

void powerTick() {
  sampleBattery(false);
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  if (odysseyRecording.load()) {
    if (batteryCritical()) odysseyStopRequested=true;
    return;
  }
  const uint32_t sdGuardUntil=odysseySdSleepGuardUntil.load();
  if (sdGuardUntil && static_cast<int32_t>(millis()-sdGuardUntil)<0) return;
#endif
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
  constexpr uint16_t TOUCH_TAP_MIN_MS = 60;
  constexpr uint16_t TOUCH_TAP_MAX_MS = 500;
  constexpr uint16_t TOUCH_DOUBLE_TAP_GAP_MS = 550;
  constexpr uint16_t TOUCH_SLEEP_HOLD_MS = 4000;
  constexpr uint16_t TOUCH_STATE_LOCKOUT_MS = 250;
  static uint32_t touchRearmAt = 0;
  static bool lastConnectedState = false;
  static bool lastStreamingState = false;
  static bool lastStandbyState = false;
  static bool standbyAfterStop = false;
  static bool deepSleepAfterStop = false;
  static uint8_t tapCount = 0;
  static uint32_t lastTapAt = 0;
  const uint32_t now=millis();
  const bool connected=deviceConnected.load();
  const bool streaming=streamingEnabled.load();
  const bool standby=remoteStandby;
  const bool raw=digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL;

  // An OTA-interrupted press must not become a power gesture when OTA finishes.
  if (otaBusy() || sleepPending) {
    touchPressedAt=0;tapCount=0;lastTapAt=0;
    deepSleepAfterStop=false;standbyAfterStop=false;
  }

  if (deepSleepAfterStop && !streaming && !raw && !otaBusy()
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
      && !odysseyRecording.load()
#endif
  ) {
    deepSleepAfterStop=false;
    enterDeepSleep("touch-hold-after-stop");
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
    lastTapAt=0;
  }

  // A lone tap intentionally does nothing. Expire it after the double-tap window.
  if (tapCount==1 && lastTapAt && uint32_t(now-lastTapAt)>TOUCH_DOUBLE_TAP_GAP_MS) {
    tapCount=0;
    lastTapAt=0;
  }

  if (raw!=touchRawState) { touchRawState=raw; touchChangedAt=now; }
  if (raw!=touchStableState && uint32_t(now-touchChangedAt)>=TOUCH_DEBOUNCE_MS) {
    touchStableState=raw;
    if (touchStableState) {
      bool localSdRecording=false;
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
      localSdRecording=odysseyRecording.load();
#endif
      // A just-started offline take must still accept an immediate second
      // double tap to stop. The normal 250 ms lockout remains for every other action.
      if (otaBusy() || sleepPending ||
          (static_cast<int32_t>(now-touchRearmAt)<0 && !localSdRecording)) {
        touchPressedAt=0;
        tapCount=0;
        lastTapAt=0;
        return;
      }
      touchPressedAt=now;
      return;
    }

    const uint32_t held=touchPressedAt ? uint32_t(now-touchPressedAt) : 0;
    touchPressedAt=0;
    if (!held || otaBusy()) {
      tapCount=0;lastTapAt=0;return;
    }

    // Sleep is requested only after release, preventing the level-sensitive
    // wake source from immediately waking again on either board.
    if (held>=TOUCH_SLEEP_HOLD_MS) {
      tapCount=0;lastTapAt=0;
      touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
      Serial.println("[TOUCH] long press -> DEEP SLEEP");
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
      if (odysseyRecording.load()) {
        odysseyStopRequested=true;
        deepSleepAfterStop=true;
        return;
      }
#endif
      if (streamingEnabled.load()) {
        deepSleepAfterStop=true;
        queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
      } else {
        enterDeepSleep("touch-hold");
      }
      return;
    }

    if (held<TOUCH_TAP_MIN_MS || held>TOUCH_TAP_MAX_MS) {
      tapCount=0;lastTapAt=0;
      return;
    }

    if (!tapCount || !lastTapAt || uint32_t(now-lastTapAt)>TOUCH_DOUBLE_TAP_GAP_MS) {
      tapCount=1;
      lastTapAt=now;
      return;
    }

    // Second valid tap acts immediately; a third tap has no power action.
    tapCount=0;lastTapAt=0;
    touchRearmAt=now+TOUCH_STATE_LOCKOUT_MS;
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
    if (odysseyRecording.load() || (!deviceConnected.load() && !streamingEnabled.load())) {
      odysseyToggleRecording();
      return;
    }
#endif
    if (streamingEnabled.load()) {
      standbyAfterStop=true;
      Serial.println("[TOUCH] double tap -> STOP + POWER SAVER");
      queueEvent(EventType::COMMAND,CMD_STOP,PROTOCOL_VERSION,streamGeneration.load());
    } else if (deviceConnected.load()) {
      Serial.println(remoteStandby ? "[TOUCH] double tap standby -> START" : "[TOUCH] double tap -> START");
      queueEvent(EventType::COMMAND,CMD_START,PROTOCOL_VERSION,streamGeneration.load());
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
  uint8_t value[48] = {};
  value[0]=DIAGNOSTICS_MAGIC;value[1]=DIAGNOSTICS_VERSION;
  uint8_t flags=0;
#if USE_REAL_I2S_MIC
  flags|=0x01;
  flags|=0x40; // Captured PCM has no firmware DSP before transport encoding.
#endif
  if (pcmTransport.load()) flags|=0x80; // Uncompressed PCM transport selected.
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
  const uint16_t reason=lastDisconnectReason.load(), status=lastNotifyStatus.load();
  value[32]=reason&255;value[33]=reason>>8;
  value[34]=status&255;value[35]=status>>8;
  put32le(value+36,linkDisconnects.load());
  put32le(value+40,lastDisconnectAt.load());
  put32le(value+44,lastNotifyError.load());
  diagnosticsCharacteristic->setValue(value,sizeof(value));
}
// Optional recovery protocol. Buffers are volatile and owned by one app session.
namespace SynapRecovery {
using StoredFrame = AudioFrame; // Recovery always retains uncompressed PCM.
class Ring {
 public:
  StoredFrame* frames=nullptr;
  uint16_t capacity=0,head=0,count=0,cursor=0;
  void reset() { head=count=cursor=0; }
  bool push(const StoredFrame& frame) {
    if (!capacity) return false;
    bool lost=false;
    if (count==capacity) { head=(head+1)%capacity; if(cursor) --cursor; else lost=true; --count; }
    frames[(head+count)%capacity]=frame; ++count; return lost;
  }
  bool peek(StoredFrame& frame) const { if(cursor>=count) return false; frame=frames[(head+cursor)%capacity]; return true; }
  void sent(uint16_t sequence) { if(cursor<count && frames[(head+cursor)%capacity].sequence==sequence) ++cursor; }
  void after(uint16_t sequence) {
    cursor=0;
    for(uint16_t i=0;i<count;++i) if(frames[(head+i)%capacity].sequence==sequence) { cursor=i+1; return; }
  }
};
}
SynapRecovery::Ring recoveryRing;
SemaphoreHandle_t recoveryMutex=nullptr;
std::atomic<bool> recoveryEnabled{false}, recoveryWaiting{false}, recoveryFinishing{false};
// Keep Stop intent after volatile audio expires; only its owner may consume it.
std::atomic<bool> recoveryStopRequested{false};
uint32_t recoveryFinishAt=0;
std::atomic<uint32_t> recoveryWaitingAt{0};
BLECharacteristic* recoveryCharacteristic=nullptr;
uint8_t recoveryToken[8]={};
uint8_t recoveryReplayAck=0; // Protected by recoveryMutex; acknowledges connected replay.
struct RecoveryRequest { uint8_t command=0,token[8]={}; uint16_t sequence=0; uint32_t connection=0; };
RecoveryRequest recoveryRequest;
class RecoveryGuard {
 public:
  RecoveryGuard() { xSemaphoreTake(recoveryMutex,portMAX_DELAY); }
  ~RecoveryGuard() { xSemaphoreGive(recoveryMutex); }
};
void encodeImaAdpcm(const int16_t* samples, uint8_t* output);
bool sendCapturedFrame(const AudioFrame& frame, uint32_t paceUs);
bool sendEncodedFrame(uint32_t generation,uint16_t sequence,const uint8_t* encoded,uint32_t paceUs,bool pcm);

void initializeRecovery() {
  recoveryMutex=xSemaphoreCreateMutex();
  if(!recoveryMutex)return;
#if CONFIG_IDF_TARGET_ESP32S3
  recoveryRing.frames=static_cast<SynapRecovery::StoredFrame*>(heap_caps_malloc(600*sizeof(SynapRecovery::StoredFrame),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if(recoveryRing.frames)recoveryRing.capacity=600;
#endif
  // 25 PCM frames use about 40 KB, preserving the previous internal-RAM budget.
  // S3 PSRAM can retain 30 seconds; C3/no-PSRAM retains 1.25 seconds.
  if(!recoveryRing.frames && heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)>140000) {
    recoveryRing.frames=static_cast<SynapRecovery::StoredFrame*>(heap_caps_malloc(25*sizeof(SynapRecovery::StoredFrame),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if(recoveryRing.frames)recoveryRing.capacity=25;
  }
}
void resetRecovery(bool disarm=false, bool preserveStop=false);
void resetRecovery(bool disarm, bool preserveStop) {
  recoveryWaiting=false; recoveryWaitingAt=0; recoveryFinishing=false; recoveryFinishAt=0;
  if(disarm)recoveryEnabled=false;
  if(recoveryMutex) {
    RecoveryGuard guard;
    const bool keepStop=preserveStop && recoveryStopRequested.load();
    recoveryRing.reset(); recoveryReplayAck=0; recoveryStopRequested=keepStop;
    if(disarm && !keepStop)memset(recoveryToken,0,sizeof(recoveryToken));
  }
}
void retainRecoveryFrame(const AudioFrame& frame) {
  if(!recoveryEnabled.load())return;
  RecoveryGuard guard;
  if(streamingEnabled.load() && frame.generation==streamGeneration.load() && recoveryRing.push(frame)) ++captureDrops;
}
bool recoveryCanSend() {
  if(!recoveryEnabled.load() || recoveryWaiting.load() || !deviceConnected.load())return false;
  RecoveryGuard guard; return recoveryRing.cursor<recoveryRing.count;
}
bool sendRecoveryFrame() {
  const uint32_t connection=connectionGeneration.load();
  // A producer may evict the ring head while BLE is retrying its fragments.
  // Keep this frame's PCM until its last fragment is accepted. Otherwise every
  // eviction restarts chunk zero and a slow link can stop completing frames.
  static SynapRecovery::StoredFrame frame;
  static bool held=false;
  static uint32_t heldConnection=0,heldReplay=0;
  const uint32_t replay=audioReplayGeneration.load();
  uint16_t pending=0;
  {
    RecoveryGuard guard;
    if(!held || heldConnection!=connection || heldReplay!=replay || frame.generation!=streamGeneration.load()) {
      held=false;
      if(!recoveryRing.peek(frame))return false;
      held=true;heldConnection=connection;heldReplay=replay;
    }
    pending=recoveryRing.count-recoveryRing.cursor;
  }
  if(recoveryWaiting.load() || !streamingEnabled.load() || !deviceConnected.load())return false;
  const uint32_t pace=pending>4 && chunksPerFrame.load()<=5 ? 30000u : 45000u;
  const uint32_t rejectedBefore=notifyRejected.load();
  const bool sent=sendCapturedFrame(frame,pace);
  if(sent)held=false;
  if(sent && replay==audioReplayGeneration.load() && !recoveryWaiting.load() && connection==connectionGeneration.load()) { RecoveryGuard guard; recoveryRing.sent(frame.sequence); }
  else if(!sent && replay==audioReplayGeneration.load() && !recoveryWaiting.load() && deviceConnected.load() && streamingEnabled.load() && frame.generation==streamGeneration.load() && connection==connectionGeneration.load()) {
    // A full controller queue is temporary. Retain this frame for a later send;
    // advancing the cursor here would discard audio the BLE stack never accepted.
    if(notifyRejected.load()!=rejectedBefore)vTaskDelay(pdMS_TO_TICKS(30));
    else requestStreamError(ErrorCode::TRANSPORT_CHANGED,frame.generation);
  }
  return sent;
}
void processRecoveryRequest() {
  if(!recoveryMutex)return;
  RecoveryRequest request;
  { RecoveryGuard guard; request=recoveryRequest; recoveryRequest.command=0; }
  if(!request.command || request.connection!=connectionGeneration.load() || !deviceConnected.load() || otaBusy() || sleepPending)return;
  if(request.command==1 && !streamingEnabled.load() && recoveryRing.capacity) {
    RecoveryGuard guard; memcpy(recoveryToken,request.token,8); recoveryEnabled=true; recoveryRing.reset(); recoveryReplayAck=0; recoveryStopRequested=false;
  } else if(request.command==2 && recoveryEnabled.load() && recoveryWaiting.load() && streamingEnabled.load()) {
    { RecoveryGuard guard; if(memcmp(request.token,recoveryToken,8)!=0)return; }
#if defined(CONFIG_BLUEDROID_ENABLED)
    if(!audioCccd || !audioCccd->getNotifications())return;
#endif
    if(!configureTransportFromPeerMtu())return;
    { RecoveryGuard guard; recoveryRing.after(request.sequence); }
    recoveryWaitingAt=0; recoveryWaiting=false;
    setDeviceState(DeviceState::STREAMING,ErrorCode::NONE); updateStatusCharacteristic(true);
  } else if(request.command==3 && recoveryEnabled.load() && !recoveryWaiting.load() &&
            streamingEnabled.load() && !recoveryFinishing.load()) {
    // A suspended web view can lose notifications while the native BLE link
    // remains connected. Rewind retained samples without restarting capture or
    // renegotiating its format. A send already in flight can only advance its
    // own sequence (Ring::sent), never skip the rewound recovery cursor.
#if defined(CONFIG_BLUEDROID_ENABLED)
    if(!audioCccd || !audioCccd->getNotifications())return;
#endif
    RecoveryGuard guard;
    if(memcmp(request.token,recoveryToken,8)!=0)return;
    recoveryRing.after(request.sequence);
    ++recoveryReplayAck;
    ++audioReplayGeneration;
  }
}
void updateRecoveryStatus(BLECharacteristic* characteristic,bool notify) {
    uint8_t value[16]={0x52,1,0,0};
    if(recoveryMutex) {
      RecoveryGuard guard;
      value[2]=(recoveryRing.capacity?0x11:0)|(recoveryEnabled.load()?2:0)|(recoveryWaiting.load()?4:0)|(recoveryFinishing.load()?8:0)|(recoveryStopRequested.load()?0x20:0);
      value[3]=recoveryReplayAck;
      value[4]=recoveryRing.capacity&255; value[5]=recoveryRing.capacity>>8;
      const uint16_t pending=recoveryRing.count-recoveryRing.cursor;
      value[6]=pending&255; value[7]=pending>>8;
      const uint32_t generation=streamGeneration.load();
      for(uint8_t i=0;i<4;++i)value[8+i]=uint8_t(generation>>(8*i));
      uint32_t tokenHash=2166136261u;
      for(uint8_t byte:recoveryToken)tokenHash=(tokenHash^byte)*16777619u;
      for(uint8_t i=0;i<4;++i)value[12+i]=uint8_t(tokenHash>>(8*i));
    }
    characteristic->setValue(value,sizeof(value));
    if(notify && deviceConnected.load())characteristic->notify();
}
void finishBufferedRecording() {
  if(recoveryFinishing.load())return;
  recoveryStopRequested=true;
  recoveryFinishing=true;recoveryFinishAt=millis();
#if USE_REAL_I2S_MIC
  stopMicrophone();
#endif
  updateRecoveryStatus(recoveryCharacteristic,true);
}
class RecoveryCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* characteristic) override { updateRecoveryStatus(characteristic,false); }
  void onWrite(BLECharacteristic* characteristic) override {
    if(!recoveryMutex)return;
    const uint8_t* data=characteristic->getData();const size_t size=characteristic->getLength();
    if(!data || !((size==9 && data[0]==1)||(size==11 && (data[0]==2 || data[0]==3))))return;
    RecoveryGuard guard;
    recoveryRequest.command=data[0];memcpy(recoveryRequest.token,data+1,8);
    recoveryRequest.sequence=size==11 ? uint16_t(data[9])|(uint16_t(data[10])<<8) : 0;
    recoveryRequest.connection=connectionGeneration.load();
  }
};

void stopStreaming(ErrorCode reason) {
  streamingEnabled.store(false);
  ++streamGeneration; // Invalidates queued AND already-in-flight old task work.
  if (audioFrameQueue) xQueueReset(audioFrameQueue);
#if USE_REAL_I2S_MIC
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  // BLE connect/disconnect resets only the BLE audio session. The standalone
  // SD take retains I2S ownership until touch stop or explicit PWA START.
  // stopMicrophone() takes the recorder's recursive mutex and otherwise waits
  // for an entire offline take, blocking the BLE control task on reconnect.
  if (!odysseyRecording.load()) stopMicrophone();
#else
  stopMicrophone();
#endif
#endif
  // Acknowledge STOP only after the final in-flight notification has returned.
  while (transmitterActive.load()) vTaskDelay(1);
  resetRecovery(!deviceConnected.load(),true);
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
  audioPayloadBytes = 0; chunksPerFrame = 0; pcmTransport=false;
  if (peerMtu < MIN_REQUIRED_MTU) return false;
  const uint16_t available = attValueCapacity - AUDIO_HEADER_BYTES;
  uint16_t bounded = available < MAX_AUDIO_PAYLOAD_BYTES ? available : MAX_AUDIO_PAYLOAD_BYTES;
  // Packet-size eligibility is not a throughput guarantee; drop/reject counters
  // remain visible. Keep the selected format stable until START or RESUME.
  pcmTransport=peerMtu>=PCM_MIN_MTU;
  if(pcmTransport.load())bounded&=~1u;
  const uint16_t frameBytes=pcmTransport.load()?AUDIO_BYTES_PER_FRAME:ADPCM_BYTES_PER_FRAME;
  chunksPerFrame = (frameBytes + bounded - 1) / bounded;
  if (chunksPerFrame > MAX_CHUNKS_PER_FRAME) return false;
  uint16_t payload=(frameBytes + chunksPerFrame - 1) / chunksPerFrame;
  if(pcmTransport.load())payload=(payload+1u)&~1u; // Never split a PCM16 sample.
  audioPayloadBytes=payload;
  return audioPayloadBytes + AUDIO_HEADER_BYTES <= attValueCapacity;
}
void startStreaming(uint8_t version) {
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  if (!odysseyPrepareForConnectedStreaming(1500u)) {
    setDeviceState(DeviceState::ERROR, ErrorCode::AUDIO_SOURCE_FAILED);
    updateStatusCharacteristic(true);
    return;
  }
#endif
#if SYNAP_CHAKSHU
  if (mediaBusy()) { updateStatusCharacteristic(true);return; }
#endif
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
  resetRecovery();
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
  portENTER_CRITICAL(&streamErrorMux);
  if (error != ErrorCode::NONE && streamingEnabled.load() && generation == streamGeneration.load() &&
      (pendingStreamError == ErrorCode::NONE || pendingStreamErrorGeneration != generation)) {
    pendingStreamError = error;
    pendingStreamErrorGeneration = generation;
  }
  portEXIT_CRITICAL(&streamErrorMux);
}
void processStreamError() {
  portENTER_CRITICAL(&streamErrorMux);
  const ErrorCode error = pendingStreamError;
  const uint32_t generation = pendingStreamErrorGeneration;
  pendingStreamError = ErrorCode::NONE;
  portEXIT_CRITICAL(&streamErrorMux);
  // A fault from a completed take cannot terminate its replacement.
  if (error != ErrorCode::NONE && streamingEnabled.load() && generation == streamGeneration.load())
    stopStreaming(error);
}

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    (void)server;
// A BLE connection alone must never stop an SD-owned recording.
    // An explicit PWA START is the only link-triggered microphone handoff.
    ++connectionGeneration;
    if(!recoveryWaiting.load())streamingEnabled.store(false);
    deviceConnected.store(true);
    connectedLedAt=millis();
    connectionEventPending.store(true);
  }
  void onDisconnect(BLEServer* server) override {
    (void)server;
    deviceConnected.store(false);
    ++linkDisconnects;lastDisconnectAt=millis();lastDisconnectReason=0xFFFF;
    if(recoveryEnabled.load() && streamingEnabled.load()) {
      recoveryWaiting=true; if(!recoveryWaitingAt.load())recoveryWaitingAt=millis();
    } else streamingEnabled.store(false);
    ++connectionGeneration;
    connectionEventPending.store(true);
  }
#if defined(CONFIG_BLUEDROID_ENABLED)
  // Arduino 3.3.5 calls BOTH overloads; the common overload owns state changes.
  void onConnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
    if(param)server->updateConnParams(param->connect.remote_bda, BLE_MIN_INTERVAL,
      BLE_MAX_INTERVAL, BLE_SLAVE_LATENCY, BLE_SUPERVISION_TIMEOUT);
  }
  void onDisconnect(BLEServer* server, esp_ble_gatts_cb_param_t* param) override {
    (void)server;
    if(param)lastDisconnectReason=static_cast<uint16_t>(param->disconnect.reason);
  }
#elif defined(CONFIG_NIMBLE_ENABLED)
  void onConnect(BLEServer* server, ble_gap_conn_desc* desc) override {
    if(desc)server->updateConnParams(desc->conn_handle, BLE_MIN_INTERVAL,
      BLE_MAX_INTERVAL, BLE_SLAVE_LATENCY, BLE_SUPERVISION_TIMEOUT);
  }
  // This Arduino NimBLE callback omits the reason; retain 0xFFFF (unavailable).
#endif
};
class ControlCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic*) override { updateStatusCharacteristic(false); }
#if defined(CONFIG_NIMBLE_ENABLED)
  // Installed by tools/patch-arduino-ble.cjs. Never parse the mutable status
  // characteristic: battery/status publication can replace it during a write.
  void onWriteValue(BLECharacteristic*, ble_gap_conn_desc*, const uint8_t* data, size_t length) override {
    const uint8_t command = length == 2 && data ? data[0] : 0xFF;
    const uint8_t version = length == 2 && data ? data[1] : 0;
    queueEvent(EventType::COMMAND, command, version, streamGeneration.load());
  }
#elif defined(CONFIG_BLUEDROID_ENABLED)
  void onWrite(BLECharacteristic*, esp_ble_gatts_cb_param_t* param) override {
    // Protocol commands are exactly two bytes, never prepared/long writes.
    if (!param || param->write.is_prep || param->write.len != 2) return;
    queueEvent(EventType::COMMAND, param->write.value[0], param->write.value[1], streamGeneration.load());
  }
#else
  void onWrite(BLECharacteristic* characteristic) override {
    const size_t length = characteristic->getLength();
    const uint8_t* data = characteristic->getData();
    const uint8_t command = length == 2 && data ? data[0] : 0xFF;
    const uint8_t version = length == 2 && data ? data[1] : 0;
    // Defer work so synchronous GATT writes return promptly.
    queueEvent(EventType::COMMAND, command, version, streamGeneration.load());
  }
#endif
};
class AudioCallbacks : public BLECharacteristicCallbacks {
  void onStatus(BLECharacteristic* characteristic, Status status, uint32_t code) override {
    (void)characteristic;
    if (status != SUCCESS_NOTIFY) {
      lastNotifyStatus=static_cast<uint16_t>(status);lastNotifyError=code;
      ++notifyRejected;
    }
  }
};
class DiagnosticsCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* characteristic) override {
    (void)characteristic;
    updateDiagnosticsCharacteristic();
  }
};

void processCommand(uint8_t command, uint8_t version) {
  if (sleepPending) return;
  if (!deviceConnected.load()) {
    if(command==CMD_STOP && streamingEnabled.load()) {
      // A physical Stop during link loss ends capture but keeps negotiated
      // audio and its Stop receipt for the same journal's reconnect.
      if(recoveryEnabled.load())finishBufferedRecording();
      else stopStreaming();
    }
    return;
  }
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
      else if(recoveryEnabled.load() && streamingEnabled.load() && !recoveryWaiting.load())finishBufferedRecording();
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
    case CMD_RESTART:
      // Firmware restart is intentionally idle-only. Never interrupt a live
      // recording, an SD-owned local take, Chakshu media work, OTA or sleep.
      if (streamingEnabled.load()) { updateStatusCharacteristic(true); break; }
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
      if (odysseyRecording.load()) { updateStatusCharacteristic(true); break; }
#endif
#if SYNAP_CHAKSHU
      if (mediaBusy()) { updateStatusCharacteristic(true); break; }
#endif
      Serial.println("[SYSTEM] restart requested over BLE");
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
      if (!odysseyPrepareSdForPowerTransition(1000u)) {
        Serial.println("[SYSTEM] restart deferred until C3 SD storage is idle");
        updateStatusCharacteristic(true);
        break;
      }
#endif
      delay(120); // GATT write response has already returned; allow logs to flush.
      ESP.restart();
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
    if(!(recoveryWaiting.load() && streamingEnabled.load()))stopStreaming();
    publishPowerEvent(remoteStandby ? POWER_STATE_STANDBY : POWER_STATE_AWAKE);
    sampleBattery(true);
  } else {
    if(!(recoveryWaiting.load() && streamingEnabled.load()))stopStreaming();
    disconnectedAt=millis();
    restartAdvertising=true;
    Serial.printf("[BLE] disconnected count=%lu reason=0x%04X uptime=%lus heap=%u rejected=%lu recovery=%u\n",
      static_cast<unsigned long>(linkDisconnects.load()),unsigned(lastDisconnectReason.load()),
      static_cast<unsigned long>(millis()/1000u),unsigned(ESP.getFreeHeap()),
      static_cast<unsigned long>(notifyRejected.load()),unsigned(recoveryWaiting.load()));
  }
}

void controlTask(void* parameter) {
  (void)parameter;
  ControlMessage message;
  for (;;) {
    const bool received=xQueueReceive(controlQueue, &message, pdMS_TO_TICKS(10)) == pdTRUE;
    reconcileConnection();
    processRecoveryRequest();
    if(recoveryWaiting.load() && uint32_t(millis()-recoveryWaitingAt.load())>=60000u)stopStreaming();
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
    processStreamError();
    // A restored link cannot transmit until RESUME binds it to the app journal.
    if(recoveryFinishing.load() && deviceConnected.load() && !recoveryWaiting.load() && (!recoveryCanSend() && !transmitterActive.load() && uint32_t(millis()-recoveryFinishAt)>150u))stopStreaming();
    if(recoveryFinishing.load() && uint32_t(millis()-recoveryFinishAt)>35000u)stopStreaming(ErrorCode::TRANSPORT_CHANGED);
    if (deviceConnected.load() && streamingEnabled.load() && !recoveryWaiting.load()) {
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
#if SYNAP_CHAKSHU
    ChakshuMedia::tick();
#endif
    powerTick();
    applyCpuPowerProfile(streamingEnabled.load() || otaNeedsActiveCpu());
    updateStatusLed();
  }
}
bool acquireAudioFrame(AudioFrame& frame) {
#if USE_REAL_I2S_MIC
  MicrophoneGuard guard;
  static int32_t raw[SAMPLES_PER_FRAME];
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
        if (startMicrophone()) { received=0; emptyReads=0; continue; }
      }
      return false;
    }
    emptyReads=0;
    received += count;
  }
  for (uint16_t i=0; i<SAMPLES_PER_FRAME; ++i) {
    const int32_t sample=raw[i] >> 16;
    // Format conversion only: retain the signed upper 16 bits of the I2S slot.
    // No filter, gain, gate or per-recording signal history precedes the codec.
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
    if (!streamingEnabled.load() || recoveryFinishing.load() || (!deviceConnected.load() && !recoveryEnabled.load())) {
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
    if (!streamingEnabled.load() || recoveryFinishing.load() || frame.generation != streamGeneration.load()) continue;
    if (!acquired) { requestStreamError(ErrorCode::AUDIO_SOURCE_FAILED, frame.generation); continue; }
    frame.sequence=nextSequence++;
    ++capturedFrames;
    retainRecoveryFrame(frame);
    if (xQueueSend(audioFrameQueue, &frame, 0) != pdTRUE && !recoveryEnabled.load()) ++captureDrops;
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

void encodeImaAdpcm(const int16_t* samples, uint8_t* output) {
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
}

// Only the transmitter task owns this cursor. A congested fragment is retried
// in place; restarting at zero can starve the tail of every PCM frame.
class AudioSendProgress {
  uint32_t generation=0, connection=0, replay=0;
  uint16_t sequence=0, payload=0;
  uint8_t chunks=0, next=0;
  bool pcm=false, valid=false;
 public:
  uint8_t begin(uint32_t g,uint32_t c,uint32_t r,uint16_t s,uint8_t n,uint16_t p,bool raw) {
    if (!valid || generation!=g || connection!=c || replay!=r || sequence!=s || chunks!=n || payload!=p || pcm!=raw) {
      generation=g;connection=c;replay=r;sequence=s;chunks=n;payload=p;pcm=raw;next=0;valid=true;
    }
    return next;
  }
  void accept(uint8_t index) { next=index+1; }
  void reset() { valid=false; }
} audioSendProgress;

bool sendEncodedFrame(uint32_t generation,uint16_t sequence,const uint8_t* encoded,uint32_t paceUs,bool pcm) {
  const uint16_t frameBytes=pcm?AUDIO_BYTES_PER_FRAME:ADPCM_BYTES_PER_FRAME;
  const uint8_t chunks=chunksPerFrame;
  const uint16_t payload=audioPayloadBytes, capacity=attValueCapacity;
  if (chunks < MIN_CHUNKS_PER_FRAME || chunks > MAX_CHUNKS_PER_FRAME ||
      !payload || payload > MAX_AUDIO_PAYLOAD_BYTES ||
      uint32_t(chunks)*payload<frameBytes || uint32_t(chunks-1)*payload>=frameBytes) return false;
  static uint8_t packet[AUDIO_HEADER_BYTES+MAX_AUDIO_PAYLOAD_BYTES];
  const uint32_t connection=connectionGeneration.load();
  const uint32_t replay=audioReplayGeneration.load();
  const uint8_t first=audioSendProgress.begin(generation,connection,replay,sequence,chunks,payload,pcm);
  for (uint8_t index=first; index<chunks; ++index) {
    if (!streamingEnabled.load() || !deviceConnected.load() ||
        generation != streamGeneration.load() || connection != connectionGeneration.load() || replay != audioReplayGeneration.load()) return false;
    const uint16_t offset=index*payload;
    if(offset>=frameBytes)return false;
    const uint16_t remaining=frameBytes-offset;
    const uint16_t length=remaining < payload ? remaining : payload;
    if (AUDIO_HEADER_BYTES+length > capacity) return false;
    packet[0]=AUDIO_PACKET_MAGIC; packet[1]=pcm?PCM_AUDIO_PROTOCOL_VERSION:AUDIO_PROTOCOL_VERSION;
    packet[2]=sequence & 255; packet[3]=sequence >> 8;
    packet[4]=index; packet[5]=chunks; packet[6]=length & 255; packet[7]=length >> 8;
    memcpy(packet+AUDIO_HEADER_BYTES, encoded+offset, length);
    audioCharacteristic->setValue(packet, AUDIO_HEADER_BYTES+length);
    bool accepted=false;
    for(uint8_t attempt=0;attempt<4;++attempt) {
      if(attempt)vTaskDelay(pdMS_TO_TICKS(15u*attempt));
      if(!streamingEnabled.load() || !deviceConnected.load() ||
          generation!=streamGeneration.load() || connection!=connectionGeneration.load() || replay!=audioReplayGeneration.load())return false;
      const uint32_t rejectedBefore=notifyRejected.load();
      // In the pinned Arduino BLE library, onStatus runs before notify returns.
      // SUCCESS_NOTIFY means queued locally, not persisted by the phone.
      audioCharacteristic->notify();
      if(notifyRejected.load()==rejectedBefore) { accepted=true;break; }
    }
    if(!accepted)return false;
    audioSendProgress.accept(index);
    // Pace from the completed attempt. An overdue notification must never cause
    // the remaining fragments to burst into the controller's congested queue.
    const uint32_t slot=static_cast<uint32_t>(index+1)*paceUs/chunks-static_cast<uint32_t>(index)*paceUs/chunks;
    const uint32_t target=micros()+slot;
    if (index+1 == chunks) {
      // Keep the frame rate bounded during queue catch-up, yielding all remaining time.
      while (static_cast<int32_t>(target-micros()) > 0) vTaskDelay(1);
    } else {
      while (static_cast<int32_t>(target-micros()) > 1000) vTaskDelay(1);
      while (static_cast<int32_t>(target-micros()) > 0) delayMicroseconds(50);
    }
  }
  audioSendProgress.reset();
  return generation == streamGeneration.load() && deviceConnected.load() && connection == connectionGeneration.load() && replay == audioReplayGeneration.load();
}
bool sendCapturedFrame(const AudioFrame& frame, uint32_t paceUs) {
  if(pcmTransport.load()) {
    // ESP32-C3/S3 PCM samples are little endian, matching the protocol-v2 wire.
    return sendEncodedFrame(frame.generation,frame.sequence,
      reinterpret_cast<const uint8_t*>(frame.samples),paceUs,true);
  }
  // Only small-MTU links use lossy compression. Recovery storage stays PCM.
  static uint8_t encoded[ADPCM_BYTES_PER_FRAME];
  encodeImaAdpcm(frame.samples,encoded);
  return sendEncodedFrame(frame.generation,frame.sequence,encoded,paceUs,false);
}
bool sendAudioFrame(const AudioFrame& frame) {
  return sendCapturedFrame(frame,45000u);
}
void transmitterTask(void* parameter) {
  (void)parameter;
  AudioFrame frame;
  for (;;) {
    const bool queued=xQueueReceive(audioFrameQueue, &frame, recoveryCanSend()?0:(recoveryEnabled.load() && streamingEnabled.load()?pdMS_TO_TICKS(20):portMAX_DELAY))==pdTRUE;
    if(recoveryEnabled.load()) {
      transmitterActive.store(true);
      const bool sent=streamingEnabled.load() && recoveryCanSend() && sendRecoveryFrame();
      transmitterActive.store(false);
      if(!queued && !sent)vTaskDelay(1);
      continue;
    }
    if(!queued)continue;
    // Claim activity before checking the session so STOP cannot miss a pending send.
    transmitterActive.store(true);
    if (streamingEnabled.load() && frame.generation == streamGeneration.load()) {
      const uint32_t rejectedBefore=notifyRejected.load();
      if (!sendAudioFrame(frame) &&
          deviceConnected.load() && streamingEnabled.load() && frame.generation == streamGeneration.load()) {
        // Legacy sessions lack a recovery ring: account for the lost frame but
        // keep recording through transient queue pressure.
        if(notifyRejected.load()!=rejectedBefore)++captureDrops;
        else requestStreamError(ErrorCode::TRANSPORT_CHANGED, frame.generation);
      }
    }
    transmitterActive.store(false);
  }
}

// C3 file I/O and recoverable recording commit records. All callers own the SD mutex.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

// A cached transfer descriptor is always closed under the same storage mutex
// before unmount, format, recording, deletion, or a new transfer identity.
static int odysseySdReadFd=-1;
static char odysseySdReadPath[96]{};
static uint32_t odysseySdReadConnection=0,odysseySdReadAt=0;
static inline bool odysseySdCloseReadLocked() {
  const int fd=odysseySdReadFd;odysseySdReadFd=-1;odysseySdReadPath[0]=0;
  odysseySdReadConnection=0;odysseySdReadAt=0;
  if (fd<0) return true;
  errno=0;
  if (close(fd)==0) return true;
  const int saved=errno?errno:EIO;
  errno=saved;
  return false;
}
static inline uint32_t odysseySdLe32(const uint8_t* p) {
  return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);
}
static inline void odysseyWavHeader(uint8_t* h,uint32_t bytes) {
  memset(h,0,44);
  memcpy(h,"RIFF",4);put32le(h+4,bytes+36);
  memcpy(h+8,"WAVEfmt ",8);put32le(h+16,16);
  h[20]=1;h[22]=1;put32le(h+24,SAMPLE_RATE);
  put32le(h+28,SAMPLE_RATE*2);h[32]=2;h[34]=16;
  memcpy(h+36,"data",4);put32le(h+40,bytes);
}
static inline uint32_t odysseySdCrcUpdate(uint32_t crc,const uint8_t* data,size_t size) {
  for (size_t i=0;i<size;++i) {
    crc^=data[i];
    for (unsigned bit=0;bit<8;++bit) crc=(crc>>1)^((crc&1u)?0xedb88320u:0u);
  }
  return crc;
}
static inline uint32_t odysseySdCrc(const uint8_t* data,size_t size) {
  return ~odysseySdCrcUpdate(0xffffffffu,data,size);
}
static constexpr uint32_t ODYSSEY_WAV_MAX_PCM_BYTES=9600000u;
static constexpr off_t ODYSSEY_INLINE_JOURNAL_OFFSET=
  (44+off_t(ODYSSEY_WAV_MAX_PCM_BYTES)+511)&~off_t(511);
static constexpr size_t ODYSSEY_INLINE_JOURNAL_BYTES=1024u;
static_assert((ODYSSEY_INLINE_JOURNAL_OFFSET&511)==0,
  "inline recovery journal must begin on a physical SD sector boundary");
static constexpr int ODYSSEY_INLINE_JOURNAL=-2;

static inline bool odysseyPwriteAll(int fd,const uint8_t* data,size_t size,off_t offset) {
  // The pinned IDF FatFs pwrite has a zero-write/ENOSPC early return
  // that skips releasing its VFS lock. Own the cursor under the SD mutex.
  const off_t previous=lseek(fd,0,SEEK_CUR);
  if (previous<0 || lseek(fd,offset,SEEK_SET)<0) return false;
  while (size) {
    const ssize_t n=write(fd,data,size);
    if (n<0 && errno==EINTR) continue;
    // Do not issue another filesystem operation after a failed write.
    if (n<=0) { if (!n) errno=ENOSPC;return false; }
    data+=n;size-=size_t(n);
  }
  return lseek(fd,previous,SEEK_SET)>=0;
}
static inline bool odysseyPreadAll(int fd,uint8_t* data,size_t size,off_t offset) {
  while (size) {
    const ssize_t n=pread(fd,data,size,offset);
    if (n<0 && errno==EINTR) continue;
    if (n<=0) { if (!n) errno=EIO;return false; }
    data+=n;size-=size_t(n);offset+=n;
  }
  return true;
}
static inline bool odysseyJournalPath(const char* wav,char* path,size_t capacity) {
  const int n=snprintf(path,capacity,"%s.jrn",wav);
  if (n<0 || size_t(n)>=capacity) { errno=ENAMETOOLONG;return false; }
  return true;
}
static inline bool odysseyRemoveJournal(const char* wav) {
  char path[144];
  return odysseyJournalPath(wav,path,sizeof(path)) && (unlink(path)==0 || errno==ENOENT);
}
// 0=absent, 1=present, -1=filesystem/path error. Legacy sidecars are
// recognized first; new recordings store the two commit sectors inline at the
// reserved tail of the WAV so recording needs only one FAT descriptor.
static inline int odysseyJournalPresence(const char* wav) {
  char path[144];struct stat st{};
  if (!odysseyJournalPath(wav,path,sizeof(path))) return -1;
  errno=0;
  if (stat(path,&st)==0) return 1;
  if (errno!=ENOENT) return -1;
  errno=0;
  if (stat(wav,&st)!=0) {
    if (errno==ENOENT) { errno=0;return 0; }
    return -1;
  }
  errno=0;
  return st.st_size>=ODYSSEY_INLINE_JOURNAL_OFFSET+off_t(ODYSSEY_INLINE_JOURNAL_BYTES)?1:0;
}
static inline bool odysseyJournalAbsent(const char* wav) {
  return odysseyJournalPresence(wav)==0;
}

struct OdysseyWavMeta {
  uint32_t takeHigh=0,takeLow=0,part=0,pcmBytes=0,crc32=0;
};
static inline bool odysseyMetaPath(const char* wav,char* path,size_t capacity) {
  const int n=snprintf(path,capacity,"%s.meta",wav);
  if (n<0 || size_t(n)>=capacity) { errno=ENAMETOOLONG;return false; }
  return true;
}
static inline bool odysseyRemoveMeta(const char* wav) {
  char path[144];
  if (!odysseyMetaPath(wav,path,sizeof(path))) return false;
  errno=0;
  return unlink(path)==0 || errno==ENOENT;
}
// Metadata is an integrity accelerator, not the source of truth. A WAV remains
// recoverable/syncable if this sidecar is absent after sudden power loss.
static inline bool odysseyWriteWavMeta(const char* wav,uint32_t takeHigh,uint32_t takeLow,
    uint32_t part,uint32_t pcmBytes,uint32_t crc32) {
  char path[144],temp[152];
  if (!odysseyMetaPath(wav,path,sizeof(path))) return false;
  const int n=snprintf(temp,sizeof(temp),"%s.tmp",path);
  if (n<0 || size_t(n)>=sizeof(temp)) { errno=ENAMETOOLONG;return false; }

  uint8_t record[32]{};
  memcpy(record,"SYNAPM01",8);
  put32le(record+8,takeHigh);put32le(record+12,takeLow);
  put32le(record+16,part);put32le(record+20,pcmBytes);put32le(record+24,crc32);
  put32le(record+28,odysseySdCrc(record,28));

  errno=0;
  if (unlink(temp)!=0 && errno!=ENOENT) return false;
  errno=0;
  const int fd=open(temp,O_CREAT|O_TRUNC|O_RDWR,0644);
  if (fd<0) return false;
  bool ok=odysseyPwriteAll(fd,record,sizeof(record),0) && fsync(fd)==0;
  int saved=ok?0:(errno?errno:EIO);
  errno=0;
  if (close(fd)!=0) { if (!saved) saved=errno?errno:EIO;ok=false; }
  if (!ok) { (void)unlink(temp);errno=saved?saved:EIO;return false; }

  errno=0;
  if (unlink(path)!=0 && errno!=ENOENT) {
    saved=errno?errno:EIO;(void)unlink(temp);errno=saved;return false;
  }
  errno=0;
  if (rename(temp,path)!=0) {
    saved=errno?errno:EIO;(void)unlink(temp);errno=saved;return false;
  }
  errno=0;
  return true;
}
// 1=valid metadata, 0=absent, -1=invalid metadata, -2=filesystem I/O failure.
static inline int odysseyReadWavMeta(const char* wav,uint32_t& takeHigh,uint32_t& takeLow,
    uint32_t& part,uint32_t& pcmBytes,uint32_t& crc32) {
  char path[144];
  if (!odysseyMetaPath(wav,path,sizeof(path))) return -2;
  errno=0;
  const int fd=open(path,O_RDONLY);
  if (fd<0) {
    if (errno==ENOENT) { errno=0;return 0; }
    return -2;
  }
  uint8_t record[32]{};
  struct stat st{};
  bool ok=fstat(fd,&st)==0 && st.st_size==off_t(sizeof(record)) &&
    odysseyPreadAll(fd,record,sizeof(record),0);
  int saved=ok?0:(errno?errno:EIO);
  errno=0;
  if (close(fd)!=0) { if (!saved) saved=errno?errno:EIO;ok=false; }
  if (!ok) { errno=saved?saved:EIO;return -2; }
  if (memcmp(record,"SYNAPM01",8)!=0 ||
      odysseySdLe32(record+28)!=odysseySdCrc(record,28)) {
    errno=0;return -1;
  }
  takeHigh=odysseySdLe32(record+8);takeLow=odysseySdLe32(record+12);
  part=odysseySdLe32(record+16);pcmBytes=odysseySdLe32(record+20);
  crc32=odysseySdLe32(record+24);
  errno=0;
  return 1;
}
static inline int odysseyCreateJournal(const char* wav) {
  char path[144];
  if (!odysseyJournalPath(wav,path,sizeof(path))) return -1;
  const int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0644);
  if (fd<0) return -1;
  // Reserve both sectors before capture starts; checkpoints never grow this file.
  uint8_t empty[512]{};
  if (!odysseyPwriteAll(fd,empty,sizeof(empty),0) ||
      !odysseyPwriteAll(fd,empty,sizeof(empty),512) || fsync(fd)!=0) {
    const int saved=errno;close(fd);errno=saved;return -1;
  }
  return fd;
}
static inline bool odysseyJournalCommit(int wavFd,int journal,const char* path,
    uint32_t bytes,uint32_t& sequence) {
  // Sync PCM first. Only then advertise its length in the alternate commit
  // sector. New C3 recordings use the WAV descriptor itself; legacy sidecar
  // descriptors remain supported for recovery/fixture compatibility.
  if (fsync(wavFd)!=0) return false;
  uint8_t record[512]{};
  memcpy(record,"SYNAPJ01",8);
  const uint32_t next=sequence+1;
  put32le(record+8,next);put32le(record+12,bytes);
  put32le(record+16,odysseySdCrc(reinterpret_cast<const uint8_t*>(path),strlen(path)));
  put32le(record+20,SAMPLE_RATE);
  const size_t prefix=std::min(size_t(bytes),size_t(468));
  if (prefix && !odysseyPreadAll(wavFd,record+24,prefix,44)) return false;
  put32le(record+508,odysseySdCrc(record,508));

  const bool inlineJournal=journal==ODYSSEY_INLINE_JOURNAL;
  const int target=inlineJournal?wavFd:journal;
  if (target<0) { errno=EBADF;return false; }
  const off_t base=inlineJournal?ODYSSEY_INLINE_JOURNAL_OFFSET:0;
  if (!odysseyPwriteAll(target,record,sizeof(record),
        base+off_t((next-1u)&1u)*512) || fsync(target)!=0)
    return false;
  sequence=next;
  return true;
}
// Returns 1 for a valid commit, 0 for no journal, -1 for invalid journal, -2 for I/O failure.
static inline int odysseyReadJournalSlots(int fd,off_t base,const char* path,
    uint32_t& bytes,uint8_t* firstPcm) {
  uint32_t newest=0;
  const uint32_t identity=odysseySdCrc(reinterpret_cast<const uint8_t*>(path),strlen(path));
  for (unsigned slot=0;slot<2;++slot) {
    uint8_t record[512]{};
    if (!odysseyPreadAll(fd,record,sizeof(record),base+off_t(slot)*512)) return -2;
    const uint32_t seq=odysseySdLe32(record+8),size=odysseySdLe32(record+12);
    if (memcmp(record,"SYNAPJ01",8)==0 && seq>newest &&
        size<=ODYSSEY_WAV_MAX_PCM_BYTES && !(size&1u) &&
        odysseySdLe32(record+16)==identity && odysseySdLe32(record+20)==SAMPLE_RATE &&
        odysseySdLe32(record+508)==odysseySdCrc(record,508)) {
      newest=seq;bytes=size;memcpy(firstPcm,record+24,468);
    }
  }
  errno=0;
  return newest?1:-1;
}
static inline int odysseyJournalRead(const char* path,uint32_t& bytes,uint8_t* firstPcm) {
  // First support the legacy .jrn sidecar so recordings made by older firmware
  // remain recoverable after OTA.
  char journalPath[144];
  if (!odysseyJournalPath(path,journalPath,sizeof(journalPath))) return -2;
  errno=0;
  int fd=open(journalPath,O_RDONLY);
  if (fd>=0) {
    struct stat st{};
    if (fstat(fd,&st)!=0) {
      const int saved=errno?errno:EIO;(void)close(fd);errno=saved;return -2;
    }
    int result=-1;
    if (st.st_size==off_t(ODYSSEY_INLINE_JOURNAL_BYTES))
      result=odysseyReadJournalSlots(fd,0,path,bytes,firstPcm);
    const int saved=errno;
    if (close(fd)!=0 && result>=0) return -2;
    errno=saved;
    return result;
  }
  if (errno!=ENOENT) return -2;

  // New recordings keep recovery commits in the reservation tail of the WAV.
  errno=0;
  fd=open(path,O_RDONLY);
  if (fd<0) return errno==ENOENT?0:-2;
  struct stat st{};
  if (fstat(fd,&st)!=0) {
    const int saved=errno?errno:EIO;(void)close(fd);errno=saved;return -2;
  }
  if (st.st_size<ODYSSEY_INLINE_JOURNAL_OFFSET+off_t(ODYSSEY_INLINE_JOURNAL_BYTES)) {
    const int rc=close(fd);
    if (rc!=0) return -2;
    errno=0;
    return 0;
  }
  const int result=odysseyReadJournalSlots(fd,ODYSSEY_INLINE_JOURNAL_OFFSET,path,bytes,firstPcm);
  const int saved=errno;
  if (close(fd)!=0 && result>=0) return -2;
  errno=saved;
  return result;
}
static inline bool odysseyWavValid(const uint8_t* h,uint32_t& bytes) {
  bytes=odysseySdLe32(h+40);
  uint8_t expected[44];odysseyWavHeader(expected,bytes);
  return bytes<=ODYSSEY_WAV_MAX_PCM_BYTES && !(bytes&1u) && memcmp(h,expected,44)==0;
}
// Retryable recovery: retain the journal until header, length and close all succeed.
static inline bool odysseyRecoverWav(const char* path) {
  errno=0;
  struct stat st{};
  if (stat(path,&st)!=0) return false;
  uint32_t bytes=0;
  uint8_t firstSector[512]{};
  const int journal=odysseyJournalRead(path,bytes,firstSector+44);
  if (journal==-2) return false;
  if (journal==-1) {
    // Preserve questionable data and its journal for manual recovery; this is
    // a content-integrity condition, not evidence that the volume itself failed.
    errno=0;
    return true;
  }
  errno=0;
  const int fd=open(path,O_RDWR);
  if (fd<0) return false;
  uint8_t header[44]{};
  bool ok=true,valid=true;
  int savedError=0;
  if (!journal) {
    if (st.st_size<44) valid=false;
    else if (!odysseyPreadAll(fd,header,sizeof(header),0)) {
      savedError=errno?errno:EIO;ok=false;
    } else valid=odysseyWavValid(header,bytes);
  }
  const uint64_t committed=44ull+bytes;
  if (committed>uint64_t(st.st_size)) valid=false;
  if (ok && valid && (journal || committed!=uint64_t(st.st_size))) {
    odysseyWavHeader(header,bytes);
    memcpy(firstSector,header,44);
    const size_t repairBytes=journal?44+std::min(size_t(bytes),size_t(468)):44;
    if (!odysseyPwriteAll(fd,firstSector,repairBytes,0) || fsync(fd)!=0 ||
        ftruncate(fd,off_t(committed))!=0 || fsync(fd)!=0) {
      savedError=errno?errno:EIO;ok=false;
    }
  }
  if (close(fd)!=0) {
    if (!savedError) savedError=errno?errno:EIO;
    ok=false;
  }
  if (!ok) { errno=savedError?savedError:EIO;return false; }
  if (!valid) { errno=0;return true; }
  if (journal && !odysseyRemoveJournal(path)) return false;
  if (bytes) { errno=0;return true; }
  errno=0;
  if (unlink(path)==0 || errno==ENOENT) { errno=0;return true; }
  return false;
}
#endif
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
    if (attempt<attempts) {
      // Some cards/modules stay busy across ESP software reset/deep sleep even
      // though the bus is already deselected. Every retry starts from a full
      // SD.end()/SPI.end() teardown; progressively longer idle time lets the
      // card finish internal work before Arduino sends its fresh CMD0 sequence.
      const uint32_t retryDelayMs=250u*uint32_t(attempt)*uint32_t(attempt);
      Serial.printf("[SD] %s retry %u/%u after %lu ms idle\n",
        reason,unsigned(attempt+1u),unsigned(attempts),
        static_cast<unsigned long>(retryDelayMs));
      delay(retryDelayMs);
    }
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
  // Boot is allowed three bounded attempts before BLE starts. This is still
  // fail-closed: a card is published ready only after mount + geometry +
  // durable VFS write/readback validation succeeds.
  const bool ready=odysseySdMountLocked("boot",3);
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
// C3 local audio owns the mounted VFS and microphone until finalization.
// BLE connection changes never redirect a take; no local PCM enters the app queue.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
#include <unistd.h>

static constexpr size_t ODYSSEY_SD_WRITE_BUFFER_BYTES=8192u;
static constexpr size_t ODYSSEY_SD_WRITE_CHUNK_BYTES=4096u;
static constexpr size_t ODYSSEY_SD_SECTOR_BYTES=512u;
static constexpr size_t ODYSSEY_WAV_HEADER_BYTES=44u;
static constexpr uint32_t ODYSSEY_WAV_CHECKPOINT_MS=15000u;
static constexpr uint32_t ODYSSEY_WAV_SEGMENT_SECONDS=300u;
static constexpr uint32_t ODYSSEY_WAV_SEGMENT_FRAMES=(SAMPLE_RATE*ODYSSEY_WAV_SEGMENT_SECONDS)/SAMPLES_PER_FRAME;
static constexpr uint32_t ODYSSEY_WAV_SEGMENT_PCM_BYTES=ODYSSEY_WAV_SEGMENT_FRAMES*SAMPLES_PER_FRAME*2u;
static_assert(ODYSSEY_WAV_SEGMENT_PCM_BYTES==ODYSSEY_WAV_MAX_PCM_BYTES,
  "inline recovery journal offset must match the five-minute PCM reservation");
static constexpr uint64_t ODYSSEY_WAV_SEGMENT_FILE_BYTES=
  uint64_t(ODYSSEY_INLINE_JOURNAL_OFFSET)+ODYSSEY_INLINE_JOURNAL_BYTES;
static constexpr uint64_t ODYSSEY_SD_FREE_RESERVE_BYTES=2ull*1024ull*1024ull;
static_assert((SAMPLE_RATE*ODYSSEY_WAV_SEGMENT_SECONDS)%SAMPLES_PER_FRAME==0,
  "WAV rollover must align with complete microphone frames");
static uint8_t odysseySdWriteBuffer[ODYSSEY_SD_WRITE_BUFFER_BYTES];
static std::atomic<uint8_t> odysseyRecordFailureStage{0};
static std::atomic<uint32_t> odysseyRecordLastBytes{0};

uint8_t odysseySdRecordFailureStage() { return odysseyRecordFailureStage.load(); }
uint32_t odysseySdRecordLastBytes() { return odysseyRecordLastBytes.load(); }

static bool odysseyHasSpaceForSegment() {
  const uint64_t freeBytes=odysseySdFreeBytesLocked();
  const uint64_t needed=ODYSSEY_WAV_SEGMENT_FILE_BYTES+ODYSSEY_SD_FREE_RESERVE_BYTES;
  if (freeBytes>=needed) return true;
  errno=ENOSPC;
  Serial.printf("[SD] offline start/rollover refused: free=%llu need=%llu bytes\n",
    static_cast<unsigned long long>(freeBytes),static_cast<unsigned long long>(needed));
  return false;
}

static bool odysseyWriteBufferedChunk(int file,uint8_t* buffer,size_t& buffered,
    size_t count,uint32_t& bytes) {
  if (!count || count>buffered) return count==0;
  size_t completed=0;
  while (completed<count) {
    const ssize_t n=write(file,buffer+completed,count-completed);
    if (n<0 && errno==EINTR) continue;
    if (n<=0) { if (!n) errno=EIO;break; }
    completed+=size_t(n);
  }
  bytes+=uint32_t(completed);buffered-=completed;
  if (buffered && completed) memmove(buffer,buffer+completed,buffered);
  return completed==count;
}

static bool odysseyDrainPcmBuffer(int file,uint8_t* buffer,size_t& buffered,
    uint32_t& bytes,bool finalFlush) {
  while (buffered) {
    const size_t fileOffset=ODYSSEY_WAV_HEADER_BYTES+size_t(bytes);
    const size_t sectorOffset=fileOffset&(ODYSSEY_SD_SECTOR_BYTES-1u);
    size_t chunk=0;

    // The WAV payload starts at byte 44. Complete that first physical sector,
    // then keep normal data writes sector-aligned without changing WAV format.
    if (sectorOffset) {
      const size_t toBoundary=ODYSSEY_SD_SECTOR_BYTES-sectorOffset;
      if (!finalFlush && buffered<toBoundary) return true;
      chunk=std::min(buffered,toBoundary);
    } else {
      const size_t aligned=buffered&~(ODYSSEY_SD_SECTOR_BYTES-1u);
      if (aligned) chunk=std::min(aligned,ODYSSEY_SD_WRITE_CHUNK_BYTES);
      else if (finalFlush) chunk=buffered;
      else return true;
    }

    if (!odysseyWriteBufferedChunk(file,buffer,buffered,chunk,bytes)) return false;
  }
  return true;
}

static bool odysseyFinalizeWav(int file,int journal,const char* path,uint32_t& sequence,
    uint8_t* header,uint8_t* buffer,size_t& buffered,uint32_t& bytes) {
  if (!odysseyDrainPcmBuffer(file,buffer,buffered,bytes,true) ||
      !odysseyJournalCommit(file,journal,path,bytes,sequence)) return false;
  // Header is overwritten only when sealing a part. The journal can repair an
  // interrupted header update; it cannot make SD hardware power-loss atomic.
  odysseyWavHeader(header,bytes);
  return odysseyPwriteAll(file,header,ODYSSEY_WAV_HEADER_BYTES,0) && fsync(file)==0 &&
    ftruncate(file,off_t(ODYSSEY_WAV_HEADER_BYTES+bytes))==0 && fsync(file)==0;
}

static void odysseyRecordTake() {
  bool failed=false,storageFailed=false;
  uint8_t failureStage=0;
  int firstErrno=0;
  auto failure=[&](uint8_t stage) -> uint8_t {
    if (!failureStage) firstErrno=errno;
    return failureStage?failureStage:stage;
  };
  uint64_t totalBytes=0;
  uint32_t bytes=0,segment=0;
  uint32_t takeHigh=0,takeLow=0;
  char logicalPath[64]{};
  char fullPath[96]{};
  uint8_t header[44];
  int file=-1;
  int journal=ODYSSEY_INLINE_JOURNAL;
  uint32_t journalSequence=0;
  uint32_t segmentCrcState=0xffffffffu;
  size_t bufferedBytes=0;

  odysseyRecordFailureStage=0;
  odysseyRecordLastBytes=0;
  errno=0;

  // Hold the single storage mutex for the whole take. A recovery/remount can
  // never tear down the VFS beneath an open recording.
  OdysseySdGuard storage;
  if (!storage) {
    errno=EBUSY;failed=true;storageFailed=true;failureStage=failure(1);
  } else if (!odysseySdReady()) {
    errno=ENODEV;failed=true;storageFailed=true;failureStage=failure(1);
  } else {
    errno=0;
    if (!odysseySdCloseReadLocked()) {
      failed=true;storageFailed=true;failureStage=failure(1);
    }
  }

  if (!failed) {
    takeHigh=esp_random();takeLow=esp_random();
    for (uint8_t attempt=0;attempt<16;++attempt) {
      snprintf(logicalPath,sizeof(logicalPath),"/synap/odyssey_audio_%08lx_%08lx_p%04lu.wav",
        static_cast<unsigned long>(takeHigh),static_cast<unsigned long>(takeLow),
        static_cast<unsigned long>(segment));
      if (!odysseySdPath(logicalPath,fullPath,sizeof(fullPath))) {
        failed=true;storageFailed=true;failureStage=failure(2);break;
      }
      struct stat existing{};
      errno=0;
      if (stat(fullPath,&existing)==0) {
        // A collision is improbable; regenerate both take identifiers before
        // opening anything so later parts remain a contiguous numbered set.
        takeHigh=esp_random();takeLow=esp_random();
        continue;
      }
      if (errno!=ENOENT) { failed=true;storageFailed=true;failureStage=failure(2);break; }
      if (!odysseyHasSpaceForSegment()) {
        failed=true;failureStage=failure(31);break;
      }
      if (!odysseySdPreallocateFile(fullPath,ODYSSEY_WAV_SEGMENT_FILE_BYTES)) {
        failed=true;storageFailed=true;failureStage=failure(30);break;
      }
      errno=0;
      file=open(fullPath,O_RDWR);
      if (file>=0) break;
      failed=true;storageFailed=true;failureStage=failure(3);break;
    }
    if (file<0 && !failed) { failed=true;storageFailed=true;failureStage=failure(2); }
  }

  odysseyWavHeader(header,0);
  if (!failed) {
    errno=0;
    if (!odysseyPwriteAll(file,header,sizeof(header),0)) failureStage=failure(33);
    else if (lseek(file,ODYSSEY_WAV_HEADER_BYTES,SEEK_SET)<0) failureStage=failure(34);
    else if (!odysseyJournalCommit(file,ODYSSEY_INLINE_JOURNAL,fullPath,0,journalSequence))
      failureStage=failure(35);
    if (failureStage) { failed=true;storageFailed=true; }
  }

#if USE_REAL_I2S_MIC
  if (!failed && !odysseyStopRequested.load()) {
    MicrophoneGuard guard;
    errno=0;
    if (!startMicrophone()) { failed=true;failureStage=failure(4); }
    else {
      odysseyRecordingStartedAt=millis();
      odysseyCaptureActive=true;
      odysseySdRecoveryActive=false;
      updateStatusLed(true);
      Serial.println("[SD] PCM capture active");
    }
    int32_t raw[SAMPLES_PER_FRAME];
    int16_t pcm[SAMPLES_PER_FRAME];
    uint32_t checkpointAt=millis();
    while (!failed && !odysseyStopRequested.load()) {
      size_t received=0;
      uint8_t emptyReads=0;
      while (received<sizeof(raw) && !odysseyStopRequested.load()) {
        const size_t count=microphoneI2S.readBytes(reinterpret_cast<char*>(raw)+received,sizeof(raw)-received);
        if (!count) {
          if (++emptyReads>=3) { failed=true;failureStage=failure(4);break; }
        } else {
          received+=count;emptyReads=0;
        }
      }
      if (failed || odysseyStopRequested.load()) break;
      for (uint16_t i=0;i<SAMPLES_PER_FRAME;++i) pcm[i]=static_cast<int16_t>(raw[i]>>16);
      if (uint64_t(bytes)+uint64_t(bufferedBytes)+sizeof(pcm)>ODYSSEY_WAV_SEGMENT_PCM_BYTES) {
        failed=true;storageFailed=true;failureStage=failure(5);break;
      }

      // Open the next reserved part only after a complete PCM frame has been
      // captured. A stop exactly at a segment boundary then leaves no empty
      // next-part file on the card.
      if (file<0) {
        snprintf(logicalPath,sizeof(logicalPath),"/synap/odyssey_audio_%08lx_%08lx_p%04lu.wav",
          static_cast<unsigned long>(takeHigh),static_cast<unsigned long>(takeLow),
          static_cast<unsigned long>(segment));
        if (!odysseySdPath(logicalPath,fullPath,sizeof(fullPath))) {
          failed=true;storageFailed=true;failureStage=failure(2);break;
        }
        if (!odysseyHasSpaceForSegment()) {
          failed=true;failureStage=failure(31);break;
        }
        if (!odysseySdPreallocateFile(fullPath,ODYSSEY_WAV_SEGMENT_FILE_BYTES)) {
          failed=true;storageFailed=true;failureStage=failure(30);break;
        }
        errno=0;
        file=open(fullPath,O_RDWR);
        if (file<0) {
          failed=true;storageFailed=true;failureStage=failure(3);break;
        }
        journal=ODYSSEY_INLINE_JOURNAL;journalSequence=0;
        bytes=0;bufferedBytes=0;segmentCrcState=0xffffffffu;odysseyWavHeader(header,0);
        if (!odysseyPwriteAll(file,header,sizeof(header),0)) {
          failed=true;storageFailed=true;failureStage=failure(33);break;
        }
        if (lseek(file,ODYSSEY_WAV_HEADER_BYTES,SEEK_SET)<0) {
          failed=true;storageFailed=true;failureStage=failure(34);break;
        }
        if (!odysseyJournalCommit(file,journal,fullPath,0,journalSequence)) {
          failed=true;storageFailed=true;failureStage=failure(35);break;
        }
        checkpointAt=millis();
      }

      if (bufferedBytes+sizeof(pcm)>ODYSSEY_SD_WRITE_BUFFER_BYTES) {
        if (!odysseyDrainPcmBuffer(file,odysseySdWriteBuffer,bufferedBytes,bytes,false)) {
          failed=true;storageFailed=true;failureStage=failure(5);break;
        }
      }
      if (bufferedBytes+sizeof(pcm)>ODYSSEY_SD_WRITE_BUFFER_BYTES) {
        failed=true;failureStage=failure(5);break;
      }
      memcpy(odysseySdWriteBuffer+bufferedBytes,pcm,sizeof(pcm));
      bufferedBytes+=sizeof(pcm);
      segmentCrcState=odysseySdCrcUpdate(segmentCrcState,
        reinterpret_cast<const uint8_t*>(pcm),sizeof(pcm));

      if (bufferedBytes>=ODYSSEY_SD_WRITE_CHUNK_BYTES+ODYSSEY_SD_SECTOR_BYTES) {
        if (!odysseyDrainPcmBuffer(file,odysseySdWriteBuffer,bufferedBytes,bytes,false)) {
          failed=true;storageFailed=true;failureStage=failure(5);break;
        }
      }

      // Commit progress without repeatedly overwriting the WAV header sector.
      if (uint32_t(millis()-checkpointAt)>=ODYSSEY_WAV_CHECKPOINT_MS) {
        if (!odysseyDrainPcmBuffer(file,odysseySdWriteBuffer,bufferedBytes,bytes,false) ||
            !odysseyJournalCommit(file,journal,fullPath,bytes,journalSequence)) {
          failed=true;storageFailed=true;failureStage=failure(6);break;
        }
        checkpointAt=millis();
      }

      if (uint64_t(bytes)+bufferedBytes==ODYSSEY_WAV_SEGMENT_PCM_BYTES) {
        if (!odysseyFinalizeWav(file,journal,fullPath,journalSequence,header,odysseySdWriteBuffer,bufferedBytes,bytes)) {
          failed=true;storageFailed=true;failureStage=failure(6);break;
        }
        const uint32_t completedBytes=bytes;
        const uint32_t completedCrc=~segmentCrcState;
        totalBytes+=completedBytes;
        bytes=0;
        const int completed=file;file=-1;
        int closeError=0;
        errno=0;
        if (close(completed)!=0) closeError=errno?errno:EIO;
        journal=ODYSSEY_INLINE_JOURNAL;
        if (closeError) {
          errno=closeError;
          failed=true;storageFailed=true;failureStage=failure(7);break;
        }
        if (!odysseyWriteWavMeta(fullPath,takeHigh,takeLow,segment,completedBytes,completedCrc))
          Serial.printf("[SD] metadata sidecar deferred for part %lu errno=%d\n",
            static_cast<unsigned long>(segment),errno);
        ++segment;
        segmentCrcState=0xffffffffu;
      }
    }
    odysseyCaptureActive=false;
    updateStatusLed(true);
    stopMicrophone();
  }
#else
  failed=true;failureStage=failure(4);
#endif

  if (file>=0) {
    // Do not retry writes or rewrite the header after FatFs reports a storage
    // error: a failed FatFs file object may be aborted, and the seek position
    // may no longer be trustworthy. Close is still attempted for cleanup.
    if (!storageFailed) {
      if (!odysseyFinalizeWav(file,journal,fullPath,journalSequence,header,odysseySdWriteBuffer,bufferedBytes,bytes)) {
        failed=true;storageFailed=true;if (!failureStage) failureStage=failure(6);
      }
    }
    totalBytes+=bytes;
    errno=0;
    if (close(file)!=0) {
      const int closeError=errno?errno:EIO;
      failed=true;storageFailed=true;
      if (!failureStage) { errno=closeError;failureStage=failure(7); }
    }
    file=-1;
  }

  // New recordings keep recovery commits inside the WAV reservation, so there
  // is no second descriptor to close and no .jrn unlink on the clean path.
  journal=ODYSSEY_INLINE_JOURNAL;

  if (!storageFailed && bytes && fullPath[0]) {
    const uint32_t finalCrc=~segmentCrcState;
    if (!odysseyWriteWavMeta(fullPath,takeHigh,takeLow,segment,bytes,finalCrc))
      Serial.printf("[SD] metadata sidecar deferred for final part %lu errno=%d\n",
        static_cast<unsigned long>(segment),errno);
  }

  // A user can stop before the first complete PCM frame, and I2S can fail at
  // startup. Do not leave an empty, otherwise-valid WAV in the sync catalogue.
  if (!storageFailed && totalBytes==0 && fullPath[0] && unlink(fullPath)!=0 && errno!=ENOENT) {
    failed=true;storageFailed=true;if (!failureStage) failureStage=failure(7);
  }

  if (!failed && totalBytes==0) failureStage=failure(8);
  odysseyRecordFailureStage=failureStage;
  odysseyRecordLastBytes=uint32_t(std::min<uint64_t>(totalBytes,0xffffffffull));

  Serial.printf("[SD] local audio %s: %08lx_%08lx, %llu PCM bytes, segments=%lu stage=%u buffer=%u chunk=%u segment=%lus checkpoint=%lus%s\n",
    failed?"failed":"saved",static_cast<unsigned long>(takeHigh),static_cast<unsigned long>(takeLow),
    static_cast<unsigned long long>(totalBytes),static_cast<unsigned long>(segment+1),unsigned(failureStage),
    unsigned(ODYSSEY_SD_WRITE_BUFFER_BYTES),unsigned(ODYSSEY_SD_WRITE_CHUNK_BYTES),
    static_cast<unsigned long>(ODYSSEY_WAV_SEGMENT_SECONDS),
    static_cast<unsigned long>(ODYSSEY_WAV_CHECKPOINT_MS/1000u),
    failed?" (mount retained for explicit recovery)":"");
  if (failed) odysseySaveRecordFailure(failureStage,firstErrno,odysseyRecordLastBytes.load());
  else if (totalBytes) odysseySaveRecordFailure(0,0,0);
  if (storageFailed) { errno=firstErrno?firstErrno:EIO;odysseySdMarkVfsFailure(); }
  if (failed || totalBytes==0) odysseyRecordFaultAt=millis();
}
// FreeRTOS self-deletion skips C++ stack unwinding; return from a separate
// function first so SD and microphone guards release their mutexes.
static void odysseyRecordTask(void*) {
  odysseyCaptureActive=false;
  bool ready=odysseySdReady();
  bool captureAttempted=false,startRecoveryAttempted=false;
  if (!ready && !odysseyStopRequested.load()) {
    startRecoveryAttempted=true;
    odysseySdRecoveryActive=true;
    updateStatusLed(true);
    Serial.println("[SD] one-gesture offline start: recovering storage before capture");
    ready=odysseyRecoverSdCard("touch");
    if (ready) {
      Serial.println("[SD] offline recovery succeeded; preparing capture from original double tap");
    } else {
      odysseySdRecoveryActive=false;
    }
  }

  if (ready && !odysseyStopRequested.load()) {
    captureAttempted=true;
    odysseyRecordTake();
  } else if (!ready) {
    errno=ENODEV;
    odysseyRecordFailureStage=1;
    odysseyRecordLastBytes=0;
    odysseySaveRecordFailure(1,ENODEV,0);
    odysseyRecordFaultAt=millis();
    Serial.println("[SD] offline recovery failed; recording did not start");
  }

  // After checked fsync/close returns, give the SD card time to finish any
  // internal flash programming before power management is allowed to tear
  // down the SPI host. This also resets the disconnected idle window after
  // every offline take instead of inheriting a stale BLE disconnect timestamp.
  const uint32_t finalizedAt=millis();
  odysseySdSleepGuardUntil=finalizedAt+5000u;
  disconnectedAt=finalizedAt;
  odysseyCaptureActive=false;
  odysseySdRecoveryActive=false;
  odysseyRecording=false;
  odysseyStopRequested=false;
  applyCpuPowerProfile(false);
  updateStatusLed(true);

  // A media I/O failure invalidates the current VFS, but a disconnected device
  // must not depend on the phone for recovery. Prepare the next take only; never
  // append to or retry the failed WAV.
  if (captureAttempted && !startRecoveryAttempted && !odysseySdReady() &&
      !deviceConnected.load() && !streamingEnabled.load() &&
      !otaBusy() && !sleepPending && !batteryCritical()) {
    odysseySdRecoveryActive=true;
    updateStatusLed(true);
    delay(750u);
    Serial.println("[SD] autonomous post-record recovery");
    (void)odysseyRecoverSdCard("post-record");
    odysseySdRecoveryActive=false;
    updateStatusLed(true);
  }
  vTaskDelete(nullptr);
}
bool odysseyPrepareForConnectedStreaming(uint32_t timeoutMs) {
  if (!odysseyRecording.load()) return true;
  // Connected PWA capture owns future I2S access, but the disconnected SD take
  // must close its WAV header/file before the microphone can change owners.
  odysseyStopRequested=true;
  Serial.println("[SD] BLE capture requested; finalizing local audio before live stream");
  const uint32_t started=millis();
  while (odysseyRecording.load() && uint32_t(millis()-started)<timeoutMs) delay(10);
  if (odysseyRecording.load()) {
    Serial.println("[SD] local audio did not finalize before BLE capture deadline");
    return false;
  }
  Serial.println("[SD] local audio finalized; microphone released to BLE capture");
  return true;
}
void odysseyToggleRecording() {
  if (odysseySdRecoveryActive.load()) return;
  if (odysseyRecording.load()) {
    odysseyStopRequested=true;
    updateStatusLed(true);
    Serial.println("[TOUCH] double tap -> SD audio STOP");
    return;
  }
  if (deviceConnected.load() || streamingEnabled.load() || otaBusy() || sleepPending || batteryCritical()) return;
  odysseyStopRequested=false;
  odysseyRecordFaultAt=0;
  odysseyCaptureActive=false;
  odysseyRecording=true;
  // Amber covers all preparation, including an already-mounted card; purple is
  // asserted only after microphone startup inside odysseyRecordTake().
  odysseySdRecoveryActive=true;
  applyCpuPowerProfile(true);
  updateStatusLed(true);
  if (xTaskCreate(odysseyRecordTask,"sd-audio",8192,nullptr,2,nullptr)!=pdPASS) {
    odysseyCaptureActive=false;
    odysseyRecording=false;
    odysseyStopRequested=false;
    odysseySdRecoveryActive=false;
    odysseyRecordFaultAt=millis();
    applyCpuPowerProfile(false);
    updateStatusLed(true);
    Serial.println("[SD] local audio task allocation failed");
    return;
  }
  Serial.println(odysseySdReady()?
    "[TOUCH] double tap -> SD audio START":
    "[TOUCH] double tap -> SD recover + audio START");
}
#endif
// Odyssey C3 SD media-v1: catalogue/read/delete for locally recorded WAV files.
// C3 storage is mounted by the proven Arduino SPI/SD path and accessed through FAT/VFS.
// Files are deleted only after the PWA has imported and verified them.
bool odysseyFormatSdCard();
uint8_t odysseySdRecordFailureStage();
uint32_t odysseySdRecordLastBytes();
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
namespace OdysseyTransfer {
struct Request {
  uint32_t connection=0,id=0,offset=0;
  uint8_t operation=0;
  char path[64]{};
};
static QueueHandle_t requests=nullptr;
static constexpr uint32_t TRANSFER_STACK_BYTES=8192;
static portMUX_TYPE responseMux=portMUX_INITIALIZER_UNLOCKED;
static uint8_t response[496]{};
static size_t responseSize=16;
static uint32_t responseConnection=0;
static char selectedPath[64]{};
static String catalogueBuffer;
static int catalogueErrno=0;

enum : uint8_t {
  OK=0,BUSY=1,BAD_COMMAND=2,NO_SD=3,IO_ERROR=7,FILE_UNAVAILABLE=11
};

static bool safeWavPath(const char* path) {
  if (!path || strncmp(path,"/synap/",7)!=0) return false;
  const size_t n=strlen(path);
  if (n<12 || n>63 ||
      (strcmp(path+n-4,".wav")!=0 && strcmp(path+n-4,".WAV")!=0)) return false;
  for (size_t i=7;i<n-4;++i) {
    const char c=path[i];
    if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')) return false;
  }
  return true;
}
static bool storageReady() {
  // Normal PWA reads are observational only. Only operation 14 may remount.
  return odysseySdReady();
}
static bool fullPath(const char* logical,char* full,size_t capacity) {
  return safeWavPath(logical) && odysseySdPath(logical,full,capacity);
}
static bool isSegmentedRecording(const char* path) {
  return path && strstr(path,"_p")!=nullptr;
}
// 1=complete and safe to expose, 0=incomplete/content-invalid, -1=filesystem I/O fault.
static int segmentedWavState(const char* fullPath,const struct stat& st) {
  const int journal=odysseyJournalPresence(fullPath);
  if (journal<0) return -1;
  if (journal>0) { errno=0;return 0; }
  errno=0;
  const int fd=open(fullPath,O_RDONLY);
  if (fd<0) return -1;
  uint8_t h[44]{};uint32_t audioBytes=0;
  const bool read=odysseyPreadAll(fd,h,sizeof(h),0);
  const int readError=read?0:(errno?errno:EIO);
  errno=0;
  const bool closed=close(fd)==0;
  const int closeError=closed?0:(errno?errno:EIO);
  if (!read || !closed) {
    errno=readError?readError:closeError;
    return -1;
  }
  if (!odysseyWavValid(h,audioBytes) || uint64_t(st.st_size)!=44ull+audioBytes) {
    errno=0;
    return 0;
  }
  errno=0;
  return 1;
}

static void reply(const Request& request,uint8_t error,uint32_t total=0,uint32_t offset=0,
                  const uint8_t* bytes=nullptr,size_t size=0) {
  uint8_t value[496]{};
  value[0]=0xCB;value[1]=1;value[2]=error?2:1;value[3]=error;
  put32le(value+4,request.id);put32le(value+8,total);put32le(value+12,offset);
  size=std::min(size,size_t(480));
  if (size && bytes) memcpy(value+16,bytes,size);
  portENTER_CRITICAL(&responseMux);
  if (request.connection==connectionGeneration.load()) {
    memcpy(response,value,16+size);
    responseSize=16+size;
    responseConnection=request.connection;
  }
  portEXIT_CRITICAL(&responseMux);
}

static uint8_t selectFile(const char* path,uint32_t& total) {
  selectedPath[0]=0;
  if (!safeWavPath(path)) return BAD_COMMAND;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  char full[96];
  if (!fullPath(path,full,sizeof(full))) return BAD_COMMAND;
  struct stat st{};
  errno=0;
  if (stat(full,&st)!=0) return errno==ENOENT?FILE_UNAVAILABLE:IO_ERROR;
  if (!S_ISREG(st.st_mode) || st.st_size<=0) return FILE_UNAVAILABLE;
  if (isSegmentedRecording(path)) {
    const int state=segmentedWavState(full,st);
    if (state<0) return IO_ERROR;
    if (!state) return FILE_UNAVAILABLE;
  }
  if (uint64_t(st.st_size)>UINT32_MAX) return FILE_UNAVAILABLE;
  total=uint32_t(st.st_size);
  snprintf(selectedPath,sizeof(selectedPath),"%s",path);
  return OK;
}

static uint8_t readSelected(const char* requestedPath,uint32_t offset,uint32_t& total,uint8_t* bytes,size_t& size) {
  // New clients make every chunk self-describing. Cache is scoped to path and
  // BLE generation; pread supports retransmission without shared seek state.
  const char* path=(requestedPath && requestedPath[0])?requestedPath:selectedPath;
  if (!path[0] || !strcmp(path,"@catalogue")) {
    total=catalogueBuffer.length();
    if (!total || offset>=total) return FILE_UNAVAILABLE;
    size=std::min(size_t(480),size_t(total-offset));
    memcpy(bytes,catalogueBuffer.c_str()+offset,size);
    return OK;
  }
  if (!safeWavPath(path)) return BAD_COMMAND;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  char full[96];
  if (!fullPath(path,full,sizeof(full))) return BAD_COMMAND;
  const uint32_t generation=connectionGeneration.load();
  if (odysseySdReadFd>=0 && (odysseySdReadConnection!=generation || strcmp(odysseySdReadPath,full)!=0))
    if (!odysseySdCloseReadLocked()) return IO_ERROR;
  if (odysseySdReadFd<0) {
    struct stat st{};
    errno=0;
    if (stat(full,&st)!=0) return errno==ENOENT?FILE_UNAVAILABLE:IO_ERROR;
    if (!S_ISREG(st.st_mode) || st.st_size<=0 || uint64_t(st.st_size)>UINT32_MAX)
      return FILE_UNAVAILABLE;
    if (isSegmentedRecording(path)) {
      const int state=segmentedWavState(full,st);
      if (state<0) return IO_ERROR;
      if (!state) return FILE_UNAVAILABLE;
    }
    errno=0;
    odysseySdReadFd=open(full,O_RDONLY);
    if (odysseySdReadFd<0) return IO_ERROR;
    snprintf(odysseySdReadPath,sizeof(odysseySdReadPath),"%s",full);
    odysseySdReadConnection=generation;
    Serial.printf("[SD] transfer begin path=%s\n",path);
  }
  struct stat st{};
  errno=0;
  if (fstat(odysseySdReadFd,&st)!=0) {
    const int saved=errno?errno:EIO;(void)odysseySdCloseReadLocked();errno=saved;return IO_ERROR;
  }
  if (st.st_size<0 || uint64_t(st.st_size)>UINT32_MAX) {
    (void)odysseySdCloseReadLocked();return FILE_UNAVAILABLE;
  }
  total=uint32_t(st.st_size);
  if (offset>=total) { (void)odysseySdCloseReadLocked();return FILE_UNAVAILABLE; }
  size=std::min(size_t(480),size_t(total-offset));
  const bool ok=odysseyPreadAll(odysseySdReadFd,bytes,size,off_t(offset));
  odysseySdReadAt=millis();
  if (!ok || uint64_t(offset)+size==total) {
    const bool closed=odysseySdCloseReadLocked();
    if (!ok || !closed) return IO_ERROR;
  }
  return OK;
}

static uint8_t catalogue(uint32_t& total) {
  selectedPath[0]=0;
  catalogueBuffer="";
  catalogueErrno=0;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  errno=0;
  if (!odysseySdCloseReadLocked()) {
    catalogueErrno=errno?errno:EIO;
    return IO_ERROR;
  }

  char directoryPath[96];
  if (!odysseySdPath("/synap",directoryPath,sizeof(directoryPath))) {
    catalogueErrno=EINVAL; return IO_ERROR;
  }
  DIR* directory=opendir(directoryPath);
  if (!directory) {
    catalogueErrno=errno;
    Serial.printf("[SD] catalogue opendir failed errno=%d path=%s\n",catalogueErrno,directoryPath);
    return IO_ERROR;
  }

  // Integrity/session metadata is optional and adds ~80 bytes per WAV. Reserve
  // once so a full 100-entry catalogue does not repeatedly fragment C3 heap.
  if (!catalogueBuffer.reserve(22528)) {
    catalogueErrno=ENOMEM;closedir(directory);return IO_ERROR;
  }
  catalogueBuffer="[";
  unsigned count=0;
  for (;;) {
    errno=0;
    dirent* entry=readdir(directory);
    if (!entry) {
      if (errno) catalogueErrno=errno;
      break;
    }
    if (!entry->d_name || !strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
    char logical[64];
    const int n=snprintf(logical,sizeof(logical),"/synap/%s",entry->d_name);
    if (n<=0 || size_t(n)>=sizeof(logical) || !safeWavPath(logical)) continue;
    char full[96];
    if (!odysseySdPath(logical,full,sizeof(full))) continue;
    struct stat st{};
    errno=0;
    if (stat(full,&st)!=0) {
      if (errno==ENOENT) continue;
      catalogueErrno=errno?errno:EIO;
      break;
    }
    if (!S_ISREG(st.st_mode) || st.st_size<0 || uint64_t(st.st_size)>UINT32_MAX) continue;
    if (isSegmentedRecording(logical)) {
      const int state=segmentedWavState(full,st);
      if (state<0) { catalogueErrno=errno?errno:EIO;break; }
      if (!state) continue;
    }
    OdysseyWavMeta meta{};
    const int metaState=odysseyReadWavMeta(full,meta.takeHigh,meta.takeLow,
      meta.part,meta.pcmBytes,meta.crc32);
    if (metaState==-2) { catalogueErrno=errno?errno:EIO;break; }
    // Invalid/missing metadata never hides a valid WAV. Existing PWA SHA
    // verification remains the fallback for recovered or legacy recordings.
    const bool metaValid=metaState==1 &&
      uint64_t(meta.pcmBytes)+44ull==uint64_t(st.st_size);
    // Keep the C3 heap bounded, but never present a truncated catalogue as
    // complete. A caller can clear/sync files and retry after an explicit
    // overflow instead of silently orphaning everything beyond entry 100.
    if (count>=100) { catalogueErrno=EOVERFLOW;break; }
    if (count++) catalogueBuffer+=",";
    catalogueBuffer+="{\"path\":\""+String(logical)+"\",\"bytes\":"+String(uint32_t(st.st_size));
    if (metaValid) {
      char take[17];
      snprintf(take,sizeof(take),"%08lx%08lx",
        static_cast<unsigned long>(meta.takeHigh),static_cast<unsigned long>(meta.takeLow));
      catalogueBuffer+=" ,\"take\":\""+String(take)+"\",\"part\":"+String(meta.part)+
        ",\"pcmBytes\":"+String(meta.pcmBytes)+",\"crc32\":"+String(meta.crc32);
    }
    catalogueBuffer+="}";
  }
  if (closedir(directory)!=0 && !catalogueErrno) catalogueErrno=errno;
  if (catalogueErrno) {
    catalogueBuffer="";
    Serial.printf("[SD] catalogue readdir/closedir failed errno=%d\n",catalogueErrno);
    return IO_ERROR; // Never send a silently truncated catalogue after an I/O fault.
  }
  catalogueBuffer+="]";
  total=catalogueBuffer.length();
  return OK;
}

static uint8_t removeFileLocked(const char* path) {
  char full[96];
  if (!fullPath(path,full,sizeof(full))) return BAD_COMMAND;

  // Deletion is intentionally idempotent and ordered WAV -> journal. If power
  // disappears between the two unlinks, the remaining journal keeps any
  // unfinished WAV hidden/recoverable. A retry can safely finish cleanup.
  struct stat st{};
  errno=0;
  const int statResult=stat(full,&st);
  if (statResult!=0 && errno!=ENOENT) return IO_ERROR;
  if (statResult==0 && !S_ISREG(st.st_mode)) return FILE_UNAVAILABLE;

  errno=0;
  if (!odysseySdCloseReadLocked()) return IO_ERROR;

  if (statResult==0) {
    errno=0;
    if (unlink(full)!=0 && errno!=ENOENT) return IO_ERROR;
    errno=0;
    if (stat(full,&st)==0) { errno=EIO;return IO_ERROR; }
    if (errno!=ENOENT) return IO_ERROR;
  }

  errno=0;
  if (!odysseyRemoveJournal(full)) return IO_ERROR;
  errno=0;
  if (!odysseyRemoveMeta(full)) return IO_ERROR;

  // Verify WAV, journal and integrity metadata are all gone before acknowledging deletion.
  errno=0;
  if (stat(full,&st)==0) { errno=EIO;return IO_ERROR; }
  if (errno!=ENOENT) return IO_ERROR;
  const int journal=odysseyJournalPresence(full);
  if (journal<0) return IO_ERROR;
  if (journal>0) { errno=EIO;return IO_ERROR; }
  OdysseyWavMeta meta{};
  const int metaState=odysseyReadWavMeta(full,meta.takeHigh,meta.takeLow,
    meta.part,meta.pcmBytes,meta.crc32);
  if (metaState==-2) return IO_ERROR;
  if (metaState!=0) { errno=EIO;return IO_ERROR; }
  errno=0;
  return OK;
}

static uint8_t removeFile(const char* path) {
  selectedPath[0]=0;
  if (!safeWavPath(path)) return BAD_COMMAND;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  return removeFileLocked(path);
}

static bool orphanJournalPath(const char* name,char* full,size_t capacity) {
  if (!name || !full || capacity<2) return false;
  const size_t length=strlen(name);
  static constexpr char suffix[]=".wav.jrn";
  if (length<=sizeof(suffix)-1 || strcmp(name+length-(sizeof(suffix)-1),suffix)!=0) return false;
  char logicalWav[64];
  const int n=snprintf(logicalWav,sizeof(logicalWav),"/synap/%.*s",
    int(length-4),name); // strip only ".jrn"; safeWavPath validates the base
  if (n<=0 || size_t(n)>=sizeof(logicalWav) || !safeWavPath(logicalWav)) return false;
  char fullWav[96];
  return odysseySdPath(logicalWav,fullWav,sizeof(fullWav)) &&
    odysseyJournalPath(fullWav,full,capacity);
}
static bool orphanMetaPath(const char* name,char* full,size_t capacity) {
  if (!name || !full || capacity<2) return false;
  const size_t length=strlen(name);
  static constexpr char suffix[]=".wav.meta";
  if (length<=sizeof(suffix)-1 || strcmp(name+length-(sizeof(suffix)-1),suffix)!=0) return false;
  char logicalWav[64];
  const int n=snprintf(logicalWav,sizeof(logicalWav),"/synap/%.*s",
    int(length-5),name); // strip ".meta"; safeWavPath validates the WAV base
  if (n<=0 || size_t(n)>=sizeof(logicalWav) || !safeWavPath(logicalWav)) return false;
  char fullWav[96];
  return odysseySdPath(logicalWav,fullWav,sizeof(fullWav)) &&
    odysseyMetaPath(fullWav,full,capacity);
}

static uint8_t clearRecordings(uint32_t& removed) {
  removed=0;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  errno=0;
  if (!odysseySdCloseReadLocked()) return IO_ERROR;
  char directoryPath[96];
  if (!odysseySdPath("/synap",directoryPath,sizeof(directoryPath))) { errno=EINVAL;return IO_ERROR; }

  // Reopen between bounded batches. This avoids holding a DIR handle while
  // unlinking entries and removes every recording without a fixed 100-file cap.
  for (;;) {
    char batch[16][64]{};
    uint8_t count=0;
    int scanError=0;
    errno=0;
    DIR* directory=opendir(directoryPath);
    if (!directory) return IO_ERROR;
    for (;;) {
      errno=0;
      dirent* entry=readdir(directory);
      if (!entry) { if (errno) scanError=errno;break; }
      if (!entry->d_name || !strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
      char logical[64];
      const int n=snprintf(logical,sizeof(logical),"/synap/%s",entry->d_name);
      if (n<=0 || size_t(n)>=sizeof(logical) || !safeWavPath(logical)) continue;
      char full[96];
      if (!odysseySdPath(logical,full,sizeof(full))) { scanError=EINVAL;break; }
      struct stat st{};
      errno=0;
      if (stat(full,&st)!=0) {
        if (errno==ENOENT) continue;
        scanError=errno?errno:EIO;break;
      }
      if (S_ISREG(st.st_mode)) {
        snprintf(batch[count],sizeof(batch[count]),"%s",logical);
        ++count;
      }
      if (count==16) break;
    }
    if (closedir(directory)!=0 && !scanError) scanError=errno?errno:EIO;
    if (scanError) { errno=scanError;return IO_ERROR; }
    if (!count) break;

    for (uint8_t i=0;i<count;++i) {
      const uint8_t result=removeFileLocked(batch[i]);
      if (result==OK) ++removed;
      else if (result!=FILE_UNAVAILABLE) return result;
    }
  }

  // Clean journals that lost their WAV due to a previous interrupted firmware
  // version. These are Synap-owned recovery metadata only; unrelated card
  // content is never touched.
  for (;;) {
    char journals[16][144]{};
    uint8_t count=0;
    int scanError=0;
    errno=0;
    DIR* directory=opendir(directoryPath);
    if (!directory) return IO_ERROR;
    for (;;) {
      errno=0;
      dirent* entry=readdir(directory);
      if (!entry) { if (errno) scanError=errno;break; }
      if (!entry->d_name || !strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
      char journal[144];
      if (!orphanJournalPath(entry->d_name,journal,sizeof(journal))) continue;
      struct stat st{};
      errno=0;
      if (stat(journal,&st)!=0) {
        if (errno==ENOENT) continue;
        scanError=errno?errno:EIO;break;
      }
      if (S_ISREG(st.st_mode)) {
        snprintf(journals[count],sizeof(journals[count]),"%s",journal);
        ++count;
      }
      if (count==16) break;
    }
    if (closedir(directory)!=0 && !scanError) scanError=errno?errno:EIO;
    if (scanError) { errno=scanError;return IO_ERROR; }
    if (!count) break;

    for (uint8_t i=0;i<count;++i) {
      const char* journal=journals[i];
      errno=0;
      if (unlink(journal)!=0 && errno!=ENOENT) return IO_ERROR;
      struct stat st{};
      errno=0;
      if (stat(journal,&st)==0) { errno=EIO;return IO_ERROR; }
      if (errno!=ENOENT) return IO_ERROR;
      errno=0;
    }
  }

  // Remove orphan integrity sidecars left if power disappeared after the WAV
  // was deleted but before metadata cleanup completed.
  for (;;) {
    char metadata[16][144]{};
    uint8_t count=0;
    int scanError=0;
    errno=0;
    DIR* directory=opendir(directoryPath);
    if (!directory) return IO_ERROR;
    for (;;) {
      errno=0;
      dirent* entry=readdir(directory);
      if (!entry) { if (errno) scanError=errno;break; }
      if (!entry->d_name || !strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
      char metaPath[144];
      if (!orphanMetaPath(entry->d_name,metaPath,sizeof(metaPath))) continue;
      struct stat st{};
      errno=0;
      if (stat(metaPath,&st)!=0) {
        if (errno==ENOENT) continue;
        scanError=errno?errno:EIO;break;
      }
      if (S_ISREG(st.st_mode)) {
        snprintf(metadata[count],sizeof(metadata[count]),"%s",metaPath);
        ++count;
      }
      if (count==16) break;
    }
    if (closedir(directory)!=0 && !scanError) scanError=errno?errno:EIO;
    if (scanError) { errno=scanError;return IO_ERROR; }
    if (!count) break;

    for (uint8_t i=0;i<count;++i) {
      errno=0;
      if (unlink(metadata[i])!=0 && errno!=ENOENT) return IO_ERROR;
    }
  }

  errno=0;
  return OK;
}

static void worker(void*) {
  Request request;
  uint8_t bytes[480];
  for (;;) {
    {
      OdysseySdGuard guard(0);
      if (guard && odysseySdReadFd>=0 && (!deviceConnected.load() ||
          odysseySdReadConnection!=connectionGeneration.load() ||
          uint32_t(millis()-odysseySdReadAt)>=15000u || odysseyRecording.load() || sleepPending)) {
        if (!odysseySdCloseReadLocked()) odysseySdMarkVfsFailure();
      }
    }

    if (xQueueReceive(requests,&request,pdMS_TO_TICKS(500))!=pdTRUE) {
      // The media worker is observational while idle. Offline touch recovery
      // belongs exclusively to odysseyRecordTask(); connected remount belongs
      // exclusively to explicit PWA operation 14.
      continue;
    }
    if (request.connection!=connectionGeneration.load() || !deviceConnected.load()) continue;
    // Connected remote standby only idles the microphone/CPU; SD media must
    // remain readable for verified sync and recovery without a forced wake.
    if (odysseyRecording.load() || streamingEnabled.load() || otaBusy() || sleepPending) {
      reply(request,BUSY);continue;
    }
    uint8_t error=OK;uint32_t total=0;size_t size=0;
    switch (request.operation) {
      case 3:
        error=selectFile(request.path,total);
        if (error==IO_ERROR) odysseySdMarkVfsFailure();
        break;
      case 4:
        error=readSelected(request.path,request.offset,total,bytes,size);
        if (error==IO_ERROR) odysseySdMarkVfsFailure();
        break;
      case 7:
        error=catalogue(total);
        // Never auto-unmount/remount a mounted card because a catalogue read
        // failed. Preserve the observed state for diagnosis; explicit op 14 is
        // the only connected remount path.
        if (error==IO_ERROR) { errno=catalogueErrno?catalogueErrno:(errno?errno:EIO);odysseySdMarkVfsFailure(); }
        break;
      case 8: total=catalogueBuffer.length();if(!total)error=FILE_UNAVAILABLE;break;
      case 14:
        selectedPath[0]=0;catalogueBuffer="";
        error=odysseyRecoverSdCard("op14")?OK:NO_SD;
        break;
      case 17: error=removeFile(request.path);if(error==IO_ERROR)odysseySdMarkVfsFailure();break;
      case 18:
        selectedPath[0]=0;catalogueBuffer="";
        if(!storageReady()) error=NO_SD;
        else {
          error=clearRecordings(total);
          if (error==IO_ERROR) odysseySdMarkVfsFailure();
        }
        break;
      case 19:
        selectedPath[0]=0;catalogueBuffer="";
        error=odysseyFormatSdCard()?OK:IO_ERROR;
        break;
      default:error=BAD_COMMAND;break;
    }
    if (request.operation==7 && (error==IO_ERROR || error==NO_SD)) {
      char detail[480];
      const int n=snprintf(detail,sizeof(detail),
        "{\"stage\":\"catalogue\",\"errno\":%d,\"sdState\":%u,\"sdProbe\":%u,\"espErr\":%ld,\"ioErrno\":%ld,\"releaseErr\":%ld,\"freeBytes\":%llu,\"mountAttempts\":%lu,\"beginAttempts\":%lu,\"releaseAttempts\":%lu,\"mountWhy\":%u,\"recordStage\":%u,\"recordBytes\":%lu,\"lastRecordStage\":%lu,\"lastRecordErrno\":%ld,\"lastRecordBytes\":%lu,\"lastRecordBuild\":%lu}",
        catalogueErrno,unsigned(odysseySdDetectionState()),unsigned(odysseySdProbeState()),
        static_cast<long>(odysseySdLastError()),static_cast<long>(odysseySdLastIoError()),
        static_cast<long>(odysseySdLastReleaseErrorCode()),static_cast<unsigned long long>(odysseySdLastFreeByteCount()),
        static_cast<unsigned long>(odysseySdAttemptCount()),static_cast<unsigned long>(odysseySdBeginAttemptCount()),
        static_cast<unsigned long>(odysseySdReleaseAttemptCount()),unsigned(odysseySdLastMountReasonCode()),
        unsigned(odysseySdRecordFailureStage()),static_cast<unsigned long>(odysseySdRecordLastBytes()),
        static_cast<unsigned long>(odysseyStoredStage),static_cast<long>(odysseyStoredErrno),
        static_cast<unsigned long>(odysseyStoredBytes),static_cast<unsigned long>(odysseyStoredBuild));
      reply(request,error,total,request.offset,reinterpret_cast<const uint8_t*>(detail),
        n>0?std::min(size_t(n),sizeof(detail)-1):0);
    } else reply(request,error,total,request.offset,bytes,size);
  }
}

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    const size_t length=characteristic->getLength();
    const uint8_t* p=characteristic->getData();
    if (!p || length<10 || length>73 || p[0]!=0xCA) return;
    Request request{};
    request.operation=p[1];
    memcpy(&request.id,p+2,4);
    memcpy(&request.offset,p+6,4);
    request.connection=connectionGeneration.load();
    const size_t pathLength=length-10;
    if (pathLength) memcpy(request.path,p+10,pathLength);
    request.path[pathLength]=0;
    if (!requests || xQueueSend(requests,&request,0)!=pdTRUE) reply(request,BUSY);
  }
};
class DataCallbacks : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* characteristic) override {
    uint8_t value[496];size_t size=16;
    portENTER_CRITICAL(&responseMux);
    if (responseConnection==connectionGeneration.load()) {
      size=responseSize;memcpy(value,response,size);
    } else {
      memset(value,0,size);value[0]=0xCB;value[1]=1;
    }
    portEXIT_CRITICAL(&responseMux);
    characteristic->setValue(value,size);
  }
};

bool available(){return requests!=nullptr;}

void initialize() {
  requests=xQueueCreate(2,sizeof(Request));
  if (!requests || xTaskCreate(worker,"odyssey-sd",TRANSFER_STACK_BYTES,nullptr,1,nullptr)!=pdPASS) {
    if (requests) vQueueDelete(requests);
    requests=nullptr;
    Serial.println("[SD] BLE transfer worker unavailable");
    return;
  }
  Request initial{};initial.connection=connectionGeneration.load();reply(initial,OK);
}

void ble(BLEService* service) {
  auto* command=service->createCharacteristic("4fa12354-0000-1000-8000-00805f9b34fb",
    BLECharacteristic::PROPERTY_WRITE|BLECharacteristic::PROPERTY_WRITE_NR);
  command->setCallbacks(new CommandCallbacks());
  auto* data=service->createCharacteristic("4fa12355-0000-1000-8000-00805f9b34fb",
    BLECharacteristic::PROPERTY_READ);
  data->setCallbacks(new DataCallbacks());
}
} // namespace OdysseyTransfer
#endif
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
  BLEService* service=bleServer->createService(BLEUUID(SERVICE_UUID),64);
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
  recoveryCharacteristic=service->createCharacteristic(RECOVERY_CHAR_UUID,BLECharacteristic::PROPERTY_READ|BLECharacteristic::PROPERTY_WRITE|BLECharacteristic::PROPERTY_NOTIFY);
#if defined(CONFIG_BLUEDROID_ENABLED)
  recoveryCharacteristic->addDescriptor(new BLE2902());
#endif
  recoveryCharacteristic->setCallbacks(new RecoveryCallbacks());
  updateDiagnosticsCharacteristic();
  updateStatusCharacteristic(false);
  otaInitialize(service);
  initializeModuleCapabilities(service);
#if SYNAP_CHAKSHU
  ChakshuMedia::ble(service);
  ChakshuTransfer::ble(service);
#elif CONFIG_IDF_TARGET_ESP32C3
  OdysseyTransfer::ble(service);
#endif
  service->start();
  BLEAdvertising* advertising=BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(BLE_MIN_INTERVAL);
  advertising->setMaxPreferred(BLE_MAX_INTERVAL);
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
  // The device profile selects the divider calibration and ADC input range.
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_6db);
  statusLed.begin();
  statusLed.clear();
  statusLed.show();
  if (!confirmTouchWakeGesture()) return;
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
#if SYNAP_CHAKSHU
  ChakshuMedia::initialize();
  ChakshuTransfer::initialize();
#elif CONFIG_IDF_TARGET_ESP32C3
  // Reproduce the last independently observed healthy lifecycle (build 1445 /
  // 1481): create the transfer worker first, then perform one mount before BLE.
  // The worker cannot touch storage until BLE submits a request.
  OdysseyTransfer::initialize();
  odysseyInitializeSdCardBeforeBle();
  // The first battery sample precedes SD probing. Re-sample only when SD
  // hardware was positively observed so standard C3 behavior stays unchanged.
  if (odysseySdBatteryDividerPresent()) sampleBattery(true);
#else
  // Odyssey S3 remains a detection-only target.
  odysseyDetectSdCard();
#endif
  initializeBLE();
  initializeRecovery();
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
  if (!bootValidated) { delay(20); return; }
#endif
  // Recording, BLE and touch run in their own tasks; this loop only validates boot.
  delay(1000);
}
