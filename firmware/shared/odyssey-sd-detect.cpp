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
static std::atomic<uint8_t> odysseySdProbeStage{0};
#if CONFIG_IDF_TARGET_ESP32C3
static SPIClass odysseySdSpi(FSPI);
static constexpr uint32_t ODYSSEY_SD_RETRY_SETTLE_MS=350u;
static constexpr uint8_t ODYSSEY_SD_MOUNT_ATTEMPTS=3;

// Read-only SD SPI diagnostic stages exposed through the existing capability byte:
// 1=no CMD0 response, 2=SPI idle entered, 3=interface recognized,
// 4=card initialized, 5=sector 0 readable, 6=normal filesystem mount succeeded.
static uint8_t odysseySdRawCommand(SPIClass& spi,uint8_t cmd,uint32_t arg,uint8_t crc,
                                   uint8_t* tail=nullptr,size_t tailSize=0) {
  digitalWrite(ODYSSEY_SD_CS,LOW);
  spi.transfer(0xFF);
  spi.transfer(uint8_t(0x40u|cmd));
  spi.transfer(uint8_t(arg>>24));spi.transfer(uint8_t(arg>>16));
  spi.transfer(uint8_t(arg>>8));spi.transfer(uint8_t(arg));
  spi.transfer(crc);
  uint8_t response=0xFF;
  for (uint8_t i=0;i<16;++i) {
    response=spi.transfer(0xFF);
    if ((response&0x80u)==0) break;
  }
  for (size_t i=0;i<tailSize;++i) tail[i]=spi.transfer(0xFF);
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  spi.transfer(0xFF);
  return response;
}
static bool odysseySdReadSectorZero(SPIClass& spi,bool blockAddressing) {
  digitalWrite(ODYSSEY_SD_CS,LOW);
  spi.transfer(0xFF);
  const uint32_t address=blockAddressing ? 0u : 0u;
  spi.transfer(0x51); // CMD17 READ_SINGLE_BLOCK
  spi.transfer(uint8_t(address>>24));spi.transfer(uint8_t(address>>16));
  spi.transfer(uint8_t(address>>8));spi.transfer(uint8_t(address));
  spi.transfer(0x01);
  uint8_t response=0xFF;
  for (uint8_t i=0;i<16;++i) {
    response=spi.transfer(0xFF);
    if ((response&0x80u)==0) break;
  }
  bool readable=false;
  if (response==0x00) {
    uint8_t token=0xFF;
    for (uint16_t i=0;i<4096 && token==0xFF;++i) token=spi.transfer(0xFF);
    if (token==0xFE) {
      for (uint16_t i=0;i<514;++i) spi.transfer(0xFF); // 512-byte sector + CRC
      readable=true;
    }
  }
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  spi.transfer(0xFF);
  return readable;
}
static uint8_t odysseySdProtocolProbe() {
  SD.end();
  odysseySdSpi.end();
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  odysseySdSpi.begin(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS);
  odysseySdSpi.beginTransaction(SPISettings(400000,MSBFIRST,SPI_MODE0));
  for (uint8_t i=0;i<20;++i) odysseySdSpi.transfer(0xFF); // >= 74 clocks with CS high

  uint8_t r0=0xFF;
  for (uint8_t attempt=0;attempt<2 && r0!=0x01;++attempt)
    r0=odysseySdRawCommand(odysseySdSpi,0,0,0x95);
  if (r0!=0x01) {
    odysseySdSpi.endTransaction();odysseySdSpi.end();digitalWrite(ODYSSEY_SD_CS,HIGH);
    Serial.printf("[SD] protocol probe stage=1 CMD0=0x%02X\n",unsigned(r0));
    return 1;
  }

  uint8_t stage=2,r7[4]={0xFF,0xFF,0xFF,0xFF};
  const uint8_t r8=odysseySdRawCommand(odysseySdSpi,8,0x000001AAu,0x87,r7,sizeof(r7));
  const bool v2=(r8==0x01 && r7[2]==0x01 && r7[3]==0xAA);
  const bool legacy=(r8&0x04u)!=0;
  if (v2 || legacy) stage=3;
  else {
    odysseySdSpi.endTransaction();odysseySdSpi.end();digitalWrite(ODYSSEY_SD_CS,HIGH);
    Serial.printf("[SD] protocol probe stage=%u CMD8=0x%02X echo=%02X%02X%02X%02X\n",
      unsigned(stage),unsigned(r8),unsigned(r7[0]),unsigned(r7[1]),unsigned(r7[2]),unsigned(r7[3]));
    return stage;
  }

  bool initialized=false;
  const uint32_t acmdArg=v2?0x40000000u:0u;
  for (uint16_t attempt=0;attempt<120 && !initialized;++attempt) {
    const uint8_t r55=odysseySdRawCommand(odysseySdSpi,55,0,0x01);
    if (r55==0x00 || r55==0x01) {
      const uint8_t r41=odysseySdRawCommand(odysseySdSpi,41,acmdArg,0x01);
      if (r41==0x00) { initialized=true;break; }
    }
    delay(10);
  }
  if (!initialized && legacy) {
    for (uint16_t attempt=0;attempt<120 && !initialized;++attempt) {
      if (odysseySdRawCommand(odysseySdSpi,1,0,0x01)==0x00) { initialized=true;break; }
      delay(10);
    }
  }
  if (!initialized) {
    odysseySdSpi.endTransaction();odysseySdSpi.end();digitalWrite(ODYSSEY_SD_CS,HIGH);
    Serial.println("[SD] protocol probe stage=3 card stayed idle");
    return 3;
  }
  stage=4;

  uint8_t ocr[4]={0,0,0,0};
  const uint8_t r58=odysseySdRawCommand(odysseySdSpi,58,0,0x01,ocr,sizeof(ocr));
  const bool blockAddressing=(r58==0x00 && (ocr[0]&0x40u)!=0);
  if (!blockAddressing) odysseySdRawCommand(odysseySdSpi,16,512,0x01);
  if (odysseySdReadSectorZero(odysseySdSpi,blockAddressing)) stage=5;

  odysseySdSpi.endTransaction();
  odysseySdSpi.end();
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  Serial.printf("[SD] protocol probe stage=%u CMD8=0x%02X OCR=%02X%02X%02X%02X block=%u\n",
    unsigned(stage),unsigned(r8),unsigned(ocr[0]),unsigned(ocr[1]),unsigned(ocr[2]),unsigned(ocr[3]),
    blockAddressing?1u:0u);
  return stage;
}
#endif
uint8_t odysseySdDetectionState() { return odysseySdBootState; }
uint8_t odysseySdProbeState() { return odysseySdBootState.load()==1 ? 6 : odysseySdProbeStage.load(); }

