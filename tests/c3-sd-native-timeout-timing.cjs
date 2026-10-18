'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const sd=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const rec=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const tx=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const ino=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');
test('first native C3 timeout is observed without retrying data commands',()=>{
  const cb=sd.split('host.do_transaction=[](int slot,sdmmc_command_t* cmd)->esp_err_t {')[1].split('sdspi_device_config_t device=')[0];
  assert.equal((cb.match(/sdspi_host_do_transaction\(slot,cmd\)/g)||[]).length,2);
  assert.match(cb,/cmd->opcode==59u && cmd->arg==1u &&\s*result==ESP_ERR_NOT_SUPPORTED/);
  assert.equal((cb.match(/vTaskDelay\(/g)||[]).length,1);
  assert.match(cb,/const uint32_t startedAt=millis\(\);/);
  assert.match(cb,/uint32_t\(millis\(\)-startedAt\)/);
  assert.match(cb,/odysseyWireElapsedMs=elapsed;/);
  assert.match(cb,/odysseyWireBudgetMs=cmd->timeout_ms;/);
  assert.match(cb,/odysseyWireDataLength=cmd->datalen;/);
  assert.match(cb,/odysseyWirePriorSuccessCmd=writeParent!=UINT32_MAX/);
  assert.match(cb,/\?writeParent:odysseyWirePriorCmd\.load\(\)/);
  assert.match(cb,/odysseyWireWritesAtFailure=odysseyWireGoodWrites.load\(\);/);
  assert.match(cb,/odysseyWireCmdPlusOne.compare_exchange_strong\(expected,cmd->opcode\+1u\)/);
  assert.doesNotMatch(cb,/delay\(|fwrite|fopen|sdmmc_send_cmd\(|spi_device_transmit|odysseyRecoverSdCard/);
  assert.match(sd,/odysseySdClearWireFault\(\)/);
});
test('timing survives reboot in a separate stage/byte-paired NVS journal without changing backward ABI',()=>{
  assert.match(rec,/prefs\.putBytes\("wtime",timing,sizeof\(timing\)\)/);
  assert.match(rec,/prefs\.getBytesLength\("wtime"\)==sizeof\(timing\)/);
  assert.match(rec,/timing\[0\]==record\[1\] && timing\[1\]==record\[2\]/);
  assert.match(rec,/const uint32_t record\[3\]=\{stage\?1u:0u,uint32_t\(stage\),bytes\}/);
  assert.match(rec,/odysseyPersistedWireMs=stage\?odysseySdWireFaultElapsedMs\(\):0u/);
  for(const field of ['WireMs','WireBudget','WireLen','WirePrev','WireGoodWrites'])
    assert.match(rec,new RegExp('uint32_t odysseyLast'+field+'\\(\\)'));
});
test('BLE op27 includes new raw timing and a bounded fallback with no card access',()=>{
  const op=tx.split('case 27: {')[1].split('case 28:')[0];
  for(const name of ['wireCmd','wireErr','wireArg','wireMs','wireBudget','wireLen','wirePrev','wireWriteOk'])
    assert(op.includes('\\\"'+name+'\\\"'),name);
  assert.match(op,/char detail\[480\]/);
  assert.match(op,/if \(n>=int\(sizeof\(detail\)\)\)/);
  assert.match(op,/if \(n<0 \|\| size_t\(n\)>=sizeof\(detail\)\)/);
  assert.doesNotMatch(op,/sdspi_host_do_transaction|fwrite|unlink|esp_vfs_fat_sdspi_mount|odysseyRecoverSdCard/);
  assert.match(ino,/odysseyWireBudgetMs=cmd->timeout_ms/);
  assert.match(ino,/prefs\.putBytes\("wtime",timing,sizeof\(timing\)\)/);
});
test('op27 concise fallback leaves enough room even for max-width uint32 values',()=>{
  const op=tx.split('case 27: {')[1].split('case 28:')[0];
  const formats=[...op.matchAll(/snprintf\(detail,sizeof\(detail\),\s*("(?:\\.|[^"])*")/g)];
  assert.equal(formats.length,2);
  const compact=JSON.parse(formats[1][1]);
  const max=compact.replace(/%lu/g,'4294967295').replace(/%u/g,'4294967295').replace(/%ld/g,'-2147483648');
  assert(max.length<480, 'fallback must fit within media 480-byte payload');
  const got=JSON.parse(compact.replace(/%lu|%u/g,'1').replace(/%ld/g,'-1'));
  assert.equal(got.wireWriteOk,1);
  assert.equal(got.wireBudget,1);
});
