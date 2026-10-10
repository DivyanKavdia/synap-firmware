'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const {patchOld:patch,patch:patchC3,before,after,multiBefore,multiAfter,faultHeader,byteBefore,byteTimeoutOnly,stopBefore,byteAfter,stopAfter}=require('../tools/patch-arduino-sd.cjs');

test('C3 write driver consumes delayed busy after data and STOP before releasing CS',()=>{
  const driver=patch([stopBefore,byteBefore,before,multiBefore].join('\n'));
  const fixture=fs.readFileSync('tests/arduino-sd-busy-latency.cpp','utf8');
  nativeTest(fixture.replace('// INSERT DRIVER',driver));
});

test('metadata CMD25 upgrade accepts the complete build-1957 patched driver',()=>{
  const old=[stopAfter,byteAfter,faultHeader+after,multiAfter].join('\n');
  const fresh=patch([stopBefore,byteBefore,before,multiBefore].join('\n'));
  assert.equal(patch(old),fresh);
  assert.match(fresh,/return sdWriteSectors\(pdrv, buffer, sector, 1\)/);
  assert.match(fresh,/if \(count <= 0\)/);
  assert.match(fresh,/SYNAP_SD_STABLE_BUSY_POLL/);
  assert.match(fresh,/synapSdWaitStable\(pdrv, 5000\)/);
});

test('busy latency patch upgrades previous 5-second patch and rejects source drift',()=>{
  const original=[stopBefore,byteBefore,before,multiBefore].join('\n');
  const once=patch(original);
  assert.equal(patch(once),once);
  const previous=once.replace(byteAfter,byteTimeoutOnly).replace(stopAfter,stopBefore);
  assert.equal(patch(previous),once);
  assert.throws(()=>patch(original.replace('write(0xFD)','write(0xFE)')),/stop token.*3\.3\.5/);
  assert.throws(()=>patch(original.replace('write16(crc)','write16(0)')),/data block.*3\.3\.5/);
});

test('C3 production CMD24 writer completes 4 KiB as eight verified sectors, without CMD25 STOP',()=>{
  const driver=patchC3([stopBefore,byteBefore,before,multiBefore].join('\n'));
  assert.match(driver,/SYNAP_SD_C3_CMD24_SECTORS/);
  assert.match(driver,/SYNAP_SD_C3_STABLE_READY/);
  const base=fs.readFileSync('tests/arduino-sd-busy-latency.cpp','utf8');
  const fixture=base.slice(0,base.indexOf('int main() {'))+`
int main() {
  char data[4096]{};
  for (unsigned idle : {0u,1u,2u}) {
    reset(idle);
    assert(sdWriteSector(0,data,17));
    assert(blocks==1 && stops==0 && statusChecks==1 && !selected);
    assert(writeCommand==24 && commandCount==1 && writeAddress==17);
    assert(!synapSdWriteFaultCode());
    reset(idle);
    assert(sdWriteSectors(0,data,17,8));
    assert(blocks==8 && stops==0 && statusChecks==8);
    assert(commandCount==8 && writeAddress==24 && !selected);
    assert(spi.payload.size()==4096 && !synapSdWriteFaultCode());
  }
  reset();busyForever=true;
  assert(!sdWriteSectors(0,data,17,8));
  assert(synapSdWriteFaultCode()==0x18040000u);
  assert(blocks==1 && statusChecks==0 && ticks<5100);
  reset();stopForever=true;
  assert(sdWriteSectors(0,data,17,8));
  assert(stops==0 && blocks==8);
  reset();reject=true;
  assert(!sdWriteSectors(0,data,17,8));
  assert(synapSdWriteFaultCode()==0x1803000du);
  assert(blocks==1 && stops==0 && statusChecks==0);
  reset();failStatus=true;
  assert(!sdWriteSectors(0,data,17,8));
  assert(synapSdWriteFaultCode()==0x18060001u);
  assert(blocks==1 && statusChecks==1);
  reset();failCommand=true;
  assert(!sdWriteSectors(0,data,17,8));
  assert(synapSdWriteFaultCode()==0x18020000u);
  assert(blocks==0 && commandCount==1);
  reset();
  assert(!sdWriteSectors(0,data,17,0));
  assert(blocks==0 && commandCount==0);
  for (int type : {2,int(CARD_SDHC)}) {
    reset();card.type=type;
    for (unsigned i=0;i<sizeof(data);++i) data[i]=char(i*37u);
    assert(sdWriteSector(0,data+512,123));
    assert(spi.payload.size()==512 && !memcmp(spi.payload.data(),data+512,512));
    assert(writeAddress==(type==CARD_SDHC?123u:123u*512u));
    assert(writeCommand==24 && commandCount==1 && stops==0);
  }
}`;
  nativeTest(fixture.replace('// INSERT DRIVER',driver));
});