static void odysseyDetectSdCardAttempt(bool diagnoseFailure) {
  (void)diagnoseFailure;
#if CONFIG_IDF_TARGET_ESP32C3
  if (odysseySdBootState.load()==1 && SD.cardType()!=CARD_NONE) {
    Serial.println("[SD] healthy mount retained");
    return;
  }
#endif
  odysseySdBootState=0;
  odysseySdProbeStage=0;
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
      odysseySdProbeStage=6;
      const char* label=type==CARD_MMC?"MMC":type==CARD_SD?"SDSC":type==CARD_SDHC?"SDHC/SDXC":"unknown";
      Serial.printf("[SD] detected: %s, %llu MiB; filesystem mounted\n", label,
        static_cast<unsigned long long>(SD.cardSize()/(1024ULL*1024ULL)));
    } else {
      odysseySdBootState=3;
      Serial.println("[SD] no card reported");
    }
  } else {
    odysseySdBootState=2;
#if CONFIG_IDF_TARGET_ESP32C3
    if (diagnoseFailure) odysseySdProbeStage=odysseySdProtocolProbe();
#endif
    Serial.printf("[SD] detection/mount failed probeStage=%u\n",unsigned(odysseySdProbeStage.load()));
  }
#if CONFIG_IDF_TARGET_ESP32C3
  if (odysseySdBootState==1) return;
#endif
  SD.end();
  sdSpi.end();
  digitalWrite(ODYSSEY_SD_CS, HIGH);
}
void odysseyDetectSdCard() { odysseyDetectSdCardAttempt(true); }

#if CONFIG_IDF_TARGET_ESP32C3
static bool odysseyMountWithRetries(const char* reason,uint32_t initialSettleMs) {
  if (odysseySdBootState.load()==1 && SD.cardType()!=CARD_NONE) return true;
  if (initialSettleMs) delay(initialSettleMs);
  for (uint8_t attempt=1;attempt<=ODYSSEY_SD_MOUNT_ATTEMPTS;++attempt) {
    Serial.printf("[SD] %s mount attempt %u/%u\n",reason,unsigned(attempt),unsigned(ODYSSEY_SD_MOUNT_ATTEMPTS));
    odysseyDetectSdCardAttempt(false);
    if (odysseySdBootState.load()==1) return true;
    if (attempt<ODYSSEY_SD_MOUNT_ATTEMPTS) delay(ODYSSEY_SD_RETRY_SETTLE_MS);
  }
  odysseySdProbeStage=odysseySdProtocolProbe();
  Serial.printf("[SD] %s protocol diagnosis stage=%u\n",reason,unsigned(odysseySdProbeStage.load()));
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
