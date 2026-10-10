// SYNAP_BATTERY_RUNTIME_BEGIN
#if CONFIG_IDF_TARGET_ESP32C3
static std::atomic<bool> odysseySdBatteryDividerObserved{false};
void markOdysseySdBatteryDividerPresent() { odysseySdBatteryDividerObserved=true; }
bool odysseySdBatteryDividerPresent() { return odysseySdBatteryDividerObserved.load(); }
// Field Odyssey C3+SD is assembled with R1=1 MOhm from SW_BAT
// to GPIO1 and R2=470 kOhm from GPIO1 to GND (1470/470 ratio).
// Standard C3 remains x2. Rev K's archived BOM differs (470k/470k);
// SD detection selects the field profile, not a resistor measurement.
// Do not compensate untrusted ADC readings with a guessed gain.
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
  // Standard C3 remains 2:1; assembled C3+SD uses 1M/470k.
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
    // Assembled C3+SD R1=1 MOhm, R2=470 kOhm, Rth~320 kOhm; C1=100 nF.
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
// Divider is documented as x2 on Rev K. It is not inferred from voltage.
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

