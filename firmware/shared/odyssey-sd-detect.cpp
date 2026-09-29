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

// Mount status: 0=not checked, 1=detected/mounted, 2=mount failed, 3=no usable card reported.
// Electrical probe: 0=not checked, 1=card replied to SPI CMD0, 2=no SPI reply.
static std::atomic<uint8_t> odysseySdBootState{0};
static std::atomic<uint8_t> odysseySdElectricalState{0};
#if CONFIG_IDF_TARGET_ESP32C3
static SPIClass odysseySdSpi(FSPI);
#endif
uint8_t odysseySdDetectionState() { return odysseySdBootState; }
uint8_t odysseySdProbeState() { return odysseySdElectricalState; }

#if CONFIG_IDF_TARGET_ESP32C3
static uint8_t odysseyRawSdProbe(uint32_t hz) {
  SPIClass& spi=odysseySdSpi;
  spi.beginTransaction(SPISettings(hz,MSBFIRST,SPI_MODE0));
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  for (uint8_t i=0;i<12;++i) spi.transfer(0xff); // >=96 idle clocks before CMD0.
  digitalWrite(ODYSSEY_SD_CS,LOW);
  static const uint8_t cmd0[6]={0x40,0,0,0,0,0x95};
  for (uint8_t b:cmd0) spi.transfer(b);
  uint8_t r1=0xff;
  for (uint8_t i=0;i<16;++i) {
    const uint8_t value=spi.transfer(0xff);
    if ((value&0x80u)==0) { r1=value; break; }
  }
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  spi.transfer(0xff);
  spi.endTransaction();
  const uint8_t state=r1==0xff?2:1;
  Serial.printf("[SD] raw SPI probe %s, CMD0 R1=0x%02x\n",state==1?"responded":"no-response",r1);
  return state;
}
#endif

void odysseyDetectSdCard() {
#if CONFIG_IDF_TARGET_ESP32C3
  // Preserve a healthy mounted card. Repeated teardown/remount cycles can turn
  // a working card into a false-offline state on a sealed device.
  if (odysseySdBootState.load()==1 && SD.cardType()!=CARD_NONE) return;

  SPIClass& sdSpi=odysseySdSpi;
  static constexpr uint32_t clocks[] = {400000u, 400000u, 250000u, 125000u};
  bool sawCardWithoutType=false;
  bool sawElectricalReply=false;
  odysseySdBootState=0;
  odysseySdElectricalState=0;

  for (uint8_t attempt=0; attempt<sizeof(clocks)/sizeof(clocks[0]); ++attempt) {
    SD.end();
    sdSpi.end();
    pinMode(ODYSSEY_SD_CS, OUTPUT);
    digitalWrite(ODYSSEY_SD_CS, HIGH);
    delay(20u + uint32_t(attempt)*20u);
    sdSpi.begin(ODYSSEY_SD_SCK, ODYSSEY_SD_MISO, ODYSSEY_SD_MOSI, ODYSSEY_SD_CS);
    delay(5);

    Serial.printf("[SD] probe attempt=%u hz=%lu CS=%d SCK=%d MOSI=%d MISO=%d\n",
      unsigned(attempt+1), static_cast<unsigned long>(clocks[attempt]),
      ODYSSEY_SD_CS, ODYSSEY_SD_SCK, ODYSSEY_SD_MOSI, ODYSSEY_SD_MISO);

    const uint8_t electrical=odysseyRawSdProbe(clocks[attempt]);
    if (electrical==1) sawElectricalReply=true;
    odysseySdElectricalState=sawElectricalReply?1:2;

    const bool mounted=SD.begin(ODYSSEY_SD_CS, sdSpi, clocks[attempt], "/odyssey-sd", 1, false);
    if (mounted) {
      const uint8_t type=SD.cardType();
      if (type!=CARD_NONE) {
        odysseySdBootState=1;
        odysseySdElectricalState=1;
        const char* label=type==CARD_MMC?"MMC":type==CARD_SD?"SDSC":type==CARD_SDHC?"SDHC/SDXC":"unknown";
        Serial.printf("[SD] detected: %s, %llu MiB; filesystem mounted at %lu Hz on attempt %u\n",
          label, static_cast<unsigned long long>(SD.cardSize()/(1024ULL*1024ULL)),
          static_cast<unsigned long>(clocks[attempt]), unsigned(attempt+1));
        return;
      }
      sawCardWithoutType=true;
    }

    SD.end();
    sdSpi.end();
    digitalWrite(ODYSSEY_SD_CS, HIGH);
  }

  odysseySdBootState=sawCardWithoutType?3:2;
  Serial.printf("[SD] recovery exhausted: mount=%u spi=%u\n",
    unsigned(odysseySdBootState.load()),unsigned(odysseySdElectricalState.load()));
#else
  odysseySdBootState=0;
  odysseySdElectricalState=0;
  SPIClass sdSpi(FSPI);

  // S3 Odyssey remains a one-shot detection probe and releases the bus.
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
  SD.end();
  sdSpi.end();
  digitalWrite(ODYSSEY_SD_CS, HIGH);
#endif
}
#endif
