'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const {nativeTest}=require('./support/native.cjs');
const {multiAfter}=require('../tools/patch-arduino-sd.cjs');

test('C3 CMD25 waits for stop programming; returns error on partial blocks or timeout',()=>{
  const result=nativeTest(String.raw`
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <iostream>
using std::string;
using std::vector;
#define log_e(...) ((void)0)
constexpr uint8_t CARD_MMC=1;
constexpr uint8_t CARD_SDHC=3;
constexpr int SET_WR_BLK_ERASE_COUNT=23;
constexpr int WRITE_BLOCK_MULTIPLE=25;
constexpr int SEND_STATUS=13;
struct Card { uint8_t type=CARD_SDHC; };
Card card;
Card* s_cards[1]={&card};
vector<string> events;
int waitCount=0, stopCount=0, acceptedBlocks=0, rejectedBlock=0, failWaitAt=0;
int statusCount=0;
bool sdSelectCard(uint8_t){ events.push_back("select");return true; }
void sdDeselectCard(uint8_t){events.push_back("deselect");}
char sdTransaction(uint8_t,char cmd,unsigned int,unsigned int* resp){
  if(cmd==SEND_STATUS){++statusCount;events.push_back("status");if(resp)*resp=0;}
  else events.push_back("preerase");
  return 0;
}
char sdCommand(uint8_t,char cmd,unsigned int,unsigned int*){
  assert(cmd==WRITE_BLOCK_MULTIPLE);events.push_back("cmd25");return 0;
}
bool sdWait(uint8_t,int timeout){
  assert(timeout==5000);
  ++waitCount;
  events.push_back("ready");
  return waitCount!=failWaitAt;
}
char sdWriteBytes(uint8_t,const char*,char token){
  assert(token==0xFC);
  ++acceptedBlocks;
  events.push_back("data");
  return acceptedBlocks==rejectedBlock?0x0B:0x05;
}
void sdStop(uint8_t){++stopCount;events.push_back("stopFD");}
${multiAfter}

static void reset(){
  events.clear();waitCount=0;stopCount=0;acceptedBlocks=0;
  rejectedBlock=0;failWaitAt=0;statusCount=0;
}
static int pos(const string& target){
  for(size_t i=0;i<events.size();++i) if(events[i]==target)return (int)i;
  return -1;
}
int main(){
  char buffer[4096]{};
  reset();
  assert(sdWriteSectors(0,buffer,44,8));
  assert(stopCount==1 && acceptedBlocks==8 && statusCount==1);
  assert(waitCount==10);
  assert(pos("stopFD")>=0 && pos("deselect")>pos("stopFD"));
  assert(events.at(events.size()-2)=="status" || events.back()=="status");
  // Card stays selected until it reports ready after the write stop.
  reset();failWaitAt=10;
  assert(!sdWriteSectors(0,buffer,44,8));
  assert(stopCount==1 && statusCount==0 && waitCount==10);
  assert(events.at(events.size()-2)=="ready" && events.back()=="deselect");
  // Reject a partial data stream WITHOUT a CMD12 read-stop or replay.
  reset();rejectedBlock=3;
  assert(!sdWriteSectors(0,buffer,44,8));
  assert(stopCount==1 && statusCount==0 && acceptedBlocks==3);
  assert(waitCount==5);
  // Fail-safe when a card never goes ready, without sending a premature stop.
  reset();failWaitAt=3;
  assert(!sdWriteSectors(0,buffer,44,8));
  assert(stopCount==0 && statusCount==0 && acceptedBlocks==2);
  std::cout<<"PASS C3 CMD25 stop and failure behaviors\n";
}
`);
  assert.match(result,/PASS C3 CMD25 stop and failure behaviors/);
});
