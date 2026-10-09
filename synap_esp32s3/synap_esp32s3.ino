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
// Latches a failed SD idle/quiesce or unresolved write fault. Never permit a
// later deep-sleep retry to mistake the now-unmounted card for an absent card.
std::atomic<bool> odysseySdUnsafeToSleep{false};
void odysseyToggleRecording();
bool odysseyPrepareForConnectedStreaming(uint32_t timeoutMs);
bool odysseyPrepareSdForPowerTransition(uint32_t timeoutMs);
bool odysseySdBusStuckLow(); // confirmed deselected MISO held LOW after failed mount
namespace OdysseyWifi {
void initialize();
bool available();
bool busy();
uint32_t configChunk(uint32_t offset,const char* text);
bool applyConfig(uint32_t mode);
bool forget();
size_t encode(char* output,size_t capacity);
}
namespace OdysseyTransfer {
void initialize();
void ble(BLEService* service);
bool available();
bool busy(); // in-flight SD BLE transfer plus a brief completion grace.
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
      || odysseyRecording.load() || OdysseyWifi::busy()
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
#if CONFIG_IDF_TARGET_ESP32C3
uint8_t odysseyLastRecordFailureStage();
uint32_t odysseyLastRecordFailureBytes();
#endif
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
  const uint8_t lastRecordStage=odysseyLastRecordFailureStage();
  const uint32_t lastRecordBytes=odysseyLastRecordFailureBytes();
  // Validation byte 15 reports successful PCM before the last failure in
  // 8 KiB units (capped at 255). Byte 19 remains the exact failure/mount stage.
  const uint32_t recordUnits=lastRecordBytes/8192u;
  p[15]=uint8_t(recordUnits>255u?255u:recordUnits);
  p[16]|=0x80; // C3 validation marker: byte 15 is recorder progress, not voice.
  const uint8_t liveProbe=odysseySdProbeState();
  p[16]|=uint8_t((liveProbe<=6u?liveProbe:7u)<<3); // bits 3..5 = live mount probe.
  p[19]=lastRecordStage?lastRecordStage:liveProbe;
#endif
#if CONFIG_IDF_TARGET_ESP32C3
  if (OdysseyTransfer::available()) {
    p[14]=1;
    // C3 media-v1 now supports paced, six-chunk BLE notification windows.
    // Bit 0 enables the client's existing offset-verified window transport;
    // bit 1 remains BLE-controlled Wi-Fi upload. Full-card format stays off.
    p[16]|=1;
    if (OdysseyWifi::available()) p[16]|=2;
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
  } else if (odysseyRecording.load()) {
    // Match build 1631 exactly: purple tracks the local recording task.
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
  // The 80 MHz idle profile was also selected while Bluefy was establishing
  // GATT subscriptions or running connected-idle. Avoid changing CPU clocks
  // underneath the single-core NimBLE host and the C3 SD/Wi-Fi tasks.
  // Give a recently dropped link a brief 12 s recovery window at 160 MHz,
  // then return to 80 MHz while advertising to protect battery life.
  const uint32_t lastDisconnect=lastDisconnectAt.load();
  const bool reconnectWindow=lastDisconnect &&
    uint32_t(millis()-lastDisconnect)<12000u;
  active=active || deviceConnected.load() || reconnectWindow ||
    odysseyRecording.load() || OdysseyWifi::busy() || OdysseyTransfer::busy();
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
#if CONFIG_IDF_TARGET_ESP32C3
  // Preserve each factory-calibrated voltage reading so the very high-source-
  // impedance C3+SD divider can reject isolated ADC spikes. Do not derive
  // voltage from the nominal 12-bit raw code or alter the resistor ratio.
  uint16_t mvSamples[16];
#endif
  for (uint8_t i=0;i<16;++i) {
    rawTotal+=analogRead(BATTERY_ADC_PIN);
    const uint32_t measuredMv=analogReadMilliVolts(BATTERY_ADC_PIN);
    mvTotal+=measuredMv;
#if CONFIG_IDF_TARGET_ESP32C3
    mvSamples[i]=uint16_t(measuredMv>65535u?65535u:measuredMv);
#endif
    delayMicroseconds(250);
  }
  uint32_t adcMv=mvTotal/16u;
  const uint32_t adcRaw=rawTotal/16u;
  bool adcUnstable=false;
#if CONFIG_IDF_TARGET_ESP32C3
  if (odysseySdBatteryDividerPresent()) {
    // R1=1 MOhm, R2=470 kOhm, Rth~320 kOhm (C1=100 nF on Rev K).
    // Trim two extremes on either side. A wide central spread means the
    // voltage is not a trustworthy battery measurement; never fake 100%.
    for (uint8_t i=1;i<16;++i) {
      const uint16_t value=mvSamples[i];
      uint8_t j=i;
      while (j && mvSamples[j-1]>value) {
        mvSamples[j]=mvSamples[j-1];
        --j;
      }
      mvSamples[j]=value;
    }
    uint32_t centralTotal=0;
    for (uint8_t i=2;i<14;++i) centralTotal+=mvSamples[i];
    adcMv=(centralTotal+6u)/12u;
    const uint16_t centralSpread=mvSamples[13]-mvSamples[2];
    adcUnstable=centralSpread>120u;
    if (adcUnstable) {
      Serial.printf("[BATTERY] C3 SD ADC unstable: central range=%u..%umV\n",
        unsigned(mvSamples[2]),unsigned(mvSamples[13]));
    }
  }
#endif
  batteryAdcMillivolts=uint16_t(adcMv>65535u?65535u:adcMv);
  batteryAdcRaw=uint16_t(adcRaw>65535u?65535u:adcRaw);
#if CONFIG_IDF_TARGET_ESP32C3
  const bool dividerWasObserved=odysseySdBatteryDividerPresent();
  if (!dividerWasObserved && detectOdysseySdBatteryDividerFromAdc(adcMv)) {
    Serial.printf("[BATTERY] inferred SD divider from adc=%lumV\n",static_cast<unsigned long>(adcMv));
  }
#endif
  const uint32_t cellMv=batteryCellMillivoltsFromAdc(adcMv);
  if (!adcUnstable && cellMv>=2800u && cellMv<=4350u) {
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
    Serial.println("[POWER] touch wake source could not be armed");
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
    // Before BLE init a failed sleep must return to an awake recovery boot,
    // not leave the pendant inert with its durable sleep lock still set.
    sleepPending=false;
    return;
#else
    delay(250);
    ESP.restart();
    return;
#endif
  }
  synapLastSleepStage=SLEEP_STAGE_ENTERING;
  Serial.printf("[POWER] deep sleep now request=%u gpio=%u\n",
    unsigned(synapSleepRequestCounter),unsigned(digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL));
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  if (!odysseyPrepareSdForPowerTransition(500u)) {
    synapLastSleepStage=SLEEP_STAGE_ABORTED;
    Serial.println("[POWER] wake-gate sleep deferred: C3 SD is not idle");
    sleepPending=false;
    return;
  }
#endif
  esp_deep_sleep_start();
  Serial.println("[POWER] deep sleep returned unexpectedly");
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  sleepPending=false;
  return;
#else
  delay(250);
  ESP.restart();
#endif
}

// A failed early return-to-sleep must not trap the C3 before BLE starts.
static bool resumeC3BootAfterFailedSleep(const char* reason) {
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  if (!writeDurableSleepLock(false))
    Serial.println("[POWER] wake-sleep fallback: NVS unlock failed");
  synapDeepSleepMarker=0;
  sleepPending=false;
  bootSleepWasLocked=false;
  synapLastSleepStage=SLEEP_STAGE_ABORTED;
  Serial.printf("[POWER] continuing awake BLE boot after failed sleep: %s\n",reason?reason:"unknown");
  return true;
#else
  (void)reason;
  return false;
#endif
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
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  if (bootResetReason==ESP_RST_POWERON) {
    synapDeepSleepMarker=0;
    sleepPending=false;
    return true;
  }
#endif
  if (!sleepResume) return true;

  bool touchWake=false;
#if CONFIG_IDF_TARGET_ESP32S3
  touchWake=(cause==ESP_SLEEP_WAKEUP_EXT0);
#elif CONFIG_IDF_TARGET_ESP32C3
  touchWake=(cause==ESP_SLEEP_WAKEUP_GPIO);
#endif
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  if (!touchWake) {
    // Even an old retained sleep request may coincide with an unfinished
    // recorder write across a brownout/reset. Recover at normal boot before
    // permitting deep sleep; never quiesce an uninitialized SD host blindly.
    const uint8_t retainedStage=odysseyLastRecordFailureStage();
    if (retainedStage>=44u && retainedStage!=48u)
      return resumeC3BootAfterFailedSleep("retained SD write fault needs validated mount");
  }
#endif
  if (!touchWake) {
    synapLastSleepStage=SLEEP_STAGE_RESET_RECOVERY;
    synapDeepSleepMarker=SYNAP_DEEP_SLEEP_MARKER;
    sleepPending=true;
    Serial.println("[POWER] sleep lock survived a non-touch reset; returning to deep sleep before BLE");
    delay(30);armTouchWakeAndSleep();
    return resumeC3BootAfterFailedSleep("non-touch reset");
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
    delay(30);armTouchWakeAndSleep();
    return resumeC3BootAfterFailedSleep("short wake press");
  }

  // Do not continue into normal touch handling until the wake press is released.
  uint32_t releasedAt=millis();
  while (uint32_t(millis()-releasedAt)<TOUCH_DEBOUNCE_MS) {
    if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) releasedAt=millis();
    delay(5);
  }
  if (!writeDurableSleepLock(false)) {
    Serial.println("[POWER] could not clear durable sleep lock; refusing BLE boot");
    delay(30);armTouchWakeAndSleep();
    return resumeC3BootAfterFailedSleep("NVS wake lock");
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
  if (odysseyRecording.load() || OdysseyWifi::busy()) return;
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
  if (odysseyRecording.load() || OdysseyWifi::busy()) return;
  if (OdysseyTransfer::busy()) {
    Serial.println("[POWER] deep sleep deferred: C3 SD transfer active");
    return;
  }
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

#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  if (odysseySdUnsafeToSleep.load()) {
    // Do not autonomously repeat destructive reinitialization on a proven
    // held-LOW SD bus. Explicit op14 or a disconnected double-tap can retry;
    // a verified mount is still required before entering deep sleep.
    if (odysseySdBusStuckLow()) return;
    // A previous quiesce may have left the always-powered SD host unmounted.
    // One bounded explicit recovery is safer than either forcing sleep or
    // permanently leaving the C3 awake. Throttle recurring idle-timeout
    // retries to protect the battery and preserve responsiveness.
    static uint32_t lastSdSleepRecoveryAt=0;
    const uint32_t now=millis();
    if (lastSdSleepRecoveryAt && uint32_t(now-lastSdSleepRecoveryAt)<30000u) return;
    lastSdSleepRecoveryAt=now ? now : 1u;
    Serial.println("[POWER] retrying validated SD mount before sleep");
    if (!odysseyRecoverSdCard("sleep")) {
      Serial.println("[POWER] sleep deferred: SD recovery has not completed");
      return;
    }
  }
#endif

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

  // Quiesce SD before notifying BLE that deep sleep is happening.
  // A failed CMD18/CMD25 idle check must not send a false power-off event.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  if (!odysseyPrepareSdForPowerTransition(1000u)) {
    writeDurableSleepLock(false);
    synapDeepSleepMarker=0;
    synapLastSleepStage=SLEEP_STAGE_ABORTED;
    sleepPending=false;
    Serial.println("[POWER] deep sleep cancelled: C3 SD storage did not quiesce");
    updateStatusLed(true);
    updateStatusCharacteristic(true);
    return;
  }
