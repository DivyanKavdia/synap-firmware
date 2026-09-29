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
// Raw SD probe: 0=not checked, 1=CMD0 replied but raw init did not finish, 2=no SPI reply,
 // 3=card initialized but sector 0 could not be read, 4=sector 0 lacks 0x55AA,
 // 5=sector 0 is readable/valid, 6=FAT/exFAT boot sector is readable.
static std::atomic<uint8_t> odysseySdBootState{0};
static std::atomic<uint8_t> odysseySdElectricalState{0};
#if CONFIG_IDF_TARGET_ESP32C3
static SPIClass odysseySdSpi(FSPI);
static std::atomic<bool> odysseySdProbeBusy{false};
static constexpr uint32_t ODYSSEY_SD_STARTUP_SETTLE_MS=3000u;

static void odysseyWaitForSdStartupSettle() {
  const uint32_t now=millis();
  if (now<ODYSSEY_SD_STARTUP_SETTLE_MS) {
    const uint32_t waitMs=ODYSSEY_SD_STARTUP_SETTLE_MS-now;
    Serial.printf("[SD] startup settle wait %lu ms\n",static_cast<unsigned long>(waitMs));
    delay(waitMs);
  }
}
#endif
uint8_t odysseySdDetectionState() { return odysseySdBootState; }
uint8_t odysseySdProbeState() { return odysseySdElectricalState; }

#if CONFIG_IDF_TARGET_ESP32C3
static uint8_t odysseySdCommand(uint8_t cmd,uint32_t arg,uint8_t crc,
    uint8_t* tail=nullptr,size_t tailSize=0,bool release=true) {
  SPIClass& spi=odysseySdSpi;
  digitalWrite(ODYSSEY_SD_CS,HIGH);spi.transfer(0xff);
  digitalWrite(ODYSSEY_SD_CS,LOW);
  spi.transfer(uint8_t(0x40u|cmd));
  spi.transfer(uint8_t(arg>>24));spi.transfer(uint8_t(arg>>16));
  spi.transfer(uint8_t(arg>>8));spi.transfer(uint8_t(arg));spi.transfer(crc);
  uint8_t r1=0xff;
  for(uint8_t i=0;i<16;++i){const uint8_t v=spi.transfer(0xff);if((v&0x80u)==0){r1=v;break;}}
  if(r1!=0xff && tail) for(size_t i=0;i<tailSize;++i) tail[i]=spi.transfer(0xff);
  if(release){digitalWrite(ODYSSEY_SD_CS,HIGH);spi.transfer(0xff);}
  return r1;
}

static bool odysseyRawReadSector(uint32_t lba,bool blockAddressed,uint8_t* sector) {
  SPIClass& spi=odysseySdSpi;
  const uint64_t byteAddress=uint64_t(lba)*512ULL;
  if(!blockAddressed && byteAddress>0xffffffffULL) return false;
  const uint32_t arg=blockAddressed?lba:uint32_t(byteAddress);
  const uint8_t r1=odysseySdCommand(17,arg,0x01,nullptr,0,false);
  if(r1!=0x00){digitalWrite(ODYSSEY_SD_CS,HIGH);spi.transfer(0xff);return false;}
  uint8_t token=0xff;
  for(uint16_t i=0;i<10000 && token==0xff;++i) token=spi.transfer(0xff);
  if(token!=0xfe){digitalWrite(ODYSSEY_SD_CS,HIGH);spi.transfer(0xff);return false;}
  for(size_t i=0;i<512;++i) sector[i]=spi.transfer(0xff);
  spi.transfer(0xff);spi.transfer(0xff); // discard data CRC
  digitalWrite(ODYSSEY_SD_CS,HIGH);spi.transfer(0xff);
  return true;
}

static bool odysseyLooksLikeFat(const uint8_t* sector) {
  return !memcmp(sector+3,"EXFAT   ",8) || !memcmp(sector+54,"FAT",3) || !memcmp(sector+82,"FAT",3);
}

