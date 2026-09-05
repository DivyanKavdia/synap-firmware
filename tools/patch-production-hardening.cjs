'use strict';
const fs=require('node:fs');
function replaceOnce(source,before,after,label){const i=source.indexOf(before);if(i<0)throw Error(`Missing production hardening anchor: ${label}`);if(source.indexOf(before,i+before.length)>=0)throw Error(`Ambiguous production hardening anchor: ${label}`);return source.slice(0,i)+after+source.slice(i+before.length)}
function patch(source){let out=source;
  // Make the prepared source itself describe the production S3 wiring. This removes
  // the hidden compiler-only GPIO13 override and lets release source hashes reproduce
  // the actual binary configuration.
  out=replaceOnce(out,'#define SYNAP_TOUCH_PIN 7','#define SYNAP_TOUCH_PIN 13','S3 production touch pin');
  // A 404-byte ADPCM frame fits one notification at large MTU. Let the negotiated
  // capacity choose 1..20 chunks instead of forcing four and wasting radio events.
  out=replaceOnce(out,'constexpr uint8_t MIN_CHUNKS_PER_FRAME = 4;','constexpr uint8_t MIN_CHUNKS_PER_FRAME = 1;','compressed minimum chunks');
  // Make an accidental brush less able to stop an already-running recording and
  // extend the post-state-change quiet period. Remember remains >=1.2 s.
  out=replaceOnce(out,'constexpr uint16_t TOUCH_STOP_MIN_MS = 140;\n  constexpr uint16_t TOUCH_STOP_MAX_MS = 850;','constexpr uint16_t TOUCH_STOP_MIN_MS = 300;\n  constexpr uint16_t TOUCH_STOP_MAX_MS = 950;','deliberate stop window');
  out=replaceOnce(out,'constexpr uint16_t TOUCH_STATE_LOCKOUT_MS = 900;','constexpr uint16_t TOUCH_STATE_LOCKOUT_MS = 1500;','touch state lockout');
  // Never enter deep sleep while the active-high TTP223 wake source is already
  // asserted; doing so creates an immediate wake/sleep loop.
  out=replaceOnce(out,'void enterDeepSleep(const char* reason) {\n  if (otaBusy() || streamingEnabled.load()) return;','void enterDeepSleep(const char* reason) {\n  if (otaBusy() || streamingEnabled.load()) return;\n  if (digitalRead(TOUCH_INPUT_PIN)==TOUCH_ACTIVE_LEVEL) return;','deep sleep release guard');
  // One scheduler/I2S timeout is not evidence that the microphone failed. Allow a
  // bounded 3-read recovery window (<=~240 ms with the configured timeout) before
  // surfacing AUDIO_SOURCE_FAILED.
  out=replaceOnce(out,`  size_t received=0;\n  while (received < sizeof(raw)) {\n    if (!streamingEnabled.load() || frame.generation != streamGeneration.load()) return false;\n    const size_t count = microphoneI2S.readBytes(\n      reinterpret_cast<char*>(raw)+received, sizeof(raw)-received);\n    if (!count) return false;\n    received += count;\n  }`,`  size_t received=0;\n  uint8_t emptyReads=0;\n  while (received < sizeof(raw)) {\n    if (!streamingEnabled.load() || frame.generation != streamGeneration.load()) return false;\n    const size_t count = microphoneI2S.readBytes(\n      reinterpret_cast<char*>(raw)+received, sizeof(raw)-received);\n    if (!count) {\n      if (++emptyReads < 3) continue;\n      return false;\n    }\n    emptyReads=0;\n    received += count;\n  }`,'I2S transient timeout recovery');
  // Mobile stacks may complete MTU negotiation after START. An increase is always
  // safe for the already-selected packet size. A decrease is also safe while the
  // current audio packet still fits; only a genuinely incompatible capacity stops.
  out=replaceOnce(out,`    if (deviceConnected.load() && streamingEnabled.load() &&\n        bleServer->getPeerMTU(bleServer->getConnId()) != peerMtu) {\n      stopStreaming(ErrorCode::TRANSPORT_CHANGED);\n    }`,`    if (deviceConnected.load() && streamingEnabled.load()) {\n      uint16_t liveMtu=bleServer->getPeerMTU(bleServer->getConnId());\n      if (liveMtu<23) liveMtu=23;\n      if (liveMtu!=peerMtu) {\n        const uint16_t liveCapacity=liveMtu-3;\n        if (liveCapacity < uint16_t(AUDIO_HEADER_BYTES+audioPayloadBytes.load())) {\n          stopStreaming(ErrorCode::TRANSPORT_CHANGED);\n        } else {\n          peerMtu=liveMtu;attValueCapacity=liveCapacity;\n          updateStatusCharacteristic(true);\n        }\n      }\n    }`,'nonfatal compatible MTU change');
  // If a previously healthy cell becomes critically low mid-update, stop further
  // flash writes rather than only rejecting a new BEGIN command.
  out=replaceOnce(out,'  otaSession.tick(now,generation,connected);\n  if (connected && !otaBusy()) {','  otaSession.tick(now,generation,connected);\n  if (batteryCritical() && otaBusy() && otaSession.state!=Synap::COMMITTED) {\n    otaSession.fail(Synap::BUSY);\n  }\n  if (connected && !otaBusy()) {','critical battery OTA abort');
  return out;
}
if(require.main===module){const file=process.argv[2];if(!file)throw Error('Usage: node tools/patch-production-hardening.cjs <sketch>');fs.writeFileSync(file,patch(fs.readFileSync(file,'utf8')));console.log('Applied Synap end-to-end production hardening')}
module.exports={patch,replaceOnce};
