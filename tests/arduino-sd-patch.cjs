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
  const wait=out.indexOf('sdWait(pdrv, 5000)');
  const deselect=out.indexOf('sdDeselectCard(pdrv);',out.indexOf('SYNAP_SD_CMD24_BUSY_FIX'));
  assert(wait>=0 && deselect>wait,'CMD24 must remain selected until post-program busy clears');
  assert.doesNotMatch(out,/token == 0x0A|token == 0x0C/);
});

test('CMD24 patch is idempotent and rejects unknown core source',()=>{
  const once=patch(before);
  assert.equal(patch(once),once);
  assert.throws(()=>patch('bool sdWriteSector(){}'),/3\.3\.5/);
});
