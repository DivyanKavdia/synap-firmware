const fs=require('node:fs');

function replaceOnce(source,before,after,label){
  const i=source.indexOf(before);
  if(i<0)throw new Error(`Missing firmware anchor: ${label}`);
  if(source.indexOf(before,i+before.length)>=0)throw new Error(`Ambiguous firmware anchor: ${label}`);
  return source.slice(0,i)+after+source.slice(i+before.length);
}

function patch(source){
  let out=source;

  // Let a large negotiated ATT MTU materially reduce notification pressure.
  // At MTU 517 this produces four ~400-byte chunks per 50 ms PCM frame instead
  // of the old hard minimum of ten 160-byte chunks. Smaller MTUs naturally use
  // more chunks and retain the existing 20-chunk safety ceiling.
  out=replaceOnce(out,
`constexpr uint8_t MIN_CHUNKS_PER_FRAME = 10;`,
`constexpr uint8_t MIN_CHUNKS_PER_FRAME = 4;`,
  'minimum audio chunks');
  out=replaceOnce(out,
`constexpr uint16_t MAX_AUDIO_PAYLOAD_BYTES = 160;`,
`constexpr uint16_t MAX_AUDIO_PAYLOAD_BYTES = 500;`,
  'maximum audio payload');

  out=replaceOnce(out,
`  if (chunks < 10 || chunks > 20 || !payload || payload > 160) return false;
  uint8_t packet[AUDIO_HEADER_BYTES+MAX_AUDIO_PAYLOAD_BYTES];`,
`  if (chunks < MIN_CHUNKS_PER_FRAME || chunks > MAX_CHUNKS_PER_FRAME ||
      !payload || payload > MAX_AUDIO_PAYLOAD_BYTES) return false;
  uint8_t packet[AUDIO_HEADER_BYTES+MAX_AUDIO_PAYLOAD_BYTES];`,
  'audio send bounds');

  // Finish each frame with transport headroom. The capture side remains clocked
  // by I2S at 50 ms/frame, while the transmitter has ~5 ms/frame to recover from
  // scheduler/BLE jitter instead of being permanently exactly at the deadline.
  out=replaceOnce(out,
`    const uint32_t target=started+static_cast<uint32_t>(index+1)*50000UL/chunks;`,
`    const uint32_t target=started+static_cast<uint32_t>(index+1)*45000UL/chunks;`,
  'audio transmit pacing');

  // One second of PCM frame buffering absorbs short browser/radio scheduling
  // stalls without immediately dropping captured frames. Sequence numbers still
  // make any true overflow visible to the PWA diagnostics/reconstruction layer.
  out=replaceOnce(out,
`  audioFrameQueue=xQueueCreate(4, sizeof(AudioFrame));`,
`  audioFrameQueue=xQueueCreate(20, sizeof(AudioFrame));`,
  'audio queue depth');

  // Battery telemetry is low priority while streaming. Sampling continues, but
  // defer its BLE notification until idle so audio never competes with periodic
  // battery/control traffic on constrained mobile links.
  out=replaceOnce(out,
`  publishBatteryEvent(true);
  updateStatusLed(true);
#endif
}`,
`  if (!streamingEnabled.load()) publishBatteryEvent(true);
  updateStatusLed(true);
#endif
}`,
  'defer battery notification during audio');

  return out;
}

if(require.main===module){
  const file=process.argv[2];
  if(!file)throw new Error('Usage: node tools/patch-audio-reliability.cjs <sketch>');
  const source=fs.readFileSync(file,'utf8');
  fs.writeFileSync(file,patch(source));
  console.log('Patched Synap BLE audio reliability');
}
module.exports={patch};
