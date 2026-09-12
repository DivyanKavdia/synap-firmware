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
bool varying=false;
uint32_t millis(){return clockMs;}
void delayMicroseconds(uint32_t us){delayUs+=us;}
uint16_t analogRead(uint8_t pin){assert(pin==BATTERY_ADC_PIN);++rawReads;return 3000;}
uint32_t analogReadMilliVolts(uint8_t pin){assert(pin==BATTERY_ADC_PIN);return adcMv+(varying?((mvReads++%2)?100:-100):0);}
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
  adcMv=CONFIG_IDF_TARGET_ESP32C3?2075:1320;
#if !SYNAP_BATTERY_MONITOR_ENABLE
  sampleBattery(true);
  assert(rawReads==0 && !batteryAvailable && !batteryCritical());
#else
  // Synthetic ADC input checks the conversion, not physical ADC accuracy.
  varying=true;
  sampleBattery(true);
  assert(rawReads==17 && mvReads==16 && delayUs==5200);
  assert(batteryAvailable && batteryPercent==100 && !batteryCritical());
  assert(batteryMillivolts==(CONFIG_IDF_TARGET_ESP32C3?4150:4130));
  assert(batteryAdcMillivolts==adcMv && batteryAdcRaw==3000);
  assert(event.value.size()==12 && event.value==control.value);
  assert(event.value[0]==0xB7 && event.value[1]==2 && event.value[3]==1);
  assert(event.value[2]==100); // PWA consumes this percentage with the available flag.
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
  assert(batteryCritical()==!bool(CONFIG_IDF_TARGET_ESP32C3));
  assert(bool(event.value[3]&4)==!bool(CONFIG_IDF_TARGET_ESP32C3));
  for(const uint32_t invalid : {0u,3000u}){
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
#endif
  std::puts("PASS battery conversion, telemetry, range, cadence and target-specific critical policy");
}