#endif

  // Tell the app only after SD has finished and wake is armed. No BLE deinit
  // is performed; deep sleep itself tears down the radio.
  Serial.println("[POWER] sleep pending; SD idle and wake source armed");
  publishPowerEvent(POWER_STATE_DEEP_SLEEP);
  if (deviceConnected.load()) delay(90);

  statusLed.clear();statusLed.show();
  synapLastSleepStage=SLEEP_STAGE_ENTERING;
  Serial.printf("[POWER] entering deep sleep request=%u battery=%umV\n",
    unsigned(synapSleepRequestCounter),unsigned(batteryMillivolts));
  esp_deep_sleep_start();

  // Deep sleep should not return. If it does, retain fail-closed semantics.
  Serial.println("[POWER] deep sleep returned unexpectedly; rebooting with sleep lock retained");
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  if (!odysseyPrepareSdForPowerTransition(500u)) return;
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
  if (OdysseyWifi::busy() || OdysseyTransfer::busy()) return;
  // Do not permanently suppress disconnected-timeout or critical-battery
  // sleep after a failed quiesce. enterDeepSleep() performs a bounded,
  // throttled recovery and still refuses sleep until SD is proven idle.
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
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  static uint32_t lastDeferredSleepAttemptAt=0;
#endif
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
      && !odysseyRecording.load() && !OdysseyWifi::busy() && !OdysseyTransfer::busy()
      // A hold while recording requests STOP but cannot sleep before
      // fclose, recovery and the post-write SD settle have completed.
      && (!odysseySdSleepGuardUntil.load() ||
          static_cast<int32_t>(now-odysseySdSleepGuardUntil.load())>=0)
      && (!lastDeferredSleepAttemptAt ||
          uint32_t(now-lastDeferredSleepAttemptAt)>=5000u)
#endif
  ) {
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
    // An aborted sleep (SD busy, failed wake arm, NVS lock) must not lose
    // the pending long-hold request. Deep sleep never returns on success.
    lastDeferredSleepAttemptAt=now ? now : 1u;
#else
    deepSleepAfterStop=false;
#endif
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
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
      // A fresh physical gesture cancels a pending delayed sleep request.
      if (deepSleepAfterStop) {
        deepSleepAfterStop=false;
        lastDeferredSleepAttemptAt=0;
        Serial.println("[POWER] pending C3 sleep cancelled by new touch");
      }
#endif
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
      if (OdysseyWifi::busy()) {
        deepSleepAfterStop=true;
        Serial.println("[POWER] deep sleep deferred until C3 Wi-Fi upload completes");
        return;
      }
      if (OdysseyTransfer::busy()) {
        deepSleepAfterStop=true;
        Serial.println("[POWER] deep sleep deferred until C3 SD BLE sync completes");
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
  // MTU alone does not guarantee throughput: the C3's single-core NimBLE
  // link in Bluefy ran out of controller buffers (status=ERROR_GATT, code=6)
  // after repeatedly queueing four 400-byte PCM notifications every 50 ms.
  // C3 therefore uses the existing per-frame IMA-ADPCM v3 transport, which
  // the PWA already decodes to PCM before journaling. One 404-byte payload
  // at MTU 517 uses only one notification per 50 ms instead of four.
  // This changes BLE bandwidth only: offline SD WAV capture stays PCM16.
  // S3/Chakshu keep their existing lossless PCM-first negotiation.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  pcmTransport=false;
#else
  // Packet-size eligibility is not a throughput guarantee; drop/reject counters
  // remain visible. Keep the selected format stable until START or RESUME.
  pcmTransport=peerMtu>=PCM_MIN_MTU;
#endif
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
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
    // With Bluefy/iOS, the phone is the connection central. Avoid firing a
    // peripheral connection-parameter update in the middle of native service
    // discovery and CCCD writes. C3 live audio now needs only one ADPCM
    // notification per frame, so the central's negotiated interval suffices.
    (void)server;
    (void)desc;
#else
    if(desc)server->updateConnParams(desc->conn_handle, BLE_MIN_INTERVAL,
      BLE_MAX_INTERVAL, BLE_SLAVE_LATENCY, BLE_SUPERVISION_TIMEOUT);
#endif
  }
  // Arduino BLE 3.3.5's NimBLE callback omits event->disconnect.reason;
  // 0xFFFF still explicitly means "not exposed by the library".
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
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  uint32_t congestionGeneration=0;
  uint8_t consecutiveRejectedFrames=0;
#endif
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
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
      if (congestionGeneration!=frame.generation) {
        congestionGeneration=frame.generation;
        consecutiveRejectedFrames=0;
      }
#endif
      const uint32_t rejectedBefore=notifyRejected.load();
      const bool sent=sendAudioFrame(frame);
      if (!sent &&
          deviceConnected.load() && streamingEnabled.load() && frame.generation == streamGeneration.load()) {
        // Report dropped frames without silently treating a failed notification
        // as delivered. The S3 legacy behavior remains unchanged.
        if(notifyRejected.load()!=rejectedBefore) {
          ++captureDrops;
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
          // If even the 8 KB/s fallback cannot obtain NimBLE mbufs, fail
          // promptly rather than letting an unresponsive BLE link run through
          // an entire second take with zero journaled audio.
          if (++consecutiveRejectedFrames>=4u) {
            Serial.println("[BLE] C3 sustained notification ENOMEM; stopping broken stream");
            requestStreamError(ErrorCode::TRANSPORT_CHANGED, frame.generation);
          }
#endif
        } else requestStreamError(ErrorCode::TRANSPORT_CHANGED, frame.generation);
      }
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
      else if (sent) consecutiveRejectedFrames=0;
#endif
    }
    transmitterActive.store(false);
  }
}

// Odyssey SD storage.
// C3 mounts through the stock Arduino-ESP32 SD SPI path and uses FAT/VFS at runtime; S3 keeps its legacy one-shot detection.
// Hardware pins remain device-profile controlled and are never remapped here.
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
uint8_t odysseyLastRecordFailureStage();
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
// Arduino-ESP32 3.3.5 always initializes the card at 400 kHz internally.
// This is the post-init runtime data clock retained by the SD driver.
static constexpr uint32_t ODYSSEY_SD_DATA_FREQ_HZ=1000000u;
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

// Restore the exact Arduino-ESP32 3.3.5 stock SD initialization used by the
// known-good Odyssey C3 build 1445. Runtime recording/sync continues to use
// the current guarded POSIX/VFS implementation after the mount succeeds.
static SPIClass odysseySdSpi(FSPI);
static std::atomic<int32_t> odysseySdLastMountError{ESP_OK};
static std::atomic<uint32_t> odysseySdMountAttempts{0};
static std::atomic<uint32_t> odysseySdBeginAttempts{0};
static std::atomic<uint8_t> odysseySdLastMountReason{0}; // 1=boot,2=op14,3=touch,4=probe
// VFS stages: 0=not run; 1=root; 2=directory setup; 3=directory type;
// 4=opendir; 5=closedir; 6=probe open; 7=probe write/flush;
// 8=probe close; 9=probe unlink; 10=ready; 11=runtime catalogue/read.
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
uint16_t odysseySdRawFFCount() { return odysseySdRawFF.load(); }
uint16_t odysseySdRawFECount() { return odysseySdRawFE.load(); }
uint16_t odysseySdRawOtherCount() { return odysseySdRawOther.load(); }
uint16_t odysseySdRawMaxFFRunCount() { return odysseySdRawMaxFFRun.load(); }
int16_t odysseySdBitBangCmd0Response() { return odysseySdBitBangCmd0.load(); }
int16_t odysseySdBitBangCmd8Response() { return odysseySdBitBangCmd8.load(); }
uint32_t odysseySdBitBangR7Response() { return odysseySdBitBangR7.load(); }
void odysseySdUseProbingClock() {}
void odysseySdMarkVfsFailure() {
  // Observation only. Catalogue/read errors must never schedule a destructive
  // remount behind the user's back. Explicit PWA Check SD (op14) or a physical
  // disconnected double-tap may request recovery.
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
const char* odysseySdMountPoint() { return ODYSSEY_SD_MOUNT_POINT; }
bool odysseySdPath(const char* logical,char* full,size_t capacity) {
  if (!logical || logical[0]!='/' || !full || capacity<2) return false;
  if (strstr(logical,"..")) return false;
  const int n=snprintf(full,capacity,"%s%s",ODYSSEY_SD_MOUNT_POINT,logical);
  return n>0 && size_t(n)<capacity;
}
static void odysseySdReleaseLocked() {
  SD.end();
  odysseySdSpi.end();
  // Exact 1445 teardown: only deassert CS. Do not preconfigure SCK/MOSI/MISO
  // before the next SPIClass::begin(), because this diagnostic build is
  // intentionally testing the old known-good ownership sequence.
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_CS,OUTPUT);
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
  odysseySdReleaseLocked();
  digitalWrite(ODYSSEY_SD_CS,HIGH);pinMode(ODYSSEY_SD_CS,OUTPUT);
  pinMode(ODYSSEY_SD_SCK,OUTPUT);digitalWrite(ODYSSEY_SD_SCK,LOW);
  pinMode(ODYSSEY_SD_MOSI,OUTPUT);digitalWrite(ODYSSEY_SD_MOSI,HIGH);
  pinMode(ODYSSEY_SD_MISO,INPUT_PULLUP);
  delayMicroseconds(50);

  const uint32_t started=millis();
  const int csHigh=digitalRead(ODYSSEY_SD_MISO)==HIGH?1:0;
  for (uint8_t i=0;i<16;++i) (void)odysseySdBitBangTransfer(0xFF);

  // The mounted FatFs session is closed and the storage mutex is held.
  // On a healthy SD card, do NOT inject CMD12 into an already idle SPI bus:
  // first require sustained 0xFF while selected AND a successful R2 status
  // (CMD13). A pulled-up floating MISO alone is not sufficient proof.
  uint16_t idleRun=0;
  digitalWrite(ODYSSEY_SD_CS,LOW);
  for (uint16_t i=0;i<256u && idleRun<64u;++i) {
    const uint8_t response=odysseySdBitBangTransfer(0xFF);
    if (response==0xFF) ++idleRun;
    else idleRun=0;
  }
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  (void)odysseySdBitBangTransfer(0xFF);
  if (idleRun>=64u) {
    uint8_t status=0xFF;
    const uint8_t r1=odysseySdBitBangCommand(13u,0u,0x01u,&status,1u);
    if (r1==0x00u && status==0x00u) {
      digitalWrite(ODYSSEY_SD_SCK,LOW);
      digitalWrite(ODYSSEY_SD_MOSI,HIGH);
      Serial.printf("[SD] quiesce idle=1 via CMD13 status=%u in %lums\n",
        unsigned(status),static_cast<unsigned long>(millis()-started));
      return 1;
    }
    Serial.printf("[SD] CMD13 idle probe inconclusive r1=0x%02X status=0x%02X; using recovery quiesce\n",
      unsigned(r1),unsigned(status));
  }

  uint8_t candidate=0xFF;
  uint32_t drained=0;
  uint8_t state=odysseySdBitBangStopReadLocked(candidate,drained,budgetMs);

  // A card left inside CMD25 ignores CMD12 and waits for a data token instead.
  // Only reachable if budget remains, and with timeouts scaled to it.
  uint8_t writeStop=0;
  const uint32_t spent=uint32_t(millis()-started);
  if (state!=1 && spent<budgetMs) {
    const uint32_t remaining=budgetMs-spent;
    writeStop=odysseySdBitBangStopWriteLocked(remaining/2u+1u,remaining/2u+1u);
    if (writeStop==1) state=1;
  }

  // Park the bus the way a deselected card expects to find it.
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  for (uint8_t i=0;i<10;++i) (void)odysseySdBitBangTransfer(0xFF);
  digitalWrite(ODYSSEY_SD_SCK,LOW);
  digitalWrite(ODYSSEY_SD_MOSI,HIGH);

  Serial.printf("[SD] quiesce idle=%u csHigh=%d drain=%lu writeStop=%u in %lums\n",
    unsigned(state),csHigh,static_cast<unsigned long>(drained),unsigned(writeStop),
    static_cast<unsigned long>(millis()-started));
  return state;
}

