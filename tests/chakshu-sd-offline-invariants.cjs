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
  assert.match(voice,/ownershipAllowsVoice\(\)\{return enabled\.load\(\)&&!linkStandDown\.load\(\)&&!deviceConnected\.load\(\)&&!sleepPending;\}/);
  assert.match(transfer,/if\(request\.local&&\(deviceConnected\.load\(\)\|\|request\.localEpoch!=localEpoch\.load\(\)\)\)continue;/);
  assert.match(transfer,/if\(!request\.local\)\{replyFor\(request,ChakshuMedia::BAD_COMMAND\);continue;\}/);

  // Recovery/probing is non-destructive. Never make a future readiness change
  // silently format a card containing unsynced memories.
  assert.match(storage,/sdcard_mount\(drive,"\/sd-probe",1,false\)/);
  assert.doesNotMatch(storage,/format_if_empty\s*=\s*true|SD_MMC\.format|\.format\(/);

  // Every capture still on SD is unsynced. Space checks may reject a new take,
  // but must never delete an older one behind the user's back.
  const spaceStart=storage.indexOf('bool ensureSpace('),
    spaceEnd=storage.indexOf('uint16_t clearCaptures()',spaceStart),
    space=storage.slice(spaceStart,spaceEnd);
  assert.match(space,/if\(freeBytes<required\)/);
  assert.match(space,/unsynced media preserved/);
  assert.doesNotMatch(space,/oldestCapture\(|removeCapture\(/);
});


test('Hey Snap photo and video commands remain routed to durable SD operations',()=>{
  const voice=fs.readFileSync('firmware/xiao-sense/voice.cpp','utf8');
  const transfer=fs.readFileSync('firmware/xiao-sense/media-transfer.cpp','utf8');
  const tick=voice.slice(voice.indexOf('void tick()'),voice.indexOf('class Callbacks'));
  const worker=transfer.slice(transfer.indexOf('void worker(void*)'),transfer.indexOf('class CommandCallbacks'));

  // PHOTO must enqueue the saved full-resolution photo operation, not a BLE
  // preview capture. VIDEO_START must enqueue the standalone SD recorder with
  // the current 10-second default.
  assert.match(tick,/command==PHOTO \|\| command==DESCRIBE[\s\S]*queueLocal\(11,command==DESCRIBE\?1u:0u\)/);
  assert.match(tick,/command==VIDEO_START[\s\S]*queueLocal\(5,uint32_t\(10u\)<<8\)/);
  assert.match(voice,/if\(operation==11\)command=photoCompletionCommand\.exchange\(PHOTO\)/);
  assert.match(voice,/else if\(operation==5\)command=VIDEO_START/);
  assert.match(voice,/else if\(operation==10\)command=AUDIO_ON/);

  // The worker must keep those requests local-only and finish them through the
  // SD-backed implementations before publishing completion to voice status.
  assert.match(worker,/request\.operation==5\|\|request\.operation==10/);
  assert.match(worker,/if\(!request\.local\)\{replyFor\(request,ChakshuMedia::BAD_COMMAND\);continue;\}/);
  assert.match(worker,/recordOffline\(request\.operation==5\)/);
  assert.match(worker,/case 11:[\s\S]*captureSavedPreview\(\)[\s\S]*writeDescribeMarker\(\)[\s\S]*ChakshuMedia::save\(s\)/);
  const replyFor=transfer.slice(transfer.indexOf('void replyFor('),transfer.indexOf('void readResponse('));
  assert.match(replyFor,/request\.operation==11\)ChakshuVoice::mediaCompleted\(request\.operation,error\)/);
  assert.match(transfer,/const bool describe=path\.endsWith\("\.jpg"\)&&SD\.exists/);
  assert.match(transfer,/\\"describe\\":true/);
});

test('describe markers are companions and are removed with the verified JPG',()=>{
  const storage=fs.readFileSync('firmware/xiao-sense/sd-storage.cpp','utf8');
  const transfer=fs.readFileSync('firmware/xiao-sense/media-transfer.cpp','utf8');
  assert.match(transfer,/constexpr char payload\[\]=\"\{\\\"schema\\\":1,\\\"voice\\\":\\\"describe\\\"\}\"/);
  const removal=storage.slice(storage.indexOf('bool removeCapture('),storage.indexOf('bool oldestCapture('));
  assert.match(removal,/SD\.exists\(jpg\.c_str\(\)\)[\s\S]*removeIfPresent\(json\)[\s\S]*removeIfPresent\(jpg\)/);
});

test('offline media is only acknowledged after its SD file is closed',()=>{
  const transfer=fs.readFileSync('firmware/xiao-sense/media-transfer.cpp','utf8');
  const recording=fs.readFileSync('firmware/xiao-sense/sd-recording.cpp','utf8');
  assert.match(voiceSafe(transfer),/Publish completion only after the SD worker has closed the capture files/);
  assert.match(recording,/audio\.flush\(\);audio\.close\(\)/);
  assert.match(recording,/index\.flush\(\);index\.close\(\)/);
  assert.match(recording,/video\.flush\(\);video\.close\(\)/);
});

function voiceSafe(source){
  const voice=fs.readFileSync('firmware/xiao-sense/voice.cpp','utf8');
  return voice+'\n'+source;
}
