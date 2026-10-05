#include <cassert>
#include <cstdint>
#include <cstdio>
using DSTATUS=int;
constexpr int STA_NOINIT=1,CARD_NONE=0,CARD_SD=1,CARD_SDHC=2,CARD_MMC=3,CARD_UNKNOWN=4;
constexpr int HIGH=1,LOW=0;
constexpr int GO_IDLE_STATE=0,SEND_OP_COND=1,SEND_IF_COND=8,SET_BLOCKLEN=16;
constexpr int APP_OP_COND=41,APP_CLR_CARD_DETECT=42,READ_OCR=58,CRC_ON_OFF=59;
constexpr uint32_t sd_go_idle_delay_ms=20,sd_op_cond_timeout_ms=3000;
#define log_w(...) ((void)0)
uint32_t clockMs=0;
int scenario=0,cmd0s=0,crcs=0,acmds=0,clocks=0;
int synap_sd_stage=0;
bool synap_sd_tracking=false;
uint32_t millis(){return clockMs;}
void delay(uint32_t n){clockMs+=n;}
void digitalWrite(int,int){}
struct Spi { void transfer(int){++clocks;} } spi;
struct Card { int status=STA_NOINIT,ssPin=0,frequency=400000,type=CARD_NONE; bool supports_crc=true; unsigned long sectors=0; Spi* spi=nullptr;} card;
using ardu_sdcard_t=Card;
Card* s_cards[]={&card};
struct AcquireSPI {AcquireSPI(Card*,int){}};
bool sdWait(int,int){return true;}
void sdDeselectCard(int){}
char sdCommand(int,int cmd,unsigned,unsigned*){
 assert(cmd==GO_IDLE_STATE);++cmd0s;
 if(scenario==5)return char(255); // absent/unresponsive card
 if(scenario==1 && cmd0s==1)return char(255);
 return 1;
}
char sdTransaction(int,int cmd,unsigned arg,unsigned* resp){
 clockMs+=100;
 switch(cmd){
 case CRC_ON_OFF: ++crcs;return scenario==2 && crcs==1?3:1;
 case SEND_IF_COND: *resp=0x1aa;return 1;
 case READ_OCR: *resp=(1u<<20)|(1u<<30);return acmds?0:1;
 case APP_OP_COND:
   ++acmds;
   if(scenario==3 && arg!=0x40000000u)return 4;
   if(scenario==4 && acmds<16)return 1;
   return 0;
 case APP_CLR_CARD_DETECT: return 0;
 default: assert(false);return char(255);
 }
}
unsigned long sdGetSectorsCount(int){return 16000000;}
// INSERT INIT
// INSERT EXPECTATION
int main(){
 for(scenario=0;scenario<=5;++scenario){
   card=Card{};card.spi=&spi;clockMs=cmd0s=crcs=acmds=clocks=0;
   synap_sd_stage=0;synap_sd_tracking=false;
   const bool ready=ff_sd_initialize(0)==0;
   assert(ready==(scenario==0 || (patched && scenario<5)));
   assert(clocks>=20);
   if(patched){
     assert(!synap_sd_tracking);
     if(scenario==5)assert(synap_sd_stage==2);
     if(ready)assert(card.type==CARD_SDHC && card.sectors==16000000);
   }
 }
 puts("SD initialization scenarios passed");
}