static bool odysseySdBeginLocked() {
  ++odysseySdBeginAttempts;
  odysseySdSpi.begin(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS);
  const bool mounted=SD.begin(ODYSSEY_SD_CS,odysseySdSpi,ODYSSEY_SD_DATA_FREQ_HZ,
    ODYSSEY_SD_MOUNT_POINT,ODYSSEY_SD_MAX_OPEN_FILES,false);
  if (mounted) markOdysseySdBatteryDividerPresent();
  else odysseySdReleaseLocked();
  return mounted;
}

static bool odysseySdValidateVfsLocked(const char* reason,uint8_t attempt) {
  odysseySdVfsStep=0;odysseySdVfsErrno=0;
  struct stat root{};
  if (stat(ODYSSEY_SD_MOUNT_POINT,&root)!=0 || !S_ISDIR(root.st_mode)) {
    odysseySdVfsStep=1;odysseySdVfsErrno=errno?errno:ENOTDIR;
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u VFS root unavailable errno=%d\n",reason,unsigned(attempt),errno);
    return false;
  }
  struct stat recordings{};
  if (stat(ODYSSEY_SD_RECORDING_DIR,&recordings)!=0) {
    if (errno!=ENOENT || mkdir(ODYSSEY_SD_RECORDING_DIR,0755)!=0) {
      odysseySdVfsStep=2;odysseySdVfsErrno=errno?errno:EIO;
      odysseySdLastMountError=ESP_FAIL;
      odysseySdBootState=2;odysseySdProbeStage=4;
      Serial.printf("[SD] %s attempt %u recording directory unavailable errno=%d\n",
        reason,unsigned(attempt),errno);
      return false;
    }
  } else if (!S_ISDIR(recordings.st_mode)) {
    odysseySdVfsStep=3;odysseySdVfsErrno=ENOTDIR;
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u /synap is not a directory\n",reason,unsigned(attempt));
    return false;
  }

  DIR* verified=opendir(ODYSSEY_SD_RECORDING_DIR);
  if (!verified) {
    const int saved=errno;
    odysseySdVfsStep=4;odysseySdVfsErrno=saved;
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u recordings opendir failed errno=%d\n",
      reason,unsigned(attempt),saved);
    return false;
  }
  if (closedir(verified)!=0) {
    const int saved=errno;
    odysseySdVfsStep=5;odysseySdVfsErrno=saved;
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u recordings closedir failed errno=%d\n",
      reason,unsigned(attempt),saved);
    return false;
  }

  const char* probePath="/odyssey-sd/synap/.synap-media-probe.tmp";
  FILE* probe=fopen(probePath,"wb");
  if (!probe) {
    const int saved=errno;
    odysseySdVfsStep=6;odysseySdVfsErrno=saved;
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u recordings not writable errno=%d\n",
      reason,unsigned(attempt),saved);
    return false;
  }
  errno=0;
  const bool writeOk=fputc('S',probe)!=EOF && fflush(probe)==0;
  const int writeErrno=writeOk?0:errno;
  errno=0;
  const bool closeOk=fclose(probe)==0;
  const int closeErrno=closeOk?0:errno;
  errno=0;
  const bool removeOk=unlink(probePath)==0;
  const int removeErrno=removeOk?0:errno;
  if (!writeOk || !closeOk || !removeOk) {
    const uint8_t failedStep=!writeOk?7:!closeOk?8:9;
    const int failedErrno=!writeOk?writeErrno:!closeOk?closeErrno:removeErrno;
    odysseySdVfsStep=failedStep;odysseySdVfsErrno=failedErrno?failedErrno:EIO;
    odysseySdLastMountError=ESP_FAIL;
    odysseySdBootState=2;odysseySdProbeStage=4;
    Serial.printf("[SD] %s attempt %u write/readiness probe failed errno=%d\n",
      reason,unsigned(attempt),int(odysseySdVfsErrno.load()));
    return false;
  }
  odysseySdVfsStep=10;odysseySdVfsErrno=0;
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
  odysseySdReleaseLocked();

  // digitalWrite BEFORE pinMode, which is the order build 1445 used. The
  // reverse drives the pin from the output register's reset value - low - for
  // the microseconds until the digitalWrite lands, so every mount attempt
  // opened with a CS glitch the known-good lifecycle never produced.
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  Serial.printf("[SD] %s attempt %u Arduino SPI init at %lu Hz, pins CS=%d SCK=%d MOSI=%d MISO=%d\n",
    reason,unsigned(attempt),static_cast<unsigned long>(ODYSSEY_SD_DATA_FREQ_HZ),
    ODYSSEY_SD_CS,ODYSSEY_SD_SCK,ODYSSEY_SD_MOSI,ODYSSEY_SD_MISO);

  bool mounted=odysseySdBeginLocked();
  if (!mounted) {
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

  const uint8_t type=SD.cardType();
  if (type==CARD_NONE) {
    odysseySdLastMountError=ESP_ERR_NOT_FOUND;
    odysseySdBootState=3;odysseySdProbeStage=0;
    Serial.printf("[SD] %s attempt %u mounted bus but card type is NONE\n",reason,unsigned(attempt));
    odysseySdReleaseLocked();
    return false;
  }

  if (!odysseySdValidateVfsLocked(reason,attempt)) {
    odysseySdReleaseLocked();
    return false;
  }

  odysseySdLastMountError=ESP_OK;
  odysseySdBootState=1;
  odysseySdProbeStage=6;
  // A complete mount plus writable VFS validation is the explicit recovery
  // that makes a previously failed sleep quiesce safe to attempt again.
  odysseySdUnsafeToSleep=false;
  const char* label=type==CARD_MMC?"MMC":type==CARD_SD?"SDSC":type==CARD_SDHC?"SDHC/SDXC":"unknown";
  Serial.printf("[SD] ready via proven Arduino SPI path: %s, %llu MiB, %lu Hz\n",
    label,static_cast<unsigned long long>(SD.cardSize()/(1024ULL*1024ULL)),
    static_cast<unsigned long>(ODYSSEY_SD_DATA_FREQ_HZ));
  return true;
}

static bool odysseySdMountLocked(const char* reason,uint8_t attempts) {
  if (odysseySdReady()) return true;
  for (uint8_t attempt=1;attempt<=attempts;++attempt) {
    if (odysseySdMountOnceLocked(reason,attempt)) return true;
    // After a brownout, an all-zero response even with CS HIGH indicates a
    // held-low bus. Repeating CMD12/CMD0 before advertising only delays BLE
    // and can leave the battery exposed to repeated power spikes. Preserve
    // fault diagnostics and offer explicit recovery after connectivity.
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
bool odysseyInitializeSdCardBeforeBle() {
  OdysseySdGuard guard;
  if (!guard) { odysseySdBootState=2;odysseySdProbeStage=1; return false; }

  // A persisted recorder write failure means the continuously-powered card may
  // have survived the MCU reset inside a data/program state. Re-arm the SD
  // protocol before the first Arduino SD.begin() instead of asking the same
  // stale card state to answer a fresh host immediately.
  const uint8_t previousRecordStage=odysseyLastRecordFailureStage();
  const bool previousStorageFault=previousRecordStage>=44u && previousRecordStage!=48u;
  if (previousStorageFault) {
    // Retained write/close faults survive reboot, unlike this in-RAM veto.
    // Only successful VFS-validated remount may clear unsafe-to-sleep.
    odysseySdUnsafeToSleep=true;
    Serial.printf("[SD] boot re-arm after recorder stage=%u\n",unsigned(previousRecordStage));
    (void)odysseySdBitBangRecoverLocked("rearm");
    delay(20);
  }

  const bool ready=odysseySdMountLocked("boot",ODYSSEY_SD_BOOT_ATTEMPTS);
  Serial.printf("[SD] boot initialization complete state=%u stage=%u before BLE\n",
    unsigned(odysseySdBootState.load()),unsigned(odysseySdProbeStage.load()));
  return ready;
}
bool odysseyRecoverSdCard(const char* reason) {
  OdysseySdGuard guard(pdMS_TO_TICKS(5000));
  if (!guard) return false;
  odysseySdBootState=0;odysseySdProbeStage=0;
  odysseySdReleaseLocked();
  return odysseySdMountLocked(reason?reason:"op14",ODYSSEY_SD_RECOVERY_ATTEMPTS);
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
// C3 local audio owns the mounted VFS and microphone until finalization.
// BLE connection changes never redirect a take; no local PCM enters the app queue.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
static std::atomic<uint8_t> odysseyPersistedRecordStage{0};
static std::atomic<uint32_t> odysseyPersistedRecordBytes{0};
static std::atomic<bool> odysseyPersistedRecordLoaded{false};
// V2 of the existing diagnostic journal retains the precise short write.
static std::atomic<uint32_t> odysseyPersistedWriteErrno{0};
static std::atomic<uint32_t> odysseyPersistedWriteReturned{0};
static std::atomic<uint32_t> odysseyPersistedWriteExpected{0};
static std::atomic<uint32_t> odysseyPersistedWriteFerror{0};

static void odysseyLoadPersistedRecordFailure() {
  if (odysseyPersistedRecordLoaded.exchange(true)) return;
  Preferences prefs;
  if (!prefs.begin("sd-recdiag",true)) return;
  uint32_t record[7]{};
  const size_t savedBytes=prefs.getBytesLength("last");
  const bool legacy=savedBytes==3u*sizeof(uint32_t);
  const bool version2=savedBytes==sizeof(record);
  if ((legacy || version2) &&
      prefs.getBytes("last",record,savedBytes)==savedBytes &&
      (legacy?record[0]==1u:record[0]==2u)) {
    odysseyPersistedRecordStage=uint8_t(record[1]&255u);
    odysseyPersistedRecordBytes=record[2];
    if (version2) {
      // Read the temporary seven-word journal produced during development.
      odysseyPersistedWriteErrno=record[3];
      odysseyPersistedWriteReturned=record[4];
      odysseyPersistedWriteExpected=record[5];
      odysseyPersistedWriteFerror=record[6];
    } else {
      // Keep "last" in its original three-word form so an OTA rollback
      // still sees an unresolved stage-70 failure and retains sleep safety.
      // Extra fields live in a separate stage/byte-paired journal.
      uint32_t detail[6]{};
      if (prefs.getBytesLength("write")==sizeof(detail) &&
          prefs.getBytes("write",detail,sizeof(detail))==sizeof(detail) &&
          detail[0]==record[1] && detail[1]==record[2]) {
        odysseyPersistedWriteErrno=detail[2];
        odysseyPersistedWriteReturned=detail[3];
        odysseyPersistedWriteExpected=detail[4];
        odysseyPersistedWriteFerror=detail[5];
      }
    }
  }
  prefs.end();
}
uint8_t odysseyLastRecordFailureStage() {
  odysseyLoadPersistedRecordFailure();
  return odysseyPersistedRecordStage.load();
}
uint32_t odysseyLastRecordFailureBytes() {
  odysseyLoadPersistedRecordFailure();
  return odysseyPersistedRecordBytes.load();
}
uint32_t odysseyLastWriteErrno() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWriteErrno.load(); }
uint32_t odysseyLastWriteReturned() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWriteReturned.load(); }
uint32_t odysseyLastWriteExpected() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWriteExpected.load(); }
uint32_t odysseyLastWriteFerror() { odysseyLoadPersistedRecordFailure(); return odysseyPersistedWriteFerror.load(); }
static void odysseyPersistRecordFailure(uint8_t stage,uint32_t bytes) {
  odysseyPersistedRecordLoaded=true;
  odysseyPersistedRecordStage=stage;
  odysseyPersistedRecordBytes=bytes;
  if (!stage) {
    odysseyPersistedWriteErrno=0;
    odysseyPersistedWriteReturned=0;
    odysseyPersistedWriteExpected=0;
    odysseyPersistedWriteFerror=0;
  }
  Preferences prefs;
  if (!prefs.begin("sd-recdiag",false)) return;
  // Preserve the original on-flash ABI for older OTA rollback builds.
  // Write optional detail first; last/3-word remains the authoritative stage.
  if (stage) {
    const uint32_t detail[6]={uint32_t(stage),bytes,
      odysseyPersistedWriteErrno.load(),odysseyPersistedWriteReturned.load(),
      odysseyPersistedWriteExpected.load(),odysseyPersistedWriteFerror.load()};
    (void)prefs.putBytes("write",detail,sizeof(detail));
  }
  const uint32_t record[3]={stage?1u:0u,uint32_t(stage),bytes};
  (void)prefs.putBytes("last",record,sizeof(record));
  prefs.end();
}