static uint8_t odysseyRawSdInspect(uint32_t hz) {
  SPIClass& spi=odysseySdSpi;
  spi.beginTransaction(SPISettings(hz,MSBFIRST,SPI_MODE0));
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  for(uint8_t i=0;i<12;++i) spi.transfer(0xff);

  const uint8_t r0=odysseySdCommand(0,0,0x95);
  if(r0==0xff){spi.endTransaction();Serial.println("[SD] raw inspect: no CMD0 response");return 2;}

  uint8_t r7[4]{};
  const uint8_t r8=odysseySdCommand(8,0x1aa,0x87,r7,sizeof(r7));
  const bool v2=r8==0x01 && r7[2]==0x01 && r7[3]==0xaa;
  if(!v2 && !(r8&0x04u)){spi.endTransaction();Serial.printf("[SD] raw inspect: CMD8 R1=0x%02x\n",r8);return 1;}

  bool initialized=false;
  const uint32_t started=millis();
  while(uint32_t(millis()-started)<1500u){
    const uint8_t r55=odysseySdCommand(55,0,0x01);
    if(r55>0x01) break;
    const uint8_t ra=odysseySdCommand(41,v2?0x40000000u:0u,0x01);
    if(ra==0x00){initialized=true;break;}
    if(ra!=0x01) break;
    delay(5);
  }
  if(!initialized){spi.endTransaction();Serial.println("[SD] raw inspect: ACMD41 did not reach ready");return 1;}

  uint8_t ocr[4]{};
  if(odysseySdCommand(58,0,0x01,ocr,sizeof(ocr))!=0x00){
    spi.endTransaction();Serial.println("[SD] raw inspect: CMD58 failed");return 1;
  }
  const bool blockAddressed=(ocr[0]&0x40u)!=0;
  if(!blockAddressed && odysseySdCommand(16,512,0x01)!=0x00){
    spi.endTransaction();Serial.println("[SD] raw inspect: CMD16 failed");return 1;
  }

  uint8_t sector[512]{};
  if(!odysseyRawReadSector(0,blockAddressed,sector)){
    spi.endTransaction();Serial.println("[SD] raw inspect: card ready but sector 0 unreadable");return 3;
  }
  const bool signature=sector[510]==0x55 && sector[511]==0xaa;
  if(!signature){spi.endTransaction();Serial.println("[SD] raw inspect: sector 0 missing 0x55AA");return 4;}
  if(odysseyLooksLikeFat(sector)){
    spi.endTransaction();Serial.println("[SD] raw inspect: FAT/exFAT boot sector readable");return 6;
  }

  // MBR case: inspect the first non-empty partition boot sector without writing.
  for(uint8_t slot=0;slot<4;++slot){
    const size_t base=446u+size_t(slot)*16u;
    const uint8_t type=sector[base+4];
    const uint32_t lba=uint32_t(sector[base+8]) | (uint32_t(sector[base+9])<<8) |
      (uint32_t(sector[base+10])<<16) | (uint32_t(sector[base+11])<<24);
    if(!type || !lba) continue;
    uint8_t boot[512]{};
    if(odysseyRawReadSector(lba,blockAddressed,boot) &&
       boot[510]==0x55 && boot[511]==0xaa && odysseyLooksLikeFat(boot)){
      spi.endTransaction();Serial.printf("[SD] raw inspect: FAT/exFAT partition readable at LBA %lu\n",
        static_cast<unsigned long>(lba));return 6;
    }
    break;
  }
  spi.endTransaction();
  Serial.println("[SD] raw inspect: sector 0 valid, filesystem boot sector unrecognized");
  return 5;
}

static uint8_t odysseyRawSdProbe(uint32_t hz) {
  SPIClass& spi=odysseySdSpi;
  spi.beginTransaction(SPISettings(hz,MSBFIRST,SPI_MODE0));
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  for (uint8_t i=0;i<12;++i) spi.transfer(0xff);
  const uint8_t r1=odysseySdCommand(0,0,0x95);
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

  // The SD adapter remains powered across software resets. Give it a short
  // post-boot settle window, and serialize mount attempts from the background
  // boot probe and any early PWA media request.
  odysseyWaitForSdStartupSettle();
  bool expected=false;
  if (!odysseySdProbeBusy.compare_exchange_strong(expected,true)) {
    const uint32_t started=millis();
    while (odysseySdProbeBusy.load() && uint32_t(millis()-started)<5000u) delay(10);
    return;
  }

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
        odysseySdProbeBusy=false;
        return;
      }
      sawCardWithoutType=true;
    }

    SD.end();
    sdSpi.end();
    digitalWrite(ODYSSEY_SD_CS, HIGH);
  }

  odysseySdBootState=sawCardWithoutType?3:2;
  // Mount failed after all normal SD.h attempts. Run a read-only raw-card
  // initialization and sector inspection to distinguish library/filesystem
  // failure from electrical/card-initialization failure.
  SD.end();sdSpi.end();
  pinMode(ODYSSEY_SD_CS,OUTPUT);digitalWrite(ODYSSEY_SD_CS,HIGH);
  sdSpi.begin(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS);
  delay(10);
  odysseySdElectricalState=odysseyRawSdInspect(400000u);
  sdSpi.end();digitalWrite(ODYSSEY_SD_CS,HIGH);
  Serial.printf("[SD] recovery exhausted: mount=%u raw=%u\n",
    unsigned(odysseySdBootState.load()),unsigned(odysseySdElectricalState.load()));
  odysseySdProbeBusy=false;
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
#if CONFIG_IDF_TARGET_ESP32C3
static void odysseyDelayedSdProbeTask(void*) {
  odysseyWaitForSdStartupSettle();
  odysseyDetectSdCard();
  vTaskDelete(nullptr);
}
bool odysseyScheduleSdCardDetection() {
  const BaseType_t created=xTaskCreate(odysseyDelayedSdProbeTask,"sd-boot",3072,nullptr,1,nullptr);
  if (created!=pdPASS) {
    Serial.println("[SD] delayed activation task unavailable; probing inline");
    return false;
  }
  Serial.printf("[SD] activation scheduled after %lu ms\n",
    static_cast<unsigned long>(ODYSSEY_SD_STARTUP_SETTLE_MS));
  return true;
}
#endif
#endif
