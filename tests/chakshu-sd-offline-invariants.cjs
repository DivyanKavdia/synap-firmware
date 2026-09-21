'use strict';
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');

test('Chakshu SD remains the durable offline inbox without capture-time remounts',()=>{
  const storage=fs.readFileSync('firmware/xiao-sense/sd-storage.cpp','utf8');
  const transfer=fs.readFileSync('firmware/xiao-sense/media-transfer.cpp','utf8');
  const recording=fs.readFileSync('firmware/xiao-sense/sd-recording.cpp','utf8');
  const voice=fs.readFileSync('firmware/xiao-sense/voice.cpp','utf8');

  // The working Sense wiring is a production invariant. GPIO21 is SD CS, not
  // an application LED. Media code must never tear the bus down mid-capture.
  assert.match(storage,/SD_SCK=7,SD_MISO=8,SD_MOSI=9,SD_CS=21/);
  assert.match(storage,/SPI\.begin\(SD_SCK,SD_MISO,SD_MOSI,SD_CS\)/);
  assert.doesNotMatch(transfer,/SD\.end\(\)|SPI\.end\(\)/);
  assert.doesNotMatch(recording,/SD\.end\(\)|SPI\.end\(\)/);

  // A connected PWA is the sole capture owner. Local/voice SD requests are
  // rejected as soon as BLE owns the device, preventing two writers.
  assert.match(voice,/ownershipAllowsVoice\(\)\{return enabled\.load\(\)&&!linkStandDown\.load\(\)&&!deviceConnected\.load\(\);\}/);
  assert.match(transfer,/if\(request\.local&&\(deviceConnected\.load\(\)\|\|request\.localEpoch!=localEpoch\.load\(\)\)\)continue;/);
  assert.match(transfer,/if\(!request\.local\)\{replyFor\(request,ChakshuMedia::BAD_COMMAND\);continue;\}/);

  // Recovery/probing is non-destructive. Never make a future readiness change
  // silently format a card containing unsynced memories.
  assert.match(storage,/sdcard_mount\(drive,"\/sd-probe",1,false\)/);
  assert.doesNotMatch(storage,/format_if_empty\s*=\s*true|SD_MMC\.format|\.format\(/);
});

test('offline media is only acknowledged after its SD file is closed',()=>{
  const transfer=fs.readFileSync('firmware/xiao-sense/media-transfer.cpp','utf8');
  const recording=fs.readFileSync('firmware/xiao-sense/sd-recording.cpp','utf8');
  assert.match(voiceSafe(transfer),/Publish completion only after the SD worker has closed the capture files/);
  assert.match(recording,/file\.flush\(\)[\s\S]*file\.close\(\)/);
});

function voiceSafe(source){
  const voice=fs.readFileSync('firmware/xiao-sense/voice.cpp','utf8');
  return voice+'\n'+source;
}
