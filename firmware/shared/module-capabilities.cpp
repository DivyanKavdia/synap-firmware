// SYNAP_BOARD_FEATURES
// Versioned 20-byte descriptor fits the default ATT payload; names are display-only.
// Bits: audio, camera, SD, flash settings, touch, battery, standby, MJPEG, SD WAV, photo.
void encodeModuleCapabilities(uint8_t* p) {
  memset(p,0,20);p[0]=0xC7;p[1]=1;p[2]=SYNAP_MODULE_ID;p[3]=1;
  uint16_t supported=1|8|16|32|64,ready=8|16|64;
  uint16_t sensor=0;
#if USE_REAL_I2S_MIC
  if (microphoneValidated.load()) ready|=1;
#endif
  if (batteryAvailable) ready|=32;
#if SYNAP_CHAKSHU
  ChakshuMedia::Snapshot status;ChakshuMedia::copy(status);
  supported=1|2|4|8|128|256|512;ready=8|status.ready;
  if (status.ready&2) ready|=128|512;
  if ((status.ready&5)==5) ready|=256;
  sensor=status.sensor;
  p[14]=ChakshuTransfer::requests?1:0;
#endif
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

