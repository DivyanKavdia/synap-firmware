#include <cassert>
#include <cstdint>
#include <cstddef>
constexpr int CARD_SDHC=1,WRITE_BLOCK_SINGLE=24,SEND_STATUS=13;
using DWORD=unsigned long;using UINT=unsigned int;using DRESULT=int;
constexpr int STA_NOINIT=1,STA_PROTECT=2,RES_OK=0,RES_ERROR=1,RES_NOTRDY=2,RES_WRPRT=3;
struct Card {int type=CARD_SDHC;int status=0;} card;
using ardu_sdcard_t=Card;
Card* s_cards[]={&card};
struct AcquireSPI{explicit AcquireSPI(Card*){}};
int tokenValue=5,writes=0,statusRequests=0,multiWrites=0,busyWaits=0,deselects=0;
bool waitOK=true;
bool sdSelectCard(int){return true;}
bool sdWait(int,int){++busyWaits;return waitOK;}
void sdDeselectCard(int){++deselects;}
char sdCommand(int,int,unsigned long long,void*){return 0;}
char sdWriteBytes(int,const char*,int){++writes;return tokenValue;}
char sdTransaction(int,int,unsigned,unsigned* resp){++statusRequests;*resp=0;return 0;}
bool sdWriteSectors(uint8_t,const char*,unsigned long long,int){++multiWrites;return true;}
// INSERT WRITE
int main(){
 char sector[512]{};
 for(tokenValue=0;tokenValue<=31;++tokenValue){
   writes=statusRequests=busyWaits=deselects=0;waitOK=true;
   assert(sdWriteSector(0,sector,1)==(tokenValue==5));
   assert(statusRequests==(tokenValue==5?1:0));
   assert(busyWaits==(tokenValue==5?1:0));
   assert(deselects>=1);
   assert(writes==1);
 }
 tokenValue=5;writes=statusRequests=busyWaits=deselects=0;waitOK=false;
 assert(!sdWriteSector(0,sector,1));
 assert(writes==1&&busyWaits==1&&statusRequests==0&&deselects==1);
 waitOK=true;writes=statusRequests=multiWrites=busyWaits=deselects=0;
 uint8_t four[2048]{};
 assert(ff_sd_write(0,four,7,4)==RES_OK);
 assert(multiWrites==0);
 assert(writes==4);
 assert(busyWaits==4);
}
