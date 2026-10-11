'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const sd=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const rec=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const tx=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const ino=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
test('C3 trace calls unmodified SDSPI transaction once, without additional commands or retries',()=>{
 const trace=sd.split('static esp_err_t odysseyTracedSdspiTransaction(')[1]
   .split('// Fault layout 0x20SS00EE')[0];
 assert.match(trace,/sdspi_host_do_transaction\(slot,cmd\)/);
 assert.equal((trace.match(/sdspi_host_do_transaction\(/g)||[]).length,1);
 assert.doesNotMatch(trace,/vTaskDelay|spi_device_transmit|sdmmc_send_cmd\(|odysseySdBitBang|odysseyRecoverSdCard/);
 assert.match(sd,/host\.do_transaction=&odysseyTracedSdspiTransaction/);
 assert.match(sd,/mount\.format_if_mount_failed=false/);
 assert.match(sd,/mount\.disk_status_check_enable=false/);
});
test('The first wire fault survives only when NVS stage and bytes agree',()=>{
 assert.match(rec,/odysseySdClearWireFault\(\)/);
 assert.match(rec,/prefs\.putBytes\("wire",wire,sizeof\(wire\)\)/);
 assert.match(rec,/wire\[0\]==record\[1\] && wire\[1\]==record\[2\]/);
 assert.match(rec,/prefs\.putBytes\("last",record,sizeof\(record\)\)/);
 assert.match(rec,/uint32_t odysseyLastWireCommand\(\)/);
});
test('BLE op27 reports raw command and native ESP error, not synthetic wrD',()=>{
 const op=tx.split('case 27: {')[1].split('case 4:')[0];
 for (const field of ['wireCmd','wireErr','wireArg','wireRsp','wireR2','faultKind'])assert(op.includes(field),field);
 assert.match(op,/char detail\[480\]/);
 assert.doesNotMatch(op,/unlink\(|fwrite\(|odysseyRecoverSdCard\(|mkdir\(/);
 assert.match(ino,/host\.do_transaction=&odysseyTracedSdspiTransaction/);
});
