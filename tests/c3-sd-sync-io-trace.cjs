'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const code=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const sketch=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
const sync=code.split('case 29: {')[1].split('case 4:')[0];
const reader=code.split('static uint8_t readWavChunk(')[1].split('static uint8_t readSelected(')[0];
test('C3 sync counters observe existing reads and never change FAT or SPI transaction logic',()=>{
  assert.match(reader,/\+\+sdBleReadRequests/);
  assert.match(reader,/sdBleReadLastRequestedOffset=offset/);
  assert.match(reader,/sdBleReadLastOkOffset=offset\+uint32_t\(size\)/);
  assert.match(code,/sdBleReadFailureOffset=sdBleReadLastRequestedOffset/);
  assert.match(code,/\+\+sdBleReadMtuFallbacks/);
  assert.match(code,/\+\+sdBleReadWindowRequests/);
  assert.doesNotMatch(sync,/fopen|fread|fwrite|unlink|esp_vfs_fat|sdmmc_read|sdspi_host|odysseyRecoverSdCard|request\(14/);
  assert.equal((code.match(/case 29:/g)||[]).length,1);
  assert(sketch.includes(code),'production sketch must be exact generated source');
});
test('RAM-only sync diagnostic includes current error operation and bounded offsets',()=>{
  for(const field of ['ioOp','ioStep','ioErr','reads','cacheHits','diskFills',
    'lastRequestedOffset','lastOkOffset','faultOffset','windowRequests','mtuFallbacks'])
    assert(sync.includes('\\\"'+field+'\\\"'),field);
  const fmt=sync.match(/snprintf\(detail,sizeof\(detail\),\s*("(?:\\.|[^"])*")/);
  assert(fmt,'diagnostic format present');
  const worst=JSON.parse(fmt[1]).replace(/%lu|%u/g,'4294967295').replace(/%ld/g,'-2147483648');
  assert(worst.length<400,'maximum-width response fits in diagnostic buffer');
  assert.match(sync,/sdLastMediaFaultOperation\.load\(\)/);
  assert.match(sync,/sdLastMediaFaultStep\.load\(\)/);
  assert.match(sync,/sdLastMediaFaultErrno\.load\(\)/);
});
