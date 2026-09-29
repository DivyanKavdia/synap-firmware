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
#include <vector>
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define MSBFIRST 1
#define SPI_MODE0 0
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
int spiEnds=0,sdEnds=0,csLevel=0,beginCalls=0,lowTransfers=0;\nuint8_t rawReply=0x01;\nuint32_t nowMs=5000;\nusing BaseType_t=int;\n#define pdPASS 1
std::vector<uint32_t> clocks;
void digitalWrite(int,int level){csLevel=level;if(level==LOW)lowTransfers=0;}
void pinMode(int,int){}
uint32_t millis(){return nowMs;}
void delay(uint32_t ms){nowMs+=ms;}
int xTaskCreate(void(*)(void*),const char*,uint32_t,void*,int,void**){return pdPASS;}
void vTaskDelete(void*){}
struct SPISettings { SPISettings(uint32_t,int,int){} };\nstruct SPIClass {\n explicit SPIClass(int){}
 void begin(int sck,int miso,int mosi,int cs){
#if CONFIG_IDF_TARGET_ESP32C3
 assert(sck==10 && miso==20 && mosi==21 && cs==0);
#else
 assert(sck==12 && miso==11 && mosi==10 && cs==9);
#endif
 }
 void beginTransaction(const SPISettings&){}\n void endTransaction(){}\n uint8_t transfer(uint8_t){if(csLevel==HIGH)return 0xff;return ++lowTransfers>6?rawReply:0xff;}\n void end(){++spiEnds;}\n};
