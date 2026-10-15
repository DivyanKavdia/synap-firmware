'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const detect=fs.readFileSync('firmware/shared/odyssey-sd-1631-detect.cpp','utf8');
const capture=fs.readFileSync('firmware/shared/odyssey-sd-1631-recording.cpp','utf8');
const tx=fs.readFileSync('firmware/shared/odyssey-sd-1631-transfer.cpp','utf8');
const ino=fs.readFileSync('synap_esp32s3/synap_esp32s3.ino','utf8');

test('empty FAT cards create recording directly in root 8.3 entry with no mkdir',()=>{
  const take=capture.split('static void odysseyRecordTake() {')[1]
    .split('static void odysseyRecordTask(void*) {')[0];
  assert.match(take,/R%07lX\.WAV/);
  assert.match(take,/esp_random\(\)&0x0fffffffu/);
  assert.match(take,/for\(uint8_t attempt=0;attempt<16u/);
  assert.match(take,/if \(stat\(fullPath,&exists\)==0\) continue/);
  assert.match(take,/errno!=ENOENT/);
  assert.match(take,/fopen\(fullPath,"wb"\)/);
  assert.doesNotMatch(take,/\bmkdir\s*\(/);
  assert.doesNotMatch(take,/\brmdir\s*\(/);
  assert.doesNotMatch(take,/\bunlink\s*\(/);
  assert.match(take,/odysseySdWriteTimeoutQuarantined\(\)/);
  assert.match(capture,/ODYSSEY_SD_CHECKPOINT_INTERVAL_MS=60000u/);
});

test('root files are strictly R plus uppercase seven hex with a .WAV suffix',()=>{
  for(const source of [detect,ino]){
    assert.match(source,/bool odysseySdRootWavName\(const char\* name\)/);
    assert.match(source,/strlen\(name\)!=12u/);
    assert.match(source,/name\[0\]!='R'/);
    assert.match(source,/strcmp\(name\+8,"\.WAV"\)!=0/);
    assert.match(source,/strncmp\(logical,"\/synap\/",7\)==0/);
    assert.match(source,/snprintf\(full,capacity,"%s\/%s",ODYSSEY_SD_MOUNT_POINT,logical\+7\)/);
  }
  const accept=n=>/^R[0-9A-F]{7}\.WAV$/.test(n);
  for(const name of ['R123ABCD.WAV','R0000000.WAV','RFFFFFFF.WAV'])
    assert(accept(name));
  for(const name of ['R123ABCD.wav','R123ABCD.BIN','odyssey_audio_1234.wav',
    'R0000000.WAV.bak','R000000g.WAV','../R1.WAV'])assert(!accept(name));
});

test('BLE catalogue reads new root and legacy /synap and does not mutate either',()=>{
  const catalogue=tx.split('static uint8_t catalogue(uint32_t& total) {')[1]
    .split('static uint8_t removeFailureStep=0;')[0];
  assert.match(catalogue,/odysseySdMountPoint\(\),ODYSSEY_SD_RECORDING_DIR/);
  assert.match(catalogue,/if \(scan==1u && directoryErrno==ENOENT\) continue/);
  assert.match(catalogue,/odysseySdRootWavName\(name\)/);
  assert.match(catalogue,/catalogueBuffer\+="\]"/);
  assert.match(catalogue,/return IO_ERROR/);
  assert.doesNotMatch(catalogue,/\bmkdir\s*\(|\bfwrite\s*\(|\bunlink\s*\(|odysseyRecoverSdCard\(/);
  assert.match(tx,/ODYSSEY_C3_SD_BLE_MUTATIONS_ENABLED=false/);
});

