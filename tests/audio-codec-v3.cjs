'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {prepare}=require('../tools/prepare-interactions.cjs');
const {patch:runtime}=require('../tools/patch-runtime-fixes.cjs');
const {patch:eventChannel}=require('../tools/patch-event-channel.cjs');
const {patch:battery}=require('../tools/patch-battery-v2.cjs');
const {patch:audio}=require('../tools/patch-audio-reliability.cjs');
const {patch:codec}=require('../tools/patch-audio-codec-v3.cjs');
const root=path.join(__dirname,'..');

test('production audio uses independent-frame IMA ADPCM protocol v3',()=>{
  const source=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');
  const prepared=codec(audio(battery(eventChannel(runtime(prepare(source))))));
  assert.match(prepared,/AUDIO_PROTOCOL_VERSION = 3/);
  assert.match(prepared,/AUDIO_CODEC_IMA_ADPCM = 1/);
  assert.match(prepared,/ADPCM_BYTES_PER_FRAME == 404/);
  assert.match(prepared,/TRANSPORT_BYTES_PER_FRAME = ADPCM_BYTES_PER_FRAME/);
  assert.match(prepared,/uint16_t encodeImaAdpcm\(const int16_t\* samples, uint8_t\* output\)/);
  assert.match(prepared,/chunksPerFrame = \(TRANSPORT_BYTES_PER_FRAME \+ bounded - 1\) \/ bounded/);
  assert.match(prepared,/audioPayloadBytes = \(TRANSPORT_BYTES_PER_FRAME \+ chunksPerFrame - 1\) \/ chunksPerFrame/);
  assert.match(prepared,/packet\[0\]=AUDIO_PACKET_MAGIC; packet\[1\]=AUDIO_PROTOCOL_VERSION/);
  assert.match(prepared,/memcpy\(packet\+AUDIO_HEADER_BYTES, encoded\+offset, length\)/);
  assert.doesNotMatch(prepared,/memcpy\(packet\+AUDIO_HEADER_BYTES, pcm\+offset, length\)/);
});