struct SerialStub {
 std::string log;
 template<typename... T> void printf(const char* f,T... v){char b[320];snprintf(b,sizeof(b),f,v...);log+=b;}
 void println(const char* s){log+=s;}
} Serial;
struct SDStub {
 bool mounted=true;uint8_t type=CARD_SDHC;int failBegins=0;
 bool begin(int,SPIClass&,uint32_t hz,const char* path,int files,bool format){
  ++beginCalls;clocks.push_back(hz);
  assert(std::string(path)=="/odyssey-sd" && files==1 && !format);
  if(failBegins>0){--failBegins;return false;}
  return mounted;
 }
 uint8_t cardType(){return type;}
 uint64_t cardSize(){return 8ULL*1024*1024*1024;}
 void end(){++sdEnds;}
} SD;
`;

test('ESP32C3: preserves a healthy mount and recovers failed mounts at progressively lower safe SPI clocks',()=>{
 nativeTest(stub+source+`
 int main(){
  assert(odysseySdDetectionState()==0);
  odysseyDetectSdCard();
  assert(odysseySdDetectionState()==1 && odysseySdProbeState()==1 && beginCalls==1 && clocks.back()==400000u);
  assert(Serial.log.find("filesystem mounted at 400000 Hz on attempt 1")!=std::string::npos);

  // A healthy mount is retained; no destructive remount is attempted.
  const int beginBefore=beginCalls,sdEndBefore=sdEnds,spiEndBefore=spiEnds;
  odysseyDetectSdCard();
  assert(beginCalls==beginBefore && sdEnds==sdEndBefore && spiEnds==spiEndBefore);

  // A previously failed/offline state retries 400k, 400k, then 250k.
  odysseySdBootState=2;SD.failBegins=2;SD.mounted=true;SD.type=CARD_SDHC;clocks.clear();
  odysseyDetectSdCard();
  assert(odysseySdDetectionState()==1);
  assert(clocks.size()==3 && clocks[0]==400000u && clocks[1]==400000u && clocks[2]==250000u);

  // Complete failure exhausts the bounded sequence and stays safely offline.
  odysseySdBootState=2;SD.mounted=false;SD.failBegins=0;rawReply=0xff;clocks.clear();
  odysseyDetectSdCard();
  assert(odysseySdDetectionState()==2 && odysseySdProbeState()==2);
  assert(clocks.size()==4 && clocks[0]==400000u && clocks[1]==400000u && clocks[2]==250000u && clocks[3]==125000u);
  assert(Serial.log.find("recovery exhausted")!=std::string::npos);

  // A card that responds without a usable type is distinct from a mount failure.
  odysseySdBootState=2;SD.mounted=true;SD.type=CARD_NONE;rawReply=0x01;clocks.clear();
  odysseyDetectSdCard();
  assert(odysseySdDetectionState()==3 && odysseySdProbeState()==1 && clocks.size()==4);
  assert(Serial.log.find("recovery exhausted: mount=3 raw=")!=std::string::npos);
  assert(csLevel==HIGH);
 }
 `,['-DCONFIG_IDF_TARGET_ESP32C3=1','-DARDUINO_USB_CDC_ON_BOOT=1',...sdFlags('ESP32C3')]);
});

test('ESP32S3: Odyssey probe remains one-shot and releases the bus',()=>{
 nativeTest(stub+source+`
 int main(){
  odysseyDetectSdCard();assert(odysseySdDetectionState()==1);assert(beginCalls==1 && clocks[0]==400000u);
  assert(spiEnds==1 && sdEnds==1 && csLevel==HIGH);
  SD.type=CARD_NONE;odysseyDetectSdCard();assert(odysseySdDetectionState()==3);
  SD.type=CARD_SDHC;SD.mounted=false;odysseyDetectSdCard();assert(odysseySdDetectionState()==2);
  assert(beginCalls==3 && spiEnds==3 && sdEnds==3);
 }
 `,['-DCONFIG_IDF_TARGET_ESP32S3=1','-DARDUINO_USB_CDC_ON_BOOT=1',...sdFlags('ESP32S3')]);
});

test('C3 rejects UART Serial and an overlapping peripheral pin at compile time',()=>{
 assert.throws(()=>nativeTest(stub+source+'\\nint main(){}',['-DCONFIG_IDF_TARGET_ESP32C3=1',...sdFlags('ESP32C3')]),/enable USB CDC/);
 assert.throws(()=>nativeTest(stub.replace('TOUCH_INPUT_PIN=3','TOUCH_INPUT_PIN=10')+source+'\\nint main(){}',
 ['-DCONFIG_IDF_TARGET_ESP32C3=1','-DARDUINO_USB_CDC_ON_BOOT=1',...sdFlags('ESP32C3')]),/SD pin overlaps/);
});

test('device profile emits Odyssey SD pins and only C3 advertises SD sync',()=>{
 for (const id of ['esp32c3-supermini-4m','esp32s3-fh4r2-qspi-4m']) {
  const target=getTarget(id),profile=renderProfile(target);
  for(const [signal,pin] of Object.entries(target.hardware.sdDetection))
   assert(profile.includes(`#define SYNAP_SD_${signal.toUpperCase()}_PIN ${pin}`));
  assert.equal(target.features.includes('sd'), id==='esp32c3-supermini-4m');
 }
 assert(!renderProfile(getTarget('xiao-esp32s3-sense-8m')).includes('SYNAP_SD_'));
});


test('recognizes FAT and exFAT boot-sector signatures without writing media',()=>{
 nativeTest(stub+source+`
 int main(){
  uint8_t sector[512]{};
  memcpy(sector+82,"FAT32   ",8);assert(odysseyLooksLikeFat(sector));
  memset(sector,0,sizeof(sector));memcpy(sector+54,"FAT16   ",8);assert(odysseyLooksLikeFat(sector));
  memset(sector,0,sizeof(sector));memcpy(sector+3,"EXFAT   ",8);assert(odysseyLooksLikeFat(sector));
  memset(sector,0,sizeof(sector));assert(!odysseyLooksLikeFat(sector));
 }
 `,['-DCONFIG_IDF_TARGET_ESP32C3=1','-DARDUINO_USB_CDC_ON_BOOT=1',...sdFlags('ESP32C3')]);
});
