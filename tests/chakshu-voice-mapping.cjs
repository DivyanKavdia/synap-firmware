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
  const structStart = voice.indexOf('struct AcLevel');
  const structEnd = voice.indexOf('\n', structStart) + 1;
  const functionStart = voice.indexOf('AcLevel measureAcLevel');
  const functionEnd = voice.indexOf('float windowGain()', functionStart);
  assert(structStart >= 0 && structEnd > structStart && functionStart >= 0 && functionEnd > functionStart);
  const helper = voice.slice(structStart, structEnd) + voice.slice(functionStart, functionEnd);
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
  assert.match(voice, /float noiseFloor=180\.0f/);
  assert.match(voice, /const float speechThreshold=voiceThreshold\(noiseFloor\)/);
  assert.match(voice, /if\(vadRun>=2\)speechHoldUntil=now\+1000u/);
  assert.match(voice, /candidateId=0;candidateConfidence=0/);
  const noiseStart=voice.indexOf('float updateNoiseFloor(');
  const noiseEnd=voice.indexOf('Inference infer()',noiseStart);
  assert(noiseStart>=0&&noiseEnd>noiseStart);
  const noiseHelpers=voice.slice(noiseStart,noiseEnd);
  assert.match(nativeTest(`#include <cstdint>
#include <cmath>
#include <cassert>
#include <cstdio>
${noiseHelpers}
int main(){
  float floor=180.0f;
  assert(voiceThreshold(floor)==440.0f);
  floor=updateNoiseFloor(floor,181,false);
  assert(floor>180.0f&&floor<181.0f);
  const float before=floor;
  floor=updateNoiseFloor(floor,600,false);
  assert(floor==before);
  assert(updateNoiseFloor(floor,50,true)==floor);
  assert(voiceThreshold(50.0f)==220.0f);
  puts("PASS adaptive AC voice gate");
}`), /PASS adaptive AC voice gate/);
  const gain=voice.slice(voice.indexOf('float windowGain()'),voice.indexOf('float updateNoiseFloor('));
  assert.match(gain,/const double mean=sum\/double\(WINDOW_SAMPLES\)/);
  assert.match(gain,/double\(sampleAt\(i\)\)-mean/);
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


test('voice diagnostics v2 separate raw candidates, VAD state and accepted commands', () => {
  const voice = fs.readFileSync('firmware/xiao-sense/voice.cpp', 'utf8');
  const encodeStart = voice.indexOf('void encodeDiagnostics(');
  const encodeEnd = voice.indexOf('class DiagnosticCallbacks', encodeStart);
  assert(encodeStart >= 0 && encodeEnd > encodeStart);
  const diag = voice.slice(encodeStart, encodeEnd);
  assert.match(diag, /bytes\[1\]=2/);
  assert.match(diag, /put16le\(bytes\+20,diagnosticNoiseFloor\.load\(\)\)/);
  assert.match(diag, /put16le\(bytes\+22,diagnosticThreshold\.load\(\)\)/);
  assert.match(diag, /bytes\[24\]=diagnosticVadRun\.load\(\)/);
  assert.match(diag, /bytes\[25\]=diagnosticHoldActive\.load\(\)/);
  assert.match(diag, /put16le\(bytes\+26,diagnosticMaxMeanAbs\.exchange\(audioMeanAbs\.load\(\)\)\)/);
  assert.match(diag, /put32le\(bytes\+28,diagnosticVadOpenCount\.load\(\)\)/);
  assert.match(diag, /bytes\[32\]=acceptedCommand\.load\(\)/);
  assert.match(diag, /put16le\(bytes\+33,acceptedConfidence\.load\(\)\)/);
  assert.match(diag, /put32le\(bytes\+35,acceptedAt\.load\(\)\)/);
  assert.match(diag, /put32le\(bytes\+39,acceptedCount\.load\(\)\)/);
  assert.match(voice, /if\(vadRun==2\)\+\+diagnosticVadOpenCount/);
  assert.match(voice, /acceptedCommand=accepted/);
  assert.match(voice, /acceptedAt=now;\+\+acceptedCount/);
});
