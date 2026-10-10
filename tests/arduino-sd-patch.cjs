'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const {patch,before,after,multiBefore,multiAfter,faultHeader,byteBefore,byteAfter}=require('../tools/patch-arduino-sd.cjs');

test('pinned Arduino 3.3.5 CMD24 patch waits for programming before deselect',()=>{
  const source='prefix\n'+byteBefore+'\n'+before+'\n'+multiBefore+'\nsuffix';
  const out=patch(source);
  assert.match(out,/SYNAP_SD_CMD24_BUSY_FIX/);
  assert.match(out,/token != 0x05/);
  assert.match(out,/token == 0x0B/);
  assert.match(out,/sdWait\(pdrv, 5000\)/);
  assert.match(out,/if \(token != 0x05\) \{[\s\S]*?return false;\n      \}\n      if \(!sdWait\(pdrv, 5000\)\) \{\n        sdDeselectCard\(pdrv\);\n        return false;\n      \}\n      sdDeselectCard\(pdrv\);/,
    'accepted CMD24 path must wait for post-program busy before deselect');
  assert.doesNotMatch(out,/token == 0x0A|token == 0x0C/);
});

test('both pinned C3 write patches are idempotent and fail closed on core drift',()=>{
  const original=byteBefore+'\n'+before+'\n'+multiBefore;
  const once=patch(original);
  assert.equal(patch(once),once);
  assert(once.includes(byteAfter),'both write paths use 5 second bounded waits');
  assert(once.includes(faultHeader),'exports first driver fault to recorder');
  assert(patch(after+'\n'+multiBefore).includes(faultHeader));
  assert(patch(before+'\n'+multiAfter).includes(faultHeader));
  assert.throws(()=>patch('bool sdWriteSector(){}'),/3\.3\.5/);
  assert.throws(()=>patch(before+'\n'+multiBefore.replace('sdStop(pdrv);','sdStopChanged(pdrv);')),
    /CMD25.*3\.3\.5/);
});

test('C3 CMD25 completes programming before raising CS, does not send CMD12 on write error',()=>{
  const out=patch(before+'\n'+multiBefore);
  const multi=out.split('bool sdWriteSectors(uint8_t pdrv')[1].split('\nunsigned long sdGetSectorsCount')[0];
  assert.match(multi,/SYNAP_SD_CMD25_BUSY_FIX/);
  assert.doesNotMatch(multi,/STOP_TRANSMISSION|sdCommand\(pdrv,\s*12/);
  assert.match(multi,/sdWriteBytes\(pdrv, currentBuffer, 0xFC\)/);
  assert.match(multi,/if \(!sdWait\(pdrv, 5000\)\) \{[\s\S]*?\}\s+sdStop\(pdrv\);/);
  assert.match(multi,/sdStop\(pdrv\);\s+if \(!sdWait\(pdrv, 5000\)\) \{[\s\S]*?return false;\s+\}\s+sdDeselectCard\(pdrv\);/);
  assert(multi.indexOf('sdDeselectCard(pdrv);\n\n  if (!accepted') <
    multi.indexOf('sdTransaction(pdrv, SEND_STATUS'), 'status follows completed STOP');
  assert.match(multi,/if \(!accepted \|\| currentCount != 0\) \{[\s\S]*?return false;/);
  assert.doesNotMatch(multi,/for \(int f = 0; f < 3/,'never replay partially accepted blocks');
});

test('driver patch preserves first sector/token/phase diagnostic without changing SD read driver',()=>{
  const src=patch(byteBefore+'\n'+before+'\n'+multiBefore);
  for(const fragment of ['synapSdWriteFaultCode','synapSdClearWriteFaultCode','synapRecordSdWriteFault(25,4','synapRecordSdWriteFault(25,5','synapRecordSdWriteFault(25,7','synapRecordSdWriteFault(24,5']) {
    assert(src.includes(fragment),fragment);
  }
  assert(!src.includes('STOP_TRANSMISSION, 0, NULL'));
  assert.match(src,/char sdWriteBytes[\s\S]*?sdWait\(pdrv, 5000\)/);
});
