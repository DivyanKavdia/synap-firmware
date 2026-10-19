'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');

test('Chakshu cold-mount diagnostics quarantine controller faults until next boot',()=>{
 const source=fs.readFileSync('firmware/xiao-sense/sd-storage.cpp','utf8');
 const lifecycle=source.slice(source.indexOf('namespace ChakshuStorage {'),source.indexOf('bool capturePath('))+'}\n';
 assert.match(nativeTest(`#include <atomic>
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
constexpr int CARD_NONE=0,CARD_MMC=1,CARD_SD=2,CARD_SDHC=3,CARD_UNKNOWN=4;
constexpr int OUTPUT=1,HIGH=1,INPUT_PULLUP=2;
using sdcard_type_t=int;
bool spiHealthy=true,mountHealthy=true,rootHealthy=true,rootDirectory=true,spaceHealthy=true,rootExists=true,mkdirHealthy=true,cardPresent=true;
bool busStarted=false,fsMounted=false;unsigned resets=0,handles=0;uint32_t lastHz=0,limitHz=1000000;
bool probeCardVisible=false,probeCardUnknown=false,probeRawHealthy=false,probeFsHealthy=false;
std::vector<uint32_t> clocks;
void pinMode(int pin,int mode){assert((pin==21&&mode==OUTPUT)||(pin==8&&mode==INPUT_PULLUP));}
void digitalWrite(int pin,int level){assert(pin==21&&level==HIGH);}
void delay(unsigned){}
unsigned millis(){return 777;}
unsigned esp_reset_reason(){return 3;}
struct File {
 bool opened;
 explicit File(bool value):opened(value){if(opened)++handles;}
 explicit operator bool()const{return opened;}
 bool isDirectory()const{return rootDirectory;}
 void close(){if(opened){--handles;opened=false;}}
 ~File(){close();}
};
struct Spi {
 void end(){assert(!fsMounted&&handles==0);busStarted=false;++resets;}
 bool begin(int sck,int miso,int mosi,int cs){
  assert(sck==7&&miso==8&&mosi==9&&cs==21);
  if(busStarted)return true; // Arduino SPI.begin is a no-op on an already-started bus.
  busStarted=spiHealthy;return busStarted;
 }
} SPI;
struct Card {
 void end(){assert(handles==0);fsMounted=false;busStarted=false;++resets;}
 bool mountUncertain(){return false;}
 uint32_t lastWireCommand(){return UINT32_MAX;}
 uint32_t lastWireError(){return 0;}
 void probeReadOnly(uint8_t& type,bool& attempted,bool& sectorOk,bool& signature,bool& filesystem){
   type=probeCardUnknown?CARD_UNKNOWN:(probeCardVisible?CARD_SDHC:CARD_NONE);
   attempted=probeCardVisible;sectorOk=probeCardVisible&&probeRawHealthy;
   signature=sectorOk;filesystem=probeFsHealthy;
   busStarted=false;
 }
 bool begin(int cs,Spi&,uint32_t hz,const char*,int files,bool format){
  assert(cs==21&&files==5&&!format);busStarted=true;clocks.push_back(hz);lastHz=hz;
  fsMounted=mountHealthy&&hz<=limitHz;return fsMounted;
 }
 int cardType(){return fsMounted&&cardPresent?1:CARD_NONE;}
 bool exists(const char*){return rootExists;}
 bool mkdir(const char*){return mkdirHealthy;}
 File open(const char*){assert(fsMounted);return File(rootHealthy);}
 uint64_t totalBytes(){return spaceHealthy?8000000u:0u;}
 uint64_t usedBytes(){return 1000000u;}
} SD;
uint8_t sdcard_init(uint8_t cs,Spi* spi,int hz){
 assert(cs==21&&spi==&SPI&&hz==400000);return 0;
}
uint8_t sdcard_uninit(uint8_t){return 0;}
bool sdcard_mount(uint8_t,const char* path,uint8_t files,bool format){
 assert(std::string(path)=="/sd-probe"&&files==1&&!format);
 return probeFsHealthy;
}
uint8_t sdcard_unmount(uint8_t){return 0;}
sdcard_type_t sdcard_type(uint8_t){return probeCardUnknown?CARD_UNKNOWN:(probeCardVisible?CARD_SDHC:CARD_NONE);}
uint32_t sdcard_num_sectors(uint8_t){return probeFsHealthy?16000u:0u;}
uint32_t sdcard_sector_size(uint8_t){return 512u;}
bool sd_read_raw(uint8_t,uint8_t* bytes,uint32_t sector){
 assert(sector==0);
 if(!probeCardVisible||!probeRawHealthy)return false;
 memset(bytes,0,512);bytes[510]=0x55;bytes[511]=0xAA;return true;
}
struct {template<class... T> void printf(const char*,T...){} } Serial;
struct {unsigned getFreeHeap(){return 40000;}} ESP;
unsigned esp_random(){return 123;}
${lifecycle}
int main(){
 using namespace ChakshuStorage;
 // Cold detection first resets any inherited SPI state, then keeps one owned
 // Sense-pin session while falling back through compatibility clocks.
 busStarted=true;
 assert(begin(false)&&clockHz==1000000&&capacity==8000000&&freeBytes==7000000);
 assert((clocks==std::vector<uint32_t>{1000000u}));
 assert(!recoveryClockLocked);
 const unsigned before=resets;assert(begin(false)&&resets==before);

 // In-flight I/O uncertainty never triggers another same-rail mount attempt.
 assert(!recoverIO()&&sdQuarantined.load()&&recoveryClockLocked&&!ready);
 assert(resets==before&&busStarted&&fsMounted);
 clocks.clear();assert(!begin(false)&&!begin(true)&&clocks.empty());
 // This mock simulates a physical SD-rail power cycle.
 sdQuarantined=false;sdQuarantineReason="none";recoveryClockLocked=false;ready=false;

 // A card that never mounted is not falsely treated as a recovered card.
 ready=false;recoveryClockLocked=false;clockHz=2000000u;mountHealthy=false;clocks.clear();
 probeCardVisible=false;probeRawHealthy=false;probeFsHealthy=false;
 assert(!begin(true));
 assert((clocks==std::vector<uint32_t>{1000000u,400000u}));
 assert(!recoveryClockLocked&&!busStarted&&!fsMounted&&sdQuarantined.load());
 clocks.clear();assert(!begin(true)&&clocks.empty());
 // Diagnostics must show the full ladder and then classify the low-level
 // failure instead of calling every SD.begin() failure a bus fault.
 assert(clockHz==400000u);
 assert(std::string(mountStage)=="card");
 assert(probeCardType==CARD_NONE&&!probeSector0&&!probeFilesystem);
 // CARD_UNKNOWN must never claim that sector zero actually failed: no raw
 // sector request was issued. The old 'card-read' label was misleading.
 sdQuarantined=false;probeCardUnknown=true;clocks.clear();
 assert(!begin(true)&&sdQuarantined.load());
 assert(std::string(mountStage)=="card-unknown"&&!probeSectorAttempted&&!probeSector0);
 sdQuarantined=false;probeCardUnknown=false;

 // If the card initializes and raw sector zero is readable but FAT mounting
 // still fails, the fault is the filesystem, not SPI wiring. The probe never
 // formats or writes the card.
 sdQuarantined=false;probeCardVisible=true;probeRawHealthy=true;probeFsHealthy=false;clocks.clear();
 assert(!begin(true)&&!ready);
 assert(std::string(mountStage)=="filesystem");
 assert(probeCardType==CARD_SDHC&&probeSector0&&probeBootSignature&&!probeFilesystem);

 // A card that identifies but cannot read a sector remains a transport/media
 // fault rather than being mislabeled as a filesystem problem.
 sdQuarantined=false;probeRawHealthy=false;clocks.clear();
 assert(!begin(true)&&std::string(mountStage)=="card-read");

 // Diagnostic mock may report readable filesystem without normal owner remount.
 sdQuarantined=false;probeRawHealthy=true;probeFsHealthy=true;clocks.clear();
 assert(!begin(true)&&std::string(mountStage)=="probe-mounted"&&probeFilesystem);

 sdQuarantined=false;mountHealthy=true;limitHz=10000000u;cardPresent=true;clocks.clear();
 assert(begin(false)&&clockHz==1000000u);
 assert((clocks==std::vector<uint32_t>{1000000u}));

 // Mount success is insufficient: directory and capacity must also be usable.
 rootHealthy=false;assert(!begin(true)&&!ready&&capacity==0&&sdQuarantined.load());
 sdQuarantined=false;rootHealthy=true;
 rootDirectory=false;assert(!begin(true)&&!ready&&handles==0&&sdQuarantined.load());
 sdQuarantined=false;rootDirectory=true;
 spaceHealthy=false;assert(!begin(true)&&!ready&&sdQuarantined.load());
 sdQuarantined=false;spaceHealthy=true;
 rootExists=false;mkdirHealthy=false;assert(begin(true)&&!captureDirectoryPresent);
 assert(!mkdirHealthy); // Mount never creates a FAT directory.
 rootExists=true;mkdirHealthy=true;assert(begin(true)&&captureDirectoryPresent);

 // A timed-out read/write cannot reinitialize a possibly busy controller.
 recoveryClockLocked=false;assert(begin(true));mountHealthy=false;
 const unsigned stalledResets=resets;
 assert(!recoverIO()&&recoveryClockLocked&&!ready&&sdQuarantined.load());
 clocks.clear();assert(!begin(false)&&!begin(true)&&clocks.empty());
 assert(resets==stalledResets);
 sdQuarantined=false;recoveryClockLocked=false;
 mountHealthy=true;limitHz=10000000u;clocks.clear();
 assert(begin(false)&&lastHz==1000000u);
 assert((clocks==std::vector<uint32_t>{1000000u}));
 uint8_t diagnostic[480]{};
 const size_t diagnosticSize=diagnostics(diagnostic,sizeof(diagnostic));
 assert(diagnosticSize>0);
 const std::string json(reinterpret_cast<char*>(diagnostic),diagnosticSize);
 assert(json.find("\\\"sdCsPin\\\":21")!=std::string::npos);
 assert(json.find("\\\"resetReason\\\":3")!=std::string::npos);
 puts("PASS SD SPI ownership, raw fault classification and sticky I/O recovery");
}`),/PASS SD SPI ownership, raw fault classification and sticky I\/O recovery/);
});
