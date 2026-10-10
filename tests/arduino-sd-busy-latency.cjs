'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const {patch,before,multiBefore,byteBefore,byteTimeoutOnly,stopBefore,byteAfter,stopAfter}=require('../tools/patch-arduino-sd.cjs');

test('C3 write driver consumes delayed busy after data and STOP before releasing CS',()=>{
  const driver=patch([stopBefore,byteBefore,before,multiBefore].join('\n'));
  const fixture=fs.readFileSync('tests/arduino-sd-busy-latency.cpp','utf8');
  nativeTest(fixture.replace('// INSERT DRIVER',driver));
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
