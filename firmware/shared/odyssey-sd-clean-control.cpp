// Odyssey SD clean-room control build.
// C3: SD is intentionally disabled. No SPI, mount, catalogue, read, write or
// offline recording code is compiled. This isolates BLE/audio/touch/power.
// Odyssey S3 retains its existing detection-only probe.
#if !SYNAP_CHAKSHU
#include <SPI.h>
#include <SD.h>

static std::atomic<uint8_t> odysseySdBootState{0};
static std::atomic<uint8_t> odysseySdProbeStage{0};
uint8_t odysseySdDetectionState() { return odysseySdBootState.load(); }
uint8_t odysseySdProbeState() { return odysseySdProbeStage.load(); }

#if CONFIG_IDF_TARGET_ESP32C3
void odysseyInitializeSdCardBeforeBle() {
  odysseySdBootState=3;
  odysseySdProbeStage=0;
  odysseySdRecoveryActive=false;
  odysseyCaptureActive=false;
  odysseyRecording=false;
  odysseyStopRequested=false;
  Serial.println("[SD] clean-room control: Odyssey C3 SD disabled");
}

void odysseyToggleRecording() {
  odysseySdRecoveryActive=false;
  odysseyCaptureActive=false;
  odysseyRecording=false;
  odysseyStopRequested=false;
  odysseyRecordFaultAt=millis();
  updateStatusLed(true);
  Serial.println("[TOUCH] double tap -> SD disabled in control build");
}

bool odysseyPrepareForConnectedStreaming(uint32_t) { return true; }
bool odysseyPrepareSdForPowerTransition(uint32_t) { return true; }

namespace OdysseyTransfer {
void initialize() {}
void ble(BLEService*) {}
bool available() { return false; }
}

#elif CONFIG_IDF_TARGET_ESP32S3
constexpr int ODYSSEY_SD_CS=SYNAP_SD_CS_PIN, ODYSSEY_SD_SCK=SYNAP_SD_SCK_PIN;
constexpr int ODYSSEY_SD_MOSI=SYNAP_SD_MOSI_PIN, ODYSSEY_SD_MISO=SYNAP_SD_MISO_PIN;
static SPIClass odysseySdSpi(FSPI);

void odysseyDetectSdCard() {
  odysseySdBootState=0;
  odysseySdProbeStage=0;
  SD.end();
  odysseySdSpi.end();
  pinMode(ODYSSEY_SD_CS,OUTPUT);
  digitalWrite(ODYSSEY_SD_CS,HIGH);
  odysseySdSpi.begin(ODYSSEY_SD_SCK,ODYSSEY_SD_MISO,ODYSSEY_SD_MOSI,ODYSSEY_SD_CS);
  const bool mounted=SD.begin(ODYSSEY_SD_CS,odysseySdSpi,400000,"/odyssey-sd",1,false);
  if (mounted && SD.cardType()!=CARD_NONE) {
    odysseySdBootState=1;
    odysseySdProbeStage=6;
    Serial.println("[SD] Odyssey S3 detection succeeded");
  } else if (mounted) {
    odysseySdBootState=3;
    Serial.println("[SD] Odyssey S3 no card reported");
  } else {
    odysseySdBootState=2;
    Serial.println("[SD] Odyssey S3 detection/mount failed");
  }
  SD.end();
  odysseySdSpi.end();
  digitalWrite(ODYSSEY_SD_CS,HIGH);
}
#else
#error Unsupported Odyssey SD target
#endif
#endif
