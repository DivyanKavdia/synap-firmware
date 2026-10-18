'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const {nativeTest}=require('./support/native.cjs');
const detect=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const rec=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const tx=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const ino=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
test('C3 SD clock is an intentionally bounded hardware-qualified 2 MHz data phase',()=>{
 for(const code of [detect,ino]){
  assert.match(code,/ODYSSEY_SD_DATA_FREQ_HZ=2000000u/);
  assert.match(code,/host\.max_freq_khz=ODYSSEY_SD_DATA_FREQ_HZ\/1000u/);
  assert.match(code,/bus\.max_transfer_sz=4096\+512/);
  assert.match(code,/SDSPI_DEFAULT_DMA/);
  assert.doesNotMatch(code,/ODYSSEY_SD_DATA_FREQ_HZ=800000u/);
 }
 assert.match(detect,/host\.do_transaction=\[\]\(int slot,sdmmc_command_t\* cmd\)->esp_err_t/);
 const capture=detect.split('host.do_transaction=[](int slot,sdmmc_command_t* cmd)->esp_err_t {')[1].split('sdspi_device_config_t device=')[0];
 assert.equal((capture.match(/sdspi_host_do_transaction\(slot,cmd\)/g)||[]).length,2);
 assert.match(capture,/cmd->opcode==59u && cmd->arg==1u &&\s*result==ESP_ERR_NOT_SUPPORTED/);
 assert.match(capture,/vTaskDelay\(pdMS_TO_TICKS\(10u\) \? pdMS_TO_TICKS\(10u\) : 1u\)/);
 assert.equal((capture.match(/vTaskDelay\(/g)||[]).length,1);
 assert.doesNotMatch(capture,/delay\(|cmd->timeout_ms\s*=/);
 const compile=nativeTest('#include <cstdint>\n'+
 'static constexpr uint32_t ODYSSEY_SD_DATA_FREQ_HZ=2000000u;\n'+
 'static_assert(ODYSSEY_SD_DATA_FREQ_HZ/1000u==2000u);\n'+
 'int main(){return 0;}');
 assert.equal(typeof compile,'string');
});
test('C3 failure NVS records actual capture clock so OTA does not relabel old timeouts',()=>{
 for(const code of [rec,ino]){
  assert.match(code,/odysseyPersistedWireClockKhz\{0\}/);
  assert.match(code,/odysseyPersistedWireClockKhz=stage\?ODYSSEY_SD_DATA_FREQ_HZ\/1000u:0u/);
  assert.match(code,/prefs\.getBytesLength\("wclk"\)==sizeof\(clock\)/);
  assert.match(code,/clock\[0\]==record\[1\] && clock\[1\]==record\[2\]/);
  assert.match(code,/prefs\.putBytes\("wclk",clock,sizeof\(clock\)\)/);
  assert.match(code,/uint32_t odysseyLastWireClockKhz\(\)/);
  assert.match(code,/const uint32_t record\[3\]=\{stage\?1u:0u,uint32_t\(stage\),bytes\}/);
 }
 const op=tx.split('case 27: {')[1].split('case 28:')[0];
 assert.match(op,/sdClockKhz/);
 // Older first-fault journal records have no clock field: zero means unknown,
 // and must not silently become the clock configured in a later OTA image.
 assert.match(rec,/static std::atomic<uint32_t> odysseyPersistedWireClockKhz\{0\}/);
 assert.match(rec,/if \(prefs\.getBytesLength\("wclk"\)==sizeof\(clock\)/);
 assert.doesNotMatch(op,/static_cast<unsigned long>\(ODYSSEY_SD_DATA_FREQ_HZ\/1000u\)/);
 assert.match(op,/static_cast<unsigned long>\(odysseyLastWireClockKhz\(\)\)/);
 assert.doesNotMatch(op,/static_cast<unsigned long>\(ODYSSEY_SD_DATA_FREQ_HZ\/1000u\)/);
 assert.doesNotMatch(op,/esp_vfs_fat_sdspi_mount|odysseyRecoverSdCard\(|fwrite\(|unlink\(/);
});
test('C3 SD still uses short CMD25 batches and quarantines unknown writes',()=>{
 for(const code of [rec,ino]){
  assert.match(code,/ODYSSEY_SD_RECORD_BATCH_BYTES=2048u/);
  assert.match(code,/if \(file && \(odysseySdWriteTimeoutQuarantined\(\) \|\| uncertainSdWrite\)\)/);
  assert.match(code,/odysseySdQuarantineUncertainWrite\(\)/);
  assert.doesNotMatch(code,/ODYSSEY_SD_FIRST_CHECKPOINT_PCM_BYTES|ODYSSEY_SD_CHECKPOINT_INTERVAL_MS|odysseyCheckpointWav/);
  assert.match(code,/fclose\(file\)/);
 }
 assert.match(detect,/odysseySdMarkHistoricalFaultResolved\(\)/);
 assert.match(detect,/odysseySdTimedOutWriteQuarantined=false; \/\/ only a verified fresh mount clears quarantine/);
});
