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
