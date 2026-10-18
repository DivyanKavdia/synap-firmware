'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const detector=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const recording=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const transfer=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const ino=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');

test('C3 quarantines a physical CMD24/25 program timeout without reset or remount',()=>{
  for(const source of [detector,ino]) {
    assert.match(source,/odysseySdTimedOutWriteQuarantined=true/);
    assert.match(source,/cmd->opcode==24u \|\| cmd->opcode==25u/);
    assert.match(source,/cmd->opcode==13u && \(prior==24u \|\| prior==25u \|\|[\s\S]*?writeParent==24u \|\| writeParent==25u\)\)\)/);
    assert.match(source,/if \(odysseySdWriteTimeoutQuarantined\(\) \|\|\s*odysseySdReadTimeoutQuarantined\(\)\) \{/);
    assert.match(source,/recovery refused after timed-out CMD17\/18 or CMD24\/25/);
    assert.match(source,/odysseySdTimedOutWriteQuarantined=false/);
  }
  const recovery=detector.split('bool odysseyRecoverSdCard(const char* reason) {')[1].split('esp_err_t odysseyFormatSdCard()')[0];
  assert(recovery.indexOf('if (odysseySdWriteTimeoutQuarantined() ||')<
    recovery.indexOf('odysseySdReleaseLocked()'),'blocked BEFORE teardown or new CMD0');
});
test('first SD write-timeout abort never fclose nor attempt a second write on stuck card',()=>{
  for(const source of [recording,ino]){
    const take=source.split('static void odysseyRecordTake() {')[1].split('static void odysseyRecordTask(void*) {')[0];
    assert(take);
    assert.match(take,/if \(file && \(odysseySdWriteTimeoutQuarantined\(\) \|\| uncertainSdWrite\)\) \{/);
    assert.match(take,/file=nullptr/);
    const quarantine=take.indexOf('if (file && (odysseySdWriteTimeoutQuarantined() || uncertainSdWrite))');
    const close=take.indexOf('if (fclose(file)!=0)');
    assert(quarantine>=0 && quarantine<close,'quarantine before potentially blocking fclose');
    assert.match(take,/odysseySdUnsafeToSleep=true/);
    assert.match(take,/odysseySdBootState=2/);
    assert.match(take,/odysseyPersistRecordFailure\(persistedStage,bytes\)/);
    assert.match(source,/previousProgramTimeout && esp_reset_reason\(\)!=ESP_RST_POWERON/);
  }
});
test('reduce forced FAT metadata updates, tolerate brief I2S starvation, never erase SD files',()=>{
  for(const source of [recording,ino]){
    assert.match(source,/ODYSSEY_SD_CHECKPOINT_INTERVAL_MS=60000u/);
    assert.match(source,/uint32_t\(millis\(\)-noAudioSince\)>=1000u/);
  }
  assert.match(transfer,/ODYSSEY_C3_SD_BLE_BULK_CLEAR_ENABLED=false/);
  const worker=transfer.split('static void worker(void*) {')[1];
  assert(worker.indexOf('!ODYSSEY_C3_SD_BLE_BULK_CLEAR_ENABLED')<worker.indexOf('case 17:'));
  assert.match(worker,/request.operation==18u/);
  assert.match(worker,/reply\(request,BAD_COMMAND\)/);
});

test('CMD24 invalid-response 264 quarantines an uncertain FAT write without a subsequent fclose',()=>{
 for(const source of [detector,ino]){
  assert.match(source,/error==ESP_ERR_TIMEOUT \|\| error==ESP_ERR_INVALID_RESPONSE/);
  assert.match(source,/odysseySdTimedOutWriteQuarantined=true/);
 }
 for(const source of [recording,ino]){
  assert.match(source,/odysseyLastWireError\(\)==uint32_t\(ESP_ERR_INVALID_RESPONSE\)/);
  const take=source.split('static void odysseyRecordTake() {')[1].split('static void odysseyRecordTask(void*) {')[0];
  const beforeClose=take.indexOf('if (odysseySdWriteTimeoutQuarantined() || uncertainSdWrite) {',take.indexOf('if (!failed && batchUsed) {'));
  assert(beforeClose>=0 && beforeClose<take.indexOf('if (fclose(file)!=0)'));
 }
});

test('second post-write CMD13 retains CMD25 ancestry and quarantines immediately',()=>{
 for(const source of [detector,ino]){
  assert.match(source,/odysseyPostWriteStatusSlots=2/);
  assert.match(source,/const uint32_t writeParent=\(opcode==13u && odysseyPostWriteStatusSlots\.load\(\)\)/);
  assert.match(source,/writeParent==24u \|\| writeParent==25u/);
  assert.match(source,/odysseyWirePriorSuccessCmd=writeParent!=UINT32_MAX/);
  assert.match(source,/odysseyPostWriteStatusSlots=0/);
  assert.match(source,/odysseyPostWriteParent=UINT32_MAX/);
  assert.doesNotMatch(source,/cmd->timeout_ms\s*=\s*5000/);
 }
 const take=recording.split('static void odysseyRecordTake() {')[1]
   .split('static void odysseyRecordTask(void*) {')[0];
 assert.match(take,/odysseySdQuarantineUncertainWrite\(\)/);
 assert.match(take,/if \(file && \(odysseySdWriteTimeoutQuarantined\(\) \|\| uncertainSdWrite\)\)/);
});