static void odysseyWavHeader(uint8_t* h,uint32_t bytes) {
  memset(h,0,44);
  memcpy(h,"RIFF",4);put32le(h+4,bytes+36);
  memcpy(h+8,"WAVEfmt ",8);put32le(h+16,16);
  h[20]=1;h[22]=1;put32le(h+24,SAMPLE_RATE);
  put32le(h+28,SAMPLE_RATE*2);h[32]=2;h[34]=16;
  memcpy(h+36,"data",4);put32le(h+40,bytes);
}
static uint8_t odysseyCreateFailureStage(int error) {
  // Preserve the actual FAT/VFS create error in the existing one-byte
  // diagnostic field. These values are emitted only when fopen() fails.
  switch (error) {
    case EIO: return 49;
    case ENODEV: return 50;
    case EMFILE:
    case ENFILE: return 51;
    case ENOSPC: return 52;
    case EROFS: return 53;
    default: return 54;
  }
}
static uint8_t odysseyBatchWriteFailureStage(int error) {
  switch (error) {
    case EIO: return 66;
    case ENODEV: return 67;
    case ENOSPC: return 68;
    case EROFS: return 69;
    default: return 70;
  }
}
static void odysseyRecordTake() {
  odysseyPersistedWriteErrno=0;
  odysseyPersistedWriteReturned=0;
  odysseyPersistedWriteExpected=0;
  odysseyPersistedWriteFerror=0;
  bool failed=false;
  uint8_t failureStage=0;
  uint32_t bytes=0;
  char logicalPath[64]{};
  char fullPath[96]{};
  uint8_t header[44];
  // Keep the batch out of the 8 KiB recorder task stack. A 4 KiB aligned
  // write gives FatFs eight contiguous sectors, routing Arduino SD through
  // CMD25 + ACMD23 instead of the fragile one-sector CMD24 loop.
  alignas(4) static uint8_t batch[4096];
  size_t batchUsed=0;
  uint32_t batchPcmBytes=0;
  FILE* file=nullptr;

  // Hold the single storage mutex for the whole take. A recovery/remount can
  // never tear down the VFS beneath an open recording.
  OdysseySdGuard storage;
  if (!storage || !odysseySdReady()) { failed=true;failureStage=40; }

  if (!failed) {
    // Append-only recording: create a normal zero-length file and let each
    // successful 4 KiB CMD25 batch extend it naturally. No preallocation means
    // STOP never needs to truncate a 32 MiB logical extent.
    snprintf(logicalPath,sizeof(logicalPath),"/synap/odyssey_audio_%08lx_%08lx.wav",
      static_cast<unsigned long>(esp_random()),static_cast<unsigned long>(esp_random()));
    if (!odysseySdPath(logicalPath,fullPath,sizeof(fullPath))) {
      failed=true;failureStage=41;
    } else {
      errno=0;
      file=fopen(fullPath,"wb");
      if (!file) {
        const int openError=errno;
        failed=true;failureStage=odysseyCreateFailureStage(openError);
        Serial.printf("[SD] WAV create failed errno=%d stage=%u path=%s\n",
          openError,unsigned(failureStage),fullPath);
      } else if (setvbuf(file,nullptr,_IONBF,0)!=0) {
        failed=true;failureStage=42;
      }
    }
  }

  odysseyWavHeader(header,0);
  if (!failed) {
    memcpy(batch,header,sizeof(header));
    batchUsed=sizeof(header);
    batchPcmBytes=0;
  }

#if USE_REAL_I2S_MIC
  if (!failed && !odysseyStopRequested.load()) {
    MicrophoneGuard guard;
    if (!startMicrophone()) { failed=true;failureStage=43; }
    int32_t raw[SAMPLES_PER_FRAME];
    int16_t pcm[SAMPLES_PER_FRAME];
    while (!failed && !odysseyStopRequested.load()) {
      size_t received=0;
      uint8_t emptyReads=0;
      while (received<sizeof(raw) && !odysseyStopRequested.load()) {
        const size_t count=microphoneI2S.readBytes(reinterpret_cast<char*>(raw)+received,sizeof(raw)-received);
        if (!count) {
          if (++emptyReads>=3) { failed=true;failureStage=43; break; }
        } else {
          received+=count;emptyReads=0;
        }
      }
      if (failed || odysseyStopRequested.load()) break;
      for (uint16_t i=0;i<SAMPLES_PER_FRAME;++i) pcm[i]=static_cast<int16_t>(raw[i]>>16);
      if (bytes>0xffffff00u-sizeof(pcm)) break; // RIFF length is 32-bit.
      const uint8_t* input=reinterpret_cast<const uint8_t*>(pcm);
      size_t remaining=sizeof(pcm);
      while (!failed && remaining) {
        const size_t room=sizeof(batch)-batchUsed;
        const size_t take=remaining<room?remaining:room;
        memcpy(batch+batchUsed,input,take);
        batchUsed+=take;
        batchPcmBytes+=uint32_t(take);
        input+=take;
        remaining-=take;
        if (batchUsed==sizeof(batch)) {
          errno=0;
          const size_t written=fwrite(batch,1,sizeof(batch),file);
          if (written!=sizeof(batch)) {
            const int writeError=errno;
            odysseyPersistedWriteErrno=writeError>0?uint32_t(writeError):0u;
            odysseyPersistedWriteReturned=uint32_t(written);
            odysseyPersistedWriteExpected=sizeof(batch);
            odysseyPersistedWriteFerror=ferror(file)?1u:0u;
            failed=true;failureStage=odysseyBatchWriteFailureStage(writeError);
            Serial.printf("[SD] PCM batch write failed errno=%d stage=%u wrote=%u pcm=%lu\n",
              writeError,unsigned(failureStage),unsigned(written),
              static_cast<unsigned long>(bytes));
            break;
          }
          bytes+=batchPcmBytes;
          batchUsed=0;
          batchPcmBytes=0;
        }
      }

      // Capture performs only sequential 4 KiB writes. Each full batch is
      // sector aligned and FatFs issues a multi-sector disk_write (CMD25).
      // The first batch carries a provisional WAV header; no random rewrite
      // is attempted while the card is in this recording session.
    }
    stopMicrophone();
  }
#else
  failed=true;failureStage=43;
#endif

  if (file) {
    if (!failed && batchUsed) {
      // Preserve the multi-block path through STOP: pad the tail to one full
      // 4 KiB batch. This adds at most 128 ms of trailing silence and avoids a
      // final single-sector CMD24 write.
      memset(batch+batchUsed,0,sizeof(batch)-batchUsed);
      errno=0;
      const size_t written=fwrite(batch,1,sizeof(batch),file);
      if (written!=sizeof(batch)) {
        const int writeError=errno;
        odysseyPersistedWriteErrno=writeError>0?uint32_t(writeError):0u;
        odysseyPersistedWriteReturned=uint32_t(written);
        odysseyPersistedWriteExpected=sizeof(batch);
        odysseyPersistedWriteFerror=ferror(file)?1u:0u;
        failed=true;failureStage=odysseyBatchWriteFailureStage(writeError);
        Serial.printf("[SD] final PCM batch failed errno=%d stage=%u wrote=%u expected=%u\n",
          writeError,unsigned(failureStage),unsigned(written),unsigned(sizeof(batch)));
      } else {
        bytes+=batchPcmBytes;
        batchUsed=0;
        batchPcmBytes=0;
      }
    }

    // STOP is deliberately append-only. fclose() is the only filesystem
    // finalization operation. The on-card header remains provisional; the
    // transfer path synthesizes the correct 44-byte WAV header from file size.
    if (fclose(file)!=0) {
      failed=true;
      if (!failureStage) failureStage=47;
    }
    file=nullptr;
  }

  if (!failed && bytes==0) failureStage=48;
  Serial.printf("[SD] local audio %s: %s, %lu committed PCM bytes stage=%u%s\n",
    failed?"failed":"saved",logicalPath,static_cast<unsigned long>(bytes),
    unsigned(failureStage),failed?" (mount retained for recovery)":"");
  // Diagnostics are published only after the historical 1631 I/O sequence has
  // completed, so they cannot change the write timing being measured.
  if (failed || bytes==0) {
    const uint8_t persistedStage=failureStage?failureStage:40;
    odysseyPersistRecordFailure(persistedStage,bytes);
    odysseySdBootState=2;
    odysseySdProbeStage=persistedStage;
    odysseyRecordFaultAt=millis();
  } else {
    odysseyPersistRecordFailure(0,0);
    odysseySdBootState=1;
    odysseySdProbeStage=6;
  }
}
// FreeRTOS self-deletion skips C++ stack unwinding; return from a separate
// function first so SD and microphone guards release their mutexes.
static void odysseyRecordTask(void*) {
  // One physical double-tap owns recovery + recording. If the previous take
  // left storage unavailable, recover it here after leaving the touch/control
  // task rather than forcing the user to perform a separate recovery gesture.
  if (!odysseySdReady()) {
    Serial.println("[SD] one-gesture offline start: recovering storage before capture");
    if (!odysseyRecoverSdCard("touch")) {
      odysseyRecordFaultAt=millis();
      // Keep the C3 awake through recovery teardown and a short SD settle.
      // The record-active flag is the sleep veto until cleanup has finished.
      const uint32_t finalizedAt=millis();
      odysseySdSleepGuardUntil=finalizedAt+5000u;
      disconnectedAt=finalizedAt;
      odysseyRecording=false;
      odysseyStopRequested=false;
      applyCpuPowerProfile(false);
      updateStatusLed(true);
      Serial.println("[SD] touch recovery failed; offline recording not started");
      vTaskDelete(nullptr);
      return;
    }
  }

  odysseyRecordTake();

  // A disk error marks the live mount unavailable. The take's guard has
  // unwound at this point, so perform one bounded re-arm before returning to
  // idle. Preserve the recorder failure stage/bytes for diagnostics.
  if (!odysseySdReady() && odysseyLastRecordFailureStage()) {
    Serial.println("[SD] post-record failure: re-arming storage for next take");
    (void)odysseyRecoverSdCard("rearm");
  }
  // An unresolved write/close failure could leave an always-powered SD card
  // inside CMD25 programming. Never allow a later idle timeout or user hold
  // to sleep based solely on the failed mount's "not ready" state.
  const uint8_t lastFault=odysseyLastRecordFailureStage();
  if (!odysseySdReady() && lastFault>=44u && lastFault!=48u) {
    odysseySdUnsafeToSleep=true;
    Serial.printf("[POWER] C3 SD sleep inhibited: unrecovered record stage=%u\n",unsigned(lastFault));
  }

  // Do not lift the sleep veto until fwrite, fclose, persistent diagnostics
  // and any bounded storage re-arm have returned and SD locks were released.
  const uint32_t finalizedAt=millis();
  odysseySdSleepGuardUntil=finalizedAt+5000u;
  disconnectedAt=finalizedAt;
  odysseyRecording=false;
  odysseyStopRequested=false;
  applyCpuPowerProfile(false);
  updateStatusLed(true);
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
  if (odysseyRecording.load()) {
    odysseyStopRequested=true;
    updateStatusLed(true);
    Serial.println("[TOUCH] double tap -> SD audio STOP");
    return;
  }
  if (deviceConnected.load() || streamingEnabled.load() || OdysseyWifi::busy() ||
      otaBusy() || sleepPending || batteryCritical()) return;
  if (!odysseySdReady()) {
    Serial.println("[TOUCH] SD unavailable; recording task will recover before capture");
  }
  odysseyStopRequested=false;
  odysseyRecordingStartedAt=millis();
  odysseyRecordFaultAt=0;
  odysseyRecording=true;
  applyCpuPowerProfile(true);
  updateStatusLed(true);
  if (xTaskCreate(odysseyRecordTask,"sd-audio",8192,nullptr,2,nullptr)!=pdPASS) {
    odysseyRecording=false;
    odysseyStopRequested=false;
    odysseyRecordFaultAt=millis();
    applyCpuPowerProfile(false);
    updateStatusLed(true);
    Serial.println("[SD] local audio task allocation failed");
    return;
  }
  Serial.println("[TOUCH] double tap -> SD audio START");
}
#endif
// Odyssey C3 Wi-Fi SD upload.
// Control remains on BLE. The ESP32-C3 joins a user-provisioned 2.4 GHz Wi-Fi
// network (home Wi-Fi or phone hotspot) only for an explicit upload session,
// then streams the append-only SD WAV directly to Synap Cloud over HTTPS.
// The SD source is never deleted here; retention stays a separate PWA choice.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
// The direct HTTPS Wi-Fi implementation currently exceeds the immutable
// 1,310,720-byte C3 OTA slot. Keep its implementation for optimization, but
// ship a diagnostic-safe build with the feature explicitly disabled; otherwise
// the OTA pipeline cannot publish fixes for the existing SD recorder.
// Never enlarge the OTA partition without a separate migration design.
#ifndef SYNAP_C3_WIFI_UPLOAD_ENABLED
#define SYNAP_C3_WIFI_UPLOAD_ENABLED 0
#endif
#if SYNAP_C3_WIFI_UPLOAD_ENABLED
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <limits.h>
#include <time.h>

