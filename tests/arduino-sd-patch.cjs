'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const {patch,before}=require('../tools/patch-arduino-sd.cjs');

test('pinned Arduino 3.3.5 CMD24 patch waits for programming before deselect',()=>{
  const source='prefix\n'+before+'\nsuffix';
  const out=patch(source);
  assert.match(out,/SYNAP_SD_CMD24_BUSY_FIX/);
  assert.match(out,/token != 0x05/);
  assert.match(out,/token == 0x0B/);
  assert.match(out,/sdWait\(pdrv, 5000\)/);
  assert.match(out,/if \(token != 0x05\) \{[\s\S]*?return false;\n      \}\n      if \(!sdWait\(pdrv, 5000\)\) \{\n        sdDeselectCard\(pdrv\);\n        return false;\n      \}\n      sdDeselectCard\(pdrv\);/,
    'accepted CMD24 path must wait for post-program busy before deselect');
  assert.doesNotMatch(out,/token == 0x0A|token == 0x0C/);
});

test('CMD24 patch is idempotent and rejects unknown core source',()=>{
  const once=patch(before);
  assert.equal(patch(once),once);
  assert.throws(()=>patch('bool sdWriteSector(){}'),/3\.3\.5/);
});
