'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {patch}=require('../tools/patch-arduino-sd.cjs');

test('unsupported SD library cannot be silently patched',()=>{
  assert.throws(()=>patch('other release'),/Pinned SD patch no longer matches/);
});

test('pinned Arduino 3.3.5 SD init carries Espressif SPI-mode fixes',
  {skip:!process.env.SYNAP_ARDUINO_SD_SRC},()=>{
    const file=path.join(process.env.SYNAP_ARDUINO_SD_SRC,'sd_diskio.cpp');
    const library=fs.readFileSync(file,'utf8');
    const fixed=patch(library);
    assert.equal(patch(fixed),fixed,'SD backport must be idempotent');
    assert.match(fixed,/sd_go_idle_delay_ms = 20/);
    assert.match(fixed,/sd_op_cond_timeout_ms = 3000/);
    const init=fixed.slice(fixed.indexOf('DSTATUS ff_sd_initialize'),fixed.indexOf('DSTATUS ff_sd_status'));
    assert((init.match(/sdCommand\(pdrv, GO_IDLE_STATE, 0, NULL\)/g)||[]).length>=2,
      'SPI initialization must issue CMD0 twice');
    assert.match(init,/delay\(sd_go_idle_delay_ms\)/);
    assert.match(init,/token != 1 && token != 0x5[\s\S]*?delay\(10\)[\s\S]*?CRC_ON_OFF/);
    assert.match(init,/APP_OP_COND, 0x40000000, NULL/);
    assert.match(init,/APP_OP_COND, 0, NULL/);
    assert.match(init,/SEND_OP_COND, 0, NULL/);
    assert.doesNotMatch(init,/APP_OP_COND, 0x40100000/);
    assert.doesNotMatch(init,/APP_OP_COND, 0x100000/);
    assert.doesNotMatch(init,/SEND_OP_COND, 0x100000/);
    assert.doesNotMatch(init,/\(millis\(\) - start\) < 1000/);
  });