namespace OdysseyWifi {

// Google Trust Services GTS Root R1 (current digitalSignature root).
// SHA-256: D9:47:43:2A:BD:E7:B7:FA:90:FC:2E:6B:59:10:1B:12:
//           80:E0:E1:C7:E4:E4:0F:A3:C6:88:7F:FF:57:A7:F4:CF
// Valid until 2036-06-22. Source: https://pki.goog/repository/
// Arduino-ESP32 3.3.5 has setCACert(), but no useBuiltinCACertBundle().
static const char GOOGLE_TRUST_ROOT_R1[] PROGMEM = R"PEM(
-----BEGIN CERTIFICATE-----
MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw
MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp
Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA
A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo
27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w
Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw
TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl
qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH
szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8
Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk
MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92
wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p
aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN
VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID
AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E
FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb
C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe
QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy
h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4
7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J
ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef
MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/
Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT
6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ
0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm
2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb
bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c
-----END CERTIFICATE-----
)PEM";

static constexpr uint32_t WIFI_CONNECT_MS=20000u;
static constexpr uint32_t TIME_SYNC_MS=12000u;
static constexpr time_t TLS_MIN_UNIX_TIME=1704067200; // 2024-01-01 UTC
static constexpr uint32_t HTTP_TIMEOUT_MS=30000u;
static constexpr uint32_t SEGMENT_MS=120000u;
static constexpr uint32_t PCM_BYTES_PER_MS=32u; // 16 kHz * mono * 16 bit / 1000.
static constexpr uint32_t SEGMENT_PCM_BYTES=SEGMENT_MS*PCM_BYTES_PER_MS;
static constexpr size_t CONFIG_CAPACITY=2304;
static constexpr size_t WIFI_TASK_STACK=12288;

enum Phase : uint8_t {
  IDLE=0,
  CONNECTING=1,
  PREPARING=2,
  UPLOADING=3,
  FINALIZING=4,
  COMPLETE=5,
  FAILED=6,
};

struct Status {
  bool active=false;
  bool configured=false;
  uint8_t phase=IDLE;
  uint32_t total=0;
  uint32_t uploaded=0;
  int16_t http=0;
  char ssid[33]{};
  char message[96]{};
};

struct UploadSpec {
  char endpoint[192]{};
  char token[896]{};
  char recordingId[40]{};
  char path[64]{};
};

static portMUX_TYPE statusMux=portMUX_INITIALIZER_UNLOCKED;
static Status statusState;
static UploadSpec uploadSpec;
static std::atomic<bool> uploadBusy{false};
static char configBuffer[CONFIG_CAPACITY]{};
static size_t configLength=0;

static void setStatus(uint8_t phase,bool active,uint32_t total,uint32_t uploaded,
                      int http,const char* message) {
  portENTER_CRITICAL(&statusMux);
  statusState.phase=phase;
  statusState.active=active;
  statusState.total=total;
  statusState.uploaded=uploaded;
  statusState.http=int16_t(http);
  if (message) snprintf(statusState.message,sizeof(statusState.message),"%s",message);
  portEXIT_CRITICAL(&statusMux);
}

static bool readCredentials(char* ssid,size_t ssidCap,char* password,size_t passwordCap) {
  if (!ssid || ssidCap<2 || !password || passwordCap<1) return false;
  ssid[0]=0;password[0]=0;
  Preferences prefs;
  if (!prefs.begin("c3-wifi",true)) return false;
  const String storedSsid=prefs.getString("ssid","");
  const String storedPassword=prefs.getString("pass","");
  prefs.end();
  if (!storedSsid.length() || storedSsid.length()>32 || storedPassword.length()>63) return false;
  snprintf(ssid,ssidCap,"%s",storedSsid.c_str());
  snprintf(password,passwordCap,"%s",storedPassword.c_str());
  return true;
}

static void refreshConfigured() {
  char ssid[33],password[64];
  const bool ok=readCredentials(ssid,sizeof(ssid),password,sizeof(password));
  portENTER_CRITICAL(&statusMux);
  statusState.configured=ok;
  snprintf(statusState.ssid,sizeof(statusState.ssid),"%s",ok?ssid:"");
  portEXIT_CRITICAL(&statusMux);
}

bool available() { return true; }
bool busy() { return uploadBusy.load(); }

static bool safeWavPath(const char* path) {
  if (!path || strncmp(path,"/synap/",7)!=0) return false;
  const size_t n=strlen(path);
  if (n<12 || n>63 || (strcasecmp(path+n-4,".wav")!=0)) return false;
  for (size_t i=7;i<n-4;++i) {
    const char c=path[i];
    if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')) return false;
  }
  return true;
}

static int hexNibble(char c) {
  if (c>='0'&&c<='9') return c-'0';
  if (c>='a'&&c<='f') return c-'a'+10;
  if (c>='A'&&c<='F') return c-'A'+10;
  return -1;
}

static bool decodeField(const char* key,char* output,size_t capacity) {
  if (!key || !output || capacity<1) return false;
  const size_t keyLength=strlen(key);
  const char* start=configBuffer;
  const char* limit=configBuffer+configLength;
  while (start<limit) {
    const char* end=static_cast<const char*>(memchr(start,'\n',size_t(limit-start)));
    if (!end) end=limit;
    if (size_t(end-start)>=keyLength && !memcmp(start,key,keyLength)) {
      const char* hex=start+keyLength;
      const size_t digits=size_t(end-hex);
      if (digits&1u || digits/2u>=capacity) return false;
      const size_t bytes=digits/2u;
      for (size_t i=0;i<bytes;++i) {
        const int hi=hexNibble(hex[i*2]),lo=hexNibble(hex[i*2+1]);
        if (hi<0 || lo<0) return false;
        output[i]=char((hi<<4)|lo);
      }
      output[bytes]=0;
      return true;
    }
    start=end<limit?end+1:limit;
  }
  return false;
}

uint32_t configChunk(uint32_t offset,const char* text) {
  const size_t n=text?strlen(text):0;
  if (offset==0) { configLength=0;configBuffer[0]=0; }
  if (offset!=configLength || !n || n>60 || configLength+n>=sizeof(configBuffer)) return UINT32_MAX;
  memcpy(configBuffer+configLength,text,n);
  configLength+=n;
  configBuffer[configLength]=0;
  return uint32_t(configLength);
}

static bool saveCredentialsFromConfig() {
  char ssid[33]{},password[64]{};
  if (!decodeField("S=",ssid,sizeof(ssid)) || !ssid[0]) return false;
  // An open network is valid, so a missing P= is treated as empty only when
  // the field itself exists in the configuration.
  if (!decodeField("P=",password,sizeof(password))) return false;
  Preferences prefs;
  if (!prefs.begin("c3-wifi",false)) return false;
  const size_t a=prefs.putString("ssid",ssid);
  const size_t b=prefs.putString("pass",password);
  prefs.end();
  if (!a || (password[0] && !b)) return false;
  refreshConfigured();
  return true;
}

bool forget() {
  if (busy()) return false;
  Preferences prefs;
  if (!prefs.begin("c3-wifi",false)) return false;
  const bool ok=prefs.clear();
  prefs.end();
  WiFi.disconnect(true,false);
  WiFi.mode(WIFI_OFF);
  refreshConfigured();
  setStatus(IDLE,false,0,0,0,ok?"Wi-Fi network removed.":"Could not remove Wi-Fi network.");
  return ok;
}

static void wavHeader(uint8_t* h,uint32_t pcmBytes) {
  memset(h,0,44);
  memcpy(h,"RIFF",4);put32le(h+4,pcmBytes+36u);
  memcpy(h+8,"WAVEfmt ",8);put32le(h+16,16u);
  h[20]=1;h[22]=1;put32le(h+24,SAMPLE_RATE);
  put32le(h+28,SAMPLE_RATE*2u);h[32]=2;h[34]=16;
  memcpy(h+36,"data",4);put32le(h+40,pcmBytes);
}

static uint8_t httpBuffer[2048];

