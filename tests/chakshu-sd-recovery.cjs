'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
test('SD recovery resets the bus, verifies the filesystem and retains its reduced clock',()=>{
 const source=fs.readFileSync('firmware/xiao-sense/sd-storage.cpp','utf8');
 const lifecycle=source.slice(source.indexOf('namespace ChakshuStorage {'),source.indexOf('bool capturePath('))+'}\n';
 assert.match(nativeTest(`#include <atomic>
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <vector>
constexpr int CARD_NONE=0;
bool spiHealthy=true,mountHealthy=true,rootHealthy=true,rootDirectory=true,spaceHealthy=true,rootExists=true,mkdirHealthy=true;
bool busStarted=false,fsMounted=false;unsigned resets=0,handles=0;uint32_t lastHz=0,limitHz=4000000;
std::vector<uint32_t> clocks;
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
 bool begin(int sck,int miso,int mosi,int cs){assert(!busStarted);assert(sck==7&&miso==8&&mosi==9&&cs==21);busStarted=spiHealthy;return busStarted;}
} SPI;
struct Card {
 void end(){assert(handles==0);fsMounted=false;}
 bool begin(int cs,Spi&,uint32_t hz,const char*,int files,bool format){
  assert(busStarted&&cs==21&&files==5&&!format);clocks.push_back(hz);lastHz=hz;
  fsMounted=mountHealthy&&hz<=limitHz;return fsMounted;
 }
 int cardType(){return fsMounted?1:CARD_NONE;}
 bool exists(const char*){return rootExists;}
 bool mkdir(const char*){return mkdirHealthy;}
 File open(const char*){assert(fsMounted);return File(rootHealthy);}
 uint64_t totalBytes(){return spaceHealthy?8000000u:0u;}
 uint64_t usedBytes(){return 1000000u;}
} SD;
struct {template<class... T> void printf(const char*,T...){} } Serial;
struct {unsigned getFreeHeap(){return 40000;}} ESP;
unsigned esp_random(){return 123;}
${lifecycle}
int main(){
 using namespace ChakshuStorage;
 assert(begin(false)&&clockHz==4000000&&capacity==8000000&&freeBytes==7000000);
 const unsigned before=resets;assert(begin(false)&&resets==before); // healthy reads do not remount
 assert(recoverIO()&&clockHz==1000000&&resets>before&&handles==0);
 ready=false;assert(begin(false)&&lastHz==1000000); // quarantine must not raise clock
 assert(begin(true)&&lastHz==1000000); // explicit recheck must not raise clock
 clockHz=4000000;limitHz=1000000;clocks.clear();assert(begin(true));
 assert((clocks==std::vector<uint32_t>{4000000,1000000}));
 rootHealthy=false;assert(!begin(true)&&!ready&&!busStarted&&!fsMounted&&capacity==0);rootHealthy=true;
 rootDirectory=false;assert(!begin(true)&&!ready&&handles==0);rootDirectory=true;
 spaceHealthy=false;assert(!begin(true)&&!ready);spaceHealthy=true;
 rootExists=false;mkdirHealthy=false;assert(!begin(true));mkdirHealthy=true;assert(begin(true));
 mountHealthy=false;assert(!recoverIO()&&!ready&&!busStarted&&!fsMounted);mountHealthy=true;
 spiHealthy=false;const size_t calls=clocks.size();assert(!begin(false)&&clocks.size()==calls);spiHealthy=true;
 assert(begin(false)&&lastHz==1000000&&handles==0);
 puts("PASS bounded non-formatting SD recovery and sticky clock");
}`),/PASS bounded non-formatting SD recovery/);
});
