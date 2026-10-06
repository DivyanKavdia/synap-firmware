'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {patch}=require('../tools/patch-arduino-sd.cjs');

test('newer SD-SPI initializer backport fails closed on an unknown source',()=>{
  assert.throws(()=>patch('unexpected SDK'),/Pinned SD patch no longer matches/);
});

test('pinned 3.3.5 source receives double CMD0 and corrected operation-condition arguments',
  {skip:!process.env.SYNAP_ARDUINO_SD_SRC},()=>{
    const original=fs.readFileSync(path.join(process.env.SYNAP_ARDUINO_SD_SRC,'sd_diskio.cpp'),'utf8');
    const fixed=patch(original);
    assert.equal(patch(fixed),fixed);
    assert.match(fixed,/sd_go_idle_delay_ms = 20/);
    assert.match(fixed,/sd_op_cond_timeout_ms = 3000/);
    assert.match(fixed,/APP_OP_COND, 0x40000000/);
    assert.match(fixed,/APP_OP_COND, 0, NULL/);
    assert.match(fixed,/SEND_OP_COND, 0, NULL/);
    const first=fixed.indexOf('(void)sdCommand(pdrv, GO_IDLE_STATE, 0, NULL)');
    const second=fixed.indexOf('if (sdCommand(pdrv, GO_IDLE_STATE, 0, NULL) != 1)',first);
    assert.ok(first>0&&second>first);
  });