static bool endpoint(char* host,size_t hostCapacity,uint16_t& port) {
  const char* value=uploadSpec.endpoint;
  if (!value || strncmp(value,"https://",8)!=0) return false;
  value+=8;
  if (!*value || strchr(value,'/')) return false;
  const char* colon=strrchr(value,':');
  size_t hostLength=colon?size_t(colon-value):strlen(value);
  if (!hostLength || hostLength>=hostCapacity) return false;
  memcpy(host,value,hostLength);host[hostLength]=0;
  port=443;
  if (colon) {
    unsigned parsed=0;
    const char* p=colon+1;
    if (!*p) return false;
    while (*p) {
      if (*p<'0' || *p>'9') return false;
      parsed=parsed*10u+unsigned(*p-'0');
      if (parsed>65535u) return false;
      ++p;
    }
    if (!parsed) return false;
    port=uint16_t(parsed);
  }
  return true;
}

static bool writeAll(WiFiClientSecure& tls,const uint8_t* data,size_t size) {
  const uint32_t started=millis();
  size_t sent=0;
  while (sent<size && tls.connected() && uint32_t(millis()-started)<HTTP_TIMEOUT_MS) {
    const size_t written=tls.write(data+sent,size-sent);
    if (written) sent+=written;
    else delay(1);
  }
  return sent==size;
}
static bool writeAll(WiFiClientSecure& tls,const char* text) {
  return text && writeAll(tls,reinterpret_cast<const uint8_t*>(text),strlen(text));
}

static int readHttpStatus(WiFiClientSecure& tls) {
  char line[64]{};
  size_t used=0;
  const uint32_t started=millis();
  while (uint32_t(millis()-started)<HTTP_TIMEOUT_MS && used+1<sizeof(line)) {
    if (!tls.available()) {
      if (!tls.connected()) break;
      delay(1);continue;
    }
    const int c=tls.read();
    if (c<0) continue;
    if (c=='\n') break;
    if (c!='\r') line[used++]=char(c);
  }
  line[used]=0;
  if (strncmp(line,"HTTP/",5)!=0) return -1;
  const char* space=strchr(line,' ');
  if (!space || space[1]<'0'||space[1]>'9'||space[2]<'0'||space[2]>'9'||space[3]<'0'||space[3]>'9')
    return -1;
  return (space[1]-'0')*100+(space[2]-'0')*10+(space[3]-'0');
}

static bool openHttps(WiFiClientSecure& tls,char* host,size_t hostCapacity) {
  uint16_t port=443;
  if (!endpoint(host,hostCapacity,port)) return false;
  tls.setCACert(GOOGLE_TRUST_ROOT_R1);
  tls.setHandshakeTimeout(15);
  return tls.connect(host,port,15000)==1;
}

static bool uploadSegment(FILE* file,uint32_t index,uint32_t pcmOffset,uint32_t pcmBytes,int& httpCode) {
  if (!file || fseek(file,long(44u+pcmOffset),SEEK_SET)!=0) { httpCode=-1;return false; }
  char host[128]{};
  WiFiClientSecure tls;
  if (!openHttps(tls,host,sizeof(host))) { httpCode=-1;return false; }

  char header[1536];
  const int headerSize=snprintf(header,sizeof(header),
    "PUT /v1/device-uploads/%s/segments/%lu HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Authorization: SynapDevice %s\r\n"
    "Content-Type: audio/wav\r\n"
    "Content-Length: %lu\r\n"
    "x-synap-start-ms: %lu\r\n"
    "x-synap-end-ms: %lu\r\n"
    "Cache-Control: no-store\r\n"
    "Connection: close\r\n\r\n",
    uploadSpec.recordingId,static_cast<unsigned long>(index),host,uploadSpec.token,
    static_cast<unsigned long>(pcmBytes+44u),
    static_cast<unsigned long>(pcmOffset/PCM_BYTES_PER_MS),
    static_cast<unsigned long>((pcmOffset+pcmBytes)/PCM_BYTES_PER_MS));
  if (headerSize<=0 || size_t(headerSize)>=sizeof(header) || !writeAll(tls,header)) {
    tls.stop();httpCode=-1;return false;
  }

  uint8_t wav[44];
  wavHeader(wav,pcmBytes);
  if (!writeAll(tls,wav,sizeof(wav))) { tls.stop();httpCode=-1;return false; }

  uint32_t remaining=pcmBytes;
  while (remaining) {
    const size_t wanted=remaining<sizeof(httpBuffer)?size_t(remaining):sizeof(httpBuffer);
    const size_t got=fread(httpBuffer,1,wanted,file);
    if (got!=wanted || !writeAll(tls,httpBuffer,got)) {
      tls.stop();httpCode=-1;return false;
    }
    remaining-=uint32_t(got);
  }

  httpCode=readHttpStatus(tls);
  tls.stop();
  return httpCode==200 || httpCode==202;
}

static bool finalizeUpload(uint32_t durationMs,uint32_t segmentCount,int& httpCode) {
  char host[128]{};
  WiFiClientSecure tls;
  if (!openHttps(tls,host,sizeof(host))) { httpCode=-1;return false; }

  char body[112];
  const int bodySize=snprintf(body,sizeof(body),
    "{\"duration_ms\":%lu,\"segment_count\":%lu}",
    static_cast<unsigned long>(durationMs),static_cast<unsigned long>(segmentCount));
  if (bodySize<=0 || size_t(bodySize)>=sizeof(body)) { tls.stop();httpCode=-1;return false; }

  char header[1536];
  const int headerSize=snprintf(header,sizeof(header),
    "POST /v1/device-uploads/%s/finalize HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Authorization: SynapDevice %s\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %u\r\n"
    "Cache-Control: no-store\r\n"
    "Connection: close\r\n\r\n",
    uploadSpec.recordingId,host,uploadSpec.token,unsigned(bodySize));
  if (headerSize<=0 || size_t(headerSize)>=sizeof(header) ||
      !writeAll(tls,header) || !writeAll(tls,reinterpret_cast<const uint8_t*>(body),size_t(bodySize))) {
    tls.stop();httpCode=-1;return false;
  }

  httpCode=readHttpStatus(tls);
  tls.stop();
  return httpCode==200 || httpCode==202;
}

static bool connectWifi() {
  char ssid[33]{},password[64]{};
  if (!readCredentials(ssid,sizeof(ssid),password,sizeof(password))) return false;
  setStatus(CONNECTING,true,0,0,0,"Connecting Odyssey C3 to Wi-Fi…");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid,password);
  const uint32_t started=millis();
  while (WiFi.status()!=WL_CONNECTED && uint32_t(millis()-started)<WIFI_CONNECT_MS) {
    if (otaBusy() || sleepPending) break;
    delay(100);
  }
  if (WiFi.status()!=WL_CONNECTED) {
    WiFi.disconnect(true,false);
    WiFi.mode(WIFI_OFF);
    return false;
  }
  return true;
}

static bool syncClock() {
  if (time(nullptr)>=TLS_MIN_UNIX_TIME) return true;
  setStatus(CONNECTING,true,0,0,0,"Securing Wi-Fi connection…");
  configTime(0,0,"time.google.com","time.cloudflare.com","pool.ntp.org");
  const uint32_t started=millis();
  while (time(nullptr)<TLS_MIN_UNIX_TIME && uint32_t(millis()-started)<TIME_SYNC_MS) {
    if (otaBusy() || sleepPending || batteryCritical() || WiFi.status()!=WL_CONNECTED) return false;
    delay(100);
  }
  return time(nullptr)>=TLS_MIN_UNIX_TIME;
}

static void stopWifi() {
  WiFi.disconnect(true,false);
  WiFi.mode(WIFI_OFF);
}

static void uploadTask(void*) {
  applyCpuPowerProfile(true);
  bool ok=false;
  int httpCode=0;
  uint32_t pcmTotal=0,uploaded=0,segments=0;
  char full[96]{};
  FILE* file=nullptr;

  setStatus(PREPARING,true,0,0,0,"Preparing SD recording for Wi-Fi sync…");

  if (!connectWifi()) {
    setStatus(FAILED,false,0,0,0,"Could not join the saved Wi-Fi/hotspot.");
    uploadBusy=false;
    applyCpuPowerProfile(false);
    vTaskDelete(nullptr);
    return;
  }
  if (!syncClock()) {
    stopWifi();
    setStatus(FAILED,false,0,0,0,"Wi-Fi connected, but secure time sync failed.");
    uploadBusy=false;
    applyCpuPowerProfile(false);
    vTaskDelete(nullptr);
    return;
  }

  {
    // Keep storage mounted and the file stable for the complete cloud upload.
    OdysseySdGuard storage(pdMS_TO_TICKS(5000));
    if (storage && odysseySdReady() && safeWavPath(uploadSpec.path) &&
        odysseySdPath(uploadSpec.path,full,sizeof(full))) {
      struct stat st{};
      if (stat(full,&st)==0 && S_ISREG(st.st_mode) && st.st_size>44 &&
          uint64_t(st.st_size)<=uint64_t(LONG_MAX)) {
        pcmTotal=uint32_t(st.st_size)-44u;
        file=fopen(full,"rb");
      }
    }

    if (!file || !pcmTotal) {
      if (file) fclose(file);
      setStatus(FAILED,false,0,0,0,"SD recording is unavailable.");
    } else {
      setStatus(UPLOADING,true,pcmTotal,0,0,"Uploading SD recording over Wi-Fi…");
      const uint32_t segmentCount=(pcmTotal+SEGMENT_PCM_BYTES-1u)/SEGMENT_PCM_BYTES;
      for (uint32_t index=0;index<segmentCount;++index) {
        if (otaBusy() || sleepPending || batteryCritical() || WiFi.status()!=WL_CONNECTED) break;
        const uint32_t offset=index*SEGMENT_PCM_BYTES;
        const uint32_t remaining=pcmTotal-offset;
        const uint32_t bytes=remaining<SEGMENT_PCM_BYTES?remaining:SEGMENT_PCM_BYTES;
        if (!uploadSegment(file,index,offset,bytes,httpCode)) break;
        uploaded+=bytes;
        segments=index+1u;
        setStatus(UPLOADING,true,pcmTotal,uploaded,httpCode,"Uploading SD recording over Wi-Fi…");
      }
      if (uploaded==pcmTotal && segments==segmentCount) {
        setStatus(FINALIZING,true,pcmTotal,uploaded,httpCode,"Verifying Wi-Fi upload…");
        const uint32_t durationMs=pcmTotal/PCM_BYTES_PER_MS;
        ok=finalizeUpload(durationMs,segmentCount,httpCode);
      }
      fclose(file);file=nullptr;
      if (ok) {
        setStatus(COMPLETE,false,pcmTotal,uploaded,httpCode,"Wi-Fi sync complete.");
      } else {
        setStatus(FAILED,false,pcmTotal,uploaded,httpCode,
          WiFi.status()==WL_CONNECTED?"Wi-Fi upload failed; SD original kept.":"Wi-Fi disconnected; SD original kept.");
      }
    }
  }

  stopWifi();
  uploadBusy=false;
  applyCpuPowerProfile(false);
  vTaskDelete(nullptr);
}

