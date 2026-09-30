// Odyssey SD mount. C3 retains the bus for local recording; S3 remains detection-only.
#if !SYNAP_CHAKSHU
#include <SPI.h>
#include <SD.h>
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

static std::atomic<uint8_t> odysseySdBootState{0};
#if CONFIG_IDF_TARGET_ESP32C3
static SPIClass odysseySdSpi(FSPI);
static constexpr uint32_t ODYSSEY_SD_RETRY_SETTLE_MS=350u;
static constexpr uint8_t ODYSSEY_SD_MOUNT_ATTEMPTS=3;
#endif
uint8_t odysseySdDetectionState() { return odysseySdBootState; }
uint8_t odysseySdProbeState() { return odysseySdBootState.load()==1 ? 6 : 0; }

void odysseyDetectSdCard() {
#if CONFIG_IDF_TARGET_ESP32C3
  if (odysseySdBootState.load()==1 && SD.cardType()!=CARD_NONE) {
    Serial.println("[SD] healthy mount retained");
    return;
  }
#endif
  odysseySdBootState=0;
#if CONFIG_IDF_TARGET_ESP32C3
  SD.end();
  odysseySdSpi.end();
  SPIClass& sdSpi=odysseySdSpi;
#else
  SPIClass sdSpi(FSPI);
#endif
  digitalWrite(ODYSSEY_SD_CS, HIGH);
  pinMode(ODYSSEY_SD_CS, OUTPUT);
  sdSpi.begin(ODYSSEY_SD_SCK, ODYSSEY_SD_MISO, ODYSSEY_SD_MOSI, ODYSSEY_SD_CS);
  Serial.printf("[SD] probe CS=%d SCK=%d MOSI=%d MISO=%d\n",
    ODYSSEY_SD_CS, ODYSSEY_SD_SCK, ODYSSEY_SD_MOSI, ODYSSEY_SD_MISO);
  const bool mounted=SD.begin(ODYSSEY_SD_CS, sdSpi, 400000, "/odyssey-sd", 1, false);
  if (mounted) {
    const uint8_t type=SD.cardType();
    if (type != CARD_NONE) {
      odysseySdBootState=1;
      const char* label=type==CARD_MMC?"MMC":type==CARD_SD?"SDSC":type==CARD_SDHC?"SDHC/SDXC":"unknown";
      Serial.printf("[SD] detected: %s, %llu MiB; filesystem mounted\n", label,
        static_cast<unsigned long long>(SD.cardSize()/(1024ULL*1024ULL)));
    } else {
      odysseySdBootState=3;
      Serial.println("[SD] no card reported");
    }
  } else {
    odysseySdBootState=2;
    Serial.println("[SD] detection/mount failed: check card, wiring and filesystem");
  }
#if CONFIG_IDF_TARGET_ESP32C3
  if (odysseySdBootState==1) return;
#endif
  SD.end();
  sdSpi.end();
  digitalWrite(ODYSSEY_SD_CS, HIGH);
}

#if CONFIG_IDF_TARGET_ESP32C3
static bool odysseyMountWithRetries(const char* reason,uint32_t initialSettleMs) {
  if (odysseySdBootState.load()==1 && SD.cardType()!=CARD_NONE) return true;
  if (initialSettleMs) delay(initialSettleMs);
  for (uint8_t attempt=1;attempt<=ODYSSEY_SD_MOUNT_ATTEMPTS;++attempt) {
    Serial.printf("[SD] %s mount attempt %u/%u\n",reason,unsigned(attempt),unsigned(ODYSSEY_SD_MOUNT_ATTEMPTS));
    odysseyDetectSdCard();
    if (odysseySdBootState.load()==1) return true;
    if (attempt<ODYSSEY_SD_MOUNT_ATTEMPTS) delay(ODYSSEY_SD_RETRY_SETTLE_MS);
  }
  return false;
}
bool odysseyInitializeSdCardBeforeBle() {
  // Preserve the proven 1445 boot mount exactly: one immediate mount attempt.
  odysseyDetectSdCard();
  const bool ready=odysseySdBootState.load()==1;
  Serial.printf("[SD] boot initialization complete state=%u before BLE\n",unsigned(odysseySdBootState.load()));
  return ready;
}
bool odysseyRecoverSdCard() {
  return odysseyMountWithRetries("recovery",ODYSSEY_SD_RETRY_SETTLE_MS);
}
#endif
#endif
