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

// Mount status (C3 also refreshes it on local start): 0=not checked, 1=detected, 2=mount failed, 3=no card reported.
static std::atomic<uint8_t> odysseySdBootState{0};
#if CONFIG_IDF_TARGET_ESP32C3
static SPIClass odysseySdSpi(FSPI);
#endif
uint8_t odysseySdDetectionState() { return odysseySdBootState; }

void odysseyDetectSdCard() {
  odysseySdBootState=0;
#if CONFIG_IDF_TARGET_ESP32C3
  SD.end();
  odysseySdSpi.end();
  SPIClass& sdSpi=odysseySdSpi;
#else
  SPIClass sdSpi(FSPI);
#endif
  // Explicit mapping avoids the board's default SPI pins (used by the mic).
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
    // A failed mount cannot distinguish absent card from wiring/filesystem trouble.
    Serial.println("[SD] detection/mount failed: check card, wiring and filesystem");
  }
#if CONFIG_IDF_TARGET_ESP32C3
  if (odysseySdBootState==1) return;
#endif
  SD.end();
  sdSpi.end();
  digitalWrite(ODYSSEY_SD_CS, HIGH);
}
#endif