static bool prepareUploadFromConfig() {
  UploadSpec next{};
  if (!decodeField("E=",next.endpoint,sizeof(next.endpoint)) ||
      !decodeField("T=",next.token,sizeof(next.token)) ||
      !decodeField("R=",next.recordingId,sizeof(next.recordingId)) ||
      !decodeField("F=",next.path,sizeof(next.path))) return false;
  if (strncmp(next.endpoint,"https://",8)!=0 || strchr(next.endpoint+8,'/') ||
      strlen(next.token)<16 || strlen(next.recordingId)!=36 || !safeWavPath(next.path)) return false;
  if (busy() || odysseyRecording.load() || streamingEnabled.load() || otaBusy() || sleepPending ||
      !odysseySdReady()) return false;
  uploadSpec=next;
  uploadBusy=true;
  setStatus(PREPARING,true,0,0,0,"Starting Wi-Fi sync…");
  if (xTaskCreate(uploadTask,"c3-wifi",WIFI_TASK_STACK,nullptr,1,nullptr)!=pdPASS) {
    uploadBusy=false;
    setStatus(FAILED,false,0,0,0,"Wi-Fi upload task unavailable.");
    return false;
  }
  return true;
}

bool applyConfig(uint32_t mode) {
  bool ok=false;
  if (mode==1) ok=saveCredentialsFromConfig();
  else if (mode==2) ok=prepareUploadFromConfig();
  configLength=0;configBuffer[0]=0;
  if (!ok && mode==1) setStatus(FAILED,false,0,0,0,"Wi-Fi network could not be saved.");
  return ok;
}

size_t encode(char* output,size_t capacity) {
  if (!output || capacity<32) return 0;
  refreshConfigured();
  Status snapshot;
  portENTER_CRITICAL(&statusMux);
  snapshot=statusState;
  portEXIT_CRITICAL(&statusMux);
  const int n=snprintf(output,capacity,
    "{\"configured\":%s,\"active\":%s,\"phase\":%u,\"total\":%lu,\"uploaded\":%lu,"
    "\"http\":%d,\"message\":\"%s\"}",
    snapshot.configured?"true":"false",snapshot.active?"true":"false",unsigned(snapshot.phase),
    static_cast<unsigned long>(snapshot.total),static_cast<unsigned long>(snapshot.uploaded),
    int(snapshot.http),snapshot.message);
  if (n<0) return 0;
  return size_t(n)<capacity?size_t(n):capacity-1;
}

