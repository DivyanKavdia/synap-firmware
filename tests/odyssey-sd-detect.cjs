'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {getTarget}=require('../tools/targets.cjs');
const {renderProfile}=require('../tools/device-profile.cjs');
const sdFlags=chip=>Object.entries(getTarget(chip==='ESP32C3'?'esp32c3-supermini-4m':'esp32s3-fh4r2-qspi-4m').hardware.sdDetection)
  .map(([signal,pin])=>`-DSYNAP_SD_${signal.toUpperCase()}_PIN=${pin}`);
const {nativeTest}=require('./support/native.cjs');
const source=fs.readFileSync(path.join(__dirname,'../firmware/shared/odyssey-sd-detect.cpp'),'utf8').replace(/^#include.*$/gm,'');
const stub=`
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#define HIGH 1
#define OUTPUT 1
#define FSPI 0
#define CARD_NONE 0
#define CARD_MMC 1
#define CARD_SD 2
#define CARD_SDHC 3
constexpr int I2S_BCLK_PIN=4,I2S_WS_PIN=5,I2S_DATA_IN_PIN=6;
#if CONFIG_IDF_TARGET_ESP32C3
constexpr int TOUCH_INPUT_PIN=3,BATTERY_ADC_PIN=1,RGB_LED_PIN=8;
#else
constexpr int TOUCH_INPUT_PIN=13,BATTERY_ADC_PIN=8,RGB_LED_PIN=48;
#endif
int spiEnds=0, sdEnds=0, csLevel=0;
void digitalWrite(int,int level){csLevel=level;}
void pinMode(int,int){}
struct SPIClass {
 explicit SPIClass(int){}
 void begin(int sck,int miso,int mosi,int cs){
#if CONFIG_IDF_TARGET_ESP32C3
 assert(sck==10 && miso==20 && mosi==21 && cs==0);
#else
 assert(sck==12 && miso==11 && mosi==10 && cs==9);
#endif
 }
 void end(){++spiEnds;}
};
struct SerialStub {
 std::string log;
 template<typename... T> void printf(const char* f,T... v){char b[256];snprintf(b,sizeof(b),f,v...);log+=b;}
 void println(const char* s){log+=s;}
} Serial;
struct SDStub {
 bool mounted=true;uint8_t type=CARD_SDHC;
 bool begin(int,SPIClass&,int hz,const char* path,int files,bool format){
  assert(hz==400000 && std::string(path)=="/odyssey-sd" && files==1 && !format);return mounted;
 }
 uint8_t cardType(){return type;}
 uint64_t cardSize(){return 8ULL*1024*1024*1024;}
 void end(){++sdEnds;}
} SD;
`;
for(const chip of ['ESP32C3','ESP32S3'])test(`${chip}: SD probe handles success, absent card and mount failure, retains C3 ready mount and cleans up failures`,()=>{
 nativeTest(stub+source+`
 int main(){
 assert(odysseySdDetectionState()==0);
 odysseyDetectSdCard();assert(odysseySdDetectionState()==1);assert(Serial.log.find("detected: SDHC/SDXC, 8192 MiB")!=std::string::npos);
 Serial.log.clear();SD.type=CARD_NONE;odysseyDetectSdCard();assert(odysseySdDetectionState()==3);assert(Serial.log.find("no card reported")!=std::string::npos);
 Serial.log.clear();SD.mounted=false;odysseyDetectSdCard();assert(odysseySdDetectionState()==2);assert(Serial.log.find("detection/mount failed")!=std::string::npos);
 #if CONFIG_IDF_TARGET_ESP32C3
 assert(spiEnds==5 && sdEnds==5 && csLevel==HIGH);
#else
 assert(spiEnds==3 && sdEnds==3 && csLevel==HIGH);
#endif
 }
 `,[`-DCONFIG_IDF_TARGET_${chip}=1`,'-DARDUINO_USB_CDC_ON_BOOT=1',...sdFlags(chip)]);
});
test('C3 rejects UART Serial and an overlapping peripheral pin at compile time',()=>{
 assert.throws(()=>nativeTest(stub+source+'\nint main(){}',['-DCONFIG_IDF_TARGET_ESP32C3=1',...sdFlags('ESP32C3')]),/enable USB CDC/);
 assert.throws(()=>nativeTest(stub.replace('TOUCH_INPUT_PIN=3','TOUCH_INPUT_PIN=10')+source+'\nint main(){}',
 ['-DCONFIG_IDF_TARGET_ESP32C3=1','-DARDUINO_USB_CDC_ON_BOOT=1',...sdFlags('ESP32C3')]),/SD pin overlaps/);
});

test('device profile emits the agreed SD pins only for Odyssey',()=>{
 for (const id of ['esp32c3-supermini-4m','esp32s3-fh4r2-qspi-4m']) {
  const target=getTarget(id),profile=renderProfile(target);
  for(const [signal,pin] of Object.entries(target.hardware.sdDetection))
   assert(profile.includes(`#define SYNAP_SD_${signal.toUpperCase()}_PIN ${pin}`));
  assert(!target.features.includes('sd'), 'Detection alone must not advertise SD recording/sync');
 }
 assert(!renderProfile(getTarget('xiao-esp32s3-sense-8m')).includes('SYNAP_SD_'));
});
