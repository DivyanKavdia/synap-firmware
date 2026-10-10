#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

constexpr uint8_t BATTERY_ADC_PIN=CONFIG_IDF_TARGET_ESP32C3?1:8;
constexpr uint32_t BATTERY_SAMPLE_MS=15000;
constexpr uint16_t BATTERY_LOW_MV=3600,BATTERY_CRITICAL_MV=3400;
constexpr uint8_t BATTERY_EVENT_MAGIC=0xB7,BATTERY_EVENT_VERSION=2;
enum {ADC_6db=6,ADC_11db=11};
int configuredAttenuation=-1;
void analogSetPinAttenuation(uint8_t pin,int attenuation){assert(pin==BATTERY_ADC_PIN);configuredAttenuation=attenuation;}
uint32_t clockMs=1000,lastBatterySampleAt=0;
uint16_t batteryMillivolts=0,batteryAdcMillivolts=0,batteryAdcRaw=0;
uint8_t batteryPercent=0,batteryValidSamples=0,batteryCriticalSamples=0;
bool batteryAvailable=false;
std::atomic<bool> streamingEnabled{false},deviceConnected{true};
uint32_t adcMv=0,rawReads=0,mvReads=0,delayUs=0;
bool varying=false, spike=false;
uint32_t millis(){return clockMs;}
void delayMicroseconds(uint32_t us){delayUs+=us;}
uint16_t analogRead(uint8_t pin){assert(pin==BATTERY_ADC_PIN);++rawReads;return 3000;}
uint32_t analogReadMilliVolts(uint8_t pin){
  assert(pin==BATTERY_ADC_PIN);
  const uint32_t n=mvReads++;
  if(varying)return adcMv+((n%2)?100:-100);
  if(spike && n%16==5)return adcMv+300;
  return adcMv;
}
struct Characteristic {
  std::vector<uint8_t> value;
  int notifications=0;
  void setValue(const uint8_t* bytes,size_t size){value.assign(bytes,bytes+size);}
  void notify(){++notifications;}
} control,event;
auto* controlCharacteristic=&control;
auto* eventCharacteristic=&event;
struct Logger {template<class... T> void printf(const char*,T...) {}} Serial;
uint32_t pdMS_TO_TICKS(uint32_t ms){return ms;}
void vTaskDelay(uint32_t){}
void updateStatusCharacteristic(bool){}
void updateStatusLed(bool){}
// INSERT CONFIGURATION
// INSERT BATTERY
uint16_t word(size_t offset){return event.value[offset]|uint16_t(event.value[offset+1])<<8;}
int main(){
  assert(batteryPercentFromMillivolts(CONFIG_IDF_TARGET_ESP32C3?4149:4129)==99);
  assert(batteryPercentFromMillivolts(CONFIG_IDF_TARGET_ESP32C3?4150:4130)==100);
  configureBatteryAdc();
  assert(configuredAttenuation==(CONFIG_IDF_TARGET_ESP32C3?ADC_11db:ADC_6db));
  adcMv=CONFIG_IDF_TARGET_ESP32C3?1995:1320;
#if !SYNAP_BATTERY_MONITOR_ENABLE
  sampleBattery(true);
  assert(rawReads==0 && !batteryAvailable && !batteryCritical());
#else
  // Synthetic ADC input checks the conversion, not physical ADC accuracy.
  varying=true;
  sampleBattery(true);
  assert(rawReads==17 && mvReads==16 && delayUs==5200);
  assert(batteryAvailable && batteryPercent==(CONFIG_IDF_TARGET_ESP32C3?84:100) && !batteryCritical());
  assert(batteryMillivolts==(CONFIG_IDF_TARGET_ESP32C3?3990:4130));
  assert(batteryAdcMillivolts==adcMv && batteryAdcRaw==3000);
  assert(event.value.size()==12 && event.value==control.value);
  assert(event.value[0]==0xB7 && event.value[1]==2 && event.value[3]==1);
  assert(event.value[2]==batteryPercent); // PWA consumes this with the available flag.
  assert(word(4)==batteryMillivolts && word(8)==adcMv && word(10)==3000);
  clockMs+=14999;sampleBattery(false);assert(rawReads==17);
  ++clockMs;sampleBattery(false);assert(rawReads==34);
  const int notifications=event.notifications;
  streamingEnabled=true;sampleBattery(true);
  assert(rawReads==51 && event.notifications==notifications);
  streamingEnabled=false;
  varying=false;adcMv=CONFIG_IDF_TARGET_ESP32C3?1650:1050;
  for(int i=0;i<3;++i)sampleBattery(true);
  assert(batteryAvailable && batteryMillivolts<=BATTERY_CRITICAL_MV);
  assert(bool(event.value[3]&2));
#if SYNAP_BATTERY_MONITOR_ENABLE && SYNAP_BATTERY_ENFORCE
  assert(batteryCritical());
  assert(bool(event.value[3]&4));
#else
  assert(!batteryCritical());
  assert(!(event.value[3]&4));
#endif
  for(const uint32_t invalid : {0u,340u,3000u}){
    adcMv=invalid;sampleBattery(true);
    assert(!batteryAvailable && !batteryCritical() && batteryPercent==0);
    assert(batteryValidSamples==0 && batteryCriticalSamples==0 && event.value[3]==0);
  }
  adcMv=CONFIG_IDF_TARGET_ESP32C3?1900:1215;sampleBattery(true);
  assert(batteryAvailable && batteryPercent>50 && batteryPercent<80);
  // Check periodic sampling through timer wrap, retaining the forced status path.
  clockMs=0xfffffff0u;sampleBattery(true);const auto before=rawReads;
  clockMs+=14999;sampleBattery(false);assert(rawReads==before);
  ++clockMs;sampleBattery(false);assert(rawReads==before+17);
  adcMv=CONFIG_IDF_TARGET_ESP32C3?2075:1320;sampleBattery(true);
  assert(batteryAvailable && batteryPercent==100 && event.value[2]==100);
  assert(batteryMillivolts==(CONFIG_IDF_TARGET_ESP32C3?4150:4130));
#if CONFIG_IDF_TARGET_ESP32C3
  // Standard C3 remains 2:1 while that reconstruction is physically plausible.
  assert(!odysseySdBatteryDividerPresent());
  // SD presence must be confirmed by SD.begin(), not inferred from ADC
  // voltage: both Rev K and standard C3 are x2. This protects older builds.
  adcMv=2051;sampleBattery(true);
  assert(!odysseySdBatteryDividerPresent());
  assert(batteryAvailable && batteryMillivolts==4102);
  markOdysseySdBatteryDividerPresent();
  adcMv=1184;sampleBattery(true);
  assert(odysseySdBatteryDividerPresent());
  assert(batteryAvailable && batteryMillivolts==3703);
  assert(batteryPercentFromMillivolts(4199)==99);
  assert(batteryPercentFromMillivolts(4200)==100);
  adcMv=1286;spike=true;sampleBattery(true);spike=false;
  // C3+SD robust mean removes a 300 mV transient without falsifying the
  // factory-calibrated 1286 mV input or the field 1470/470 divider.
  assert(batteryAvailable && batteryMillivolts==4022 && batteryPercent==87);
  varying=true;sampleBattery(true);varying=false;
  // Alternating +/-100 mV across the *central* 12 readings is unstable;
  // preserve ADC telemetry but never publish a battery percentage.
  assert(!batteryAvailable && !batteryCritical() && batteryPercent==0);
  adcMv=1286;sampleBattery(true);
  assert(batteryAvailable && batteryMillivolts==4022 && batteryPercent==87);
  // Charging plateau uses actual Rev K resistor ratio.
  for (const uint32_t chargingMv : {1343u,1344u,1350u}) {
    adcMv=chargingMv;sampleBattery(true);
    const uint32_t expected=(chargingMv*1470u+235u)/470u;
    assert(batteryAvailable && batteryMillivolts==expected);
    assert(batteryPercent==100 && event.value[2]==100 && (event.value[3]&1));
  }
  // ADC=1.6V reconstructs ~5.0V on the field divider: reject SD writes.
  adcMv=1600;sampleBattery(true);
  assert(!batteryAvailable && batteryPercent==0 && batteryMillivolts==5004);
#endif
#endif
  std::puts("PASS battery conversion, telemetry, range, cadence and target-specific critical policy");
}