void initialize() {
  refreshConfigured();
  setStatus(IDLE,false,0,0,0,"Wi-Fi idle.");
}

} // namespace OdysseyWifi
#else
// No Wi-Fi upload is advertised until the C3 TLS image fits the deployed OTA
// partition. Media-v1 BLE catalogue/read/delete and offline SD recording stay
// available exactly as in the proven recorder baseline.
namespace OdysseyWifi {
bool available() { return false; }
bool busy() { return false; }
uint32_t configChunk(uint32_t,const char*) { return UINT32_MAX; }
bool applyConfig(uint32_t) { return false; }
bool forget() { return false; }
size_t encode(char* output,size_t capacity) {
  static constexpr char unavailable[] =
      R"JSON({"configured":false,"active":false,"phase":0,"total":0,"uploaded":0,"http":0,"message":"Wi-Fi sync requires a size-optimized firmware update."})JSON";
  if (!output || capacity<=sizeof(unavailable)-1u) return 0;
  memcpy(output,unavailable,sizeof(unavailable));
  return sizeof(unavailable)-1u;
}
void initialize() {}
} // namespace OdysseyWifi
#endif // SYNAP_C3_WIFI_UPLOAD_ENABLED
#endif // Odyssey C3
// Odyssey C3 SD media-v1: catalogue/read/delete for locally recorded WAV files.
// C3 storage is mounted through the stock Arduino SD SPI path and accessed through FAT/VFS.
// Files are deleted only after the PWA has imported and verified them.
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
namespace OdysseyTransfer {
struct Request {
  uint32_t connection=0,id=0,offset=0;
  uint8_t operation=0;
  char path[64]{};
};
static QueueHandle_t requests=nullptr;
// Keep SD reads protected while BLE is transferring and briefly afterward.
static std::atomic<bool> sdBleTransferInFlight{false};
static std::atomic<uint32_t> sdBleLastTransferAt{0};
bool busy() {
  if (sdBleTransferInFlight.load()) return true;
  const uint32_t last=sdBleLastTransferAt.load();
  return last && uint32_t(millis()-last)<15000u;
}
// Six paced notifications per request avoid thousands of GATT read round trips.
static BLECharacteristic* streamCharacteristic=nullptr;
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

static void virtualWavHeader(uint8_t* h,uint32_t totalBytes) {
  memset(h,0,44);
  const uint32_t pcmBytes=totalBytes>44u?totalBytes-44u:0u;
  memcpy(h,"RIFF",4);put32le(h+4,pcmBytes+36u);
  memcpy(h+8,"WAVEfmt ",8);put32le(h+16,16u);
  h[20]=1;h[22]=1;put32le(h+24,SAMPLE_RATE);
  put32le(h+28,SAMPLE_RATE*2u);h[32]=2;h[34]=16;
  memcpy(h+36,"data",4);put32le(h+40,pcmBytes);
}
static void patchVirtualWavHeader(uint8_t* bytes,size_t size,uint32_t offset,uint32_t totalBytes) {
  if (!bytes || !size || totalBytes<44u || offset>=44u) return;
  uint8_t header[44];
  virtualWavHeader(header,totalBytes);
  const size_t count=std::min(size_t(44u-offset),size);
  memcpy(bytes,header+offset,count);
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
  if (stat(full,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0) return FILE_UNAVAILABLE;
  if (uint64_t(st.st_size)>UINT32_MAX) return FILE_UNAVAILABLE;
  total=uint32_t(st.st_size);
  snprintf(selectedPath,sizeof(selectedPath),"%s",path);
  return OK;
}

static uint8_t readSelected(const char* requestedPath,uint32_t offset,uint32_t& total,uint8_t* bytes,size_t& size) {
  // New clients make every chunk self-describing. This prevents catalogue
  // refreshes or another client from replacing the selected file mid-transfer.
  if (requestedPath && requestedPath[0]) {
    if (!strcmp(requestedPath,"@catalogue")) {
      total=catalogueBuffer.length();
      if (!total || offset>=total) return FILE_UNAVAILABLE;
      size=std::min(size_t(480),size_t(total-offset));
      memcpy(bytes,catalogueBuffer.c_str()+offset,size);
      return OK;
    }
    if (!safeWavPath(requestedPath)) return BAD_COMMAND;
    if (offset==0) Serial.printf("[SD] transfer begin path=%s\n",requestedPath);
    OdysseySdGuard guard;
    if (!guard || !storageReady()) return NO_SD;
    char full[96];
    if (!fullPath(requestedPath,full,sizeof(full))) return BAD_COMMAND;
    struct stat st{};
    if (stat(full,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0 || uint64_t(st.st_size)>UINT32_MAX)
      return FILE_UNAVAILABLE;
    total=uint32_t(st.st_size);
    if (offset>=total) return FILE_UNAVAILABLE;
    size=std::min(size_t(480),size_t(total-offset));
    FILE* file=fopen(full,"rb");
    if (!file) return FILE_UNAVAILABLE;
    const bool ok=fseek(file,long(offset),SEEK_SET)==0 && fread(bytes,1,size,file)==size;
    fclose(file);
    if (ok) patchVirtualWavHeader(bytes,size,offset,total);
    return ok?OK:IO_ERROR;
  }

  // Legacy clients still use the selected-file/catalogue state.
  if (!selectedPath[0]) {
    total=catalogueBuffer.length();
    if (!total || offset>=total) return FILE_UNAVAILABLE;
    size=std::min(size_t(480),size_t(total-offset));
    memcpy(bytes,catalogueBuffer.c_str()+offset,size);
    return OK;
  }

  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  char full[96];
  if (!fullPath(selectedPath,full,sizeof(full))) return BAD_COMMAND;
  struct stat st{};
  if (stat(full,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0 || uint64_t(st.st_size)>UINT32_MAX)
    return FILE_UNAVAILABLE;
  total=uint32_t(st.st_size);
  if (offset>=total) return FILE_UNAVAILABLE;
  size=std::min(size_t(480),size_t(total-offset));

  FILE* file=fopen(full,"rb");
  if (!file) return FILE_UNAVAILABLE;
  const bool ok=fseek(file,long(offset),SEEK_SET)==0 && fread(bytes,1,size,file)==size;
  fclose(file);
  if (ok) patchVirtualWavHeader(bytes,size,offset,total);
  return ok?OK:IO_ERROR;
}

static uint8_t catalogue(uint32_t& total) {
  selectedPath[0]=0;
  catalogueBuffer="";
  catalogueErrno=0;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;

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

  if (!catalogueBuffer.reserve(2048)) {
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
    if (stat(full,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<0) continue;
    if (count++) catalogueBuffer+=",";
    catalogueBuffer+="{\"path\":\""+String(logical)+"\",\"bytes\":"+String(uint32_t(st.st_size))+"}";
    if (count>=100) break;
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

static uint8_t removeFile(const char* path) {
  selectedPath[0]=0;
  if (!safeWavPath(path)) return BAD_COMMAND;
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return NO_SD;
  char full[96];
  if (!fullPath(path,full,sizeof(full))) return BAD_COMMAND;
  struct stat st{};
  if (stat(full,&st)!=0 || !S_ISREG(st.st_mode)) return FILE_UNAVAILABLE;
  return unlink(full)==0?OK:IO_ERROR;
}

static uint16_t clearRecordings() {
  OdysseySdGuard guard;
  if (!guard || !storageReady()) return 0;
  char directoryPath[96];
  if (!odysseySdPath("/synap",directoryPath,sizeof(directoryPath))) return 0;
  DIR* directory=opendir(directoryPath);
  if (!directory) return 0;

  String logicalPaths[100];
  uint16_t count=0;
  while (dirent* entry=readdir(directory)) {
    if (count>=100) break;
    if (!entry->d_name || !strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
    char logical[64];
    const int n=snprintf(logical,sizeof(logical),"/synap/%s",entry->d_name);
    if (n<=0 || size_t(n)>=sizeof(logical) || !safeWavPath(logical)) continue;
    char full[96];
    if (!odysseySdPath(logical,full,sizeof(full))) continue;
    struct stat st{};
    if (stat(full,&st)==0 && S_ISREG(st.st_mode)) logicalPaths[count++]=logical;
  }
  closedir(directory);

  uint16_t removed=0;
  for (uint16_t i=0;i<count;++i) {
    char full[96];
    if (odysseySdPath(logicalPaths[i].c_str(),full,sizeof(full)) && unlink(full)==0) ++removed;
  }
  return removed;
}

// Fast media-v1 window, using the same per-chunk, explicit-path FAT read as
// fallback op4. A bounded window never owns the SD mutex across BLE waits.
static void notifySdWindow(const Request& request,uint8_t kind,uint8_t error,
    uint32_t total,uint32_t offset,const uint8_t* payload=nullptr,size_t length=0) {
  if (!streamCharacteristic || request.connection!=connectionGeneration.load() ||
      !deviceConnected.load()) return;
  uint8_t packet[496]{};
  packet[0]=0xCC;packet[1]=1;packet[2]=kind;packet[3]=error;
  put32le(packet+4,request.id);put32le(packet+8,total);put32le(packet+12,offset);
  length=std::min(length,size_t(480));
  if (payload && length) memcpy(packet+16,payload,length);
  streamCharacteristic->setValue(packet,16+length);
  streamCharacteristic->notify();
}

static void streamSdWindow(const Request& request) {
  // Older/small-MTU Bluefy connections fall back to op4 without overlong BLE
  // notifications. A 16-byte empty-window completion tells the PWA to fall back.
  const uint16_t capacity=attValueCapacity.load();
  if (capacity<496u || !streamCharacteristic || !request.path[0]) {
    notifySdWindow(request,2,0,0,request.offset);
    return;
  }
  const size_t chunkCapacity=std::min(size_t(480),size_t(capacity-16u));
  uint8_t chunk[480]{};
  uint32_t offset=request.offset,fullSize=0;
  uint8_t error=OK;
  // PWA retains at most 8 chunks; 6-chunk windows leave headroom on iOS.
  for (uint8_t n=0;n<6;++n) {
    if (request.connection!=connectionGeneration.load() || !deviceConnected.load() ||
        streamingEnabled.load() || odysseyRecording.load() || OdysseyWifi::busy() ||
        otaBusy() || sleepPending) return;
    uint32_t total=0;
    size_t length=0;
    error=readSelected(request.path,offset,total,chunk,length);
    if (error!=OK || !length || length>chunkCapacity) {
      if (error==IO_ERROR) odysseySdMarkVfsFailure();
      if (error==OK) error=BAD_COMMAND;
      notifySdWindow(request,2,error,fullSize,offset);
      return;
    }
    if (fullSize && total!=fullSize) {
      notifySdWindow(request,2,FILE_UNAVAILABLE,fullSize,offset);
      return;
    }
    fullSize=total;
    // An ATT notification is not an ACK. The receiver checks offsets, can
    // resume missing windows, and verifies the final file before import.
    notifySdWindow(request,1,OK,fullSize,offset,chunk,length);
    offset+=uint32_t(length);
    if (offset>=fullSize) break;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  // End packet closes the credit window. Never delete/modify the SD source.
  if (request.connection==connectionGeneration.load() && deviceConnected.load()) {
    vTaskDelay(pdMS_TO_TICKS(20));
    notifySdWindow(request,2,OK,fullSize,offset);
  }
}

static void worker(void*) {
  Request request;
  uint8_t bytes[480];
  for (;;) {
    if (xQueueReceive(requests,&request,pdMS_TO_TICKS(500))!=pdTRUE) {
      // Never remount merely because the worker is idle or catalogue failed.
      // A physical disconnected double-tap is an explicit recovery request,
      // just like PWA operation 14, and may safely run while storage is idle.
      const bool idleEnough=!odysseyRecording.load() && !streamingEnabled.load() &&
        !OdysseyWifi::busy() && !otaBusy() && !sleepPending;
      if (odysseySdConsumeRecoveryRequest()) {
        if (idleEnough) {
          Serial.println("[SD] physical touch requested software recovery");
          (void)odysseyRecoverSdCard("touch");
        } else {
          odysseySdRequestRecovery();
        }
      }
      continue;
    }
    if (request.connection!=connectionGeneration.load() || !deviceConnected.load()) continue;
    sdBleTransferInFlight=true;
    sdBleLastTransferAt=millis();
    // Connected remote standby only idles the microphone/CPU; SD media must
    // remain readable for verified sync and recovery without a forced wake.
    // Wi-Fi status must remain readable over BLE while the uploader owns SD.
    if (request.operation==25) {
      char detail[480];
      const size_t n=OdysseyWifi::encode(detail,sizeof(detail));
      reply(request,OK,uint32_t(n),0,reinterpret_cast<const uint8_t*>(detail),n);
      sdBleLastTransferAt=millis();
      sdBleTransferInFlight=false;
      continue;
    }
    if (odysseyRecording.load() || streamingEnabled.load() || OdysseyWifi::busy() ||
        otaBusy() || sleepPending) {
      reply(request,BUSY);
      sdBleLastTransferAt=millis();
      sdBleTransferInFlight=false;
      continue;
    }
    uint8_t error=OK;uint32_t total=0;size_t size=0;
    switch (request.operation) {
      case 3: error=selectFile(request.path,total); break;
      case 12:
        streamSdWindow(request);
        sdBleLastTransferAt=millis();
        sdBleTransferInFlight=false;
        continue;
      case 16: error=OK; break;
      case 4:
        error=readSelected(request.path,request.offset,total,bytes,size);
        if (error==IO_ERROR) odysseySdMarkVfsFailure();
        break;
      case 7:
        error=catalogue(total);
        // Never auto-unmount/remount a mounted card because a catalogue read
        // failed. Preserve the observed state for diagnosis; explicit op 14 is
        // the only connected remount path.
        if (error==IO_ERROR) odysseySdMarkVfsFailure();
        break;
      case 8: total=catalogueBuffer.length();if(!total)error=FILE_UNAVAILABLE;break;
      case 14:
        selectedPath[0]=0;catalogueBuffer="";
        error=odysseyRecoverSdCard("op14")?OK:NO_SD;
        break;
      case 17: error=removeFile(request.path); break;
      case 18:
        selectedPath[0]=0;catalogueBuffer="";
        if(!storageReady())error=NO_SD;else total=clearRecordings();
        break;
      // C3 direct Wi-Fi sync control. Configuration is staged in <=60-byte BLE
      // chunks so credentials and short-lived cloud tickets never need a new
      // characteristic or protocol version.
      case 23: {
        const uint32_t next=OdysseyWifi::configChunk(request.offset,request.path);
        if (next==UINT32_MAX) error=BAD_COMMAND; else total=next;
        break;
      }
      case 24:
        if (request.offset!=1u && request.offset!=2u) error=BAD_COMMAND;
        else error=OdysseyWifi::applyConfig(request.offset)?OK:BUSY;
        break;
      case 26:
        error=OdysseyWifi::forget()?OK:BUSY;
        break;
      default:error=BAD_COMMAND;break;
    }
    if (request.operation==7 && (error==IO_ERROR || error==NO_SD)) {
      char detail[480];
      const int n=snprintf(detail,sizeof(detail),
        "{\"stage\":\"catalogue\",\"errno\":%d,\"sdState\":%u,\"sdProbe\":%u,\"espErr\":%ld,\"mountAttempts\":%lu,\"beginAttempts\":%lu,\"mountWhy\":%u,\"vfsStep\":%u,\"vfsErrno\":%ld,\"bbHigh\":%d,\"bbLow\":%d,\"raw0\":%u,\"rawFF\":%u,\"rawFE\":%u,\"rawOther\":%u,\"rawMaxFF\":%u,\"bbCmd12Candidate\":%d,\"bbReadIdle\":%u,\"bbDrain\":%lu,\"bbStop\":%u,\"bbCmd0\":%d,\"bbCmd8\":%d,\"bbR7\":%lu,\"wrE\":%lu,\"wrN\":%lu,\"wrX\":%lu,\"wrF\":%lu}",
        catalogueErrno,unsigned(odysseySdDetectionState()),unsigned(odysseySdProbeState()),
        static_cast<long>(odysseySdLastError()),static_cast<unsigned long>(odysseySdAttemptCount()),
        static_cast<unsigned long>(odysseySdBeginAttemptCount()),unsigned(odysseySdLastMountReasonCode()),
        unsigned(odysseySdVfsStepValue()),static_cast<long>(odysseySdVfsErrnoValue()),
        int(odysseySdBitBangCsHighState()),int(odysseySdBitBangCsLowState()),
        unsigned(odysseySdRawZeroCount()),unsigned(odysseySdRawFFCount()),
        unsigned(odysseySdRawFECount()),unsigned(odysseySdRawOtherCount()),
        unsigned(odysseySdRawMaxFFRunCount()),int(odysseySdBitBangCmd12Response()),
        unsigned(odysseySdBitBangCmd12ReadyState()),static_cast<unsigned long>(odysseySdBitBangDrainByteCount()),
        unsigned(odysseySdBitBangStopStateValue()),int(odysseySdBitBangCmd0Response()),
        int(odysseySdBitBangCmd8Response()),static_cast<unsigned long>(odysseySdBitBangR7Response()),
        static_cast<unsigned long>(odysseyLastWriteErrno()),
        static_cast<unsigned long>(odysseyLastWriteReturned()),
        static_cast<unsigned long>(odysseyLastWriteExpected()),
        static_cast<unsigned long>(odysseyLastWriteFerror()));
      reply(request,error,total,request.offset,reinterpret_cast<const uint8_t*>(detail),
        n>0?std::min(size_t(n),sizeof(detail)-1):0);
    } else reply(request,error,total,request.offset,bytes,size);
    sdBleLastTransferAt=millis();
    sdBleTransferInFlight=false;
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
  // Phase 2 keeps the proven request/response media-v1 transport and adds
  // BLE-controlled C3 Wi-Fi cloud upload. Notification-window transfer and
  // destructive full-card format remain unadvertised.
  auto* command=service->createCharacteristic("4fa12354-0000-1000-8000-00805f9b34fb",
    BLECharacteristic::PROPERTY_WRITE|BLECharacteristic::PROPERTY_WRITE_NR);
  command->setCallbacks(new CommandCallbacks());
  auto* data=service->createCharacteristic("4fa12355-0000-1000-8000-00805f9b34fb",
    BLECharacteristic::PROPERTY_READ);
  data->setCallbacks(new DataCallbacks());
  streamCharacteristic=service->createCharacteristic("4fa1235a-0000-1000-8000-00805f9b34fb",
    BLECharacteristic::PROPERTY_NOTIFY);
#if defined(CONFIG_BLUEDROID_ENABLED)
  streamCharacteristic->addDescriptor(new BLE2902());
#endif
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
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  // A physical power-switch ON is a new user power-on request. NVS survives
  // battery removal but must not strand a cold-booted C3 in a former OFF state.
  // Deep-sleep GPIO wakes are ESP_RST_DEEPSLEEP and retain the 4-second hold.
  if (bootResetReason==ESP_RST_POWERON) {
    const bool staleLock=readDurableSleepLock();
    synapDeepSleepMarker=0;
    if (staleLock && !writeDurableSleepLock(false))
      Serial.println("[POWER] cold boot: could not clear old NVS sleep lock");
    if (staleLock)
      Serial.println("[POWER] cold power-on overrides retained deep-sleep lock");
  }
#endif
  bootSleepWasLocked=readDurableSleepLock() || (synapDeepSleepMarker==SYNAP_DEEP_SLEEP_MARKER);
#if CONFIG_IDF_TARGET_ESP32C3 && !SYNAP_CHAKSHU
  // Even on a transient NVS write failure, the physical power-on must be
  // allowed to complete. Next cold boot will retry clearing the old lock.
  if (bootResetReason==ESP_RST_POWERON) bootSleepWasLocked=false;
#endif
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
  // C3 + SD uses a 1 MOhm / 470 kOhm battery divider (up to ~1.34 V
  // at a full 4.2 V cell). C3 6 dB ADC attenuation only covers ~1.3 V
  // and is unsuitable here; retain S3's historically validated 6 dB setting.
#if CONFIG_IDF_TARGET_ESP32C3
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
#else
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_6db);
#endif
  statusLed.begin();
  statusLed.clear();
  statusLed.show();
  if (!confirmTouchWakeGesture()) return;
  disconnectedAt=millis();
  setDeviceState(DeviceState::DISCONNECTED, ErrorCode::NONE);
  // A C3 may be fitted with the 1 MOhm/470 kOhm SD divider rather than the
  // original 1 MOhm/1 MOhm divider. Do not publish a misleading x2 reading
  // before the SD hardware identification has completed.
#if !CONFIG_IDF_TARGET_ESP32C3
  sampleBattery(true);
#endif
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
  OdysseyWifi::initialize();
  OdysseyTransfer::initialize();
  odysseyInitializeSdCardBeforeBle();
  // The initial C3 reading now uses the appropriate divider when SD mounts.
  // Standard C3 without SD retains its 2:1 measurement.
  sampleBattery(true);
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
