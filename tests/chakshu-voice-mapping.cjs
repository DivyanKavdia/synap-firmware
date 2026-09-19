'use strict';
const {test} = require('node:test'), assert = require('node:assert/strict'), fs = require('node:fs');
const {nativeTest} = require('./support/native.cjs');
test('all trained classes map to protocol commands, and actions require a wake', () => {
  const voice = fs.readFileSync('firmware/xiao-sense/voice.cpp', 'utf8');
  const model = fs.readFileSync('firmware/xiao-sense/tiny-voice-model.h', 'utf8');
  const contract = fs.readFileSync('firmware/xiao-sense/voice-contract.cpp', 'utf8');
  const mapping = voice.slice(voice.indexOf('uint8_t classCommand('), voice.indexOf('void consider('));
  assert.match(nativeTest(`#include <cstdint>
#include <cstddef>
#include <cassert>
#include <cstdio>
class BLEService;
${contract}
${model}
namespace ChakshuVoice { using namespace ChakshuTinyModel; ${mapping} }
int main(){
 const uint8_t expected[]={0,0,1,2,3,8};
 for(uint8_t i=0;i<6;++i)assert(ChakshuVoice::classCommand(i)==expected[i]);
 assert(ChakshuVoice::classCommand(99)==0);
 ChakshuVoice::Gate gate;
 assert(gate.accept(2,0.99f,100)==0);
 assert(gate.accept(1,0.99f,200)==1);
 assert(gate.accept(2,0.99f,400)==2);
 assert(gate.accept(3,0.99f,600)==0);
 assert(gate.accept(1,0.99f,2000)==1);
 assert(gate.accept(8,0.99f,2300)==8);
 puts("PASS class mapping and wake gate");
}`), /PASS class mapping/);
});
test('voice activity uses DC-centred acoustic energy rather than microphone offset', () => {
  const voice = fs.readFileSync('firmware/xiao-sense/voice.cpp', 'utf8');
  const start = voice.indexOf('struct AcLevel');
  const end = voice.indexOf('float windowGain()', start);
  assert(start >= 0 && end > start);
  const helper = voice.slice(start, end);
  assert.match(nativeTest(`#include <cstdint>
#include <cstddef>
#include <cassert>
#include <cstdio>
${helper}
int main(){
 const int16_t dc[]={1250,1250,1250,1250,1250,1250,1250,1250};
 const auto silent=measureAcLevel(dc,8);
 assert(silent.meanAbs==0&&silent.peak==0);
 const int16_t speech[]={900,1100,900,1100,900,1100,900,1100};
 const auto active=measureAcLevel(speech,8);
 assert(active.meanAbs==100&&active.peak==100);
 const int16_t rail[]={-32768,32767};
 const auto extreme=measureAcLevel(rail,2);
 assert(extreme.meanAbs==32767&&extreme.peak==32768);
 puts("PASS dc-centred voice level");
}`), /PASS dc-centred voice level/);
  assert.match(voice, /const float speechThreshold=fmaxf\(55\.0f,noiseFloor\*2\.6f\)/);
  assert.match(voice, /candidateId=0;candidateConfidence=0/);
});

test('SD cleanup recognizes exact generated photo/audio paths without admitting other files', () => {
  const source = fs.readFileSync('firmware/xiao-sense/sd-storage.cpp', 'utf8');
  const validator = source.slice(source.indexOf('bool capturePath('), source.indexOf('String stemFor('));
  assert.match(nativeTest(`#include <cstring>
#include <cassert>
#include <cstdio>
${validator}
int main(){
 assert(capturePath("/synap/12345678-00000001.jpg"));
 assert(capturePath("/synap/12345678-00000001.wav"));
 assert(capturePath("/synap/12345678-00000001.mjpeg"));
 assert(capturePath("/synap/12345678-00000001.json"));
 assert(!capturePath("/synap/12345678-00000001-extra.jpg"));
 assert(!capturePath("/synap/12345678-00000001.jpg/other.wav"));
 assert(!capturePath("/synap/model.bin"));assert(!capturePath(nullptr));
 puts("PASS capture paths");
}`), /PASS capture paths/);
});
