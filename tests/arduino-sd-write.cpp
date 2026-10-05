#include <cassert>
#include <cstdint>
#include <cstddef>
constexpr int CARD_SDHC=1,WRITE_BLOCK_SINGLE=24,SEND_STATUS=13;
struct Card {int type=CARD_SDHC;} card;
Card* s_cards[]={&card};
int tokenValue=5,writes=0,statusRequests=0;
bool crcOnce=false;
bool sdSelectCard(int){return true;}
void sdDeselectCard(int){}
char sdCommand(int,int,unsigned long long,void*){return 0;}
char sdWriteBytes(int,const char*,int){++writes;return crcOnce&&writes==1?10:tokenValue;}
char sdTransaction(int,int,unsigned,unsigned* resp){++statusRequests;*resp=0;return 0;}
// INSERT WRITE
int main(){
 char sector[512]{};
 for(tokenValue=0;tokenValue<=31;++tokenValue){
   writes=statusRequests=0;
   assert(sdWriteSector(0,sector,1)==(tokenValue==5));
   assert(statusRequests==(tokenValue==5?1:0));
   assert(writes==(tokenValue==10?3:1));
 }
 tokenValue=5;writes=statusRequests=0;crcOnce=true;
 assert(sdWriteSector(0,sector,1)&&writes==2&&statusRequests==1);
}
