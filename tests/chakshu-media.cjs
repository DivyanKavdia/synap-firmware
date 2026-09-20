'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
test('SD operations reject stale requests and competing audio/OTA without losing accepted results',()=>{
  const source=fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/media.cpp'),'utf8');
  const start=source.indexOf('void tick() {'),end=source.indexOf('\n}\n}\nbool mediaBusy()',start)+2;
  assert(start>=0&&end>start);
  const fixture=fs.readFileSync(path.join(__dirname,'chakshu-media.cpp'),'utf8');
  assert.match(nativeTest(fixture.replace('// INSERT MEDIA TICK',source.slice(start,end))),/PASS media ownership/);
});

test('hardware status hides old connection results while preserving readiness',()=>{
  const source=fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/media.cpp'),'utf8');
  const types=source.slice(source.indexOf('struct Snapshot {'),source.indexOf('portMUX_TYPE mux='));
  const copy=source.slice(source.indexOf('void copyForConnection('),source.indexOf('void refresh('));
  const encode=source.slice(source.indexOf('void encode('),source.indexOf('uint8_t recordWav('));
  const fixture=`#include <atomic>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <cstdio>
${types}
std::atomic<uint32_t> connectionGeneration{7};Snapshot status;
void copy(Snapshot& s){s=status;}
void put32le(uint8_t* p,uint32_t n){for(int i=0;i<4;++i)p[i]=n>>(8*i);}
${copy}
${encode}
int main(){
 status.connection=7;status.id=1;status.operation=2;status.state=2;status.ready=7;status.bytes=1234;strcpy(status.path,"photo.jpg");
 uint8_t p[20];encode(p);assert(p[3]==1&&p[4]==2&&p[6]==7);
 ++connectionGeneration;encode(p);assert(p[2]==0&&p[3]==0&&p[4]==0&&p[6]==7&&p[16]==0);
 Snapshot current;copyForConnection(current);assert(!current.path[0]&&status.path[0]);
 status.connection=8;encode(p);assert(p[3]==1&&p[4]==2&&p[6]==7);
 puts("PASS connection-scoped hardware status");
}`;
  assert.match(nativeTest(fixture),/PASS connection-scoped hardware status/);
});

test('catalogue remounts a present card after transient readiness loss',()=>{
  const source=fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/media-transfer.cpp'),'utf8');
  const start=source.indexOf('uint8_t catalogue()');
  const end=source.indexOf('\n}\n\nvoid recordOffline',start)+2;
  assert(start>=0&&end>start);
  const catalogue=source.slice(start,end);
  assert.match(catalogue,/!ChakshuStorage::ready&&!ChakshuStorage::begin\(false\)/);
  assert.match(catalogue,/return ChakshuMedia::NO_SD/);
});

test('transient SD I/O recovery drops to conservative SPI speeds before failing',()=>{
  const storage=fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/sd-storage.cpp'),'utf8');
  const transfer=fs.readFileSync(path.join(__dirname,'../firmware/xiao-sense/media-transfer.cpp'),'utf8');
  const start=storage.indexOf('bool recoverIO()');
  const end=storage.indexOf('bool capturePath(',start);
  assert(start>=0&&end>start);
  const recovery=storage.slice(start,end);
  assert.match(recovery,/mountAt\(1000000u\)/);
  assert.doesNotMatch(recovery,/10000000u/);
  assert.match(storage,/SD\.begin\(21,SPI,hz,"\/sd",5,false\)/);
  const select=transfer.slice(transfer.indexOf('uint8_t selectFile('),transfer.indexOf('uint8_t readSelection('));
  const read=transfer.slice(transfer.indexOf('uint8_t readSelection('),transfer.indexOf('bool validPath('));
  const catalogue=transfer.slice(transfer.indexOf('uint8_t catalogue()'),transfer.indexOf('void recordOffline'));
  assert.match(select,/ChakshuStorage::recoverIO\(\)/);
  assert.match(read,/ChakshuStorage::recoverIO\(\)/);
  assert.match(catalogue,/ChakshuStorage::recoverIO\(\)/);
  assert.equal((read.match(/recoverIO\(\)/g)||[]).length,1,'a chunk read gets one recovery attempt');
});
