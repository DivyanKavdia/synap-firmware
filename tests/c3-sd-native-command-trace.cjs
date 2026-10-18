'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const sd=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const rec=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const tx=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const ino=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
test('C3 retries only CRC enable CMD59 once after an illegal command',()=>{
 const trace=sd.split('host.do_transaction=[](int slot,sdmmc_command_t* cmd)->esp_err_t {')[1]
   .split('sdspi_device_config_t device=')[0];
 assert.match(trace,/sdspi_host_do_transaction\(slot,cmd\)/);
 assert.equal((trace.match(/sdspi_host_do_transaction\(/g)||[]).length,2);
 assert.match(trace,/cmd->opcode==59u && cmd->arg==1u &&\s*result==ESP_ERR_NOT_SUPPORTED/);
 assert.match(trace,/vTaskDelay\(pdMS_TO_TICKS\(10u\) \? pdMS_TO_TICKS\(10u\) : 1u\)/);
 assert.equal((trace.match(/vTaskDelay\(/g)||[]).length,1);
 assert.doesNotMatch(trace,/spi_device_transmit|sdmmc_send_cmd\(|odysseySdBitBang|odysseyRecoverSdCard/);
 assert.match(sd,/host\.do_transaction=\[\]\(int slot,sdmmc_command_t\* cmd\)->esp_err_t/);
 assert.match(sd,/cmd->opcode==52u \|\| cmd->opcode==5u/);
 assert.match(sd,/error==ESP_ERR_NOT_SUPPORTED/);
 assert.match(sd,/if \(cmd && error!=ESP_OK && !expectedIoProbe\)/);
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
 assert.match(ino,/host\.do_transaction=\[\]\(int slot,sdmmc_command_t\* cmd\)->esp_err_t/);
});
