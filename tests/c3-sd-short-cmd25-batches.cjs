'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const src=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const full=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
test('C3 CMD25 compatibility trial writes four contiguous sectors, never CMD24',()=>{
 for(const s of [src,full]){
  assert.match(s,/ODYSSEY_SD_RECORD_BATCH_BYTES=2048u/);
  assert.match(s,/ODYSSEY_SD_RECORD_BATCH_BYTES%512u==0u/);
  assert.match(s,/alignas\(4\) static uint8_t batch\[ODYSSEY_SD_RECORD_BATCH_BYTES\]/);
  assert.match(s,/if \(batchUsed==sizeof\(batch\)\)/);
  assert.match(s,/fwrite\(batch,1,sizeof\(batch\),file\)/);
  assert.match(s,/memset\(batch\+batchUsed,0,sizeof\(batch\)-batchUsed\)/);
  assert.doesNotMatch(s.split('static void odysseyRecordTake() {')[1].split('static void odysseyRecordTask(void*)')[0], /sdmmc_write_sectors|sdspi_host_do_transaction|fwrite\(batch,1,512|SD\.format\(/);
 }
 const helper=src.split('// Experimental C3 compatibility profile:')[1].split('static constexpr uint32_t ODYSSEY_SD_FIRST_CHECKPOINT')[0];
 const compile=nativeTest('#include <cstdint>\n#include <cstddef>\n#include <iostream>\n'+
 ' // Experimental C3 compatibility profile:'+helper+
 '\nint main(){ static_assert(ODYSSEY_SD_RECORD_BATCH_BYTES==4*512u); std::cout << "PASS C3 four-sector batches"; }');
 assert.match(compile,/PASS C3 four-sector batches/);
});
test('C3 first metadata checkpoint and quarantine remain independent of batch size',()=>{
 assert.match(src,/ODYSSEY_SD_FIRST_CHECKPOINT_PCM_BYTES=262144u/);
 assert.match(src,/ODYSSEY_SD_CHECKPOINT_INTERVAL_MS=60000u/);
 assert.match(src,/if \(file && \(odysseySdWriteTimeoutQuarantined\(\) \|\| uncertainSdWrite\)\)/);
 assert.match(src,/if \(odysseySdWriteTimeoutQuarantined\(\) \|\| uncertainSdWrite\) \{/);
 assert.match(src,/odysseySdQuarantineUncertainWrite\(\)/);
 assert.match(src,/file=nullptr/);
});
