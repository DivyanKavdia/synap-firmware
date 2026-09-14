// Synap pendant firmware for ESP32-S3FH4R2. Wiring and build settings: README.md.
// Built from firmware/shared; regenerate with node tools/assemble-source.cjs.
#ifndef SYNAP_CHAKSHU
#define SYNAP_CHAKSHU 0
#endif
#define SYNAP_MODULE_ID 1
#include <Arduino.h>
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

#define DEVICE_NAME "synap"
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
constexpr uint16_t BATTERY_LOW_MV = 3600;
constexpr uint16_t BATTERY_CRITICAL_MV = 3400;
constexpr uint8_t BATTERY_EVENT_MAGIC = 0xB7;
constexpr uint8_t BATTERY_EVENT_VERSION = 2;
// Short, dim status pulses limit the onboard WS2812's battery load.
constexpr uint8_t LED_DIM = 4;
constexpr int8_t I2S_BCLK_PIN = 4, I2S_WS_PIN = 5, I2S_DATA_IN_PIN = 6;
#if CONFIG_IDF_TARGET_ESP32S3
constexpr uint32_t IDLE_CPU_MHZ = 80, ACTIVE_CPU_MHZ = 240;
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
uint32_t lastBatterySampleAt = 0;
uint16_t batteryMillivolts = 0, batteryAdcMillivolts = 0, batteryAdcRaw = 0;
uint8_t batteryPercent = 0, batteryValidSamples = 0, batteryCriticalSamples = 0;
std::atomic<bool> batteryAvailable{false};

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

