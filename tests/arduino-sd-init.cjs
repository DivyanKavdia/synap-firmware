'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
const {nativeTest}=require('./support/native.cjs');
const {patch}=require('../tools/patch-arduino-sd.cjs');
const fixture=fs.readFileSync('tests/fixtures/arduino-sd-init-3.3.5.cpp','utf8');
// Include only boundaries needed by the patch; the real installed SDK is also
// checked in CI. Native execution uses the actual old/new initializer bodies.
const writeFixture=fs.readFileSync('tests/fixtures/arduino-sd-write-3.3.5.cpp','utf8');
const skeleton=`  CRC_ON_OFF = 59
} ardu_sdcard_command_t;

typedef struct {
  return token;
}

bool sdReadBytes
${writeFixture}
${fixture}
DSTATUS ff_sd_status(
  esp_err_t err = esp_vfs_fat_register(path, drv, max_files, &fs);
  FRESULT res = f_mount(fs, drv, 1);
`;
const fixed=patch(skeleton);
const init=fixed.slice(fixed.indexOf('DSTATUS ff_sd_initialize'),fixed.indexOf('#else\nDSTATUS ff_sd_initialize'));
const harness=fs.readFileSync('tests/arduino-sd-init.cpp','utf8');
test('patched real C3 initializer handles second CMD0, CRC retry, strict ACMD41 and slow cards',()=>{
  nativeTest(harness.replace('// INSERT INIT',init).replace('// INSERT EXPECTATION','constexpr bool patched=true;'),['-funsigned-char']);
});
test('unpatched initializer reproduces the command compatibility failures',()=>{
  nativeTest(harness.replace('// INSERT INIT',fixture).replace('// INSERT EXPECTATION','constexpr bool patched=false;'),['-funsigned-char']);
});
test('driver patch is idempotent, fails on source drift and leaves S3 init unchanged',()=>{
  assert.equal(patch(fixed),fixed);
  assert.throws(()=>patch('unsupported driver'),/boundary missing/);
  assert.throws(()=>patch(skeleton.replace('0x40100000','0x40100001')),/SDHC ACMD41/);
  const original=fixture.slice(fixture.indexOf('DSTATUS ff_sd_initialize'));
  assert.equal(fixed.split('#else\nDSTATUS ff_sd_initialize')[1].split('#endif')[0].trim(),original.slice('DSTATUS ff_sd_initialize'.length).trim());
  assert.match(fixed,/synap_sd_fat=res/);
  assert.match(fixed,/synap_sd_vfs=err/);
});
test('installed SDK must contain the C3 driver patch before compilation',{
 skip:!process.env.SYNAP_ARDUINO_SD_SRC
},()=>{
  const actual=fs.readFileSync(path.join(process.env.SYNAP_ARDUINO_SD_SRC,'sd_diskio.cpp'),'utf8');
  assert.match(actual,/SYNAP_C3_SD_INIT_V2/);
  assert.equal(patch(actual),actual);
  assert(actual.includes(init.trim()));
});

test('C3 single-sector writes reject every unaccepted response, even when CMD13 succeeds',()=>{
  const start=fixed.indexOf('bool sdWriteSector(');
  const write=fixed.slice(start,fixed.indexOf('\n}',start)+2);
  nativeTest(fs.readFileSync('tests/arduino-sd-write.cpp','utf8').replace('// INSERT WRITE',write),
    ['-DCONFIG_IDF_TARGET_ESP32C3=1','-funsigned-char']);
});
