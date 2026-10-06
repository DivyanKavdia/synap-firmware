'use strict';
const {test}=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs');
const source=fs.readFileSync('firmware/shared/odyssey-sd-clean-recording.cpp','utf8');

test('fresh C3 recorder has one write-only SD owner',()=>{
  assert.match(source,/ODYSSEY_SD_WRITE_CHUNK_BYTES=512u/);
  assert.match(source,/SD\.begin\([\s\S]*"\/odyssey-sd",2,false\)/);
  assert.match(source,/SD\.open\(temp,FILE_WRITE\)/);
  assert.match(source,/SD\.rename\(temp,wav\)/);
  assert.match(source,/namespace OdysseyTransfer[\s\S]*available\(\)\{return false;\}/);
  assert.doesNotMatch(source,/opendir|readdir|FILE_READ|readSelected|SYNAPJ01|SYNAPM01/);
});

test('fresh recorder writes sector-sized chunks and finalizes the WAV before rename',()=>{
  assert.match(source,/sectorOffset=offset&\(ODYSSEY_SD_WRITE_CHUNK_BYTES-1u\)/);
  assert.match(source,/chunk=ODYSSEY_SD_WRITE_CHUNK_BYTES/);
  const seek=source.indexOf('file.seek(0)');
  const header=source.indexOf('freshWavHeader(header,bytes)',seek);
  const close=source.indexOf('file.close()',header);
  const rename=source.indexOf('SD.rename(temp,wav)',close);
  assert.ok(seek>0&&header>seek&&close>header&&rename>close);
  assert.match(source,/file\.size\(\)!=ODYSSEY_WAV_HEADER_BYTES\+size_t\(bytes\)/);
});

test('purple capture state begins only after microphone startup',()=>{
  const start=source.indexOf('if(!startMicrophone())');
  const active=source.indexOf('odysseyCaptureActive=true',start);
  const prepared=source.indexOf('odysseySdRecoveryActive=false',active);
  assert.ok(start>0&&active>start&&prepared>active);
  assert.match(source,/odysseyCaptureActive=false;updateStatusLed\(true\);stopMicrophone\(\)/);
});

test('fresh recorder preserves BLE, OTA and sleep handoff contracts',()=>{
  assert.match(source,/odysseyPrepareForConnectedStreaming\(uint32_t timeoutMs\)/);
  assert.match(source,/odysseyStopRequested=true;[\s\S]*while\(odysseyRecording\.load\(\)/);
  assert.match(source,/odysseyPrepareSdForPowerTransition\(uint32_t\)/);
  assert.match(source,/if\(odysseyRecording\.load\(\)\)return false/);
  assert.match(source,/odysseySdSleepGuardUntil=done\+2000u/);
});
