const fs=require('node:fs');

function replaceOnce(source,before,after,label){
  const index=source.indexOf(before);
  if(index<0)throw new Error(`Missing firmware anchor: ${label}`);
  if(source.indexOf(before,index+before.length)>=0)throw new Error(`Ambiguous firmware anchor: ${label}`);
  return source.slice(0,index)+after+source.slice(index+before.length);
}

function patch(source){
  let out=source;
  out=replaceOnce(out,
`constexpr uint8_t AUDIO_PACKET_MAGIC = 0xA5;
constexpr uint8_t STATUS_PACKET_MAGIC = 0x5A;`,
`constexpr uint8_t AUDIO_PACKET_MAGIC = 0xA5;
constexpr uint8_t AUDIO_PROTOCOL_VERSION = 3;
constexpr uint8_t AUDIO_CODEC_IMA_ADPCM = 1;
constexpr uint8_t STATUS_PACKET_MAGIC = 0x5A;`,
  'audio protocol v3 constants');

  out=replaceOnce(out,
`constexpr uint16_t AUDIO_BYTES_PER_FRAME = 1600;
constexpr uint8_t AUDIO_HEADER_BYTES = 8;`,
`constexpr uint16_t AUDIO_BYTES_PER_FRAME = 1600;
constexpr uint16_t ADPCM_HEADER_BYTES = 4;
constexpr uint16_t ADPCM_BYTES_PER_FRAME = ADPCM_HEADER_BYTES + (SAMPLES_PER_FRAME / 2);
constexpr uint16_t TRANSPORT_BYTES_PER_FRAME = ADPCM_BYTES_PER_FRAME;
constexpr uint8_t AUDIO_HEADER_BYTES = 8;
static_assert(SAMPLES_PER_FRAME % 2 == 0, "ADPCM frame requires an even PCM sample count");
static_assert(ADPCM_BYTES_PER_FRAME == 404, "Synap protocol-v3 ADPCM frame size");`,
  'ADPCM transport size');

  // The former 91-byte floor was derived from 1600-byte PCM frames. A 404-byte
  // ADPCM frame fits in at most 20 chunks from ATT MTU 32 upward.
  out=replaceOnce(out,
`constexpr uint16_t MIN_REQUIRED_MTU = 91;`,
`constexpr uint16_t MIN_REQUIRED_MTU = 32;`,
  'compressed minimum MTU');

  out=replaceOnce(out,
`  chunksPerFrame = (AUDIO_BYTES_PER_FRAME + bounded - 1) / bounded;`,
`  chunksPerFrame = (TRANSPORT_BYTES_PER_FRAME + bounded - 1) / bounded;`,
  'compressed chunk count');
  out=replaceOnce(out,
`  audioPayloadBytes = (AUDIO_BYTES_PER_FRAME + chunksPerFrame - 1) / chunksPerFrame;`,
`  audioPayloadBytes = (TRANSPORT_BYTES_PER_FRAME + chunksPerFrame - 1) / chunksPerFrame;`,
  'compressed payload size');

  const encoder=`static const uint16_t IMA_STEP_TABLE[89] = {
  7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,
  34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
  157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,
  724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,
  2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
  10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
};
static const int8_t IMA_INDEX_TABLE[8] = {-1,-1,-1,-1,2,4,6,8};

uint16_t encodeImaAdpcm(const int16_t* samples, uint8_t* output) {
  int32_t predictor=samples[0];
  int32_t index=0;
  output[0]=uint8_t(predictor&255);output[1]=uint8_t((predictor>>8)&255);
  output[2]=uint8_t(index);output[3]=AUDIO_CODEC_IMA_ADPCM;
  memset(output+ADPCM_HEADER_BYTES,0,ADPCM_BYTES_PER_FRAME-ADPCM_HEADER_BYTES);
  for (uint16_t sampleIndex=1;sampleIndex<SAMPLES_PER_FRAME;++sampleIndex) {
    const int32_t step=IMA_STEP_TABLE[index];
    int32_t difference=int32_t(samples[sampleIndex])-predictor;
    uint8_t code=0;
    if (difference<0) { code=8;difference=-difference; }
    int32_t delta=step>>3;
    if (difference>=step) { code|=4;difference-=step;delta+=step; }
    if (difference>=(step>>1)) { code|=2;difference-=step>>1;delta+=step>>1; }
    if (difference>=(step>>2)) { code|=1;delta+=step>>2; }
    predictor+=(code&8)?-delta:delta;
    if (predictor>32767) predictor=32767;
    if (predictor<-32768) predictor=-32768;
    index+=IMA_INDEX_TABLE[code&7];
    if (index<0) index=0;
    if (index>88) index=88;
    const uint16_t packedIndex=ADPCM_HEADER_BYTES+((sampleIndex-1)>>1);
    if ((sampleIndex-1)&1) output[packedIndex]|=uint8_t((code&15)<<4);
    else output[packedIndex]=uint8_t(code&15);
  }
  return ADPCM_BYTES_PER_FRAME;
}

`;
  out=replaceOnce(out,
`bool sendAudioFrame(const AudioFrame& frame, uint16_t sequence) {`,
encoder+`bool sendAudioFrame(const AudioFrame& frame, uint16_t sequence) {`,
  'IMA ADPCM encoder');

  // sendAudioFrame runs only in the transmitter task. Keep its two large scratch
  // buffers in BSS instead of consuming nearly 1 KB of the task stack on every
  // first frame, which could reset the ESP and drop BLE at recording start.
  out=replaceOnce(out,
`  uint8_t packet[AUDIO_HEADER_BYTES+MAX_AUDIO_PAYLOAD_BYTES];
  const uint8_t* pcm=reinterpret_cast<const uint8_t*>(frame.samples);
  const uint32_t started=micros();`,
`  static uint8_t packet[AUDIO_HEADER_BYTES+MAX_AUDIO_PAYLOAD_BYTES];
  static uint8_t encoded[ADPCM_BYTES_PER_FRAME];
  const uint16_t encodedBytes=encodeImaAdpcm(frame.samples,encoded);
  if (encodedBytes!=TRANSPORT_BYTES_PER_FRAME) return false;
  const uint32_t started=micros();`,
  'encode frame before BLE');

  out=replaceOnce(out,
`    const uint16_t remaining=AUDIO_BYTES_PER_FRAME-offset;`,
`    const uint16_t remaining=encodedBytes-offset;`,
  'compressed frame remaining bytes');
  out=replaceOnce(out,
`    packet[0]=AUDIO_PACKET_MAGIC; packet[1]=PROTOCOL_VERSION;`,
`    packet[0]=AUDIO_PACKET_MAGIC; packet[1]=AUDIO_PROTOCOL_VERSION;`,
  'audio packet protocol version');
  out=replaceOnce(out,
`    memcpy(packet+AUDIO_HEADER_BYTES, pcm+offset, length);`,
`    memcpy(packet+AUDIO_HEADER_BYTES, encoded+offset, length);`,
  'compressed packet payload');

  // BLE notify plus the 1608-byte queued AudioFrame need more than the original
  // 4 KB margin even after moving codec scratch storage out of the stack.
  out=replaceOnce(out,
`xTaskCreatePinnedToCore(transmitterTask, "transmit", 4096, nullptr, 2, nullptr, 1)`,
`xTaskCreatePinnedToCore(transmitterTask, "transmit", 8192, nullptr, 2, nullptr, 1)`,
  'transmitter stack headroom');

  return out;
}

if(require.main===module){
  const file=process.argv[2];
  if(!file)throw new Error('Usage: node tools/patch-audio-codec-v3.cjs <sketch>');
  fs.writeFileSync(file,patch(fs.readFileSync(file,'utf8')));
  console.log('Patched Synap protocol-v3 IMA ADPCM audio transport');
}
module.exports={patch};
