'use strict';
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {materialize}=require('../tools/materialize-target.cjs');
const root=path.join(__dirname,'..');

test('production audio prefers PCM16 and retains independent-frame ADPCM only for small MTUs',()=>{
  const source=fs.readFileSync(path.join(root,'synap_esp32s3/synap_esp32s3.ino'),'utf8');
  assert.match(source,/AUDIO_PROTOCOL_VERSION = 3/);
  assert.match(source,/AUDIO_CODEC_IMA_ADPCM = 1/);
  assert.match(source,/ADPCM_BYTES_PER_FRAME == 404/);
  assert.match(source,/MIN_CHUNKS_PER_FRAME = 1/);
  assert.match(source,/MIN_REQUIRED_MTU = 32/);
  assert.match(source,/void encodeImaAdpcm\(const int16_t\* samples, uint8_t\* output\)/);
  assert.match(source,/PCM_MIN_MTU = 185/);
  assert.match(source,/pcmTransport=peerMtu>=PCM_MIN_MTU/);
  assert.match(source,/reinterpret_cast<const uint8_t\*>\(frame.samples\)/);
  assert.match(source,/static uint8_t packet\[AUDIO_HEADER_BYTES\+MAX_AUDIO_PAYLOAD_BYTES\]/);
  assert.match(source,/static uint8_t encoded\[ADPCM_BYTES_PER_FRAME\]/);
  assert.doesNotMatch(source,/\n  uint8_t packet\[AUDIO_HEADER_BYTES\+MAX_AUDIO_PAYLOAD_BYTES\]/);
  assert.doesNotMatch(source,/\n  uint8_t encoded\[ADPCM_BYTES_PER_FRAME\]/);
  assert.match(source,/xTaskCreatePinnedToCore\(transmitterTask, "transmit", 8192/);
  assert.match(source,/packet\[0\]=AUDIO_PACKET_MAGIC; packet\[1\]=pcm\?PCM_AUDIO_PROTOCOL_VERSION:AUDIO_PROTOCOL_VERSION/);
  assert.match(source,/memcpy\(packet\+AUDIO_HEADER_BYTES, encoded\+offset, length\)/);
  assert.doesNotMatch(source,/memcpy\(packet\+AUDIO_HEADER_BYTES, pcm\+offset, length\)/);
  assert.match(source,/#define SYNAP_TOUCH_PIN 13/);

  const c3=materialize(source,'esp32c3-supermini-4m');
  assert.match(c3,/xTaskCreate\(transmitterTask, "transmit", 8192/);
  assert.doesNotMatch(c3,/xTaskCreatePinnedToCore\(transmitterTask/);
  assert.match(c3,/#define SYNAP_TOUCH_PIN 3/);
});
